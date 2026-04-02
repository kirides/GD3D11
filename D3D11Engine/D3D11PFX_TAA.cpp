#include "pch.h"
#include "D3D11PFX_TAA.h"

#include <FidelityFX/host/ffx_fsr2.h>

#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "D3D11ShaderManager.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ConstantBuffer.h"
#include "GothicAPI.h"

D3D11PFX_TAA::D3D11PFX_TAA(D3D11PfxRenderer* rnd) 
    : D3D11PFX_Effect(rnd)
    , m_JitterIndex(0)
    , m_Width(0)
    , m_Height(0)
    , m_FirstFrame(true) {
    
    m_CurrentJitter = XMFLOAT2(0, 0);
    m_PreviousJitter = XMFLOAT2(0, 0);
    m_PrevCameraPosition = XMFLOAT3(0, 0, 0);
    XMStoreFloat4x4( &m_PrevViewProj, XMMatrixIdentity() );
    XMStoreFloat4x4( &m_UnjitteredViewProj, XMMatrixIdentity() );
}

bool D3D11PFX_TAA::Init() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    
    // Create TAA constant buffer
    TAAConstantBuffer cb = {};
    m_TAAConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
        sizeof(TAAConstantBuffer), &cb);
    
    // Create velocity constant buffer
    VelocityBufferConstantBuffer vcb = {};
    m_VelocityConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
        sizeof(VelocityBufferConstantBuffer), &vcb);
    
    // Linear sampler for color sampling
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    engine->GetDevice()->CreateSamplerState(&sampDesc, m_samplerLinear.GetAddressOf());

    // Point sampler for depth/velocity sampling (no filtering)
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    engine->GetDevice()->CreateSamplerState( &sampDesc, m_samplerPoint.GetAddressOf() );

    // Create history buffer (same format as back buffer for color)
    DXGI_FORMAT format = engine->GetBackBufferFormat();
    m_HistoryBuffer = std::make_unique<RenderToTextureBuffer>(
        engine->GetDevice().Get(), m_Width, m_Height, format);

    SetDebugName( m_HistoryBuffer->GetTexture().Get(), "TAA_HistoryBuffer" );
    SetDebugName( m_HistoryBuffer->GetRenderTargetView().Get(), "TAA_HistoryBuffer_RTV" );
    SetDebugName( m_HistoryBuffer->GetShaderResView().Get(), "TAA_HistoryBuffer_SRV" );
    
    // Create velocity buffer (RG16F for 2D motion vectors)
    // Using R16G16_FLOAT for high precision motion vectors
    m_VelocityBuffer = std::make_unique<RenderToTextureBuffer>(
        engine->GetDevice().Get(), m_Width, m_Height, DXGI_FORMAT_R16G16_FLOAT);

    SetDebugName( m_VelocityBuffer->GetTexture().Get(), "TAA_VelocityBuffer" );
    SetDebugName( m_VelocityBuffer->GetRenderTargetView().Get(), "TAA_VelocityBuffer_RTV" );
    SetDebugName( m_VelocityBuffer->GetShaderResView().Get(), "TAA_VelocityBuffer_SRV" );
    
    // Reset state on resize
    m_FirstFrame = true;
    m_JitterIndex = 0;
    m_CurrentJitter = XMFLOAT2(0, 0);
    m_PreviousJitter = XMFLOAT2(0, 0);
    
    return true;
}

void D3D11PFX_TAA::OnResize(const INT2& size) {
    if (size.x == m_Width && size.y == m_Height) {
        return;
    }
    m_recreate = true;
    m_Width = size.x;
    m_Height = size.y;
}

void D3D11PFX_TAA::OnDisabled() {
    m_CurrentJitter = XMFLOAT2( 0, 0 );
    m_PreviousJitter = XMFLOAT2( 0, 0 );
    m_JitterIndex = 0;
    m_FirstFrame = true;
}

void D3D11PFX_TAA::ReleaseResources() {
    if ( m_recreate ) {
        return;
    }
    m_recreate = true;

    m_HistoryBuffer.reset();
    m_VelocityBuffer.reset();
    m_TAAConstantBuffer.reset();
    m_VelocityConstantBuffer.reset();
    m_samplerLinear.Reset();
    m_samplerPoint.Reset();
}

