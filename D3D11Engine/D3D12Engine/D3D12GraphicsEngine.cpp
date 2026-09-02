// D3D12GraphicsEngine — core: device/queues/swapchain/frame/present/uploads/resources.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12ResourceCreate.h"
#include "D3D12LineRenderer.h"
#include "D3D12VertexBuffer.h"
#include "D3D12Texture.h"
#include <d3dx12_barriers.h>
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../zCView.h"
#include "../zCWorld.h"
#include "../ImGuiShim.h"
#include "../oCGame.h"
#include "../DXGIHelpers.h"
#include "../WindAnimation.h"
#include "../GMeshSimple.h"

#include "D3D12TracyDebug.h"

// imgui_impl_dx12 calls CreateDXGIFactory1 directly (for tearing detection). dxgi.dll is present on
// every Windows 7+ and the D3D11 fallback swapchain already needs it at runtime, so a load-time link
// here is safe — it does NOT reintroduce the D3D12 soft-dependency that lets old systems fall back.
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    static constexpr UINT64 kCopyBatchFlushThresholdBytes = 32ull * 1024 * 1024;
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    constexpr UINT kSrvHeapCapacity = 65536;
}

D3D12GraphicsEngine::D3D12GraphicsEngine() {
    m_LineRenderer = std::make_unique<D3D12LineRenderer>();
    m_BackbufferResolution = m_NewResolution = Engine::GAPI->GetRendererState().RendererSettings.LoadedResolution;
    m_Resolution = ComputeRenderResolution( m_BackbufferResolution );
    m_AppliedResolutionScalePercent = Engine::GAPI->GetRendererState().RendererSettings.ResolutionScalePercent;
    // Static samplers are serialized into the root signature blob, so the bias has to be right before Init()
    // builds any of them. A later change goes through RebakeMipLodBias.
    m_AppliedMipLodBias = ComputeMipLodBias( m_Resolution, m_BackbufferResolution );
    D3D12RootLayout::SetAnisoMipLodBias( m_AppliedMipLodBias );
    // Same LowLatency toggle D3D11 uses for its swapchain waitable object. kBackBufferCount is always
    // N+1 - 1 slot for the frame currently in flight plus N queued behind it - so normally N=2 queued
    // (3 slots) and LowLatency trims that to N=1 queued (2 slots, matching D3D11's non-low-latency
    // double buffering). Must be set before the Attach() calls below so D3D12ShadowMap/D3D12PointShadows
    // pick up the right value, and before any per-frame resource is created - switching it requires a restart.
    kBackBufferCount = Engine::GAPI->GetRendererState().RendererSettings.LowLatency ? 2 : 3;
    // Hand the shadow modules their engine back-reference HERE, not in their Init(): Init() has ordering
    // constraints (the caster PSOs need the depth-prepass shader blobs, so the CSM map is created well into
    // Init()), but the engine calls into the modules BEFORE that — CreateWorldIndirect/CreateVobIndirect ask
    // them to build their per-cascade argument rings while defining the shared command-signature layout. So
    // the back-reference must be valid from construction, independent of any Init() ordering.
    m_ShadowMap.Attach( *this );
    m_PointShadows.Attach( *this );
}

D3D12GraphicsEngine::~D3D12GraphicsEngine() {
    if ( m_SwapChainReady ) {
        WaitForGpuIdle();
        // Force-run all remaining cleanups — the GPU is idle, so every deferral has expired.
        std::deque<PendingCleanupJob> jobs;
        {
            std::lock_guard<std::mutex> lock( m_CleanupMutex );
            jobs.swap( m_PendingCleanupJobs );
        }
        for ( auto& pending : jobs ) {
            if ( pending.Job ) pending.Job();
        }
    }
    // After the idle+drain above: the FFX context releases its internal D3D12 resources synchronously, so it
    // must not outlive in-flight work — and must go before the device does.
    ReleaseFsr3();
    if ( m_FenceEvent ) CloseHandle( m_FenceEvent );
    if ( m_UploadEvent ) CloseHandle( m_UploadEvent );
    if ( m_FrameLatencyWaitableObject ) CloseHandle( m_FrameLatencyWaitableObject );
}

