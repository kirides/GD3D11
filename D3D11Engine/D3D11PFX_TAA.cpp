#include "pch.h"
#include "D3D11PFX_TAA.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "D3D11ShaderManager.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ConstantBuffer.h"
#include "GothicAPI.h"

// Halton sequence generator for jitter - low-discrepancy sequence
// Provides better sub-pixel coverage than random sampling
static float Halton(int index, int base) {
    float result = 0.0f;
    float f = 1.0f / base;
    int i = index;
    while (i > 0) {
        result += f * (i % base);
        i = i / base;
        f = f / base;
    }
    return result;
}

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
    
    // Generate Halton sequence with 16 samples for better temporal coverage
    // Using bases 2 and 3 provides good low-discrepancy distribution
    const int JITTER_SAMPLES = 16;
    m_JitterSequence.resize(JITTER_SAMPLES);
    for (int i = 0; i < JITTER_SAMPLES; i++) {
        // Center the Halton sequence around 0 (-0.5 to 0.5 range)
        // This ensures jitter is distributed evenly around pixel center
        m_JitterSequence[i] = XMFLOAT2(
            Halton(i + 1, 2) - 0.5f,
            Halton(i + 1, 3) - 0.5f
        );
    }
}

D3D11PFX_TAA::~D3D11PFX_TAA() {}

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

    return true;
}

void D3D11PFX_TAA::OnResize(const INT2& size) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    
    m_Width = size.x;
    m_Height = size.y;
    
    // Create history buffer (same format as back buffer for color)
    DXGI_FORMAT format = engine->GetBackBufferFormat();
    m_HistoryBuffer = std::make_unique<RenderToTextureBuffer>(
        engine->GetDevice().Get(), size.x, size.y, format);

    SetDebugName( m_HistoryBuffer->GetTexture().Get(), "TAA_HistoryBuffer" );
    SetDebugName( m_HistoryBuffer->GetRenderTargetView().Get(), "TAA_HistoryBuffer_RTV" );
    SetDebugName( m_HistoryBuffer->GetShaderResView().Get(), "TAA_HistoryBuffer_SRV" );
    
    // Create velocity buffer (RG16F for 2D motion vectors)
    // Using R16G16_FLOAT for high precision motion vectors
    m_VelocityBuffer = std::make_unique<RenderToTextureBuffer>(
        engine->GetDevice().Get(), size.x, size.y, DXGI_FORMAT_R16G16_FLOAT);

    SetDebugName( m_VelocityBuffer->GetTexture().Get(), "TAA_VelocityBuffer" );
    SetDebugName( m_VelocityBuffer->GetRenderTargetView().Get(), "TAA_VelocityBuffer_RTV" );
    SetDebugName( m_VelocityBuffer->GetShaderResView().Get(), "TAA_VelocityBuffer_SRV" );
    
    // Reset state on resize
    m_FirstFrame = true;
    m_JitterIndex = 0;
    m_CurrentJitter = XMFLOAT2(0, 0);
    m_PreviousJitter = XMFLOAT2(0, 0);
}

void D3D11PFX_TAA::OnDisabled() {
    // Remove any residual jitter from projection matrix when TAA is disabled
    XMFLOAT4X4& projF = Engine::GAPI->GetProjectionMatrix();
    projF._31 -= m_CurrentJitter.x * 2.0f;
    projF._32 -= m_CurrentJitter.y * 2.0f;

    m_CurrentJitter = XMFLOAT2( 0, 0 );
    m_PreviousJitter = XMFLOAT2( 0, 0 );
    m_FirstFrame = true;
}