void D3D11PFX_TAA::AdvanceJitter() {    
    // Store the previous jitter for removal
    m_PreviousJitter = m_CurrentJitter;

    // Advance to next jitter sample
    auto renderWidth = Engine::GraphicsEngine->GetResolution().x;
    auto displayWidth = Engine::GraphicsEngine->GetBackbufferResolution().x;
    const int32_t phaseCount = ffxFsr2GetJitterPhaseCount( renderWidth, displayWidth );

    // 2. Advance index safely
    if ( phaseCount > 0 ) {
        m_JitterIndex = (m_JitterIndex + 1) % phaseCount;
    } else {
        m_JitterIndex = 0;
    }

    // 3. Calculate FSR2 jitter offset for the current index
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    if ( phaseCount > 0 ) {
        ffxFsr2GetJitterOffset( &jitterX, &jitterY, m_JitterIndex, phaseCount );
    }

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );

    auto projF = Engine::GAPI->GetProjectionMatrix();
    
    // Remove the jitter applied in the previous frame.
    // just safety, as zEngine always resets Projection on frame start
    projF._13 = 0;
    projF._23 = 0;
    
    XMMATRIX viewProj = XMMatrixMultiply( XMLoadFloat4x4( &projF ), view );
    
    // row-major view proj
    XMStoreFloat4x4( &m_UnjitteredViewProj, viewProj );

    m_CurrentJitterUnscaled = XMFLOAT2( jitterX, jitterY );

    m_CurrentJitter = XMFLOAT2(
        jitterX / static_cast<float>(m_Width),
        jitterY / static_cast<float>(m_Height)
    );
    
    // Apply the new jitter to the projection matrix for scene rendering
    // The factor of 2 converts from UV space to clip space (-1 to 1)
    projF._13 = m_CurrentJitter.x * 2.0f;
    projF._23 = -m_CurrentJitter.y * 2.0f;

    Engine::GAPI->GetRendererState().TransformState.TransformProj = projF;
}

void D3D11PFX_TAA::RenderVelocityBuffer(
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& depthSRV) {
    
    if ( m_recreate ) {
        if ( !Init() ) {
            return;
        }
        m_recreate = false;
    }

    // depthSRV is using reverse-z where far plane is 0.0 and near plane is 1.0
    // where most "near" items are in the range of 0.05 to 0.2, and all far items are lower.

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    
    // Set default states
    engine->SetDefaultStates();
    engine->UpdateRenderStates();
    
    // Save old render targets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf());
    
    // Build velocity constant buffer
    VelocityBufferConstantBuffer vcb;
    
    // Get the UNJITTERED inverse view-projection
    // m_UnjitteredViewProj is stored in column-major
    XMMATRIX unjitteredViewProj = XMLoadFloat4x4( &m_UnjitteredViewProj );
    XMMATRIX invViewProj = XMMatrixInverse( nullptr, unjitteredViewProj );
    
    XMStoreFloat4x4(&vcb.InvViewProj, invViewProj );
    
    // PrevViewProj is also stored Column-major
    XMMATRIX prevViewProj = XMLoadFloat4x4( &m_PrevViewProj );
    XMStoreFloat4x4( &vcb.PrevViewProj, prevViewProj );
    
    vcb.JitterOffset = m_CurrentJitter;
    vcb.PrevJitterOffset = m_PreviousJitter;
    vcb.Resolution = XMFLOAT2(static_cast<float>(m_Width), static_cast<float>(m_Height));
    
    m_VelocityConstantBuffer->UpdateBuffer(&vcb);
    m_VelocityConstantBuffer->BindToPixelShader(0);
    
    // Set velocity buffer as render target
    context->OMSetRenderTargets(1, m_VelocityBuffer->GetRenderTargetView().GetAddressOf(), nullptr);
    
    // Clear velocity buffer to zero (no motion)
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->ClearRenderTargetView(m_VelocityBuffer->GetRenderTargetView().Get(), clearColor);
    
    // Bind shaders
    engine->GetShaderManager().GetVShader( VShaderID::VS_PFX )->Apply();
    auto velocityPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Velocity );
    if (!velocityPS) {
        // Shader not found, skip velocity buffer generation
        context->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
        return;
    }
    velocityPS->Apply();
    
    // Bind depth texture
    ID3D11ShaderResourceView* srvs[1] = { depthSRV.Get() };
    context->PSSetShaderResources(0, 1, srvs);
    context->PSSetSamplers( 0, 1, m_samplerLinear.GetAddressOf() );
    context->PSSetSamplers( 1, 1, m_samplerPoint.GetAddressOf() );
    
    // Draw fullscreen quad
    FxRenderer->DrawFullScreenQuad();
    
    // Cleanup
    ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
    context->PSSetShaderResources(0, 1, nullSRVs);
    
    // Restore render targets
    context->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> D3D11PFX_TAA::GetVelocityBufferSRV() const {
    if (m_VelocityBuffer) {
        return m_VelocityBuffer->GetShaderResView();
    }
    return nullptr;
}

