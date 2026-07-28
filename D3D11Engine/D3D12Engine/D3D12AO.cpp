// D3D12GraphicsEngine — simple screen-space AO (plan item #4, "SAO"). Forward+ has no GBuffer normals before
// lighting, so the main compute pass reconstructs view-space normals from neighbouring depth samples (mirrors
// D3D11's CS_PFX_SAO.hlsl SAO_RECONSTRUCT_NORMALS fallback); a depth-aware separable blur denoises it
// (mirrors CS_PFX_SAO_Blur.hlsl). See Shaders/D3D12/SSAO.hlsl for the shader source.
//
// The AO source is the PREVIOUS frame's COMPLETE depth buffer (m_PrevDepth, snapshotted by CopyDepthForAO at
// the end of world rendering), not this frame's depth prepass. The prepass only lays down world + instanced
// VOBs + skeletal, so grass/foliage and the other late depth writers were both invisible to the AO *and*
// wrongly lit by the mask of whatever sat behind them. A full-frame snapshot costs one depth-sized copy and
// buys AO that includes every depth writer, at the price of being one frame stale — which the lit pixel
// shaders undo by reprojecting per-pixel through m_PrevDepthViewProj (see include/ScreenSpaceAO.hlsl).
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

bool D3D12GraphicsEngine::CreateAOResources( INT2 size ) {
    m_AOResourcesReady = false;
    m_PrevDepthValid = false;   // the snapshot below is freshly (re)allocated garbage until CopyDepthForAO runs
    if ( size.x < 4 || size.y < 4 || !m_DepthBuffer ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    auto makeTex = [&]( ComPtr<ID3D12Resource>& out, ComPtr<D3D12MA::Allocation>& outAlloc, const wchar_t* name ) -> bool {
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = static_cast<UINT64>( size.x );
        dd.Height = static_cast<UINT>( size.y );
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = DXGI_FORMAT_R8_UNORM;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, outAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create an SSAO texture (" << size.x << "x" << size.y << ").";
            return false;
        }
        out->SetName( name );
        return true;
        };

    if ( !makeTex( m_AOMask, m_AOMaskAlloc, L"AOMask" ) ) return false;
    if ( !makeTex( m_AOBlurTemp, m_AOBlurTempAlloc, L"AOBlurTemp" ) ) return false;

    // Previous-frame depth snapshot. Deliberately created with the SAME resource desc as m_DepthBuffer
    // (R32_TYPELESS + ALLOW_DEPTH_STENCIL, matching CreateDepthBuffer) so CopyResource is unambiguously legal:
    // it requires identical dimensions/format, and matching the flags too costs nothing here.
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
        clear.DepthStencil.Depth = 0.0f;   // reversed-Z, same optimized clear as the real depth buffer
        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, kPrevDepthReadState, &clear,
            m_PrevDepthAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_PrevDepth.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the previous-frame depth copy (" << size.x << "x" << size.y << ").";
            return false;
        }
        m_PrevDepth->SetName( L"PrevFrameDepth(AO)" );
    }

    auto ensureSlot = [&]( UINT& slot ) -> bool {
        if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
        return slot != UINT_MAX;
        };
    if ( !ensureSlot( m_AOMaskSrvSlot ) || !ensureSlot( m_AOMaskUavSlot ) || !ensureSlot( m_AOBlurTempUavSlot )
        || !ensureSlot( m_PrevDepthSrvSlot ) )
        return false;
    // 2 contiguous slots per blur direction (see the header comment on m_AOBlurHPairSlot/m_AOBlurVPairSlot).
    if ( m_AOBlurHPairSlot == UINT_MAX ) {
        const UINT base = AllocateSrvSlot(); const UINT next = AllocateSrvSlot();
        if ( base == UINT_MAX || next == UINT_MAX || next != base + 1 ) return false;
        m_AOBlurHPairSlot = base;
    }
    if ( m_AOBlurVPairSlot == UINT_MAX ) {
        const UINT base = AllocateSrvSlot(); const UINT next = AllocateSrvSlot();
        if ( base == UINT_MAX || next == UINT_MAX || next != base + 1 ) return false;
        m_AOBlurVPairSlot = base;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    device->CreateShaderResourceView( m_AOMask.Get(), &srv, GetSrvCpuHandle( m_AOMaskSrvSlot ) );
    device->CreateUnorderedAccessView( m_AOMask.Get(), nullptr, &uav, GetSrvCpuHandle( m_AOMaskUavSlot ) );
    device->CreateUnorderedAccessView( m_AOBlurTemp.Get(), nullptr, &uav, GetSrvCpuHandle( m_AOBlurTempUavSlot ) );

    // R32_FLOAT view of the typeless depth SNAPSHOT (same view shape CreateDepthBuffer builds for the live
    // buffer). Read by the AO main+blur compute passes AND, bindlessly, by the lit pixel shaders' reprojection
    // disocclusion test — hence it needs a stable heap slot of its own.
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = {};
    depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
    depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView( m_PrevDepth.Get(), &depthSrv, GetSrvCpuHandle( m_PrevDepthSrvSlot ) );

    device->CreateShaderResourceView( m_AOMask.Get(), &srv, GetSrvCpuHandle( m_AOBlurHPairSlot ) );
    device->CreateShaderResourceView( m_PrevDepth.Get(), &depthSrv, GetSrvCpuHandle( m_AOBlurHPairSlot + 1 ) );
    device->CreateShaderResourceView( m_AOBlurTemp.Get(), &srv, GetSrvCpuHandle( m_AOBlurVPairSlot ) );
    device->CreateShaderResourceView( m_PrevDepth.Get(), &depthSrv, GetSrvCpuHandle( m_AOBlurVPairSlot + 1 ) );

    m_AOMaskInPixelState = false;   // freshly (re)created above, born in UNORDERED_ACCESS
    m_AOResourcesReady = true;
    return true;
}

