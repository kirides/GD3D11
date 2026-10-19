#include "../pch.h"
#include "D3D12HeapCheck.h"
#include "D3D12ShaderBackend.h"
#include "../Logger.h"

namespace {
    constexpr UINT kZeroOffset = D3D12HeapCheck::kLiveBytes;   // 16 zero bytes after the bitmap
    constexpr UINT kMaxLoggedLines = 64;

    D3D12_RESOURCE_DESC BufferDesc( UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE ) {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        return d;
    }
}

bool D3D12HeapCheck::Init( Rhi::Device* device, D3D12_CPU_DESCRIPTOR_HANDLE reportUav, D3D12_CPU_DESCRIPTOR_HANDLE liveSrv, UINT frameSlots ) {
    const D3D12_RESOURCE_DESC liveDesc = BufferDesc( kLiveBytes + 256 );
    const D3D12_RESOURCE_DESC reportDesc = BufferDesc( kReportBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
    const D3D12_RESOURCE_DESC readbackDesc = BufferDesc( static_cast<UINT64>( kReportBytes ) * frameSlots );
    if ( FAILED( device->CreateResource( D3D12_HEAP_TYPE_UPLOAD, &liveDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_Live.ReleaseAndGetAddressOf() ) )
        || FAILED( device->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &reportDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, m_Report.ReleaseAndGetAddressOf() ) )
        || FAILED( device->CreateResource( D3D12_HEAP_TYPE_READBACK, &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, m_Readback.ReleaseAndGetAddressOf() ) ) ) {
        Logging::Wrn( "GPU heap check: buffer creation failed; the check stays off." );
        return false;
    }
    m_Live->SetName( L"HeapCheckLive" );
    m_Report->SetName( L"HeapCheckReport" );
    m_Readback->SetName( L"HeapCheckReadback" );

    D3D12_RANGE noRead = { 0, 0 };
    void* mapped = nullptr;
    if ( FAILED( m_Live->Map( 0, &noRead, &mapped ) ) || !mapped ) return false;
    m_Bits.assign( kLiveBytes / 4, 0u );
    std::memset( mapped, 0, kLiveBytes + 256 );
    m_Captured.assign( frameSlots, false );

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = kLiveBytes / 4;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView( m_Live.Get(), &srv, liveSrv );

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = kReportBytes / 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView( m_Report.Get(), nullptr, &uav, reportUav );

    m_LiveCpu = static_cast<uint32_t*>( mapped );
    SetLiveRange( kReportSlot, kHeapSize - kReportSlot );   // the check's own slots and the MaterialFx table
    Logging::Wrn( "GPU heap check ON (-GPUHEAPCHECK): every bindless heap index is validated; expect lower GPU performance." );
    return true;
}

void D3D12HeapCheck::SetLive( UINT slot, bool live ) {
    if ( !m_LiveCpu || slot >= kHeapSize ) return;
    uint32_t& word = m_Bits[slot >> 5];
    const uint32_t bit = 1u << ( slot & 31u );
    word = live ? ( word | bit ) : ( word & ~bit );
    m_LiveCpu[slot >> 5] = word;
}

void D3D12HeapCheck::SetLiveRange( UINT first, UINT count ) {
    for ( UINT i = 0; i < count; ++i ) SetLive( first + i, true );
}

void D3D12HeapCheck::BeginFrame( Rhi::CmdList& cmd, UINT frameIndex ) {
    if ( !m_LiveCpu || frameIndex >= m_Captured.size() ) return;
    if ( m_Captured[frameIndex] ) LogReport( frameIndex );

    // Buffers decay to COMMON between submits, so the list starts with the report in COMMON.
    if ( m_ReportCleared ) {
        cmd->CopyBufferRegion( m_Readback.Get(), static_cast<UINT64>( frameIndex ) * kReportBytes, m_Report.Get(), 0, kReportBytes );
        cmd->TransitionBarrier( m_Report.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST );
        m_Captured[frameIndex] = true;
    }
    cmd->CopyBufferRegion( m_Report.Get(), 0, m_Live.Get(), kZeroOffset, 16 );
    cmd->TransitionBarrier( m_Report.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
    m_ReportCleared = true;
}

void D3D12HeapCheck::LogReport( UINT frameIndex ) {
    m_Captured[frameIndex] = false;
    const SIZE_T begin = static_cast<SIZE_T>( frameIndex ) * kReportBytes;
    D3D12_RANGE range = { begin, begin + kReportBytes };
    void* mapped = nullptr;
    if ( FAILED( m_Readback->Map( 0, &range, &mapped ) ) || !mapped ) return;
    const auto* data = reinterpret_cast<const uint32_t*>( static_cast<const uint8_t*>( mapped ) + begin );
    const uint32_t count = data[0];
    if ( count ) {
        m_Total += count;
        if ( count > kReportMax && !m_LoggedOverflow ) {
            m_LoggedOverflow = true;
            Logging::Wrn( "GPU heap check: {} violations in one frame, only the first {} are recorded.", count, kReportMax );
        }
        for ( uint32_t i = 0; i < std::min( count, kReportMax ); ++i ) {
            const uint32_t* e = data + 4 + i * 3;
            const uint64_t key = ( static_cast<uint64_t>( e[0] ) << 32 ) | e[1];
            if ( m_Logged.count( key ) ) continue;
            if ( m_Logged.size() >= kMaxLoggedLines ) {
                if ( !m_LoggedCap ) Logging::Wrn( "GPU heap check: more than {} distinct shader lines reported; further ones are not logged.", kMaxLoggedLines );
                m_LoggedCap = true;
                break;
            }
            m_Logged.insert( key );
            Logging::Err( "GPU heap check: {} line {} accessed heap index {} ({}); {} violations so far.",
                D3D12ShaderBackend::ShaderNameForId( e[0] ), e[1], e[2], e[2] >= kHeapSize ? "out of range" : "not a live slot", m_Total );
        }
    }
    D3D12_RANGE noWrite = { 0, 0 };
    m_Readback->Unmap( 0, &noWrite );
}
