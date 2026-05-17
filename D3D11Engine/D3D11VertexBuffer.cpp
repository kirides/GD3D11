#include "D3D11VertexBuffer.h"

#include "pch.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include <meshoptimizer/src/meshoptimizer.h>
#include <limits>
#include <vector>
#include "D3D11_Helpers.h"

namespace {
    bool ConvertIndicesToUInt32( const VERTEX_INDEX* src, size_t count, std::vector<unsigned int>& dst ) {
        dst.resize( count );
        for ( size_t i = 0; i < count; ++i ) {
            dst[i] = src[i];
        }

        return true;
    }

    bool ConvertIndicesToVertexIndex( const std::vector<unsigned int>& src, VERTEX_INDEX* dst ) {
        const unsigned int maxVertexIndex = static_cast<unsigned int>(std::numeric_limits<VERTEX_INDEX>::max());

        for ( size_t i = 0; i < src.size(); ++i ) {
            if ( src[i] > maxVertexIndex ) {
                return false;
            }

            dst[i] = static_cast<VERTEX_INDEX>(src[i]);
        }

        return true;
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
XRESULT D3D11VertexBuffer::OptimizeVertices( VERTEX_INDEX* indices, byte* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride ) {
    if ( !indices || !vertices || numIndices == 0 || numVertices == 0 || stride == 0 ) {
        return XR_SUCCESS;
    }

    // meshoptimizer supports per-vertex element sizes up to 256 bytes.
    if ( stride > 256 ) {
        return XR_SUCCESS;
    }

    const unsigned int maxVertexIndex = static_cast<unsigned int>(std::numeric_limits<VERTEX_INDEX>::max());
    if ( numVertices > maxVertexIndex + 1 ) {
        LogError() << "OptimizeVertices: numVertices exceeds VERTEX_INDEX range";
        return XR_FAILED;
    }

    std::vector<unsigned int> indexData;
    ConvertIndicesToUInt32( indices, numIndices, indexData );

    std::vector<unsigned int> remap( numVertices );
    meshopt_optimizeVertexFetchRemap( remap.data(), indexData.data(), numIndices, numVertices );

    std::vector<unsigned int> remappedIndices( numIndices );
    meshopt_remapIndexBuffer( remappedIndices.data(), indexData.data(), numIndices, remap.data() );

    std::vector<byte> remappedVertices( static_cast<size_t>(numVertices) * stride );
    memcpy( remappedVertices.data(), vertices, remappedVertices.size() );
    meshopt_remapVertexBuffer( remappedVertices.data(), vertices, numVertices, stride, remap.data() );

    if ( !ConvertIndicesToVertexIndex( remappedIndices, indices ) ) {
        LogError() << "OptimizeVertices: remapped index exceeds VERTEX_INDEX range";
        return XR_FAILED;
    }

    memcpy( vertices, remappedVertices.data(), remappedVertices.size() );

    return XR_SUCCESS;
}

/** Optimizes the given set of vertices */
XRESULT D3D11VertexBuffer::OptimizeFaces( VERTEX_INDEX* indices, byte* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride ) {
    (void)vertices;
    (void)stride;

    if ( !indices || numIndices < 3 || numVertices == 0 || (numIndices % 3) != 0 ) {
        return XR_SUCCESS;
    }

    const unsigned int maxVertexIndex = static_cast<unsigned int>(std::numeric_limits<VERTEX_INDEX>::max());
    if ( numVertices > maxVertexIndex + 1 ) {
        LogError() << "OptimizeFaces: numVertices exceeds VERTEX_INDEX range";
        return XR_FAILED;
    }

    std::vector<unsigned int> indexData;
    ConvertIndicesToUInt32( indices, numIndices, indexData );
    meshopt_optimizeVertexCache( indexData.data(), indexData.data(), numIndices, numVertices );

    if ( !ConvertIndicesToVertexIndex( indexData, indices ) ) {
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
