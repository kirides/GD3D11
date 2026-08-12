// D3D12 debug/editor line renderer — the D3D12LineRenderer cache/flush logic plus the engine-side draw
// (D3D12GraphicsEngine::CreateLineVertexBuffers / DrawLines). Port of D3D11LineRenderer.cpp; the pipeline
// state itself lives in D3D12PipelineState::CreateLines (Shaders/D3D12/Lines.hlsl).
#include "../pch.h"
#include "D3D12LineRenderer.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../Logger.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    // 4 MB per frame-in-flight = 65536 lines. Debug overlays (wireframe meshes, editor brushes, BSP
    // visualizations) are the heaviest consumers; anything past this is dropped and logged, never grown.
    constexpr UINT kLineVertexBufferBytes = 4 * 1024 * 1024;

    // The active backend, or nullptr if it isn't ours. Gothic can swap engines (D3D12 init failure falls
    // back to D3D11), and this object is reachable through BaseGraphicsEngine::GetLineRenderer either way.
    D3D12GraphicsEngine* ActiveD3D12Engine() {
        if ( !Engine::GraphicsEngine || Engine::GraphicsEngine->GetBackendAPI() != EGraphicsEngineBackend::D3D12 )
            return nullptr;
        return static_cast<D3D12GraphicsEngine*>( Engine::GraphicsEngine );
    }
}


XRESULT D3D12LineRenderer::AddLine( const LineVertex& v1, const LineVertex& v2 ) {
    if ( LineCache.size() + 2 > kMaxCachedVertices ) {
        if ( !CacheOverflowLogged ) {
            LogWarn() << "D3D12: debug-line cache full (" << kMaxCachedVertices
                << " vertices). Further lines are dropped until it is flushed.";
            CacheOverflowLogged = true;
        }
        return XR_FAILED;
    }

    LineCache.push_back( v1 );
    LineCache.push_back( v2 );
    return XR_SUCCESS;
}


XRESULT D3D12LineRenderer::AddLineScreenSpace( const LineVertex& v1, const LineVertex& v2 ) {
    if ( ScreenSpaceLineCache.size() + 2 > kMaxCachedVertices ) {
        if ( !CacheOverflowLogged ) {
            LogWarn() << "D3D12: screen-space debug-line cache full (" << kMaxCachedVertices
                << " vertices). Further lines are dropped until it is flushed.";
            CacheOverflowLogged = true;
        }
        return XR_FAILED;
    }

    ScreenSpaceLineCache.push_back( v1 );
    ScreenSpaceLineCache.push_back( v2 );
    return XR_SUCCESS;
}


XRESULT D3D12LineRenderer::Flush() {
    if ( !LineCache.empty() ) {
        if ( D3D12GraphicsEngine* engine = ActiveD3D12Engine() )
            engine->DrawLines( LineCache, false );
        LineCache.clear();
    }
    CacheOverflowLogged = false;
    return XR_SUCCESS;
}


XRESULT D3D12LineRenderer::FlushScreenSpace() {
    if ( !ScreenSpaceLineCache.empty() ) {
        if ( D3D12GraphicsEngine* engine = ActiveD3D12Engine() )
            engine->DrawLines( ScreenSpaceLineCache, true );
        ScreenSpaceLineCache.clear();
    }
    CacheOverflowLogged = false;
    return XR_SUCCESS;
}


XRESULT D3D12LineRenderer::ClearCache() {
    // D3D11LineRenderer::ClearCache only clears the world-space list; clearing both here is strictly safer
    // (this is the only path that can drop screen-space lines without a frame having drawn them).
    LineCache.clear();
    ScreenSpaceLineCache.clear();
    CacheOverflowLogged = false;
    return XR_SUCCESS;
}


bool D3D12GraphicsEngine::CreateLineVertexBuffers() {
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = DefaultUploadHeapType;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kLineVertexBufferBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_Allocator->CreateResource( &allocDesc, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_LineVertexBufferAlloc[i].ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_LineVertexBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_LineVertexBuffer[i]->SetName( i == 0 ? L"LineVertexRing0" : L"LineVertexRing1" );
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( m_LineVertexBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_LineVertexBufferPtr[i] ) ) ) )
            return false;
    }
    m_LineVertexBufferCapacity = kLineVertexBufferBytes;
    return true;
}


