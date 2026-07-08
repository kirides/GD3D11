#include "pch.h"
#include "D3D11PFX_Bloom.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11CShader.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "TexturePool.h"

extern bool FeatureLevel10Compatibility;

namespace {
    const int BLOOM_MAX_MIPS = 6;
    // Smallest pyramid mip we allow (in pixels on the shorter side).
    const int BLOOM_MIN_MIP_SIZE = 8;
}

D3D11PFX_Bloom::D3D11PFX_Bloom( D3D11PfxRenderer* rnd ) : D3D11PFX_Effect( rnd ) {
    // Linear + clamp: bilinear taps must not wrap across the opposite screen edge.
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    m_ClampSampler = FxRenderer->GetSampler( sd );
}

XRESULT D3D11PFX_Bloom::Render( ID3D11RenderTargetView* output, ID3D11ShaderResourceView* sceneSrv, INT2 resolution ) {
    // Compute-only effect (no FeatureLevel 10 fallback).
    if ( FeatureLevel10Compatibility )
        return XR_FAILED;

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    INT2 res = resolution;

    if ( res.x < 4 || res.y < 4 )
        return XR_FAILED;

    engine->SetDefaultStates();

    // Save current render targets, then unbind so compute may write the pyramid.
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );
    ID3D11RenderTargetView* nullRtv = nullptr;
    context->OMSetRenderTargets( 1, &nullRtv, nullptr );

    // Determine mip count: mip i is res >> (i+1), stop once the shorter side < min size.
    int mipCount = 0;
    for ( int i = 0; i < BLOOM_MAX_MIPS; i++ ) {
        int mw = res.x >> (i + 1);
        int mh = res.y >> (i + 1);
        if ( mw < BLOOM_MIN_MIP_SIZE || mh < BLOOM_MIN_MIP_SIZE )
            break;
        mipCount++;
    }
    if ( mipCount < 1 )
        mipCount = 1;

    DXGI_FORMAT fmt = engine->GetBackBufferFormat();
    const DXGI_USAGE bindFlags =
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    // Acquire the downsample pyramid up front (all held simultaneously so the pool hands
    // out distinct textures rather than aliasing one).
    std::vector<TextureHandle> down;
    down.reserve( mipCount );
    for ( int i = 0; i < mipCount; i++ ) {
        int mw = std::max( 1, res.x >> (i + 1) );
        int mh = std::max( 1, res.y >> (i + 1) );
        down.push_back( FxRenderer->GetTexturePool()->Acquire(
            TexturePool::Description{ mw, mh, fmt, bindFlags } ) );
    }

    ID3D11SamplerState* sampler = m_ClampSampler.Get();
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11ShaderResourceView* nullSRV2[2] = { nullptr, nullptr };

    BloomConstantBuffer cb = {};
    cb.B_Threshold = settings.BloomThreshold;
    cb.B_Knee = settings.BloomKnee;
    cb.B_Intensity = settings.BloomStrength;
    cb.B_FilterRadius = settings.BloomRadius;

    // --- Pass 1: Prefilter (bright-pass + Karis downsample) full-res scene -> down[0] ---
    {
        auto cs = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_Bloom_Prefilter );
        cs->Apply();
        cb.B_TexelSize = float2( 1.0f / res.x, 1.0f / res.y );
        cs->GetBuffer( "BloomConstantBuffer" ).Update( &cb ).Bind();

        context->CSSetSamplers( 0, 1, &sampler );
        context->CSSetShaderResources( 0, 1, &sceneSrv );
        context->CSSetUnorderedAccessViews( 0, 1, down[0]->GetUnorderedAccessView().GetAddressOf(), nullptr );

        context->Dispatch( (down[0]->GetSizeX() + 7) / 8, (down[0]->GetSizeY() + 7) / 8, 1 );

        context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
        context->CSSetShaderResources( 0, 1, nullSRV2 );
    }

    // --- Downsample chain: down[i-1] -> down[i] ---
    {
        auto cs = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_Bloom_Downsample );
        cs->Apply();
        context->CSSetSamplers( 0, 1, &sampler );

        for ( int i = 1; i < mipCount; i++ ) {
            cb.B_TexelSize = float2( 1.0f / down[i - 1]->GetSizeX(), 1.0f / down[i - 1]->GetSizeY() );
            cs->GetBuffer( "BloomConstantBuffer" ).Update( &cb ).Bind();

            ID3D11ShaderResourceView* srv = down[i - 1]->GetShaderResView().Get();
            context->CSSetShaderResources( 0, 1, &srv );
            context->CSSetUnorderedAccessViews( 0, 1, down[i]->GetUnorderedAccessView().GetAddressOf(), nullptr );

            context->Dispatch( (down[i]->GetSizeX() + 7) / 8, (down[i]->GetSizeY() + 7) / 8, 1 );

            context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
            context->CSSetShaderResources( 0, 1, nullSRV2 );
        }
    }

    // --- Upsample chain: current = down[N-1]; for i = N-2..0: up[i] = down[i] + tent(current) ---
    // up[i] lives in its own buffer to avoid read/write aliasing with down[i].
    std::vector<TextureHandle> up( mipCount );
    RenderToTextureBuffer* finalBloom = down[0].get();

    if ( mipCount >= 2 ) {
        auto cs = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_Bloom_Upsample );
        cs->Apply();
        context->CSSetSamplers( 0, 1, &sampler );

        for ( int i = mipCount - 2; i >= 0; i-- ) {
            RenderToTextureBuffer* source = (i == mipCount - 2) ? down[mipCount - 1].get() : up[i + 1].get();

            int mw = std::max( 1, res.x >> (i + 1) );
            int mh = std::max( 1, res.y >> (i + 1) );
            up[i] = FxRenderer->GetTexturePool()->Acquire(
                TexturePool::Description{ mw, mh, fmt, bindFlags } );

            cb.B_TexelSize = float2( 1.0f / source->GetSizeX(), 1.0f / source->GetSizeY() );
            cs->GetBuffer( "BloomConstantBuffer" ).Update( &cb ).Bind();

            ID3D11ShaderResourceView* upSRVs[2] = {
                source->GetShaderResView().Get(),   // t0 = lower mip to upsample
                down[i]->GetShaderResView().Get()   // t1 = this level's downsampled mip
            };
            context->CSSetShaderResources( 0, 2, upSRVs );
            context->CSSetUnorderedAccessViews( 0, 1, up[i]->GetUnorderedAccessView().GetAddressOf(), nullptr );

            context->Dispatch( (up[i]->GetSizeX() + 7) / 8, (up[i]->GetSizeY() + 7) / 8, 1 );

            context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
            context->CSSetShaderResources( 0, 2, nullSRV2 );
        }

        finalBloom = up[0].get();
    }

    context->CSSetShader( nullptr, nullptr, 0 );

    // --- Composite: additively blend the (half-res) bloom onto the scene ---
    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    engine->GetShaderManager().GetVShader( VShaderID::VS_PFX )->Apply();
    auto compositePS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_BloomComposite );
    compositePS->Apply();
    compositePS->GetBuffer( "BloomConstantBuffer" ).Update( &cb ).Bind();

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(res.x);
    vp.Height = static_cast<float>(res.y);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports( 1, &vp );

    context->PSSetSamplers( 0, 1, &sampler );
    context->OMSetRenderTargets( 1, &output, nullptr );

    ID3D11ShaderResourceView* nullSRV1 = nullptr;
    context->PSSetShaderResources( 0, 1, &nullSRV1 );
    ID3D11ShaderResourceView* bloomSRV = finalBloom->GetShaderResView().Get();
    context->PSSetShaderResources( 0, 1, &bloomSRV );

    FxRenderer->DrawFullScreenQuad();

    context->PSSetShaderResources( 0, 1, &nullSRV1 );

    // Restore states + render targets.
    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::DEFAULT_DEPTH_COMP_STATE;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = true;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

    return XR_SUCCESS;
}
