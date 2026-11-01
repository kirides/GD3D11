// D3D11 FFT ocean simulation; see D3D11Ocean.h.
#include "pch.h"
#include "D3D11Ocean.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11CShader.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"
#include "GothicGraphicsState.h"

using Microsoft::WRL::ComPtr;

namespace {
    constexpr UINT kThreads = 8;   // == CS_Ocean.hlsl's [numthreads(8,8,1)]
    constexpr float kFoamDecay = 0.35f, kFoamThreshold = 0.8f, kFoamGain = 2.0f;

    UINT Log2( UINT n ) { UINT r = 0; while ( (1u << r) < n ) ++r; return r; }

    bool CreateStructured( ID3D11Device* device, UINT elements, UINT stride, const char* name, ComPtr<ID3D11Buffer>& buffer, ComPtr<ID3D11UnorderedAccessView>& uav ) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = elements * stride;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = stride;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = elements;
        if ( FAILED( device->CreateBuffer( &bd, nullptr, buffer.ReleaseAndGetAddressOf() ) )
            || FAILED( device->CreateUnorderedAccessView( buffer.Get(), &ud, uav.ReleaseAndGetAddressOf() ) ) ) {
            return false;
        }
        SetDebugName( buffer.Get(), name );
        return true;
    }
}


void D3D11Ocean::Release() {
    m_H0.Reset(); m_WorkA.Reset(); m_WorkB.Reset(); m_Foam.Reset();
    m_H0Uav.Reset(); m_WorkAUav.Reset(); m_WorkBUav.Reset(); m_FoamUav.Reset();
    m_Disp.Reset(); m_Deriv.Reset();
    for ( UINT i = 0; i < kMaxMips; ++i ) { m_DispUav[i].Reset(); m_DerivUav[i].Reset(); m_DispSrvMip[i].Reset(); m_DerivSrvMip[i].Reset(); }
    m_DispSrvFull.Reset(); m_DerivSrvFull.Reset();
    m_Quality = -1;
    m_Ready = false;
}


bool D3D11Ocean::CreateResources( ID3D11Device* device, int quality ) {
    Release();
    m_Cascades = OceanFFT::CascadesFor( quality );
    const UINT n = static_cast<UINT>( m_Cascades.size );
    const UINT count = static_cast<UINT>( m_Cascades.count );
    m_Mips = Log2( n ) + 1;
    const UINT texels = n * n;

    bool ok = CreateStructured( device, texels * count, 16, "OceanH0", m_H0, m_H0Uav )
        && CreateStructured( device, texels, 16, "OceanWorkA", m_WorkA, m_WorkAUav )
        && CreateStructured( device, texels, 16, "OceanWorkB", m_WorkB, m_WorkBUav )
        && CreateStructured( device, texels * count, 4, "OceanFoam", m_Foam, m_FoamUav );

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = n;
    td.Height = n;
    td.MipLevels = m_Mips;
    td.ArraySize = count;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for ( int t = 0; ok && t < 2; ++t ) {
        ComPtr<ID3D11Texture2D>& tex = t == 0 ? m_Disp : m_Deriv;
        ok = SUCCEEDED( device->CreateTexture2D( &td, nullptr, tex.GetAddressOf() ) );
        if ( !ok ) break;
        SetDebugName( tex.Get(), t == 0 ? "OceanDisp" : "OceanDeriv" );
        for ( UINT mip = 0; ok && mip < m_Mips; ++mip ) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = td.Format;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            ud.Texture2DArray.MipSlice = mip;
            ud.Texture2DArray.FirstArraySlice = 0;
            ud.Texture2DArray.ArraySize = count;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = td.Format;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MostDetailedMip = mip;
            sd.Texture2DArray.MipLevels = 1;
            sd.Texture2DArray.FirstArraySlice = 0;
            sd.Texture2DArray.ArraySize = count;
            ok = SUCCEEDED( device->CreateUnorderedAccessView( tex.Get(), &ud, ( t == 0 ? m_DispUav : m_DerivUav )[mip].GetAddressOf() ) )
              && SUCCEEDED( device->CreateShaderResourceView( tex.Get(), &sd, ( t == 0 ? m_DispSrvMip : m_DerivSrvMip )[mip].GetAddressOf() ) );
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC full = {};
        full.Format = td.Format;
        full.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        full.Texture2DArray.MostDetailedMip = 0;
        full.Texture2DArray.MipLevels = m_Mips;
        full.Texture2DArray.FirstArraySlice = 0;
        full.Texture2DArray.ArraySize = count;
        ok = ok && SUCCEEDED( device->CreateShaderResourceView( tex.Get(), &full, ( t == 0 ? m_DispSrvFull : m_DerivSrvFull ).GetAddressOf() ) );
    }
    if ( !ok ) {
        Logging::Wrn( "D3D11: failed to create the ocean simulation resources." );
        Release();
        return false;
    }
    m_Quality = quality;
    m_SpectrumDirty = true;
    Logging::Inf( "D3D11: ocean simulation {} cascades of {}x{}.", count, n, n );
    return true;
}


