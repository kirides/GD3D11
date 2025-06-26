#include "D3D11VertexBuffer.h"

#include "pch.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include <DirectXMesh.h>
#include "D3D11_Helpers.h"

D3D11VertexBuffer::D3D11VertexBuffer() {}

D3D11VertexBuffer::~D3D11VertexBuffer() {}

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
    }

    SetDebugName( VertexBuffer.Get(), fileName );

    delete[] data;

    return XR_SUCCESS;
}

/** Updates the vertexbuffer with the given data */
XRESULT D3D11VertexBuffer::UpdateBuffer( void* data, UINT size ) {
    void* mappedData;
    UINT bsize;

    if ( SizeInBytes < size ) {
        size = SizeInBytes;
    }

    if ( XR_SUCCESS == Map( EMapFlags::M_WRITE_DISCARD, &mappedData, &bsize ) ) {
        if ( size ) {
            bsize = size;
        }
        // Copy data
        memcpy( mappedData, data, bsize );
        Unmap();

        return XR_SUCCESS;
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
    return XR_SUCCESS;

    uint32_t* remap = new uint32_t[numVertices];
    if ( FAILED( DirectX::OptimizeVertices( indices, numIndices / 3, numVertices, remap ) ) ) {
        delete[] remap;
        return XR_FAILED;
    }

    // Remap vertices
    byte* vxCopy = new byte[numVertices * stride];
    memcpy( vxCopy, vertices, numVertices * stride );

    for ( unsigned int i = 0; i < numVertices; i++ ) {
        // Assign the vertex at remap[i] to its new vertex
        memcpy( vertices + remap[i] * stride, vxCopy + i * stride, stride );
    }

    for ( unsigned int i = 0; i < numIndices; i++ ) {
        // Remap the indices.
        indices[i] = static_cast<VERTEX_INDEX>(remap[indices[i]]);
    }

    delete[] vxCopy;
    delete[] remap;

    return XR_SUCCESS;
}

/** Optimizes the given set of vertices */
XRESULT D3D11VertexBuffer::OptimizeFaces( VERTEX_INDEX* indices, byte* vertices, unsigned int numIndices, unsigned int numVertices, unsigned int stride ) {
    return XR_SUCCESS;

    unsigned int numFaces = numIndices / 3;
    uint32_t* remap = new uint32_t[numFaces];

    if ( FAILED( DirectX::OptimizeFaces( indices, numFaces, &numVertices, remap ) ) ) {
        delete[] remap;
        return XR_FAILED;
    }
    // Remap vertices
    VERTEX_INDEX* ibCopy = new VERTEX_INDEX[numFaces * 3];
    memcpy( ibCopy, indices, numFaces * 3 * sizeof( VERTEX_INDEX ) );

    for ( unsigned int i = 0; i < numFaces; i++ ) {
        // Copy the remapped face
        memcpy( &indices[i * 3], &ibCopy[remap[i] * 3], 3 * sizeof( VERTEX_INDEX ) );
    }

    delete[] ibCopy;
    delete[] remap;

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
