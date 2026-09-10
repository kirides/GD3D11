#pragma once
#include <wrl/client.h>
#include <d3d11_1.h>
#include <cstdint>
#include <array>
#include <source_location>
#include <string>
#include <vector>

struct ConstantBufferAllocation {
    ID3D11Buffer* pBuffer = nullptr;
    uint32_t offsetInBytes = 0;
    uint32_t sizeInBytes = 0;

    bool operator==( const ConstantBufferAllocation& other ) const {
        return pBuffer == other.pBuffer && offsetInBytes == other.offsetInBytes && sizeInBytes == other.sizeInBytes;
    }
};

// Ring of FrameCount independently-owned DYNAMIC buffers, one slot used per frame-in-flight.
// Each slot has its own ID3D11Query fence: before a slot is reused (FrameCount frames after it
// was last written), we wait for its fence so we know the GPU is done reading it. This replaces
// relying on D3D11_MAP_WRITE_DISCARD to let the driver rename the backing allocation, which made
// memory usage unpredictable under load.
class ConstantBufferPool {
public:
    static constexpr uint32_t FrameCount = 3;

private:
    struct FrameSlot {
        Microsoft::WRL::ComPtr<ID3D11Buffer> Buffer;
        Microsoft::WRL::ComPtr<ID3D11Query> FrameFence;
        bool FencePending = false;
    };

    // Allocations are immutable once written, so identical bytes within a frame can share one.
    static constexpr uint32_t MaxCachedSize = 1024;
    static constexpr uint32_t MaxCacheProbes = 16;
    struct CacheEntry {
        uint32_t Hash = 0;
        uint32_t Size = 0;
        uint32_t BufferOffset = 0;
        uint32_t ShadowOffset = 0;
        uint32_t Stamp = 0;   // live only while equal to m_cacheStamp
    };

    std::array<FrameSlot, FrameCount> m_frames;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> m_Context;
    uint32_t m_bufferSize = 0;   // size of a single slot's buffer
    uint32_t m_currentOffset = 0;
    uint32_t m_frameIndex = 0;   // slot currently being written to
    bool m_wrapWarned = false;   // warn only once if the ring wraps mid-frame
    std::string m_debugName;

    std::vector<CacheEntry> m_cache;       // open addressing, power-of-two size
    std::vector<uint8_t> m_cacheShadow;    // CPU copy of cached payloads for the exact compare on a hash hit
    uint32_t m_cacheStamp = 1;
    uint32_t m_frameAllocations = 0;
    uint32_t m_frameCacheHits = 0;

    void WaitForSlot( FrameSlot& slot );
    void ResetCache();

public:
    void Initialize( ID3D11Device* device, uint32_t totalSizeInBytes = 4 * 1024 * 1024, const char* debugName = nullptr );

    void BeginFrame();
    ConstantBufferAllocation Allocate( const void* pData, uint32_t sizeInBytes,
        std::source_location where = std::source_location::current() );
    void BindPS( uint32_t slot, const ConstantBufferAllocation& allocation );
    void BindVS( uint32_t slot, const ConstantBufferAllocation& allocation );
    void BindCS( uint32_t slot, const ConstantBufferAllocation& allocation );
    void BindGS( uint32_t slot, const ConstantBufferAllocation& allocation );
    void BindDS( uint32_t slot, const ConstantBufferAllocation& allocation );
    void BindHS( uint32_t slot, const ConstantBufferAllocation& allocation );
    void EndFrame();

    ID3D11Buffer* GetBuffer() const { return m_frames[m_frameIndex].Buffer.Get(); }

    /** Changes whenever earlier allocations may have been overwritten: a new frame, or the ring wrapped. */
    uint32_t GetGeneration() const { return m_cacheStamp; }
};
