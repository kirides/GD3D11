// D3D12GraphicsEngine — depth of field.
//
// Compute port of D3D11PFX_DepthOfField::RenderCS. The three passes and all of their maths live in
// Shaders/D3D12/DoF.hlsl (which documents where it diverges from the three D3D11 CS_PFX_DoF* shaders — only in
// the plumbing); this file is the host side: the resources, the focus ping-pong and the dispatches.
//
// FRAME SLOT. RenderDepthOfField runs on the LINEAR HDR scene colour, after the TAA resolve and before bloom.
// That is D3D11's slot exactly: DoF is the first pass of its "Post-processing B" block, which sits after the
// upscale/TAA stage and ahead of bloom and the tonemap. Blurring before bloom is what makes an out-of-focus
// highlight bloom as the disc it has become rather than as the point it was; being after TAA keeps a moving
// focus point from smearing through the temporal history.
//
// TRANSPARENCY TO THE REST OF THE CHAIN. Scene colour in, scene colour out (the composite goes to a scratch
// texture and is copied back), so nothing downstream has to know the pass ran.
//
// RENDER-GRAPH USE (first live consumer of D3D12RenderGraph's actual resource system, not just its pass
// structure — see D3D12RenderGraph.h / D3D12AliasedTextureArena.h). The half-res blur target and the full-res
// composite scratch are PURELY transient within one call of RenderDepthOfField — written, read once, then
// dead — so unlike the focus ping-pong (which must survive across frames for the temporal smoothing to mean
// anything) they are acquired fresh from a local D3D12RenderGraph each call instead of being held as members.
// They are NOT resized/recreated explicitly on a resolution change any more either: since they are asked for
// at the CURRENT m_Resolution every call, a resize just makes next frame's CreateTexture() ask for a
// different size, which the graph handles the same way it handles any other description change.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12ResourceCreate.h"
#include "D3D12RenderGraph.h"
#include "../Engine.h"
#include "../GothicAPI.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    // Combined shader-read state for the depth buffer, matching what the fog/god-ray and AO passes use: the
    // blur and composite read it from compute (NON_PIXEL), and keeping PIXEL in the mask costs nothing while
    // making the state identical to the one every other depth-reading pass parks it in.
    constexpr D3D12_RESOURCE_STATES kDoFDepthRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // D3D12RenderGraph's transient-texture flag convention (see RGTextureDesc::textureFlags / RGBuilder's
    // consumer in D3D12RenderGraph.cpp): bit 0 requests a UAV alongside the SRV. Both DoF scratch targets are
    // compute-written, so both need it.
    constexpr uint32_t kRgNeedsUav = 1u;
}

/** (Re)creates the temporally-persistent focus ping-pong pair only. The half-res blur target and the
    full-res composite scratch used to live here too; they are now D3D12RenderGraph-managed transient
    textures acquired inside RenderDepthOfField (see the file header) and need no explicit creation or
    resize handling.

    Called LAZILY from RenderDepthOfField the first time DoF is actually switched on, and from the resize path
    only if it already ran once — see the header note on why this is not built unconditionally like the bloom
    pyramid. Heap slots are allocated once and re-pointed at the fresh resources. Non-fatal: on failure
    m_DoFResourcesReady stays false and RenderDepthOfField no-ops, leaving the scene in focus. */
