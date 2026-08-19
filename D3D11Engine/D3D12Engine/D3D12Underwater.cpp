// D3D12GraphicsEngine — underwater screen effect.
//
// Port of D3D11GraphicsEngine::DrawUnderwaterEffects, which is a single call into the generic blur post-FX
// with a custom final-copy shader:
//     PfxRenderer->BlurTexture( HDRBackBuffer, false, 0.10f, UNDERWATER_COLOR_MOD,
//                               PShaderID::PS_PFX_UnderwaterFinal );
// D3D11PFX_Blur::RenderBlur expands that into quarter-res Gaussian H -> quarter-res Gaussian V -> full-res
// copy back through PS_PFX_UnderwaterFinal (which animates the UVs from distortion2.dds). All of the maths
// lives in Shaders/D3D12/Underwater.hlsl, which also documents where it deliberately reproduces D3D11
// oddities (the doubled colour mod, the distortion-texture-as-depth read); this file is the host side.
//
// FRAME SLOT. D3D11 adds its "Draw UnderwaterFX" render-graph pass after the AA/sharpen passes and after
// PresentPending is set, i.e. on the finished image and before Gothic's own 2D UI/HUD phase — the HUD must
// stay sharp and untinted. RenderSharpen() is the equivalent point on this backend, so DrawUnderwaterEffects
// is called straight after it from OnStartWorldRendering.
//
// TRANSPARENCY TO THE REST OF THE CHAIN. Display target in, display target out, and every scratch resource is
// left in the resting state the next frame expects, so nothing downstream has to know the pass ran.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12Texture.h"
#include "D3D12ResourceCreate.h"
#include "D3D12RenderGraph.h"
#include "../Engine.h"
#include "../GothicAPI.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    // D3D11GraphicsEngine.cpp's UNDERWATER_COLOR_MOD. Applied by BOTH blur passes, exactly as D3D11 does (its
    // blur CB carries B_ColorMod into the vertical pass unchanged), so the tint that reaches the screen is the
    // square of this — see the Underwater.hlsl header.
    constexpr float kUnderwaterColorMod[4] = { 0.5f, 0.7f, 1.0f, 1.0f };

    // D3D11's `scale` argument to BlurTexture. Note it is a fraction of a QUARTER-res texel, so most of the
    // softening comes from the 4x down/up sampling rather than from the kernel itself.
    constexpr float kUnderwaterBlurSize = 0.10f;

    // FP16 rather than the display format: it is the one format guaranteed to support typed UAV stores (the
    // R10G10B10A2 swapchain format is not), and it carries the HDR display target's range unclipped too.
    constexpr DXGI_FORMAT kUnderwaterBlurFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
}

/** Blur H -> blur V (both quarter-res, both tinted) -> distorted full-res composite over the display target.
    The quarter-res blur pair USED to be built lazily as members the first time the camera went under water
    (same reasoning DoF's old members had — rare state, 32-bit VA is scarce). Both are purely single-frame
    scratch (written then read once then dead, no cross-frame data dependency), so they are now
    D3D12RenderGraph-managed transient textures acquired fresh below instead — same conversion DoF's and the
    god-ray mask/zoom textures got (see D3D12DoF.cpp / D3D12Fog.cpp). */
