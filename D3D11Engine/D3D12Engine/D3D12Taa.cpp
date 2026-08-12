// D3D12GraphicsEngine — temporal anti-aliasing.
//
// The resolve is a port of Intel's Graphics Optimized TAA (MIT licensed; see Shaders/D3D12/TAAResolve.hlsl,
// which carries the Intel and Microsoft copyright headers and documents exactly where the port diverges from
// the original). This file is the host side: the history ping-pong, the sub-pixel jitter, and the dispatch.
//
// FRAME SLOT. RenderTAA runs on the LINEAR HDR scene colour, after RenderFogAndGodRays and before RenderBloom.
// That placement matters in both directions: fog and god rays are part of the scene and must be inside the
// temporal accumulation, while bloom, luminance adaptation and the tonemap must be outside it (bloom of an
// aliased frame flickers, and adapting exposure to a pre-TAA image chases the noise TAA exists to remove).
//
// TRANSPARENCY TO THE REST OF THE CHAIN. The resolve does all of its weighting and variance-clipping maths in
// Reinhard-tonemapped space — that is the trick that stops one fireball pixel from blowing the clipping box
// open — but stores and outputs LINEAR HDR. So nothing downstream needs to know TAA ran; the pass reads the
// scene colour and writes the scene colour back.
//
// JITTER. Uses the FSR3 phase sequence (ffxFsr3GetJitterPhaseCount/Offset), which is already linked because
// D3D11PFX_TAA::AdvanceJitter uses it. Sharing the sequence is deliberate: the planned FSR3 upscaler expects
// exactly this jitter, so it becomes a drop-in rather than a competing jitter regime. The offset goes into
// TransformProj._13/_23, which every geometry pass picks up through GothicAPI::GetProjectionMatrix, while
// MotionCB::UnjitteredViewProj deliberately stays clean so motion vectors do not encode the jitter as motion.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../zCCamera.h"
#include <FidelityFX/host/ffx_fsr3.h>

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    // Bit layout of TAAResolve.hlsl's DebugFlags. Everything except MarkNoHistory is on for the "best quality"
    // configuration Intel documents; they are runtime bits rather than #defines so the maintainer can bisect a
    // suspected artifact to a single feature without a rebuild.
    constexpr uint32_t kTaaMarkNoHistory        = 0x01;
    constexpr uint32_t kTaaDepthThreshold       = 0x02;
    constexpr uint32_t kTaaBicubicFilter        = 0x04;
    constexpr uint32_t kTaaVarianceClipping     = 0x08;
    constexpr uint32_t kTaaYCoCg                = 0x10;
    constexpr uint32_t kTaaNeighbourhoodSampling = 0x20;
    constexpr uint32_t kTaaLongestVelocityVector = 0x40;

    // Relative view-Z tolerance for the disocclusion test. Generous enough to survive the sub-pixel reprojection
    // error on a steeply slanted surface, tight enough to reject a genuinely different one.
    constexpr float kTaaDepthTolerance = 0.05f;
}

/** (Re)creates the history pair and the private previous-depth snapshot. Called from the same resize path as
    the AO and motion resources. Heap slots persist; only resources and views are rebuilt. */
