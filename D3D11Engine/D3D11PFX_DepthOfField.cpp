#include "pch.h"
#include "D3D11PFX_DepthOfField.h"
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

D3D11PFX_DepthOfField::D3D11PFX_DepthOfField( D3D11PfxRenderer* rnd ) : D3D11PFX_Effect( rnd ), m_FocusIndex( 0 ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R32_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE 
        | (FeatureLevel10Compatibility ? 0 : D3D11_BIND_UNORDERED_ACCESS);

    for ( int i = 0; i < 2; i++ ) {
        engine->GetDevice()->CreateTexture2D( &texDesc, nullptr, m_FocusTexture[i].GetAddressOf() );
        engine->GetDevice()->CreateShaderResourceView( m_FocusTexture[i].Get(), nullptr, m_FocusSRV[i].GetAddressOf() );
        engine->GetDevice()->CreateRenderTargetView( m_FocusTexture[i].Get(), nullptr, m_FocusRTV[i].GetAddressOf() );

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;
        engine->GetDevice()->CreateUnorderedAccessView( m_FocusTexture[i].Get(), &uavDesc, m_FocusUAV[i].GetAddressOf() );
    }
}

XRESULT D3D11PFX_DepthOfField::Render( ID3D11ShaderResourceView* backbuffer ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    engine->SetDefaultStates();

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );
    auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;

    if ( !FeatureLevel10Compatibility ) {
        auto res = RenderCS( backbuffer );
        engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return res;
    }

    auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    auto focusPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_DoF_FocusResolve );
    auto blurPS = engine->GetShaderManager().GetPShader(
        rendererSettings.DoFGaussBlur
            ? PShaderID::PS_PFX_DoF_Gauss
            : PShaderID::PS_PFX_DoF );
    auto compositePS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_DoF_Composite );

    vs->Apply();


    DepthOfFieldConstantBuffer cb = {};
    cb.DoF_FocusRange = rendererSettings.DoFFocusRange;
    cb.DoF_BokehRadius = rendererSettings.DoFBokehRadius;
    cb.DoF_MaxBlur = rendererSettings.DoFMaxBlur;

    auto& proj = Engine::GAPI->GetProjectionMatrix();
    cb.DoF_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._34, proj._33 );
    cb.DoF_NearPlane = Engine::GAPI->GetRendererState().RendererInfo.NearPlane;
    cb.DoF_FarPlane = Engine::GAPI->GetRendererState().RendererInfo.FarPlane;

    // --- Pass 0: Focus Resolve (1x1 temporal smoothing) ---
    int prevIdx = m_FocusIndex;
    int curIdx = 1 - m_FocusIndex;

    focusPS->Apply();
    focusPS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();

    D3D11_VIEWPORT oldVP;
    UINT numVP = 1;
    engine->GetContext()->RSGetViewports( &numVP, &oldVP );
    D3D11_VIEWPORT focusVP = { 0, 0, 1, 1, 0, 1 };
    engine->GetContext()->RSSetViewports( 1, &focusVP );

    engine->GetContext()->OMSetRenderTargets( 1, m_FocusRTV[curIdx].GetAddressOf(), nullptr );
    engine->GetDepthBuffer()->BindToPixelShader( engine->GetContext().Get(), 0 );
    engine->GetContext()->PSSetShaderResources( 1, 1, m_FocusSRV[prevIdx].GetAddressOf() );

    FxRenderer->DrawFullScreenQuad();

    m_FocusIndex = curIdx;
    engine->GetContext()->RSSetViewports( 1, &oldVP );

    ID3D11ShaderResourceView* nullSRV2[2] = { nullptr, nullptr };
    engine->GetContext()->PSSetShaderResources( 0, 2, nullSRV2 );

    // --- Pass 1: Half-res bokeh blur ---
    auto res = engine->GetResolution();
    DXGI_FORMAT bbufferFormat = engine->GetBackBufferFormat();
    auto halfBuffer = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ res.x / 2, res.y / 2, bbufferFormat } );

    D3D11_VIEWPORT halfVP = { 0, 0, static_cast<float>(res.x / 2), static_cast<float>(res.y / 2), 0, 1 };
    engine->GetContext()->RSSetViewports( 1, &halfVP );

    blurPS->Apply();
    blurPS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();

    engine->GetContext()->OMSetRenderTargets( 1, halfBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    // t0 = full-res scene, t1 = full-res depth, t2 = focus (1x1)
    engine->GetContext()->PSSetShaderResources( 0, 1, &backbuffer );
    engine->GetDepthBuffer()->BindToPixelShader( engine->GetContext().Get(), 1 );
    engine->GetContext()->PSSetShaderResources( 2, 1, m_FocusSRV[m_FocusIndex].GetAddressOf() );

    FxRenderer->DrawFullScreenQuad();

    ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
    engine->GetContext()->PSSetShaderResources( 0, 4, nullSRVs );
    engine->GetContext()->RSSetViewports( 1, &oldVP );

    // --- Pass 2: Full-res composite (render to temp, then blit to avoid read-write hazard) ---
    auto compositeBuffer = FxRenderer->GetTempBuffer();

    compositePS->Apply();
    compositePS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();

    engine->GetContext()->OMSetRenderTargets( 1, compositeBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    // t0 = full-res scene, t1 = half-res blur, t2 = full-res depth, t3 = focus (1x1)
    engine->GetContext()->PSSetShaderResources( 0, 1, &backbuffer );
    ID3D11ShaderResourceView* halfSRV = halfBuffer->GetShaderResView().Get();
    engine->GetContext()->PSSetShaderResources( 1, 1, &halfSRV );
    engine->GetDepthBuffer()->BindToPixelShader( engine->GetContext().Get(), 2 );
    engine->GetContext()->PSSetShaderResources( 3, 1, m_FocusSRV[m_FocusIndex].GetAddressOf() );

    FxRenderer->DrawFullScreenQuad();

    engine->GetContext()->PSSetShaderResources( 0, 4, nullSRVs );

    // Blit composite result to backbuffer
    FxRenderer->CopyTextureToRTV( compositeBuffer->GetShaderResView(), oldRTV, res );

    engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

    return XR_SUCCESS;
}

/** Compute shader path for FL11+ */
XRESULT D3D11PFX_DepthOfField::RenderCS( ID3D11ShaderResourceView* backbuffer ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();

    engine->SetDefaultStates();

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    ID3D11RenderTargetView* nullRtv = nullptr;
    engine->GetContext()->OMSetRenderTargets( 1, &nullRtv, nullptr );

    auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;

    DepthOfFieldConstantBuffer cb = {};
    cb.DoF_FocusRange = rendererSettings.DoFFocusRange;
    cb.DoF_BokehRadius = rendererSettings.DoFBokehRadius;
    cb.DoF_MaxBlur = rendererSettings.DoFMaxBlur;

    auto& proj = Engine::GAPI->GetProjectionMatrix();
    cb.DoF_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._34, proj._33 );
    cb.DoF_NearPlane = Engine::GAPI->GetRendererState().RendererInfo.NearPlane;
    cb.DoF_FarPlane = Engine::GAPI->GetRendererState().RendererInfo.FarPlane;

    auto defaultSampler = engine->GetDefaultSamplerState();
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };

    // --- Pass 0: Focus Resolve (1x1 compute) ---
    int prevIdx = m_FocusIndex;
    int curIdx = 1 - m_FocusIndex;

    auto focusCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_DoF_FocusResolve );
    focusCS->Apply();
    focusCS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();

    context->CSSetSamplers( 0, 1, &defaultSampler );

    ID3D11ShaderResourceView* focusSRVs[2] = {
        engine->GetDepthBuffer()->GetShaderResView().Get(),
        m_FocusSRV[prevIdx].Get()
    };
    context->CSSetShaderResources( 0, 2, focusSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, m_FocusUAV[curIdx].GetAddressOf(), nullptr );

    context->Dispatch( 1, 1, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 2, nullSRVs );

    m_FocusIndex = curIdx;

    // --- Pass 1: Half-res bokeh blur ---
    auto res = engine->GetResolution();
    DXGI_FORMAT bbufferFormat = engine->GetBackBufferFormat();
    auto halfBuffer = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ res.x / 2, res.y / 2, bbufferFormat,
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );

    auto blurCS = engine->GetShaderManager().GetCShader(
        rendererSettings.DoFGaussBlur
            ? CShaderID::CS_PFX_DoF_Gauss
            : CShaderID::CS_PFX_DoF );
    blurCS->Apply();
    blurCS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();

    context->CSSetSamplers( 0, 1, &defaultSampler );

    // t0 = full-res scene, t1 = full-res depth, t2 = focus (1x1)
    ID3D11ShaderResourceView* blurSRVs[3] = {
        backbuffer,
        engine->GetDepthBuffer()->GetShaderResView().Get(),
        m_FocusSRV[m_FocusIndex].Get()
    };
    context->CSSetShaderResources( 0, 3, blurSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, halfBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (res.x / 2 + 7) / 8, (res.y / 2 + 7) / 8, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 3, nullSRVs );

    // --- Pass 2: Full-res composite ---
    auto compositeBuffer = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ res.x, res.y, bbufferFormat,
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );

    auto compositeCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_DoF_Composite );
    compositeCS->Apply();
    compositeCS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();

    context->CSSetSamplers( 0, 1, &defaultSampler );

    // t0 = full-res scene, t1 = half-res blur, t2 = full-res depth, t3 = focus (1x1)
    ID3D11ShaderResourceView* compositeSRVs[4] = {
        backbuffer,
        halfBuffer->GetShaderResView().Get(),
        engine->GetDepthBuffer()->GetShaderResView().Get(),
        m_FocusSRV[m_FocusIndex].Get()
    };
    context->CSSetShaderResources( 0, 4, compositeSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, compositeBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (res.x + 7) / 8, (res.y + 7) / 8, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 4, nullSRVs );
    context->CSSetShader( nullptr, nullptr, 0 );

    // Blit composite result to backbuffer
    FxRenderer->CopyTextureToRTV( compositeBuffer->GetShaderResView(), oldRTV, res );

    engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

    return XR_SUCCESS;
}
