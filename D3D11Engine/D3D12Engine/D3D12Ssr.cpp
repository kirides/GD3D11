// D3D12GraphicsEngine — opaque-surface SSR temporal history capture.
//
// See the header comment on m_SsrPrevColor/m_SsrPrevDepth (D3D12GraphicsEngine.h) for why this is a
// TEMPORAL capture (previous frame, not this frame) rather than the same-frame copy trick D3D12Water.cpp
// uses: reflecting on-screen opaque geometry from INSIDE the Forward+ opaque pass cannot read this same
// frame's still-being-rasterized color/depth, so the source has to be one frame old.
//
// This file currently only builds and fills the two history textures — nothing reads them yet. The
// consumer (a screen-space ray march in World.hlsl/Vob.hlsl/Skeletal.hlsl, reprojected through
// m_PrevViewProjUnjittered) is the next increment; see D3D12_SSR_WET_SURFACES_PLAN.md (repo root).
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12ResourceCreate.h"
#include "../Engine.h"
#include "../GothicAPI.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

/** (Re)creates the two persistent history textures. Called from the same resize path as the motion/TAA
    resources (CreateFrameResources); heap slots persist across resizes, only the resources and views are
    rebuilt. */
bool D3D12GraphicsEngine::CreateSsrHistoryResources( INT2 size ) {
    m_SsrHistoryValid = false;   // stale after any resize — last frame's buffer covered a different resolution
    if ( size.x < 4 || size.y < 4 ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Allocator ) return false;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    // Color: same format as the scene color, so the eventual march's hit sample needs no conversion.
    {
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = static_cast<UINT64>( size.x );
        dd.Height = static_cast<UINT>( size.y );
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = kSceneColorFormat;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if ( FAILED( D3D12ResourceCreate::CreateTexture( m_Allocator.Get(), heapDefault, dd, kSsrPrevReadState,
            nullptr, m_SsrPrevColorAlloc.ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_SsrPrevColor.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the SSR previous-frame color history (" << size.x << "x" << size.y << ").";
            return false;
        }
        m_SsrPrevColor->SetName( L"SsrPrevColor" );
    }

    // Depth: plain R32_FLOAT, not R32_TYPELESS+ALLOW_DEPTH_STENCIL — this is never bound as a real depth
    // target, only ever CopyResource's destination and an SRV source. Same reasoning as D3D12Water.cpp's
    // WaterDepthCopy: CopyResource only needs format-FAMILY compatibility with m_DepthBuffer's typeless
    // resource, not matching resource flags.
    {
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = static_cast<UINT64>( size.x );
        dd.Height = static_cast<UINT>( size.y );
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = DXGI_FORMAT_R32_FLOAT;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if ( FAILED( D3D12ResourceCreate::CreateTexture( m_Allocator.Get(), heapDefault, dd, kSsrPrevReadState,
            nullptr, m_SsrPrevDepthAlloc.ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_SsrPrevDepth.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the SSR previous-frame depth history (" << size.x << "x" << size.y << ").";
            return false;
        }
        m_SsrPrevDepth->SetName( L"SsrPrevDepth" );
    }

    auto ensureSlot = [&]( UINT& slot ) -> bool {
        if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
        return slot != UINT_MAX;
        };
    if ( !ensureSlot( m_SsrPrevColorSrvSlot ) || !ensureSlot( m_SsrPrevDepthSrvSlot ) ) return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = kSceneColorFormat;
    device->CreateShaderResourceView( m_SsrPrevColor.Get(), &srv, GetSrvCpuHandle( m_SsrPrevColorSrvSlot ) );
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateShaderResourceView( m_SsrPrevDepth.Get(), &srv, GetSrvCpuHandle( m_SsrPrevDepthSrvSlot ) );

    return true;
}

/** Snapshots the just-finished OPAQUE scene (color + depth) into the SSR history textures, for next frame's
    reflection march to read. Called once per frame, after the last opaque draw (including opaque decals) and
    BEFORE water/transparents — mirrors D3D12Water.cpp's own scene/depth copy timing, except this copy is kept
    across the frame boundary instead of being consumed the same frame. Deliberately excludes water and
    transparents: the SSR plan only ever needs to reflect opaque wet/glossy surfaces, and capturing after
    them would mean fighting water's own Z-prepass and alpha-blended overdraw for no benefit yet.

    Single buffer, not ping-ponged, deliberately: mirrors D3D12Taa.cpp's m_TaaPrevDepth reasoning exactly — a
    read of this frame's history (once a consumer exists) is always ordered, in the command stream, before
    this frame's own capture below, and the whole frame executes in submission order on one direct queue, so
    there is no cross-frame hazard from reusing a single resource. */
void D3D12GraphicsEngine::CaptureSsrOpaqueHistory() {
    if ( !m_FrameOpen || !m_CmdList || !m_SceneColor || !m_DepthBuffer ) return;
    if ( !m_SsrPrevColor || !m_SsrPrevDepth ) return;

    DX_ZONE( m_CmdList.Get(), "SSR history capture" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "SSR history capture" );

    m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );

    const D3D12_RESOURCE_STATES sceneFrom = m_SceneColorInPixelState
        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_CmdList->TransitionBarriers( {
        { m_SceneColor.Get(), sceneFrom, D3D12_RESOURCE_STATE_COPY_SOURCE },
        { m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE },
        { m_SsrPrevColor.Get(), kSsrPrevReadState, D3D12_RESOURCE_STATE_COPY_DEST },
        { m_SsrPrevDepth.Get(), kSsrPrevReadState, D3D12_RESOURCE_STATE_COPY_DEST },
        } );

    m_CmdList->CopyResource( m_SsrPrevColor.Get(), m_SceneColor.Get() );
    m_CmdList->CopyResource( m_SsrPrevDepth.Get(), m_DepthBuffer.Get() );

    m_CmdList->TransitionBarriers( {
        { m_SceneColor.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET },
        { m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE },
        { m_SsrPrevColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kSsrPrevReadState },
        { m_SsrPrevDepth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kSsrPrevReadState },
        } );
    m_SceneColorInPixelState = false;

    BindSceneColorTarget();
    m_SsrHistoryValid = true;
}
