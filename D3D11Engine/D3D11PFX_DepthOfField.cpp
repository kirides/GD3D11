#include "pch.h"
#include "D3D11PFX_DepthOfField.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "TexturePool.h"

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
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    for ( int i = 0; i < 2; i++ ) {
        engine->GetDevice()->CreateTexture2D( &texDesc, nullptr, m_FocusTexture[i].GetAddressOf() );
        engine->GetDevice()->CreateShaderResourceView( m_FocusTexture[i].Get(), nullptr, m_FocusSRV[i].GetAddressOf() );
        engine->GetDevice()->CreateRenderTargetView( m_FocusTexture[i].Get(), nullptr, m_FocusRTV[i].GetAddressOf() );
    }
}

D3D11PFX_DepthOfField::~D3D11PFX_DepthOfField() {}

XRESULT D3D11PFX_DepthOfField::Render( ID3D11ShaderResourceView* backbuffer ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    engine->SetDefaultStates();

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    auto vs = engine->GetShaderManager().GetVShader( "VS_PFX" );
    auto focusPS = engine->GetShaderManager().GetPShader( "PS_PFX_DoF_FocusResolve" );
    auto blurPS = engine->GetShaderManager().GetPShader( "PS_PFX_DoF" );
    auto compositePS = engine->GetShaderManager().GetPShader( "PS_PFX_DoF_Composite" );

    vs->Apply();

    auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;

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
    focusPS->GetConstantBuffer()[0]->UpdateBuffer( &cb );
    focusPS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

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
    blurPS->GetConstantBuffer()[0]->UpdateBuffer( &cb );
    blurPS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

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
    compositePS->GetConstantBuffer()[0]->UpdateBuffer( &cb );
    compositePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

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
