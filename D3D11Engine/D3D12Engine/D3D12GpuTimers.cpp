// D3D12GpuTimers — -GPUTIMERS timestamp zones, see the header.
#include "../pch.h"
#include "D3D12GpuTimers.h"
#include "D3D12Rhi.h"
#include "../RHI/RhiCmdList.h"
#include "../Logger.h"

using Microsoft::WRL::ComPtr;

namespace {
    constexpr uint64_t kLogIntervalMs = 5000;
}

bool D3D12GpuTimers::Init( Rhi::Device* device, UINT frameSlots ) {
    ID3D12Device* native = D3D12Rhi::NativeDevice( device );
    ID3D12CommandQueue* queue = D3D12Rhi::Native( device->GetDirectQueue() );
    if ( !native || !queue || frameSlots == 0 ) return false;
    UINT64 frequency = 0;
    if ( FAILED( queue->GetTimestampFrequency( &frequency ) ) || frequency == 0 ) return false;
    m_TicksPerMs = static_cast<double>( frequency ) / 1000.0;
    m_FrameSlots = frameSlots;

    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2 * kMaxZones * frameSlots;
    if ( FAILED( native->CreateQueryHeap( &qd, IID_PPV_ARGS( m_Heap.ReleaseAndGetAddressOf() ) ) ) ) return false;

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>( qd.Count ) * sizeof( uint64_t );
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped = nullptr;
    if ( FAILED( device->CreateResource( D3D12_HEAP_TYPE_READBACK, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, m_Readback.ReleaseAndGetAddressOf() ) )
        || FAILED( m_Readback->Map( 0, nullptr, &mapped ) ) ) {
        m_Heap.Reset();
        return false;
    }
    m_ReadbackPtr = static_cast<const uint64_t*>( mapped );
    m_Zones.assign( frameSlots, {} );
    m_LastLog = GetTickCount64();
    Logging::Inf( "D3D12: GPU timers on ({} zones per frame).", kMaxZones );
    return true;
}


void D3D12GpuTimers::Add( std::vector<Sum>& sums, const char* name, double value ) {
    for ( Sum& s : sums ) {
        if ( s.Name == name ) { s.Total += value; ++s.Count; return; }
    }
    sums.push_back( { name, value, 1 } );
}


void D3D12GpuTimers::BeginFrame( UINT frameIndex ) {
    if ( !IsActive() ) return;
    m_Frame = frameIndex % m_FrameSlots;
    std::vector<Zone>& zones = m_Zones[m_Frame];
    if ( !zones.empty() ) {
        const uint64_t* t = m_ReadbackPtr + 2 * kMaxZones * m_Frame;
        for ( const Zone& z : zones ) {
            const uint64_t a = t[z.Query * 2], b = t[z.Query * 2 + 1];
            if ( b > a ) Add( m_Times, z.Name, static_cast<double>( b - a ) / m_TicksPerMs );
        }
        ++m_Frames;
    }
    zones.clear();
    MaybeLog();
}


UINT D3D12GpuTimers::Begin( Rhi::CmdList& cmd, const char* name ) {
    UINT q;
    {
        std::lock_guard<std::mutex> lock( m_Mutex );
        std::vector<Zone>& zones = m_Zones[m_Frame];
        if ( zones.size() >= kMaxZones ) return UINT_MAX;
        q = static_cast<UINT>( zones.size() );
        zones.push_back( { name, q } );
    }
    D3D12Rhi::Native( cmd.Get() )->EndQuery( m_Heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, ( m_Frame * kMaxZones + q ) * 2 );
    return q;
}


void D3D12GpuTimers::End( Rhi::CmdList& cmd, UINT zone ) {
    if ( zone == UINT_MAX ) return;
    D3D12Rhi::Native( cmd.Get() )->EndQuery( m_Heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, ( m_Frame * kMaxZones + zone ) * 2 + 1 );
}


void D3D12GpuTimers::Resolve( Rhi::CmdList& cmd ) {
    if ( !IsActive() ) return;
    const UINT n = static_cast<UINT>( m_Zones[m_Frame].size() );
    if ( n == 0 ) return;
    const UINT first = m_Frame * kMaxZones * 2;
    D3D12Rhi::Native( cmd.Get() )->ResolveQueryData( m_Heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, n * 2,
        D3D12Rhi::Native( m_Readback.Get() ), static_cast<UINT64>( first ) * sizeof( uint64_t ) );
}


void D3D12GpuTimers::Note( const char* name, double value ) {
    if ( IsActive() ) Add( m_Counters, name, value );
}


void D3D12GpuTimers::MaybeLog() {
    const uint64_t now = GetTickCount64();
    if ( now - m_LastLog < kLogIntervalMs || m_Frames == 0 ) return;
    std::string line = std::format( "GPU timers ({} frames, ms):", m_Frames );
    for ( const Sum& s : m_Times ) line += std::format( " {}={:.3f}", s.Name, s.Total / m_Frames );
    if ( !m_Counters.empty() ) line += " |";
    for ( const Sum& s : m_Counters ) line += std::format( " {}={:.0f}", s.Name, s.Total / std::max<uint32_t>( 1u, s.Count ) );
    Logging::Inf( "{}", line );
    m_Times.clear();
    m_Counters.clear();
    m_Frames = 0;
    m_LastLog = now;
}
