#include "pch.h"
#include "D3D11PFX_TAA.h"

// The composite FSR3 effect (upscaler + frame interpolation) is not built by this fork;
// only the upscaler is. Its jitter helpers are the same Halton sequence the composite
// header forwarded to, just under their real names.
#include <FidelityFX/upscalers/fsr3/include/ffx_fsr3upscaler.h>

#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "D3D11ShaderManager.h"
#include "D3D11CShader.h"
#include "RenderToTextureBuffer.h"
#include "GothicAPI.h"

namespace {
    // Bit layout of CS_PFX_TAAResolve.hlsl's DebugFlags. Everything except MarkNoHistory is on for the "best
    // quality" configuration Intel documents; runtime bits (not #defines) so a suspected artifact can be
    // bisected to one feature without a rebuild — same reasoning as the D3D12 backend's D3D12Taa.cpp.
    constexpr uint32_t kTaaMarkNoHistory         = 0x01;
    constexpr uint32_t kTaaDepthThreshold        = 0x02;
    constexpr uint32_t kTaaBicubicFilter         = 0x04;
    constexpr uint32_t kTaaVarianceClipping      = 0x08;
    constexpr uint32_t kTaaYCoCg                 = 0x10;
    constexpr uint32_t kTaaNeighbourhoodSampling = 0x20;
    constexpr uint32_t kTaaLongestVelocityVector = 0x40;

    // Relative view-Z tolerance for the disocclusion test. Same value and reasoning as the D3D12 backend.
    constexpr float kTaaDepthTolerance = 0.05f;
}

D3D11PFX_TAA::D3D11PFX_TAA(D3D11PfxRenderer* rnd)
    : D3D11PFX_Effect(rnd)
    , m_JitterIndex(0)
    , m_Width(0)
    , m_Height(0)
    , m_FirstFrame(true) {

    m_CurrentJitter = XMFLOAT2(0, 0);
    m_PreviousJitter = XMFLOAT2(0, 0);
    m_PreviousJitterUnscaled = XMFLOAT2(0, 0);
    m_PrevCameraPosition = XMFLOAT3(0, 0, 0);
    XMStoreFloat4x4( &m_PrevViewProj, XMMatrixIdentity() );
    XMStoreFloat4x4( &m_UnjitteredViewProj, XMMatrixIdentity() );
}

bool D3D11PFX_TAA::Init() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    // Linear sampler for color/history sampling
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

    // History ping-pong: same format as the HDR scene colour, and UAV-capable so the compute resolve can write
    // it directly (no extra copy through an intermediate RTV).
    DXGI_FORMAT format = engine->GetBackBufferFormat();
    for ( UINT i = 0; i < 2; ++i ) {
        m_HistoryBuffer[i] = std::make_unique<RenderToTextureBuffer>(
            engine->GetDevice().Get(), m_Width, m_Height, format,
            nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS );
        SetDebugName( m_HistoryBuffer[i]->GetTexture().Get(), i == 0 ? "TAA_History0" : "TAA_History1" );
    }
    m_HistoryIndex = 0;
    m_HistoryValid = false;

    // Private previous-depth snapshot: same typeless format as the engine's depth buffer, SRV-only (read in the
    // resolve, written via CopyResource — see the end of RenderPostFX).
    m_PrevDepthBuffer = std::make_unique<RenderToTextureBuffer>(
        engine->GetDevice().Get(), m_Width, m_Height, DXGI_FORMAT_R32_TYPELESS,
        nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT, 1, 1,
        D3D11_BIND_SHADER_RESOURCE );
    SetDebugName( m_PrevDepthBuffer->GetTexture().Get(), "TAA_PrevDepth" );
    m_PrevDepthValid = false;

    // Velocity buffer (RG16F) for the camera-reconstructed fallback path (RenderVelocityBuffer).
    m_VelocityBuffer = std::make_unique<RenderToTextureBuffer>(
        engine->GetDevice().Get(), m_Width, m_Height, DXGI_FORMAT_R16G16_FLOAT);

    SetDebugName( m_VelocityBuffer->GetTexture().Get(), "TAA_VelocityBuffer" );
    SetDebugName( m_VelocityBuffer->GetRenderTargetView().Get(), "TAA_VelocityBuffer_RTV" );
    SetDebugName( m_VelocityBuffer->GetShaderResView().Get(), "TAA_VelocityBuffer_SRV" );

    // Reset state on resize
    m_FirstFrame = true;
    m_JitterIndex = 0;
    m_FrameNumber = 0;
    m_CurrentJitter = XMFLOAT2(0, 0);
    m_PreviousJitter = XMFLOAT2(0, 0);
    m_CurrentJitterUnscaled = XMFLOAT2(0, 0);
    m_PreviousJitterUnscaled = XMFLOAT2(0, 0);

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
    m_CurrentJitterUnscaled = XMFLOAT2( 0, 0 );
    m_PreviousJitterUnscaled = XMFLOAT2( 0, 0 );
    m_JitterIndex = 0;
    m_FirstFrame = true;
    // The accumulated history was (or will be) built with jitter; do not blend against it once TAA comes back.
    m_HistoryValid = false;
    m_PrevDepthValid = false;
}

