#pragma once
#include "../RHI/Rhi.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Rhi { class CmdList; }

/** -GPUTIMERS (D3D12 only): timestamp pairs around named passes on the direct queue. Each frame slot's results are
    read when the slot comes round again; averages are logged every few seconds together with Note() counters. */
class D3D12GpuTimers {
public:
    static constexpr UINT kMaxZones = 48;

    bool Init( Rhi::Device* device, UINT frameSlots );
    bool IsActive() const { return m_Heap != nullptr; }

    /** After the slot's fence wait: accumulate what the slot measured last time round. */
    void BeginFrame( UINT frameIndex );
    /** `name` must be a string literal; returns a zone id for End, or UINT_MAX when out of zones. Any thread. */
    UINT Begin( Rhi::CmdList& cmd, const char* name );
    void End( Rhi::CmdList& cmd, UINT zone );
    /** Before the frame's last Close. */
    void Resolve( Rhi::CmdList& cmd );
    /** A per-frame counter, averaged and logged with the timers. */
    void Note( const char* name, double value );

private:
    struct Zone { const char* Name; UINT Query; };
    struct Sum { const char* Name; double Total; uint32_t Count; };
    void Add( std::vector<Sum>& sums, const char* name, double value );
    void MaybeLog();

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_Heap;
    Microsoft::WRL::ComPtr<Rhi::Resource> m_Readback;
    const uint64_t* m_ReadbackPtr = nullptr;
    double m_TicksPerMs = 0.0;
    UINT m_FrameSlots = 0;
    UINT m_Frame = 0;
    std::vector<std::vector<Zone>> m_Zones;   // per frame slot, recorded this or last time round
    std::vector<Sum> m_Times, m_Counters;
    uint32_t m_Frames = 0;
    uint64_t m_LastLog = 0;
    std::mutex m_Mutex;   // Begin from the shadow recording jobs
};

/** Scoped zone; a no-op when the timers are off. */
class D3D12GpuTimerScope {
public:
    D3D12GpuTimerScope( D3D12GpuTimers* timers, Rhi::CmdList& cmd, const char* name )
        : m_T( timers && timers->IsActive() ? timers : nullptr ), m_Cmd( cmd ), m_Zone( m_T ? m_T->Begin( cmd, name ) : UINT_MAX ) {}
    ~D3D12GpuTimerScope() { if ( m_T ) m_T->End( m_Cmd, m_Zone ); }
    D3D12GpuTimerScope( const D3D12GpuTimerScope& ) = delete;
    D3D12GpuTimerScope& operator=( const D3D12GpuTimerScope& ) = delete;
private:
    D3D12GpuTimers* m_T;
    Rhi::CmdList& m_Cmd;
    UINT m_Zone;
};
