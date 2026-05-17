#include "D3D11VertexBuffer.h"

#include "pch.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include <meshoptimizer/src/meshoptimizer.h>
#include <limits>
#include <vector>
#include "D3D11_Helpers.h"

namespace {
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

    void BuildQuantizedVertexKeyBuffer( const byte* srcVertices, unsigned int numVertices, unsigned int stride, std::vector<byte>& outKeyBuffer ) {
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

/** Creates the vertexbuffer with the given arguments */
XRESULT D3D11VertexBuffer::Init( void* initData, unsigned int sizeInBytes, EBindFlags EBindFlags, EUsageFlags usage, ECPUAccessFlags cpuAccess, const std::string& fileName, unsigned int structuredByteSize ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    if ( sizeInBytes == 0 ) {
        LogError() << "VertexBuffer size can't be 0!";
    }

    SizeInBytes = sizeInBytes;

    // Create our own vertexbuffer
    D3D11_BUFFER_DESC bufferDesc;
    bufferDesc.ByteWidth = sizeInBytes;
    bufferDesc.Usage = static_cast<D3D11_USAGE>(usage);
    bufferDesc.BindFlags = static_cast<D3D11_USAGE>(EBindFlags);
    bufferDesc.CPUAccessFlags = static_cast<D3D11_USAGE>(cpuAccess);
    bufferDesc.MiscFlags = 0;
    bufferDesc.StructureByteStride = structuredByteSize;

    // Check for structured buffer
    if ( (EBindFlags & EBindFlags::B_SHADER_RESOURCE) != 0 ) {
        bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    }

    // Check for unordered access
    if ( (EBindFlags & EBindFlags::B_UNORDERED_ACCESS) != 0 ) {
        bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    }

    // In case we dont have data, allocate some to satisfy D3D11
    char* data = nullptr;
    if ( !initData ) {
        data = new char[bufferDesc.ByteWidth];
        memset( data, 0, bufferDesc.ByteWidth );

        initData = data;
    }

    D3D11_SUBRESOURCE_DATA InitData;
    InitData.pSysMem = initData;
    InitData.SysMemPitch = 0;
    InitData.SysMemSlicePitch = 0;

    LE( engine->GetDevice()->CreateBuffer( &bufferDesc, &InitData, VertexBuffer.ReleaseAndGetAddressOf() ) );
    if ( !VertexBuffer.Get() ) {
        delete[] data;
        return XR_SUCCESS;
    }

    // Check for structured buffer again to create the SRV
    if ( (EBindFlags & EBindFlags::B_SHADER_RESOURCE) != 0 && structuredByteSize > 0 ) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.ElementWidth = sizeInBytes / structuredByteSize;

        engine->GetDevice()->CreateShaderResourceView( VertexBuffer.Get(), &srvDesc, ShaderResourceView.ReleaseAndGetAddressOf() );
        SetDebugName( ShaderResourceView.Get(), fileName+"_SRV");
    }

    // Check for unordered access again to create the UAV
    if ( (EBindFlags & EBindFlags::B_UNORDERED_ACCESS) != 0 ) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = sizeInBytes / structuredByteSize;

        engine->GetDevice()->CreateUnorderedAccessView( VertexBuffer.Get(), &uavDesc, UnorderedAccessView.ReleaseAndGetAddressOf() );
        SetDebugName( UnorderedAccessView.Get(), fileName + "_UAV" );
    }

    SetDebugName( VertexBuffer.Get(), fileName );

    delete[] data;

    return XR_SUCCESS;
}

/** Updates the vertexbuffer with the given data */
XRESULT D3D11VertexBuffer::UpdateBuffer( void* data, UINT size ) {
    if ( SizeInBytes < size ) {
        size = SizeInBytes;
    }

    void* mappedData;
    UINT bsize;

    if ( XR_SUCCESS == Map( EMapFlags::M_WRITE_DISCARD, &mappedData, &bsize ) ) {
        if ( mappedData ) {
            if ( size ) {
                size = std::min(size, bsize);
            }
            if ( size < bsize ) {
                ZeroMemory( mappedData, SizeInBytes );
            }
            // Copy data
            if ( data ) {
                memcpy( mappedData, data, size );
            }
        }

        return Unmap();
    }

    return XR_FAILED;
}

/** Maps the buffer */
XRESULT D3D11VertexBuffer::Map( int flags, void** dataPtr, UINT* size ) {
    D3D11_MAPPED_SUBRESOURCE res;
    if ( FAILED( reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->Map( VertexBuffer.Get(), 0, static_cast<D3D11_MAP>(flags), 0, &res ) ) ) {
        return XR_FAILED;
    }

    *dataPtr = res.pData;
    *size = SizeInBytes;

    return XR_SUCCESS;
}

/** Unmaps the buffer */
XRESULT D3D11VertexBuffer::Unmap() {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->Unmap( VertexBuffer.Get(), 0 );
    return XR_SUCCESS;
}

/** Returns the D3D11-Buffer object */
Microsoft::WRL::ComPtr <ID3D11Buffer>& D3D11VertexBuffer::GetVertexBuffer() {
    return VertexBuffer;
}

/** Optimizes the given set of vertices */
XRESULT D3D11VertexBuffer::OptimizeVertices( VERTEX_INDEX* indices, byte* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride, std::vector<VERTEX_INDEX>* outShadowIndices ) {
    if ( !indices || !vertices || numIndices == 0 || numVertices == 0 || stride == 0 ) {
        if ( outShadowIndices ) {
            outShadowIndices->clear();
        }
        return XR_SUCCESS;
    }

    // meshoptimizer supports per-vertex element sizes up to 256 bytes.
    if ( stride > 256 ) {
        if ( outShadowIndices ) {
            outShadowIndices->clear();
        }
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

    std::vector<byte> remappedVertices( static_cast<size_t>(numVertices) * stride );
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
        if ( outShadowIndices ) {
            outShadowIndices->clear();
        }
        return XR_FAILED;
    }

    memcpy( vertices, remappedVertices.data(), remappedVertices.size() );

    return XR_SUCCESS;
}

/** Optimizes the given set of vertices */
XRESULT D3D11VertexBuffer::OptimizeFaces( VERTEX_INDEX* indices, byte* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride ) {
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
    std::vector<byte> remapKeyVertices;
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

    std::vector<byte> reindexedVertices( static_cast<size_t>(numVertices) * stride );
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

/** Returns the size in bytes of this buffer */
unsigned int D3D11VertexBuffer::GetSizeInBytes() const {
    return SizeInBytes;
}

/** Returns the SRV of this buffer, if it represents a structured buffer */
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& D3D11VertexBuffer::GetShaderResourceView() {
    return ShaderResourceView;
}
