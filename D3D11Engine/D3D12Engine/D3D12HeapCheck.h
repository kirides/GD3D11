#pragma once
#include "../RHI/RhiCmdList.h"
#include <cstdint>
#include <unordered_set>
#include <vector>

/** -GPUHEAPCHECK: every GD_HEAP() index is checked on the GPU against a bitmap of live heap slots
    (Shaders/D3D12/include/HeapCheck.hlsl, which mirrors the constants below). Violations are captured at
    the next frame start, read once that frame slot comes round again, and logged once per shader line. */
class D3D12HeapCheck {
public:
    static constexpr UINT kHeapSize = 65536;
    static constexpr UINT kReportSlot = 65533;   // RWByteAddressBuffer
    static constexpr UINT kLiveSlot = 65534;     // ByteAddressBuffer, one bit per slot
    static constexpr UINT kReportMax = 256;
    static constexpr UINT kReportBytes = 4096;   // 16-byte header (count) + kReportMax * { shader id, line, index }
    static constexpr UINT kLiveBytes = kHeapSize / 8;

    bool Init( Rhi::Device* device, D3D12_CPU_DESCRIPTOR_HANDLE reportUav, D3D12_CPU_DESCRIPTOR_HANDLE liveSrv, UINT frameSlots );
    bool IsActive() const { return m_LiveCpu != nullptr; }

    /** The SRV-heap allocator's lock must be held. */
    void SetLive( UINT slot, bool live );
    void SetLiveRange( UINT first, UINT count );

    /** Logs the report of the frame that last used `frameIndex`, then captures and clears it for this frame. */
    void BeginFrame( Rhi::CmdList& cmd, UINT frameIndex );

private:
    void LogReport( UINT frameIndex );

    Microsoft::WRL::ComPtr<Rhi::Resource> m_Live;       // UPLOAD: bitmap + 16 zero bytes for the counter clear
    Microsoft::WRL::ComPtr<Rhi::Resource> m_Report;     // DEFAULT, UAV
    Microsoft::WRL::ComPtr<Rhi::Resource> m_Readback;   // READBACK, one report per frame slot
    uint32_t* m_LiveCpu = nullptr;
    std::vector<uint32_t> m_Bits;          // CPU copy, so the write-combined mapping is never read
    std::vector<bool> m_Captured;          // frame slot holds a capture not yet logged
    bool m_ReportCleared = false;          // the first frame has nothing to capture
    std::unordered_set<uint64_t> m_Logged; // (shader id, line) already reported
    bool m_LoggedCap = false;
    bool m_LoggedOverflow = false;
    uint64_t m_Total = 0;
};