bool D3D12GraphicsEngine::CreateTaaResources( INT2 size ) {
    m_TaaResourcesReady = false;
    m_TaaHistoryValid = false;      // freshly allocated garbage until the first resolve writes it
    m_TaaPrevDepthValid = false;
    if ( size.x < 4 || size.y < 4 ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Allocator ) return false;
    // Init runs before the first CreateSwapChain; don't re-enable the feature if the pipeline failed there.
    if ( !m_Pipelines.Taa.PSO ) return false;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    // History: same format as the scene colour, which is what lets the result be handed back with a plain
    // CopyResource instead of a full-screen blit shader.
    for ( UINT i = 0; i < 2; ++i ) {
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = static_cast<UINT64>( size.x );
        dd.Height = static_cast<UINT>( size.y );
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = kSceneColorFormat;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, m_TaaHistoryAlloc[i].ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_TaaHistory[i].ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create a TAA history buffer (" << size.x << "x" << size.y << ").";
            return false;
        }
        m_TaaHistory[i]->SetName( i == 0 ? L"TaaHistory0" : L"TaaHistory1" );
    }

    // Private previous-depth snapshot. Same resource desc as m_DepthBuffer so CopyResource is unambiguously
    // legal — the same reasoning the water pass's depth copy follows.
    {
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = static_cast<UINT64>( size.x );
        dd.Height = static_cast<UINT>( size.y );
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = DXGI_FORMAT_R32_TYPELESS;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 0.0f;   // reversed-Z
        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, kPrevDepthReadState, &clear,
            m_TaaPrevDepthAlloc.ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_TaaPrevDepth.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the TAA previous-depth snapshot.";
            return false;
        }
        m_TaaPrevDepth->SetName( L"TaaPrevDepth" );
    }

    auto ensureSlot = [&]( UINT& slot ) -> bool {
        if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
        return slot != UINT_MAX;
        };
    for ( UINT i = 0; i < 2; ++i ) {
        if ( !ensureSlot( m_TaaHistorySrvSlot[i] ) || !ensureSlot( m_TaaHistoryUavSlot[i] ) ) return false;
    }
    if ( !ensureSlot( m_TaaPrevDepthSrvSlot ) ) return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = kSceneColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = kSceneColorFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    for ( UINT i = 0; i < 2; ++i ) {
        device->CreateShaderResourceView( m_TaaHistory[i].Get(), &srv, GetSrvCpuHandle( m_TaaHistorySrvSlot[i] ) );
        device->CreateUnorderedAccessView( m_TaaHistory[i].Get(), nullptr, &uav, GetSrvCpuHandle( m_TaaHistoryUavSlot[i] ) );
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = {};
    depthSrv.Format = DXGI_FORMAT_R32_FLOAT;   // typed view of the typeless snapshot
    depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView( m_TaaPrevDepth.Get(), &depthSrv, GetSrvCpuHandle( m_TaaPrevDepthSrvSlot ) );

    m_TaaHistoryIndex = 0;
    m_TaaResourcesReady = true;
    return true;
}

/** TAA is on and everything it needs exists. Also the gate for the jitter — the two must never disagree, or the
    image is jittered with nothing resolving it (visible shimmer) or resolved with no jitter (blurry, no
    supersampling gain). */
bool D3D12GraphicsEngine::IsTaaEnabled() const {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    return settings.AntiAliasingMode == GothicRendererSettings::AA_TAA
        && m_TaaResourcesReady
        && m_MotionResourcesReady          // no velocity buffer, no reprojection
        && m_Pipelines.Taa.PSO
        && m_Pipelines.Taa.RootSig
        && m_SceneColor
        && m_DepthBuffer && m_DepthSrvSlot != UINT_MAX;
}

/** Advances the sub-pixel jitter and writes it into the projection matrix for this frame's geometry passes.
    Called at the very top of OnStartWorldRendering, BEFORE UploadMotionConstants — which then has to read the
    projection back out with _13/_23 zeroed, so its UnjitteredViewProj stays clean.

    Direct counterpart of D3D11PFX_TAA::AdvanceJitter, including the clip-space conversion factor of 2. */
void D3D12GraphicsEngine::AdvanceJitter() {
    const bool wantJitter = IsTaaEnabled();
    if ( !wantJitter ) {
        // Leave the projection alone. Gothic resets it every frame anyway, but being explicit means a frame in
        // which TAA is switched off mid-session cannot inherit the previous frame's offset.
        if ( m_TaaJitterActive ) {
            m_TaaJitterPixels = XMFLOAT2( 0.0f, 0.0f );
            m_TaaPrevJitterPixels = XMFLOAT2( 0.0f, 0.0f );
            m_TaaJitterActive = false;
            m_TaaHistoryValid = false;   // the accumulated history was built with jitter; do not blend it later
        }
        return;
    }

    // The TAA resolve itself runs entirely at the RENDER resolution — the up/downscale happens later, in the
    // tonemap resolve. But the phase count is what decides how many sub-pixel samples the sequence needs to
    // cover, and at a render scale below 100% each render pixel covers more than one display pixel, so feed
    // FSR the real ratio (it scales the base 8-sample Halton run by (display/render)^2). At 100% this is
    // exactly the old renderWidth == displayWidth call.
    const int32_t renderWidth = m_Resolution.x;
    const int32_t displayWidth = m_BackbufferResolution.x;
    const int32_t phaseCount = ffxFsr3GetJitterPhaseCount( renderWidth, displayWidth );

    float jitterX = 0.0f;
    float jitterY = 0.0f;
    if ( phaseCount > 0 ) {
        m_TaaJitterIndex = ( m_TaaJitterIndex + 1 ) % phaseCount;
        ffxFsr3GetJitterOffset( &jitterX, &jitterY, m_TaaJitterIndex, phaseCount );
    } else {
        m_TaaJitterIndex = 0;
    }

    // FSR returns the offset in PIXELS, in [-0.5, 0.5]. The resolve's previous-depth lookup wants it in pixels;
    // the projection wants it in clip units, where a whole pixel is 2/resolution.
    m_TaaPrevJitterPixels = m_TaaJitterPixels;   // before the overwrite — this is what m_TaaPrevDepth was drawn with
    m_TaaJitterPixels = XMFLOAT2( jitterX, jitterY );
    m_TaaJitterActive = true;
    ++m_TaaFrameNumber;

    XMFLOAT4X4& proj = Engine::GAPI->GetProjectionMatrix();   // returns a reference INTO TransformState
    proj._13 = 2.0f * jitterX / static_cast<float>( m_Resolution.x );
    proj._23 = -2.0f * jitterY / static_cast<float>( m_Resolution.y );
}