void D3D12GraphicsEngine::DrawLines( const std::vector<LineVertex>& lines, bool screenSpace ) {
    if ( !m_SwapChainReady || !m_FrameOpen || lines.empty() ) return;
    if ( !m_Pipelines.Lines.RootSig || !m_LineVertexBuffer[m_FrameIndex] ) return;

    ID3D12PipelineState* pso = screenSpace ? m_Pipelines.Lines.ScreenPSO.Get() : m_Pipelines.Lines.WorldPSO.Get();
    if ( !pso ) return;

    DX_ZONE( m_CmdList.Get(), "Draw debug lines" );

    const UINT frame = m_FrameIndex;
    const UINT bytes = static_cast<UINT>( lines.size() * sizeof( LineVertex ) );
    if ( m_LineVertexBufferOffset + bytes > m_LineVertexBufferCapacity ) {
        if ( !m_LineOverflowLogged ) {
            LogWarn() << "D3D12: debug-line vertex ring overflow (" << m_LineVertexBufferCapacity
                << " bytes/frame). Some debug lines dropped this frame.";
            m_LineOverflowLogged = true;
        }
        return;
    }

    memcpy( m_LineVertexBufferPtr[frame] + m_LineVertexBufferOffset, lines.data(), bytes );
    const D3D12_GPU_VIRTUAL_ADDRESS gpuVA = m_LineVertexBuffer[frame]->GetGPUVirtualAddress() + m_LineVertexBufferOffset;
    m_LineVertexBufferOffset += bytes;

    // The HDR scene colour, NOT the display target: drawn inside the scene so the tonemap resolve's
    // up/downscale carries them (see the call site in OnStartWorldRendering). BindSceneColorTarget also binds
    // the equally-sized scene depth, which the world-space lines test against. The screen-space list is a 2D
    // overlay whose PSO is DSVFormat UNKNOWN, so it takes the colour-only binding.
    BindSceneColorTarget();
    if ( screenSpace ) {
        m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, nullptr );
    }

    m_CmdList->SetPipelineState( pso );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.Lines.RootSig.Get() );

    // b0 ViewProj — reversed-Z, identical derivation to the world/VOB passes. D3D11's line flush resets the
    // world transform to identity and re-sets the view transform before drawing; do the same so the lines
    // land in world space regardless of what the last 3D pass left in GothicAPI's transform state.
    Engine::GAPI->SetViewTransformXM( Engine::GAPI->GetViewMatrixXM() );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    // By VALUE, with the TAA jitter stripped. AdvanceJitter wrote the sub-pixel offset straight into
    // TransformProj._13/_23 and nothing clears it for the rest of the frame. RenderTAA runs one pass later but
    // reprojects through the velocity buffer, and nothing writes velocity for a debug line — so the jitter
    // would never be resolved away, and a one-pixel line wobbling along the FSR3 phase sequence reads as
    // full-amplitude flicker (very visible with the ImGui editor's gizmos).
    XMFLOAT4X4 projM = Engine::GAPI->GetProjectionMatrix();
    projM._13 = 0.0f;
    projM._23 = 0.0f;
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );

    // Full-screen RENDER-resolution viewport, to match the scene-colour target. Gothic's tracked D3D7
    // viewport is irrelevant here and can be degenerate (same hazard the 2D/UI path guards against).
    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };

    // b1 viewport pos/size for the screen-space (xyzrhw) transform — mirrors D3D11's BindViewportInformation
    // (pixels divided by GothicUIScale), which the screen-space line VS consumes exactly like the UI path.
    // NOT the rasterizer viewport above: this is the coordinate space the hooked DrawLine/DrawLineZ emit
    // vertices in — Gothic's virtual screen, which OnBeginFrame sets to the NATIVE size. The VS turns those
    // pixels into NDC, which the render-res viewport then maps onto the smaller target.
    const float uiScale = std::max<float>( 0.001f, Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale );
    const float vpConsts[8] = {
        0.0f, 0.0f,
        static_cast<float>( m_BackbufferResolution.x ) / uiScale,
        static_cast<float>( m_BackbufferResolution.y ) / uiScale,
        0.0f, 0.0f, 0.0f, 0.0f };
    m_CmdList->SetGraphicsRoot32BitConstants( 1, 8, vpConsts, 0 );

    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_LINELIST );

    const D3D12_VERTEX_BUFFER_VIEW vbv = { gpuVA, bytes, sizeof( LineVertex ) };
    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    m_CmdList->DrawInstanced( static_cast<UINT>( lines.size() ), 1, 0, 0 );

    // No binding to restore: every pass after this one (TAA, DoF, bloom, the resolve) sets its own targets.
}