bool D3D12GraphicsEngine::CreateDoFResources( INT2 size ) {
    m_DoFResourcesReady = false;
    m_DoFCreateAttempted = true;
    if ( size.x < 4 || size.y < 4 ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Allocator ) return false;
    // Init runs before the first CreateSwapChain; don't build resources for a pipeline that failed there.
    if ( !m_Pipelines.DoF.FocusPSO || !m_Pipelines.DoF.CompositePSO ) return false;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    auto ensureSlot = [&]( UINT& slot ) -> bool {
        if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
        return slot != UINT_MAX;
        };

    auto makeTex = [&]( int w, int h, DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& out,
        ComPtr<D3D12MA::Allocation>& outAlloc, const wchar_t* name ) -> bool {
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = static_cast<UINT64>( w );
        dd.Height = static_cast<UINT>( h );
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = fmt;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        // Rests in UNORDERED_ACCESS between frames (see RenderDepthOfField), so create it in it — that makes
        // the "before" state at the top of the very first frame deterministic.
        if ( FAILED( D3D12ResourceCreate::CreateTexture( m_Allocator.Get(), heapDefault, dd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, outAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create a depth-of-field focus texture (" << w << "x" << h << ").";
            return false;
        }
        out->SetName( name );
        return true;
        };

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    // Focus pair: 1x1 R32_FLOAT, the auto-focus distance. Resolution-independent, but rebuilt with the rest
    // so there is exactly one creation path; the history is worthless across a resolution change anyway.
    for ( UINT i = 0; i < 2; ++i ) {
        if ( !makeTex( 1, 1, DXGI_FORMAT_R32_FLOAT, m_DoFFocus[i], m_DoFFocusAlloc[i],
            i == 0 ? L"DoFFocus0" : L"DoFFocus1" ) ) return false;
        if ( !ensureSlot( m_DoFFocusSrvSlot[i] ) || !ensureSlot( m_DoFFocusUavSlot[i] ) ) return false;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateShaderResourceView( m_DoFFocus[i].Get(), &srv, GetSrvCpuHandle( m_DoFFocusSrvSlot[i] ) );
        device->CreateUnorderedAccessView( m_DoFFocus[i].Get(), nullptr, &uav, GetSrvCpuHandle( m_DoFFocusUavSlot[i] ) );
    }
    m_DoFFocusIndex = 0;
    m_DoFFocusValid = false;   // contents are undefined until the first resolve writes one of them

    m_DoFResourcesReady = true;
    return true;
}


/** Focus resolve -> half-res blur -> full-res composite -> copy back over the scene colour, the last three
    steps issued through a local D3D12RenderGraph (see the file header). */
void D3D12GraphicsEngine::RenderDepthOfField() {
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( !settings.EnableDoF ) return;
    if ( !m_FrameOpen || !m_CmdList || !m_SceneColor || m_SceneColorSrvSlot == UINT_MAX
        || !m_DepthBuffer || m_DepthSrvSlot == UINT_MAX )
        return;
    if ( !m_Pipelines.DoF.RootSig || !m_Pipelines.DoF.FocusPSO || !m_Pipelines.DoF.CompositePSO )
        return;

    // Lazily build the focus ping-pong the first time DoF is switched on. Creating a resource mid-frame is
    // safe: nothing is destroyed here, so there is no in-flight resource to fence against.
    // m_DoFCreateAttempted keeps a failure from retrying every frame; cleared on resize.
    if ( !m_DoFResourcesReady ) {
        if ( m_DoFCreateAttempted ) return;
        if ( !CreateDoFResources( m_Resolution ) ) {
            LogWarn() << "D3D12: depth of field is enabled but its focus texture could not be created — DoF disabled.";
            return;
        }
    }

    ID3D12PipelineState* blurPso = settings.DoFGaussBlur
        ? m_Pipelines.DoF.GaussPSO.Get()
        : m_Pipelines.DoF.BlurPSO.Get();
    if ( !blurPso ) return;

    DX_ZONE( m_CmdList.Get(), "Depth of Field" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "Depth of Field" );

    const UINT prevIdx = m_DoFFocusIndex;
    const UINT curIdx = 1 - m_DoFFocusIndex;
    const INT2 halfSize = { std::max( 1, m_Resolution.x / 2 ), std::max( 1, m_Resolution.y / 2 ) };

    // b0 DoFCB — see Shaders/D3D12/DoF.hlsl. One block for all three passes; the per-pass fields (OutIndex and
    // the output resolution) are rewritten between dispatches, everything else is set once here.
    struct DoFConsts {
        float    FocusRange;
        float    BokehRadius;
        float    MaxBlur;
        uint32_t FocusValid;
        uint32_t SceneIndex;
        uint32_t DepthIndex;
        uint32_t PrevFocusIndex;
        uint32_t FocusIndex;
        uint32_t BlurIndex;
        uint32_t FocusUavIndex;
        uint32_t OutIndex;
        uint32_t Pad;
        float    FullResX;
        float    FullResY;
        float    OutResX;
        float    OutResY;
    } cb = {};
    static_assert( sizeof( DoFConsts ) == 16 * sizeof( uint32_t ),
        "DoFConsts must match DoF.hlsl's b0 DoFCB and the 16 root constants pushed below" );

    // The same three tuning values D3D11PFX_DepthOfField pushes, straight from the shared settings. D3D11 also
    // uploads DoF_ProjParams / near / far, which none of the three compute shaders read (depth linearization is
    // the parameter-free reversed-Z reciprocal) — so they are not carried over.
    cb.FocusRange = settings.DoFFocusRange;
    cb.BokehRadius = settings.DoFBokehRadius;
    cb.MaxBlur = settings.DoFMaxBlur;
    cb.FocusValid = m_DoFFocusValid ? 1u : 0u;
    cb.SceneIndex = m_SceneColorSrvSlot;
    cb.DepthIndex = m_DepthSrvSlot;
    cb.PrevFocusIndex = m_DoFFocusSrvSlot[prevIdx];
    cb.FocusIndex = m_DoFFocusSrvSlot[curIdx];
    cb.FocusUavIndex = m_DoFFocusUavSlot[curIdx];
    cb.FullResX = static_cast<float>( m_Resolution.x );
    cb.FullResY = static_cast<float>( m_Resolution.y );

    // Scene colour RENDER_TARGET -> compute-readable, depth out of DEPTH_WRITE, previous focus UAV -> readable.
    // The DSV/RTV must be unbound first: a resource cannot be bound as a render target while it is read through
    // an SRV (same dance RenderTAA / RenderBloom / RenderFogAndGodRays do). None of this touches the graph's
    // resources, so it happens once, directly, before the graph runs.
    m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );
    {
        D3D12ResourceTransition pre[3];
        UINT n = 0;
        if ( !m_SceneColorInPixelState ) {
            pre[n++] = { m_SceneColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        }
        pre[n++] = { m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, kDoFDepthRead };
        pre[n++] = { m_DoFFocus[prevIdx].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        m_CmdList->TransitionBarriers( pre, n );
        m_SceneColorInPixelState = true;
    }

    D3D12RenderGraph dofGraph( &m_AliasArena );
    RGResourceHandle halfHandle = RG_INVALID_HANDLE;
    RGResourceHandle compositeHandle = RG_INVALID_HANDLE;
    bool compositeCopied = false;   // did the composite pass below actually run the copy-back?

    // --- Pass 0: focus resolve (1x1) --- Writes through FocusUavIndex, not OutIndex, and reads neither OutRes
    // — it is the one pass whose output is a fixed 1x1 texel, so those three fields stay at zero-init here.
    // Touches no graph resources (m_DoFFocus is a persistent member pair, not transient), but the curIdx/
    // prevIdx UAV<->SRV handover it performs must happen before the blur pass below reads FocusIndex.
    dofGraph.AddPass( RG_PASS_NAME( "DoF Focus Resolve" ), [&]( D3D12RGBuilder&, D3D12RenderPass& pass ) {
        pass.m_executeCallback = [this, prevIdx, curIdx, &cb]( const D3D12RenderGraph&, D3D12CmdList& cmdList ) {
            cmdList.SetComputeRootSignature( m_Pipelines.DoF.RootSig.Get() );
            cmdList.SetPipelineState( m_Pipelines.DoF.FocusPSO.Get() );
            cmdList.SetComputeRoot32BitConstants( 0, 16, &cb, 0 );
            cmdList.Dispatch( 1, 1, 1 );

            // This frame's focus value becomes the read side for the two passes below; hand the previous one
            // back to its resting UAV state. A UAV-write -> SRV-read handover needs a real state transition,
            // not a UAV barrier: a UAV barrier only orders UAV access and performs no cache flush, so the SRV
            // read would be undefined.
            cmdList.TransitionBarriers( {
                { m_DoFFocus[curIdx].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                { m_DoFFocus[prevIdx].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
                } );
            m_DoFFocusIndex = curIdx;
            m_DoFFocusValid = true;
            };
        } );

    // --- Pass 1: half-res bokeh (or Gaussian) blur --- The first CreateTexture() this backend has ever issued
    // through D3D12RenderGraph: a real transient resource, acquired from m_AliasArena via interval coloring
    // rather than held as a member. tex->State is caller-maintained (see D3D12RenderTarget.h) — check it
    // rather than assuming, since a freshly (re)placed resource starts at RENDER_TARGET while a reused one
    // already sits wherever last frame's DoF pass left it.
    dofGraph.AddPass( RG_PASS_NAME( "DoF Half-Res Blur" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
        halfHandle = builder.CreateTexture( { static_cast<uint32_t>( halfSize.x ), static_cast<uint32_t>( halfSize.y ),
            static_cast<int>( kSceneColorFormat ), L"DoFHalf", kRgNeedsUav } );

        pass.m_executeCallback = [this, blurPso, halfSize, &cb, halfHandle]( const D3D12RenderGraph& graph, D3D12CmdList& cmdList ) {
            D3D12RenderTarget* half = graph.GetPhysicalTexture( halfHandle );
            if ( !half ) return;   // arena exhausted or creation failed (logged once by the arena) — skip the blur

            if ( half->State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) {
                cmdList.TransitionBarrier( half->GetResource(), half->State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
                half->State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }

            cmdList.SetComputeRootSignature( m_Pipelines.DoF.RootSig.Get() );
            cmdList.SetPipelineState( blurPso );
            cb.OutIndex = half->GetUavSlot();
            cb.OutResX = static_cast<float>( halfSize.x );
            cb.OutResY = static_cast<float>( halfSize.y );
            cmdList.SetComputeRoot32BitConstants( 0, 16, &cb, 0 );
            cmdList.Dispatch( ( halfSize.x + 7 ) / 8, ( halfSize.y + 7 ) / 8, 1 );

            // The composite pass below samples this through BlurIndex (its SRV slot).
            cmdList.TransitionBarrier( half->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
            half->State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            };
        } );

    // --- Pass 2: full-res composite into a graph-managed scratch texture, then copy back onto the scene colour ---
    dofGraph.AddPass( RG_PASS_NAME( "DoF Composite" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
        builder.Read( halfHandle );
        compositeHandle = builder.CreateTexture( { static_cast<uint32_t>( m_Resolution.x ), static_cast<uint32_t>( m_Resolution.y ),
            static_cast<int>( kSceneColorFormat ), L"DoFComposite", kRgNeedsUav } );
        // CreateTexture() already declares compositeHandle read (see D3D12RGBuilder::CreateTexture), so the
        // resource itself is allocated. What's still missing is that its VISIBLE result leaves the graph
        // entirely via the CopyResource onto m_SceneColor below — a plain member, not a resource the graph
        // owns — so nothing the graph can see depends on this pass having run. Without MarkExternalEffect,
        // Execute()'s dead-pass elimination would see a self-contained write/read pair with no visible
        // external effect and skip the callback. See D3D12RenderPass::m_hasExternalSideEffect.
        builder.MarkExternalEffect();

        pass.m_executeCallback = [this, &cb, &compositeCopied, halfHandle, compositeHandle]( const D3D12RenderGraph& graph, D3D12CmdList& cmdList ) {
            D3D12RenderTarget* half = graph.GetPhysicalTexture( halfHandle );
            D3D12RenderTarget* composite = graph.GetPhysicalTexture( compositeHandle );
            if ( !half || !composite ) return;   // scene colour / depth / focus are still restored below unconditionally

            if ( composite->State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) {
                cmdList.TransitionBarrier( composite->GetResource(), composite->State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
                composite->State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }

            cmdList.SetComputeRootSignature( m_Pipelines.DoF.RootSig.Get() );
            cmdList.SetPipelineState( m_Pipelines.DoF.CompositePSO.Get() );
            cb.BlurIndex = half->GetSrvSlot();
            cb.OutIndex = composite->GetUavSlot();
            cb.OutResX = static_cast<float>( m_Resolution.x );
            cb.OutResY = static_cast<float>( m_Resolution.y );
            cmdList.SetComputeRoot32BitConstants( 0, 16, &cb, 0 );
            cmdList.Dispatch( ( m_Resolution.x + 7 ) / 8, ( m_Resolution.y + 7 ) / 8, 1 );

            // Hand the result back to the scene colour. Both are kSceneColorFormat, so this is a straight copy.
            cmdList.TransitionBarriers( {
                { composite->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE },
                { m_SceneColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST },
                } );
            cmdList.CopyResource( m_SceneColor.Get(), composite->GetResource() );
            composite->State = D3D12_RESOURCE_STATE_COPY_SOURCE;
            compositeCopied = true;
            };
        } );

    dofGraph.Compile();
    dofGraph.Execute( m_CmdList );

    // Resting states for the engine-owned resources DoF borrowed — NOT graph-managed, so nothing else restores
    // them, and this must run regardless of whether the composite pass above actually found its textures (see
    // its early-out). Depth back to DEPTH_WRITE, scene colour back to RENDER_TARGET (from COPY_DEST if the
    // copy-back ran, otherwise from the plain shader-read state the pre-pass block put it in), this frame's
    // focus texture back to its UAV resting state for whenever it becomes "prevIdx" again.
    m_CmdList->TransitionBarriers( {
        { m_DepthBuffer.Get(), kDoFDepthRead, D3D12_RESOURCE_STATE_DEPTH_WRITE },
        { m_SceneColor.Get(), compositeCopied ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET },
        { m_DoFFocus[curIdx].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
        } );
    m_SceneColorInPixelState = false;

    // Rebind the scene colour + depth for whatever comes next in the frame (bloom re-transitions the scene
    // colour itself, but the render target must not be left unbound).
    BindSceneColorTarget();
}
