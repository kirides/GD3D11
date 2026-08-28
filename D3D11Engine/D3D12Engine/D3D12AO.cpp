// D3D12GraphicsEngine — the AO entry point plus the simple screen-space AO ("SAO"). Intel XeGTAO, which is
// what AoMode == AO_ASSAO selects on this backend, lives in D3D12GTAO.cpp and shares this file's depth input,
// m_AOMask output and m_ActiveAOMaskSrvSlot publication.
// Simple SSAO ("SAO", plan item #4): the main compute pass reconstructs view-space normals from neighbouring
// depth samples (mirrors D3D11's CS_PFX_SAO.hlsl SAO_RECONSTRUCT_NORMALS fallback); a depth-aware separable
// blur denoises it (mirrors CS_PFX_SAO_Blur.hlsl). See Shaders/D3D12/SSAO.hlsl for the shader source.
//
// The AO source is THIS frame's depth buffer as the Forward+ DEPTH PREPASS left it — world mesh, instanced
// VOBs, skeletals + node attachments and (range-limited) vegetation. RenderSSAO runs immediately after the
// prepass and before any lit pass, so the mask it produces is already in this frame's screen space: the lit
// pixel shaders read it at their own pixel with no reprojection (see include/ScreenSpaceAO.hlsl).
//
// It used to run off a full-frame COPY of the PREVIOUS frame's COMPLETED depth, because grass was not in the
// prepass and a grass pixel would otherwise sample the AO of the terrain behind it. Folding vegetation into
// the prepass removed that reason, and with it the depth-sized allocation, the per-frame CopyResource, the
// one-frame lag and the per-pixel reprojection in five pixel shaders. The remaining late depth writers (water,
// decals, particles, the blended transparencies) are not AO occluders — they were only ever occluders in the
// snapshot scheme by accident of running a frame behind.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12ResourceCreate.h"
#include "../Engine.h"
#include "../GothicAPI.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

