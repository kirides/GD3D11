// D3D12GraphicsEngine — simple screen-space AO (plan item #4, "SAO"). Forward+ has no GBuffer normals before
// lighting, so the main compute pass reconstructs view-space normals from neighbouring depth samples (mirrors
// D3D11's CS_PFX_SAO.hlsl SAO_RECONSTRUCT_NORMALS fallback); a depth-aware separable blur denoises it
// (mirrors CS_PFX_SAO_Blur.hlsl). See Shaders/D3D12/SSAO.hlsl for the shader source.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

bool D3D12GraphicsEngine::CreateAOResources( INT2 size ) {
    m_AOResourcesReady = false;
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

    auto ensureSlot = [&]( UINT& slot ) -> bool {
        if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
        return slot != UINT_MAX;
        };
    if ( !ensureSlot( m_AOMaskSrvSlot ) || !ensureSlot( m_AOMaskUavSlot ) || !ensureSlot( m_AOBlurTempUavSlot ) )
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

    // Depth SRV (R32_FLOAT view of the typeless depth resource — matches CreateDepthBuffer's m_DepthSrvSlot view).
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

void D3D12GraphicsEngine::RenderSSAO() {
    // Called from OnStartWorldRendering right after DispatchLightCulling (prepass depth is complete), before
    // the lit geometry passes bind m_ActiveAOMaskSrvSlot. Simple-SSAO is the D3D12 substitute for every AO
    // mode (D3D11's HBAO+/ASSAO have no D3D12 port yet) — any AoMode != AO_NONE runs it.
    m_ActiveAOMaskSrvSlot = m_WhiteTexture ? m_WhiteTexture->GetSrvSlot() : UINT_MAX;   // default: no occlusion

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( settings.AoMode == AOMode::AO_NONE ) return;
    if ( !m_FrameOpen || !m_AOResourcesReady || !m_DepthBuffer
        || !m_Pipelines.AO.MainPSO || !m_Pipelines.AO.MainRootSig
        || !m_Pipelines.AO.BlurPSO || !m_Pipelines.AO.BlurRootSig )
        return;

    DX_ZONE( m_CmdList, "SSAO" );

    const auto& sao = settings.SaoSettings;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    const UINT gx = ( static_cast<UINT>( m_Resolution.x ) + 7 ) / 8;
    const UINT gy = ( static_cast<UINT>( m_Resolution.y ) + 7 ) / 8;

    // Depth prepass left the buffer in DEPTH_WRITE (DispatchLightCulling already handed it back after its own
    // compute read) — flip it readable for these three compute dispatches, then hand it back for the lit passes.
    {
        auto b = TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        m_CmdList->ResourceBarrier( 1, &b );
    }

    // m_AOMask rests in UNORDERED_ACCESS (its creation state) except right after a prior successful run, which
    // leaves it PIXEL_SHADER_RESOURCE for the lit passes — flip it back before the main pass writes it.
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
    m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_DepthSrvSlot ) );
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
        m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_AOBlurHPairSlot ) );   // t0=AOMask, t1=depth
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
        m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_AOBlurVPairSlot ) );   // t0=AOBlurTemp, t1=depth
        m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_AOMaskUavSlot ) );
        m_CmdList->Dispatch( gx, gy, 1 );
    }

    // Leave m_AOMask readable for the lit geometry passes' bindless fetch; depth back to DEPTH_WRITE; and
    // restore m_AOBlurTemp to its rest state (UNORDERED_ACCESS) — it was left in NON_PIXEL_SHADER_RESOURCE
    // by the blur-pass-1 barrier above (scratch buffer, no cross-frame reader) and, without this, the NEXT
    // frame's blur-pass-1 barrier would assert an UNORDERED_ACCESS "before" state that doesn't match reality
    // (GPU validation: "RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH" on AOBlurTemp).
    {
        D3D12_RESOURCE_BARRIER b[3] = {
            TransitionBarrier( m_AOMask.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ),
            TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE ),
            TransitionBarrier( m_AOBlurTemp.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
        };
        m_CmdList->ResourceBarrier( 3, b );
    }

    m_AOMaskInPixelState = true;
    m_ActiveAOMaskSrvSlot = m_AOMaskSrvSlot;
}
