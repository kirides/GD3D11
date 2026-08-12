// D3D12GraphicsEngine — AMD FidelityFX Super Resolution 3 temporal upscaler.
//
// Host side of the FFX FSR3Upscaler effect. The D3D11 counterpart is D3D11PFX_FSR3 + D3D11Upscaling; that
// pair is the spec this file implements, and every deliberate divergence is called out below.
//
// FRAME SLOT — different from D3D11's, on purpose. D3D11 runs FSR3 at the very end of its chain, on the
// TONEMAPPED LDR backbuffer, because that is where its render-graph upscale stage lives. This backend has a
// natural pre-tonemap slot instead: RenderFsr3Upscale runs after bloom + luminance adaptation and immediately
// BEFORE ResolveSceneToBackBuffer, on the LINEAR HDR scene colour. That is the placement AMD documents
// (HIGH_DYNAMIC_RANGE + AUTO_EXPOSURE want scene-referred colour, not a display-encoded image), and it costs
// no extra pass: the tonemap resolve already samples uv [0,1] of a source texture into a native-sized
// viewport, so pointing it at the display-res FSR output instead of the render-res scene colour turns its
// implicit bilinear upscale into a 1:1 blit. See GetTonemapSourceSrvSlot().
//
// The one consequence of being pre-tonemap: bloom and the auto-exposure histogram are computed at RENDER
// resolution and carried up by the upscale. Both are low-frequency by construction, so this is the cheap side
// of the trade rather than a compromise.
//
// MUTUAL EXCLUSION WITH TAA. AntiAliasingMode selects exactly one of AA_TAA / AA_FSR, so RenderTAA and
// RenderFsr3Upscale can never both run. They deliberately SHARE the jitter (AdvanceJitter, the FSR3 phase
// sequence) and the motion-vector G-buffer — sharing the sequence is what makes this a drop-in.
//
// SHIPPING REQUIREMENT: ffx_backend_dx12_x86.dll must sit beside the mod (blobs/libs/), next to the
// ffx_fsr3*_x86.dll pair D3D11 already needs. The .lib vendored here is an IMPORT library, so a missing DLL
// is a load-time failure for the whole process, not a graceful FSR3 fallback.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../zCCamera.h"

#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#pragma comment( lib, "ffx_backend_dx12_x86.lib" )
#pragma comment( lib, "ffx_fsr3_x86.lib" )
#pragma comment( lib, "ffx_fsr3upscaler_x86.lib" )

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {

    // Names for the three shared resources, in m_Fsr3Shared order.
    constexpr const wchar_t* kSharedNames[3] = {
        L"Fsr3DilatedDepth", L"Fsr3DilatedMotionVectors", L"Fsr3ReconstructedPrevNearestDepth"
    };

    // ffxGetResourceDX12 takes a NON-const wchar_t* for the debug name (the DX11 entry point takes a const
    // one), and the shared-resource descriptions hand out const pointers. Casting here keeps that in one
    // place; the backend only ever copies the string into FfxResource::name, and only in debug builds.
    FfxResource AsFfxResource( ID3D12Resource* res, const wchar_t* name, FfxResourceStates state ) {
        if ( !res ) return FfxResource{};
        return ffxGetResourceDX12( res, GetFfxResourceDescriptionDX12( res ),
            const_cast<wchar_t*>( name ), state );
    }

    /** DXGI format for one of the shared resources FFX asks us to allocate. Covers exactly what
        ffxFsr3UpscalerGetSharedResourceDescriptions returns as of FSR 3.1 — the same three formats
        D3D11PFX_FSR3 hardcodes. Anything else means the SDK changed and is reported rather than
        silently mis-allocated. */
    DXGI_FORMAT DxgiFromFfxSurfaceFormat( FfxSurfaceFormat fmt ) {
        switch ( fmt ) {
        case FFX_SURFACE_FORMAT_R32_FLOAT:    return DXGI_FORMAT_R32_FLOAT;      // dilatedDepth
        case FFX_SURFACE_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;   // dilatedMotionVectors
        case FFX_SURFACE_FORMAT_R32_UINT:     return DXGI_FORMAT_R32_UINT;       // reconstructedPrevNearestDepth
        default:                              return DXGI_FORMAT_UNKNOWN;
        }
    }

#ifdef DEBUG_D3D11
    void Fsr3Log( FfxMsgType type, const wchar_t* message ) {
        LogWarn() << "D3D12 FSR3 (" << static_cast<int>( type ) << "): " << message;
    }
#endif

} // namespace


