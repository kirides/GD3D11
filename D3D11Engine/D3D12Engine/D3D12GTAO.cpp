// D3D12GraphicsEngine — Intel XeGTAO (ground-truth ambient occlusion), host side of
// Shaders/D3D12/XeGTAO.hlsl / Intel's vendored XeGTAO.h/XeGTAO.hlsli (MIT). Selected by
// AOMode::AO_ASSAO on D3D12 only; D3D11 keeps its own ASSAO port. Other AO modes use D3D12AO.cpp.
//
// Shares m_DepthBuffer (Forward+ prepass depth) and m_AOMask with the simple SSAO path it replaces,
// so the two are interchangeable and every consumer stays untouched.
//
// Dispatch stages: 1) CSPrefilterDepths16x16 - depth pyramid. 2) CSGenerateNormals - view-space
// normals from depth, skipped when the prepass G-buffer normal already exists. 3) CSGTAO{Low..Ultra}
// - the horizon integral. 4) CSDenoise[Last]Pass - 1-3 edge-aware passes; the last always runs even
// with denoising "disabled", since it also applies Intel's occlusion-term scale into m_AOMask.
//
// All eight PSOs share one bindless (SM6.6) root signature - two root-constant blocks, no tables.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12ResourceCreate.h"
#include "D3D12RenderGraph.h"
#include "../Engine.h"
#include "../GothicAPI.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    // Gothic's reversed-Z projection has an infinite far plane, so cleared depth (0, sky) linearizes to
    // a divide-by-zero -> inf view vector -> NaN, which XeGTAO has no guard against and spreads into
    // real geometry as black speckle. Bias the denominator instead of patching Intel's files: it turns
    // cleared pixels into a very large but finite depth, which naturally comes out unoccluded.
    constexpr float kSkyDepthEpsilon = 1e-9f;

    // Mirrors XeGTAO.h's GTAOConstants exactly (same member order/types); static_assert below is the
    // tripwire if either side gains a field.
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

    // Mirrors XeGTAO.hlsl's b1 GTAOBindingsCB. Unused fields stay at the sentinel so a stray fetch is
    // an obvious out-of-range index.
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
        // 1 -> NormalsIndex is the prepass's world-space normal G-buffer (LoadNormal decodes/rotates it
        // to view space). 0 -> CSGenerateNormals' view-space map, needing neither decode nor rotation.
        uint32_t NormalsAreGBuffer;
        uint32_t Pad0;
        XMFLOAT4X4 ViewMatrix;   // uploaded verbatim, row-major; shader mul()s it untransposed

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

    // A very large beta makes the centre tap dominate, expressing "denoising disabled" without a separate shader.
    constexpr float kDenoiseDisabledBeta = 1.0e4f;
    constexpr float kDenoiseEnabledBeta = 1.2f;
}

/** (Re)creates m_GtaoWorkingDepth (the 5-level view-space depth pyramid) and its persistent heap slots.
    Must run after CreateAOResources, which owns m_AOMask (shared with the simple SSAO path).

    This is the only XeGTAO intermediate built here - normals, edges, AO-term ping-pong buffers, and
    the Half-mode downsampled depth are single-mip transients acquired fresh each RenderGTAO() call
    instead (see RenderGTAO). m_GtaoWorkingDepth can't follow: it needs a per-mip UAV 5-level MIP
    chain, which RGTextureDesc/D3D12RenderTarget/D3D12AliasedTextureArena don't support. */
