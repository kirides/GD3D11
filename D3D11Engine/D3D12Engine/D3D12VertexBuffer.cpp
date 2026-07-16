#include "../pch.h"
#include "D3D12VertexBuffer.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"

#include <meshoptimizer/src/meshoptimizer.h>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace {
    inline D3D12GraphicsEngine* Engine12() {
        return static_cast<D3D12GraphicsEngine*>( Engine::GraphicsEngine );
    }

    // --- CPU-side meshoptimizer helpers (backend-neutral; mirrors D3D11VertexBuffer.cpp) ---
    constexpr float kOverdrawThreshold = 1.05f;
    constexpr int kNormalQuantizationBits = 10;

    void ConvertIndicesToUInt32( const VERTEX_INDEX* src, size_t count, std::vector<unsigned int>& dst ) {
        dst.resize( count );
        for ( size_t i = 0; i < count; ++i ) {
            dst[i] = src[i];
        }
    }

    bool ConvertIndicesToVertexIndex( const std::vector<unsigned int>& src, VERTEX_INDEX* dst, size_t dstCount ) {
        const unsigned int maxVertexIndex = static_cast<unsigned int>(std::numeric_limits<VERTEX_INDEX>::max());
        if ( src.size() > dstCount ) {
            return false;
        }

        for ( size_t i = 0; i < src.size(); ++i ) {
            if ( src[i] > maxVertexIndex ) {
                return false;
            }
            dst[i] = static_cast<VERTEX_INDEX>(src[i]);
        }
        return true;
    }

    float DequantizeSnorm( int v, int bits ) {
        const int maxValue = (1 << (bits - 1)) - 1;
        if ( v > maxValue ) {
            v = maxValue;
        } else if ( v < -maxValue ) {
            v = -maxValue;
        }
        return static_cast<float>(v) / static_cast<float>(maxValue);
    }

    void BuildQuantizedVertexKeyBuffer( const uint8_t* srcVertices, unsigned int numVertices, unsigned int stride, std::vector<uint8_t>& outKeyBuffer ) {
        const size_t totalBytes = static_cast<size_t>(numVertices) * stride;
        outKeyBuffer.assign( srcVertices, srcVertices + totalBytes );

        // Quantize attributes in the key stream to collapse tiny floating-point drift during reindexing.
        if ( stride != sizeof( ExVertexStruct ) ) {
            return;
        }

        ExVertexStruct* keyVertices = reinterpret_cast<ExVertexStruct*>(outKeyBuffer.data());
        for ( unsigned int i = 0; i < numVertices; ++i ) {
            ExVertexStruct& v = keyVertices[i];

            v.Normal.x = DequantizeSnorm( meshopt_quantizeSnorm( v.Normal.x, kNormalQuantizationBits ), kNormalQuantizationBits );
            v.Normal.y = DequantizeSnorm( meshopt_quantizeSnorm( v.Normal.y, kNormalQuantizationBits ), kNormalQuantizationBits );
            v.Normal.z = DequantizeSnorm( meshopt_quantizeSnorm( v.Normal.z, kNormalQuantizationBits ), kNormalQuantizationBits );

            v.TexCoord.x = meshopt_dequantizeHalf( meshopt_quantizeHalf( v.TexCoord.x ) );
            v.TexCoord.y = meshopt_dequantizeHalf( meshopt_quantizeHalf( v.TexCoord.y ) );
            v.TexCoord2.x = meshopt_dequantizeHalf( meshopt_quantizeHalf( v.TexCoord2.x ) );
            v.TexCoord2.y = meshopt_dequantizeHalf( meshopt_quantizeHalf( v.TexCoord2.y ) );
        }
    }
}

D3D12VertexBuffer::~D3D12VertexBuffer() {
    if ( m_Resource && m_MappedPtr ) {
        m_Resource->Unmap( 0, nullptr );
        m_MappedPtr = nullptr;
    }
    // Defer the GPU resource release. This buffer may still be referenced by an in-flight command list
    // OR by the currently-open (unsubmitted) command list — Gothic can evict a mesh visual mid-frame
    // (LRU cache / changed node attachment), freeing all its VB+IB at once. A synchronous free deletes
    // a resource the GPU still references -> OBJECT_DELETED_WHILE_STILL_IN_USE -> device hang. Note the
    // old WaitForGpuIdle here did NOT prevent this: it only flushes *submitted* work, not the open list
    // the caller is still recording into. The engine drops the resource once its frame fence has passed.
    if ( m_Resource ) {
        if ( D3D12GraphicsEngine* engine = Engine12() ) {
            engine->QueueResourceForRelease( std::move( m_Resource ) );
        }
        m_Resource.Reset();
    }
}