void D3D11PFX_TAA::AdvanceJitter() {
    // Store the previous jitter for removal
    m_PreviousJitter = m_CurrentJitter;

    // Advance to next jitter sample
    m_JitterIndex = (m_JitterIndex + 1) % m_JitterSequence.size();

    // Get the current projection matrix from the renderer state
    XMFLOAT4X4& projF = Engine::GAPI->GetProjectionMatrix();
    
    // Remove the previous jitter from the projection matrix to get the unjittered version
    projF._31 -= m_PreviousJitter.x * 2.0f;
    projF._32 -= m_PreviousJitter.y * 2.0f;
    
    // Store the unjittered view-projection for use in the velocity/resolve pass
    // Note: Gothic stores matrices in row-major order, so we multiply view * proj
    XMMATRIX view = XMLoadFloat4x4( &Engine::GAPI->GetRendererState().TransformState.TransformView );
    XMMATRIX proj = XMLoadFloat4x4( &projF );
    XMMATRIX viewProj = XMMatrixMultiply( view, proj );
    // Store WITHOUT transpose - we'll handle the conversion in the shader setup
    XMStoreFloat4x4( &m_UnjitteredViewProj, viewProj );
    
    // Calculate new jitter in UV space (pixels / resolution)
    // The jitter sequence is in pixel space (-0.5 to 0.5), convert to UV space
    m_CurrentJitter = XMFLOAT2(
        m_JitterSequence[m_JitterIndex].x / static_cast<float>(m_Width),
        m_JitterSequence[m_JitterIndex].y / static_cast<float>(m_Height)
    );
    
    // Apply the new jitter to the projection matrix for scene rendering
    // The factor of 2 converts from UV space to clip space (-1 to 1)
    projF._31 += m_CurrentJitter.x * 2.0f;
    projF._32 += m_CurrentJitter.y * 2.0f;
}

void D3D11PFX_TAA::RenderVelocityBuffer(
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& depthSRV) {
    
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
    // m_UnjitteredViewProj is stored in row-major (non-transposed)
    XMMATRIX unjitteredViewProj = XMLoadFloat4x4( &m_UnjitteredViewProj );
    XMMATRIX invViewProj = XMMatrixInverse( nullptr, unjitteredViewProj );
    
    // For HLSL: transpose for column-major shader consumption
    XMStoreFloat4x4(&vcb.InvViewProj, XMMatrixTranspose(invViewProj));
    
    // PrevViewProj is also stored non-transposed, transpose it for HLSL
    XMMATRIX prevViewProj = XMLoadFloat4x4( &m_PrevViewProj );
    XMStoreFloat4x4(&vcb.PrevViewProj, XMMatrixTranspose(prevViewProj));
    
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
    engine->GetShaderManager().GetVShader("VS_PFX")->Apply();
    auto velocityPS = engine->GetShaderManager().GetPShader("PS_PFX_Velocity");
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
    
    XMStoreFloat4x4(&cb.InvViewProj, XMMatrixTranspose(invViewProj));
    
    // PrevViewProj transpose for HLSL
    XMMATRIX prevViewProj = XMLoadFloat4x4( &m_PrevViewProj );
    XMStoreFloat4x4(&cb.PrevViewProj, XMMatrixTranspose(prevViewProj));
    
    cb.JitterOffset = m_CurrentJitter;
    cb.Resolution = XMFLOAT2(static_cast<float>(m_Width), static_cast<float>(m_Height));
    
    // Blend factor: higher = more current frame (sharper but more flickering)
    // Using 0.08 as base gives good balance between stability and sharpness
    // The shader will adaptively increase this at edges and for motion
    cb.BlendFactor = m_FirstFrame ? 1.0f : 0.08f;
    cb.MotionScale = 1.0f;
    
    m_TAAConstantBuffer->UpdateBuffer(&cb);
    m_TAAConstantBuffer->BindToPixelShader(0);
    
    // Store current unjittered viewproj for next frame (already non-transposed)
    m_PrevViewProj = m_UnjitteredViewProj;
    
    // Store current camera position for next frame
    m_PrevCameraPosition = Engine::GAPI->GetCameraPosition();

    // Set render target
    context->OMSetRenderTargets( 1, tempBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    // Bind shaders
    engine->GetShaderManager().GetVShader("VS_PFX")->Apply();
    auto taaPS = engine->GetShaderManager().GetPShader("PS_PFX_TAA");
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
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> velSRV = velocitySRV;
    if (!velSRV && m_VelocityBuffer) {
        velSRV = m_VelocityBuffer->GetShaderResView();
    }
    
    ID3D11ShaderResourceView* srvs[4] = {
        currentFrameSRV.Get(),
        m_FirstFrame ? currentFrameSRV.Get() : m_HistoryBuffer->GetShaderResView().Get(),
        depthSRV.Get(),
        velSRV.Get()
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
