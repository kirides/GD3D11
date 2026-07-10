#include "ConstantBufferPool.h"
#include "Logger.h"

void ConstantBufferPool::BeginFrame() {
    m_currentOffset = 0;
    m_firstMapThisFrame = true;
}

ConstantBufferAllocation ConstantBufferPool::Allocate( const void* pData, uint32_t sizeInBytes ) {
    uint32_t alignedSize = (sizeInBytes + 255) & ~255;

    // Choose the map mode carefully:
    //  - The very first Map of the frame uses DISCARD so the driver can hand us a
    //    fresh buffer while the GPU may still be reading last frame's copy.
    //  - Every subsequent Map uses NO_OVERWRITE (we only ever write untouched
    //    regions ahead of m_currentOffset).
    //  - If we run out of room mid-frame we wrap to 0 but keep NO_OVERWRITE:
    //    a DISCARD here would invalidate data that earlier draws THIS frame still
    //    reference. NO_OVERWRITE risks overwriting still-in-flight data only if the
    //    ring is genuinely full within a frame, so warn once - the pool should be
    //    sized so this never happens.
    D3D11_MAP mapMode;
    if ( m_firstMapThisFrame ) {
        mapMode = D3D11_MAP_WRITE_DISCARD;
        m_firstMapThisFrame = false;
    } else {
        if ( m_currentOffset + alignedSize > m_bufferSize ) {
            m_currentOffset = 0; // wrap
            if ( !m_wrapWarned ) {
                m_wrapWarned = true;
                LogWarn() << "ConstantBufferPool wrapped mid-frame (size " << m_bufferSize
                    << " bytes); increase the pool size to avoid potential overwrite hazards.";
            }
        }
        mapMode = D3D11_MAP_WRITE_NO_OVERWRITE;
    }

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    if ( SUCCEEDED( m_Context->Map( m_poolBuffer.Get(), 0, mapMode, 0, &mappedResource ) ) ) {
        memcpy( static_cast<uint8_t*>(mappedResource.pData) + m_currentOffset, pData, sizeInBytes );
        m_Context->Unmap( m_poolBuffer.Get(), 0 );
    }

    ConstantBufferAllocation alloc;
    alloc.pBuffer = m_poolBuffer.Get();
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
}