bool D3D12GraphicsEngine::CreateAOResources( INT2 size ) {
    m_AOResourcesReady = false;
    if ( size.x < 4 || size.y < 4 || !m_DepthBuffer ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;
    // The blur pairs below alias m_DepthSrvSlot's view, so the depth buffer's own SRV must already exist.
    if ( m_DepthSrvSlot == UINT_MAX ) return false;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    auto makeTex = [&]( ComPtr<ID3D12Resource>& out, ComPtr<D3D12MA::Allocation>& outAlloc, const wchar_t* name ) -> bool {
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = static_cast<UINT64>( size.x );
        dd.Height = static_cast<UINT>( size.y );
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        // R8_TYPELESS, not R8_UNORM: m_AOMask needs both an R8_UNORM view (what the lit pixel shaders and this
        // file's blur passes read/write) and an R8_UINT UAV (XeGTAO's denoise writes Intel's packed
        // `uint(value * 255 + 0.5)` term) over the same texels. Every view below states its format explicitly,
        // so the resource itself carries none. m_AOBlurTemp gets the same desc for free; it only ever uses the
        // R8_UNORM view.
        dd.Format = DXGI_FORMAT_R8_TYPELESS;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if ( FAILED( D3D12ResourceCreate::CreateTexture( m_Allocator.Get(), heapDefault, dd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, outAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create an SSAO texture (" << size.x << "x" << size.y << ").";
            return false;
        }
        out->SetName( name );
        return true;
        };

    if ( !makeTex( m_AOMask, m_AOMaskAlloc, L"AOMask" ) ) return false;
    if ( !makeTex( m_AOBlurTemp, m_AOBlurTempAlloc, L"AOBlurTemp" ) ) return false;

    auto ensureSlot = [&]( UINT& slot ) -> bool {
        if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
        return slot != UINT_MAX;
        };
    if ( !ensureSlot( m_AOMaskSrvSlot ) || !ensureSlot( m_AOMaskUavSlot ) || !ensureSlot( m_AOMaskUintUavSlot )
        || !ensureSlot( m_AOBlurTempUavSlot ) )
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

    // R8_UINT alias of the very same m_AOMask texels — XeGTAO's final denoise output (see the header comment on
    // m_AOMaskUintUavSlot). Writing uint(v*255+0.5) here and reading the R8_UNORM SRV above round-trips exactly.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uintUav = {};
    uintUav.Format = DXGI_FORMAT_R8_UINT;
    uintUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView( m_AOMask.Get(), nullptr, &uintUav, GetSrvCpuHandle( m_AOMaskUintUavSlot ) );

    // R32_FLOAT view of the typeless LIVE depth buffer — a private copy of what m_DepthSrvSlot holds, because
    // the blur passes bind (AO input, depth) as one 2-entry descriptor TABLE and those two must be heap-adjacent.
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = {};
    depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
    depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Texture2D.MipLevels = 1;

    device->CreateShaderResourceView( m_AOMask.Get(), &srv, GetSrvCpuHandle( m_AOBlurHPairSlot ) );
    device->CreateShaderResourceView( m_DepthBuffer.Get(), &depthSrv, GetSrvCpuHandle( m_AOBlurHPairSlot + 1 ) );
    device->CreateShaderResourceView( m_AOBlurTemp.Get(), &srv, GetSrvCpuHandle( m_AOBlurVPairSlot ) );
    device->CreateShaderResourceView( m_DepthBuffer.Get(), &depthSrv, GetSrvCpuHandle( m_AOBlurVPairSlot + 1 ) );

    m_AOMaskInPixelState = false;   // freshly (re)created above, born in UNORDERED_ACCESS
    m_AOResourcesReady = true;
    return true;
}

void D3D12GraphicsEngine::UploadAoScreenConstants() {
    // The screen-space AO block of the shared shadow CB: 1/screen-size, which the lit pixel shaders multiply
    // SV_Position by to get the mask UV. Written UNCONDITIONALLY every frame (AO on or off) — a frame that
    // skipped it would sample through whatever resolution happened to be current when it was last written.
    // The writers of m_ShadowCB must tile exactly: D3D12ShadowMap::Prepare owns [0, kWetnessCbOffset),
    // UploadWetnessConstants [kWetnessCbOffset, kAoReprojCbOffset), this the next 80 bytes (of which only the
    // first 8 are live) and UploadSkyIblConstants the rest. The CB is 512 bytes.
    static_assert( kWetnessCbOffset + sizeof( WetnessCBData ) == kAoReprojCbOffset,
        "the AO screen block must start right after the wetness block" );
    static_assert( sizeof( AoScreenCBData ) <= kAoReprojCbReservedBytes, "AO screen block overflows its slot" );
    if ( !m_ShadowCBMapped[m_FrameIndex] ) return;

    AoScreenCBData cb = {};
    cb.InvResX = m_Resolution.x > 0 ? 1.0f / static_cast<float>( m_Resolution.x ) : 0.0f;
    cb.InvResY = m_Resolution.y > 0 ? 1.0f / static_cast<float>( m_Resolution.y ) : 0.0f;
    memcpy( m_ShadowCBMapped[m_FrameIndex] + kAoReprojCbOffset, &cb, sizeof( cb ) );
}

void D3D12GraphicsEngine::RenderSSAO() {
    // THE AO entry point. Called from OnStartWorldRendering right after the depth prepass (and the light cull),
    // before the lit geometry passes bind m_ActiveAOMaskSrvSlot. Reads m_DepthBuffer, which at this point in
    // the frame holds exactly the prepass content — see the file header.
    //
    // Two implementations write the same m_AOMask and are selected by AoMode:
    //   AO_ASSAO -> Intel XeGTAO (D3D12GTAO.cpp) — D3D12's replacement for D3D11's ASSAO port.
    //   anything else non-NONE -> the simple SSAO below. HBAO+ and SAO have no D3D12 port of their own, so both
    //   land here; that is unchanged behaviour.
    //
    // The mask defaults to the 1x1 white texture (no occlusion) so every consumer can read it unconditionally,
    // including on frames where AO is off or bailed out below.
    m_ActiveAOMaskSrvSlot = m_WhiteTexture ? m_WhiteTexture->GetSrvSlot() : UINT_MAX;
    UploadAoScreenConstants();

    if ( Engine::GAPI->GetRendererState().RendererSettings.AoMode == AOMode::AO_NONE ) return;

    if ( IsGtaoEnabled() ) {
        RenderGTAO();
        return;
    }
    RenderSimpleSSAO();
}

void D3D12GraphicsEngine::BeginAoDepthRead() {
    // The prepass left m_DepthBuffer in DEPTH_WRITE; the AO compute passes read it as an SRV. Same round-trip
    // BuildHiZ does a few calls earlier — the DSV stays bound but nothing draws while it is in a read state.
    m_CmdList->TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
}

void D3D12GraphicsEngine::EndAoDepthRead() {
    // Straight back to DEPTH_WRITE: the lit geometry passes right after this re-test (and re-write) depth, and
    // every later transition of this resource asserts DEPTH_WRITE as its "before" state.
    m_CmdList->TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE );
}

void D3D12GraphicsEngine::RenderSimpleSSAO() {
    // The depth-reconstructed-normals Alchemy-AO estimate plus a separable depth-aware blur
    // (Shaders/D3D12/SSAO.hlsl). Callers go through RenderSSAO, which owns the mode selection and has already
    // published the white-mask default.
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( !m_FrameOpen || !m_AOResourcesReady || !m_DepthBuffer || m_DepthSrvSlot == UINT_MAX
        || !m_Pipelines.AO.MainPSO || !m_Pipelines.AO.MainRootSig
        || !m_Pipelines.AO.BlurPSO || !m_Pipelines.AO.BlurRootSig )
        return;

    DX_ZONE( m_CmdList.Get(), "SSAO" );

    const auto& sao = settings.SaoSettings;
    // This frame's projection — the same one the prepass depth below was rasterized with. Its _13/_23 carry the
    // TAA jitter, which none of the terms used here read.
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    // m_AoResourceSize, NOT m_Resolution: m_AOMask/m_AOBlurTemp are built at GetAoTargetResolution(), which is
    // m_Resolution halved when RendererSettings.AoResolution == Half (see D3D12GraphicsEngine.cpp). The DEPTH
    // input stays full-res regardless — the shader samples it via normalized UV, so reading it at this coarser
    // stride is exactly the intended "AO at half res" decimation, not a mismatch.
    const UINT gx = ( static_cast<UINT>( m_AoResourceSize.x ) + 7 ) / 8;
    const UINT gy = ( static_cast<UINT>( m_AoResourceSize.y ) + 7 ) / 8;

    BeginAoDepthRead();

    // The AO mask rests in UNORDERED_ACCESS (its creation state) except right after a prior successful run,
    // which leaves it PIXEL_SHADER_RESOURCE for the lit passes.
    if ( m_AOMaskInPixelState ) {
        m_CmdList->TransitionBarrier( m_AOMask.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
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
        1.0f / m_AoResourceSize.x, 1.0f / m_AoResourceSize.y, 0.0f, 0.0f
    };

    m_CmdList->SetPipelineState( m_Pipelines.AO.MainPSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.AO.MainRootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 12, &cb, 0 );
    m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_DepthSrvSlot ) );
    m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_AOMaskUavSlot ) );
    m_CmdList->Dispatch( gx, gy, 1 );

    // AO estimate: UAV write -> SRV read for the blur passes (real state transition, not just a UAV barrier —
    // same reasoning as the bloom pyramid's UAV<->SRV round-trips). Both blur-pair slot 0 entries alias
    // m_AOMask/m_AOBlurTemp respectively, so transitioning the resource covers whichever pair reads it.
    {
        m_CmdList->TransitionBarrier( m_AOMask.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
    }

    struct BlurCB {
        float InvResX, InvResY, DirX, DirY, ProjZX, ProjZY, Sharpness, _pad;
    };

    m_CmdList->SetPipelineState( m_Pipelines.AO.BlurPSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.AO.BlurRootSig.Get() );

    // --- Blur pass 1 (horizontal): m_AOMask -> m_AOBlurTemp ---
    {
        BlurCB blurCb = { 1.0f / m_AoResourceSize.x, 1.0f / m_AoResourceSize.y, 1.0f, 0.0f, projM._33, projM._43, sao.BlurSharpness, 0.0f };
        m_CmdList->SetComputeRoot32BitConstants( 0, 8, &blurCb, 0 );
        m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_AOBlurHPairSlot ) );   // t0=AOMask, t1=depth
        m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_AOBlurTempUavSlot ) );
        m_CmdList->Dispatch( gx, gy, 1 );
    }
    {
        m_CmdList->TransitionBarriers( {
        	{ m_AOBlurTemp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        	{ m_AOMask.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
        } );
    }

    // --- Blur pass 2 (vertical): m_AOBlurTemp -> m_AOMask (final) ---
    {
        BlurCB blurCb = { 1.0f / m_AoResourceSize.x, 1.0f / m_AoResourceSize.y, 0.0f, 1.0f, projM._33, projM._43, sao.BlurSharpness, 0.0f };
        m_CmdList->SetComputeRoot32BitConstants( 0, 8, &blurCb, 0 );
        m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_AOBlurVPairSlot ) );   // t0=AOBlurTemp, t1=depth
        m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_AOMaskUavSlot ) );
        m_CmdList->Dispatch( gx, gy, 1 );
    }

    // Leave m_AOMask readable for the lit geometry passes' bindless fetch, and restore m_AOBlurTemp to its rest
    // state (UNORDERED_ACCESS) — it was left in NON_PIXEL_SHADER_RESOURCE by the blur-pass-1 barrier above
    // (scratch buffer, no cross-frame reader) and, without this, the NEXT frame's blur-pass-1 barrier would
    // assert an UNORDERED_ACCESS "before" state that doesn't match reality (GPU validation:
    // "RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH" on AOBlurTemp).
    {
        m_CmdList->TransitionBarriers( {
        	{ m_AOMask.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
        	{ m_AOBlurTemp.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
        } );
    }

    EndAoDepthRead();

    m_AOMaskInPixelState = true;
    m_ActiveAOMaskSrvSlot = m_AOMaskSrvSlot;
}