/** The resolve. Reads the scene colour + velocity + both depth buffers + last frame's history, writes this
    frame's history, then copies it back over the scene colour. */
void D3D12GraphicsEngine::RenderTAA() {
    if ( !m_FrameOpen || !m_CmdList ) return;
    if ( !IsTaaEnabled() ) {
        // Switched off (or unavailable): drop the history so re-enabling it later starts clean rather than
        // blending against an arbitrarily old frame.
        m_TaaHistoryValid = false;
        return;
    }
    if ( m_VelocitySrvSlot == UINT_MAX || m_TaaPrevDepthSrvSlot == UINT_MAX ) return;
    // The resolve reconstructs the expected previous depth from these matrices rather than reading a third
    // velocity channel (the motion-vector buffer stays 2-channel RG16F for the other temporal consumers), so
    // without them the depth-confidence test cannot run at all.
    const D3D12_GPU_VIRTUAL_ADDRESS motionCb = GetMotionCbAddress();
    if ( !motionCb ) return;

    DX_ZONE( m_CmdList.Get(), "TAA resolve" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "TAA resolve" );

    const UINT writeIdx = m_TaaHistoryIndex;
    const UINT readIdx = 1 - m_TaaHistoryIndex;

    // Scene colour RENDER_TARGET -> shader read; write history to UAV, read history from a shader-read state.
    // Depth leaves DEPTH_WRITE, so unbind the DSV first (same dance RenderBloom / RenderFogAndGodRays do).
    m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );
    {
        D3D12_RESOURCE_BARRIER pre[4];
        UINT n = 0;
        if ( !m_SceneColorInPixelState ) {
            pre[n++] = TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        }
        pre[n++] = TransitionBarrier( m_TaaHistory[readIdx].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        pre[n++] = TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        if ( n ) m_CmdList->ResourceBarrier( n, pre );
        m_SceneColorInPixelState = true;
    }

    m_CmdList->SetPipelineState( m_Pipelines.Taa.PSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.Taa.RootSig.Get() );
    m_CmdList->SetComputeRootConstantBufferView( 0, motionCb );   // b0 MotionCB (shared with the prepass/fill)

    // Every quality feature on — Intel's documented "best quality" set. MarkNoHistory stays off; it tints
    // no-history pixels green, which is a debugging aid, not a rendering mode.
    uint32_t debugFlags = kTaaDepthThreshold | kTaaBicubicFilter | kTaaVarianceClipping
                        | kTaaYCoCg | kTaaNeighbourhoodSampling | kTaaLongestVelocityVector;
    // The depth test needs a previous depth buffer to test against; on the first frame of a world there is
    // none, and comparing against a cleared (far-plane) snapshot would reject every pixel.
    if ( !m_TaaPrevDepthValid ) debugFlags &= ~kTaaDepthThreshold;

    struct TaaConstants {
        float    Resolution[4];
        float    Jitter[2];
        uint32_t FrameNumber;
        uint32_t DebugFlags;
        uint32_t VelocityIndex;
        uint32_t ColourIndex;
        uint32_t HistoryIndex;
        uint32_t DepthIndex;
        uint32_t PrevDepthIndex;
        uint32_t OutIndex;
        float    DepthTolerance;
        uint32_t HistoryValid;
        float    PrevJitter[2];
    } cb = {};
    static_assert( sizeof( TaaConstants ) == 18 * sizeof( uint32_t ),
        "TaaConstants must match TAAResolve.hlsl's b1 TaaCB and the 18 root constants pushed below" );

    cb.Resolution[0] = static_cast<float>( m_Resolution.x );
    cb.Resolution[1] = static_cast<float>( m_Resolution.y );
    cb.Resolution[2] = 1.0f / static_cast<float>( m_Resolution.x );
    cb.Resolution[3] = 1.0f / static_cast<float>( m_Resolution.y );
    cb.Jitter[0] = m_TaaJitterPixels.x;
    cb.Jitter[1] = m_TaaJitterPixels.y;
    cb.FrameNumber = m_TaaFrameNumber;
    cb.DebugFlags = debugFlags;
    cb.VelocityIndex = m_VelocitySrvSlot;   // RG16F, 2-channel — see the shader's note 2
    cb.ColourIndex = m_SceneColorSrvSlot;
    // No history yet (first frame of a world, resize, TAA just re-enabled). HistoryValid below is what actually
    // makes the resolve ignore it — this only keeps the bindless slot pointing at a real, correctly-formatted
    // texture instead of at a freshly-allocated one whose contents are undefined.
    cb.HistoryIndex = m_TaaHistoryValid ? m_TaaHistorySrvSlot[readIdx] : m_SceneColorSrvSlot;
    cb.HistoryValid = m_TaaHistoryValid ? 1u : 0u;
    cb.DepthIndex = m_DepthSrvSlot;
    cb.PrevDepthIndex = m_TaaPrevDepthValid ? m_TaaPrevDepthSrvSlot : m_DepthSrvSlot;
    cb.OutIndex = m_TaaHistoryUavSlot[writeIdx];
    cb.DepthTolerance = kTaaDepthTolerance;
    cb.PrevJitter[0] = m_TaaPrevJitterPixels.x;
    cb.PrevJitter[1] = m_TaaPrevJitterPixels.y;
    m_CmdList->SetComputeRoot32BitConstants( 1, 18, &cb, 0 );

    m_CmdList->Dispatch( ( m_Resolution.x + 7 ) / 8, ( m_Resolution.y + 7 ) / 8, 1 );

    // Hand the result back to the scene colour for bloom/tonemap. Both are kSceneColorFormat, so this is a
    // straight CopyResource. NOTE: it also overwrites the scene colour's ALPHA with the TAA confidence weight.
    // Nothing downstream on this backend reads scene-colour alpha (bloom, luminance and the tonemap all take
    // .rgb), and the fog composition only preserves alpha rather than consuming it.
    {
        D3D12_RESOURCE_BARRIER toCopy[] = {
            TransitionBarrier( m_TaaHistory[writeIdx].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE ),
            TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST ),
        };
        m_CmdList->ResourceBarrier( _countof( toCopy ), toCopy );
        m_CmdList->CopyResource( m_SceneColor.Get(), m_TaaHistory[writeIdx].Get() );
    }

    // Snapshot this frame's depth for the NEXT frame's disocclusion test. Nothing else in the frame keeps a
    // previous-frame depth, so this pass owns the copy.
    if ( m_TaaPrevDepth ) {
        D3D12_RESOURCE_BARRIER toCopy[] = {
            TransitionBarrier( m_TaaPrevDepth.Get(), kPrevDepthReadState, D3D12_RESOURCE_STATE_COPY_DEST ),
            TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_SOURCE ),
        };
        m_CmdList->ResourceBarrier( _countof( toCopy ), toCopy );
        m_CmdList->CopyResource( m_TaaPrevDepth.Get(), m_DepthBuffer.Get() );
        D3D12_RESOURCE_BARRIER back[] = {
            TransitionBarrier( m_TaaPrevDepth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kPrevDepthReadState ),
            TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE ),
        };
        m_CmdList->ResourceBarrier( _countof( back ), back );
        m_TaaPrevDepthValid = true;
    } else {
        auto depthBack = TransitionBarrier( m_DepthBuffer.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE );
        m_CmdList->ResourceBarrier( 1, &depthBack );
    }

    // Rest states for the next frame: history back to UAV (both slots), scene colour back to RENDER_TARGET.
    {
        D3D12_RESOURCE_BARRIER post[] = {
            TransitionBarrier( m_TaaHistory[writeIdx].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
            TransitionBarrier( m_TaaHistory[readIdx].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
            TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_RENDER_TARGET ),
        };
        m_CmdList->ResourceBarrier( _countof( post ), post );
        m_SceneColorInPixelState = false;
    }

    // Rebind the scene colour + depth for whatever comes next in the frame (bloom reads it as an SRV and
    // re-transitions itself, but the render target must not be left unbound).
    BindSceneColorTarget();

    m_TaaHistoryIndex = readIdx;   // ping-pong
    m_TaaHistoryValid = true;
}