/** AA_FSR is selected, the FSR 3 upscaler is the chosen upscaler, and everything the dispatch needs exists.
    Also the gate AdvanceJitter uses — the two must never disagree, or the frame is jittered with nothing
    resolving it (shimmer) or resolved with no jitter (blurry, no supersampling gain).

    Pure query: EnsureFsr3Ready() does the building, and AdvanceJitter calls it first so this answer is
    stable for the whole frame. */
bool D3D12GraphicsEngine::IsFsr3Enabled() const {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    return settings.AntiAliasingMode == GothicRendererSettings::AA_FSR
        && settings.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3
        // FSR3 upscales; it has no supersampling path. Above 100% the tonemap resolve's plain bilinear
        // downscale is the correct (and only) answer — same rule D3D11Upscaling::AddUpscalingPass applies.
        && settings.ResolutionScalePercent <= 100
        && m_Fsr3Context != nullptr
        && m_Fsr3SharedReady
        && m_Fsr3OutputReady
        && m_MotionResourcesReady          // no velocity buffer, no reprojection
        && m_VelocitySrvSlot != UINT_MAX
        && m_SceneColor
        && m_DepthBuffer;
}


/** Builds the FFX context, the three shared resources FFX asks the application to own, and the display-res
    HDR output — on the first frame FSR3 is wanted, and again after any resolution change (ReleaseFsr3 drops
    everything from the two Create*ResolutionTargets paths, which run with the GPU idled).

    Called from AdvanceJitter, i.e. the top of OnStartWorldRendering. Creation mid-frame is safe; DESTRUCTION
    is the part that needs an idle GPU, and that only ever happens on those idled paths or in the destructor. */
void D3D12GraphicsEngine::EnsureFsr3Ready() {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const bool wanted = settings.AntiAliasingMode == GothicRendererSettings::AA_FSR
        && settings.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3
        && settings.ResolutionScalePercent <= 100;
    if ( !wanted || m_Fsr3InitFailed ) return;
    if ( m_Fsr3Context && m_Fsr3SharedReady && m_Fsr3OutputReady ) return;
    if ( !m_SwapChainReady || !m_Allocator ) return;
    if ( m_Resolution.x < 4 || m_Resolution.y < 4 ) return;
    if ( m_BackbufferResolution.x < 4 || m_BackbufferResolution.y < 4 ) return;

    if ( !CreateFsr3Output( m_BackbufferResolution ) ) { m_Fsr3InitFailed = true; return; }
    if ( !CreateFsr3Context( m_Resolution, m_BackbufferResolution ) ) { m_Fsr3InitFailed = true; return; }

    LogInfo() << "D3D12: FSR 3 upscaler ready (" << m_Resolution.x << "x" << m_Resolution.y << " -> "
        << m_BackbufferResolution.x << "x" << m_BackbufferResolution.y << ").";
}


/** The display-resolution HDR target FSR3 writes and the tonemap resolve then reads. kSceneColorFormat so it
    is interchangeable with the scene colour everywhere downstream (the tonemap PSO's SRV, GetBackbufferData's
    re-tonemap) without a second PSO variant. Rests in PIXEL_SHADER_RESOURCE, which is where the tonemap
    wants it and where RenderFsr3Upscale puts it back. */
bool D3D12GraphicsEngine::CreateFsr3Output( INT2 size ) {
    m_Fsr3OutputReady = false;
    if ( size.x < 4 || size.y < 4 ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Allocator ) return false;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = static_cast<UINT64>( size.x );
    dd.Height = static_cast<UINT>( size.y );
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = kSceneColorFormat;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;   // FSR3's final RCAS pass writes it as a UAV
    if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr, m_Fsr3OutputAlloc.ReleaseAndGetAddressOf(),
        IID_PPV_ARGS( m_Fsr3Output.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: failed to create the FSR3 output target (" << size.x << "x" << size.y << ").";
        return false;
    }
    m_Fsr3Output->SetName( L"Fsr3Output" );
    m_Fsr3OutputInUavState = false;

    if ( m_Fsr3OutputSrvSlot == UINT_MAX ) m_Fsr3OutputSrvSlot = AllocateSrvSlot();
    if ( m_Fsr3OutputSrvSlot == UINT_MAX ) return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = kSceneColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView( m_Fsr3Output.Get(), &srv, GetSrvCpuHandle( m_Fsr3OutputSrvSlot ) );

    m_Fsr3OutputReady = true;
    m_Fsr3Reset = true;   // a fresh output means there is no coherent history to accumulate onto
    return true;
}