void D3D12GraphicsEngine::DrawUnderwaterEffects() {
    if ( !Engine::GAPI->IsUnderWater() ) return;
    if ( !m_FrameOpen || !m_CmdList || !m_SwapChainReady || !m_LdrCopyReady ) return;
    if ( !m_Pipelines.Underwater.BlurRootSig || !m_Pipelines.Underwater.BlurPSO
        || !m_Pipelines.Underwater.CompositeRootSig || !m_Pipelines.Underwater.CompositePSO )
        return;

    // distortion2.dds drives both UV offsets. If it failed to load, fall back to the 1x1 white texture: that
    // degrades the animated distortion to a constant sub-pixel shift instead of dropping the blue blur too.
    const D3D12Texture* distortion = ( m_DistortionTexture && m_DistortionTexture->HasSRV() )
        ? m_DistortionTexture.get()
        : ( ( m_WhiteTexture && m_WhiteTexture->HasSRV() ) ? m_WhiteTexture.get() : nullptr );
    if ( !distortion ) return;

    DX_ZONE( m_CmdList.Get(), "Underwater FX" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "Underwater FX" );

    ID3D12Resource* displayTarget = GetDisplayTarget();
    D3D12_CPU_DESCRIPTOR_HANDLE displayRtv = GetDisplayRtv();

    // --- Copy the finished display image into m_LdrCopy (the blur's source). ---
    // A texture cannot be its own SRV and RTV, and the composite below writes the display target — same
    // copy-then-read shape RenderSMAA / RenderSharpen use. m_LdrCopy rests in COPY_DEST.
    {
        m_CmdList->TransitionBarrier( displayTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE );
    }
    m_CmdList->CopyResource( m_LdrCopy.Get(), displayTarget );
    {
        // NON_PIXEL: the two blur passes read it from compute.
        m_CmdList->TransitionBarriers( {
            { m_LdrCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { displayTarget, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET },
        } );
    }

    // b0 UnderwaterBlurCB — see Shaders/D3D12/Underwater.hlsl. Only the source/destination and the step
    // direction change between the two passes.
    struct BlurConsts {
        uint32_t SrcIndex;
        uint32_t OutIndex;
        float    TexelStepX;
        float    TexelStepY;
        float    ColorMod[4];
        float    OutResX;
        float    OutResY;
        float    BlurSize;
        float    Pad;
    } cb = {};
    static_assert( sizeof( BlurConsts ) == 12 * sizeof( uint32_t ),
        "BlurConsts must match Underwater.hlsl's b0 UnderwaterBlurCB and the 12 root constants pushed below" );

    const INT2 blurSize = { std::max( 1, m_BackbufferResolution.x / 4 ), std::max( 1, m_BackbufferResolution.y / 4 ) };
    std::copy( std::begin( kUnderwaterColorMod ), std::end( kUnderwaterColorMod ), std::begin( cb.ColorMod ) );
    cb.OutResX = static_cast<float>( blurSize.x );
    cb.OutResY = static_cast<float>( blurSize.y );
    cb.BlurSize = kUnderwaterBlurSize;

    const UINT groupsX = ( static_cast<UINT>( blurSize.x ) + 7 ) / 8;
    const UINT groupsY = ( static_cast<UINT>( blurSize.y ) + 7 ) / 8;

    // Both blur textures are purely single-frame scratch (written then read once then dead, no cross-frame
    // data dependency), so they go through a local D3D12RenderGraph — same conversion DoF's and the god-ray
    // mask/zoom textures got (see D3D12DoF.cpp / D3D12Fog.cpp).
    D3D12RenderGraph underwaterGraph( &m_AliasArena );
    RGResourceHandle blurHHandle = RG_INVALID_HANDLE;
    RGResourceHandle blurVHandle = RG_INVALID_HANDLE;
    UINT blurVSrvSlot = UINT_MAX;   // resolved by Pass 2; read by the composite draw further down

    // --- Pass 1: horizontal, full-res frame -> blurH (this is also the 4x downscale). ---
    underwaterGraph.AddPass( RG_PASS_NAME( "Underwater Blur H" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
        blurHHandle = builder.CreateTexture( { static_cast<uint32_t>( blurSize.x ), static_cast<uint32_t>( blurSize.y ),
            static_cast<int>( kUnderwaterBlurFormat ), L"UnderwaterBlurH", 1u } );

        pass.m_executeCallback = [this, &cb, groupsX, groupsY, blurHHandle]( const D3D12RenderGraph& graph, D3D12CmdList& cmdList ) {
            D3D12RenderTarget* blurH = graph.GetPhysicalTexture( blurHHandle );
            if ( !blurH ) return;

            if ( blurH->State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) {
                cmdList.TransitionBarrier( blurH->GetResource(), blurH->State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
                blurH->State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }

            cb.SrcIndex = m_LdrCopySrvSlot;
            cb.OutIndex = blurH->GetUavSlot();
            cb.TexelStepX = 1.0f / blurH->GetWidth();
            cb.TexelStepY = 0.0f;
            cmdList.SetComputeRootSignature( m_Pipelines.Underwater.BlurRootSig.Get() );
            cmdList.SetPipelineState( m_Pipelines.Underwater.BlurPSO.Get() );
            cmdList.SetComputeRoot32BitConstants( 0, 12, &cb, 0 );
            cmdList.Dispatch( groupsX, groupsY, 1 );

            // UAV-write -> SRV-read needs a real state transition, not a UAV barrier: a UAV barrier only
            // orders access and performs no cache flush, so the SRV read would be undefined.
            cmdList.TransitionBarrier( blurH->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
            blurH->State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            };
        } );

    // --- Pass 2: vertical, blurH -> blurV. ---
    underwaterGraph.AddPass( RG_PASS_NAME( "Underwater Blur V" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
        builder.Read( blurHHandle );
        blurVHandle = builder.CreateTexture( { static_cast<uint32_t>( blurSize.x ), static_cast<uint32_t>( blurSize.y ),
            static_cast<int>( kUnderwaterBlurFormat ), L"UnderwaterBlurV", 1u } );
        // The composite draw further down reads this pass's result via blurVSrvSlot, a plain local captured
        // by reference — not a graph Read(), so mark the side effect explicitly.
        builder.MarkExternalEffect();

        pass.m_executeCallback = [this, &cb, groupsX, groupsY, &blurVSrvSlot, blurHHandle, blurVHandle]( const D3D12RenderGraph& graph, D3D12CmdList& cmdList ) {
            D3D12RenderTarget* blurH = graph.GetPhysicalTexture( blurHHandle );
            D3D12RenderTarget* blurV = graph.GetPhysicalTexture( blurVHandle );
            if ( !blurH || !blurV ) return;

            if ( blurV->State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) {
                cmdList.TransitionBarrier( blurV->GetResource(), blurV->State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
                blurV->State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }

            cb.SrcIndex = blurH->GetSrvSlot();
            cb.OutIndex = blurV->GetUavSlot();
            cb.TexelStepX = 0.0f;
            cb.TexelStepY = 1.0f / blurV->GetHeight();
            cmdList.SetComputeRootSignature( m_Pipelines.Underwater.BlurRootSig.Get() );
            cmdList.SetPipelineState( m_Pipelines.Underwater.BlurPSO.Get() );
            cmdList.SetComputeRoot32BitConstants( 0, 12, &cb, 0 );
            cmdList.Dispatch( groupsX, groupsY, 1 );

            cmdList.TransitionBarrier( blurV->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
            blurV->State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            blurVSrvSlot = blurV->GetSrvSlot();
            };
        } );

    underwaterGraph.Compile();
    underwaterGraph.Execute( m_CmdList );

    // --- Pass 3: full-res distorted composite back over the display target. ---
    struct CompositeConsts {
        uint32_t BlurIndex;
        uint32_t DistortionIndex;
        float    Time;
        float    Pad;
    } comp = {};
    static_assert( sizeof( CompositeConsts ) == 4 * sizeof( uint32_t ),
        "CompositeConsts must match Underwater.hlsl's b0 UnderwaterCompositeCB and the 4 root constants below" );
    comp.BlurIndex = blurVSrvSlot;
    comp.DistortionIndex = distortion->GetSrvSlot();
    comp.Time = Engine::GAPI->GetTimeSeconds();   // D3D11's RI_Time

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_BackbufferResolution.x ), static_cast<float>( m_BackbufferResolution.y ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, m_BackbufferResolution.x, m_BackbufferResolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    m_CmdList->IASetVertexBuffers( 0, 0, nullptr );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.Underwater.CompositeRootSig.Get() );
    m_CmdList->SetPipelineState( m_Pipelines.Underwater.CompositePSO.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 4, &comp, 0 );
    m_CmdList->OMSetRenderTargets( 1, &displayRtv, FALSE, nullptr );
    m_CmdList->DrawInstanced( 3, 1, 0, 0 );

    // Resting state for the next frame: the scratch copy back to COPY_DEST (SMAA/sharpen expect to find it
    // there). The display target stays RENDER_TARGET and bound, ready for Gothic's 2D UI/HUD to composite on
    // top. The blur pair needs no explicit reset — D3D12RenderTarget::State is caller-maintained and
    // self-correcting, same as DoF's and the god-ray textures.
    m_CmdList->TransitionBarrier( m_LdrCopy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST );
}
