#include "pch.h"
#include "D3D11PFX_SAO.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11CShader.h"
#include "D3D11PShader.h"
#include "D3D11VShader.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"

D3D11PFX_SAO::D3D11PFX_SAO( D3D11PfxRenderer* rnd ) : D3D11PFX_Effect( rnd ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto res = engine->GetResolution();

    m_AOBuffer = std::make_unique<RenderToTextureBuffer>(
        engine->GetDevice().Get(), res.x, res.y, DXGI_FORMAT_R8_UNORM,
        nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE );

    m_BlurTempBuffer = std::make_unique<RenderToTextureBuffer>(
        engine->GetDevice().Get(), res.x, res.y, DXGI_FORMAT_R8_UNORM,
        nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE );
}

XRESULT D3D11PFX_SAO::Render(
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalsSRV,
    ID3D11RenderTargetView* outputRTV ) {

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    auto res = engine->GetResolution();

    // Unbind RTVs for compute shader passes
    ID3D11RenderTargetView* nullRtv = nullptr;
    context->OMSetRenderTargets( 1, &nullRtv, nullptr );

    // Recreate buffers if resolution changed
    if ( m_AOBuffer->GetSizeX() != static_cast<UINT>(res.x) ||
         m_AOBuffer->GetSizeY() != static_cast<UINT>(res.y) ) {
        m_AOBuffer = std::make_unique<RenderToTextureBuffer>(
            engine->GetDevice().Get(), res.x, res.y, DXGI_FORMAT_R8_UNORM,
            nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE );
        m_BlurTempBuffer = std::make_unique<RenderToTextureBuffer>(
            engine->GetDevice().Get(), res.x, res.y, DXGI_FORMAT_R8_UNORM,
            nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE );
    }

    auto& saoSettings = Engine::GAPI->GetRendererState().RendererSettings.SaoSettings;
    auto& projMatrix = Engine::GAPI->GetProjectionMatrix();

    auto defaultSampler = engine->GetDefaultSamplerState();
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };

    // --- Pass 1: SAO Main ---
    SAOConstantBuffer saoCB = {};
    saoCB.SAO_ProjParams = float4( 1.0f / projMatrix._11, 1.0f / projMatrix._22, projMatrix._34, projMatrix._33 );
    saoCB.SAO_Radius = saoSettings.Radius * 100.0f; // Scale to match engine units
    saoCB.SAO_Bias = saoSettings.Bias;
    saoCB.SAO_Intensity = saoSettings.Intensity;
    saoCB.SAO_NumSamples = saoSettings.NumSamples;
    saoCB.SAO_InvResolution = float2( 1.0f / res.x, 1.0f / res.y );
    saoCB.SAO_BlurSharpness = saoSettings.BlurSharpness;

    auto saoCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_SAO );
    saoCS->Apply();
    saoCS->GetBuffer( "SAOConstantBuffer" ).Update( &saoCB ).Bind();

    context->CSSetSamplers( 0, 1, &defaultSampler );

    ID3D11ShaderResourceView* saoSRVs[2] = { depthSRV, normalsSRV };
    context->CSSetShaderResources( 0, 2, saoSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, m_AOBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (res.x + 7) / 8, (res.y + 7) / 8, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 2, nullSRVs );

    // --- Pass 2: Bilateral Blur (horizontal) ---
    SAOBlurConstantBuffer blurCB = {};
    blurCB.SAO_Blur_InvResolution = float2( 1.0f / res.x, 1.0f / res.y );
    blurCB.SAO_Blur_Sharpness = saoSettings.BlurSharpness;
    blurCB.SAO_Blur_ProjParams = saoCB.SAO_ProjParams;

    auto blurCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_SAO_Blur );

    // Horizontal pass: AO -> BlurTemp
    blurCB.SAO_Blur_Direction = float2( 1.0f, 0.0f );
    blurCS->Apply();
    blurCS->GetBuffer( "SAOBlurConstantBuffer" ).Update( &blurCB ).Bind();

    context->CSSetSamplers( 0, 1, &defaultSampler );

    ID3D11ShaderResourceView* blurHSRVs[2] = { m_AOBuffer->GetShaderResView().Get(), depthSRV };
    context->CSSetShaderResources( 0, 2, blurHSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, m_BlurTempBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (res.x + 7) / 8, (res.y + 7) / 8, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 2, nullSRVs );

    // --- Pass 3: Bilateral Blur (vertical) ---
    // Vertical pass: BlurTemp -> AO
    blurCB.SAO_Blur_Direction = float2( 0.0f, 1.0f );
    blurCS->Apply();
    blurCS->GetBuffer( "SAOBlurConstantBuffer" ).Update( &blurCB ).Bind();

    context->CSSetSamplers( 0, 1, &defaultSampler );

    ID3D11ShaderResourceView* blurVSRVs[2] = { m_BlurTempBuffer->GetShaderResView().Get(), depthSRV };
    context->CSSetShaderResources( 0, 2, blurVSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, m_AOBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (res.x + 7) / 8, (res.y + 7) / 8, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 2, nullSRVs );
    context->CSSetShader( nullptr, nullptr, 0 );

    // --- Pass 4: Multiply-blend AO onto the scene ---
    // Follow the same pattern as DrawScreenFade()'s modulate blend path:
    // Set blend, depth, shaders, RTV, then DrawFullScreenQuad (which calls UpdateRenderStates)

    Engine::GAPI->GetRendererState().BlendState.SetModulateBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    // Apply VS_PFX + PS_PFX_Simple_R8
    engine->GetShaderManager().GetVShader( VShaderID::VS_PFX )->Apply();
    engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Simple_R8 )->Apply();

    // Set viewport to full resolution
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(res.x);
    vp.Height = static_cast<float>(res.y);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports( 1, &vp );

    // Bind the explicit outputRTV (from render graph), DSV = nullptr
    context->OMSetRenderTargets( 1, &outputRTV, nullptr );

    // Unbind slot 0, then bind AO buffer
    ID3D11ShaderResourceView* nullSRV1 = nullptr;
    context->PSSetShaderResources( 0, 1, &nullSRV1 );
    auto aoSRV = m_AOBuffer->GetShaderResView();
    context->PSSetShaderResources( 0, 1, aoSRV.GetAddressOf() );

    // Draw fullscreen triangle (calls UpdateRenderStates internally)
    FxRenderer->DrawFullScreenQuad();

    // Cleanup: unbind SRV
    context->PSSetShaderResources( 0, 1, &nullSRV1 );

    // Restore default states
    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::DEFAULT_DEPTH_COMP_STATE;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = true;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    return XR_SUCCESS;
}