/** Creates the FFX DX12 backend interface, the FSR3Upscaler context, and the three "shared" resources whose
    allocation FFX delegates to the application (they exist so frame interpolation can consume the upscaler's
    dilated depth/motion; we don't use frame generation, but the dispatch requires them regardless).

    The scratch buffer must outlive the context — FfxInterface is copied into the context by value, but the
    pointer it carries is ours. */
bool D3D12GraphicsEngine::CreateFsr3Context( INT2 renderSize, INT2 upscaleSize ) {
    DestroyFsr3Context();

    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    const size_t scratchSize = ffxGetScratchMemorySizeDX12( FFX_FSR3UPSCALER_CONTEXT_COUNT );
    // MUST be zeroed, not malloc'd: ffxGetInterfaceDX12 reinterprets the scratch as its BackendContext_DX12
    // and tests `!backendContext->refCount` BEFORE memset-ing it, failing on a non-zero read. With malloc,
    // whether the interface comes up at all depends on heap garbage. (D3D11PFX_FSR3::Init has the same bug.)
    m_Fsr3Scratch = calloc( 1, scratchSize );
    if ( !m_Fsr3Scratch ) {
        LogWarn() << "D3D12: out of memory allocating the FSR3 backend scratch buffer (" << scratchSize << " bytes).";
        return false;
    }

    // Flags mirror D3D11PFX_FSR3::Init exactly. DEPTH_INVERTED | DEPTH_INFINITE describe this engine's
    // reversed-Z infinite projection (D32_FLOAT cleared to 0.0, GREATER_EQUAL) and are what make the
    // FLT_MAX/near-plane camera metrics in RenderFsr3Upscale the correct ones.
    FfxFsr3UpscalerContextDescription desc = {};
    desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE
        | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE
        | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED
        | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE
        | FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION;
#ifdef DEBUG_D3D11
    desc.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
    desc.fpMessage = &Fsr3Log;
#endif
    desc.maxRenderSize.width = static_cast<uint32_t>( renderSize.x );
    desc.maxRenderSize.height = static_cast<uint32_t>( renderSize.y );
    desc.maxUpscaleSize.width = static_cast<uint32_t>( upscaleSize.x );
    desc.maxUpscaleSize.height = static_cast<uint32_t>( upscaleSize.y );

    if ( ffxGetInterfaceDX12( &desc.backendInterface, ffxGetDeviceDX12( device ), m_Fsr3Scratch, scratchSize,
        FFX_FSR3UPSCALER_CONTEXT_COUNT ) != FFX_OK ) {
        LogWarn() << "D3D12: ffxGetInterfaceDX12 failed; FSR 3 unavailable.";
        free( m_Fsr3Scratch );
        m_Fsr3Scratch = nullptr;
        return false;
    }

    m_Fsr3Context = new FfxFsr3UpscalerContext{};
    const FfxErrorCode err = ffxFsr3UpscalerContextCreate( m_Fsr3Context, &desc );
    if ( err != FFX_OK ) {
        // FFX_ERROR_BACKEND_API_ERROR (0x8000000d) here means a D3D12 call inside the FFX backend failed and
        // FFX discarded the HRESULT; it can only come from CreatePipelineDX12. Requires instrumenting the SDK
        // to narrow further — see the FSR3 note in the project memory for the one that has already bitten us.
        LogWarn() << "D3D12: ffxFsr3UpscalerContextCreate failed (" << static_cast<int>( err )
            << "); FSR 3 unavailable.";
        delete m_Fsr3Context;
        m_Fsr3Context = nullptr;
        free( m_Fsr3Scratch );
        m_Fsr3Scratch = nullptr;
        return false;
    }

    m_Fsr3Reset = true;

    if ( !CreateFsr3SharedResources() ) {
        DestroyFsr3Context();
        return false;
    }
    return true;
}


/** The three application-owned resources of FfxFsr3UpscalerDispatchDescription. Sizes and formats come from
    the context itself rather than being hardcoded (D3D11PFX_FSR3 hardcodes them), so an SDK bump that
    changes one shows up as a log line instead of a silently wrong allocation.

    RESTING STATE = kFsr3SharedRestState, and every frame declares them as FFX_RESOURCE_STATE_COMPUTE_READ.
    That is not arbitrary: within one dispatch FFX writes each of them in the reconstruct-and-dilate pass and
    READS them in the later depth-clip/accumulate passes, so COMPUTE_READ is also where FFX leaves them — the
    declaration stays truthful across frames without us ever touching them. If the debug layer reports a
    before-state mismatch on one of these, this assumption is the thing to revisit first. */
