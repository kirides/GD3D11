#include "ConstantBufferPool.h"
#include "Logger.h"
#include "D3D11_Helpers.h"

void ConstantBufferPool::Initialize( ID3D11Device* device, uint32_t totalSizeInBytes, const char* debugName ) {
    m_bufferSize = totalSizeInBytes;
    m_currentOffset = 0;
    m_frameIndex = 0;
    m_wrapWarned = false;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext( &context );
    context.As( &m_Context );

    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.ByteWidth = m_bufferSize;
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_QUERY_DESC queryDesc = {};
    queryDesc.Query = D3D11_QUERY_EVENT;

    for ( uint32_t i = 0; i < FrameCount; i++ ) {
        FrameSlot& frameSlot = m_frames[i];
        device->CreateBuffer( &bufferDesc, nullptr, &frameSlot.Buffer );
        device->CreateQuery( &queryDesc, &frameSlot.FrameFence );
        frameSlot.FencePending = false;

        SetDebugName( frameSlot.Buffer.Get(),
            std::string( debugName ? debugName : "ConstantBufferPool" ) + "_frame" + std::to_string( i ) );
    }
}

void ConstantBufferPool::WaitForSlot( FrameSlot& slot ) {
    if ( !slot.FencePending ) {
        return; // never used yet - nothing the GPU could still be reading
    }

    BOOL signaled = FALSE;
    while ( m_Context->GetData( slot.FrameFence.Get(), &signaled, sizeof( signaled ), 0 ) != S_OK ) {
        Sleep( 0 ); // yield until the GPU catches up to the frame that last used this slot
    }

    slot.FencePending = false;
}

void ConstantBufferPool::BeginFrame() {
    m_frameIndex = (m_frameIndex + 1) % FrameCount;
    WaitForSlot( m_frames[m_frameIndex] );

    m_currentOffset = 0;
    m_wrapWarned = false;
}

ConstantBufferAllocation ConstantBufferPool::Allocate( const void* pData, uint32_t sizeInBytes ) {
    uint32_t alignedSize = (sizeInBytes + 255) & ~255;

    FrameSlot& slot = m_frames[m_frameIndex];

    if ( m_currentOffset + alignedSize > m_bufferSize ) {
        m_currentOffset = 0; // wrap within this frame's own buffer
        if ( !m_wrapWarned ) {
            m_wrapWarned = true;
            LogWarn() << "ConstantBufferPool wrapped mid-frame (size " << m_bufferSize
                << " bytes); increase the pool size to avoid potential overwrite hazards.";
        }
    }

    // This slot is exclusively owned by the current frame-in-flight - BeginFrame() already
    // waited on its fence, so no other still-executing command list can be reading it. That
    // makes NO_OVERWRITE safe for every Map, even the first one for this slot, without ever
    // needing DISCARD (and the unpredictable driver-side buffer renaming that comes with it).
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    if ( SUCCEEDED( m_Context->Map( slot.Buffer.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mappedResource ) ) ) {
        memcpy( static_cast<uint8_t*>(mappedResource.pData) + m_currentOffset, pData, sizeInBytes );
        m_Context->Unmap( slot.Buffer.Get(), 0 );
    }

    ConstantBufferAllocation alloc;
    alloc.pBuffer = slot.Buffer.Get();
    alloc.offsetInBytes = m_currentOffset;
    alloc.sizeInBytes = alignedSize;

    // Advance the offset for the next allocation
    m_currentOffset += alignedSize;

    return alloc;
}

void ConstantBufferPool::BindPS(uint32_t slot, const ConstantBufferAllocation& a) {
    if ( slot < 0 || !a.pBuffer ) return;
    UINT first = a.offsetInBytes / 16;
    UINT num = a.sizeInBytes / 16;
    m_Context->PSSetConstantBuffers1( slot, 1, &a.pBuffer, &first, &num );
}

void ConstantBufferPool::BindVS(uint32_t slot, const ConstantBufferAllocation& a)
{
    if ( slot < 0 || !a.pBuffer ) return;
    UINT first = a.offsetInBytes / 16;
    UINT num = a.sizeInBytes / 16;
    m_Context->VSSetConstantBuffers1( slot, 1, &a.pBuffer, &first, &num );
}

void ConstantBufferPool::BindCS(uint32_t slot, const ConstantBufferAllocation& a)
{
    if ( slot < 0 || !a.pBuffer ) return;
    UINT first = a.offsetInBytes / 16;
    UINT num = a.sizeInBytes / 16;
    m_Context->CSSetConstantBuffers1( slot, 1, &a.pBuffer, &first, &num );
}

void ConstantBufferPool::BindGS(uint32_t slot, const ConstantBufferAllocation& a)
{
    if ( slot < 0 || !a.pBuffer ) return;
    UINT first = a.offsetInBytes / 16;
    UINT num = a.sizeInBytes / 16;
    m_Context->GSSetConstantBuffers1( slot, 1, &a.pBuffer, &first, &num );
}

void ConstantBufferPool::BindDS(uint32_t slot, const ConstantBufferAllocation& a)
{
    if ( slot < 0 || !a.pBuffer ) return;
    UINT first = a.offsetInBytes / 16;
    UINT num = a.sizeInBytes / 16;
    m_Context->DSSetConstantBuffers1( slot, 1, &a.pBuffer, &first, &num );
}

void ConstantBufferPool::BindHS(uint32_t slot, const ConstantBufferAllocation& a)
{
    if ( slot < 0 || !a.pBuffer ) return;
    UINT first = a.offsetInBytes / 16;
    UINT num = a.sizeInBytes / 16;
    m_Context->HSSetConstantBuffers1( slot, 1, &a.pBuffer, &first, &num );
}

void ConstantBufferPool::EndFrame() {
    // Mark the end of this frame's GPU-visible usage of the current slot so BeginFrame()
    // knows what to wait on before handing this slot back out FrameCount frames from now.
    FrameSlot& slot = m_frames[m_frameIndex];
    m_Context->End( slot.FrameFence.Get() );
    slot.FencePending = true;
}
