// D3D12GraphicsEngine — Intel XeGTAO (ground-truth ambient occlusion).
//
// This is the host side of Shaders/D3D12/XeGTAO.hlsl, which drives Intel's vendored XeGTAO.h/XeGTAO.hlsli (MIT;
// see their headers). XeGTAO is what AOMode::AO_ASSAO selects on the D3D12 backend — the D3D11 renderer keeps
// its own ASSAO port, untouched. Every other AO mode still lands on D3D12AO.cpp's simple SSAO.
//
// WHY IT LOOKS SO MUCH LIKE THE SSAO PATH IT REPLACES. It reads the same input (m_DepthBuffer, straight out of
// the Forward+ depth prepass) and writes the same output (m_AOMask, read at face value by
// include/ScreenSpaceAO.hlsl). Both are forced by Forward+, not by XeGTAO: lighting resolves inside the
// geometry pass, so the prepass is the only depth that exists when the mask is needed. Keeping the contract
// identical is what makes the two implementations interchangeable and leaves every consumer untouched.
//
// THE DISPATCH STAGES:
//   1. CSPrefilterDepths16x16  raw reversed-Z NDC depth -> a 5-level view-space depth pyramid.
//   2. CSGenerateNormals       view-space normals reconstructed from the same depth (R11G11B10_UNORM).
//                              SKIPPED whenever the depth prepass's own normal G-buffer is available: those are
//                              real shading normals for the exact geometry that wrote this depth, and they cost
//                              nothing extra. See LoadNormal in XeGTAO.hlsl for the two decodes.
//   3. CSGTAO{Low,...,Ultra}   the GTAO horizon integral -> working AO term (R8_UINT) + packed depth edges.
//   4. CSDenoise[Last]Pass     1-3 edge-aware passes; the last writes m_AOMask and re-applies Intel's
//                              XE_GTAO_OCCLUSION_TERM_SCALE. Even with denoising "disabled" one pass runs,
//                              because that scale is what gets the term into the [0,1] the consumers expect.
//
// All eight PSOs share one root signature and are fully bindless (SM6.6 ResourceDescriptorHeap), so a stage
// binds nothing but two root-constant blocks — there is no descriptor table and no per-frame heap churn.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12ResourceCreate.h"
#include "D3D12RenderGraph.h"
#include "../Engine.h"
#include "../GothicAPI.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    // Gothic's projection is reversed-Z with an INFINITE far plane (GothicAPI::GetProjectionMatrix: _33 = 0,
    // _34 = near = 1, so depth = 1/viewZ). A cleared depth of 0 — sky, and anything else no geometry covered —
    // therefore linearizes to a division by zero, and XeGTAO has no "is this a background pixel" branch: the
    // resulting inf becomes a zero-length view vector, normalize() yields NaN, and the NaN spreads through the
    // denoise into real geometry as black speckle.
    //
    // Rather than patch Intel's files, bias the denominator: XeGTAO computes viewZ as
    // DepthUnpackConsts.x / (DepthUnpackConsts.y - depth), so shifting .y down by epsilon turns cleared pixels
    // into a very large FINITE depth (1/1e-9 == 1e9 units) that the algorithm handles on its own — the sample
    // radius collapses to sub-pixel there and the pixel comes out unoccluded, which is the right answer for sky.
    // The error this introduces elsewhere is epsilon relative to depth: at Gothic's far plane (~1e-5 in depth
    // units) that is below 1e-4, and it vanishes towards the near plane.
    constexpr float kSkyDepthEpsilon = 1e-9f;

    // Mirrors XeGTAO.h's GTAOConstants exactly — same member order, same types. HLSL cbuffer packing puts every
    // float2/int2 pair on an 8-byte boundary inside a 16-byte row with no straddling here, so the layouts agree
    // member-for-member; the static_assert below is the tripwire if either side gains a field.
    struct GtaoConstants {
        int32_t ViewportSize[2];
        float   ViewportPixelSize[2];
        float   DepthUnpackConsts[2];
        float   CameraTanHalfFOV[2];
        float   NDCToViewMul[2];
        float   NDCToViewAdd[2];
        float   NDCToViewMul_x_PixelSize[2];
        float   EffectRadius;
        float   EffectFalloffRange;
        float   RadiusMultiplier;
        float   Padding0;
        float   FinalValuePower;
        float   DenoiseBlurBeta;
        float   SampleDistributionPower;
        float   ThinOccluderCompensation;
        float   DepthMIPSamplingOffset;
        int32_t NoiseIndex;
    };
    static_assert( sizeof( GtaoConstants ) == 24 * sizeof( uint32_t ),
        "GtaoConstants must match XeGTAO.h's GTAOConstants and the 24 root constants CreateGtao declares at b0" );

    // Mirrors XeGTAO.hlsl's b1 GTAOBindingsCB. Fields a stage doesn't use stay at the sentinel so a stray fetch
    // is an obvious out-of-range index rather than a plausible-looking descriptor.
    struct GtaoBindings {
        uint32_t RawDepthIndex;
        uint32_t WorkingDepthIndex;
        uint32_t NormalsIndex;
        uint32_t EdgesIndex;
        uint32_t AOTermIndex;
        uint32_t Out0Index;
        uint32_t Out1Index;
        uint32_t Out2Index;
        uint32_t Out3Index;
        uint32_t Out4Index;
        // 1 -> NormalsIndex is the prepass's RG16F octahedral WORLD-space normal G-buffer, which LoadNormal
        // decodes and rotates into view space with the three rows below. 0 -> it is CSGenerateNormals' packed
        // R11G11B10 view-space map, which needs neither.
        uint32_t NormalsAreGBuffer;
        uint32_t Pad0;
        // This frame's view matrix, uploaded VERBATIM (row-major, as every other matrix in this backend is).
        // The shader reads it back column-major and mul()s a w=0 direction through it; nothing transposes it
        // on either side. The 12 uints above are exactly 3 cbuffer rows, so this lands 16-byte aligned.
        XMFLOAT4X4 ViewMatrix;

        GtaoBindings() {
            RawDepthIndex = WorkingDepthIndex = NormalsIndex = EdgesIndex = AOTermIndex = 0xFFFFFFFFu;
            Out0Index = Out1Index = Out2Index = Out3Index = Out4Index = 0xFFFFFFFFu;
            NormalsAreGBuffer = 0;
            Pad0 = 0;
            ViewMatrix = {};
        }
    };
    static_assert( sizeof( GtaoBindings ) == 28 * sizeof( uint32_t ),
        "GtaoBindings must match XeGTAO.hlsl's b1 block and the 28 root constants CreateGtao declares" );
    constexpr UINT kGtaoBindingConstants = 28;

    // Intel's DenoiseBlurBeta convention: a very large beta makes the centre tap dominate to the point that the
    // spatial filter is a no-op, which is how "denoising disabled" is expressed without a separate shader.
    constexpr float kDenoiseDisabledBeta = 1.0e4f;
    constexpr float kDenoiseEnabledBeta = 1.2f;
}