bool D3D12GraphicsEngine::CreateFsr3SharedResources() {
    m_Fsr3SharedReady = false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Allocator || !m_Fsr3Context ) return false;

    FfxFsr3UpscalerSharedResourceDescriptions shared = {};
    if ( ffxFsr3UpscalerGetSharedResourceDescriptions( m_Fsr3Context, &shared ) != FFX_OK ) {
        LogWarn() << "D3D12: ffxFsr3UpscalerGetSharedResourceDescriptions failed; FSR 3 unavailable.";
        return false;
    }

    const FfxCreateResourceDescription* descs[3] = {
        &shared.dilatedDepth, &shared.dilatedMotionVectors, &shared.reconstructedPrevNearestDepth
    };

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    for ( UINT i = 0; i < 3; ++i ) {
        const FfxResourceDescription& src = descs[i]->resourceDescription;
        const DXGI_FORMAT fmt = DxgiFromFfxSurfaceFormat( src.format );
        if ( fmt == DXGI_FORMAT_UNKNOWN ) {
            LogWarn() << "D3D12: FSR3 asked for shared resource " << i << " in unmapped surface format "
                << static_cast<int>( src.format ) << "; FSR 3 unavailable (extend DxgiFromFfxSurfaceFormat).";
            return false;
        }

        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = src.width;
        dd.Height = src.height;
        dd.DepthOrArraySize = 1;
        dd.MipLevels = std::max<UINT16>( 1, static_cast<UINT16>( src.mipCount ) );
        dd.Format = fmt;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, kFsr3SharedRestState, nullptr,
            m_Fsr3SharedAlloc[i].ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_Fsr3Shared[i].ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create FSR3 shared resource " << kSharedNames[i] << ".";
            return false;
        }
        m_Fsr3Shared[i]->SetName( kSharedNames[i] );
    }

    m_Fsr3SharedReady = true;
    return true;
}


/** Destroys the FFX context (which releases every internal FSR3 resource) and frees the backend scratch.
    The GPU MUST be idle: FFX releases its resources immediately, with no deferral through the engine's
    pending-cleanup queue. Every caller either sits behind WaitForGpuIdle() or is the destructor. */
void D3D12GraphicsEngine::DestroyFsr3Context() {
    if ( m_Fsr3Context ) {
        ffxFsr3UpscalerContextDestroy( m_Fsr3Context );
        delete m_Fsr3Context;
        m_Fsr3Context = nullptr;
    }
    if ( m_Fsr3Scratch ) {
        free( m_Fsr3Scratch );
        m_Fsr3Scratch = nullptr;
    }
    m_Fsr3SharedReady = false;
    for ( UINT i = 0; i < 3; ++i ) {
        m_Fsr3Shared[i].Reset();
        m_Fsr3SharedAlloc[i].Reset();
    }
}


/** Full teardown: context + shared resources + the display-res output. Called from both
    Create*ResolutionTargets paths (a render OR display resolution change invalidates the context, which is
    built for one specific pair) and from the destructor. EnsureFsr3Ready rebuilds on the next frame that
    wants FSR3. Clears m_Fsr3InitFailed, so a resize is also the retry opportunity after a failed init. */
void D3D12GraphicsEngine::ReleaseFsr3() {
    DestroyFsr3Context();
    m_Fsr3OutputReady = false;
    m_Fsr3Output.Reset();
    m_Fsr3OutputAlloc.Reset();
    m_Fsr3OutputInUavState = false;
    // The SRV heap slot is deliberately KEPT (like the bloom/TAA slots) and re-pointed by CreateFsr3Output.
    m_Fsr3RanThisFrame = false;
    m_Fsr3Reset = true;
    m_Fsr3InitFailed = false;
}


/** The dispatch. Reads the render-res linear-HDR scene colour + depth + velocity, writes the display-res
    m_Fsr3Output, and hands the tonemap resolve a native-sized source (GetTonemapSourceSrvSlot).

    Called from OnStartWorldRendering after RenderBloom/RenderLuminanceAdapt and before
    ResolveSceneToBackBuffer. No-op unless IsFsr3Enabled(). */