void D3D11PFX_TAA::ReleaseResources() {
    if ( m_recreate ) {
        return;
    }
    m_recreate = true;

    m_HistoryBuffer[0].reset();
    m_HistoryBuffer[1].reset();
    m_PrevDepthBuffer.reset();
    m_VelocityBuffer.reset();
    m_HistoryValid = false;
    m_PrevDepthValid = false;
    m_samplerLinear.Reset();
    m_samplerPoint.Reset();
}

void D3D11PFX_TAA::AdvanceJitter() {
    // Store the previous jitter for removal. Both forms: the UV-scaled one the velocity CB consumes, and the
    // pixel one the resolve's previous-depth lookup needs (the previous depth buffer was rasterized with it).
    m_PreviousJitter = m_CurrentJitter;
    m_PreviousJitterUnscaled = m_CurrentJitterUnscaled;

    // Advance to next jitter sample
    auto renderWidth = Engine::GraphicsEngine->GetResolution().x;
    auto displayWidth = Engine::GraphicsEngine->GetBackbufferResolution().x;
    const int32_t phaseCount = ffxFsr3UpscalerGetJitterPhaseCount( renderWidth, displayWidth );

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
        ffxFsr3UpscalerGetJitterOffset( &jitterX, &jitterY, m_JitterIndex, phaseCount );
    }

    ++m_FrameNumber;

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

    // Forward matrix + eye, so the shader's sky branch can reproject the view ray (SkyMotionVectors.h).
    vcb.UnjitteredViewProj = m_UnjitteredViewProj;
    const float3 eye = Engine::GAPI->GetCameraPosition();
    vcb.CameraPosition = XMFLOAT4( eye.x, eye.y, eye.z, 0.0f );

    engine->BindDynamicCBToPixelShader( 0, engine->AllocateDynamicCB( &vcb, sizeof( vcb ) ) );

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