void D3D12GraphicsEngine::CopyDepthForAO() {
    // End of world rendering: snapshot the COMPLETE depth buffer (world, VOBs, skeletal, grass, decals, water —
    // everything that wrote depth this frame) for the NEXT frame's AO pass, together with the camera that
    // produced it. Must run after the last depth-writing pass and before RenderFogAndGodRays, which is the
    // first thing to move the depth buffer out of DEPTH_WRITE.
    if ( !m_FrameOpen || !m_DepthBuffer || !m_PrevDepth ) return;

    // AO off -> don't pay for a full-res depth copy every frame. Invalidate too, so re-enabling AO can't
    // reproject through whatever camera happened to be current when it was switched off.
    if ( Engine::GAPI->GetRendererState().RendererSettings.AoMode == AOMode::AO_NONE ) {
        m_PrevDepthValid = false;
        return;
    }

    DX_ZONE( m_CmdList, "Copy depth for AO" );

    D3D12_RESOURCE_BARRIER pre[2] = {
        TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE ),
        TransitionBarrier( m_PrevDepth.Get(), kPrevDepthReadState, D3D12_RESOURCE_STATE_COPY_DEST ),
    };
    m_CmdList->ResourceBarrier( 2, pre );

    m_CmdList->CopyResource( m_PrevDepth.Get(), m_DepthBuffer.Get() );

    D3D12_RESOURCE_BARRIER post[2] = {
        TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE ),
        TransitionBarrier( m_PrevDepth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kPrevDepthReadState ),
    };
    m_CmdList->ResourceBarrier( 2, post );

    // Capture the camera that rendered these texels. Rebuilt exactly like DrawWorldMesh does (proj * view,
    // identity world) — every geometry pass in this frame recomputed it identically, so this is the same
    // matrix the depth above was rasterized with.
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMStoreFloat4x4( &m_PrevDepthViewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );
    m_PrevDepthProj = projM;
    m_PrevDepthValid = true;
}

