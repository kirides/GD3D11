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

/** (Re)creates the four private intermediates and their persistent heap slots. Called from the same resize path
    as CreateAOResources — which owns m_AOMask, the output this pass shares with the simple SSAO path, so this
    must run AFTER it. Heap slots persist across recreations; only resources and views are rebuilt. */
bool D3D12GraphicsEngine::CreateGtaoResources( INT2 size ) {
    m_GtaoResourcesReady = false;
    // The working-depth pyramid is a fixed 5 levels (XE_GTAO_DEPTH_MIP_LEVELS is hard-coded upstream), and
    // CreateResource rejects a MipLevels the dimensions can't support — so refuse anything under 16x16 here
    // rather than letting the allocation fail and log a scary warning at a resolution nobody plays at.
    if ( size.x < 16 || size.y < 16 ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Allocator ) return false;
    // Init runs before the first CreateSwapChain; don't re-enable the feature if the pipelines failed there.
    if ( !m_Pipelines.Gtao.RootSig || !m_Pipelines.Gtao.PrefilterPSO ) return false;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    auto makeTex = [&]( ComPtr<ID3D12Resource>& out, ComPtr<D3D12MA::Allocation>& outAlloc,
        DXGI_FORMAT format, UINT mips, const wchar_t* name ) -> bool {
            D3D12_RESOURCE_DESC dd = {};
            dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            dd.Width = static_cast<UINT64>( size.x );
            dd.Height = static_cast<UINT>( size.y );
            dd.DepthOrArraySize = 1;
            dd.MipLevels = static_cast<UINT16>( mips );
            dd.Format = format;
            dd.SampleDesc.Count = 1;
            dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr, outAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
                LogWarn() << "D3D12: failed to create an XeGTAO texture (" << size.x << "x" << size.y << ").";
                return false;
            }
            out->SetName( name );
            return true;
        };

    // R32_FLOAT, not Intel's default R16_FLOAT — see the note in the engine header: Gothic's view depths run
    // past fp16's 65504 ceiling near the horizon, and FP32 keeps every element type a plain `float`, which is
    // what makes the bindless fetches in the shader trivial.
    if ( !makeTex( m_GtaoWorkingDepth, m_GtaoWorkingDepthAlloc, DXGI_FORMAT_R32_FLOAT, kGtaoDepthMipLevels, L"GtaoWorkingDepth" ) )
        return false;
    if ( !makeTex( m_GtaoNormals, m_GtaoNormalsAlloc, DXGI_FORMAT_R32_UINT, 1, L"GtaoNormals" ) )
        return false;
    if ( !makeTex( m_GtaoEdges, m_GtaoEdgesAlloc, DXGI_FORMAT_R8_UNORM, 1, L"GtaoEdges" ) )
        return false;
    if ( !makeTex( m_GtaoAOTerm[0], m_GtaoAOTermAlloc[0], DXGI_FORMAT_R8_UINT, 1, L"GtaoAOTerm0" )
        || !makeTex( m_GtaoAOTerm[1], m_GtaoAOTermAlloc[1], DXGI_FORMAT_R8_UINT, 1, L"GtaoAOTerm1" ) )
        return false;

    auto ensureSlot = [&]( UINT& slot ) -> bool {
        if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
        return slot != UINT_MAX;
        };
    if ( !ensureSlot( m_GtaoWorkingDepthSrvSlot ) || !ensureSlot( m_GtaoNormalsSrvSlot )
        || !ensureSlot( m_GtaoNormalsUavSlot ) || !ensureSlot( m_GtaoEdgesSrvSlot )
        || !ensureSlot( m_GtaoEdgesUavSlot ) )
        return false;
    for ( UINT m = 0; m < kGtaoDepthMipLevels; ++m ) {
        if ( !ensureSlot( m_GtaoWorkingDepthUavSlot[m] ) ) return false;
    }
    for ( UINT i = 0; i < 2; ++i ) {
        if ( !ensureSlot( m_GtaoAOTermSrvSlot[i] ) || !ensureSlot( m_GtaoAOTermUavSlot[i] ) ) return false;
    }

    // Working depth: one SRV over the WHOLE chain (the main pass samples explicit MIP levels through it) plus
    // one UAV per level, because the prefilter writes all five in a single dispatch.
    {
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
    }

    auto makeViews = [&]( ID3D12Resource* res, DXGI_FORMAT format, UINT srvSlot, UINT uavSlot ) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView( res, &srv, GetSrvCpuHandle( srvSlot ) );

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView( res, nullptr, &uav, GetSrvCpuHandle( uavSlot ) );
        };

    makeViews( m_GtaoNormals.Get(), DXGI_FORMAT_R32_UINT, m_GtaoNormalsSrvSlot, m_GtaoNormalsUavSlot );
    makeViews( m_GtaoEdges.Get(), DXGI_FORMAT_R8_UNORM, m_GtaoEdgesSrvSlot, m_GtaoEdgesUavSlot );
    for ( UINT i = 0; i < 2; ++i )
        makeViews( m_GtaoAOTerm[i].Get(), DXGI_FORMAT_R8_UINT, m_GtaoAOTermSrvSlot[i], m_GtaoAOTermUavSlot[i] );

    m_GtaoResourcesReady = true;
    return true;
}

