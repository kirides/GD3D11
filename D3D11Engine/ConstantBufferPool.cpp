#include "ConstantBufferPool.h"
#include "Logging.h"
#include "D3D11_Helpers.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace {
    // MurmurHash3 x86_32: word-at-a-time with 32-bit math only, so it stays cheap on this x86 target.
    uint32_t HashBytes( const void* data, uint32_t size ) {
        constexpr uint32_t c1 = 0xcc9e2d51;
        constexpr uint32_t c2 = 0x1b873593;
        const uint8_t* p = static_cast<const uint8_t*>( data );
        uint32_t h = 0;

        for ( uint32_t n = size / 4; n; --n, p += 4 ) {
            uint32_t k;
            memcpy( &k, p, sizeof( k ) );
            k *= c1; k = std::rotl( k, 15 ); k *= c2;
            h ^= k; h = std::rotl( h, 13 ); h = h * 5 + 0xe6546b64;
        }

        uint32_t k = 0;
        switch ( size & 3 ) {
        case 3: k ^= static_cast<uint32_t>( p[2] ) << 16; [[fallthrough]];
        case 2: k ^= static_cast<uint32_t>( p[1] ) << 8; [[fallthrough]];
        case 1: k ^= p[0]; k *= c1; k = std::rotl( k, 15 ); k *= c2; h ^= k;
        }

        h ^= size;
        h ^= h >> 16; h *= 0x85ebca6b;
        h ^= h >> 13; h *= 0xc2b2ae35;
        h ^= h >> 16;
        return h;
    }
}

void ConstantBufferPool::Initialize( ID3D11Device* device, uint32_t totalSizeInBytes, const char* debugName ) {
    m_bufferSize = totalSizeInBytes;
    m_currentOffset = 0;
    m_frameIndex = 0;
    m_wrapWarned = false;
    m_debugName = debugName ? debugName : "ConstantBufferPool";

    // One table slot per possible 256-byte allocation; probing gives up rather than overfilling it.
    m_cache.assign( std::bit_ceil( std::max<uint32_t>( m_bufferSize / 256, 1u ) ), CacheEntry{} );
    m_cacheShadow.clear();
    m_cacheStamp = 1;

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

        SetDebugName( frameSlot.Buffer.Get(), m_debugName + "_frame" + std::to_string( i ) );
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

void ConstantBufferPool::ResetCache() {
    // Keeps its capacity, so the shadow converges on the busiest frame's footprint instead of reallocating.
    m_cacheShadow.clear();
    if ( ++m_cacheStamp == 0 ) {
        // The stamp wrapped around, which would make every stale entry read as live again.
        std::fill( m_cache.begin(), m_cache.end(), CacheEntry{} );
        m_cacheStamp = 1;
    }
}

void ConstantBufferPool::BeginFrame() {
    m_frameIndex = (m_frameIndex + 1) % FrameCount;
    WaitForSlot( m_frames[m_frameIndex] );

    m_currentOffset = 0;
    m_wrapWarned = false;
    m_frameAllocations = 0;
    m_frameCacheHits = 0;
    ResetCache();
}

ConstantBufferAllocation ConstantBufferPool::Allocate( const void* pData, uint32_t sizeInBytes, std::source_location where ) {
    uint32_t alignedSize = (sizeInBytes + 255) & ~255;

    FrameSlot& slot = m_frames[m_frameIndex];
    ++m_frameAllocations;

    const bool cacheable = sizeInBytes > 0 && sizeInBytes <= MaxCachedSize && !m_cache.empty();
    const uint32_t hash = cacheable ? HashBytes( pData, sizeInBytes ) : 0;
    const uint32_t mask = static_cast<uint32_t>( m_cache.size() ) - 1;

    if ( cacheable ) {
        for ( uint32_t probe = 0, i = hash & mask; probe < MaxCacheProbes; ++probe, i = (i + 1) & mask ) {
            const CacheEntry& e = m_cache[i];
            if ( e.Stamp != m_cacheStamp ) break; // entries are never removed singly, so a gap ends the chain
            if ( e.Hash == hash && e.Size == sizeInBytes
                && memcmp( m_cacheShadow.data() + e.ShadowOffset, pData, sizeInBytes ) == 0 ) {
                ++m_frameCacheHits;
                ConstantBufferAllocation hit;
                hit.pBuffer = slot.Buffer.Get();
                hit.offsetInBytes = e.BufferOffset;
                hit.sizeInBytes = alignedSize;
                return hit;
            }
        }
    }

    if ( m_currentOffset + alignedSize > m_bufferSize ) {
        m_currentOffset = 0; // wrap within this frame's own buffer
        ResetCache();        // everything cached so far is about to be overwritten
        if ( !m_wrapWarned ) {
            m_wrapWarned = true;
            // Attributed to the allocation that overflowed, not to this file.
            Logging::WrnAt( where, "{} wrapped mid-frame (size {} bytes) allocating {} bytes in {} after {} allocations "
                "({} served from cache); increase the pool size to avoid potential overwrite hazards.",
                m_debugName, m_bufferSize, sizeInBytes, where.function_name(), m_frameAllocations, m_frameCacheHits );
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

    if ( cacheable ) {
        for ( uint32_t probe = 0, i = hash & mask; probe < MaxCacheProbes; ++probe, i = (i + 1) & mask ) {
            CacheEntry& e = m_cache[i];
            if ( e.Stamp == m_cacheStamp ) continue;
            const uint8_t* bytes = static_cast<const uint8_t*>( pData );
            e.Hash = hash;
            e.Size = sizeInBytes;
            e.BufferOffset = m_currentOffset;
            e.ShadowOffset = static_cast<uint32_t>( m_cacheShadow.size() );
            e.Stamp = m_cacheStamp;
            m_cacheShadow.insert( m_cacheShadow.end(), bytes, bytes + sizeInBytes );
            break;
        }
    }

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
