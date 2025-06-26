#include "D3D11IndirectBuffer.h"

#include "pch.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include <DirectXMesh.h>
#include "D3D11_Helpers.h"

D3D11IndirectBuffer::D3D11IndirectBuffer() {}

D3D11IndirectBuffer::~D3D11IndirectBuffer() {}

/** Creates the buffer with the given arguments */
XRESULT D3D11IndirectBuffer::Init( void* initData, unsigned int sizeInBytes, EBindFlags EBindFlags, EUsageFlags usage, ECPUAccessFlags cpuAccess, const std::string& fileName, unsigned int structuredByteSize ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    if ( sizeInBytes == 0 ) {
        LogError() << "IndirectBuffer size can't be 0!";
    }

    SizeInBytes = sizeInBytes;

    // Create our own IndirectBuffer
    D3D11_BUFFER_DESC bufferDesc;
    bufferDesc.ByteWidth = sizeInBytes;
    bufferDesc.Usage = static_cast<D3D11_USAGE>(usage);
    bufferDesc.BindFlags = static_cast<D3D11_USAGE>(EBindFlags);
    bufferDesc.CPUAccessFlags = static_cast<D3D11_USAGE>(cpuAccess);
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    bufferDesc.StructureByteStride = 0;

    // Check for unordered access
    if ( (EBindFlags & EBindFlags::B_UNORDERED_ACCESS) != 0 ) {
        bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        bufferDesc.StructureByteStride = 4;
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

    LE( engine->GetDevice()->CreateBuffer( &bufferDesc, &InitData, IndirectBuffer.ReleaseAndGetAddressOf() ) );
    if ( !IndirectBuffer.Get() ) {
        delete[] data;
        return XR_SUCCESS;
    }

    // Check for unordered access again to create the UAV
    if ( (EBindFlags & EBindFlags::B_UNORDERED_ACCESS) != 0 ) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = sizeInBytes / 4;

        engine->GetDevice()->CreateUnorderedAccessView( IndirectBuffer.Get(), &uavDesc, UnorderedAccessView.ReleaseAndGetAddressOf() );
    }

    SetDebugName( IndirectBuffer.Get(), fileName );

    delete[] data;

    return XR_SUCCESS;
}

/** Updates the buffer with the given data */
XRESULT D3D11IndirectBuffer::UpdateBuffer( void* data, UINT size ) {
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
XRESULT D3D11IndirectBuffer::Map( int flags, void** dataPtr, UINT* size ) {
    D3D11_MAPPED_SUBRESOURCE res;
    if ( FAILED( reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->Map( IndirectBuffer.Get(), 0, static_cast<D3D11_MAP>(flags), 0, &res ) ) ) {
        return XR_FAILED;
    }

    *dataPtr = res.pData;
    *size = SizeInBytes;

    return XR_SUCCESS;
}

/** Unmaps the buffer */
XRESULT D3D11IndirectBuffer::Unmap() {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->Unmap( IndirectBuffer.Get(), 0 );
    return XR_SUCCESS;
}

/** Returns the size in bytes of this buffer */
unsigned int D3D11IndirectBuffer::GetSizeInBytes() const {
    return SizeInBytes;
}