void D3D11Ocean::FillConsts( PassConsts& c, int cascade, float time, float dt, float fftDirection ) const {
    const OceanFFT::Cascade& cc = m_Cascades.c[cascade];
    const OceanFFT::SpectrumParams& p = m_Params;
    c.tile[0] = cc.length; c.tile[1] = cc.kLow; c.tile[2] = cc.kHigh; c.tile[3] = static_cast<float>( m_Cascades.size );
    c.wind[0] = p.windAlpha; c.wind[1] = p.windOmegaP; c.wind[2] = p.windGamma; c.wind[3] = p.windSpeed;
    c.wind2[0] = p.windAngle; c.wind2[1] = p.spreadXi; c.wind2[2] = p.shortWaveCut; c.wind2[3] = static_cast<float>( cascade );
    c.swell[0] = p.swellAlpha; c.swell[1] = p.swellOmegaP; c.swell[2] = p.swellAngle; c.swell[3] = p.swellSpread;
    c.time[0] = time; c.time[1] = dt; c.time[2] = m_Sea.choppiness; c.time[3] = fftDirection;
    c.foam[0] = kFoamDecay; c.foam[1] = kFoamThreshold; c.foam[2] = kFoamGain; c.foam[3] = OceanFFT::RepeatTime;
}


bool D3D11Ocean::Update( D3D11GraphicsEngine& engine, const GothicRendererSettings& settings ) {
    m_Ready = false;
    if ( settings.WaterSimulation == GothicRendererSettings::WATER_SIM_OFF || FeatureLevel10Compatibility ) {
        if ( m_Quality >= 0 ) Release();
        m_Failed = false;
        return false;
    }

    auto& sm = engine.GetShaderManager();
    const int quality = std::min( static_cast<int>( settings.WaterSimulation ) - 1, 1 );   // D3D11: Medium at most
    auto& csSpectrum = sm.GetCShader( CShaderID::CS_OceanSpectrum );
    auto& csEvolve = sm.GetCShader( CShaderID::CS_OceanEvolve );
    auto& csResolve = sm.GetCShader( CShaderID::CS_OceanResolve );
    auto& csMip = sm.GetCShader( CShaderID::CS_OceanMip );
    auto& csFft = sm.GetCShader( quality == 0 ? CShaderID::CS_OceanFFT128 : CShaderID::CS_OceanFFT256 );
    if ( !csSpectrum || !csEvolve || !csResolve || !csMip || !csFft ) return false;
    if ( m_Failed && quality == m_Quality ) return false;

    if ( quality != m_Quality ) {
        m_Failed = !CreateResources( engine.GetDevice().Get(), quality );
        if ( m_Failed ) return false;
    }
    if ( m_Quality < 0 ) return false;

    OceanFFT::SeaState sea = OceanFFT::SeaStateForLevel( settings.WaterSeaState );
    sea.windAngle = settings.WaterWindAngle * 0.0174532925f;
    sea.swellAngle = sea.windAngle + 0.4f;
    sea.scale = settings.WaterWaveScale;
    sea.choppiness *= settings.WaterChoppiness;
    auto differs = []( float a, float b ) { return std::abs( a - b ) > 0.01f * std::max( 1.0f, std::abs( a ) ); };
    if ( m_SpectrumDirty || differs( sea.windSpeed, m_Sea.windSpeed ) || differs( sea.fetch, m_Sea.fetch ) || differs( sea.windAngle, m_Sea.windAngle )
        || differs( sea.swellHeight, m_Sea.swellHeight ) || differs( sea.swellPeriod, m_Sea.swellPeriod ) || differs( sea.scale, m_Sea.scale ) ) {
        m_SpectrumDirty = true;
        m_Params = OceanFFT::MakeSpectrumParams( sea );
    }
    m_Sea = sea;

    const auto now = std::chrono::steady_clock::now();
    float dt = m_HaveLast ? std::chrono::duration<float>( now - m_Last ).count() : 0.0f;
    m_Last = now;
    m_HaveLast = true;
    dt = std::clamp( dt, 0.0f, 0.1f );
    m_Seconds += dt;
    const float t = static_cast<float>( std::fmod( m_Seconds, static_cast<double>( OceanFFT::RepeatTime ) ) );

    auto _scope = engine.RecordGraphicsEvent( GE_NAME( "Ocean FFT" ) );
    ID3D11DeviceContext* ctx = engine.GetContext().Get();
    const UINT n = static_cast<UINT>( m_Cascades.size );
    const UINT count = static_cast<UINT>( m_Cascades.count );
    const UINT groups = n / kThreads;

    ID3D11UnorderedAccessView* nullUavs[6] = {};
    ID3D11ShaderResourceView* nullSrvs[2] = {};
    auto bindWork = [&]( ID3D11UnorderedAccessView* dispUav, ID3D11UnorderedAccessView* derivUav ) {
        ID3D11UnorderedAccessView* uavs[6] = { m_H0Uav.Get(), m_WorkAUav.Get(), m_WorkBUav.Get(), m_FoamUav.Get(), dispUav, derivUav };
        ctx->CSSetUnorderedAccessViews( 0, 6, uavs, nullptr );
    };
    auto run = [&]( std::shared_ptr<D3D11CShader>& cs, const PassConsts& c, UINT gx, UINT gy ) {
        cs->Apply();
        cs->UpdateBuffer( "OceanCB", &c, sizeof( c ) );
        ctx->Dispatch( gx, gy, 1 );
    };

    if ( m_SpectrumDirty ) {
        bindWork( m_DispUav[0].Get(), m_DerivUav[0].Get() );
        for ( UINT c = 0; c < count; ++c ) {
            PassConsts pc;
            FillConsts( pc, static_cast<int>( c ), t, dt, 0.0f );
            run( csSpectrum, pc, groups, groups );
        }
        m_SpectrumDirty = false;
    }

    bindWork( m_DispUav[0].Get(), m_DerivUav[0].Get() );
    for ( UINT c = 0; c < count; ++c ) {
        PassConsts pc;
        FillConsts( pc, static_cast<int>( c ), t, dt, 0.0f );
        run( csEvolve, pc, groups, groups );
        run( csFft, pc, n, 1 );   // rows
        pc.time[3] = 1.0f;
        run( csFft, pc, n, 1 );   // columns
        pc.time[3] = 0.0f;
        run( csResolve, pc, groups, groups );
    }

    // Mip chain: mip m-1 is read through its SRV while mip m is written
    for ( UINT mip = 1; mip < m_Mips; ++mip ) {
        ctx->CSSetUnorderedAccessViews( 0, 6, nullUavs, nullptr );
        ID3D11UnorderedAccessView* uavs[6] = { nullptr, nullptr, nullptr, nullptr, m_DispUav[mip].Get(), m_DerivUav[mip].Get() };
        ctx->CSSetUnorderedAccessViews( 0, 6, uavs, nullptr );
        ID3D11ShaderResourceView* srvs[2] = { m_DispSrvMip[mip - 1].Get(), m_DerivSrvMip[mip - 1].Get() };
        ctx->CSSetShaderResources( 0, 2, srvs );
        const UINT size = std::max( 1u, n >> mip );
        const UINT g = ( size + kThreads - 1 ) / kThreads;
        for ( UINT c = 0; c < count; ++c ) {
            PassConsts pc;
            FillConsts( pc, static_cast<int>( c ), t, dt, 0.0f );
            pc.tile[3] = static_cast<float>( size );
            run( csMip, pc, g, g );
        }
        ctx->CSSetShaderResources( 0, 2, nullSrvs );
    }

    ctx->CSSetUnorderedAccessViews( 0, 6, nullUavs, nullptr );
    ctx->CSSetShaderResources( 0, 2, nullSrvs );
    ctx->CSSetShader( nullptr, nullptr, 0 );
    m_Ready = true;
    return true;
}
