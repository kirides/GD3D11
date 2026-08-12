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

/** (Re)creates the quarter-res blur ping-pong pair.

    Called LAZILY from DrawUnderwaterEffects the first time the camera actually goes under water, and from the
    resize path only if the pair already exists — same policy as the DoF textures, and for the same reason:
    most sessions never trigger this and 32-bit address space is the scarce resource (see CLAUDE.md). Heap
    slots are allocated once and re-pointed at the fresh resources. Non-fatal: on failure
    m_UnderwaterResourcesReady stays false and DrawUnderwaterEffects no-ops, leaving the frame untinted. */
bool D3D12GraphicsEngine::CreateUnderwaterResources( INT2 size ) {
    m_UnderwaterResourcesReady = false;
    m_UnderwaterCreateAttempted = true;
    if ( size.x < 4 || size.y < 4 ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Allocator ) return false;
    // Init runs before the first CreateSwapChain; don't build resources for a pipeline that failed there.
    if ( !m_Pipelines.Underwater.BlurPSO || !m_Pipelines.Underwater.CompositePSO ) return false;

    // Integer-divided like D3D11's fxbuffer->GetSizeX() / 4, and never below 1 texel so a tiny window cannot
    // produce a zero-sized resource.
    m_UnderwaterBlurSize = { std::max( 1, size.x / 4 ), std::max( 1, size.y / 4 ) };

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = kUnderwaterBlurFormat;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = kUnderwaterBlurFormat;

    for ( UINT i = 0; i < 2; ++i ) {
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = static_cast<UINT64>( m_UnderwaterBlurSize.x );
        dd.Height = static_cast<UINT>( m_UnderwaterBlurSize.y );
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = kUnderwaterBlurFormat;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        // Both rest in UNORDERED_ACCESS between frames (see DrawUnderwaterEffects), so create them in it —
        // that makes the "before" state at the top of the pass deterministic on the very first frame.
        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, m_UnderwaterBlurAlloc[i].ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_UnderwaterBlur[i].ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create an underwater blur texture ("
                << m_UnderwaterBlurSize.x << "x" << m_UnderwaterBlurSize.y << ").";
            return false;
        }
        m_UnderwaterBlur[i]->SetName( i == 0 ? L"UnderwaterBlurH" : L"UnderwaterBlurV" );

        if ( m_UnderwaterBlurSrvSlot[i] == UINT_MAX ) m_UnderwaterBlurSrvSlot[i] = AllocateSrvSlot();
        if ( m_UnderwaterBlurUavSlot[i] == UINT_MAX ) m_UnderwaterBlurUavSlot[i] = AllocateSrvSlot();
        if ( m_UnderwaterBlurSrvSlot[i] == UINT_MAX || m_UnderwaterBlurUavSlot[i] == UINT_MAX ) return false;

        device->CreateShaderResourceView( m_UnderwaterBlur[i].Get(), &srv, GetSrvCpuHandle( m_UnderwaterBlurSrvSlot[i] ) );
        device->CreateUnorderedAccessView( m_UnderwaterBlur[i].Get(), nullptr, &uav, GetSrvCpuHandle( m_UnderwaterBlurUavSlot[i] ) );
    }

    m_UnderwaterResourcesReady = true;
    return true;
}