void D3D12GraphicsEngine::UploadAoReprojConstants() {
    // Publishes the reprojection block the lit World/Vob/Skeletal pixel shaders read out of the shadow CB they
    // already bind. Written UNCONDITIONALLY every frame (including the "no valid snapshot" state) — a frame
    // that skipped the write would reproject this frame's geometry through a stale camera.
    // The writers of m_ShadowCB must tile exactly: D3D12ShadowMap::Prepare owns [0, kWetnessCbOffset),
    // UploadWetnessConstants [kWetnessCbOffset, kAoReprojCbOffset), and this the rest (the CB is 512 bytes).
    static_assert( kWetnessCbOffset + sizeof( WetnessCBData ) == kAoReprojCbOffset,
        "AO-reprojection CB block must start right after the wetness block" );
    static_assert( kAoReprojCbOffset + sizeof( AoReprojCBData ) <= 512, "shadow CB overflow" );
    if ( !m_ShadowCBMapped[m_FrameIndex] ) return;

    AoReprojCBData cb = {};
    const bool valid = m_PrevDepthValid && m_PrevDepth && m_PrevDepthSrvSlot != UINT_MAX;
    cb.PrevViewProj = m_PrevDepthViewProj;
    // Reversed-Z linearization terms of the snapshot's own projection (viewZ = ProjZY / (depth - ProjZX)),
    // used by the disocclusion test to compare "where this pixel would have been" against what was stored.
    cb.PrevProjZX = m_PrevDepthProj._33;
    cb.PrevProjZY = m_PrevDepthProj._43;
    // 0xFFFFFFFF is never dereferenced by the shader (ReprojValid gates it first), but keep it out of the
    // valid-descriptor range regardless so a future reorder can't turn it into a wild bindless fetch.
    cb.PrevDepthIndex = valid ? m_PrevDepthSrvSlot : 0xFFFFFFFFu;
    cb.ReprojValid = valid ? 1.0f : 0.0f;

    memcpy( m_ShadowCBMapped[m_FrameIndex] + kAoReprojCbOffset, &cb, sizeof( cb ) );
}