XRESULT D3D12GraphicsEngine::Init() {
    if ( !m_Device.Init() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: device creation failed.";
        return XR_FAILED;
    }
    D3D12CmdList::SetEnhancedBarriersDeviceSupport( m_Device.EnhancedBarriersSupported() );

    m_DeviceCapabilities.DeviceDescription = m_Device.GetDeviceDescription();
    DXGI_ADAPTER_DESC1 adapterDesc = {};
    if ( m_Device.GetAdapter() && SUCCEEDED( m_Device.GetAdapter()->GetDesc1( &adapterDesc ) ) ) {
        m_DeviceCapabilities.VendorId = adapterDesc.VendorId;
    }
    // No vendor driver extensions in this backend - indirect draws and explicit UAV barriers are core D3D12.
    m_DeviceCapabilities.DriverExtensions = false;
    m_DeviceCapabilities.MultiDrawIndirect = true;
    m_DeviceCapabilities.UAVOverlap = true;
    m_DeviceCapabilities.Native16BitTextures = true;
    m_DeviceCapabilities.LayeredRendering = m_Device.LayeredRenderingSupported();
    // Gated at adapter selection (DeviceSupportsBindless), so a device that got this far has it.
    m_DeviceCapabilities.BindlessResources = true;
    m_DeviceCapabilities.EnhancedBarriers = m_Device.EnhancedBarriersSupported();
    m_DeviceCapabilities.TypedUAVLoadAdditionalFormats = m_Device.TypedUAVLoadAdditionalFormatsSupported();
    Engine::GAPI->GetRendererState().RendererSettings.ApplyDeviceCapabilities( m_DeviceCapabilities );

    if ( !CreateAllocators() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create allocators.";
        return XR_FAILED;
    }
    if ( !CreateUploadObjects() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create upload objects.";
        return XR_FAILED;
    }
    if ( !InitCopyQueue() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to initialize the copy queue.";
        return XR_FAILED;
    }
    if ( !CreateSrvHeap() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create SRV heap.";
        return XR_FAILED;
    }
    if ( !m_TexturePool.Attach( *this ) ) {
        // Non-fatal: nothing consumes the pool yet (see D3D12RenderGraph.h), so a failure here just means
        // that infrastructure stays unavailable until the next Init(). Nothing in today's frame depends on it.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the pooled-render-target RTV heap.";
    }
    if ( !m_AliasArena.Attach( *this ) ) {
        // Non-fatal for the same reason: no pass constructs a D3D12RenderGraph yet (see D3D12RenderGraph.h).
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the render-graph aliasing arena.";
    }
    if ( !m_Pipelines.Init( &m_Device, &m_ShaderBackend ) ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to init the pipeline-state module.";
        return XR_FAILED;
    }
    // Must run BEFORE any Create*() too: every scene PSO bakes kSceneColorFormat into RTVFormats[0].
    // R11G11B10 drops alpha (nothing blends against destination alpha) but is an optional typed-UAV
    // format, and the TAA/DoF/bloom compute passes bind the scene colour as a UAV.
    {
        auto& rs = Engine::GAPI->GetRendererState().RendererSettings;
        if ( rs.CompressBackBuffer ) {
            if ( m_DeviceCapabilities.TypedUAVLoadAdditionalFormats ) {
                kSceneColorFormat = DXGI_FORMAT_R11G11B10_FLOAT;
                LogInfo() << "D3D12: compressed scene colour (R11G11B10_FLOAT).";
            } else {
                LogWarn() << "D3D12: CompressBackBuffer requested but the device lacks "
                             "TypedUAVLoadAdditionalFormats; keeping R16G16B16A16_FLOAT.";
                rs.CompressBackBuffer = false;
            }
        }
    }

    // Must run BEFORE any Create*(): it decides m_Pipelines.DisplayFormat, which every display-space PSO
    // (2D/UI, video, preview, lines, tonemap, SMAA's final pass, sharpen) bakes into its RTV format. That is
    // also why the HDR toggle needs a restart rather than taking effect on the next resize.
    DetectHdrOutputCapability();
    if ( m_HdrOutputActive && !m_Pipelines.CreateHdrEncode() ) {
        // Without the encode pass nothing in the FP16 display buffer would ever reach the swapchain, so give
        // up on HDR here — while it is still free to do so, i.e. before any display-space PSO has baked
        // DisplayFormat. Everything downstream then builds exactly as it does in SDR.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the HDR scanout encode pipeline; falling back to SDR output.";
        m_HdrOutputActive = false;
        m_HdrEncodePQ = false;
        m_Pipelines.DisplayFormat = kBackBufferFormat;
    }
    if ( !m_Pipelines.CreateUI() || !CreateUIVertexBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the 2D/UI pipeline.";
        return XR_FAILED;
    }
    if ( !CreateWhiteTexture() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the white fallback texture.";
        return XR_FAILED;
    }
    LoadDistortionTexture();   // non-fatal: wet ground just skips the no-normalmap fallback if this is missing
    if ( !m_Pipelines.CreateWorld() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the world-mesh pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateDepthPrepass() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the depth prepass pipeline.";
        return XR_FAILED;
    }
    if ( !CreateWorldIndirect() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the world ExecuteIndirect resources.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateWorldTransparency() ) {
        // Non-fatal: DrawWorldTransparencyRun early-outs on a missing root sig, which leaves the peeled
        // alpha-blended world surfaces (ice/glass) simply undrawn — the same as before this pass existed,
        // minus their opaque stand-in. Everything else keeps rendering.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the alpha-blended world-mesh pipeline "
                     "(ice/glass surfaces will not be drawn).";
    }
    if ( !m_Pipelines.CreateLightCull() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the light-culling compute pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateVob() || !CreateVobInstanceBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the VOB pipeline.";
        return XR_FAILED;
    }
    if ( !CreateVobIndirect() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the VOB ExecuteIndirect resources.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateCull() || !CreateVobCullResources() ) {
        // Non-fatal: GPU VOB culling is an optimization, not a resource anything samples unconditionally.
        // EvaluateGpuVobCulling() returns false when any of this is missing, which leaves the CPU per-VOB
        // frustum cull in charge (RndCullContext::drawFlags.SkipVobFrustumCull stays clear) — the exact
        // behavior the backend had before. Must run AFTER CreateVobInstanceBuffers (it sizes the compacted
        // instance buffer from m_VobInstanceBufferCapacity) and after CreateVobIndirect.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the GPU VOB-culling resources (falling back to CPU frustum culling).";
    }
    if ( !m_Pipelines.CreateMorphFold() || !CreateMorphFoldResources() ) {
        // Non-fatal, but it MUST be attempted here — before the first world/attachment conversion. Whether
        // the fold is available is what decides how a morph submesh's vertex buffer is created (DEFAULT+UAV
        // for the GPU fold vs DYNAMIC+CA_WRITE for ZENGIN's CPU deform), and MorphGpu::IsActive() freezes
        // that answer for the session. Failing here simply leaves the CPU deform as the path.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the GPU morph-fold resources (morph meshes will deform on the CPU).";
    }
    if ( !CreateLightBuffer() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the point-light buffer.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateSkeletal() || !CreateSkeletalConstantBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the skeletal pipeline.";
        return XR_FAILED;
    }
    if ( !CreateSkeletalIndirect() ) {
        // Fatal: both skeletal passes submit exclusively through these (T9), so a missing signature/ring would
        // silently drop every NPC/monster and every node attachment. Must run after CreateSkeletal (it needs
        // Skeletal.RootSig) and after CreateVobIndirect (the attachment rings ride the VOB command signature).
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the skeletal ExecuteIndirect resources.";
        return XR_FAILED;
    }
    if ( !CreateShadowConstantBuffer() || !m_ShadowMap.Init() ) {
        // Fatal: the lit world PSO samples the shadow map (t4) + CB (b3) unconditionally, so a missing map would
        // leave those root slots unbound. Failing here cleanly falls back to D3D11 (D3D12 is dev-forced/opt-in).
        // Runs after the depth-prepass + VOB + skeletal pipelines so the caster PSOs can reuse all three depth VS blobs.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the sun shadow map.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreatePointShadow() || !m_PointShadows.Init() ) {
        // Fatal: the lit PSOs sample the cube array (t5) unconditionally once P2.10d lands, so a missing resource
        // would leave that root slot unbound. Failing here cleanly falls back to D3D11 (D3D12 is dev-forced).
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create point-light shadow cubes.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateWater() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the water pipeline.";
        return XR_FAILED;
    }
    if ( !CreateWaterConstantBuffers() ) {
        // Fatal-ish for water only: DrawWaterSurfaces skips its color pass without the CB (the Z-prepass
        // still runs so height fog stays correct), leaving water surfaces unshaded. Not worth failing the
        // whole backend over — but it should never happen, so log it loudly.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the water constant buffers (water will not be shaded).";
    }
    LoadReflectionCube();   // non-fatal: water then reflects only on-screen geometry via SSR
    if ( !m_Pipelines.CreateParticle() || !CreateParticleInstanceBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the particle pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateDecal() || !CreateDecalQuadVB() || !CreateDecalInstanceBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the decal pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateTonemap() ) {
        // Fatal: the 3D scene PSOs now target the HDR scene-color RT (kSceneColorFormat), so without the tonemap
        // resolve nothing reaches the swapchain. Failing here cleanly falls back to D3D11 (D3D12 is dev-forced).
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the tonemap pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateLumAdapt() || !CreateLumAdaptedBuffer() ) {
        // Fatal: Tonemap.hlsl's PS now reads m_LumAdaptedBuffer (t1) unconditionally every frame — a missing
        // buffer would leave that root SRV unbound. Same reasoning as the tonemap PSO itself just above.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the dynamic-exposure (auto-exposure) pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreatePreview() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the inventory-item preview pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateBloom() ) {
        // Non-fatal: bloom is an opt-in visual enhancement (RendererSettings.EnableBloom, default off), not a
        // required resource any other PSO samples unconditionally — unlike tonemap/shadow/point-shadow above.
        // RenderBloom() guards on the PSOs existing and just skips the effect if this failed.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the bloom pipeline (bloom will be unavailable).";
    }
    if ( !m_Pipelines.CreateGhost() ) {
        // Non-fatal: ghost/transparency VOBs are a niche effect (invisible-potion/fade items). DrawGhostVobs()
        // guards on Ghost.PSO existing and, if this failed, simply drains+discards Engine::GAPI->TransparencyVobs
        // every frame instead of drawing it — GothicAPI still needs that drain to avoid an unbounded per-frame leak.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the ghost pipeline (ghost VOBs will be invisible).";
    }
    if ( !m_Pipelines.CreateGhostSkeletal() ) {
        // Non-fatal: skeletal ghosts (invisible/fading NPCs) are rarer still. DrawGhostVobs() guards on
        // GhostSkeletal.PSO existing and just skips those entries (still drained, no leak) if this failed.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the skeletal ghost pipeline (invisible NPCs will not render).";
    }
    if ( !m_Pipelines.CreateGrass() ) {
        // Non-fatal: vegetation boxes are an optional decoration layer. DrawVegetation() guards on
        // Grass.PSO existing and just skips the pass (grass simply doesn't render) if this failed.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the grass pipeline (vegetation boxes will not render).";
    } else if ( !m_ShadowMap.CreateGrassCaster() ) {
        // Non-fatal: the CSM pass guards on the grass caster PSO and just skips grass's shadow
        // contribution (it still renders lit, just casts no shadow) if this failed.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the grass shadow-caster pipeline (grass will not cast shadows).";
    }
    if ( !CreateShadowRecordCommandLists() ) {
        // Non-fatal: BeginShadowRecording() checks m_ShadowCmdListsReady and falls back to recording every shadow
        // pass serially into m_CmdList (exactly what it did before) — slower, identical output.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the shadow command lists (shadow passes will be recorded single-threaded).";
    }
    if ( !m_Pipelines.CreateVideo() ) {
        // Non-fatal: Bink cutscene playback (zBinkPlayer.cpp) is a niche path. DrawVertexArray's PS_Video branch
        // guards on Video.PSO existing and falls back to the normal FF/UI draw (a black/untextured quad) if not.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the video (Bink) pipeline (cutscenes will not render).";
    }
    if ( !m_Pipelines.CreateSmaa() ) {
        // Non-fatal: SMAA is an opt-in AA mode (RendererSettings.AntiAliasingMode == AA_SMAA). RenderSMAA()
        // guards on the PSOs existing and just skips the effect (no AA) if this failed.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the SMAA pipeline (SMAA anti-aliasing will be unavailable).";
    } else {
        LoadSmaaTextures();   // non-fatal; RenderSMAA also guards on the LUTs being present
    }
    if ( !m_Pipelines.CreateSharpen() ) {
        // Non-fatal: RenderSharpen() guards on the PSO for the selected mode and just leaves the frame
        // unsharpened. Note this one IS on by default (RendererSettings.SharpeningMode == SHARPEN_CAS).
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the sharpen pipeline (the image will not be sharpened).";
    }
    if ( !m_Pipelines.CreateGammaCorrect() ) {
        // Non-fatal: ApplyDisplayGammaCorrection() guards on the PSO, so the frame just shows at the
        // uncorrected 1.0/1.0 brightness and contrast.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the gamma-correct pipeline (the brightness/contrast sliders will do nothing).";
    }
    if ( !m_Pipelines.CreateUnderwater() ) {
        // Non-fatal: DrawUnderwaterEffects() guards on the PSOs, so a failure here just leaves the frame
        // untinted and undistorted while swimming — everything else renders as before.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the underwater pipeline (no underwater screen effect).";
    }
    if ( !m_Pipelines.CreateAO() ) {
        // Non-fatal: SSAO is an opt-in visual enhancement (RendererSettings.AoMode, defaults to a real AO mode
        // but nothing else depends on it unconditionally). RenderSSAO() guards on the PSOs existing and just
        // leaves the AO mask at white (no occlusion) if this failed.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the SSAO pipeline (screen-space AO will be unavailable).";
    }
    // Intel XeGTAO — AoMode::AO_ASSAO on this backend. Non-fatal: IsGtaoEnabled() reports false and RenderSSAO
    // falls back to the simple SSAO path above, so a failure here costs AO quality, never AO.
    if ( !m_Pipelines.CreateGtao() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the XeGTAO pipeline "
                     "(AO mode 'ASSAO' will fall back to the simple SSAO).";
    }
    // Motion-vector G-buffer support (camera-velocity fill + debug overlay). Non-fatal: nothing consumes the
    // velocity/normal targets yet (TAA consumes velocity; FSR3 is still to come, and XeGTAO deliberately derives
    // its own normals from the depth snapshot instead), so a failure here costs TAA, never the frame. CreateMotionConstantBuffers must succeed too — without
    // the CB the *GBuf prepass PSOs would have an unbound root CBV, so failure disables the whole feature.
    if ( !m_Pipelines.CreateMotion() || !CreateMotionConstantBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the motion-vector pipeline "
                     "(no motion vectors / normal buffer — TAA and FSR3 will have no input).";
        m_MotionResourcesReady = false;
    }
    // Temporal AA (Intel's TAA resolve). Non-fatal: RenderTAA and AdvanceJitter both guard through
    // IsTaaEnabled(), so a failure here leaves the frame un-jittered and un-resolved exactly as before.
    if ( !m_Pipelines.CreateTaa() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the TAA pipeline (temporal AA unavailable).";
    }
    // Depth of field. Non-fatal and opt-in (RendererSettings.EnableDoF, off by default): RenderDepthOfField
    // guards on the PSOs and leaves the whole scene in focus if this failed. Its textures are built lazily the
    // first time DoF is switched on, not here — see CreateDoFResources.
    if ( !m_Pipelines.CreateDoF() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the depth-of-field pipeline (DoF unavailable).";
    }
    if ( !m_Pipelines.CreateSkyIbl() ) {
        // Non-fatal: the lit shaders test the sky-IBL cube indices for 0xFFFFFFFF and fall back to the flat
        // ambient term they used before this existed, so a failure here costs indirect specular, not lighting.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the sky-IBL pipeline (falling back to flat ambient).";
    } else if ( !CreateSkyIblResources() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the sky-IBL cubemaps (falling back to flat ambient).";
    }
    if ( !m_Pipelines.CreateSky() ) {
        // Non-fatal: DrawSky() falls back to Gothic's fixed-function sky (zCSkyController_Outdoor::RenderSkyPre)
        // whenever DrawAtmosphereSkyDome() can't draw, so a failure here costs the per-frame draw-call saving,
        // never the sky itself.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the sky-dome pipeline (falling back to Gothic's fixed-function sky).";
    } else if ( !CreateSkyConstantBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the sky-dome constant buffers (falling back to Gothic's fixed-function sky).";
    }
    if ( !m_Pipelines.CreateFog() ) {
        // Non-fatal: the height-fog/god-ray composition is opt-in (RendererSettings.DrawFog / EnableGodRays)
        // and outdoor-only. RenderFogAndGodRays() guards on the PSOs; EvaluateHeightFogActive() additionally
        // keeps the geometry shaders' cheap linear distance fog switched ON when this failed, so a broken
        // composition pipeline degrades to the pre-existing D3D12 fog instead of no fog at all.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the height-fog/god-ray pipeline (falling back to the shaders' linear distance fog).";
    } else if ( !CreateFogConstantBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the height-fog constant buffers (falling back to the shaders' linear distance fog).";
    }
    if ( !m_Pipelines.CreateAdvanceRain() ) {
        // Non-fatal: rain/snow is an opt-in weather effect (RendererSettings.EnableRain). AdvanceRain() guards
        // on the PSO existing and just skips advancing/drawing particles if this failed.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the rain-advance compute pipeline (rain/snow particles will be unavailable).";
    }
    if ( !m_Pipelines.CreateRainDraw() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the rain-draw pipeline (rain/snow particles will be unavailable).";
    }
    if ( !m_Pipelines.CreateLines() || !CreateLineVertexBuffers() ) {
        // Non-fatal: debug/editor lines are a diagnostic overlay. DrawLines() guards on the PSOs + ring
        // existing; D3D12LineRenderer still drains its caches every frame either way (no unbounded growth).
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the debug-line pipeline (debug lines will not render).";
    }
    if ( !m_Pipelines.CreateFx() || !CreateFxVertexBuffers() ) {
        // Non-fatal: the three FX passes guard on the root sig / ring existing. Without them blood splatter,
        // spell ground marks and weapon/spell trails simply don't draw — the same as before they were ported.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the FX pipeline (quad marks and poly strips will not render).";
    }
    D3D12ShaderBackend::LogAndResetCacheStats( "startup" );
    LogInfo() << "D3D12GraphicsEngine initialized (device + 2D + world + VOB + skeletal + water + particle + decal + HDR tonemap pipelines up). Swapchain is created once the game window is set.";
    return XR_SUCCESS;
}


bool D3D12GraphicsEngine::CreateAllocators() {
    D3D12MA::ALLOCATOR_DESC allocatorDesc{};
    allocatorDesc.pDevice = m_Device.GetDevice();
    allocatorDesc.pAdapter = m_Device.GetAdapter();
    allocatorDesc.Flags = D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED;

    if ( GetModuleHandleA( "renderdoc.dll" ) != NULL ) {
        allocatorDesc.Flags |= D3D12MA::ALLOCATOR_FLAGS::ALLOCATOR_FLAG_ALWAYS_COMMITTED;
    }

    if (FAILED(D3D12MA::CreateAllocator(&allocatorDesc, m_Allocator.ReleaseAndGetAddressOf()))) {
        return false;
    }
	if ( m_Allocator->IsGPUUploadHeapSupported() ) {
	    // Holy hell, D3D12_HEAP_TYPE_GPU_UPLOAD is fucking expensive ?? Do not use if doing many updates! this completely tanks FPS
	    // for example for dynamic verticies, this causes 99% usage in FixedFunction vertex updates
		// DefaultUploadHeapType = D3D12_HEAP_TYPE_GPU_UPLOAD;
	}
    
    return m_Allocator != nullptr;
}


bool D3D12GraphicsEngine::CreateUploadObjects() {
    ID3D12Device* device = m_Device.GetDevice();
    if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS( m_UploadAllocator.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_UploadAllocator.Get(), nullptr, IID_PPV_ARGS( m_UploadCmdList.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_UploadCmdList->Close();
    if ( FAILED( device->CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( m_UploadFence.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_UploadEvent = CreateEvent( nullptr, FALSE, FALSE, nullptr );
    return m_UploadEvent != nullptr;
}


bool D3D12GraphicsEngine::InitCopyQueue() {
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    if ( FAILED( device->CreateCommandQueue( &queueDesc, IID_PPV_ARGS( m_CopyQueue.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( FAILED( device->CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( m_CopyFence.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    m_CopyFenceEvent = CreateEvent( nullptr, FALSE, FALSE, nullptr );
    return m_CopyFenceEvent != nullptr;
}


void D3D12GraphicsEngine::ReleaseCompletedCopyResources( UINT64 fenceValue ) {
    // Caller holds m_CopyQueueMutex. Pending batches are pushed in ascending fence order, so the front
    // is always the oldest — stop at the first not-yet-completed batch.
    while ( !m_PendingCopyReleases.empty() ) {
        auto& pending = m_PendingCopyReleases.front();
        if ( pending.FenceValue > fenceValue ) break;
        // Staging buffers are done being read: drop the one-off ones, but recycle the command
        // allocator+list for reuse by the next batch (their copies have completed, so Reset is now
        // legal) — and likewise hand the pooled staging chunks back instead of freeing them, which is
        // what keeps steady-state uploads at zero CreateResource calls.
        if ( pending.CopyAllocator && pending.CopyCommandList ) {
            m_FreeCopyCmdObjects.push_back( CopyCmdObjects{
                std::move( pending.CopyAllocator ), std::move( pending.CopyCommandList ) } );
        }
        for ( auto& chunk : pending.StagingChunks ) {
            chunk.Offset = 0;
            m_FreeStagingChunks.push_back( std::move( chunk ) );
        }
        m_PendingCopyReleases.pop_front(); // O(1) popped release, zero memory shifts
    }
}


void D3D12GraphicsEngine::WaitForCopyFence( UINT64 fenceValue ) {
    if ( !m_CopyFence || !m_CopyFenceEvent ) return;
    if ( m_CopyFence->GetCompletedValue() >= fenceValue ) return;
    m_CopyFence->SetEventOnCompletion( fenceValue, m_CopyFenceEvent );
    WaitForSingleObject( m_CopyFenceEvent, INFINITE );
}


void D3D12GraphicsEngine::TransitionTextureToSRVOnDirectQueue( ID3D12Resource* texture ) {
    if ( !texture || !m_Device.GetDevice() ) return;

    // Use the active frame's existing command list instead of creating temporary allocators & command lists
    if ( m_FrameOpen && m_CmdList ) {
        m_CmdList->TransitionBarrier( texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
        return;
    }

    // Fallback if called outside frame boundaries: execute asynchronously on direct queue WITHOUT CPU blocking
    ID3D12Device* device = m_Device.GetDevice();
    ComPtr<ID3D12CommandAllocator> transitionAllocator;
    ComPtr<ID3D12GraphicsCommandList> transitionCmdList;

    if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS( transitionAllocator.ReleaseAndGetAddressOf() ) ) ) )
        return;
    if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        transitionAllocator.Get(), nullptr, IID_PPV_ARGS( transitionCmdList.ReleaseAndGetAddressOf() ) ) ) )
        return;

    // Bare ID3D12GraphicsCommandList (not the D3D12CmdList wrapper) -- this transient list is built and thrown
    // away outside the normal per-frame recording path, so it stays on the legacy transition API.
    auto toSRV = TransitionBarrier( texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
    transitionCmdList->ResourceBarrier( 1, &toSRV );
    if ( FAILED( transitionCmdList->Close() ) ) return;

    ID3D12CommandList* lists[] = { transitionCmdList.Get() };
    m_Device.GetDirectQueue()->ExecuteCommandLists( 1, lists );

    const UINT64 waitValue = ++m_UploadFenceValue;
    if ( FAILED( m_Device.GetDirectQueue()->Signal( m_UploadFence.Get(), waitValue ) ) ) return;

    // OPTIMIZATION: Defer deletion to m_PerFrameCleanupItems via fence value instead of CPU blocking with WaitForSingleObject!
    QueueCleanupJob( [allocator = transitionAllocator, list = transitionCmdList]() {
        // Keeps resources alive until current frame fence is reached on GPU
    } );
}


bool D3D12GraphicsEngine::UploadTextureSubresources( ID3D12Resource* dst, const D3D12_SUBRESOURCE_DATA* subresources, UINT numSubresources ) {
    if ( !dst || !subresources || numSubresources == 0 ) return false;
    ID3D12Device* device = m_Device.GetDevice();

    D3D12_RESOURCE_DESC desc = dst->GetDesc();

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts( numSubresources );
    std::vector<UINT>    numRows( numSubresources );
    std::vector<UINT64>  rowSizes( numSubresources );
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints( &desc, 0, numSubresources, 0, layouts.data(), numRows.data(), rowSizes.data(), &totalBytes );

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = totalBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = DefaultUploadHeapType;

	ComPtr<D3D12MA::Allocation> uploadAllocation;
	ComPtr<ID3D12Resource> upload;
	if ( FAILED( m_Allocator->CreateResource(
		&allocDesc,
		&bufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		uploadAllocation.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( upload.ReleaseAndGetAddressOf() ) ) ) ) {
		return false;
	}

	BYTE* mapped = nullptr;
	D3D12_RANGE noRead = { 0, 0 };
	if ( FAILED( upload->Map( 0, &noRead, reinterpret_cast<void**>(&mapped) ) ) )
		return false;

	for ( UINT i = 0; i < numSubresources; ++i ) {
		BYTE* dstSlice = mapped + layouts[i].Offset;
		const BYTE* srcData = reinterpret_cast<const BYTE*>( subresources[i].pData );
		for ( UINT row = 0; row < numRows[i]; ++row ) {
			memcpy( dstSlice + static_cast<SIZE_T>( layouts[i].Footprint.RowPitch ) * row,
				srcData + static_cast<SIZE_T>( subresources[i].RowPitch ) * row,
				static_cast<SIZE_T>( rowSizes[i] ) );
		}
	}
	upload->Unmap( 0, nullptr );

	// Record the copy into the shared, batched copy command list (submitted once at flush). The lock
	// serializes recording into the single list and guards the batch bookkeeping — see the header note.
    std::scoped_lock lock( m_CopyQueueMutex );
	if ( !BeginCopyBatch() )
		return false;

	for ( UINT i = 0; i < numSubresources; ++i ) {
		D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
		dstLoc.pResource = dst;
		dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLoc.SubresourceIndex = i;

		D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
		srcLoc.pResource = upload.Get();
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLoc.PlacedFootprint = layouts[i];

		m_CopyBatchList->CopyTextureRegion( &dstLoc, 0, 0, 0, &srcLoc, nullptr );
	}

	// Keep the staging buffer alive until the batch's copies complete (moved to the pending list at flush).
	m_CopyBatchUploadAllocs.push_back( std::move( uploadAllocation ) );
	m_CopyBatchUploadResources.push_back( std::move( upload ) );
	m_CopyBatchDestResources.emplace_back( dst );   // and the destination — see PendingCopyRelease::DestResources
	m_CopyBatchBytes += totalBytes;

	// Bound VA/memory growth during a long world-load burst that never Presents: flush mid-burst once
	// the accumulated staging crosses the threshold. Still one submit per threshold-worth, not per texture.
	if ( m_CopyBatchBytes >= kCopyBatchFlushThresholdBytes )
		FlushTextureUploadsLocked();

	ReleaseCompletedCopyResources( m_CopyFence->GetCompletedValue() );
	return true;
}


bool D3D12GraphicsEngine::AcquireStagingSpaceLocked( UINT64 size, UINT64 alignment,
	ID3D12Resource** outResource, UINT64* outOffset, uint8_t** outCpuPtr ) {
	// Caller holds m_CopyQueueMutex.
	if ( size > kStagingChunkSize ) return false;   // too big to pool — dedicated resource instead
	if ( alignment == 0 ) alignment = 1;

	auto tryFit = [&]( StagingChunk& chunk ) {
		const UINT64 aligned = ( chunk.Offset + alignment - 1 ) & ~( alignment - 1 );
		if ( aligned + size > chunk.Capacity ) return false;
		*outResource = chunk.Resource.Get();
		*outOffset = aligned;
		*outCpuPtr = chunk.MappedPtr + aligned;
		chunk.Offset = aligned + size;
		return true;
	};

	// Chunks already charged to this batch. Walking back-to-front finds the most recently used one
	// first; the list is capped at kMaxStagingChunks so this stays trivial.
	for ( auto it = m_CopyBatchStagingChunks.rbegin(); it != m_CopyBatchStagingChunks.rend(); ++it ) {
		if ( tryFit( *it ) ) return true;
	}

	// Take an idle chunk (its copies have completed — see ReleaseCompletedCopyResources).
	if ( !m_FreeStagingChunks.empty() ) {
		StagingChunk chunk = std::move( m_FreeStagingChunks.back() );
		m_FreeStagingChunks.pop_back();
		chunk.Offset = 0;
		m_CopyBatchStagingChunks.push_back( std::move( chunk ) );
		return tryFit( m_CopyBatchStagingChunks.back() );
	}

	// Grow the pool, up to the VA cap. This is the only CreateResource on the pooled path and it
	// happens a handful of times for the whole process lifetime.
	if ( m_LiveStagingChunks >= kMaxStagingChunks ) return false;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = kStagingChunkSize;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = DefaultUploadHeapType;

	StagingChunk chunk;
	if ( FAILED( m_Allocator->CreateResource( &allocDesc, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		chunk.Allocation.ReleaseAndGetAddressOf(), IID_PPV_ARGS( chunk.Resource.ReleaseAndGetAddressOf() ) ) ) ) {
		return false;
	}

	D3D12_RANGE noRead = { 0, 0 };
	void* mapped = nullptr;
	if ( FAILED( chunk.Resource->Map( 0, &noRead, &mapped ) ) ) {
		return false;
	}
	chunk.MappedPtr = static_cast<uint8_t*>( mapped );
	chunk.Capacity = kStagingChunkSize;
	chunk.Offset = 0;
	chunk.Resource->SetPrivateData( WKPDID_D3DDebugObjectName, 13, "StagingChunk" );

	++m_LiveStagingChunks;
	m_CopyBatchStagingChunks.push_back( std::move( chunk ) );
	return tryFit( m_CopyBatchStagingChunks.back() );
}


bool D3D12GraphicsEngine::UploadBufferData( ID3D12Resource* dst, UINT64 dstOffset, const void* srcData, UINT64 sizeInBytes ) {
	if ( !dst || !srcData || sizeInBytes == 0 ) return false;

	// Record into the shared batched copy list (submitted once at flush) — same rationale as
	// UploadTextureSubresources: no per-call CreateCommandList / ExecuteCommandLists / cross-queue Wait.
	// Fast path: suballocate from the persistently-mapped staging pool. The memcpy runs under the lock
	// because the space belongs to the shared pool, but a pooled upload is a mesh's VB/IB — small, and
	// far cheaper than the CreateResource it replaces.
	{
		std::unique_lock<std::mutex> lock( m_CopyQueueMutex );
		if ( !BeginCopyBatch() )
			return false;

		ID3D12Resource* stagingResource = nullptr;
		UINT64 stagingOffset = 0;
		uint8_t* stagingCpu = nullptr;

		// 16-byte alignment: CopyBufferRegion imposes none, this is purely so the memcpy lands aligned.
		if ( AcquireStagingSpaceLocked( sizeInBytes, 16, &stagingResource, &stagingOffset, &stagingCpu ) ) {
			memcpy( stagingCpu, srcData, sizeInBytes );
			m_CopyBatchList->CopyBufferRegion( dst, dstOffset, stagingResource, stagingOffset, sizeInBytes );

			m_CopyBatchDestResources.emplace_back( dst );   // see PendingCopyRelease::DestResources
			m_CopyBatchBytes += sizeInBytes;
			if ( m_CopyBatchBytes >= kCopyBatchFlushThresholdBytes )
				FlushTextureUploadsLocked();

			ReleaseCompletedCopyResources( m_CopyFence->GetCompletedValue() );
			return true;
		}
	}

	// Pool can't serve it: the upload is bigger than a chunk (world mesh, large wrapped mesh) or the
	// pool is at its VA cap. Dedicated staging resource, freed once the batch's fence completes. Built
	// with the lock RELEASED — this is exactly the big-allocation/big-memcpy case, and blocking every
	// other uploader on it is what the batching was introduced to avoid.
	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = sizeInBytes;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = DefaultUploadHeapType;

	ComPtr<D3D12MA::Allocation> uploadAllocation;
	ComPtr<ID3D12Resource> upload;
	if ( FAILED( m_Allocator->CreateResource(
		&allocDesc,
		&bufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		uploadAllocation.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( upload.ReleaseAndGetAddressOf() ) ) ) ) {
		return false;
	}

	BYTE* mapped = nullptr;
	D3D12_RANGE noRead = { 0, 0 };
	if ( FAILED( upload->Map( 0, &noRead, reinterpret_cast<void**>(&mapped) ) ) )
		return false;
	memcpy( mapped, srcData, sizeInBytes );
	upload->Unmap( 0, nullptr );

	std::lock_guard<std::mutex> lock( m_CopyQueueMutex );
	if ( !BeginCopyBatch() )   // may be a different batch than the one probed above; harmless
		return false;

	m_CopyBatchList->CopyBufferRegion( dst, dstOffset, upload.Get(), 0, sizeInBytes );

	m_CopyBatchUploadAllocs.push_back( std::move( uploadAllocation ) );
	m_CopyBatchUploadResources.push_back( std::move( upload ) );
	m_CopyBatchDestResources.emplace_back( dst );   // see PendingCopyRelease::DestResources
	m_CopyBatchBytes += sizeInBytes;

	if ( m_CopyBatchBytes >= kCopyBatchFlushThresholdBytes )
		FlushTextureUploadsLocked();

	ReleaseCompletedCopyResources( m_CopyFence->GetCompletedValue() );
	return true;
}


bool D3D12GraphicsEngine::BeginCopyBatch() {
	if ( m_CopyBatchOpen ) return true;
	ID3D12Device* device = m_Device.GetDevice();
	if ( !device ) return false;

	// Recycle a completed (allocator,list) pair if one is available, else create one.
	if ( !m_FreeCopyCmdObjects.empty() ) {
		m_CopyBatchAllocator = std::move( m_FreeCopyCmdObjects.back().Allocator );
		m_CopyBatchList = std::move( m_FreeCopyCmdObjects.back().List );
		m_FreeCopyCmdObjects.pop_back();
		if ( FAILED( m_CopyBatchAllocator->Reset() ) ) return false;         // safe: its copies completed (fence-gated recycle)
		if ( FAILED( m_CopyBatchList->Reset( m_CopyBatchAllocator.Get(), nullptr ) ) ) return false;
	} else {
		if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_COPY,
			IID_PPV_ARGS( m_CopyBatchAllocator.ReleaseAndGetAddressOf() ) ) ) )
			return false;
		if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_COPY,
			m_CopyBatchAllocator.Get(), nullptr, IID_PPV_ARGS( m_CopyBatchList.ReleaseAndGetAddressOf() ) ) ) )
			return false;
		// CreateCommandList returns the list already open for recording.
	}
	m_CopyBatchOpen = true;
	m_CopyBatchBytes = 0;
	return true;
}


void D3D12GraphicsEngine::FlushTextureUploadsLocked() {
	if ( !m_CopyBatchOpen ) return;
	m_CopyBatchOpen = false;

	if ( FAILED( m_CopyBatchList->Close() ) ) {
		// Drop the batch; its staging buffers free with the ComPtr vectors, cmd objects are discarded.
		// The batch was never submitted, so no GPU is reading the pooled chunks — reuse them right away.
		m_CopyBatchUploadAllocs.clear();
		m_CopyBatchUploadResources.clear();
		m_CopyBatchDestResources.clear();
		for ( auto& chunk : m_CopyBatchStagingChunks ) {
			chunk.Offset = 0;
			m_FreeStagingChunks.push_back( std::move( chunk ) );
		}
		m_CopyBatchStagingChunks.clear();
		m_CopyBatchBytes = 0;
		return;
	}

	ID3D12CommandList* lists[] = { m_CopyBatchList.Get() };
	m_CopyQueue->ExecuteCommandLists( 1, lists );

	const UINT64 fenceValue = ++m_CopyFenceValue;
	m_CopyQueue->Signal( m_CopyFence.Get(), fenceValue );

	// ONE cross-queue GPU wait for the whole batch: the direct (render) queue won't sample any of
	// these textures until the batch's copies complete. Replaces the old per-texture render stall.
	m_Device.GetDirectQueue()->Wait( m_CopyFence.Get(), fenceValue );

	PendingCopyRelease pending;
	pending.FenceValue = fenceValue;
	pending.UploadAllocations = std::move( m_CopyBatchUploadAllocs );
	pending.UploadResources = std::move( m_CopyBatchUploadResources );
	pending.DestResources = std::move( m_CopyBatchDestResources );
	pending.StagingChunks = std::move( m_CopyBatchStagingChunks );
	pending.CopyAllocator = std::move( m_CopyBatchAllocator );
	pending.CopyCommandList = std::move( m_CopyBatchList );
	m_PendingCopyReleases.push_back( std::move( pending ) );

	m_CopyBatchUploadAllocs.clear();
	m_CopyBatchUploadResources.clear();
	m_CopyBatchDestResources.clear();
	m_CopyBatchStagingChunks.clear();
	m_CopyBatchBytes = 0;
}


void D3D12GraphicsEngine::FlushTextureUploads() {
	std::lock_guard<std::mutex> lock( m_CopyQueueMutex );
	FlushTextureUploadsLocked();
}


bool D3D12GraphicsEngine::CreateSrvHeap() {
	ID3D12Device* device = m_Device.GetDevice();
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	desc.NumDescriptors = kSrvHeapCapacity;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if ( FAILED( device->CreateDescriptorHeap( &desc, IID_PPV_ARGS( m_SrvHeap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_SrvDescriptorSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
	m_SrvHeapCapacity = kSrvHeapCapacity;
	m_SrvAllocated = 0;
	return true;
}


UINT D3D12GraphicsEngine::AllocateSrvSlot() {
	std::lock_guard<std::mutex> lock( m_SrvHeapMutex );

	// Try to reuse a freed slot first
	if ( !m_FreeSrvSlots.empty() ) {
		UINT slot = m_FreeSrvSlots.back();
		m_FreeSrvSlots.pop_back();
		return slot;
	}

	// Fall back to bump allocation
	if ( m_SrvAllocated >= m_SrvHeapCapacity ) {
		LogWarn() << "D3D12: SRV heap exhausted (" << m_SrvHeapCapacity << " descriptors).";
		return UINT_MAX;
	}
	return m_SrvAllocated++;
}


void D3D12GraphicsEngine::FreeSrvSlot( UINT slot ) {
	if ( slot == UINT_MAX
		|| slot == m_WhiteTexture->GetSrvSlot()
		|| slot == m_BlackTexture->GetSrvSlot()
		) {
		return;
	}
	// Every default-ORM step is permanent, not just the one currently selected — the player can move the
	// roughness slider back at any time, and a freed slot would already have been handed to a game texture.
	for ( const auto& orm : m_DefaultOrmTextures ) {
		if ( orm && slot == orm->GetSrvSlot() ) return;
	}

	ID3D12Device* device = m_Device.GetDevice();

	std::lock_guard<std::mutex> lock( m_SrvHeapMutex );

	// Nullify the descriptor to prevent pointing to dead memory
	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = GetSrvCpuHandleLocked( slot );

	D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
	nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	nullDesc.Texture2D.MipLevels = 1;

	// Bind white texture to free slot.

	// Writing a null resource view to this descriptor slot safely clears it
	device->CreateShaderResourceView( m_WhiteTexture->GetResource(), &nullDesc, cpuHandle );

	m_FreeSrvSlots.push_back( slot );
}


D3D12_CPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvCpuHandleLocked( UINT slot ) const {
	// Caller holds m_SrvHeapMutex.
	if ( std::ranges::contains( m_FreeSrvSlots, slot ) ) {
		// Ensure invalid slots provide some texture instead of breaking
		return GetSrvCpuHandleLocked( m_BlackTexture->GetSrvSlot() );
	}

	D3D12_CPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(slot) * m_SrvDescriptorSize;
	return h;
}


D3D12_GPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvGpuHandleLocked( UINT slot ) const {
	// Caller holds m_SrvHeapMutex.
	if ( std::ranges::contains( m_FreeSrvSlots, slot ) ) {
		// Ensure invalid slots provide some texture instead of breaking
		return GetSrvGpuHandleLocked( m_BlackTexture->GetSrvSlot() );
	}

	D3D12_GPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetGPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<UINT64>(slot) * m_SrvDescriptorSize;
	return h;
}


D3D12_CPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvCpuHandle( UINT slot ) const {
	std::lock_guard<std::mutex> lock( m_SrvHeapMutex );
	return GetSrvCpuHandleLocked( slot );
}


D3D12_GPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvGpuHandle( UINT slot ) const {
	std::lock_guard<std::mutex> lock( m_SrvHeapMutex );
	return GetSrvGpuHandleLocked( slot );
}


bool D3D12GraphicsEngine::CreateShadowConstantBuffer() {
    // The per-frame-in-flight shadow CB every lit pass binds (b3 on the world/VOB root sig). Small and written
    // once per frame, so a persistently-mapped UPLOAD buffer per frame context — no ring offset needed.
    //
    // 512, not 256: THREE owners write three DISJOINT byte ranges of the same buffer, so none clobbers another.
    // [0, kWetnessCbOffset)  D3D12ShadowMap::Prepare       — cascade view-projs + sun dir/color/strength + texels
    // [kWetnessCbOffset, ..) UploadWetnessConstants        — scene wetness (needs the rain-shadow camera first)
    // [kAoReprojCbOffset,..) UNUSED HOLE                   — was the AO-mask reprojection block; see the header
    // [kSkyIblCbOffset,  ..) UploadSkyIblConstants         — sky-IBL cube indices + intensity
    // Each writer static_asserts its own block size against these offsets; keep them in sync with the HLSL
    // ShadowCB declaration.
    D3D12MA::ALLOCATION_DESC uploadAlloc = {};
    uploadAlloc.HeapType = DefaultUploadHeapType;

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = 512;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_Allocator->CreateResource( &uploadAlloc, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_ShadowCBAlloc[i].ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_ShadowCB[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_ShadowCB[i]->SetName( L"ShadowSamplingCB" );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( m_ShadowCB[i]->Map( 0, &noRead, &mapped ) ) ) return false;
        m_ShadowCBMapped[i] = static_cast<uint8_t*>( mapped );
        m_ShadowCBGpu[i] = m_ShadowCB[i]->GetGPUVirtualAddress();
    }
    return true;
}


bool D3D12GraphicsEngine::CreateWhiteTexture() {
	CreateTexture( m_WhiteTexture );
	const uint32_t white = 0xFFFFFFFFu;
	if ( XR_SUCCESS != m_WhiteTexture->Init( INT2( 1, 1 ), GfxTexture::ETextureFormat::TF_R8G8B8A8, 1, &white, "WhiteFallbackTexture" ) ) {
		return false;
	}

	CreateTexture( m_BlackTexture );
	const uint32_t black = 0xFF000000;
	if ( XR_SUCCESS != m_BlackTexture->Init( INT2( 1, 1 ), GfxTexture::ETextureFormat::TF_R8G8B8A8, 1, &black, "BlackFallbackTexture" ) ) {
		return false;
	}

	// One default-ORM texture per selectable roughness step. R = AO (1), G = roughness, B = metallic (0),
	// laid out as a full ORM so EncodeOrmSlot's format 0 reads it correctly. Nine 1x1 textures cost nine
	// SRV slots and 36 bytes of VRAM, which is why this is a table rather than one texture we rewrite:
	// the slot then never moves, so nothing that baked it into a frame's draw commands can go stale.
	for ( int i = 0; i < DefaultRoughness::kNumSteps; ++i ) {
		CreateTexture( m_DefaultOrmTextures[i] );
		const float roughness = DefaultRoughness::ForStep( i );
		const uint32_t g = static_cast<uint32_t>( roughness * 255.0f + 0.5f ) & 0xFFu;
		const uint32_t orm = 0xFF000000u | ( g << 8 ) | 0xFFu;   // ABGR in memory: R=255 G=rough B=0 A=255
		char name[64];
		snprintf( name, sizeof( name ), "DefaultOrmTexture(1,%.2f,0)", roughness );
		if ( XR_SUCCESS != m_DefaultOrmTextures[i]->Init( INT2( 1, 1 ), GfxTexture::ETextureFormat::TF_R8G8B8A8, 1, &orm, name ) ) {
			return false;
		}
	}
	RefreshDefaultOrmSlot();

	return true;
}


void D3D12GraphicsEngine::RefreshDefaultOrmSlot() {
	const int step = DefaultRoughness::StepFor( Engine::GAPI->GetRendererState().RendererSettings.DefaultMaterialRoughness );
	if ( m_DefaultOrmTextures[step] ) {
		m_DefaultOrmSrvSlot = m_DefaultOrmTextures[step]->GetSrvSlot();
	}
}


bool D3D12GraphicsEngine::LoadDistortionTexture() {
	// Same file D3D11GraphicsEngine::DistortionTexture loads (D3D11GraphicsEngine.cpp:819). Used by
	// BuildWorldDrawCommands as a wet-ground normalmap stand-in for materials that have none while it's
	// raining — non-fatal: that fallback just stays unavailable (dry-normal-less look) if this fails.
	m_DistortionTexture = std::make_unique<D3D12Texture>();
	const std::string path = Engine::GAPI->GetStartDirectory() + "\\system\\GD3D11\\textures\\distortion2.dds";
	if ( m_DistortionTexture->Init( path ) != XR_SUCCESS || !m_DistortionTexture->HasSRV() ) {
		LogWarn() << "D3D12: rain-distortion texture not found/loadable (" << path << ") — wet-ground fallback for non-normalmapped materials disabled.";
		m_DistortionTexture.reset();
		return false;
	}
	return true;
}


bool D3D12GraphicsEngine::CreateDepthBuffer( INT2 size ) {
	if ( size.x <= 0 || size.y <= 0 ) return false;
	ID3D12Device* device = m_Device.GetDevice();

	// DSV heap — created once, reused across resizes. Slot 0 = this scene depth buffer; slot 1 = the
	// native-resolution preview depth (see GetPreviewDsv), only populated while the render scale is != 100%.
	if ( !m_DsvHeap ) {
		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
		dsvHeapDesc.NumDescriptors = 2;
		dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if ( FAILED( device->CreateDescriptorHeap( &dsvHeapDesc, IID_PPV_ARGS( m_DsvHeap.ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_DsvDescriptorSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );
	}

	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = static_cast<UINT64>(size.x);
	dd.Height = static_cast<UINT>(size.y);
	dd.DepthOrArraySize = 1;
	dd.MipLevels = 1;
	dd.Format = DXGI_FORMAT_R32_TYPELESS;   // typeless so the same texels serve a D32_FLOAT DSV and an R32_FLOAT SRV
	dd.SampleDesc.Count = 1;
	dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	// Reversed-Z: the world clears depth to 0.0, so make that the optimized clear value.
	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DXGI_FORMAT_D32_FLOAT;
	clear.DepthStencil.Depth = 0.0f;

	// Born in DEPTH_WRITE. Now also SRV-readable: DispatchLightCulling brackets a NON_PIXEL_SHADER_RESOURCE
	// read of it (per-tile far-Z) and transitions back to DEPTH_WRITE, so it is DEPTH_WRITE at every other point.
	if ( FAILED( D3D12ResourceCreate::CreateTexture( m_Allocator.Get(), allocDesc, dd,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, m_DepthBufferAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_DepthBuffer.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: failed to create the depth buffer (" << size.x << "x" << size.y << ").";
		return false;
	}
	m_DepthBuffer->SetName( L"DepthBuffer(D32)" );

	D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
	dsv.Format = DXGI_FORMAT_D32_FLOAT;   // typeless resource viewed as depth here
	dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	device->CreateDepthStencilView( m_DepthBuffer.Get(), &dsv, m_DsvHeap->GetCPUDescriptorHandleForHeapStart() );
	m_CmdList.InvalidateRenderTargets();   // descriptor rewritten in place — see D3D12StateCache.h

	// R32_FLOAT SRV of the same texels for the light cull's per-tile far-Z read. Slot allocated once; the view is
	// (re)created every call so it always points at the current (post-resize) resource.
	if ( m_DepthSrvSlot == UINT_MAX ) {
		m_DepthSrvSlot = AllocateSrvSlot();
		if ( m_DepthSrvSlot == UINT_MAX ) return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC dsrv = {};
	dsrv.Format = DXGI_FORMAT_R32_FLOAT;   // typeless resource viewed as a single float channel
	dsrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	dsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	dsrv.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView( m_DepthBuffer.Get(), &dsrv, GetSrvCpuHandle( m_DepthSrvSlot ) );

	// Forward+ tile grid storage is resolution-dependent too — (re)build it here so it always tracks the
	// depth buffer's size (called from both init and the resize path). GPU is idle at both call sites.
	if ( !CreateLightCullBuffers( size ) ) return false;

	// Drop the preview depth (it only exists to cover a size mismatch); the next DrawVobSingle rebuilds it
	// if still needed. Every caller has idled the GPU.
	m_PreviewDepthBuffer.Reset();
	m_PreviewDepthAlloc.Reset();
	m_PreviewDepthSize = {};
	m_PreviewDepthFailed = false;
	return true;
}


/** See m_PreviewDepthBuffer. At 100% render scale this is just the scene DSV; otherwise it lazily builds a
    backbuffer-sized depth buffer into DSV heap slot 1 so the preview's RTV and DSV dimensions agree.
    Lazy mid-frame creation is safe for the same reason as the underwater/DoF ones: nothing is destroyed. */
D3D12_CPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetPreviewDsv() {
	const D3D12_CPU_DESCRIPTOR_HANDLE none = {};
	if ( !m_DsvHeap ) return none;

	if ( m_Resolution.x == m_BackbufferResolution.x && m_Resolution.y == m_BackbufferResolution.y ) {
		if ( !m_DepthBuffer ) return none;
		return m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	}

	D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	dsv.ptr += m_DsvDescriptorSize;

	if ( m_PreviewDepthBuffer
		&& m_PreviewDepthSize.x == m_BackbufferResolution.x && m_PreviewDepthSize.y == m_BackbufferResolution.y )
		return dsv;
	if ( m_PreviewDepthFailed ) return none;
	if ( m_BackbufferResolution.x <= 0 || m_BackbufferResolution.y <= 0 ) return none;

	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = static_cast<UINT64>( m_BackbufferResolution.x );
	dd.Height = static_cast<UINT>( m_BackbufferResolution.y );
	dd.DepthOrArraySize = 1;
	dd.MipLevels = 1;
	dd.Format = DXGI_FORMAT_D32_FLOAT;   // depth-only: nothing ever samples this one
	dd.SampleDesc.Count = 1;
	dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DXGI_FORMAT_D32_FLOAT;
	clear.DepthStencil.Depth = 0.0f;   // reversed-Z far, same as the scene depth

	if ( FAILED( D3D12ResourceCreate::CreateTexture( m_Allocator.Get(), allocDesc, dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
		m_PreviewDepthAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_PreviewDepthBuffer.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: failed to create the inventory-preview depth buffer ("
			<< m_BackbufferResolution.x << "x" << m_BackbufferResolution.y << ") — item previews will not render.";
		m_PreviewDepthBuffer.Reset();
		m_PreviewDepthAlloc.Reset();
		m_PreviewDepthFailed = true;
		return none;
	}
	m_PreviewDepthBuffer->SetName( L"PreviewDepthBuffer(D32)" );

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	m_Device.GetDevice()->CreateDepthStencilView( m_PreviewDepthBuffer.Get(), &dsvDesc, dsv );
	m_CmdList.InvalidateRenderTargets();   // descriptor written in place — see D3D12StateCache.h
	m_PreviewDepthSize = m_BackbufferResolution;
	return dsv;
}


bool D3D12GraphicsEngine::CreateSceneColorTarget( INT2 size ) {
	// HDR scene-color render target (Phase 3): the 3D world/VOB/skeletal/water/decal/particle passes render into
	// this R16F target so lighting can exceed 1.0 (bright sun + stacked additive point lights keep their detail
	// instead of clipping to white). ResolveSceneToBackBuffer then tonemaps it into the swapchain. Resolution-
	// sized → (re)created here on init and every resize (RTV heap + SRV slot persist; only the resource + views
	// are rebuilt). DEFAULT-heap GPU memory (64bpp, or 32 when CompressBackBuffer picked R11G11B10 at init),
	// so it barely touches the 32-bit CPU address space.
	if ( size.x <= 0 || size.y <= 0 ) return false;
	ID3D12Device* device = m_Device.GetDevice();
	if ( !device || !m_RtvHeap ) return false;

	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = static_cast<UINT64>(size.x);
	dd.Height = static_cast<UINT>(size.y);
	dd.DepthOrArraySize = 1;
	dd.MipLevels = 1;
	dd.Format = kSceneColorFormat;
	dd.SampleDesc.Count = 1;
	dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	// Born in RENDER_TARGET (the world pass renders straight into it; ResolveSceneToBackBuffer flips it to
	// PIXEL_SHADER_RESOURCE and back next frame). GPU is idle at every call site (init / post-WaitForGpuIdle resize).
	if ( FAILED( D3D12ResourceCreate::CreateTexture( m_Allocator.Get(), allocDesc, dd,
		D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, m_SceneColorAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_SceneColor.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: failed to create the HDR scene-color target (" << size.x << "x" << size.y << ").";
		return false;
	}
	m_SceneColor->SetName( kSceneColorFormat == DXGI_FORMAT_R11G11B10_FLOAT
		? L"SceneColorHDR(R11G11B10)" : L"SceneColorHDR(R16F)" );
	m_SceneColorInPixelState = false;

	// RTV in the extra heap slot (index kBackBufferMax, past the swapchain RTVs - the heap always reserves
	// the max slot count regardless of the actually configured kBackBufferCount, so this offset never moves).
	m_SceneColorRtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
	m_SceneColorRtv.ptr += static_cast<SIZE_T>(kBackBufferMax) * m_RtvDescriptorSize;
	device->CreateRenderTargetView( m_SceneColor.Get(), nullptr, m_SceneColorRtv );
	m_CmdList.InvalidateRenderTargets();

	// SRV for the tonemap resolve (slot allocated once; view re-created each call to point at the current resource).
	if ( m_SceneColorSrvSlot == UINT_MAX ) {
		m_SceneColorSrvSlot = AllocateSrvSlot();
		if ( m_SceneColorSrvSlot == UINT_MAX ) return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = kSceneColorFormat;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView( m_SceneColor.Get(), &srv, GetSrvCpuHandle( m_SceneColorSrvSlot ) );
	return true;
}



void D3D12GraphicsEngine::BindSceneColorTarget() {
	// Make the HDR scene-color target the world pass's render target (+ keep the shared depth buffer). Transitions
	// it back from PIXEL_SHADER_RESOURCE (last frame's resolve left it there) to RENDER_TARGET when needed.
	if ( !m_SceneColor || !m_CmdList ) return;
	if ( m_SceneColorInPixelState ) {
		m_CmdList->TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
		m_SceneColorInPixelState = false;
	}
	const bool haveDepth = m_DepthBuffer && m_DsvHeap;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
	if ( haveDepth ) dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, haveDepth ? &dsv : nullptr );
	m_ColorTargetIsHDR = true;
}


/** Peak luminance the HDR highlight roll-off targets, in nits. Monitor-reported metadata is frequently wrong
	(a 400-nit edge-lit panel claiming 1499, or reporting nothing at all), so the user override wins whenever
	HDR_AutoMaxBrightness is off, and the auto path falls back to a conservative 1000 when DXGI reports nothing. */
float D3D12GraphicsEngine::GetHdrMaxBrightnessNits() const {
	const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
	float nits = s.HDR_AutoMaxBrightness ? m_HdrMonitorMaxNits : s.HDR_MaxBrightness;
	if ( !( nits > 0.0f ) ) nits = 1000.0f;
	return std::clamp( nits, 100.0f, 10000.0f );
}

/** Nit level that 1.0 in the display buffer maps to — the brightness of the HUD/menus and of diffuse-white
	surfaces. Clamped below the peak so there is always at least a little highlight headroom left; a paper white
	at or above the ceiling would leave the roll-off nothing to work with and read as a flat, clipped image. */
float D3D12GraphicsEngine::GetHdrPaperWhiteNits() const {
	const float paperWhite = std::clamp( Engine::GAPI->GetRendererState().RendererSettings.HDR_PaperWhite, 50.0f, 1000.0f );
	return std::min( paperWhite, GetHdrMaxBrightnessNits() / 1.25f );
}


ID3D12Resource* D3D12GraphicsEngine::GetDisplayTarget() const {
	return m_HdrDisplay ? m_HdrDisplay.Get() : m_BackBuffers[m_FrameIndex].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetDisplayRtv() const {
	if ( m_HdrDisplay ) return m_HdrDisplayRtv;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>( m_FrameIndex ) * m_RtvDescriptorSize;
	return rtv;
}


void D3D12GraphicsEngine::ResolveSceneToBackBuffer() {
	// Tonemap the finished HDR scene onto the display target, then leave it bound so the 2D UI (drawn after
	// OnStartWorldRendering) composites on top. If HDR is unavailable, no-op (nothing to show).
	if ( !m_SceneColor || !m_Pipelines.Tonemap.PSO || !m_Pipelines.Tonemap.RootSig || !m_CmdList ) return;
	DX_ZONE( m_CmdList.Get(), "Tonemap resolve (HDR->display)" );

	if ( !m_SceneColorInPixelState ) {
		m_CmdList->TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		m_SceneColorInPixelState = true;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetDisplayRtv();
	m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, nullptr );   // no depth for the fullscreen resolve
	m_ColorTargetIsHDR = false;

	// NATIVE viewport over the render-res scene texture: the fullscreen triangle's uv spans [0,1] of
	// m_SceneColor and s0 is a bilinear clamp sampler, so this draw IS the render-scale up/downscale.
	// When FSR 3 ran it already produced a display-res image (GetTonemapSourceSrvSlot) and this becomes 1:1.
	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_BackbufferResolution.x), static_cast<float>(m_BackbufferResolution.y), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, m_BackbufferResolution.x, m_BackbufferResolution.y };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->SetPipelineState( m_Pipelines.Tonemap.PSO.Get() );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.Tonemap.RootSig.Get() );
	m_CmdList->SetGraphicsRootDescriptorTable( 0, GetSrvGpuHandle( GetTonemapSourceSrvSlot() ) );
	// applyDisplayCorrection=false: brightness/contrast are applied later, in Present, so they cover the UI too.
	const TonemapRootConstants tonemapConsts = MakeTonemapConstants( m_HdrOutputActive, false );
	// Count MUST match the AddConstants( 0, ... ) in D3D12PipelineState::CreateTonemap.
	static_assert( kTonemapRootConstantCount == 12, "Tonemap root constant count changed - update CreateTonemap" );
	m_CmdList->SetGraphicsRoot32BitConstants( 1, kTonemapRootConstantCount, &tonemapConsts, 0 );
	m_CmdList->SetGraphicsRootShaderResourceView( 2, m_LumAdaptedBuffer->GetGPUVirtualAddress() );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	m_CmdList->IASetVertexBuffers( 0, 0, nullptr );
	m_CmdList->DrawInstanced( 3, 1, 0, 0 );
}


/** Fills Tonemap.hlsl's b0. `hdrOutput` selects the display path (highlight roll-off, extended-sRGB out)
	over the SDR operators; GetBackbufferData passes false so screenshots always capture the SDR image.
	`applyDisplayCorrection` folds brightness/contrast into the resolve — only the screenshot path wants that;
	on screen ApplyDisplayGammaCorrection does it later so Gothic's 2D UI/HUD is corrected too, as in D3D11. */
D3D12GraphicsEngine::TonemapRootConstants D3D12GraphicsEngine::MakeTonemapConstants( bool hdrOutput, bool applyDisplayCorrection ) const {
	const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
	TonemapRootConstants c = {};
	c.Exposure = s.Exposure > 0.0f ? s.Exposure : 1.0f;
	// Dynamic-exposure knobs. NOT RendererSettings.HDRMiddleGray (0.8) — that one is calibrated for D3D11's own
	// tonemap curves and overexposes this path by ~4.4x; AutoExposureMiddleGray defaults to the photographic 0.18.
	// Strength/Min/Max are what stop a dark interior from being normalized up to daylight brightness.
	c.MiddleGray = s.AutoExposureMiddleGray > 0.0f ? s.AutoExposureMiddleGray : 0.18f;
	c.AutoExposureStrength = std::clamp( s.AutoExposureStrength, 0.0f, 1.0f );
	c.AutoExposureMin = std::max( s.AutoExposureMin, 0.0f );
	c.AutoExposureMax = std::max( s.AutoExposureMax, c.AutoExposureMin );
	c.LumWhite = s.HDRLumWhite;
	c.ToneMapMode = static_cast<UINT>( s.HDRToneMap );
	c.HdrOutput = hdrOutput ? 1u : 0u;
	// Headroom expressed in paper-white units: how many times brighter than diffuse white the panel can go.
	// GetHdrPaperWhiteNits keeps this >= 1.25 by construction.
	c.DisplayHeadroom = hdrOutput ? ( GetHdrMaxBrightnessNits() / GetHdrPaperWhiteNits() ) : 0.0f;
	// 1.0/1.0 is the identity the shader branches around; only the screenshot path asks for the real values.
	c.Brightness = applyDisplayCorrection ? std::max( Engine::GAPI->GetBrightnessValue(), 0.0f ) : 1.0f;
	c.Gamma = applyDisplayCorrection ? std::max( Engine::GAPI->GetGammaValue(), 0.01f ) : 1.0f;
	return c;
}


/** Best-effort query of whether we can scan out real HDR, run once at Init (before any PSO bakes
	DisplayFormat). Walks the adapter's outputs looking for one Windows currently has in HDR mode and keeps its
	luminance metadata. The window doesn't exist yet, so this can only pick "some HDR output on this adapter";
	CreateSwapChain refreshes the metadata from the output the window actually landed on. */
void D3D12GraphicsEngine::DetectHdrOutputCapability() {
	m_HdrOutputActive = false;
	m_HdrEncodePQ = false;
	m_HdrMonitorMaxNits = m_HdrMonitorMinNits = m_HdrMonitorMaxFullFrameNits = 0.0f;
	m_Pipelines.DisplayFormat = kBackBufferFormat;

	if ( !Engine::GAPI->GetRendererState().RendererSettings.HDR_Monitor ) return;

	IDXGIAdapter1* adapter = m_Device.GetAdapter();
	if ( !adapter ) return;

	ComPtr<IDXGIOutput> output;
	for ( UINT i = 0; adapter->EnumOutputs( i, output.ReleaseAndGetAddressOf() ) != DXGI_ERROR_NOT_FOUND; ++i ) {
		ComPtr<IDXGIOutput6> output6;
		if ( FAILED( output.As( &output6 ) ) ) continue;   // pre-Windows-10-1703: no HDR metadata at all

		DXGI_OUTPUT_DESC1 desc = {};
		if ( FAILED( output6->GetDesc1( &desc ) ) ) continue;
		if ( desc.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ) continue;   // display not in HDR mode

		m_HdrOutputActive = true;
		m_HdrMonitorMaxNits = desc.MaxLuminance;
		m_HdrMonitorMinNits = desc.MinLuminance;
		m_HdrMonitorMaxFullFrameNits = desc.MaxFullFrameLuminance;
		break;
	}

	if ( !m_HdrOutputActive ) {
		LogInfo() << "D3D12: HDR output requested but no HDR-enabled display was found on this adapter; using SDR.";
		return;
	}

	m_Pipelines.DisplayFormat = kHdrDisplayFormat;
	LogInfo() << "D3D12: HDR output enabled. Monitor reports " << m_HdrMonitorMaxNits << " nits peak, "
		<< m_HdrMonitorMaxFullFrameNits << " nits full-frame, " << m_HdrMonitorMinNits << " nits black.";
}


/** Puts the (already created) swapchain into the HDR10 colour space and publishes the mastering metadata.
	Called after every swapchain (re)creation. R10G10B10A2 + G2084/P2020 is the classic HDR10 setup: the
	application produces the PQ signal itself, which is exactly what HdrEncode.hlsl does.
	On refusal m_HdrEncodePQ stays false and the encode pass degrades to an SDR passthrough — the FP16 display
	buffer and every PSO built for it are already committed at that point, so there is no going back to the
	direct-to-swapchain path mid-run, but the frame still presents correctly. */
void D3D12GraphicsEngine::ApplySwapChainColorSpace() {
	m_HdrEncodePQ = false;
	if ( !m_HdrOutputActive || !m_SwapChain ) return;

	// Refresh the luminance metadata from the output the window actually ended up on — on a multi-monitor
	// setup that need not be the one Init found, and the settings UI reports these numbers to the player.
	ComPtr<IDXGIOutput> output;
	if ( SUCCEEDED( m_SwapChain->GetContainingOutput( output.GetAddressOf() ) ) ) {
		ComPtr<IDXGIOutput6> output6;
		DXGI_OUTPUT_DESC1 desc = {};
		if ( SUCCEEDED( output.As( &output6 ) ) && SUCCEEDED( output6->GetDesc1( &desc ) )
			&& desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ) {
			m_HdrMonitorMaxNits = desc.MaxLuminance;
			m_HdrMonitorMinNits = desc.MinLuminance;
			m_HdrMonitorMaxFullFrameNits = desc.MaxFullFrameLuminance;
		}
	}

	constexpr DXGI_COLOR_SPACE_TYPE kHdr10 = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
	UINT support = 0;
	if ( FAILED( m_SwapChain->CheckColorSpaceSupport( kHdr10, &support ) )
		|| !( support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT )
		|| FAILED( m_SwapChain->SetColorSpace1( kHdr10 ) ) ) {
		LogWarn() << "D3D12: the swapchain refused the HDR10 colour space; presenting SDR from the HDR display buffer.";
		return;
	}
	m_HdrEncodePQ = true;

	// Mastering metadata (optional, best-effort): tells the display what range we actually authored for, so its
	// own tone mapping doesn't second-guess a signal we already rolled off. Chromaticities are Rec.709 (our
	// working primaries) in DXGI's 1/50000 units; luminance is the effective peak, i.e. what the user's slider
	// or the monitor's own report put us at. IDXGISwapChain4 is Windows 10 1703+; skip quietly without it.
	ComPtr<IDXGISwapChain4> swapChain4;
	if ( FAILED( m_SwapChain.As( &swapChain4 ) ) ) return;

	const float maxNits = GetHdrMaxBrightnessNits();
	DXGI_HDR_METADATA_HDR10 meta = {};
	meta.RedPrimary[0] = 32000;   meta.RedPrimary[1] = 16500;    // 0.640, 0.330
	meta.GreenPrimary[0] = 15000; meta.GreenPrimary[1] = 30000;  // 0.300, 0.600
	meta.BluePrimary[0] = 7500;   meta.BluePrimary[1] = 3000;    // 0.150, 0.060
	meta.WhitePoint[0] = 15635;   meta.WhitePoint[1] = 16450;    // D65
	meta.MaxMasteringLuminance = static_cast<UINT>( maxNits * 10000.0f );   // 1/10000 nit units
	meta.MinMasteringLuminance = static_cast<UINT>( std::max( 0.0f, m_HdrMonitorMinNits ) * 10000.0f );
	meta.MaxContentLightLevel = static_cast<UINT16>( std::min( 65535.0f, maxNits ) );
	meta.MaxFrameAverageLightLevel = static_cast<UINT16>( std::min( 65535.0f, GetHdrPaperWhiteNits() ) );
	swapChain4->SetHDRMetaData( DXGI_HDR_METADATA_TYPE_HDR10, sizeof( meta ), &meta );
}


/** (Re)builds the FP16 composite target the whole post-scene chain renders into while HDR output is active.
	Rests in RENDER_TARGET for the entire frame, exactly like the swapchain backbuffer it stands in for, so the
	copy-out/copy-back dances in RenderSMAA/RenderSharpen work against it unchanged. Cleared every frame by
	OnBeginFrame, so nothing carries over between frames. */
bool D3D12GraphicsEngine::CreateHdrDisplayTarget( INT2 size ) {
	m_HdrDisplay.Reset();
	m_HdrDisplayAlloc.Reset();
	if ( !m_HdrOutputActive ) return true;   // SDR: display target IS the swapchain, nothing to build
	if ( size.x < 4 || size.y < 4 ) return false;

	ID3D12Device* device = m_Device.GetDevice();
	if ( !device || !m_RtvHeap ) return false;

	D3D12MA::ALLOCATION_DESC heapDefault = {};
	heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = static_cast<UINT64>( size.x );
	dd.Height = static_cast<UINT>( size.y );
	dd.DepthOrArraySize = 1;
	dd.MipLevels = 1;
	dd.Format = kHdrDisplayFormat;
	dd.SampleDesc.Count = 1;
	dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = kHdrDisplayFormat;
	memcpy( clearValue.Color, m_ClearColor, sizeof( clearValue.Color ) );

	if ( FAILED( D3D12ResourceCreate::CreateTexture( m_Allocator.Get(), heapDefault, dd, D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
		m_HdrDisplayAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_HdrDisplay.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: failed to create the HDR display composite target (" << size.x << "x" << size.y << ").";
		return false;
	}
	m_HdrDisplay->SetName( L"HdrDisplayComposite" );

	// RTV heap slot kBackBufferMax+3 (after the swapchain RTVs, the scene-color RTV and SMAA's edge/blend).
	m_HdrDisplayRtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
	m_HdrDisplayRtv.ptr += static_cast<SIZE_T>( kBackBufferMax + 3 ) * m_RtvDescriptorSize;
	device->CreateRenderTargetView( m_HdrDisplay.Get(), nullptr, m_HdrDisplayRtv );
	m_CmdList.InvalidateRenderTargets();

	if ( m_HdrDisplaySrvSlot == UINT_MAX ) m_HdrDisplaySrvSlot = AllocateSrvSlot();
	if ( m_HdrDisplaySrvSlot == UINT_MAX ) return false;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	srv.Format = kHdrDisplayFormat;
	device->CreateShaderResourceView( m_HdrDisplay.Get(), &srv, GetSrvCpuHandle( m_HdrDisplaySrvSlot ) );
	return true;
}


/** The single ST.2084 conversion of the frame: display buffer -> swapchain. Runs at the very end of Present,
	after the 2D UI and the ImGui overlay have composited into the display buffer, with the backbuffer still in
	RENDER_TARGET (OnBeginFrame transitioned it) and before the transition to PRESENT. */
void D3D12GraphicsEngine::EncodeHdrDisplayToBackBuffer() {
	if ( !m_HdrOutputActive || !m_HdrDisplay || !m_CmdList ) return;
	if ( !m_Pipelines.HdrEncode.PSO || !m_Pipelines.HdrEncode.RootSig || m_HdrDisplaySrvSlot == UINT_MAX ) return;
	DX_ZONE( m_CmdList.Get(), "HDR scanout encode (display->swapchain)" );

	m_CmdList->TransitionBarrier( m_HdrDisplay.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );

	D3D12_CPU_DESCRIPTOR_HANDLE backRtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
	backRtv.ptr += static_cast<SIZE_T>( m_FrameIndex ) * m_RtvDescriptorSize;
	m_CmdList->OMSetRenderTargets( 1, &backRtv, FALSE, nullptr );

	// Display->swapchain, both native (the tonemap resolve already upscaled).
	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_BackbufferResolution.x ), static_cast<float>( m_BackbufferResolution.y ), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, m_BackbufferResolution.x, m_BackbufferResolution.y };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	// The PS fetches the display buffer via SM6.6 ResourceDescriptorHeap, so the shader-visible heap must be
	// bound. It normally still is from OnBeginFrame, but this runs after the ImGui overlay, which sets heaps
	// of its own — cheap insurance for the one pass that would otherwise present garbage.
	if ( m_SrvHeap ) {
		ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
		m_CmdList->SetDescriptorHeaps( 1, heaps );
	}

	m_CmdList->SetPipelineState( m_Pipelines.HdrEncode.PSO.Get() );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.HdrEncode.RootSig.Get() );

	struct { UINT SrcIndex; float PaperWhiteNits; UINT EncodeMode; float _pad; } consts = {
		m_HdrDisplaySrvSlot, GetHdrPaperWhiteNits(), m_HdrEncodePQ ? 1u : 0u, 0.0f };
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 4, &consts, 0 );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	m_CmdList->IASetVertexBuffers( 0, 0, nullptr );
	m_CmdList->DrawInstanced( 3, 1, 0, 0 );

	// Back to the resting state the next frame's OnBeginFrame expects to clear.
	m_CmdList->TransitionBarrier( m_HdrDisplay.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
}


XRESULT D3D12GraphicsEngine::SetWindow( HWND hWnd ) {
    LogInfo() << "D3D12: Creating swapchain";
    CommonSetWindow( hWnd ); // stores m_OutputWindow, force-activates, clips cursor, hides mouse cursor

    // Use the configured target resolution (NOT the current client rect — Gothic creates its window
    // tiny, so GetClientRect here would size the swapchain to a few pixels). Mirrors the D3D11 path,
    // which takes RendererSettings.LoadedResolution. OnResize sizes the OS window + builds the swapchain.
    INT2 size = Engine::GAPI->GetRendererState().RendererSettings.LoadedResolution;
    if ( size.x <= 0 || size.y <= 0 ) {
        RECT rc = {};
        GetClientRect( hWnd, &rc );
        size = INT2( std::max<int>( 800, rc.right - rc.left ), std::max<int>( 600, rc.bottom - rc.top ) );
    }

    m_NewResolution = size;
    return OnResize( size );
}


void D3D12GraphicsEngine::QueueSrvResourceForRelease( UINT slot, Microsoft::WRL::ComPtr<ID3D12Resource> resource )
{
    QueueCleanupJob( [this, slot, resource = std::move(resource)]() {
        // Recycle the descriptor slot safely
        this->FreeSrvSlot( slot );

        // The ComPtr 'resource' capture naturally dies here, releasing the ID3D12Resource;
    } );
}


void D3D12GraphicsEngine::QueueCleanupJob( std::move_only_function<void()> callback )
{
    if ( callback == nullptr ) return;
    // Hold a reference until the frame this was queued in has retired on the GPU (MoveToNextFrame drains
    // by ordinal, after its fence wait), then drop it. The capture keeps the resource alive until every
    // command list that could reference it has finished.
    // The ordinal is read INSIDE the lock: reading it first and locking after is exactly the race that
    // let a worker's job land in an already-drained bucket — see m_PendingCleanupJobs' declaration.
    std::lock_guard<std::mutex> lock( m_CleanupMutex );
    m_PendingCleanupJobs.push_back( PendingCleanupJob{ m_CleanupFrameOrdinal, std::move( callback ) } );
}


void D3D12GraphicsEngine::QueueResourceForRelease( Microsoft::WRL::ComPtr<ID3D12Resource> resource )
{
    if ( !resource ) return;
    // No slot to recycle — just hold a reference until this frame index comes back around (after its
    // fence is waited on in MoveToNextFrame), then drop it. The capture keeps the resource alive until
    // every command list that could reference it has finished on the GPU.
    QueueCleanupJob( [resource = std::move(resource)]() {} );
}


void D3D12GraphicsEngine::QueueAllocationForRelease( Microsoft::WRL::ComPtr<D3D12MA::Allocation> value )
{
    if ( !value ) return;
    // No slot to recycle — just hold a reference until this frame index comes back around (after its
    // fence is waited on in MoveToNextFrame), then drop it. The capture keeps the resource alive until
    // every command list that could reference it has finished on the GPU.
    QueueCleanupJob( [resource = std::move(value)]() { } );
}


/** Sizes the actual OS window to the target resolution and tells Gothic about the mode so its 2D
    UI coordinate space matches. Mirrors the windowed / borderless branch of the D3D11 backend. */
void D3D12GraphicsEngine::ResizeOutputWindow( INT2 size ) {
    if ( !m_OutputWindow || size.x <= 0 || size.y <= 0 ) return;

#ifndef BUILD_SPACER
    RECT desktopRect = {};
    GetClientRect( GetDesktopWindow(), &desktopRect );
    const bool borderless = ( size.x >= desktopRect.right && size.y >= desktopRect.bottom );

    if ( borderless ) {
        // Fullscreen-borderless: strip the frame and cover the desktop.
        ApplyWindowStyle( WindowModes::WINDOW_MODE_FULLSCREEN_BORDERLESS, RECT{ 0, 0, desktopRect.right, desktopRect.bottom } );
    } else {
        // Windowed: fixed-size window whose CLIENT area equals the target resolution.
        LONG style = ( WS_OVERLAPPEDWINDOW | WS_VISIBLE ) & ~( WS_MAXIMIZEBOX | WS_THICKFRAME );
        RECT wr = { 0, 0, size.x, size.y };
        AdjustWindowRectEx( &wr, style, FALSE, WS_EX_APPWINDOW );

        RECT cur = {};
        int x = 0, y = 0;
        if ( GetWindowRect( m_OutputWindow, &cur ) ) { x = cur.left; y = cur.top; }
        ApplyWindowStyle( WindowModes::WINDOW_MODE_WINDOWED, RECT{ x, y, x + (wr.right - wr.left), y + (wr.bottom - wr.top) } );
    }

    zCView::SetWindowMode( size.x, size.y, 32 );
    // Inform Gothic of the resolution (drives its virtual UI coordinate space).
    zCView::SetVirtualMode( size.x, size.y, 32 );
    POINT virtualSize = { 8192, 8192 };
    zCViewDraw::GetScreen().SetVirtualSize( virtualSize );
#endif
}


static bool CheckTearingSupport() {
    BOOL allowTearing = FALSE;
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory5)))) {
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    }
    return allowTearing == TRUE;
}


/** Render resolution = backbuffer * ResolutionScalePercent, clamp included — see
    D3D11GraphicsEngine::RecreateBuffers. RendererSettings.Upscaler (FSR 1/3) is ignored: the vendored
    FidelityFX SDK ships only ffx_backend_dx11_x86.lib, so the tonemap resolve is the only upscaler here. */
INT2 D3D12GraphicsEngine::ComputeRenderResolution( INT2 backbufferSize ) {
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( settings.ResolutionScalePercent == 100 ) return backbufferSize;

    settings.ResolutionScalePercent = std::clamp( settings.ResolutionScalePercent, 25, 200 );
    const float scale = static_cast<float>( settings.ResolutionScalePercent ) / 100.0f;
    return INT2{
        std::max( 1, static_cast<int>( static_cast<float>( backbufferSize.x ) * scale ) ),
        std::max( 1, static_cast<int>( static_cast<float>( backbufferSize.y ) * scale ) )
    };
}


/** Mip LOD bias for the material sampler, straight out of D3D11GraphicsEngine::CreateAndBindDefaultSampler:
    log2(render/display), clamped at 0 so supersampling doesn't bias UP and throw away its extra samples. */
float D3D12GraphicsEngine::ComputeMipLodBias( INT2 renderSize, INT2 displaySize ) {
    if ( renderSize.x <= 0 || displaySize.x <= 0 ) return 0.0f;
    const float ratio = static_cast<float>( renderSize.x ) / static_cast<float>( displaySize.x );
    return std::min( 0.0f, std::log2( ratio ) );
}


/** See the header comment. */
INT2 D3D12GraphicsEngine::GetAoTargetResolution( INT2 renderSize ) {
    if ( Engine::GAPI->GetRendererState().RendererSettings.AoResolution != AoResolutionScale::Half )
        return renderSize;
    return INT2{ std::max( 1, renderSize.x / 2 ), std::max( 1, renderSize.y / 2 ) };
}


/** Re-bakes `newBias` into every root signature's static aniso sampler. A static sampler lives in the root
    signature blob and a PSO binds its root signature at creation, so this is a full rebuild through the
    shader hot-reload path (backup + rollback included) — seconds, not milliseconds, hence the debounce in
    ApplyPendingResolutionScale. The caller must have idled the GPU. */
bool D3D12GraphicsEngine::RebakeMipLodBias( float newBias ) {
    const float previousBias = m_AppliedMipLodBias;
    D3D12RootLayout::SetAnisoMipLodBias( newBias );

    D3D12PipelineState backup = m_Pipelines;   // cheap AddRef pass — see ApplyPendingShaderReload
    std::vector<std::string> failedFatal, failedOptional;
    if ( !m_Pipelines.ReloadAll( m_HdrOutputActive, failedFatal, failedOptional ) ) {
        m_Pipelines = backup;
        D3D12RootLayout::SetAnisoMipLodBias( previousBias );
        std::string names;
        for ( const auto& n : failedFatal ) { if ( !names.empty() ) names += ", "; names += n; }
        LogWarn() << "D3D12: could not re-bake the texture mip bias (" << names
            << " failed to rebuild) — keeping the previous pipelines. Textures will look blurrier than they should.";
        return false;
    }
    if ( !failedOptional.empty() ) {
        std::string names;
        for ( const auto& n : failedOptional ) { if ( !names.empty() ) names += ", "; names += n; }
        LogWarn() << "D3D12: mip-bias rebuild finished with degraded passes (" << names << ").";
    }
    m_AppliedMipLodBias = newBias;
    return true;
}


/** Everything sized to the RENDER resolution — i.e. everything the frame touches before the tonemap
    resolve, which is where the up/downscale happens. Only depth + scene color are fatal; the rest follow
    the same non-fatal contract they have on the resize path (their passes guard on the resource existing). */
bool D3D12GraphicsEngine::CreateRenderResolutionTargets( INT2 renderSize ) {
    // Every D3D12RenderGraph-managed transient texture (DoF/god-ray/underwater/SMAA scratch) is sized off
    // m_Resolution or m_BackbufferResolution, both of which are about to change — their names' reserved
    // offsets (D3D12AliasedTextureArena::ReserveNamedRange) must NOT survive into the new resolution, or a
    // name would keep its old, now too-small range and silently corrupt whatever got packed after it. This
    // always runs before CreateDisplayResolutionTargets (every call site below), so one Clear() covers both.
    m_AliasArena.Clear();

    // The FFX context is built for one specific (render, display) size pair and its internal resources are
    // freed immediately by ffxFsr3UpscalerContextDestroy — so it has to go here, on a path whose callers have
    // already idled the GPU, rather than lazily from the frame. EnsureFsr3Ready rebuilds it next frame.
    ReleaseFsr3();
    if ( !CreateDepthBuffer( renderSize ) ) return false;
    if ( !CreateSceneColorTarget( renderSize ) ) return false;
    CreateBloomResources( renderSize );
    const INT2 aoSize = GetAoTargetResolution( renderSize );
    CreateAOResources( aoSize );
    CreateGtaoResources( aoSize );   // must follow CreateAOResources: XeGTAO writes ITS m_AOMask
    m_AoResourceSize = aoSize;
    m_AppliedAoResolution = Engine::GAPI->GetRendererState().RendererSettings.AoResolution;
    CreateMotionResources( renderSize ); // motion-vector + normal G-buffer; prepass falls back to depth-only
    CreateTaaResources( renderSize );    // also drops the history, which any resolution change invalidates
    CreateSsrHistoryResources( renderSize ); // opaque-SSR previous-frame color+depth; see D3D12Ssr.cpp
    // DoF textures are built lazily (~20 MB of VA, off by default), so only re-size them if they exist.
    // Clearing the attempted flag lets a previous failure retry.
    m_DoFCreateAttempted = false;
    if ( m_DoFResourcesReady ) CreateDoFResources( renderSize );
    CreateHiZResources( renderSize );    // without it the GPU VOB cull runs frustum-only (no occlusion)
    // Height-fog/god-ray textures no longer need a resize hook — they're D3D12RenderGraph-managed transients
    // acquired fresh at the current resolution every call (see D3D12Fog.cpp's RenderFogAndGodRays).
    // Water's scene/depth copies no longer need a resize hook — they're D3D12RenderGraph-managed transients
    // acquired fresh at the current resolution every call (see D3D12Water.cpp's DrawWaterSurfaces).
    if ( !CreateLumPartialBuffer( renderSize ) ) {
        // Non-fatal: RenderLuminanceAdapt() guards on this and skips the update, leaving m_LumAdaptedBuffer
        // at its last valid value.
        LogWarn() << "D3D12GraphicsEngine: failed to create the dynamic-exposure partial-sum buffer.";
    }
    m_AppliedResolutionScalePercent = Engine::GAPI->GetRendererState().RendererSettings.ResolutionScalePercent;
    return true;
}


/** Everything sized to the NATIVE backbuffer — the post-tonemap passes. All non-fatal (each guards on its
    own resources); the underwater pair is lazily built, so it is only re-sized here if it is already up. */
void D3D12GraphicsEngine::CreateDisplayResolutionTargets( INT2 displaySize ) {
    ReleaseFsr3();                          // display size is half of the pair the FFX context is built for
    CreateLdrCopyResource( displaySize );   // shared LDR scratch for SMAA/sharpen; both no-op without it
    // SMAA's edges/blend and the underwater blur pair no longer need a resize hook — both are
    // D3D12RenderGraph-managed transients acquired fresh at the current resolution every call (see
    // D3D12PostFX.cpp's RenderSMAA / D3D12Underwater.cpp's DrawUnderwaterEffects).
}


bool D3D12GraphicsEngine::CreateSwapChain( INT2 size ) {
    m_BackbufferResolution = size;
    m_Resolution = ComputeRenderResolution( size );

    m_TearingSupported = CheckTearingSupport();
    
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = static_cast<UINT>( size.x );
    scd.Height = static_cast<UINT>( size.y );
    scd.Format = kBackBufferFormat;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kBackBufferCount;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags = ( m_TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0 )
        | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = m_Device.GetFactory()->CreateSwapChainForHwnd(
        m_Device.GetDirectQueue(), m_OutputWindow, &scd, nullptr, nullptr, swapChain1.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogWarn() << "CreateSwapChainForHwnd failed (0x" << std::hex << hr << ").";
        return false;
    }

    // GD3D11 manages fullscreen itself; disable DXGI's Alt+Enter handling.
    m_Device.GetFactory()->MakeWindowAssociation( m_OutputWindow, DXGI_MWA_NO_ALT_ENTER );

    if ( FAILED( swapChain1.As( &m_SwapChain ) ) ) {
        LogWarn() << "Swapchain does not support IDXGISwapChain3.";
        return false;
    }
    m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

    // Cap the swapchain's own render-ahead queue to N queued frames (kBackBufferCount - 1: kBackBufferCount
    // itself counts the 1 frame currently in flight too), matching the single wait we do per loop iteration
    // (CGameManagerRunLoop_PaceFrame / OnBeginFrame fallback via WaitForFrameLatencyWaitable).
    m_SwapChain->SetMaximumFrameLatency( kBackBufferCount - 1 );
    m_FrameLatencyWaitableObject = m_SwapChain->GetFrameLatencyWaitableObject();
    if ( m_FrameLatencyWaitableObject ) {
        // First wait is satisfied immediately (nothing presented yet) - consume it now so the very
        // first real frame doesn't observe a stale signal.
        WaitForSingleObjectEx( m_FrameLatencyWaitableObject, 1000, TRUE );
    }

    if ( !CreateFrameResources() ) return false;
    if ( !AcquireBackBufferRTVs() ) return false;
    if ( !CreateRenderResolutionTargets( m_Resolution ) ) return false;   // NOT `size` — see below
    ApplySwapChainColorSpace();      // HDR10 colour space + mastering metadata (no-op unless HDR output is active)
    if ( !CreateHdrDisplayTarget( size ) ) {
        // The FP16 composite target is where every display-space PSO renders once HDR is active, so losing it
        // would leave those draws unbound. Degrade to the SDR passthrough encode rather than showing nothing:
        // GetDisplayRtv() falls back to the swapchain, whose format no longer matches those PSOs, so the honest
        // move is to give up on the swapchain entirely and let Init's caller fall back to D3D11.
        LogWarn() << "D3D12GraphicsEngine::CreateSwapChain: failed to create the HDR display target.";
        return false;
    }
    CreateDisplayResolutionTargets( size );   // post-tonemap passes, native size

    m_SwapChainReady = true;

    // Bring up the ImGui overlay on the D3D12 backend (mirrors D3D11's OnResize-time init). ImGui
    // texture SRVs are allocated out of our shader-visible heap via callbacks; drawn each Present().
    if ( Engine::ImGuiHandle && !Engine::ImGuiHandle->Initiated && m_SrvHeap ) {
        // The overlay draws into the display target (m_HdrDisplay when HDR output is up), so it must be
        // built for that RTV format, not the swapchain's. It needs no HDR awareness beyond that: the display
        // buffer holds the same gamma-encoded values it would write to an SDR swapchain.
        Engine::ImGuiHandle->InitD3D12( m_OutputWindow, this, m_Device.GetDevice(),
            m_Device.GetDirectQueue(), kBackBufferCount, m_Pipelines.DisplayFormat, m_SrvHeap.Get() );
    }
    return true;
}


bool D3D12GraphicsEngine::CreateFrameResources() {
    ID3D12Device* device = m_Device.GetDevice();

    // RTV descriptor heap: kBackBufferMax backbuffer slots (reserved at the compile-time max regardless of
    // the actually configured kBackBufferCount, so every fixed offset below stays stable) + 1 for the HDR
    // scene-color target (slot kBackBufferMax) + 2 for the SMAA edge/blend intermediates (slots
    // kBackBufferMax+1 / +2) + 1 for the HDR display composite target (slot kBackBufferMax+3; only
    // populated when real HDR output is active) + 2 for the motion-vector / octahedral-normal G-buffer the
    // depth prepass writes (slots +4 / +5, D3D12Motion.cpp).
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kBackBufferMax + 6;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if ( FAILED( device->CreateDescriptorHeap( &rtvHeapDesc, IID_PPV_ARGS( m_RtvHeap.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_RtvDescriptorSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_RTV );

    // Per-frame command allocators
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS( m_CmdAllocators[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
    }

    // A single command list (created recording, then closed — OnBeginFrame resets it each frame)
    if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_CmdAllocators[m_FrameIndex].Get(), nullptr, IID_PPV_ARGS( m_CmdList.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_CmdList->Close();

    // Frame-sync fence
    if ( FAILED( device->CreateFence( m_FenceValues[m_FrameIndex], D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS( m_Fence.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_FenceValues[m_FrameIndex]++;

    if ( !m_FenceEvent ) {
        m_FenceEvent = CreateEvent( nullptr, FALSE, FALSE, nullptr );
        if ( !m_FenceEvent ) return false;
    }
    return true;
}


bool D3D12GraphicsEngine::AcquireBackBufferRTVs() {
    ID3D12Device* device = m_Device.GetDevice();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_SwapChain->GetBuffer( i, IID_PPV_ARGS( m_BackBuffers[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_BackBuffers[i]->SetName( i == 0 ? L"BackBuffer0" : L"BackBuffer1" );
        device->CreateRenderTargetView( m_BackBuffers[i].Get(), nullptr, rtvHandle );
        m_CmdList.InvalidateRenderTargets();
        rtvHandle.ptr += m_RtvDescriptorSize;
    }
    return true;
}


XRESULT D3D12GraphicsEngine::OnBeginFrame() {
    if ( !m_SwapChainReady ) return XR_SUCCESS;

    if ( !g_MainLoopFramePacingInstalled ) {
        FrameMark;
        WaitForFrameLatencyWaitable();
        FrameLimiterBeginFrame();
    } else if ( Engine::GAPI->IsIngameMenuPaused()) {
        FrameMark;
	}

    PausedFrameLimiterBeginFrame();

    TracyD3D12BeginFrame;

    // Apply a pending TriggerResize() request here — the command list from the previous frame is already
    // Closed+Executed+Presented at this point (no open recording to disrupt), so this is the one place in
    // the frame it's safe to stall the GPU and swap the depth/scene-color/swapchain resources out from under
    // ourselves. Mirrors D3D11GraphicsEngine::OnBeginFrame's `if (NewResolution != Resolution) OnResize(...)`.
    if ( m_NewResolution.x > 0 && m_NewResolution.y > 0
        && ( m_NewResolution.x != m_BackbufferResolution.x || m_NewResolution.y != m_BackbufferResolution.y ) ) {
        OnResize( m_NewResolution );
    }

    // Same spot, same reasoning, for a render-scale change (ImGui slider / ini).
    ApplyPendingResolutionScale();

    // Same spot, same reasoning, for an AO-resolution change (ImGui combo / ini).
    ApplyPendingAoResolutionChange();

    // Same spot, same reasoning, for a pending shader hot-reload request (ReloadShaders only records it —
    // see ApplyPendingShaderReload's comment for the GPU-flush + rollback-on-failure this does).
    ApplyPendingShaderReload();

    // Same spot, same reasoning, for the shadow-map quality setting (mirrors D3D11ShadowMap::PrepareRender's
    // per-frame settings check). Clamp+snap here too so the ImGui slider/ini can't leave an in-between size.
    if ( m_ShadowMap.HasResources() ) {
        auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
        const UINT desiredShadowSize = D3D12ShadowMap::ClampSize( settings.ShadowMapSize );
        if ( desiredShadowSize != m_ShadowMap.GetSize() ) {
            m_ShadowMap.Resize( desiredShadowSize );
        }
        settings.ShadowMapSize = static_cast<int>( m_ShadowMap.GetSize() );
    }

    {
        std::lock_guard<std::mutex> lock( m_CopyQueueMutex );
        ReleaseCompletedCopyResources( m_CopyFence->GetCompletedValue() );
    }

    // Finalize any textures a Gothic resource-manager worker thread finished loading since last frame
    // (MyDirectDrawSurface7::Unlock's worker-thread branch calls UpdateDataDeferred + AddFrameLoadedTexture,
    // but leaves MyDirectDrawSurface7::IsReady false until this runs) — mirrors
    // D3D11GraphicsEngine::OnBeginFrame's identical block. D3D12Texture doesn't use the D3D11-only
    // staging-texture/mip-map deferral lists (GetStagingTextures/GetMipMapGeneration stay empty for this
    // backend — its uploads already go through the thread-safe copy-queue batcher), so only the
    // ready-flag handshake is needed here.
    Engine::GAPI->EnterResourceCriticalSection();
    Engine::GAPI->SetFrameProcessedTexturesReady();
    Engine::GAPI->LeaveResourceCriticalSection();

    HRESULT hr = m_CmdAllocators[m_FrameIndex]->Reset();
    if ( FAILED( hr ) ) {
        WaitForGpuIdle();
        hr = m_CmdAllocators[m_FrameIndex]->Reset();
        if ( FAILED( hr ) ) return XR_FAILED;
    }
    // Goes through the state cache, which drops its shadow with the list state Reset just discarded.
    hr = m_CmdList.Reset( m_CmdAllocators[m_FrameIndex].Get(), nullptr );
    if ( FAILED( hr ) ) return XR_FAILED;
    ResetCpuContextTracker();
    m_CmdList.ResetStats();
    for ( UINT s = 0; s < kShadowRecordSlots; ++s ) m_ShadowCmdLists[s][m_FrameIndex].ResetStats();

    m_CmdList->TransitionBarrier( m_BackBuffers[m_FrameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET );

    // The swapchain stays transitioned to RENDER_TARGET even in HDR mode — EncodeHdrDisplayToBackBuffer
    // writes it at the end of Present — but everything the frame draws goes to the display target.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetDisplayRtv();

    // Bind the display + depth target for the frame. The 3D world pass (OnStartWorldRendering) uses
    // the depth buffer; the 2D/UI PSO has depth disabled, so it draws over the result regardless.
    // A bound DSV must match its RTV's dimensions, so render-res depth + native display target can only be
    // bound together at 100%. The clear below addresses the view directly and is unaffected.
    const bool haveDepth = m_DepthBuffer && m_DsvHeap;
    const bool bindDepth = haveDepth
        && m_Resolution.x == m_BackbufferResolution.x && m_Resolution.y == m_BackbufferResolution.y;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
    if ( haveDepth ) dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();

    m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, bindDepth ? &dsv : nullptr );
    m_ColorTargetIsHDR = false;
    m_CmdList->ClearRenderTargetView( rtv, m_ClearColor, 0, nullptr );
    if ( haveDepth )  // reversed-Z: clear to 0.0
        m_CmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr );

    // Bind the shader-visible SRV heap for this frame's 2D draws (descriptor tables reference it).
    ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
    m_CmdList->SetDescriptorHeaps( 1, heaps );

    // Reset the per-frame 2D vertex ring + VOB instance ring + default the viewport to the full backbuffer.
    m_UIVertexBufferOffset = 0;
    m_UIOverflowLogged = false;
    m_LineVertexBufferOffset = 0;
    m_LineOverflowLogged = false;
    m_FxVertexBufferOffset = 0;
    m_FxOverflowLogged = false;
    m_VobInstanceBufferOffset = 0;
    m_VobInstanceOverflowLogged = false;
    // The shadow ring has no shared cursor to reset (each slot's cursor is local to its UploadVobs call), just
    // the per-slot warn-once flags.
    for ( bool& logged : m_ShadowInstanceOverflowLogged ) logged = false;
    m_SkeletalCBBufferOffset = 0;
    m_SkeletalCBOverflowLogged = false;
    m_ParticleInstanceBufferOffset = 0;
    m_ParticleInstanceOverflowLogged = false;
    m_DecalInstanceBufferOffset = 0;
    m_DecalInstanceOverflowLogged = false;
    m_LightOverflowLogged = false;   // light buffer is rebuilt from 0 each frame in BuildFrameLightBuffer
    // Pick up an ImGui roughness change once, here, so every command builder this frame — including the
    // cascade builds running on worker threads — reads the same slot.
    RefreshDefaultOrmSlot();
    if ( !Engine::GAPI->IsGamePaused() )
        UpdateWindAnimation( m_WindBuffer );   // advances windDir/globalTime; DrawVobsInstanced fills min/maxHeight/playerPos
    m_CurrentTexture = nullptr;
    // NATIVE size: default viewport for the 2D/UI path, which draws onto the already-upscaled display
    // target. Every 3D pass sets its own render-res viewport.
    m_CurrentViewport = { 0.0f, 0.0f, static_cast<float>( m_BackbufferResolution.x ), static_cast<float>( m_BackbufferResolution.y ), 0.0f, 1.0f };
    m_CurrentScissor = { 0, 0, m_BackbufferResolution.x, m_BackbufferResolution.y };

    // Only when the resolution actually changed, like D3D11 does from OnResize. SetVirtualMode ends in
    // zCView::SetMode, which rewrites vid_xdim/ydim + screen->psizex and then RecalcChildsSize/Pos over every
    // open view; running that per frame re-derives layouts that were set in pixels (oCViewDocument sizes the
    // book that way) and ratchets them smaller each frame. First frame still applies it.
    if ( m_AppliedZViewMode.x != m_BackbufferResolution.x || m_AppliedZViewMode.y != m_BackbufferResolution.y ) {
        m_AppliedZViewMode = m_BackbufferResolution;

        zCView::SetWindowMode(
            m_BackbufferResolution.x,
            m_BackbufferResolution.y,
            32 );

        zCView::SetVirtualMode(
            static_cast<int>(m_BackbufferResolution.x),
            static_cast<int>(m_BackbufferResolution.y),
            32 );

        // SetMode leaves the zCViewDraw screen's virtual size derived from pixels; restore the 8192 space the
        // document/page views are authored in (D3D11 OnResize does the same right after its SetVirtualMode).
        POINT virtualSize = { 8192, 8192 };
        zCViewDraw::GetScreen().SetVirtualSize( virtualSize );
    }

    m_FrameOpen = true;
    return XR_SUCCESS;
}


XRESULT D3D12GraphicsEngine::OnEndFrame() {
    if ( !m_SwapChainReady || !m_FrameOpen ) return XR_SUCCESS;
    Present();
    m_FrameOpen = false;
    m_PresentPending = false;
    TracyD3D12CollectHere

    if ( !g_MainLoopFramePacingInstalled ) {
        FrameLimiterEndFrame();
    }
    PausedFrameLimiterEndFrame();
    Engine::GAPI->OnEndFrame();

    return XR_SUCCESS;
}


static const wchar_t* GetOpName( D3D12_AUTO_BREADCRUMB_OP op ) {
    switch ( op ) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return L"SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return L"BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return L"EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return L"DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return L"DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return L"ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return L"Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return L"CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return L"CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return L"CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return L"CopyTiles";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return L"ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return L"ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return L"ClearUnorderedAccessView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return L"ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return L"ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return L"ExecuteBundle";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return L"Present";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return L"BuildRaytracingAccelerationStructure";
    case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO: return L"EmitRaytracingAccelerationStructurePostBuildInfo";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return L"CopyRaytracingAccelerationStructure";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return L"DispatchRays";
    case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEMETACOMMAND: return L"InitializeMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND: return L"ExecuteMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_ESTIMATEMOTION: return L"EstimateMotion";
    case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return L"EnhancedBarrier";
    default: return L"Unknown D3D12 Command";
    }
}


static const wchar_t* SafeWideString( const wchar_t* str ) {
    return str ? str : L"[Unnamed Object]";
}


static const wchar_t* FindCpuRecordedContext( UINT crashIndex ) {
    const wchar_t* lastKnownContext = L"Unknown/Outside Scopes";

    // Look back through what the CPU logged during recording up to the crash point
    for ( UINT i = 0; i <= crashIndex; ++i ) {
        if ( i < g_CpuContextHistory.size() && g_CpuContextHistory[i].pContextText != nullptr ) {
            lastKnownContext = g_CpuContextHistory[i].pContextText;
        }
    }
    return lastKnownContext;
}


static void PrintNode( const D3D12_AUTO_BREADCRUMB_NODE1* node ) {
    if ( !node ) {
        return;
    }

    std::wstring builder{};
    builder.reserve(1024);

    builder.append( L"--- Outstanding Command List GPU Breadcrumbs ---\n" );
    builder.append( L"Command List Debug Name: " ).append( SafeWideString( node->pCommandListDebugNameW ) ).append( L"\n" );
    builder.append( L"Command Queue Debug Name: " ).append( SafeWideString( node->pCommandQueueDebugNameW ) ).append( L"\n" );
    LogInfo() << builder.c_str();

    // Log out the History of GPU Operations recorded
    // Note: pLastBreadcrumbValue points to the number of completed operations.
    // Operations *up to* (*node->pLastBreadcrumbValue) finished. Anything past failed or hung.
    UINT completedOps = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;

    builder.clear();
    builder.append( L"Completed Op Count: " ).append( std::to_wstring( completedOps ) ).append( L" / " ).append( std::to_wstring( node->BreadcrumbCount ) ).append( L"\n" );
    LogInfo() << builder.c_str();

    for ( UINT i = 0; i < node->BreadcrumbCount; ++i ) {
        builder.clear();

        if ( i < completedOps ) {
            builder.append( L" [ok] " );
        } else if ( i == completedOps ) {
            builder.append( L" [ERR] " );
        } else {
            builder.append( L" [ ] " );
        }

        builder.append( L"Op #" ).append( std::to_wstring( i ) ).append( L": " );
        builder.append( GetOpName( node->pCommandHistory[i] ) );

        if ( i == completedOps ) {
            builder.append( L"   <=== !!! HARDWARE HANG DETECTED AT THIS OPERATION !!!" );

            // Pull the exact recorded context step tied directly to this operation cluster!
            const wchar_t* contextAtCrash = FindCpuRecordedContext( completedOps );
            builder.append( L"\n   <=== !!! ACTIVE SCOPE AT TIME OF HARDWARE CRASH: \"" )
                .append( contextAtCrash ).append( L"\" !!!" );
        }

        builder.append( L"\n" );
        LogInfo() << builder.c_str();
    }

    if ( node->pNext ) {
        PrintNode( node->pNext );
    }
#undef PRINT_NODE_FIELD
}


static void DiagnoseErrors(ID3D12Device* device) {
    // Deliberately does NOT read GetPageFaultAllocationOutput: page-fault tracking is only ever turned on
    // under DEBUG_D3D11 (see D3D12Device::Init) and must never be relied on for player-machine diagnostics.
    // Auto-breadcrumbs + breadcrumb context are FORCED_ON unconditionally, so this is always available.
    ComPtr<ID3D12DeviceRemovedExtendedData1> pRemovedExtendedData;
    if ( !device || FAILED( device->QueryInterface( IID_PPV_ARGS( pRemovedExtendedData.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12 DiagnoseErrors: ID3D12DeviceRemovedExtendedData1 unavailable, no breadcrumbs to dump.";
        return;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 output;
    if ( SUCCEEDED( pRemovedExtendedData->GetAutoBreadcrumbsOutput1( &output ) ) ) {
        PrintNode( output.pHeadAutoBreadcrumbNode );
    } else {
        LogWarn() << "D3D12 DiagnoseErrors: GetAutoBreadcrumbsOutput1 failed, no breadcrumbs to dump.";
    }
}


void D3D12GraphicsEngine::HandleDeviceRemoved( HRESULT removedReason, const char* context ) {
    // Best-effort: the device may itself be in a state where this queries nothing, but DiagnoseErrors
    // handles that (logs and returns) rather than crashing here on top of the original failure.
    DiagnoseErrors( m_Device.GetDevice() );

    auto msg = std::format( "D3D12 device removed at {} (reason: 0x{:08X}). See Log.txt for GPU breadcrumbs.",
        context, static_cast<uint32_t>( removedReason ) );
    LogWarn() << msg.c_str();
    MessageBoxA( NULL, msg.c_str(), "GD3D11 (DX12): Device Removed", MB_OK );
    exit( removedReason );
}

XRESULT D3D12GraphicsEngine::Present() {
    if ( !m_SwapChainReady || !m_FrameOpen ) return XR_SUCCESS;

    // Publish this frame's state-cache tally into the shared stats block the ImGui overlay reads
    // ("StateChanges"). Issued = binds that actually reached the driver, across the main list and every
    // shadow-recording list; the filtered count is what the cache saved. Written here, immediately before
    // the overlay draws, because GothicAPI::OnWorldUpdate zeroes RendererInfo at the top of each frame.
    {
        UINT issued = m_CmdList.GetStats().Issued;
        UINT filtered = m_CmdList.GetStats().Filtered;
        for ( UINT s = 0; s < kShadowRecordSlots; ++s ) {
            issued += m_ShadowCmdLists[s][m_FrameIndex].GetStats().Issued;
            filtered += m_ShadowCmdLists[s][m_FrameIndex].GetStats().Filtered;
        }
        GothicRendererInfo& info = Engine::GAPI->GetRendererState().RendererInfo;
        info.StateChanges = issued;
        info.FramePipelineStates = filtered;   // "SC_PipelineStates" row — redundant binds dropped
    }

    // Brightness/contrast over the finished image (scene + Gothic's 2D UI/HUD), before the ImGui overlay —
    // the same point D3D11 applies it. No-op at the default 1.0/1.0.
    ApplyDisplayGammaCorrection();

    // Draw the ImGui overlay last, on top of the 2D UI, while the backbuffer is still a render target.
    // The SRV heap is bound (OnBeginFrame); re-bind the RTV defensively in case a draw changed it.
    if ( Engine::ImGuiHandle && Engine::ImGuiHandle->Initiated ) {
        TracyD3D12ZoneCGX( m_CmdList.Get(), "ImGui" );
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetDisplayRtv();
        m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, nullptr );
        Engine::ImGuiHandle->RenderLoopD3D12( m_CmdList.Get() );
        // imgui_impl_dx12 records on the RAW list: its own PSO, root signature, descriptor heaps, RTV,
        // viewport, scissor, topology, blend factor and vertex/index buffers. The state cache cannot see
        // any of it, so drop the whole shadow — this is the one place in the backend that goes behind it.
        m_CmdList.InvalidateAll();
    }

    // With real HDR output the frame so far lives in the FP16 display buffer; this is the one pass that turns
    // it into the ST.2084 signal the swapchain scans out. No-op in SDR (the display target IS the backbuffer).
    EncodeHdrDisplayToBackBuffer();

    m_CmdList->TransitionBarrier( m_BackBuffers[m_FrameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT );

    // Submit any batched texture/buffer uploads accumulated this frame and insert the single
    // copy->direct cross-queue wait BEFORE the frame's graphics execute, so everything sampled below
    // is ready. Cheap no-op when nothing was cached in.
    FlushTextureUploads();

    if ( FAILED( m_CmdList->Close() ) ) return XR_FAILED;

    ID3D12CommandList* lists[] = { m_CmdList.Get() };
    m_Device.GetDirectQueue()->ExecuteCommandLists( 1, lists );

    const bool vsync = Engine::GAPI->GetRendererState().RendererSettings.EnableVSync;
    const UINT syncInterval = vsync ? 1 : 0;

    UINT presentFlags = 0;
    if (!vsync && m_TearingSupported) {
        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    
    HRESULT hr = m_SwapChain->Present( syncInterval, presentFlags );
    if ( FAILED( hr ) ) {
        auto r = static_cast<uint32_t>(hr);
        if ( hr == DXGI_ERROR_DEVICE_REMOVED) {
            auto removedReason = m_Device.GetDevice()->GetDeviceRemovedReason();
            HandleDeviceRemoved( removedReason, "Present" ); // [[noreturn]]
        } else {
            auto msg = std::format( "D3D12 Present failed (0x{:08X})", r );
            LogWarn() << "D3D12 Present failed (0x" << std::hex << r << ").";
            MessageBoxA( NULL, msg.c_str(), "GD3D11 (DX12): Error", MB_OK);
        }
        exit( hr );
        return XR_FAILED;
    }

    MoveToNextFrame();
    return XR_SUCCESS;
}


bool D3D12GraphicsEngine::WaitOnFrameFence( UINT64 value, const char* site ) {
    if ( !m_Fence || !m_FenceEvent ) return false;
    if ( m_Fence->GetCompletedValue() >= value ) return true;
    if ( FAILED( m_Fence->SetEventOnCompletion( value, m_FenceEvent ) ) ) return false;

    // The registration stays armed across a timed-out wait, so re-waiting on the same event is legal and
    // loses no wakeup. See the header for why this must never be an INFINITE wait.
    for ( UINT round = 1; ; ++round ) {
        if ( WaitForSingleObject( m_FenceEvent, kFenceWaitTimeoutMs ) == WAIT_OBJECT_0 )
            return true;

        const HRESULT removedReason = m_Device.GetDevice()
            ? m_Device.GetDevice()->GetDeviceRemovedReason() : E_FAIL;

        UINT64 copyPending = 0, copyDone = 0;
        {
            std::lock_guard<std::mutex> lock( m_CopyQueueMutex );
            copyPending = m_CopyFenceValue;
            copyDone = m_CopyFence ? m_CopyFence->GetCompletedValue() : 0;
        }

        LogWarn() << "D3D12: " << site << " has been waiting " << ( round * kFenceWaitTimeoutMs ) / 1000
            << "s for frame fence " << value << " (completed " << m_Fence->GetCompletedValue()
            << "). Copy fence: " << copyDone << "/" << copyPending
            << ( copyDone < copyPending ? " (direct queue is blocked on the copy queue)" : "" )
            << ". Device removed reason: 0x" << std::hex << static_cast<uint32_t>( removedReason ) << std::dec << ".";

        if ( FAILED( removedReason ) ) {
            // The fence can no longer advance — waiting more only extends the freeze. Diagnose + terminate
            // here rather than returning false: every caller of WaitOnFrameFence ignores that return value
            // and would otherwise carry on resetting allocators/lists against a dead device.
            LogWarn() << "D3D12: giving up on frame fence " << value << " — the device is gone.";
            HandleDeviceRemoved( removedReason, site ); // [[noreturn]]
        }
    }
}


void D3D12GraphicsEngine::MoveToNextFrame() {
    const UINT64 currentFenceValue = m_FenceValues[m_FrameIndex];

    // Close the ordinal for the frame we are about to submit BEFORE its Signal: from here on any
    // QueueCleanupJob (render thread or worker) belongs to the next frame. Doing it after the Signal
    // would let a job that is really the next frame's be tagged with a fence that is already going down.
    uint64_t submittedOrdinal = 0;
    {
        std::lock_guard<std::mutex> lock( m_CleanupMutex );
        submittedOrdinal = m_CleanupFrameOrdinal++;
    }

    m_Device.GetDirectQueue()->Signal( m_Fence.Get(), currentFenceValue );

    m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

    WaitOnFrameFence( m_FenceValues[m_FrameIndex], "MoveToNextFrame" );
    m_FenceValues[m_FrameIndex] = currentFenceValue + 1;

    // The fence we just waited on is the one the frame that last used this back-buffer index signalled,
    // i.e. ordinal (submitted - (kBackBufferCount-1)). Every job queued in that frame or earlier is now
    // safe to run; anything newer stays queued.
    const uint64_t retiredOrdinal = ( submittedOrdinal >= kBackBufferCount )
        ? submittedOrdinal - ( kBackBufferCount - 1 ) : 0;

    // Move the due jobs out under the lock, then run them unlocked — a job (e.g.
    // QueueSrvResourceForRelease's) can itself lock other mutexes (m_SrvHeapMutex), and a worker thread
    // may be concurrently pushing into m_PendingCleanupJobs via QueueCleanupJob.
    std::vector<std::move_only_function<void()>> jobs;
    {
        std::lock_guard<std::mutex> lock( m_CleanupMutex );
        // Ordinals are assigned under this same lock, so the deque is (near-)sorted; a racing worker can
        // only ever file a LOWER ordinal behind a higher one, which just costs that job one more frame.
        while ( !m_PendingCleanupJobs.empty() && m_PendingCleanupJobs.front().FrameOrdinal <= retiredOrdinal ) {
            jobs.emplace_back( std::move( m_PendingCleanupJobs.front().Job ) );
            m_PendingCleanupJobs.pop_front();
        }
    }
    for ( auto& cleanupCallback : jobs ) {
        if ( cleanupCallback ) cleanupCallback(); // Calls FreeSrvSlot() and drops the captured ComPtrs
    }

    m_TexturePool.GiveTick();
}


void D3D12GraphicsEngine::WaitForGpuIdle() {
    if ( !m_Fence || !m_Device.GetDirectQueue() ) return;

    // Submit any still-open upload batch first, so its copies are actually queued before we wait on
    // the copy fence below (an un-flushed batch has recorded copies that were never signaled).
    FlushTextureUploads();

    if ( m_CopyFence && m_CopyFenceEvent ) {
        const UINT64 copyFenceValue = m_CopyFenceValue;
        if ( copyFenceValue > 0 && m_CopyFence->GetCompletedValue() < copyFenceValue ) {
            m_CopyFence->SetEventOnCompletion( copyFenceValue, m_CopyFenceEvent );
            WaitForSingleObject( m_CopyFenceEvent, INFINITE );
        }
    }

    // Calculate a "one-off" future value beyond all active frames.
    // This avoids colliding with any m_FenceValues currently in flight.
    UINT64 completedValue = m_Fence->GetCompletedValue();
    UINT64 idleValue = completedValue + 1;

    // Scan all active frames to ensure we choose a value strictly greater 
    // than any pending fence signals.
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( m_FenceValues[i] >= idleValue ) {
            idleValue = m_FenceValues[i] + 1;
        }
    }

    // Queue the signal command on the GPU timeline.
    // Because GPU execution is sequential, this milestone is only reached 
    // when ALL work previously queued has finished.
    if ( FAILED( m_Device.GetDirectQueue()->Signal( m_Fence.Get(), idleValue ) ) ) return;

    // Perform a CPU wait using a transient local event.
    if ( m_Fence->GetCompletedValue() < idleValue ) {
        // Create an anonymous, auto-reset event
        HANDLE eventHandle = CreateEventEx( nullptr, nullptr, 0, EVENT_ALL_ACCESS );
        if ( eventHandle ) {
            m_Fence->SetEventOnCompletion( idleValue, eventHandle );
            WaitForSingleObject( eventHandle, INFINITE );
            CloseHandle( eventHandle );
        }
    }

    // Update our CPU-side trackers so they know the GPU is completely caught up.
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        m_FenceValues[i] = idleValue;
    }
}


void D3D12GraphicsEngine::FlushCommandListSync() {
    // Closes + submits whatever is currently recorded in m_CmdList and blocks until the GPU has consumed
    // it, then Resets the SAME per-frame allocator/list so recording can continue. This is NOT the normal
    // once-per-frame Close/Execute in Present() (no PRESENT transition, no MoveToNextFrame/frame-index
    // advance) — it exists solely for GetBackbufferData, which must synchronously read pixels back mid-
    // frame (Gothic's savegame-thumbnail Lock() fires before this frame's own Present).
    if ( !m_CmdList || !m_Fence || !m_Device.GetDirectQueue() ) return;

    // Ensure any batched uploads recorded before this mid-frame sync are submitted + waited-on, so the
    // pixels read back here reflect textures cached in this frame.
    FlushTextureUploads();

    if ( FAILED( m_CmdList->Close() ) ) return;

    ID3D12CommandList* lists[] = { m_CmdList.Get() };
    m_Device.GetDirectQueue()->ExecuteCommandLists( 1, lists );

    const UINT64 waitValue = ++m_FenceValues[m_FrameIndex];
    if ( SUCCEEDED( m_Device.GetDirectQueue()->Signal( m_Fence.Get(), waitValue ) ) ) {
        WaitOnFrameFence( waitValue, "FlushCommandListSync" );
    }

    m_CmdAllocators[m_FrameIndex]->Reset();
    m_CmdList->Reset( m_CmdAllocators[m_FrameIndex].Get(), nullptr );
}


bool D3D12GraphicsEngine::CreateShadowRecordCommandLists() {
    // Allocator + list pair per (recording slot x frame-in-flight) for the deferred shadow-recording path. Two
    // hard D3D12 rules drive the 2D array: an allocator can back only ONE currently-recording list (so slots that
    // record concurrently need one each), and it may not be Reset while the GPU still consumes a list recorded
    // from it (so each frame-in-flight needs its own). Total = kShadowRecordSlots * kBackBufferCount allocators
    // (the CSM cascades plus the point-cube and rain-shadowmap passes), which is a handful of small VA
    // reservations — acceptable even under the 32-bit budget.
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    for ( UINT c = 0; c < kShadowRecordSlots; ++c ) {
        for ( UINT i = 0; i < kBackBufferCount; ++i ) {
            if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS( m_ShadowCmdAllocators[c][i].ReleaseAndGetAddressOf() ) ) ) ) {
                LogWarn() << "D3D12: failed to create a shadow command allocator — deferred shadow recording disabled.";
                return false;
            }
            if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_ShadowCmdAllocators[c][i].Get(), nullptr,
                IID_PPV_ARGS( m_ShadowCmdLists[c][i].ReleaseAndGetAddressOf() ) ) ) ) {
                LogWarn() << "D3D12: failed to create a shadow command list — deferred shadow recording disabled.";
                return false;
            }
            // CreateCommandList returns the list already open; close it so BeginShadowRecording's Reset is symmetric.
            m_ShadowCmdLists[c][i]->Close();
        }
    }
    m_ShadowCmdListsReady = true;
    return true;
}


void D3D12GraphicsEngine::SubmitRecordedCommandsAndReopen() {
    // Closes + submits m_CmdList and immediately reopens it on the SAME frame allocator. Unlike
    // FlushCommandListSync there is NO fence wait and NO allocator Reset: resetting the allocator would pull
    // the memory out from under the list we just submitted, and waiting would defeat the point. Recording new
    // commands into an allocator whose previously-closed list is still in flight is explicitly legal — only one
    // list at a time may RECORD from it.
    //
    // Everything the command list itself carries as state (descriptor heaps, render targets, viewport, PSO,
    // root signature) is lost across Reset; the caller is responsible for re-establishing what it needs. The
    // shader-visible SRV heap is re-bound here because literally every subsequent pass needs it.
    if ( !m_CmdList || !m_Device.GetDirectQueue() ) return;

    // Present() normally inserts the frame's single copy->direct cross-queue wait immediately before the one
    // graphics execute. Submitting graphics work EARLIER than that would let it sample textures whose copy-queue
    // upload hasn't landed, so flush here too (a no-op when nothing was cached in since the last flush) — the
    // wait then precedes both this batch and the cascade lists that follow it.
    FlushTextureUploads();

    if ( FAILED( m_CmdList->Close() ) ) return;
    ID3D12CommandList* lists[] = { m_CmdList.Get() };
    m_Device.GetDirectQueue()->ExecuteCommandLists( 1, lists );

    if ( FAILED( m_CmdList->Reset( m_CmdAllocators[m_FrameIndex].Get(), nullptr ) ) ) return;
    ResetCpuContextTracker();

    if ( m_SrvHeap ) {
        ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
        m_CmdList->SetDescriptorHeaps( 1, heaps );
    }
}


void D3D12GraphicsEngine::RestoreFrameRenderTarget() {
    // Mirrors the RTV/viewport/heap portion of OnBeginFrame's tail (NOT the per-frame ring-offset resets —
    // those must stay untouched, this runs mid-frame after rings may already have been consumed).
    if ( !m_CmdList || !m_RtvHeap ) return;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetDisplayRtv();

    // Post-resolve, so the RTV is native-sized and the render-res scene depth only matches at 100%. The 2D/UI
    // PSOs are DSVFormat UNKNOWN anyway, and DrawVobSingle binds its own matching DSV.
    const bool haveDepth = m_DepthBuffer && m_DsvHeap
        && m_Resolution.x == m_BackbufferResolution.x && m_Resolution.y == m_BackbufferResolution.y;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
    if ( haveDepth ) dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();

    m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, haveDepth ? &dsv : nullptr );
    m_ColorTargetIsHDR = false;

    if ( m_SrvHeap ) {
        ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
        m_CmdList->SetDescriptorHeaps( 1, heaps );
    }

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_BackbufferResolution.x ), static_cast<float>( m_BackbufferResolution.y ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, m_BackbufferResolution.x, m_BackbufferResolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CurrentViewport = vp;
    m_CurrentScissor = sc;
    m_CurrentTexture = nullptr;
}


/** Unpacks one R10G10B10A2_UNORM texel (the swapchain/tonemap-target format) into BGRA8 — the 32bpp
    layout MyDirectDrawSurface7::Lock's DDLOCK_READONLY path hands to Gothic (masks 0x00FF0000/0x0000FF00/
    0x000000FF for R/G/B, alpha unused). 10->8 bit is a rounded scale; plenty for a save thumbnail. */
static void UnpackR10G10B10A2ToBGRA8( uint32_t packed, byte* dstBGRA ) {
    const uint32_t r10 = packed & 0x3FFu;
    const uint32_t g10 = (packed >> 10) & 0x3FFu;
    const uint32_t b10 = (packed >> 20) & 0x3FFu;
    dstBGRA[0] = static_cast<byte>( (b10 * 255u + 511u) / 1023u );
    dstBGRA[1] = static_cast<byte>( (g10 * 255u + 511u) / 1023u );
    dstBGRA[2] = static_cast<byte>( (r10 * 255u + 511u) / 1023u );
    dstBGRA[3] = 255;
}


/** Returns the data of the backbuffer (savegame thumbnail / screenshot). See D3D11GraphicsEngine's
    counterpart for the calling convention: MyDirectDrawSurface7::Lock calls OnStartWorldRendering() right
    before this to force a fresh world render, then reads *data as a top-down 32bpp BGRA8 buffer. */
void D3D12GraphicsEngine::GetBackbufferData( bool thumbnail, byte** data, INT2& buffersize, int& pixelsize ) {
    *data = nullptr;
    pixelsize = 4;
    // Native: the re-tonemap below upscales out of m_SceneColor for free, like ResolveSceneToBackBuffer.
    buffersize = thumbnail ? INT2( 256, 256 ) : m_BackbufferResolution;

    if ( !m_CmdList || !m_SwapChainReady || !m_SceneColor || !m_Pipelines.TonemapCapturePSO || !m_Pipelines.Tonemap.RootSig ) {
        LogInfo() << (thumbnail ? "Thumbnail failed. D3D12 backend not ready" : "GetBackbufferData failed. D3D12 backend not ready");
        return;
    }

    ID3D12Device* device = m_Device.GetDevice();

    // The world + tonemap-resolve commands recorded by the OnStartWorldRendering() call the caller just
    // made are still sitting unexecuted in m_CmdList — nothing has actually landed on the GPU yet. Flush
    // + block (D3D12 has no immediate-context Flush() like D3D11's GetContext()->Flush()), then keep
    // recording afterwards so the rest of this frame's 2D UI + Present continue on the same list.
    FlushCommandListSync();

    // Re-tonemap the (now GPU-resident) HDR scene into a fresh render target sized to what the caller
    // asked for. We deliberately do NOT copy the real swapchain backbuffer: it's DXGI_FORMAT_R10G10B10A2_
    // UNORM (kBackBufferFormat) which the Tonemap PSO is baked for, but Gothic's Lock() consumer expects a
    // simple 32bpp BGRA8 buffer, and a thumbnail additionally needs downscaling to 256x256 — both are
    // exactly what one more tonemap draw at a smaller viewport gives us for free (mirrors D3D11's
    // GetBackbufferData drawing HDRBackBuffer through PS_PFX_GammaCorrectInv into a differently-sized RT).
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = static_cast<UINT64>( buffersize.x );
    td.Height = static_cast<UINT>( buffersize.y );
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = kBackBufferFormat;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = kBackBufferFormat;

    Microsoft::WRL::ComPtr<D3D12MA::Allocation> captureAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource> captureTex;
    if ( FAILED( D3D12ResourceCreate::CreateTexture( m_Allocator.Get(), allocDesc, td, D3D12_RESOURCE_STATE_RENDER_TARGET,
        &clearValue, captureAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( captureTex.ReleaseAndGetAddressOf() ) ) ) ) {
        LogInfo() << (thumbnail ? "Thumbnail failed. Capture texture could not be created" : "GetBackbufferData failed. Capture texture could not be created");
        RestoreFrameRenderTarget();
        return;
    }
    captureTex->SetName( L"BackbufferCapture" );

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> captureRtvHeap;
    if ( FAILED( device->CreateDescriptorHeap( &rtvHeapDesc, IID_PPV_ARGS( captureRtvHeap.ReleaseAndGetAddressOf() ) ) ) ) {
        LogInfo() << (thumbnail ? "Thumbnail failed. RTV heap could not be created" : "GetBackbufferData failed. RTV heap could not be created");
        RestoreFrameRenderTarget();
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE captureRtv = captureRtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView( captureTex.Get(), nullptr, captureRtv );
    m_CmdList.InvalidateRenderTargets();

    // m_SceneColor was left in PIXEL_SHADER_RESOURCE state by OnStartWorldRendering's ResolveSceneToBackBuffer
    // call (now genuinely true on the GPU after the flush above) — safe to sample without re-transitioning.
    m_CmdList->OMSetRenderTargets( 1, &captureRtv, FALSE, nullptr );
    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( buffersize.x ), static_cast<float>( buffersize.y ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, buffersize.x, buffersize.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    if ( m_SrvHeap ) {
        ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
        m_CmdList->SetDescriptorHeaps( 1, heaps );
    }

    // TonemapCapturePSO, not Tonemap.PSO: with HDR output active the latter targets the FP16 display buffer,
    // and a savegame thumbnail wants the plain SDR image regardless — hence hdrOutput=false below too.
    m_CmdList->SetPipelineState( m_Pipelines.TonemapCapturePSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.Tonemap.RootSig.Get() );
    // Same source the on-screen resolve used: m_Fsr3Output when FSR 3 upscaled this frame, else m_SceneColor.
    m_CmdList->SetGraphicsRootDescriptorTable( 0, GetSrvGpuHandle( GetTonemapSourceSrvSlot() ) );
    // Brightness/contrast go in here, not through Present's pass: the capture never sees the 2D UI anyway,
    // and D3D11's GetBackbufferData likewise draws HDRBackBuffer through PS_PFX_GammaCorrectInv.
    const TonemapRootConstants captureConsts = MakeTonemapConstants( false, true );
    m_CmdList->SetGraphicsRoot32BitConstants( 1, kTonemapRootConstantCount, &captureConsts, 0 );
    m_CmdList->SetGraphicsRootShaderResourceView( 2, m_LumAdaptedBuffer->GetGPUVirtualAddress() );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    m_CmdList->IASetVertexBuffers( 0, 0, nullptr );
    m_CmdList->DrawInstanced( 3, 1, 0, 0 );

    m_CmdList->TransitionBarrier( captureTex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE );

    D3D12_RESOURCE_DESC capDesc = captureTex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0; UINT64 rowSizeBytes = 0, totalBytes = 0;
    device->GetCopyableFootprints( &capDesc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes );

    D3D12MA::ALLOCATION_DESC rbAllocDesc = {};
    rbAllocDesc.HeapType = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rbDesc = {};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = totalBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<D3D12MA::Allocation> readbackAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if ( FAILED( m_Allocator->CreateResource( &rbAllocDesc, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, readbackAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( readback.ReleaseAndGetAddressOf() ) ) ) ) {
        LogInfo() << (thumbnail ? "Thumbnail failed. Readback buffer could not be created" : "GetBackbufferData failed. Readback buffer could not be created");
        RestoreFrameRenderTarget();
        return;
    }

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = readback.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = captureTex.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    m_CmdList->CopyTextureRegion( &dstLoc, 0, 0, 0, &srcLoc, nullptr );

    // Block until the copy has actually landed in the readback buffer before we Map it.
    FlushCommandListSync();

    byte* d = new byte[ static_cast<size_t>( buffersize.x ) * static_cast<size_t>( buffersize.y ) * 4 ];
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>( totalBytes ) };
    void* mapped = nullptr;
    if ( SUCCEEDED( readback->Map( 0, &readRange, &mapped ) ) ) {
        const uint8_t* srcRow = reinterpret_cast<const uint8_t*>( mapped );
        byte* dstRow = d;
        for ( int row = 0; row < buffersize.y; ++row ) {
            const uint32_t* srcPixels = reinterpret_cast<const uint32_t*>( srcRow );
            byte* dstPixels = dstRow;
            for ( int col = 0; col < buffersize.x; ++col ) {
                UnpackR10G10B10A2ToBGRA8( srcPixels[col], dstPixels );
                dstPixels += 4;
            }
            srcRow += footprint.Footprint.RowPitch;
            dstRow += static_cast<size_t>( buffersize.x ) * 4;
        }
        readback->Unmap( 0, nullptr );
    } else {
        LogInfo() << (thumbnail ? "Thumbnail failed" : "GetBackbufferData failed");
    }

    *data = d;

    // The flushes above Reset the command list without leaving anything bound — rebind the swapchain
    // backbuffer so Gothic's subsequent 2D UI draws (and Present's ImGui pass) land correctly.
    RestoreFrameRenderTarget();
}


bool D3D12GraphicsEngine::ResizeSwapChain( INT2 size ) {
    if ( !m_SwapChainReady ) return false;
    if ( size.x <= 0 || size.y <= 0 ) return false;
    if ( size.x == m_BackbufferResolution.x && size.y == m_BackbufferResolution.y ) return true;

    WaitForGpuIdle();
    for ( UINT i = 0; i < kBackBufferCount; ++i ) m_BackBuffers[i].Reset();
    m_HdrDisplay.Reset();          // rebuilt at the new size below (SRV slot is kept and re-pointed)
    m_HdrDisplayAlloc.Reset();

    // Must pass the SAME flags the swapchain was created with (CreateSwapChainForHwnd's scd.Flags) —
    // neither DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING nor _FRAME_LATENCY_WAITABLE_OBJECT can be added/removed
    // via ResizeBuffers, only the create call. The waitable handle itself stays valid across the resize.
    const UINT swapChainFlags = ( m_TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0 )
        | ( m_FrameLatencyWaitableObject ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0 );
    HRESULT hr = m_SwapChain->ResizeBuffers( kBackBufferCount,
        static_cast<UINT>( size.x ), static_cast<UINT>( size.y ), kBackBufferFormat, swapChainFlags );
    if ( FAILED( hr ) ) {
        LogWarn() << "D3D12 ResizeBuffers failed (0x" << std::hex << hr << ").";
        return false;
    }

    m_BackbufferResolution = size;
    m_Resolution = ComputeRenderResolution( size );
    m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();
    if ( !AcquireBackBufferRTVs() ) return false;
    if ( !CreateRenderResolutionTargets( m_Resolution ) ) return false;   // GPU idled above
    // ResizeBuffers drops the colour space along with the buffers, and the window may have been dragged to a
    // different (possibly non-HDR) monitor — re-apply and refresh the metadata before rebuilding the target.
    ApplySwapChainColorSpace();
    if ( !CreateHdrDisplayTarget( size ) ) return false;   // no-op in SDR; fatal in HDR (see CreateSwapChain)
    CreateDisplayResolutionTargets( size );
    return true;
}


/** Picks up an ImGui/ini change to ResolutionScalePercent, from the top of OnBeginFrame — same slot and
    reasoning as the pending-resize/shader-reload handling next to it. Only the render-resolution targets
    change size. Mirrors D3D11GraphicsEngine::OnBeginFrame's `RecreateBuffers()` branch. */
void D3D12GraphicsEngine::ApplyPendingResolutionScale() {
    if ( !m_SwapChainReady ) return;
    const int requested = Engine::GAPI->GetRendererState().RendererSettings.ResolutionScalePercent;
    if ( requested == m_AppliedResolutionScalePercent ) {
        m_PendingResolutionScalePercent = 0;   // slider came back to where we already are
        m_ResolutionScaleStableFrames = 0;
        return;
    }
    // Debounce: the ImGui slider reports a new percentage every frame of a drag, and each apply below idles
    // the GPU and rebuilds every render-resolution target (and possibly every shader).
    if ( requested != m_PendingResolutionScalePercent ) {
        m_PendingResolutionScalePercent = requested;
        m_ResolutionScaleStableFrames = 0;
        return;
    }
    if ( ++m_ResolutionScaleStableFrames < kResolutionScaleDebounceFrames ) return;
    m_ResolutionScaleStableFrames = 0;
    m_PendingResolutionScalePercent = 0;

    const INT2 newRenderRes = ComputeRenderResolution( m_BackbufferResolution );
    if ( newRenderRes.x == m_Resolution.x && newRenderRes.y == m_Resolution.y ) {
        m_AppliedResolutionScalePercent = requested;   // rounded to the same pixel count; nothing to rebuild
        return;
    }

    WaitForGpuIdle();   // the targets below are still referenced by up to kBackBufferCount in-flight frames
    m_Resolution = newRenderRes;
    if ( !CreateRenderResolutionTargets( m_Resolution ) ) {
        // The frame path's null checks keep it from drawing into a mismatched target.
        LogWarn() << "D3D12GraphicsEngine: failed to rebuild the render targets at " << m_Resolution.x
            << "x" << m_Resolution.y << " (render scale " << requested << "%).";
        return;
    }

    // Skipped when the bias barely moved — a 1% slider step is ~0.014 of a mip, not worth a pipeline rebuild.
    // Failure is non-fatal: the old pipelines stay, just sampling a little blurrier than ideal.
    const float newBias = ComputeMipLodBias( m_Resolution, m_BackbufferResolution );
    if ( std::abs( newBias - m_AppliedMipLodBias ) > 0.02f ) {
        Engine::GAPI->PrintMessageTimed( INT2( 30, 30 ), "Applying render scale..." );
        RebakeMipLodBias( newBias );
    }

    m_AppliedResolutionScalePercent = requested;
    LogInfo() << "D3D12 render scale " << requested << "% -> rendering at "
        << m_Resolution.x << "x" << m_Resolution.y
        << " (display " << m_BackbufferResolution.x << "x" << m_BackbufferResolution.y
        << "), texture mip bias " << m_AppliedMipLodBias << ".";
}


/** Picks up an ImGui/ini change to RendererSettings.AoResolution. Rebuilds only the AO resources at
    the new target size - no debounce needed, it's a discrete combo box, not a per-drag-frame slider. */
void D3D12GraphicsEngine::ApplyPendingAoResolutionChange() {
    if ( !m_SwapChainReady ) return;
    const AoResolutionScale requested = Engine::GAPI->GetRendererState().RendererSettings.AoResolution;
    if ( requested == m_AppliedAoResolution ) return;

    WaitForGpuIdle();   // the AO targets below are still referenced by up to kBackBufferCount in-flight frames
    const INT2 aoSize = GetAoTargetResolution( m_Resolution );
    if ( !CreateAOResources( aoSize ) ) {
        LogWarn() << "D3D12GraphicsEngine: failed to rebuild the SSAO targets at " << aoSize.x << "x" << aoSize.y
            << " for an AO resolution change.";
    }
    CreateGtaoResources( aoSize );   // must follow CreateAOResources: XeGTAO writes ITS m_AOMask; non-fatal
    m_AoResourceSize = aoSize;
    m_AppliedAoResolution = requested;
    LogInfo() << "D3D12 AO resolution -> " << ( requested == AoResolutionScale::Half ? "Half" : "Full" )
        << " (" << aoSize.x << "x" << aoSize.y << ").";
}


XRESULT D3D12GraphicsEngine::OnResize( INT2 newSize ) {
    if ( newSize.x <= 0 || newSize.y <= 0 ) return XR_SUCCESS;

    // Never exceed the monitor's current desktop resolution, no matter where the size came from (initial
    // SetWindow() at launch, an in-game TriggerResize(), a stale/forced ini or -zRes commandline value,
    // etc.) — clamping only the ImGui dropdown's offered list isn't enough, since callers can still reach
    // OnResize() directly with an oversized value. A window/swapchain bigger than the desktop is at best
    // wasted GPU memory (DXGI_SCALING_STRETCH silently covers for it) and at worst an off-screen window.
    const int maxWidth = GetSystemMetrics( SM_CXSCREEN );
    const int maxHeight = GetSystemMetrics( SM_CYSCREEN );
    if ( maxWidth > 0 && maxHeight > 0 && ( newSize.x > maxWidth || newSize.y > maxHeight ) ) {
        LogWarn() << "D3D12GraphicsEngine::OnResize: requested " << newSize.x << "x" << newSize.y
            << " exceeds the desktop resolution (" << maxWidth << "x" << maxHeight << ") — clamping.";
        newSize.x = std::min( newSize.x, maxWidth );
        newSize.y = std::min( newSize.y, maxHeight );
        m_NewResolution = newSize;   // keep in sync so OnBeginFrame doesn't re-trigger this every frame
    }

    if ( m_SwapChainReady && newSize.x == m_BackbufferResolution.x && newSize.y == m_BackbufferResolution.y )
        return XR_SUCCESS; // nothing to do

    ResizeOutputWindow( newSize );

    if ( !m_SwapChainReady ) {
        if ( !CreateSwapChain( newSize ) ) {
            LogWarn() << "D3D12GraphicsEngine::OnResize: swapchain creation failed.";
            return XR_FAILED;
        }
        LogInfo() << "D3D12 swapchain created (" << newSize.x << "x" << newSize.y << ").";
    } else {
        if ( !ResizeSwapChain( newSize ) ) {
            // Depth/scene-color/light-cull targets may now be a mix of old- and new-resolution (or null, if
            // resource creation itself failed). Don't retry every frame — leave m_Resolution at whatever
            // ResizeSwapChain last got to and let the render path's existing null checks (haveDepth, etc.)
            // keep the frame from touching a mismatched/null DSV until the user tries again.
            LogWarn() << "D3D12GraphicsEngine::OnResize: ResizeSwapChain failed (" << newSize.x << "x" << newSize.y << ").";
            m_NewResolution = m_BackbufferResolution;
            return XR_FAILED;
        }
    }

    if ( Engine::ImGuiHandle && Engine::ImGuiHandle->Initiated ) {
        Engine::ImGuiHandle->OnResize( newSize );
    }
    return XR_SUCCESS;
}


XRESULT D3D12GraphicsEngine::TriggerResize( INT2 resolution ) {
    // Just record the request (mirrors D3D11GraphicsEngine::TriggerResize / NewResolution) — applied at the
    // top of the next OnBeginFrame, never here. This can run mid-frame (e.g. from the ImGui settings window,
    // while the command list is open and mid-recording), and resizing the swapchain/depth/scene-color targets
    // right now would touch resources the currently-recording command list still references.
    m_NewResolution = resolution;
    return XR_SUCCESS;
}


XRESULT D3D12GraphicsEngine::Clear( const float4& /*color*/ ) {
    return XR_SUCCESS; // first-light clears to the sentinel color in OnBeginFrame
}


XRESULT D3D12GraphicsEngine::ReloadShaders( ShaderCategory categories ) {
    // Just record the request — see the header comment for why nothing else happens here. This may run
    // from ImGui mid-frame (a held/spammed "Reload all Shaders" button, several settings toggles in one
    // frame), so it must stay this cheap and this safe: OR-ing a bitmask has no failure mode.
    m_PendingShaderReload |= categories;
    return XR_SUCCESS;
}


void D3D12GraphicsEngine::ApplyPendingShaderReload() {
    if ( m_PendingShaderReload == ShaderCategory::None ) return;
    // Clear immediately: whatever we're about to do covers everything requested up to this point, and any
    // request that arrives *during* the reload (there can't be one — this runs synchronously on the same
    // thread ImGui/the settings code calls ReloadShaders from, before the next frame even opens) must not
    // be lost by an unrelated later clear.
    m_PendingShaderReload = ShaderCategory::None;

    LogInfo() << "D3D12: reloading shaders...";
    Engine::GAPI->PrintMessageTimed( INT2( 30, 30 ), "Reloading shaders..." );

    // Every current PSO/root-sig/blob must be provably unreferenced by the GPU before ReloadAll() releases
    // and replaces it (D3D12PipelineState::Create*() destroys-then-recreates; destroying an object the GPU
    // is still executing against is the corruption this whole path exists to avoid). The previous frame's
    // command list is already Closed+Executed+Presented at this OnBeginFrame checkpoint, so this flush
    // drains exactly the in-flight work — no different from what TriggerResize's OnResize already does here.
    WaitForGpuIdle();

    // Snapshot the fully-working pipeline state before touching anything. Every member is a ComPtr (or a
    // container of them) or trivially-copyable, so this is a cheap AddRef pass, not a GPU operation — and it
    // is what makes a partial/failed reload safe: if any FATAL pass fails to recompile below, every ComPtr
    // this copy still holds is restored verbatim, so no draw call can ever observe a null "was working a
    // second ago" PSO. Non-fatal (optional) passes are allowed to end up degraded on their own, same as a
    // fresh Init() would leave them — every draw site for those already guards on the PSO existing.
    D3D12PipelineState backup = m_Pipelines;

    std::vector<std::string> failedFatal, failedOptional;
    const bool ok = m_Pipelines.ReloadAll( m_HdrOutputActive, failedFatal, failedOptional );

    if ( !ok ) {
        m_Pipelines = backup;   // whole-state rollback — see ReloadAll's header comment for why this is safe
        std::string names;
        for ( const auto& n : failedFatal ) { if ( !names.empty() ) names += ", "; names += n; }
        LogWarn() << "D3D12: shader reload failed (" << names << ") - keeping the previous, working shaders. "
                     "Fix the shader source and reload again.";
        Engine::GAPI->PrintMessageTimed( INT2( 30, 30 ), "Shader reload FAILED (" + names + ") - kept previous shaders." );
        return;
    }

    if ( !failedOptional.empty() ) {
        std::string names;
        for ( const auto& n : failedOptional ) { if ( !names.empty() ) names += ", "; names += n; }
        LogWarn() << "D3D12: shader reload finished with degraded passes (" << names
                   << ") - those effects are disabled/simplified until fixed and reloaded again.";
    }
    D3D12ShaderBackend::LogAndResetCacheStats( "reload" );
    LogInfo() << "D3D12: shaders reloaded.";
}


XRESULT D3D12GraphicsEngine::CreateVertexBuffer( std::unique_ptr<GfxVertexBuffer>& outBuffer ) {
    outBuffer = std::make_unique<D3D12VertexBuffer>();
    return XR_SUCCESS;
}


XRESULT D3D12GraphicsEngine::CreateTexture( GfxTexture** outTexture ) {
    if ( outTexture ) *outTexture = new D3D12Texture();
    return XR_SUCCESS;
}


XRESULT D3D12GraphicsEngine::CreateTexture( std::unique_ptr<GfxTexture>& outTexture ) {
    outTexture = std::make_unique<D3D12Texture>();
    return XR_SUCCESS;
}


XRESULT D3D12GraphicsEngine::CreateTexture( std::unique_ptr<D3D12Texture>& outTexture ) {
    outTexture = std::make_unique<D3D12Texture>();
    return XR_SUCCESS;
}


XRESULT D3D12GraphicsEngine::GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling ) {
    if ( !modeList ) return XR_SUCCESS;

    modeList->clear();
    if ( XR_SUCCESS != DXGI_GetDisplayModeList( m_Device.GetDevice()->GetAdapterLuid(), m_OutputWindow, &m_CachedDisplayModes ) ) {
        m_CachedDisplayModes.clear();
        m_CachedDisplayModes.push_back( DisplayModeInfo( std::max<int>( 1, m_BackbufferResolution.x ), std::max<int>( 1, m_BackbufferResolution.y ), 60, 1 ) );
    }
    return AppendCachedDisplayModes( modeList, includeSuperSampling );
}


BaseLineRenderer* D3D12GraphicsEngine::GetLineRenderer() {
    return m_LineRenderer.get();
}