bool D3D12GraphicsEngine::CreateGtaoResources( INT2 size ) {
    m_GtaoResourcesReady = false;
    if ( size.x < 16 || size.y < 16 ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Allocator ) return false;
    if ( !m_Pipelines.Gtao.RootSig || !m_Pipelines.Gtao.PrefilterPSO ) return false;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    // R32_FLOAT, not Intel's default R16_FLOAT: Gothic's view depths exceed fp16's 65504 ceiling near
    // the horizon.
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

    // One SRV over the whole chain plus one UAV per level (the prefilter writes all five at once).
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

/** XeGTAO is the selected AO mode and everything it needs exists. Shares m_AOMask/depth with the
    simple SSAO path, so depends on its resources too. */
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

/** The full chain. Caller (RenderSSAO) has already defaulted m_ActiveAOMaskSrvSlot to the white
    "no occlusion" texture and verified IsGtaoEnabled().

    Normals, edges, the AO-term ping-pong buffers, and the Half-mode downsampled depth are purely
    transient within this call, so they come from a LOCAL D3D12RenderGraph (not the shared postFxGraph,
    which builds later near the tonemap). Compile()+Execute() both happen before this function
    returns, so every pass callback can capture stack locals by plain value. */
void D3D12GraphicsEngine::RenderGTAO() {
    if ( !m_FrameOpen || !m_CmdList ) return;

    DX_ZONE( m_CmdList.Get(), "XeGTAO" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "XeGTAO" );

    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const GTAOSettings& gtao = settings.GtaoSettings;
    const int quality = std::clamp( gtao.QualityLevel, 0, 3 );
    // One (final) pass always runs even with denoising off: it applies Intel's occlusion-term scale.
    const int denoisePassCount = std::max( 1, std::clamp( gtao.DenoisePasses, 0, 3 ) );

    // m_AoResourceSize, NOT m_Resolution - halved vs m_Resolution in Half mode (GetAoTargetResolution).
    // Half mode also needs its own pre-downsampled "raw" depth (the "GTAO Downsample" pass below):
    // Intel's algorithm ties ViewportSize to the resolution of the raw depth it reads directly.
    const UINT width = static_cast<UINT>( m_AoResourceSize.x );
    const UINT height = static_cast<UINT>( m_AoResourceSize.y );
    const bool halfRes = settings.AoResolution == AoResolutionScale::Half;
    constexpr uint32_t kRgNeedsUav = 1u;   // D3D12RenderGraph's transient-texture flag convention (bit 0 = UAV)

    // --- Constants -------------------------------------------------------------------------------------------
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();

    // World -> view for the G-buffer normal path (LoadNormal). Same view matrix every geometry pass
    // uploads, re-derived so it can't drift from the camera the prepass used.
    XMMATRIX viewXM = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( viewXM );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;

    // Prefer the prepass's own G-buffer normals (real shading normals, free) over reconstructing from
    // depth. Forced off in Half mode: m_NormalBuffer is native-res and would sample the wrong quadrant
    // at this pass's halved pixel coordinates.
    const bool gbufNormals = !halfRes && MotionGBufferActive() && m_NormalBuffer && m_NormalSrvSlot != UINT_MAX;

    GtaoConstants cb = {};
    cb.ViewportSize[0] = static_cast<int32_t>( width );
    cb.ViewportSize[1] = static_cast<int32_t>( height );
    cb.ViewportPixelSize[0] = 1.0f / static_cast<float>( width );
    cb.ViewportPixelSize[1] = 1.0f / static_cast<float>( height );

    // This backend's reversed-Z linearization, negated for XeGTAO's mul/(add-depth) convention. Intel's
    // GTAOUpdateConstants is not used: its handedness fix-up assumes a finite far plane and would
    // corrupt this infinite reversed-Z one. See kSkyDepthEpsilon for the bias on `add`.
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
    // Freeze the dither sequence without TAA - a per-frame rotation with nothing accumulating it is just crawling noise.
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
    // Depth buffer: DEPTH_WRITE -> shader-read. Normal G-buffer: RENDER_TARGET -> shader-read (still
    // expected there by FillCameraVelocity later). m_AOMask flips back if a previous run left it
    // readable. All batched into one TransitionBarriers() call rather than BeginAoDepthRead()'s own
    // un-batched one; RenderSimpleSSAO (D3D12AO.cpp) still uses that helper directly.
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
    m_CmdList->SetComputeRootSignature( m_Pipelines.Gtao.RootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 24, &cb, 0 );

    D3D12RenderGraph gtaoGraph( &m_AliasArena );

    // --- 0. Half-resolution downsample (Half mode only; not part of Intel's XeGTAO sample).
    RGResourceHandle halfDepthHandle = RG_INVALID_HANDLE;
    if ( halfRes ) {
        gtaoGraph.AddPass( RG_PASS_NAME( "GTAO Downsample" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
            halfDepthHandle = builder.CreateTexture( { width, height, static_cast<int>( DXGI_FORMAT_R32_FLOAT ),
                L"GtaoHalfDepth", kRgNeedsUav }, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
            pass.m_executeCallback = [this, common, width, height, halfDepthHandle]( const D3D12RenderGraph& g, D3D12CmdList& cmdList ) {
                D3D12RenderTarget* halfDepth = g.GetPhysicalTexture( halfDepthHandle );
                if ( !halfDepth ) return;   // arena exhausted; logged once by it
                GtaoBindings b = common;
                b.RawDepthIndex = m_DepthSrvSlot;   // the one stage reading native-res depth directly
                b.Out0Index = halfDepth->GetUavSlot();
                cmdList.SetComputeRootSignature( m_Pipelines.Gtao.RootSig.Get() );
                cmdList.SetComputeRoot32BitConstants( 1, kGtaoBindingConstants, &b, 0 );
                cmdList.SetPipelineState( m_Pipelines.Gtao.DownsamplePSO.Get() );
                cmdList.Dispatch( ( width + 7 ) / 8, ( height + 7 ) / 8, 1 );
                };
            } );
    }

    // --- 1. Prefilter depths ---------------------------------------------------------------------------------
    // Writes m_GtaoWorkingDepth, which is NOT graph-tracked, so this pass declares no Write() of its
    // own - only the Read() of the graph-owned half-depth in Half mode (never dead-pass-eliminated,
    // same as DoF's Prepare/Restore).
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
    // Only when there's no prepass G-buffer to take real shading normals from instead.
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
        // m_GtaoWorkingDepth is NOT graph-tracked; Prefilter left it in UNORDERED_ACCESS, flip to shader-read.
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
    // Ping-pong between the two working AO terms; the last pass writes m_AOMask through its R8_UINT
    // alias. aoTermHandle[1] is created lazily the first time a non-last iteration needs it.
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
                // Writes m_AOMask, which the graph doesn't track - without this, dead-pass elimination
                // would skip the callback entirely (see D3D12DoF.cpp's Composite pass).
                builder.MarkExternalEffect();
                // Every resting-state restore for a resource borrowed from outside the graph, folded onto
                // this final pass. Must wait for TransitionExternalAfter (after the dispatch), not a
                // pre-pass transition, since m_AOMask isn't written until this callback runs.
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
                cmdList.Dispatch( ( width + 15 ) / 16, ( height + 7 ) / 8, 1 ); // each thread handles 2 horizontal pixels
                };
            } );

        if ( !lastPass ) srcTerm = curDst;
    }

    gtaoGraph.Compile();
    gtaoGraph.Execute( m_CmdList );

    // All external-resource restores ride the last "GTAO Denoise" pass's TransitionExternalAfter batch
    // above - nothing left to restore here.
    m_AOMaskInPixelState = true;
    m_ActiveAOMaskSrvSlot = m_AOMaskSrvSlot;
}