// The resolve: reads the scene colour + velocity + both depth buffers + last frame's history, writes this
// frame's history via the compute UAV, then copies the result back over the scene colour (renderTarget) so the
// pass stays transparent to bloom/HDR/tonemap downstream. Direct counterpart of D3D12GraphicsEngine::RenderTAA.
void D3D11PFX_TAA::RenderPostFX(
    RenderToTextureBuffer& renderTarget,
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

    engine->SetDefaultStates();
    engine->UpdateRenderStates();

    // Save old render targets; compute writes need them unbound (a render target can't also be a UAV source).
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf());
    context->OMSetRenderTargets( 0, nullptr, nullptr );

    const UINT writeIdx = m_HistoryIndex;
    const UINT readIdx = 1 - m_HistoryIndex;

    // Every quality feature on — Intel's documented "best quality" set. MarkNoHistory stays off; it tints
    // no-history pixels green, a debugging aid rather than a rendering mode.
    uint32_t debugFlags = kTaaDepthThreshold | kTaaBicubicFilter | kTaaVarianceClipping
                        | kTaaYCoCg | kTaaNeighbourhoodSampling | kTaaLongestVelocityVector;
    // No previous-depth snapshot yet (first frame of a world / after a resize): comparing against a cleared
    // (far-plane) snapshot would reject every pixel, so drop the depth test rather than let it hard-fail.
    if ( !m_PrevDepthValid ) debugFlags &= ~kTaaDepthThreshold;

    TAAResolveConstantBuffer cb = {};

    // Get the UNJITTERED inverse view-projection
    XMMATRIX unjitteredViewProj = XMLoadFloat4x4( &m_UnjitteredViewProj );
    XMMATRIX invViewProj = XMMatrixInverse( nullptr, unjitteredViewProj );
    XMStoreFloat4x4( &cb.InvUnjitteredViewProj, invViewProj );

    XMMATRIX prevViewProj = XMLoadFloat4x4( &m_PrevViewProj );
    XMStoreFloat4x4( &cb.PrevViewProj, prevViewProj );

    cb.Resolution = XMFLOAT4(
        static_cast<float>( m_Width ), static_cast<float>( m_Height ),
        1.0f / static_cast<float>( m_Width ), 1.0f / static_cast<float>( m_Height ) );
    cb.JitterTolerance = XMFLOAT4( m_CurrentJitterUnscaled.x, m_CurrentJitterUnscaled.y, kTaaDepthTolerance, 0.0f );
    cb.PrevJitter = XMFLOAT4( m_PreviousJitterUnscaled.x, m_PreviousJitterUnscaled.y, 0.0f, 0.0f );
    cb.FrameNumber = m_FrameNumber;
    cb.DebugFlags = debugFlags;
    // HistoryValid hard-forces the no-history branch in the shader when there is nothing accumulated yet — see
    // CS_PFX_TAAResolve.hlsl's header comment for why merely pointing the SRV somewhere harmless isn't enough.
    cb.HistoryValid = ( m_HistoryValid && !m_FirstFrame ) ? 1u : 0u;

    m_PrevViewProj = m_UnjitteredViewProj;
    m_PrevCameraPosition = Engine::GAPI->GetCameraPosition();

    auto cs = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_TAAResolve );
    cs->Apply();
    cs->UpdateBuffer( "TaaCB", &cb, sizeof( cb ) );

    ID3D11ShaderResourceView* readHistorySRV = ( m_HistoryValid && !m_FirstFrame )
        ? m_HistoryBuffer[readIdx]->GetShaderResView().Get()
        : renderTarget.GetShaderResView().Get();

    ID3D11ShaderResourceView* srvs[5] = {
        renderTarget.GetShaderResView().Get(),   // t0: scene colour (this frame)
        readHistorySRV,                          // t1: history (previous frame, or a harmless substitute)
        velocitySRV.Get(),                       // t2: RG16F motion vectors
        depthSRV.Get(),                          // t3: depth (this frame)
        m_PrevDepthValid ? m_PrevDepthBuffer->GetShaderResView().Get() : depthSRV.Get(), // t4: depth (previous)
    };
    context->CSSetShaderResources( 0, 5, srvs );

    ID3D11SamplerState* samplers[2] = { m_samplerLinear.Get(), m_samplerPoint.Get() };
    context->CSSetSamplers( 0, 2, samplers );

    context->CSSetUnorderedAccessViews( 0, 1, m_HistoryBuffer[writeIdx]->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( ( m_Width + 7 ) / 8, ( m_Height + 7 ) / 8, 1 );

    // Unbind: the write target becomes a CopyResource source next, and the read SRVs may be rebound elsewhere.
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    ID3D11ShaderResourceView* nullSRVs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    context->CSSetShaderResources( 0, 5, nullSRVs );
    context->CSSetShader( nullptr, nullptr, 0 );

    // Hand the result back to the scene colour for bloom/tonemap. Both are the backbuffer format, so this is a
    // straight CopyResource; nothing downstream reads scene-colour alpha, which now carries the TAA confidence
    // weight instead.
    context->CopyResource( renderTarget.GetTexture().Get(), m_HistoryBuffer[writeIdx]->GetTexture().Get() );

    // Snapshot this frame's depth for the NEXT frame's disocclusion test. Must be our own copy rather than
    // DepthStencilBufferCopy: that buffer is refilled with THIS frame's depth later in the frame (for
    // upscaling), so by the time next frame's TAA runs it no longer holds what TAA needs.
    Microsoft::WRL::ComPtr<ID3D11Resource> depthResource;
    depthSRV->GetResource( depthResource.GetAddressOf() );
    context->CopyResource( m_PrevDepthBuffer->GetTexture().Get(), depthResource.Get() );
    m_PrevDepthValid = true;

    context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

    m_HistoryIndex = readIdx;   // ping-pong
    m_HistoryValid = true;
    m_FirstFrame = false;
}