/** Blur H -> blur V (both quarter-res, both tinted) -> distorted full-res composite over the display target. */
void D3D12GraphicsEngine::DrawUnderwaterEffects() {
    if ( !Engine::GAPI->IsUnderWater() ) return;
    if ( !m_FrameOpen || !m_CmdList || !m_SwapChainReady || !m_LdrCopyReady ) return;
    if ( !m_Pipelines.Underwater.BlurRootSig || !m_Pipelines.Underwater.BlurPSO
        || !m_Pipelines.Underwater.CompositeRootSig || !m_Pipelines.Underwater.CompositePSO )
        return;

    // Lazily build the quarter-res pair the first time the player submerges. Creating a resource mid-frame is
    // safe: nothing is destroyed here, so there is no in-flight resource to fence against.
    if ( !m_UnderwaterResourcesReady ) {
        if ( m_UnderwaterCreateAttempted ) return;
        if ( !CreateUnderwaterResources( m_BackbufferResolution ) ) {
            LogWarn() << "D3D12: the camera is under water but the underwater blur textures could not be "
                         "created — the underwater effect is disabled.";
            return;
        }
    }
    // Resolution changed without the resources following (the resize path only rebuilds them if they already
    // exist, so this is a belt-and-braces check rather than an expected state).
    if ( m_UnderwaterBlurSize.x != std::max( 1, m_BackbufferResolution.x / 4 )
        || m_UnderwaterBlurSize.y != std::max( 1, m_BackbufferResolution.y / 4 ) )
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
        auto b = TransitionBarrier( displayTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE );
        m_CmdList->ResourceBarrier( 1, &b );
    }
    m_CmdList->CopyResource( m_LdrCopy.Get(), displayTarget );
    {
        D3D12_RESOURCE_BARRIER b[2] = {
            // NON_PIXEL: the two blur passes read it from compute.
            TransitionBarrier( m_LdrCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ),
            TransitionBarrier( displayTarget, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET ),
        };
        m_CmdList->ResourceBarrier( 2, b );
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

    std::copy( std::begin( kUnderwaterColorMod ), std::end( kUnderwaterColorMod ), std::begin( cb.ColorMod ) );
    cb.OutResX = static_cast<float>( m_UnderwaterBlurSize.x );
    cb.OutResY = static_cast<float>( m_UnderwaterBlurSize.y );
    cb.BlurSize = kUnderwaterBlurSize;

    m_CmdList->SetComputeRootSignature( m_Pipelines.Underwater.BlurRootSig.Get() );
    m_CmdList->SetPipelineState( m_Pipelines.Underwater.BlurPSO.Get() );

    const UINT groupsX = ( static_cast<UINT>( m_UnderwaterBlurSize.x ) + 7 ) / 8;
    const UINT groupsY = ( static_cast<UINT>( m_UnderwaterBlurSize.y ) + 7 ) / 8;

    // --- Pass 1: horizontal, full-res frame -> blur[0] (this is also the 4x downscale). ---
    cb.SrcIndex = m_LdrCopySrvSlot;
    cb.OutIndex = m_UnderwaterBlurUavSlot[0];
    cb.TexelStepX = 1.0f / m_UnderwaterBlurSize.x;
    cb.TexelStepY = 0.0f;
    m_CmdList->SetComputeRoot32BitConstants( 0, 12, &cb, 0 );
    m_CmdList->Dispatch( groupsX, groupsY, 1 );

    // UAV-write -> SRV-read needs a real state transition, not a UAV barrier: a UAV barrier only orders access
    // and performs no cache flush, so the SRV read would be undefined.
    {
        auto b = TransitionBarrier( m_UnderwaterBlur[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        m_CmdList->ResourceBarrier( 1, &b );
    }

    // --- Pass 2: vertical, blur[0] -> blur[1]. ---
    cb.SrcIndex = m_UnderwaterBlurSrvSlot[0];
    cb.OutIndex = m_UnderwaterBlurUavSlot[1];
    cb.TexelStepX = 0.0f;
    cb.TexelStepY = 1.0f / m_UnderwaterBlurSize.y;
    m_CmdList->SetComputeRoot32BitConstants( 0, 12, &cb, 0 );
    m_CmdList->Dispatch( groupsX, groupsY, 1 );

    {
        D3D12_RESOURCE_BARRIER b[2] = {
            TransitionBarrier( m_UnderwaterBlur[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ),
            TransitionBarrier( m_UnderwaterBlur[0].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
        };
        m_CmdList->ResourceBarrier( 2, b );
    }

    // --- Pass 3: full-res distorted composite back over the display target. ---
    struct CompositeConsts {
        uint32_t BlurIndex;
        uint32_t DistortionIndex;
        float    Time;
        float    Pad;
    } comp = {};
    static_assert( sizeof( CompositeConsts ) == 4 * sizeof( uint32_t ),
        "CompositeConsts must match Underwater.hlsl's b0 UnderwaterCompositeCB and the 4 root constants below" );
    comp.BlurIndex = m_UnderwaterBlurSrvSlot[1];
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

    // Resting states for the next frame: the scratch copy back to COPY_DEST (SMAA/sharpen expect to find it
    // there), the blur pair back to UNORDERED_ACCESS. The display target stays RENDER_TARGET and bound, ready
    // for Gothic's 2D UI/HUD to composite on top.
    {
        D3D12_RESOURCE_BARRIER b[2] = {
            TransitionBarrier( m_LdrCopy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST ),
            TransitionBarrier( m_UnderwaterBlur[1].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
        };
        m_CmdList->ResourceBarrier( 2, b );
    }
}