/** (Re)creates m_GtaoWorkingDepth (the 5-level view-space depth pyramid) and its persistent heap slots.
    Called from the same resize path as CreateAOResources — which owns m_AOMask, the output this pass shares
    with the simple SSAO path, so this must run AFTER it.

    This is now the ONLY XeGTAO intermediate built here. Normals, edges, both AO-term ping-pong buffers and
    (in Half mode) the pre-downsampled half-res depth are single-mip, single-UAV textures fully written and
    consumed within one RenderGTAO() call, so they are D3D12RenderGraph-managed transients acquired fresh
    every call instead — see RenderGTAO. No explicit creation, no resize hook, no readiness flag for them any
    more. m_GtaoWorkingDepth can't follow: it needs a 5-level MIP chain with a per-mip UAV, and RGTextureDesc /
    D3D12RenderTarget / D3D12AliasedTextureArena only support single-mip textures — extending them to would
    touch every other consumer (DoF, bloom, god rays, underwater, SMAA), so it stays on this manual path. */
bool D3D12GraphicsEngine::CreateGtaoResources( INT2 size ) {
    m_GtaoResourcesReady = false;
    // CreateResource rejects a MipLevels the dimensions can't support — so refuse anything under 16x16 here
    // rather than letting the allocation fail and log a scary warning at a resolution nobody plays at.
    if ( size.x < 16 || size.y < 16 ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Allocator ) return false;
    // Init runs before the first CreateSwapChain; don't re-enable the feature if the pipelines failed there.
    if ( !m_Pipelines.Gtao.RootSig || !m_Pipelines.Gtao.PrefilterPSO ) return false;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    // R32_FLOAT, not Intel's default R16_FLOAT — see the note in the engine header: Gothic's view depths run
    // past fp16's 65504 ceiling near the horizon, and FP32 keeps every element type a plain `float`, which is
    // what makes the bindless fetches in the shader trivial.
    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = static_cast<UINT64>( size.x );
    dd.Height = static_cast<UINT>( size.y );
    dd.DepthOrArraySize = 1;
    dd.MipLevels = static_cast<UINT16>( kGtaoDepthMipLevels );
    dd.Format = DXGI_FORMAT_R32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if ( FAILED( D3D12ResourceCreate::CreateTexture( m_Allocator.Get(), heapDefault, dd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, m_GtaoWorkingDepthAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_GtaoWorkingDepth.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: failed to create the XeGTAO working-depth pyramid (" << size.x << "x" << size.y << ").";
        return false;
    }
    m_GtaoWorkingDepth->SetName( L"GtaoWorkingDepth" );

    auto ensureSlot = [&]( UINT& slot ) -> bool {
        if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
        return slot != UINT_MAX;
        };
    if ( !ensureSlot( m_GtaoWorkingDepthSrvSlot ) ) return false;
    for ( UINT m = 0; m < kGtaoDepthMipLevels; ++m ) {
        if ( !ensureSlot( m_GtaoWorkingDepthUavSlot[m] ) ) return false;
    }

    // One SRV over the WHOLE chain (the main pass samples explicit MIP levels through it) plus one UAV per
    // level, because the prefilter writes all five in a single dispatch.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = kGtaoDepthMipLevels;
    device->CreateShaderResourceView( m_GtaoWorkingDepth.Get(), &srv, GetSrvCpuHandle( m_GtaoWorkingDepthSrvSlot ) );

    for ( UINT m = 0; m < kGtaoDepthMipLevels; ++m ) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Texture2D.MipSlice = m;
        device->CreateUnorderedAccessView( m_GtaoWorkingDepth.Get(), nullptr, &uav,
            GetSrvCpuHandle( m_GtaoWorkingDepthUavSlot[m] ) );
    }

    m_GtaoResourcesReady = true;
    return true;
}

/** XeGTAO is the selected AO mode and everything it needs exists. Shares m_AOMask and the depth-prepass depth
    with the simple SSAO path, so it depends on that path's resources being ready too. The five graph-managed
    scratch textures (see RenderGTAO) need no readiness check here — a failed Acquire() just makes that one
    pass's callback no-op for the frame, the same fallback shape as DoF/god-rays' own graph resources. */
bool D3D12GraphicsEngine::IsGtaoEnabled() const {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( settings.AoMode != AOMode::AO_ASSAO ) return false;
    if ( !m_GtaoResourcesReady || !m_AOResourcesReady ) return false;
    if ( !m_DepthBuffer || m_DepthSrvSlot == UINT_MAX ) return false;
    if ( m_AOMaskUintUavSlot == UINT_MAX ) return false;
    const auto& p = m_Pipelines.Gtao;
    if ( !p.RootSig || !p.PrefilterPSO || !p.NormalsPSO || !p.DenoisePSO || !p.DenoiseLastPSO ) return false;
    if ( settings.AoResolution == AoResolutionScale::Half && !p.DownsamplePSO ) return false;
    const int quality = std::clamp( settings.GtaoSettings.QualityLevel, 0, 3 );
    return p.MainPSO[quality] != nullptr;
}

/** The full chain. Caller (RenderSSAO) has already defaulted m_ActiveAOMaskSrvSlot to the white "no occlusion" texture and
    verified IsGtaoEnabled().

    RENDER-GRAPH USE (mirrors D3D12DoF.cpp's; see D3D12RenderGraph.h / D3D12AliasedTextureArena.h). Normals,
    edges, both AO-term ping-pong buffers and the Half-mode downsampled depth are PURELY transient within one
    call of this function — written, read, then dead — so they are acquired from a LOCAL D3D12RenderGraph
    built right here rather than held as members. Local, not the shared postFxGraph: that graph is built much
    later in the frame, near the tonemap (D3D12Scene.cpp's OnStartWorldRendering), long after AO has already
    run off the depth prepass. Compile()+Execute() both happen before this function returns (same as
    D3D12Water.cpp's local waterGraph) — UNLIKE D3D12DoF.cpp's passes, which run later from a different
    function and so must std::shared_ptr their cross-pass state, every pass callback below can capture this
    function's stack locals by plain value and stay safe. They are NOT resized/recreated explicitly on a
    resolution or AoResolution change either: since they are asked for at the CURRENT width/height every call,
    a change just makes next frame's CreateTexture() ask for a different size, which the graph handles the
    same way it handles any other description change. */
void D3D12GraphicsEngine::RenderGTAO() {
    if ( !m_FrameOpen || !m_CmdList ) return;

    DX_ZONE( m_CmdList.Get(), "XeGTAO" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "XeGTAO" );

    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const GTAOSettings& gtao = settings.GtaoSettings;
    const int quality = std::clamp( gtao.QualityLevel, 0, 3 );
    // Even with denoising off, one (final) pass has to run: it is what re-applies Intel's occlusion-term scale
    // and moves the term out of the working texture into m_AOMask.
    const int denoisePassCount = std::max( 1, std::clamp( gtao.DenoisePasses, 0, 3 ) );

    // m_AoResourceSize, NOT m_Resolution: every stage below (prefilter through denoise) dispatches and sizes
    // its constants off this. It equals m_Resolution in Full mode; halved in Half mode — see
    // D3D12GraphicsEngine::GetAoTargetResolution. Half mode additionally needs its own pre-downsampled "raw
    // depth" (see the "GTAO Downsample" pass below) rather than just pointing consts.ViewportSize at
    // m_DepthSrvSlot: Intel's algorithm ties consts.ViewportSize to the resolution of the RAW depth it is
    // handed (XeGTAO_PrefilterDepths16x16 reads a 2x2 block per dispatch thread), so a half-res working-depth
    // chain needs an already-half-res "raw" depth feeding it.
    const UINT width = static_cast<UINT>( m_AoResourceSize.x );
    const UINT height = static_cast<UINT>( m_AoResourceSize.y );
    const bool halfRes = settings.AoResolution == AoResolutionScale::Half;
    constexpr uint32_t kRgNeedsUav = 1u;   // D3D12RenderGraph's transient-texture flag convention (bit 0 = UAV)

    // --- Constants -------------------------------------------------------------------------------------------
    // This frame's projection — the one the depth prepass below was rasterized with. (Its _13/_23 carry the TAA
    // jitter, which this ignores; that is a sub-pixel shift applied uniformly to the centre and every sample,
    // so it cancels within the pass.)
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();

    // World -> view for the G-buffer normal path (LoadNormal). The SAME view matrix every geometry pass uploads,
    // passed through untouched — see the warning in LoadNormal about hand-expanding it instead of mul()ing.
    // Re-derived exactly like DrawWorldMesh does, so it cannot drift from the camera the prepass used.
    XMMATRIX viewXM = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( viewXM );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;

    // Feed XeGTAO the depth prepass's own normals when they exist — real shading normals for exactly the
    // geometry that wrote this depth. CSGenerateNormals (reconstruction from depth) stays as the fallback for
    // when the G-buffer PSOs/targets failed to create, and is ALSO forced in Half mode: m_NormalBuffer is
    // native resolution and LoadNormal's G-buffer path indexes it by this pass's own (halved) pixel
    // coordinates, which would sample only its top-left quadrant. Regenerating from the (already-halved)
    // depth this pass already has is simpler and cheaper than remapping the G-buffer read.
    const bool gbufNormals = !halfRes && MotionGBufferActive() && m_NormalBuffer && m_NormalSrvSlot != UINT_MAX;

    GtaoConstants cb = {};
    cb.ViewportSize[0] = static_cast<int32_t>( width );
    cb.ViewportSize[1] = static_cast<int32_t>( height );
    cb.ViewportPixelSize[0] = 1.0f / static_cast<float>( width );
    cb.ViewportPixelSize[1] = 1.0f / static_cast<float>( height );

    // viewZ = ProjZY / (depth - ProjZX) is this backend's reversed-Z linearization (same terms the simple SSAO
    // and the ScreenSpaceAO reprojection use). XeGTAO wants it as mul / (add - depth), hence the negated mul.
    // Intel's GTAOUpdateConstants is deliberately NOT used to derive these: its handedness fix-up
    // ("if mul*add < 0, negate add") assumes a conventional finite-far-plane projection and would corrupt an
    // infinite reversed-Z one, where add is 0. See kSkyDepthEpsilon for the bias on `add`.
    cb.DepthUnpackConsts[0] = -projM._43;
    cb.DepthUnpackConsts[1] = projM._33 - kSkyDepthEpsilon;

    const float tanHalfFOVX = 1.0f / projM._11;
    const float tanHalfFOVY = 1.0f / projM._22;
    cb.CameraTanHalfFOV[0] = tanHalfFOVX;
    cb.CameraTanHalfFOV[1] = tanHalfFOVY;
    cb.NDCToViewMul[0] = tanHalfFOVX * 2.0f;
    cb.NDCToViewMul[1] = tanHalfFOVY * -2.0f;
    cb.NDCToViewAdd[0] = tanHalfFOVX * -1.0f;
    cb.NDCToViewAdd[1] = tanHalfFOVY * 1.0f;
    cb.NDCToViewMul_x_PixelSize[0] = cb.NDCToViewMul[0] * cb.ViewportPixelSize[0];
    cb.NDCToViewMul_x_PixelSize[1] = cb.NDCToViewMul[1] * cb.ViewportPixelSize[1];

    cb.EffectRadius = gtao.Radius;               // Gothic world units (1 m == 100), see GTAOSettings
    cb.EffectFalloffRange = gtao.FalloffRange;
    cb.RadiusMultiplier = gtao.RadiusMultiplier;
    cb.FinalValuePower = gtao.FinalValuePower;
    cb.DenoiseBlurBeta = ( gtao.DenoisePasses == 0 ) ? kDenoiseDisabledBeta : kDenoiseEnabledBeta;
    cb.SampleDistributionPower = gtao.SampleDistributionPower;
    cb.ThinOccluderCompensation = gtao.ThinOccluderCompensation;
    cb.DepthMIPSamplingOffset = gtao.DepthMIPSamplingOffset;
    // The spatio-temporal dither only rotates when something is accumulating it. Without TAA a per-frame
    // rotation is just crawling noise, so freeze the sequence at index 0 and let the spatial dither stand alone.
    if ( IsTaaEnabled() ) {
        ++m_GtaoFrameNumber;
        cb.NoiseIndex = static_cast<int32_t>( m_GtaoFrameNumber % 64u );
    } else {
        m_GtaoFrameNumber = 0;
        cb.NoiseIndex = 0;
    }

    m_CmdList->SetComputeRootSignature( m_Pipelines.Gtao.RootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 24, &cb, 0 );

    // The per-dispatch bindings block, pre-seeded with everything that is the same for all of them.
    GtaoBindings common;
    common.NormalsAreGBuffer = gbufNormals ? 1u : 0u;
    common.ViewMatrix = viewM;

    // --- Barriers for the resources that stay OUTSIDE the graph -----------------------------------------------
    // The depth buffer comes out of the prepass in DEPTH_WRITE and has to be flipped to a shader-read state;
    // the normal G-buffer comes out of it in RENDER_TARGET and gets the same treatment, since
    // FillCameraVelocity at the end of the frame is what normally moves it and still expects to find it
    // there; m_AOMask flips back if a previous successful AO run left it readable for the lit passes.
    // m_GtaoWorkingDepth rests in UNORDERED_ACCESS between frames and needs no "before" flip here — the
    // Prefilter pass below writes it via UAV directly, same as it always did. All three fire back-to-back
    // with no GPU work between them, so — unlike the pre-pass TransitionExternal hook, which would have to
    // pick between two different FIRST passes depending on halfRes (see D3D12_RENDERGRAPH_BARRIER_BATCHING.md)
    // — there's no reason for them to be three separate Barrier() calls in a capture: one plain
    // TransitionBarriers() batch covers it, independent of the graph entirely. Not a call to
    // BeginAoDepthRead() (that issues its own un-batched TransitionBarrier); its depth-buffer transition is
    // inlined below instead. RenderSimpleSSAO (D3D12AO.cpp) still calls BeginAoDepthRead() as-is — it has no
    // sibling transitions to batch it with.
    {
        D3D12ResourceTransition pre[3];
        UINT preCount = 0;
        pre[preCount++] = { m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        if ( gbufNormals ) {
            pre[preCount++] = { m_NormalBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        }
        if ( m_AOMaskInPixelState ) {
            pre[preCount++] = { m_AOMask.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            m_AOMaskInPixelState = false;
        }
        m_CmdList->TransitionBarriers( pre, preCount );
    }

    // --- The graph-managed scratch chain -------------------------------------------------------------------
    // See this function's own header comment for why this is a LOCAL, synchronous graph. Root signature + the
    // b0 GTAOConstants block are bound once here (unchanged from before); each pass rebinds it too, matching
    // D3D12DoF.cpp's defensive style, even though nothing else is interleaved between this and Execute().
    m_CmdList->SetComputeRootSignature( m_Pipelines.Gtao.RootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 24, &cb, 0 );

    D3D12RenderGraph gtaoGraph( &m_AliasArena );

    // --- 0. Half-resolution downsample (Half mode only; GD3D11 addition, not part of Intel's XeGTAO sample —
    //        see the width/height comment above for why the rest of the chain can't just be pointed at
    //        m_DepthSrvSlot with a halved ViewportSize).
    RGResourceHandle halfDepthHandle = RG_INVALID_HANDLE;
    if ( halfRes ) {
        gtaoGraph.AddPass( RG_PASS_NAME( "GTAO Downsample" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
            halfDepthHandle = builder.CreateTexture( { width, height, static_cast<int>( DXGI_FORMAT_R32_FLOAT ),
                L"GtaoHalfDepth", kRgNeedsUav }, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
            pass.m_executeCallback = [this, common, width, height, halfDepthHandle]( const D3D12RenderGraph& g, D3D12CmdList& cmdList ) {
                D3D12RenderTarget* halfDepth = g.GetPhysicalTexture( halfDepthHandle );
                if ( !halfDepth ) return;   // arena exhausted (logged once by it) — every pass below sees a null too and no-ops
                GtaoBindings b = common;
                b.RawDepthIndex = m_DepthSrvSlot;   // native-res source — the ONE stage that reads it directly
                b.Out0Index = halfDepth->GetUavSlot();
                cmdList.SetComputeRootSignature( m_Pipelines.Gtao.RootSig.Get() );
                cmdList.SetComputeRoot32BitConstants( 1, kGtaoBindingConstants, &b, 0 );
                cmdList.SetPipelineState( m_Pipelines.Gtao.DownsamplePSO.Get() );
                cmdList.Dispatch( ( width + 7 ) / 8, ( height + 7 ) / 8, 1 );
                };
            } );
    }

    // --- 1. Prefilter depths ---------------------------------------------------------------------------------
    // 8x8 threads, each handling a 2x2 block: one group covers 16x16 pixels. Writes m_GtaoWorkingDepth, which
    // is NOT graph-tracked (see CreateGtaoResources), so this pass declares no Write() of its own — only the
    // Read() of the graph-owned half-depth in Half mode. A pass with reads but no writes is never dead-pass-
    // eliminated (Execute()'s check only fires when m_writes is non-empty), same as DoF's Prepare/Restore.
    gtaoGraph.AddPass( RG_PASS_NAME( "GTAO Prefilter" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
        if ( halfRes ) builder.Read( halfDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        pass.m_executeCallback = [this, common, width, height, halfRes, halfDepthHandle]( const D3D12RenderGraph& g, D3D12CmdList& cmdList ) {
            GtaoBindings b = common;
            if ( halfRes ) {
                D3D12RenderTarget* halfDepth = g.GetPhysicalTexture( halfDepthHandle );
                if ( !halfDepth ) return;
                b.RawDepthIndex = halfDepth->GetSrvSlot();
            } else {
                b.RawDepthIndex = m_DepthSrvSlot;
            }
            b.Out0Index = m_GtaoWorkingDepthUavSlot[0];
            b.Out1Index = m_GtaoWorkingDepthUavSlot[1];
            b.Out2Index = m_GtaoWorkingDepthUavSlot[2];
            b.Out3Index = m_GtaoWorkingDepthUavSlot[3];
            b.Out4Index = m_GtaoWorkingDepthUavSlot[4];
            cmdList.SetComputeRootSignature( m_Pipelines.Gtao.RootSig.Get() );
            cmdList.SetComputeRoot32BitConstants( 1, kGtaoBindingConstants, &b, 0 );
            cmdList.SetPipelineState( m_Pipelines.Gtao.PrefilterPSO.Get() );
            cmdList.Dispatch( ( width + 15 ) / 16, ( height + 15 ) / 16, 1 );
            };
        } );

    // --- 2. Normals ------------------------------------------------------------------------------------------
    // Only when there is no prepass G-buffer to take them from: that one already holds this frame's real
    // shading normals for every prepass writer, so reconstructing worse ones from depth would be pure cost.
    // (Also the path Half mode always takes — see gbufNormals' comment above.)
    RGResourceHandle normalsHandle = RG_INVALID_HANDLE;
    if ( !gbufNormals ) {
        gtaoGraph.AddPass( RG_PASS_NAME( "GTAO Normals" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
            if ( halfRes ) builder.Read( halfDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
            normalsHandle = builder.CreateTexture( { width, height, static_cast<int>( DXGI_FORMAT_R32_UINT ),
                L"GtaoNormals", kRgNeedsUav }, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
            pass.m_executeCallback = [this, common, width, height, halfRes, halfDepthHandle, normalsHandle]
                ( const D3D12RenderGraph& g, D3D12CmdList& cmdList ) {
                D3D12RenderTarget* normals = g.GetPhysicalTexture( normalsHandle );
                if ( !normals ) return;
                GtaoBindings b = common;
                if ( halfRes ) {
                    D3D12RenderTarget* halfDepth = g.GetPhysicalTexture( halfDepthHandle );
                    if ( !halfDepth ) return;
                    b.RawDepthIndex = halfDepth->GetSrvSlot();
                } else {
                    b.RawDepthIndex = m_DepthSrvSlot;
                }
                b.Out0Index = normals->GetUavSlot();
                cmdList.SetComputeRootSignature( m_Pipelines.Gtao.RootSig.Get() );
                cmdList.SetComputeRoot32BitConstants( 1, kGtaoBindingConstants, &b, 0 );
                cmdList.SetPipelineState( m_Pipelines.Gtao.NormalsPSO.Get() );
                cmdList.Dispatch( ( width + 7 ) / 8, ( height + 7 ) / 8, 1 );
                };
            } );
    }

    // --- 3. The GTAO integral --------------------------------------------------------------------------------
    RGResourceHandle aoTerm0Handle = RG_INVALID_HANDLE;
    RGResourceHandle edgesHandle = RG_INVALID_HANDLE;
    gtaoGraph.AddPass( RG_PASS_NAME( "GTAO Integral" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
        if ( !gbufNormals ) builder.Read( normalsHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        aoTerm0Handle = builder.CreateTexture( { width, height, static_cast<int>( DXGI_FORMAT_R8_UINT ),
            L"GtaoAOTerm0", kRgNeedsUav }, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        edgesHandle = builder.CreateTexture( { width, height, static_cast<int>( DXGI_FORMAT_R8_UNORM ),
            L"GtaoEdges", kRgNeedsUav }, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        // m_GtaoWorkingDepth is NOT graph-tracked; Prefilter left it in UNORDERED_ACCESS, so flip it to
        // shader-read here — folded into the SAME batched TransitionBarriers() call as the Read()/
        // CreateTexture() declarations above instead of a separate mid-callback TransitionBarrier (mirrors
        // the old manual toRead() call exactly, just issued one call earlier).
        builder.TransitionExternal( m_GtaoWorkingDepth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        pass.m_executeCallback = [this, common, width, height, quality, gbufNormals, normalsHandle, aoTerm0Handle, edgesHandle]
            ( const D3D12RenderGraph& g, D3D12CmdList& cmdList ) {
            D3D12RenderTarget* aoTerm0 = g.GetPhysicalTexture( aoTerm0Handle );
            D3D12RenderTarget* edges = g.GetPhysicalTexture( edgesHandle );
            if ( !aoTerm0 || !edges ) return;
            GtaoBindings b = common;
            b.WorkingDepthIndex = m_GtaoWorkingDepthSrvSlot;
            if ( gbufNormals ) {
                b.NormalsIndex = m_NormalSrvSlot;
            } else {
                D3D12RenderTarget* normals = g.GetPhysicalTexture( normalsHandle );
                if ( !normals ) return;
                b.NormalsIndex = normals->GetSrvSlot();
            }
            b.Out0Index = aoTerm0->GetUavSlot();
            b.Out1Index = edges->GetUavSlot();
            cmdList.SetComputeRootSignature( m_Pipelines.Gtao.RootSig.Get() );
            cmdList.SetComputeRoot32BitConstants( 1, kGtaoBindingConstants, &b, 0 );
            cmdList.SetPipelineState( m_Pipelines.Gtao.MainPSO[quality].Get() );
            cmdList.Dispatch( ( width + 7 ) / 8, ( height + 7 ) / 8, 1 );
            };
        } );

    // --- 4. Denoise ------------------------------------------------------------------------------------------
    // Ping-pong between the two working AO terms; the last pass writes m_AOMask (external — the graph never
    // owned it) through its R8_UINT alias. Each thread handles 2 horizontal pixels, hence the doubled X
    // divisor. aoTermHandle[0] always exists already (the Integral pass above); aoTermHandle[1] is created
    // lazily the first time a non-last iteration needs to write it — denoisePassCount maxes at 3, so that is
    // at most once, but the check is written to hold for any count.
    RGResourceHandle aoTermHandle[2] = { aoTerm0Handle, RG_INVALID_HANDLE };
    int srcTerm = 0;
    for ( int denoiseIdx = 0; denoiseIdx < denoisePassCount; ++denoiseIdx ) {
        const bool lastPass = ( denoiseIdx == denoisePassCount - 1 );
        const int curSrc = srcTerm;
        const int curDst = 1 - srcTerm;   // only meaningful when !lastPass

        gtaoGraph.AddPass( RG_PASS_NAME( "GTAO Denoise" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
            builder.Read( aoTermHandle[curSrc], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
            builder.Read( edgesHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
            const RGResourceHandle srcHandle = aoTermHandle[curSrc];
            RGResourceHandle dstHandle = RG_INVALID_HANDLE;
            if ( !lastPass ) {
                if ( aoTermHandle[curDst] == RG_INVALID_HANDLE ) {
                    aoTermHandle[curDst] = builder.CreateTexture( { width, height, static_cast<int>( DXGI_FORMAT_R8_UINT ),
                        L"GtaoAOTerm1", kRgNeedsUav }, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
                } else {
                    builder.Write( aoTermHandle[curDst], D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
                }
                dstHandle = aoTermHandle[curDst];
            } else {
                // Writes m_AOMask, which the graph doesn't track as a Write — without this, Execute()'s
                // dead-pass elimination would see a pass whose only real output is invisible to it and skip
                // the callback entirely (see D3D12DoF.cpp's Composite pass for the identical situation).
                builder.MarkExternalEffect();
                // Every resting-state restore for a resource this chain borrowed from OUTSIDE the graph,
                // folded onto this — the LAST pass in the graph, unconditionally (this iteration is always
                // the final AddPass call the function makes) — instead of standing apart in a separate
                // end-of-function block. m_AOMask's flip must wait until THIS pass's dispatch has actually
                // written it, so none of these could be pre-pass transitions; TransitionExternalAfter fires
                // right after the callback below returns, all four batched into ONE call. The depth-buffer
                // entry is EndAoDepthRead()'s own transition, inlined here rather than calling that helper
                // (which would issue its own separate, un-batched TransitionBarrier) — RenderSimpleSSAO
                // (D3D12AO.cpp) still calls BeginAoDepthRead()/EndAoDepthRead() as a bracketed pair since it
                // has no graph pass to hang the "after" side off of.
                builder.TransitionExternalAfter( m_AOMask.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
                builder.TransitionExternalAfter( m_GtaoWorkingDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
                builder.TransitionExternalAfter( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE );
                if ( gbufNormals ) {
                    builder.TransitionExternalAfter( m_NormalBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
                }
            }
            pass.m_executeCallback = [this, common, width, height, lastPass, srcHandle, dstHandle, edgesHandle]
                ( const D3D12RenderGraph& g, D3D12CmdList& cmdList ) {
                D3D12RenderTarget* src = g.GetPhysicalTexture( srcHandle );
                D3D12RenderTarget* edges = g.GetPhysicalTexture( edgesHandle );
                if ( !src || !edges ) return;
                GtaoBindings b = common;
                b.AOTermIndex = src->GetSrvSlot();
                b.EdgesIndex = edges->GetSrvSlot();
                if ( lastPass ) {
                    b.Out0Index = m_AOMaskUintUavSlot;
                } else {
                    D3D12RenderTarget* dst = g.GetPhysicalTexture( dstHandle );
                    if ( !dst ) return;
                    b.Out0Index = dst->GetUavSlot();
                }
                cmdList.SetComputeRootSignature( m_Pipelines.Gtao.RootSig.Get() );
                cmdList.SetComputeRoot32BitConstants( 1, kGtaoBindingConstants, &b, 0 );
                cmdList.SetPipelineState( ( lastPass ? m_Pipelines.Gtao.DenoiseLastPSO : m_Pipelines.Gtao.DenoisePSO ).Get() );
                cmdList.Dispatch( ( width + 15 ) / 16, ( height + 7 ) / 8, 1 );
                };
            } );

        if ( !lastPass ) srcTerm = curDst;
    }

    gtaoGraph.Compile();
    gtaoGraph.Execute( m_CmdList );

    // Every resting-state restore for a resource borrowed from OUTSIDE the graph (m_AOMask,
    // m_GtaoWorkingDepth, the depth buffer, and the G-buffer normal target when borrowed) now rides the
    // last "GTAO Denoise" pass's TransitionExternalAfter batch — see the denoise loop above — so there is
    // nothing left to restore here. Skipping any of those would produce a GPU-validation
    // RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH rather than a visible artifact, same as before this refactor.

    m_AOMaskInPixelState = true;
    m_ActiveAOMaskSrvSlot = m_AOMaskSrvSlot;
}