void D3D12GraphicsEngine::RenderSSAO() {
    // Called from OnStartWorldRendering right after DispatchLightCulling, before the lit geometry passes bind
    // m_ActiveAOMaskSrvSlot. Reads m_PrevDepth (last frame's complete depth — see the file header), so unlike
    // every other consumer of depth in this frame it needs NO barriers on the live m_DepthBuffer and does not
    // serialise against the prepass. Simple-SSAO is the D3D12 substitute for every AO mode (D3D11's HBAO+/ASSAO
    // have no D3D12 port yet) — any AoMode != AO_NONE runs it.
    m_ActiveAOMaskSrvSlot = m_WhiteTexture ? m_WhiteTexture->GetSrvSlot() : UINT_MAX;   // default: no occlusion
    UploadAoReprojConstants();

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( settings.AoMode == AOMode::AO_NONE ) return;
    if ( !m_FrameOpen || !m_AOResourcesReady || !m_PrevDepth || !m_PrevDepthValid
        || !m_Pipelines.AO.MainPSO || !m_Pipelines.AO.MainRootSig
        || !m_Pipelines.AO.BlurPSO || !m_Pipelines.AO.BlurRootSig )
        return;

    DX_ZONE( m_CmdList, "SSAO" );

    const auto& sao = settings.SaoSettings;
    // The snapshot's OWN projection, not this frame's — the depth being unprojected here was rasterized with it.
    const XMFLOAT4X4& projM = m_PrevDepthProj;
    const UINT gx = ( static_cast<UINT>( m_Resolution.x ) + 7 ) / 8;
    const UINT gy = ( static_cast<UINT>( m_Resolution.y ) + 7 ) / 8;

    // m_PrevDepth already rests in a shader-read state, so the only resource that needs flipping here is the
    // AO mask itself: it rests in UNORDERED_ACCESS (its creation state) except right after a prior successful
    // run, which leaves it PIXEL_SHADER_RESOURCE for the lit passes.
    if ( m_AOMaskInPixelState ) {
        auto b = TransitionBarrier( m_AOMask.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        m_CmdList->ResourceBarrier( 1, &b );
        m_AOMaskInPixelState = false;
    }

    // --- Main estimate: depth -> m_AOMask (raw, unblurred) ---
    struct SSAOCB {
        float ProjScaleX, ProjScaleY, ProjZX, ProjZY;
        float Radius, Bias, Intensity; int NumSamples;
        float InvResX, InvResY, _padX, _padY;
    } cb = {
        projM._11, projM._22, projM._33, projM._43,
        sao.Radius, sao.Bias, sao.Intensity, sao.NumSamples,
        1.0f / m_Resolution.x, 1.0f / m_Resolution.y, 0.0f, 0.0f
    };

    m_CmdList->SetPipelineState( m_Pipelines.AO.MainPSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.AO.MainRootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 12, &cb, 0 );
    m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_PrevDepthSrvSlot ) );
    m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_AOMaskUavSlot ) );
    m_CmdList->Dispatch( gx, gy, 1 );

    // AO estimate: UAV write -> SRV read for the blur passes (real state transition, not just a UAV barrier —
    // same reasoning as the bloom pyramid's UAV<->SRV round-trips). Both blur-pair slot 0 entries alias
    // m_AOMask/m_AOBlurTemp respectively, so transitioning the resource covers whichever pair reads it.
    {
        auto b = TransitionBarrier( m_AOMask.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        m_CmdList->ResourceBarrier( 1, &b );
    }

    struct BlurCB {
        float InvResX, InvResY, DirX, DirY, ProjZX, ProjZY, Sharpness, _pad;
    };

    m_CmdList->SetPipelineState( m_Pipelines.AO.BlurPSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.AO.BlurRootSig.Get() );

    // --- Blur pass 1 (horizontal): m_AOMask -> m_AOBlurTemp ---
    {
        BlurCB blurCb = { 1.0f / m_Resolution.x, 1.0f / m_Resolution.y, 1.0f, 0.0f, projM._33, projM._43, sao.BlurSharpness, 0.0f };
        m_CmdList->SetComputeRoot32BitConstants( 0, 8, &blurCb, 0 );
        m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_AOBlurHPairSlot ) );   // t0=AOMask, t1=prev depth
        m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_AOBlurTempUavSlot ) );
        m_CmdList->Dispatch( gx, gy, 1 );
    }
    {
        D3D12_RESOURCE_BARRIER b[2] = {
            TransitionBarrier( m_AOBlurTemp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ),
            TransitionBarrier( m_AOMask.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
        };
        m_CmdList->ResourceBarrier( 2, b );
    }

    // --- Blur pass 2 (vertical): m_AOBlurTemp -> m_AOMask (final) ---
    {
        BlurCB blurCb = { 1.0f / m_Resolution.x, 1.0f / m_Resolution.y, 0.0f, 1.0f, projM._33, projM._43, sao.BlurSharpness, 0.0f };
        m_CmdList->SetComputeRoot32BitConstants( 0, 8, &blurCb, 0 );
        m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_AOBlurVPairSlot ) );   // t0=AOBlurTemp, t1=prev depth
        m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_AOMaskUavSlot ) );
        m_CmdList->Dispatch( gx, gy, 1 );
    }

    // Leave m_AOMask readable for the lit geometry passes' bindless fetch, and restore m_AOBlurTemp to its rest
    // state (UNORDERED_ACCESS) — it was left in NON_PIXEL_SHADER_RESOURCE by the blur-pass-1 barrier above
    // (scratch buffer, no cross-frame reader) and, without this, the NEXT frame's blur-pass-1 barrier would
    // assert an UNORDERED_ACCESS "before" state that doesn't match reality (GPU validation:
    // "RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH" on AOBlurTemp).
    {
        D3D12_RESOURCE_BARRIER b[2] = {
            TransitionBarrier( m_AOMask.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ),
            TransitionBarrier( m_AOBlurTemp.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
        };
        m_CmdList->ResourceBarrier( 2, b );
    }

    m_AOMaskInPixelState = true;
    m_ActiveAOMaskSrvSlot = m_AOMaskSrvSlot;
}