XRESULT D3D11PFX_SAO::RenderAO(
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalsSRV ) {

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    auto res = engine->GetResolution();

    // Unbind RTVs for compute shader passes
    ID3D11RenderTargetView* nullRtv = nullptr;
    context->OMSetRenderTargets( 1, &nullRtv, nullptr );

    // Recreate buffers if resolution changed
    if ( m_AOBuffer->GetSizeX() != static_cast<UINT>(res.x) ||
         m_AOBuffer->GetSizeY() != static_cast<UINT>(res.y) ) {
        m_AOBuffer = std::make_unique<RenderToTextureBuffer>(
            engine->GetDevice().Get(), res.x, res.y, DXGI_FORMAT_R8_UNORM,
            nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE );
        m_BlurTempBuffer = std::make_unique<RenderToTextureBuffer>(
            engine->GetDevice().Get(), res.x, res.y, DXGI_FORMAT_R8_UNORM,
            nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE );
    }

    auto& saoSettings = Engine::GAPI->GetRendererState().RendererSettings.SaoSettings;
    auto& projMatrix = Engine::GAPI->GetProjectionMatrix();

    auto defaultSampler = engine->GetDefaultSamplerState();
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };

    // --- Pass 1: SAO Main ---
    SAOConstantBuffer saoCB = {};
    saoCB.SAO_ProjParams = float4( 1.0f / projMatrix._11, 1.0f / projMatrix._22, projMatrix._34, projMatrix._33 );
    saoCB.SAO_Radius = saoSettings.Radius * 100.0f;
    saoCB.SAO_Bias = saoSettings.Bias;
    saoCB.SAO_Intensity = saoSettings.Intensity;
    saoCB.SAO_NumSamples = saoSettings.NumSamples;
    saoCB.SAO_InvResolution = float2( 1.0f / res.x, 1.0f / res.y );
    saoCB.SAO_BlurSharpness = saoSettings.BlurSharpness;

    auto saoCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_SAO );
    saoCS->Apply();
    saoCS->GetBuffer( "SAOConstantBuffer" ).Update( &saoCB ).Bind();

    context->CSSetSamplers( 0, 1, &defaultSampler );

    ID3D11ShaderResourceView* saoSRVs[2] = { depthSRV, normalsSRV };
    context->CSSetShaderResources( 0, 2, saoSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, m_AOBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (res.x + 7) / 8, (res.y + 7) / 8, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 2, nullSRVs );

    // --- Pass 2: Bilateral Blur (horizontal) ---
    SAOBlurConstantBuffer blurCB = {};
    blurCB.SAO_Blur_InvResolution = float2( 1.0f / res.x, 1.0f / res.y );
    blurCB.SAO_Blur_Sharpness = saoSettings.BlurSharpness;
    blurCB.SAO_Blur_ProjParams = saoCB.SAO_ProjParams;

    auto blurCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_SAO_Blur );

    blurCB.SAO_Blur_Direction = float2( 1.0f, 0.0f );
    blurCS->Apply();
    blurCS->GetBuffer( "SAOBlurConstantBuffer" ).Update( &blurCB ).Bind();

    context->CSSetSamplers( 0, 1, &defaultSampler );

    ID3D11ShaderResourceView* blurHSRVs[2] = { m_AOBuffer->GetShaderResView().Get(), depthSRV };
    context->CSSetShaderResources( 0, 2, blurHSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, m_BlurTempBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (res.x + 7) / 8, (res.y + 7) / 8, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 2, nullSRVs );

    // --- Pass 3: Bilateral Blur (vertical) ---
    blurCB.SAO_Blur_Direction = float2( 0.0f, 1.0f );
    blurCS->Apply();
    blurCS->GetBuffer( "SAOBlurConstantBuffer" ).Update( &blurCB ).Bind();

    context->CSSetSamplers( 0, 1, &defaultSampler );

    ID3D11ShaderResourceView* blurVSRVs[2] = { m_BlurTempBuffer->GetShaderResView().Get(), depthSRV };
    context->CSSetShaderResources( 0, 2, blurVSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, m_AOBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (res.x + 7) / 8, (res.y + 7) / 8, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 2, nullSRVs );
    context->CSSetShader( nullptr, nullptr, 0 );

    return XR_SUCCESS;
}

ID3D11ShaderResourceView* D3D11PFX_SAO::GetAOResultSRV() const {
    return m_AOBuffer ? m_AOBuffer->GetShaderResView().Get() : nullptr;
}