void D3D11PFX_TAA::RenderPostFX(
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& currentFrameSRV,
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& depthSRV,
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& velocitySRV) {
    
    if (m_recreate) {
        if (!Init()) {
            return;
        }
        m_recreate = false;
    }
    
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    
    // Set default states
    engine->SetDefaultStates();
    engine->UpdateRenderStates();
    
    // Save old render targets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf());

    // Get temp buffer to render into
    auto tempBuffer = FxRenderer->GetTempBuffer();

    // update the temp buffer with the latest backbuffer data
    FxRenderer->CopyTextureToRTV( currentFrameSRV, tempBuffer->GetRenderTargetView(), engine->GetResolution() );
    
    // Build constant buffer
    TAAConstantBuffer cb;
    
    // Get the UNJITTERED inverse view-projection
    XMMATRIX unjitteredViewProj = XMLoadFloat4x4( &m_UnjitteredViewProj );
    XMMATRIX invViewProj = XMMatrixInverse( nullptr, unjitteredViewProj );
    
    XMStoreFloat4x4( &cb.InvViewProj, invViewProj );

    // PrevViewProj transpose for HLSL
    XMMATRIX prevViewProj = XMLoadFloat4x4( &m_PrevViewProj );
    XMStoreFloat4x4( &cb.PrevViewProj, prevViewProj );

    cb.JitterOffset = m_CurrentJitter;
    cb.Resolution = XMFLOAT2(static_cast<float>(m_Width), static_cast<float>(m_Height));

    cb.BlendFactor = m_FirstFrame ? 1.0f : 0.08f;


    cb.MotionScale = 1.0f;
    
    m_TAAConstantBuffer->UpdateBuffer(&cb);
    m_TAAConstantBuffer->BindToPixelShader(0);
    
    m_PrevViewProj = m_UnjitteredViewProj;

    // Store current camera position for next frame
    m_PrevCameraPosition = Engine::GAPI->GetCameraPosition();

    // Set render target
    context->OMSetRenderTargets( 1, tempBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    // Bind shaders
    engine->GetShaderManager().GetVShader( VShaderID::VS_PFX )->Apply();
    auto taaPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_TAA );
    if (!taaPS) {
        FxRenderer->CopyTextureToRTV( currentFrameSRV, oldRTV );
        context->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
        return;
    }
    taaPS->Apply();

    // Bind textures:
    // t0: Current frame
    // t1: History buffer
    // t2: Depth buffer
    // t3: Velocity buffer
    ID3D11ShaderResourceView* srvs[4] = {
        currentFrameSRV.Get(),
        m_FirstFrame ? currentFrameSRV.Get() : m_HistoryBuffer->GetShaderResView().Get(),
        depthSRV.Get(),
        velocitySRV.Get()
    };
    context->PSSetShaderResources(0, 4, srvs);
    
    // Bind samplers
    context->PSSetSamplers( 0, 1, m_samplerLinear.GetAddressOf() );
    context->PSSetSamplers( 1, 1, m_samplerPoint.GetAddressOf() );

    // Draw fullscreen quad
    FxRenderer->DrawFullScreenQuad();
    
    // Copy output to history buffer for next frame
    context->CopyResource(m_HistoryBuffer->GetTexture().Get(), 
                          tempBuffer->GetTexture().Get());

    // Cleanup shader resources
    ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
    context->PSSetShaderResources( 0, 4, nullSRVs );

    // Copy result back to the original render target
    FxRenderer->CopyTextureToRTV( tempBuffer->GetShaderResView(), oldRTV );

    // Restore original render targets
    context->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
    
    m_FirstFrame = false;
}