/** XeGTAO is the selected AO mode and everything it needs exists. Shares m_AOMask and the depth-prepass depth
    with the simple SSAO path, so it depends on that path's resources being ready too. */
bool D3D12GraphicsEngine::IsGtaoEnabled() const {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( settings.AoMode != AOMode::AO_ASSAO ) return false;
    if ( !m_GtaoResourcesReady || !m_AOResourcesReady ) return false;
    if ( !m_DepthBuffer || m_DepthSrvSlot == UINT_MAX ) return false;
    if ( m_AOMaskUintUavSlot == UINT_MAX ) return false;
    const auto& p = m_Pipelines.Gtao;
    if ( !p.RootSig || !p.PrefilterPSO || !p.NormalsPSO || !p.DenoisePSO || !p.DenoiseLastPSO ) return false;
    const int quality = std::clamp( settings.GtaoSettings.QualityLevel, 0, 3 );
    return p.MainPSO[quality] != nullptr;
}

/** The full chain. Caller (RenderSSAO) has already defaulted m_ActiveAOMaskSrvSlot to the white "no occlusion" texture and
    verified IsGtaoEnabled(). */
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

    const UINT width = static_cast<UINT>( m_Resolution.x );
    const UINT height = static_cast<UINT>( m_Resolution.y );

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
    // when the G-buffer PSOs/targets failed to create.
    const bool gbufNormals = MotionGBufferActive() && m_NormalBuffer && m_NormalSrvSlot != UINT_MAX;

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

    // --- Barriers --------------------------------------------------------------------------------------------
    // Every private intermediate rests in UNORDERED_ACCESS. The depth buffer comes out of the prepass in
    // DEPTH_WRITE and has to be flipped to a shader-read state (and back, at the bottom); the normal G-buffer
    // comes out of it in RENDER_TARGET and gets the same treatment, since FillCameraVelocity at the end of the
    // frame is what normally moves it and still expects to find it there.
    BeginAoDepthRead();
    if ( gbufNormals ) {
        m_CmdList->TransitionBarrier( m_NormalBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
    }
    // ...and flip m_AOMask back if a previous successful AO run left it readable for the lit passes.
    if ( m_AOMaskInPixelState ) {
        m_CmdList->TransitionBarrier( m_AOMask.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        m_AOMaskInPixelState = false;
    }

    auto toRead = [&]( ID3D12Resource* res ) {
        m_CmdList->TransitionBarrier( res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        };
    auto toWrite = [&]( ID3D12Resource* res ) {
        m_CmdList->TransitionBarrier( res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        };

    // --- 1. Prefilter depths ---------------------------------------------------------------------------------
    // 8x8 threads, each handling a 2x2 block: one group covers 16x16 pixels.
    {
        GtaoBindings b = common;
        b.RawDepthIndex = m_DepthSrvSlot;
        b.Out0Index = m_GtaoWorkingDepthUavSlot[0];
        b.Out1Index = m_GtaoWorkingDepthUavSlot[1];
        b.Out2Index = m_GtaoWorkingDepthUavSlot[2];
        b.Out3Index = m_GtaoWorkingDepthUavSlot[3];
        b.Out4Index = m_GtaoWorkingDepthUavSlot[4];
        m_CmdList->SetComputeRoot32BitConstants( 1, kGtaoBindingConstants, &b, 0 );
        m_CmdList->SetPipelineState( m_Pipelines.Gtao.PrefilterPSO.Get() );
        m_CmdList->Dispatch( ( width + 15 ) / 16, ( height + 15 ) / 16, 1 );
    }

    // --- 2. Normals ------------------------------------------------------------------------------------------
    // Only when there is no prepass G-buffer to take them from: that one already holds this frame's real
    // shading normals for every prepass writer, so reconstructing worse ones from depth would be pure cost.
    if ( !gbufNormals ) {
        GtaoBindings b = common;
        b.RawDepthIndex = m_DepthSrvSlot;
        b.Out0Index = m_GtaoNormalsUavSlot;
        m_CmdList->SetComputeRoot32BitConstants( 1, kGtaoBindingConstants, &b, 0 );
        m_CmdList->SetPipelineState( m_Pipelines.Gtao.NormalsPSO.Get() );
        m_CmdList->Dispatch( ( width + 7 ) / 8, ( height + 7 ) / 8, 1 );
    }

    // --- 3. The GTAO integral --------------------------------------------------------------------------------
    toRead( m_GtaoWorkingDepth.Get() );
    if ( !gbufNormals ) toRead( m_GtaoNormals.Get() );
    {
        GtaoBindings b = common;
        b.WorkingDepthIndex = m_GtaoWorkingDepthSrvSlot;
        b.NormalsIndex = gbufNormals ? m_NormalSrvSlot : m_GtaoNormalsSrvSlot;
        b.Out0Index = m_GtaoAOTermUavSlot[0];
        b.Out1Index = m_GtaoEdgesUavSlot;
        m_CmdList->SetComputeRoot32BitConstants( 1, kGtaoBindingConstants, &b, 0 );
        m_CmdList->SetPipelineState( m_Pipelines.Gtao.MainPSO[quality].Get() );
        m_CmdList->Dispatch( ( width + 7 ) / 8, ( height + 7 ) / 8, 1 );
    }

    // --- 4. Denoise ------------------------------------------------------------------------------------------
    // Ping-pong between the two working AO terms; the last pass writes m_AOMask through its R8_UINT alias.
    // Each thread handles 2 horizontal pixels, hence the doubled X divisor.
    toRead( m_GtaoAOTerm[0].Get() );
    toRead( m_GtaoEdges.Get() );
    int srcTerm = 0;   // the only working term currently in a shader-read state
    for ( int pass = 0; pass < denoisePassCount; ++pass ) {
        const bool lastPass = ( pass == denoisePassCount - 1 );
        const int dstTerm = 1 - srcTerm;

        GtaoBindings b = common;
        b.AOTermIndex = m_GtaoAOTermSrvSlot[srcTerm];
        b.EdgesIndex = m_GtaoEdgesSrvSlot;
        b.Out0Index = lastPass ? m_AOMaskUintUavSlot : m_GtaoAOTermUavSlot[dstTerm];
        m_CmdList->SetComputeRoot32BitConstants( 1, kGtaoBindingConstants, &b, 0 );
        m_CmdList->SetPipelineState( ( lastPass ? m_Pipelines.Gtao.DenoiseLastPSO : m_Pipelines.Gtao.DenoisePSO ).Get() );
        m_CmdList->Dispatch( ( width + 15 ) / 16, ( height + 7 ) / 8, 1 );

        if ( !lastPass ) {
            // Hand off: the freshly written term becomes next pass's source, the consumed one goes back to
            // its rest state so the swap leaves exactly one term readable — the invariant srcTerm relies on.
            toRead( m_GtaoAOTerm[dstTerm].Get() );
            toWrite( m_GtaoAOTerm[srcTerm].Get() );
            srcTerm = dstTerm;
        }
    }

    // --- Rest states -----------------------------------------------------------------------------------------
    // m_AOMask readable for the lit geometry passes' bindless fetch; every private intermediate back to
    // UNORDERED_ACCESS, the depth buffer back to DEPTH_WRITE and the normal G-buffer back to RENDER_TARGET —
    // which is what the next barrier on each asserts as its "before" state. Skipping any of these produces a
    // GPU-validation RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH rather than a visible artifact.
    {
        D3D12_RESOURCE_BARRIER post[5] = {
            TransitionBarrier( m_AOMask.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ),
            TransitionBarrier( m_GtaoWorkingDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
            TransitionBarrier( m_GtaoEdges.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
            TransitionBarrier( m_GtaoAOTerm[srcTerm].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
            {},   // m_GtaoNormals, only when the reconstruction path actually ran (see below)
        };
        UINT postCount = 4;
        if ( !gbufNormals ) {
            post[postCount++] = TransitionBarrier( m_GtaoNormals.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        }
        m_CmdList->ResourceBarrier( postCount, post );
    }
    if ( gbufNormals ) {
        m_CmdList->TransitionBarrier( m_NormalBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
    }
    EndAoDepthRead();

    m_AOMaskInPixelState = true;
    m_ActiveAOMaskSrvSlot = m_AOMaskSrvSlot;
}