XRESULT D3D12VertexBuffer::Init( void* initData, unsigned int sizeInBytes, EBindFlags /*bindFlags*/,
    EUsageFlags /*usage*/, ECPUAccessFlags /*cpuAccess*/, const std::string& fileName, unsigned int /*structuredByteSize*/ ) {

    ID3D12Device* device = Engine12() ? Engine12()->GetD3DDevice() : nullptr;
    if ( !device ) return XR_FAILED;

    if ( sizeInBytes == 0 ) {
        LogError() << "VertexBuffer size can't be 0!";
        sizeInBytes = 1; // still create a valid (tiny) resource so callers/Map() stay well-defined
    }
    m_SizeInBytes = sizeInBytes;

    // UPLOAD-heap committed resource: CPU-writable, GPU-readable, created (and kept) in GENERIC_READ.
    D3D12_HEAP_PROPERTIES heapUpload = {};
    heapUpload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = sizeInBytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;

    if ( FAILED( device->CreateCommittedResource( &heapUpload, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( m_Resource.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12VertexBuffer: CreateCommittedResource failed (" << sizeInBytes << " bytes).";
        m_SizeInBytes = 0;
        return XR_FAILED;
    }

    // Persistently map for the resource's lifetime (standard for dynamic upload buffers).
    D3D12_RANGE noRead = { 0, 0 };
    void* mapped = nullptr;
    if ( FAILED( m_Resource->Map( 0, &noRead, &mapped ) ) ) {
        LogWarn() << "D3D12VertexBuffer: Map failed.";
        m_Resource.Reset();
        m_SizeInBytes = 0;
        return XR_FAILED;
    }
    m_MappedPtr = static_cast<uint8_t*>( mapped );

    if ( initData ) {
        memcpy( m_MappedPtr, initData, sizeInBytes );
    } else {
        memset( m_MappedPtr, 0, sizeInBytes );
    }

    // Name the resource so the D3D12 debug layer identifies it (e.g. in OBJECT_DELETED_WHILE_STILL_IN_USE
    // reports) as a vertex/index buffer + its source mesh, instead of 'Unnamed Object'.
    const std::string debugName = "VB:" + ( fileName.empty() ? std::string( "unnamed" ) : fileName );
    m_Resource->SetPrivateData( WKPDID_D3DDebugObjectName, static_cast<UINT>( debugName.size() ), debugName.c_str() );

    return XR_SUCCESS;
}

XRESULT D3D12VertexBuffer::UpdateBuffer( void* data, unsigned int size ) {
    if ( !m_MappedPtr || !data ) return XR_FAILED;
    if ( size == 0 || size > m_SizeInBytes ) size = m_SizeInBytes;
    memcpy( m_MappedPtr, data, size );
    return XR_SUCCESS;
}

XRESULT D3D12VertexBuffer::Map( int /*flags*/, void** dataPtr, unsigned int* size ) {
    // Upload heap is persistently mapped, so every map flag (incl. WRITE_DISCARD) resolves to the
    // same pointer. Safe here: dynamic D3D7 buffers are snapshotted CPU-side per draw and never GPU-bound.
    if ( !m_MappedPtr ) {
        if ( dataPtr ) *dataPtr = nullptr;
        if ( size ) *size = 0;
        return XR_FAILED;
    }
    if ( dataPtr ) *dataPtr = m_MappedPtr;
    if ( size ) *size = m_SizeInBytes;
    return XR_SUCCESS;
}

XRESULT D3D12VertexBuffer::Unmap() {
    return XR_SUCCESS; // persistent map — nothing to do
}

XRESULT D3D12VertexBuffer::OptimizeVertices( VERTEX_INDEX* indices, uint8_t* vertices, unsigned int numIndices,
    unsigned int numVertices, unsigned int stride, std::vector<VERTEX_INDEX>* outShadowIndices ) {
    if ( !indices || !vertices || numIndices == 0 || numVertices == 0 || stride == 0 ) {
        if ( outShadowIndices ) outShadowIndices->clear();
        return XR_SUCCESS;
    }

    // meshoptimizer supports per-vertex element sizes up to 256 bytes.
    if ( stride > 256 ) {
        if ( outShadowIndices ) outShadowIndices->clear();
        return XR_SUCCESS;
    }

    const unsigned int maxVertexIndex = static_cast<unsigned int>(std::numeric_limits<VERTEX_INDEX>::max());
    if ( numVertices > maxVertexIndex + 1 ) {
        LogError() << "OptimizeVertices: numVertices exceeds VERTEX_INDEX range";
        return XR_FAILED;
    }

    ZoneScoped;

    std::vector<unsigned int> indexData;
    ConvertIndicesToUInt32( indices, numIndices, indexData );

    std::vector<unsigned int> remap( numVertices );
    const size_t fetchedVertexCount = meshopt_optimizeVertexFetchRemap( remap.data(), indexData.data(), numIndices, numVertices );

    std::vector<unsigned int> remappedIndices( numIndices );
    meshopt_remapIndexBuffer( remappedIndices.data(), indexData.data(), numIndices, remap.data() );

    std::vector<uint8_t> remappedVertices( static_cast<size_t>(numVertices) * stride );
    memcpy( remappedVertices.data(), vertices, remappedVertices.size() );
    meshopt_remapVertexBuffer( remappedVertices.data(), vertices, numVertices, stride, remap.data() );

    if ( outShadowIndices ) {
        std::vector<unsigned int> shadowIndices( numIndices );
        meshopt_generateShadowIndexBuffer( shadowIndices.data(),
            remappedIndices.data(),
            numIndices,
            remappedVertices.data(),
            fetchedVertexCount,
            sizeof( float ) * 3,
            stride );

        outShadowIndices->resize( numIndices );
        if ( !ConvertIndicesToVertexIndex( shadowIndices, outShadowIndices->data(), outShadowIndices->size() ) ) {
            LogError() << "OptimizeVertices: shadow index exceeds VERTEX_INDEX range";
            outShadowIndices->clear();
            return XR_FAILED;
        }
    }

    if ( !ConvertIndicesToVertexIndex( remappedIndices, indices, numIndices ) ) {
        LogError() << "OptimizeVertices: remapped index exceeds VERTEX_INDEX range";
        if ( outShadowIndices ) outShadowIndices->clear();
        return XR_FAILED;
    }

    memcpy( vertices, remappedVertices.data(), remappedVertices.size() );

    return XR_SUCCESS;
}

XRESULT D3D12VertexBuffer::OptimizeFaces( VERTEX_INDEX* indices, uint8_t* vertices, unsigned int numIndices,
    unsigned int numVertices, unsigned int stride ) {
    if ( !indices || !vertices || numIndices < 3 || numVertices == 0 || (numIndices % 3) != 0 || stride == 0 ) {
        return XR_SUCCESS;
    }

    if ( stride > 256 ) {
        return XR_SUCCESS;
    }

    const unsigned int maxVertexIndex = static_cast<unsigned int>(std::numeric_limits<VERTEX_INDEX>::max());
    if ( numVertices > maxVertexIndex + 1 ) {
        LogError() << "OptimizeFaces: numVertices exceeds VERTEX_INDEX range";
        return XR_FAILED;
    }

    ZoneScoped;

    std::vector<unsigned int> indexData;
    ConvertIndicesToUInt32( indices, numIndices, indexData );

    // Step 1: Indexing/reindexing with a quantized key stream to reduce float drift duplicates.
    std::vector<uint8_t> remapKeyVertices;
    BuildQuantizedVertexKeyBuffer( vertices, numVertices, stride, remapKeyVertices );

    std::vector<unsigned int> remap( numVertices );
    const size_t indexedVertexCount = meshopt_generateVertexRemap( remap.data(),
        indexData.data(),
        numIndices,
        remapKeyVertices.data(),
        numVertices,
        stride );
    if ( indexedVertexCount == 0 ) {
        return XR_FAILED;
    }

    std::vector<unsigned int> reindexedIndices( numIndices );
    meshopt_remapIndexBuffer( reindexedIndices.data(), indexData.data(), numIndices, remap.data() );

    std::vector<uint8_t> reindexedVertices( static_cast<size_t>(numVertices) * stride );
    memcpy( reindexedVertices.data(), vertices, reindexedVertices.size() );
    meshopt_remapVertexBuffer( reindexedVertices.data(), vertices, numVertices, stride, remap.data() );

    memcpy( vertices, reindexedVertices.data(), reindexedVertices.size() );
    indexData.swap( reindexedIndices );

    // Step 2: Vertex cache optimization.
    meshopt_optimizeVertexCache( indexData.data(), indexData.data(), numIndices, indexedVertexCount );

    // Step 3 (optional): Overdraw optimization.
    if ( stride >= sizeof( float ) * 3 ) {
        meshopt_optimizeOverdraw( indexData.data(),
            indexData.data(),
            numIndices,
            reinterpret_cast<const float*>(vertices),
            indexedVertexCount,
            stride,
            kOverdrawThreshold );
    }

    if ( !ConvertIndicesToVertexIndex( indexData, indices, numIndices ) ) {
        LogError() << "OptimizeFaces: remapped index exceeds VERTEX_INDEX range";
        return XR_FAILED;
    }

    return XR_SUCCESS;
}