void D3D12GraphicsEngine::RenderFsr3Upscale() {
    m_Fsr3RanThisFrame = false;
    if ( !m_FrameOpen || !m_CmdList ) return;
    if ( !IsFsr3Enabled() ) return;

    DX_ZONE( m_CmdList.Get(), "FSR3 upscale" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "FSR3 upscale" );

    // --- states in -------------------------------------------------------------------------------------
    // Depth leaves DEPTH_WRITE, so the DSV must be unbound first (same dance RenderBloom / RenderTAA do).
    // Everything FFX reads goes to NON_PIXEL_SHADER_RESOURCE exactly (not the combined read state the
    // velocity buffer normally rests in): FFX_RESOURCE_STATE_COMPUTE_READ maps to that single state, and a
    // barrier FFX issues with a wider before-state than the resource actually carries is a debug-layer error.
    m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );
    {
        D3D12_RESOURCE_BARRIER pre[4];
        UINT n = 0;
        pre[n++] = TransitionBarrier( m_SceneColor.Get(),
            m_SceneColorInPixelState ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                     : D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        pre[n++] = TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        pre[n++] = TransitionBarrier( m_VelocityBuffer.Get(),
            m_VelocityInPixelState ? kVelocityReadState : D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        pre[n++] = TransitionBarrier( m_Fsr3Output.Get(),
            m_Fsr3OutputInUavState ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                   : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        m_CmdList->ResourceBarrier( n, pre );
        m_SceneColorInPixelState = true;   // corrected to PIXEL_SHADER_RESOURCE on the way out
        m_Fsr3OutputInUavState = true;
    }

    // --- dispatch description --------------------------------------------------------------------------
    FfxFsr3UpscalerDispatchDescription dd = {};
    dd.commandList = ffxGetCommandListDX12( m_CmdList.Get() );

    dd.color = AsFfxResource( m_SceneColor.Get(), L"Fsr3InputColor", FFX_RESOURCE_STATE_COMPUTE_READ );
    dd.depth = AsFfxResource( m_DepthBuffer.Get(), L"Fsr3InputDepth", FFX_RESOURCE_STATE_COMPUTE_READ );
    dd.motionVectors = AsFfxResource( m_VelocityBuffer.Get(), L"Fsr3InputVelocity", FFX_RESOURCE_STATE_COMPUTE_READ );
    dd.output = AsFfxResource( m_Fsr3Output.Get(), L"Fsr3Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS );
    FfxResource* sharedSlots[3] = { &dd.dilatedDepth, &dd.dilatedMotionVectors, &dd.reconstructedPrevNearestDepth };
    for ( UINT i = 0; i < 3; ++i ) {
        *sharedSlots[i] = AsFfxResource( m_Fsr3Shared[i].Get(), kSharedNames[i], FFX_RESOURCE_STATE_COMPUTE_READ );
    }
    // exposure / reactive / transparencyAndComposition stay zeroed — all three are optional. AUTO_EXPOSURE
    // covers the first. The other two would need per-pixel authoring this backend has no producer for:
    // D3D11 fills its reactive mask from the rain pass (its "Draw Rain" writes RT1) and the D3D12 rain pass
    // has no second render target. An absent mask means "nothing reactive", which is exactly what an all-zero
    // mask would mean, so this is a feature gap and not a correctness one.

    dd.renderSize.width = static_cast<uint32_t>( m_Resolution.x );
    dd.renderSize.height = static_cast<uint32_t>( m_Resolution.y );
    dd.upscaleSize.width = static_cast<uint32_t>( m_BackbufferResolution.x );
    dd.upscaleSize.height = static_cast<uint32_t>( m_BackbufferResolution.y );

    // Jitter: AdvanceJitter already advanced the FSR3 phase sequence and baked the offset into
    // TransformProj._13/_23 for this frame's geometry. FSR wants the SAME value, in pixels, unscaled.
    dd.jitterOffset.x = m_TaaJitterPixels.x;
    dd.jitterOffset.y = m_TaaJitterPixels.y;
    // m_VelocityBuffer holds UV-space (prevUV - currUV) motion; FSR works in pixels. Same scale D3D11 passes.
    dd.motionVectorScale.x = static_cast<float>( m_Resolution.x );
    dd.motionVectorScale.y = static_cast<float>( m_Resolution.y );

    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    dd.enableSharpening = settings.SharpenFactor >= 0.001f;
    dd.sharpness = std::clamp( settings.SharpenFactor, 0.0f, 1.0f );   // FSR3: 0 = none, 1 = max
    // FFX validates frameTimeDelta as milliseconds and rejects implausibly small values; D3D11 clamps to 1ms.
    dd.frameTimeDelta = std::max( 1.0f, Engine::GAPI->GetDeltaTime() * 1000.0f );
    dd.preExposure = 1.0f;              // the tonemap applies exposure AFTER the upscale, never before it
    dd.reset = m_Fsr3Reset;
    dd.viewSpaceToMetersFactor = 0.01f; // ZENGIN world units are centimetres

    // Camera metrics, matching D3D11Upscaling::AddFSR3Pass. With DEPTH_INVERTED | DEPTH_INFINITE, FSR wants
    // the metrics of the INVERTED depth range: "near" is at infinity and "far" is the real near plane.
    // GetFOV's first out-parameter is the one D3D11 feeds cameraFovAngleVertical; keep them identical so any
    // mistake here is at least the same mistake on both backends.
    float fovA = 90.0f, fovB = 90.0f;
    zCCamera* cam = zCCamera::GetCamera();
    if ( cam ) cam->GetFOV( fovA, fovB );
    dd.cameraFovAngleVertical = XMConvertToRadians( fovA );
    dd.cameraNear = FLT_MAX;
    dd.cameraFar = std::max( cam ? cam->GetNearPlane() : 0.01f, 0.075f );   // FFX validation wants >= 0.075

    const FfxErrorCode err = ffxFsr3UpscalerContextDispatch( m_Fsr3Context, &dd );

    // --- state cache + heaps ---------------------------------------------------------------------------
    // The FFX backend records on the RAW list: its own PSOs, root signatures, descriptor heaps and tables.
    // The state cache cannot see any of it, so drop the whole shadow and re-bind our SRV heap — the same
    // contract imgui_impl_dx12 has in Present (see D3D12StateCache.h).
    m_CmdList.InvalidateAll();
    if ( m_SrvHeap ) {
        ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
        m_CmdList->SetDescriptorHeaps( 1, heaps );
    }

    // --- states out ------------------------------------------------------------------------------------
    // Rest states for the rest of this frame and for the next one: scene colour in PIXEL_SHADER_RESOURCE
    // (where ResolveSceneToBackBuffer expects it and where next frame's BindSceneColorTarget transitions
    // FROM), depth back to DEPTH_WRITE, velocity back to its combined read state, output to
    // PIXEL_SHADER_RESOURCE for the tonemap. The shared resources are left alone on purpose — see
    // CreateFsr3SharedResources.
    {
        D3D12_RESOURCE_BARRIER post[4];
        UINT n = 0;
        post[n++] = TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
        post[n++] = TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE );
        post[n++] = TransitionBarrier( m_VelocityBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            kVelocityReadState );
        post[n++] = TransitionBarrier( m_Fsr3Output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
        m_CmdList->ResourceBarrier( n, post );
        m_VelocityInPixelState = true;
        m_Fsr3OutputInUavState = false;
    }

    if ( err != FFX_OK ) {
        // Reported rather than early-returned: the pre-barriers have already executed, so the states-out block
        // above must run regardless. Leaving m_Fsr3RanThisFrame false makes the tonemap fall back to the scene
        // colour, i.e. this frame is simply bilinearly upscaled (and RenderSharpen takes over again).
        if ( !m_Fsr3DispatchFailureLogged ) {
            LogWarn() << "D3D12: ffxFsr3UpscalerContextDispatch failed (" << static_cast<int>( err )
                << "); falling back to bilinear upscaling. Further failures are not logged.";
            m_Fsr3DispatchFailureLogged = true;
        }
        return;
    }

    m_Fsr3Reset = false;
    m_Fsr3RanThisFrame = true;
}


/** Which texture the tonemap resolve (and GetBackbufferData's capture re-tonemap) should sample. Both
    candidates are kSceneColorFormat and both rest in PIXEL_SHADER_RESOURCE, so this is the whole of the
    "FSR3 replaces the tonemap's implicit bilinear upscale" wiring: a display-res source under the
    already-native viewport is a 1:1 blit. */
UINT D3D12GraphicsEngine::GetTonemapSourceSrvSlot() const {
    if ( m_Fsr3RanThisFrame && m_Fsr3OutputSrvSlot != UINT_MAX ) return m_Fsr3OutputSrvSlot;
    return m_SceneColorSrvSlot;
}
