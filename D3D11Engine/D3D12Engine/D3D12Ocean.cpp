// D3D12/Vulkan FFT ocean simulation; see D3D12Ocean.h.
#include "../pch.h"
#include "D3D12Ocean.h"
#include "D3D12GraphicsEngine.h"
#include "../GothicGraphicsState.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    constexpr UINT kThreads = 8;   // == Ocean.hlsl's [numthreads(8,8,1)]
    constexpr float kFoamDecay = 0.35f, kFoamThreshold = 0.8f, kFoamGain = 2.0f;

    D3D12_RESOURCE_DESC BufferDesc( UINT64 bytes ) {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return d;
    }

    UINT Log2( UINT n ) { UINT r = 0; while ( (1u << r) < n ) ++r; return r; }
}


bool D3D12Ocean::AllocateSlots() {
    if ( m_SlotsAllocated ) return true;
    auto alloc = [&]( UINT& s ) { s = m_E->AllocateSrvSlot(); return s != UINT_MAX; };
    bool ok = alloc( m_SlotH0 ) && alloc( m_SlotWorkA ) && alloc( m_SlotWorkB ) && alloc( m_SlotFoam )
        && alloc( m_SlotDispFull ) && alloc( m_SlotDerivFull );
    for ( UINT i = 0; ok && i < kMaxMips; ++i ) {
        ok = alloc( m_SlotDispUav[i] ) && alloc( m_SlotDerivUav[i] ) && alloc( m_SlotDispSrvMip[i] ) && alloc( m_SlotDerivSrvMip[i] );
    }
    if ( !ok ) {
        Logging::Wrn( "D3D12: no descriptor slots left for the ocean simulation." );
        return false;
    }
    m_SlotsAllocated = true;   // never freed: bounded (46 slots), reused across quality changes
    return true;
}


void D3D12Ocean::Release() {
    // The slots stay allocated; their descriptors are rewritten by the next CreateResources
    for ( auto* r : { std::addressof( m_H0 ), std::addressof( m_WorkA ), std::addressof( m_WorkB ), std::addressof( m_Foam ), std::addressof( m_Disp ), std::addressof( m_Deriv ) } ) {
        if ( *r ) m_E->QueueResourceForRelease( std::move( *r ) );
        r->Reset();
    }
    m_Quality = -1;
    m_Ready = false;
    m_TexInSrvState = false;
}


bool D3D12Ocean::CreateResources( int quality ) {
    Release();
    if ( !AllocateSlots() ) return false;

    m_Cascades = OceanFFT::CascadesFor( quality );
    const UINT n = static_cast<UINT>( m_Cascades.size );
    const UINT count = static_cast<UINT>( m_Cascades.count );
    m_Mips = Log2( n ) + 1;
    Rhi::Device* device = m_E->m_Rhi.Get();

    const UINT64 texels = static_cast<UINT64>( n ) * n;
    struct Buf { ComPtr<Rhi::Resource>* res; UINT64 bytes; const wchar_t* name; };
    const Buf bufs[] = {
        { std::addressof( m_H0 ), texels * count * 16, L"OceanH0" },
        { std::addressof( m_WorkA ), texels * 16, L"OceanWorkA" },
        { std::addressof( m_WorkB ), texels * 16, L"OceanWorkB" },
        { std::addressof( m_Foam ), texels * count * 4, L"OceanFoam" },
    };
    for ( const Buf& b : bufs ) {
        const D3D12_RESOURCE_DESC d = BufferDesc( b.bytes );
        if ( FAILED( device->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, b.res->ReleaseAndGetAddressOf() ) ) ) {
            Logging::Wrn( "D3D12: failed to create the ocean buffer {}.", static_cast<int>( b.bytes >> 20 ) );
            Release();
            return false;
        }
        (*b.res)->SetName( b.name );
    }

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = n;
    td.Height = n;
    td.DepthOrArraySize = static_cast<UINT16>( count );
    td.MipLevels = static_cast<UINT16>( m_Mips );
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for ( auto [res, name] : { std::pair{ std::addressof( m_Disp ), L"OceanDisp" }, std::pair{ std::addressof( m_Deriv ), L"OceanDeriv" } } ) {
        if ( FAILED( device->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            res->ReleaseAndGetAddressOf(), Rhi::RESOURCE_FLAG_TRACK_LAYOUT ) ) ) {
            Logging::Wrn( "D3D12: failed to create the ocean displacement textures." );
            Release();
            return false;
        }
        (*res)->SetName( name );
    }
    m_TexInSrvState = false;
    m_Quality = quality;
    WriteDescriptors();
    m_SpectrumDirty = true;
    Logging::Inf( "D3D12: ocean simulation {} cascades of {}x{}.", count, n, n );
    return true;
}


void D3D12Ocean::WriteDescriptors() {
    Rhi::Device* device = m_E->m_Rhi.Get();
    const UINT n = static_cast<UINT>( m_Cascades.size );
    const UINT count = static_cast<UINT>( m_Cascades.count );

    auto bufUav = [&]( UINT slot, Rhi::Resource* r, UINT elements, UINT stride ) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.Format = DXGI_FORMAT_UNKNOWN;
        u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = elements;
        u.Buffer.StructureByteStride = stride;
        device->CreateUnorderedAccessView( r, nullptr, &u, m_E->GetSrvCpuHandle( slot ) );
    };
    bufUav( m_SlotH0, m_H0.Get(), n * n * count, 16 );
    bufUav( m_SlotWorkA, m_WorkA.Get(), n * n, 16 );
    bufUav( m_SlotWorkB, m_WorkB.Get(), n * n, 16 );
    bufUav( m_SlotFoam, m_Foam.Get(), n * n * count, 4 );

    for ( UINT mip = 0; mip < kMaxMips; ++mip ) {
        const UINT m = std::min( mip, m_Mips - 1 );   // unused slots mirror the last mip
        for ( int t = 0; t < 2; ++t ) {
            Rhi::Resource* r = t == 0 ? m_Disp.Get() : m_Deriv.Get();
            D3D12_UNORDERED_ACCESS_VIEW_DESC u = {};
            u.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            u.Texture2DArray.MipSlice = m;
            u.Texture2DArray.ArraySize = count;
            device->CreateUnorderedAccessView( r, nullptr, &u, m_E->GetSrvCpuHandle( t == 0 ? m_SlotDispUav[mip] : m_SlotDerivUav[mip] ) );

            D3D12_SHADER_RESOURCE_VIEW_DESC s = {};
            s.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.Texture2DArray.MostDetailedMip = m;
            s.Texture2DArray.MipLevels = 1;
            s.Texture2DArray.ArraySize = count;
            device->CreateShaderResourceView( r, &s, m_E->GetSrvCpuHandle( t == 0 ? m_SlotDispSrvMip[mip] : m_SlotDerivSrvMip[mip] ) );
        }
    }
    for ( int t = 0; t < 2; ++t ) {
        D3D12_SHADER_RESOURCE_VIEW_DESC s = {};
        s.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2DArray.MostDetailedMip = 0;
        s.Texture2DArray.MipLevels = m_Mips;
        s.Texture2DArray.ArraySize = count;
        device->CreateShaderResourceView( t == 0 ? m_Disp.Get() : m_Deriv.Get(), &s, m_E->GetSrvCpuHandle( t == 0 ? m_SlotDispFull : m_SlotDerivFull ) );
    }
}


void D3D12Ocean::FillConsts( PassConsts& c, int cascade, float time, float dt, float fftDirection ) const {
    const OceanFFT::Cascade& cc = m_Cascades.c[cascade];
    const OceanFFT::SpectrumParams& p = OceanFFT::GetSurface().Params();
    c.tile[0] = cc.length; c.tile[1] = cc.kLow; c.tile[2] = cc.kHigh; c.tile[3] = static_cast<float>( m_Cascades.size );
    c.wind[0] = p.windAlpha; c.wind[1] = p.windOmegaP; c.wind[2] = p.windGamma; c.wind[3] = p.windSpeed;
    c.wind2[0] = p.windAngle; c.wind2[1] = p.spreadXi; c.wind2[2] = p.shortWaveCut; c.wind2[3] = static_cast<float>( cascade );
    c.swell[0] = p.swellAlpha; c.swell[1] = p.swellOmegaP; c.swell[2] = p.swellAngle; c.swell[3] = p.swellSpread;
    c.time[0] = time; c.time[1] = dt; c.time[2] = OceanFFT::GetSurface().Sea().choppiness; c.time[3] = fftDirection;
    c.foam[0] = kFoamDecay; c.foam[1] = kFoamThreshold; c.foam[2] = kFoamGain; c.foam[3] = OceanFFT::RepeatTime;
    c.idxA[0] = m_SlotH0; c.idxA[1] = m_SlotWorkA; c.idxA[2] = m_SlotWorkB; c.idxA[3] = m_SlotFoam;
    c.idxB[0] = m_SlotDispUav[0]; c.idxB[1] = m_SlotDerivUav[0]; c.idxB[2] = m_SlotDispSrvMip[0]; c.idxB[3] = m_SlotDerivSrvMip[0];
}


bool D3D12Ocean::Update( const GothicRendererSettings& settings ) {
    m_Ready = false;
    if ( settings.WaterSimulation == GothicRendererSettings::WATER_SIM_OFF ) {
        if ( m_Quality >= 0 ) Release();
        m_Failed = false;
        return false;
    }
    if ( !m_E->m_FrameOpen ) return false;

    auto& po = m_E->m_Pipelines.Ocean;
    OceanFFT::Surface& surface = OceanFFT::GetSurface();
    surface.Refresh( settings.WaterSimulation, settings.WaterSeaState, settings.WaterWindAngle, settings.WaterWaveScale, settings.WaterChoppiness, 2 );
    const int quality = surface.Quality();
    const int fftIndex = quality;   // 0:128, 1:256, 2:512
    if ( quality < 0 || !po.Spectrum.PSO || !po.Evolve.PSO || !po.Resolve.PSO || !po.Mip.PSO || !po.Fft[fftIndex].PSO ) return false;
    if ( m_Failed && quality == m_Quality ) return false;

    if ( quality != m_Quality ) {
        m_Failed = !CreateResources( quality );
        if ( m_Failed ) return false;
    }
    if ( m_Quality < 0 ) return false;
    if ( surface.Generation() != m_Generation ) {   // the sea state moved: bake the spectrum again
        m_Generation = surface.Generation();
        m_SpectrumDirty = true;
    }

    const double seconds = surface.Seconds();
    const float dt = static_cast<float>( std::clamp( seconds - m_PrevSeconds, 0.0, 0.1 ) );
    m_PrevSeconds = seconds;
    const float t = surface.WrappedSeconds();

    DX_ZONE( m_E->m_CmdList.Get(), "Ocean FFT" );
    D3D12CmdList& cmd = m_E->m_CmdList;
    const UINT n = static_cast<UINT>( m_Cascades.size );
    const UINT count = static_cast<UINT>( m_Cascades.count );
    const UINT groups = n / kThreads;
    const D3D12_RESOURCE_STATES srvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    if ( m_TexInSrvState ) {
        cmd->TransitionBarriers( { { m_Disp.Get(), srvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
                                   { m_Deriv.Get(), srvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } } );
        m_TexInSrvState = false;
    }

    auto run = [&]( const D3D12PipelineState::ComputePipeline& pipe, const PassConsts& c, UINT gx, UINT gy ) {
        cmd->SetPipelineState( pipe.PSO.Get() );
        cmd->SetComputeRootSignature( pipe.RootSig.Get() );
        cmd->SetComputeRoot32BitConstants( 0, 32, &c, 0 );
        cmd->Dispatch( gx, gy, 1 );
    };

    if ( m_SpectrumDirty ) {
        for ( UINT c = 0; c < count; ++c ) {
            PassConsts pc;
            FillConsts( pc, static_cast<int>( c ), t, dt, 0.0f );
            run( po.Spectrum, pc, groups, groups );
        }
        cmd->UAVBarriers( { m_H0.Get() } );
        m_SpectrumDirty = false;
    }

    for ( UINT c = 0; c < count; ++c ) {
        PassConsts pc;
        FillConsts( pc, static_cast<int>( c ), t, dt, 0.0f );
        run( po.Evolve, pc, groups, groups );
        cmd->UAVBarriers( { m_WorkA.Get(), m_WorkB.Get() } );
        run( po.Fft[fftIndex], pc, n, 1 );   // rows
        cmd->UAVBarriers( { m_WorkA.Get(), m_WorkB.Get() } );
        pc.time[3] = 1.0f;
        run( po.Fft[fftIndex], pc, n, 1 );   // columns
        cmd->UAVBarriers( { m_WorkA.Get(), m_WorkB.Get() } );
        pc.time[3] = 0.0f;
        run( po.Resolve, pc, groups, groups );
        cmd->UAVBarriers( { m_WorkA.Get(), m_WorkB.Get(), m_Foam.Get() } );   // the next cascade overwrites the work buffers
    }

    // Mip chain: mip m-1 of every slice becomes a shader resource, mip m is written
    const auto sub = [&]( UINT mip, UINT slice ) { return mip + slice * m_Mips; };
    for ( UINT mip = 1; mip < m_Mips; ++mip ) {
        std::vector<Rhi::ResourceTransition> tr;
        for ( UINT c = 0; c < count; ++c ) {
            tr.push_back( { m_Disp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvState, sub( mip - 1, c ) } );
            tr.push_back( { m_Deriv.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvState, sub( mip - 1, c ) } );
        }
        cmd->TransitionBarriers( tr.data(), static_cast<UINT>( tr.size() ) );
        const UINT size = std::max( 1u, n >> mip );
        const UINT g = ( size + kThreads - 1 ) / kThreads;
        for ( UINT c = 0; c < count; ++c ) {
            PassConsts pc;
            FillConsts( pc, static_cast<int>( c ), t, dt, 0.0f );
            pc.tile[3] = static_cast<float>( size );
            pc.idxB[0] = m_SlotDispUav[mip]; pc.idxB[1] = m_SlotDerivUav[mip];
            pc.idxB[2] = m_SlotDispSrvMip[mip - 1]; pc.idxB[3] = m_SlotDerivSrvMip[mip - 1];
            run( po.Mip, pc, g, g );
        }
    }
    // the last mip is still a UAV; the rest are already shader resources
    {
        std::vector<Rhi::ResourceTransition> tr;
        for ( UINT c = 0; c < count; ++c ) {
            tr.push_back( { m_Disp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvState, sub( m_Mips - 1, c ) } );
            tr.push_back( { m_Deriv.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvState, sub( m_Mips - 1, c ) } );
        }
        cmd->TransitionBarriers( tr.data(), static_cast<UINT>( tr.size() ) );
    }
    m_TexInSrvState = true;
    m_Ready = true;
    return true;
}
