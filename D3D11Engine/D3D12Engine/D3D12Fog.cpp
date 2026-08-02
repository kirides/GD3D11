// D3D12GraphicsEngine — height fog + god rays (parity plan item #5). The D3D11 spec is its PostFX
// composition pass: D3D11PfxRenderer::RenderPostFXComposition (constant setup + the fullscreen blit) and
// D3D11PFX_GodRays::RenderToTextureCS (the two quarter-res compute passes that build the ray texture),
// wired together in D3D11GraphicsEngine::OnStartWorldRendering under `compositionActive`.
//
// Structural difference (documented at length in Shaders/D3D12/HeightFog.hlsl): D3D11 copies the whole
// backbuffer to a temp texture so the composition PS can read it and `lerp` in the shader. Both terms are
// pure blend ops, so this port emits premultiplied output and blends it directly onto m_SceneColor — no
// full-resolution HDR copy, no extra pool texture.
//
// Everything here is skipped for indoor worlds and whenever the corresponding renderer setting is off,
// exactly like D3D11 (`DrawFog && isOutdoor`, `EnableGodRays && isOutdoor`).
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../GSky.h"
#include "../ConstantBufferStructs.h"
#include "../Toolbox.h"
#include "../zCBspTree.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    // Per-draw root constants of the composition pass (b2 in HeightFog.hlsl).
    struct FogCompositeConsts {
        UINT DepthIndex;
        UINT GodRaysIndex;
        UINT Flags;
        UINT Pad;
    };
    constexpr UINT kFogFlagHeightFog = 1u;
    constexpr UINT kFogFlagGodRays = 2u;

    // b0 root constants of GodRays.hlsl's CSMask (padded out to the shared 12-DWORD root signature).
    struct GodRayMaskConsts {
        UINT SceneColorIndex;
        UINT DepthIndex;
        UINT OutputIndex;
        UINT Pad;
    };

    // b0 root constants of GodRays.hlsl's CSZoom — 12 DWORDs, the full width of the shared root signature.
    struct GodRayZoomConsts {
        float Decay;
        float Weight;
        float Center[2];
        float Density;
        float ColorMod[3];
        UINT  MaskIndex;
        UINT  OutputIndex;
        UINT  Pad[2];
    };
    static_assert( sizeof( GodRayZoomConsts ) == 12 * sizeof( UINT ), "CSZoom root constants must be 12 DWORDs" );
}


bool D3D12GraphicsEngine::CreateFogConstantBuffers() {
    // One persistently-mapped UPLOAD buffer per frame-in-flight, 512 B: [0,256) the height-fog CB (b0),
    // [256,512) the atmosphere CB (b1). Both root CBV addresses must be 256-byte aligned, hence the split
    // rather than one packed struct. Same pattern as the shared shadow CB (CreateShadowConstantBuffer).
    D3D12MA::ALLOCATION_DESC uploadAlloc = {};
    uploadAlloc.HeapType = DefaultUploadHeapType;

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = 512;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    static_assert( sizeof( HeightfogConstantBuffer ) <= kFogAtmosphereCbOffset,
        "HeightfogConstantBuffer must fit in the first 256-byte block of the fog CB" );
    static_assert( sizeof( AtmosphereConstantBuffer ) <= 512 - kFogAtmosphereCbOffset,
        "AtmosphereConstantBuffer must fit in the second 256-byte block of the fog CB" );

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_Allocator->CreateResource( &uploadAlloc, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_FogCBAlloc[i].ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_FogCB[i].ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the height-fog constant buffer.";
            return false;
        }
        m_FogCB[i]->SetName( L"HeightFogCB" );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( m_FogCB[i]->Map( 0, &noRead, &mapped ) ) ) return false;
        m_FogCBMapped[i] = static_cast<uint8_t*>( mapped );
        m_FogCBGpu[i] = m_FogCB[i]->GetGPUVirtualAddress();
    }
    return true;
}


bool D3D12GraphicsEngine::CreateFogResources( INT2 size ) {
    // Quarter-resolution god-ray chain (mask -> radial blur), the D3D12 equivalent of D3D11's
    // GetTempBufferDS4() pool textures. HDR format so bright sky pixels keep their intensity through the
    // 64-tap blur, exactly like D3D11 (which uses GetBackBufferFormat(), i.e. its HDR intermediate).
    m_FogResourcesReady = false;
    m_GodRaySize = { 0, 0 };
    if ( size.x < 8 || size.y < 8 ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    const INT2 ds4 = { std::max( 1, size.x / 4 ), std::max( 1, size.y / 4 ) };

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    auto makeTex = [&]( ComPtr<ID3D12Resource>& out, ComPtr<D3D12MA::Allocation>& outAlloc, const wchar_t* name ) -> bool {
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = static_cast<UINT64>( ds4.x );
        dd.Height = static_cast<UINT>( ds4.y );
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = kSceneColorFormat;
        dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, outAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create a god-ray texture (" << ds4.x << "x" << ds4.y << ").";
            return false;
        }
        out->SetName( name );
        return true;
        };

    if ( !makeTex( m_GodRayMask, m_GodRayMaskAlloc, L"GodRayMask" ) ) return false;
    if ( !makeTex( m_GodRayZoom, m_GodRayZoomAlloc, L"GodRayZoom" ) ) return false;

    auto ensureSlot = [&]( UINT& slot ) -> bool {
        if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
        return slot != UINT_MAX;
        };
    if ( !ensureSlot( m_GodRayMaskSrvSlot ) || !ensureSlot( m_GodRayMaskUavSlot )
        || !ensureSlot( m_GodRayZoomSrvSlot ) || !ensureSlot( m_GodRayZoomUavSlot ) )
        return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = kSceneColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = kSceneColorFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    device->CreateShaderResourceView( m_GodRayMask.Get(), &srv, GetSrvCpuHandle( m_GodRayMaskSrvSlot ) );
    device->CreateUnorderedAccessView( m_GodRayMask.Get(), nullptr, &uav, GetSrvCpuHandle( m_GodRayMaskUavSlot ) );
    device->CreateShaderResourceView( m_GodRayZoom.Get(), &srv, GetSrvCpuHandle( m_GodRayZoomSrvSlot ) );
    device->CreateUnorderedAccessView( m_GodRayZoom.Get(), nullptr, &uav, GetSrvCpuHandle( m_GodRayZoomUavSlot ) );

    m_GodRaySize = ds4;
    m_FogResourcesReady = true;
    return true;
}


bool D3D12GraphicsEngine::EvaluateHeightFogActive() const {
    // Mirrors D3D11's `compositionHeightFog = DrawFog && isOutdoor`, plus a D3D12-side resource check so a
    // failed CreateFog()/CreateFogConstantBuffers() can't switch the shaders' linear fog off with nothing
    // to replace it. Called once per frame at the top of OnStartWorldRendering — before the geometry passes,
    // which consult m_HeightFogActive to suppress their own cheap distance fog.
    if ( !Engine::GAPI->GetRendererState().RendererSettings.DrawFog ) return false;
    if ( !m_Pipelines.Fog.CompositePSO || !m_Pipelines.Fog.CompositeRootSig ) return false;
    if ( !m_FogCBMapped[m_FrameIndex] || !m_SceneColor || !m_DepthBuffer || m_DepthSrvSlot == UINT_MAX ) return false;

    auto* worldInfo = Engine::GAPI->GetLoadedWorldInfo();
    if ( !worldInfo || !worldInfo->BspTree ) return false;
    return worldInfo->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;
}


void D3D12GraphicsEngine::RenderFogAndGodRays() {
    // Called from OnStartWorldRendering after ALL scene content (opaque, water, decals, particles, rain,
    // ghosts) and before RenderBloom — the same slot D3D11's composition pass occupies (after "Draw ghosts"
    // / "Draw ParticleFX #2", before the post-upscale bloom+tonemap block).
    // Both halves need the depth buffer (sky detection for the rays, position reconstruction for the fog) and
    // both go through the composition draw, so its PSO/root sig/CB are hard requirements for the whole pass.
    if ( !m_FrameOpen || !m_CmdList || !m_SceneColor || !m_DepthBuffer || m_DepthSrvSlot == UINT_MAX ) return;
    if ( !m_Pipelines.Fog.CompositePSO || !m_Pipelines.Fog.CompositeRootSig || !m_FogCBMapped[m_FrameIndex] ) return;

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const bool heightFog = m_HeightFogActive;   // evaluated once at the top of the frame

    // God rays: `EnableGodRays && isOutdoor` (D3D11), plus this backend's resource/PSO guards. The outdoor
    // test is already folded into EvaluateHeightFogActive; redo the BSP check here so god rays can still run
    // in the (settings-wise possible) DrawFog=off case.
    bool godRays = settings.EnableGodRays && m_FogResourcesReady
        && m_Pipelines.Fog.MaskPSO && m_Pipelines.Fog.ZoomPSO && m_Pipelines.Fog.GodRayRootSig
        && m_SceneColorSrvSlot != UINT_MAX;
    if ( godRays ) {
        auto* worldInfo = Engine::GAPI->GetLoadedWorldInfo();
        godRays = worldInfo && worldInfo->BspTree && worldInfo->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;
    }

    GodRayZoomConsts zoomConsts = {};
    if ( godRays ) {
        // --- Sun screen position + weight falloff: a verbatim port of D3D11PFX_GodRays::RenderToTextureCS ---
        GSky* sky = Engine::GAPI->GetSky();
        if ( !sky || sky->GetAtmoshpereSettings().LightDirection.y <= 0 ) {
            godRays = false;   // no god rays at night
        } else {
            XMVECTOR xmSunPosition = XMLoadFloat3( &sky->GetAtmosphereCB().AC_LightPos );
            const float outerRadius = sky->GetAtmosphereCB().AC_OuterRadius;
            xmSunPosition *= outerRadius;
            xmSunPosition += Engine::GAPI->GetCameraPositionXM();

            XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
            XMMATRIX proj = XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() );
            XMMATRIX viewProj = XMMatrixTranspose( XMMatrixMultiply( proj, view ) );
            view = XMMatrixTranspose( view );

            XMFLOAT3 sunViewPosition; XMStoreFloat3( &sunViewPosition, XMVector3TransformCoord( xmSunPosition, view ) );
            XMFLOAT3 sunPosition; XMStoreFloat3( &sunPosition, XMVector3TransformCoord( xmSunPosition, viewProj ) );

            if ( sunViewPosition.z < 0.0f ) {
                godRays = false;   // sun behind the camera
            } else {
                zoomConsts.Decay = settings.GodRayDecay;
                zoomConsts.Weight = settings.GodRayWeight;
                zoomConsts.Density = settings.GodRayDensity;
                zoomConsts.Center[0] = sunPosition.x / 2.0f + 0.5f;
                zoomConsts.Center[1] = sunPosition.y / -2.0f + 0.5f;
                zoomConsts.ColorMod[0] = settings.GodRayColorMod.x;
                zoomConsts.ColorMod[1] = settings.GodRayColorMod.y;
                zoomConsts.ColorMod[2] = settings.GodRayColorMod.z;

                // Fade the rays out as the sun leaves the screen, so they don't pop at the viewport edge.
                if ( std::abs( zoomConsts.Center[0] - 0.5f ) > 0.5f )
                    zoomConsts.Weight *= std::max( 0.0f, 1.0f - ( std::abs( zoomConsts.Center[0] - 0.5f ) - 0.5f ) / 0.5f );
                if ( std::abs( zoomConsts.Center[1] - 0.5f ) > 0.5f )
                    zoomConsts.Weight *= std::max( 0.0f, 1.0f - ( std::abs( zoomConsts.Center[1] - 0.5f ) - 0.5f ) / 0.5f );

                zoomConsts.MaskIndex = m_GodRayMaskSrvSlot;
                zoomConsts.OutputIndex = m_GodRayZoomUavSlot;
            }
        }
    }

    if ( !heightFog && !godRays ) return;

    DX_ZONE( m_CmdList.Get(), "Height fog + god rays" );

    // The depth buffer is still bound as the DSV from the geometry passes — drop it (color target only)
    // before transitioning it to a shader-resource state. Combined NON_PIXEL|PIXEL because the god-ray mask
    // reads it from a COMPUTE shader and the composition from a PIXEL shader, in the same block.
    constexpr D3D12_RESOURCE_STATES kDepthRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    {
        m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, nullptr );
        auto b = TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, kDepthRead );
        m_CmdList->ResourceBarrier( 1, &b );
    }

    // ---------------------------------------------------------------------------------------------------
    // God rays: quarter-res mask -> quarter-res radial blur (both compute, both bindless).
    // ---------------------------------------------------------------------------------------------------
    if ( godRays ) {
        // Scene color must be readable by the mask CS; compute can't run with it bound as an RTV.
        if ( !m_SceneColorInPixelState ) {
            auto toSrv = TransitionBarrier( m_SceneColor.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
            m_CmdList->ResourceBarrier( 1, &toSrv );
            m_SceneColorInPixelState = true;
        }
        m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );

        const UINT gx = ( static_cast<UINT>( m_GodRaySize.x ) + 7 ) / 8;
        const UINT gy = ( static_cast<UINT>( m_GodRaySize.y ) + 7 ) / 8;

        m_CmdList->SetComputeRootSignature( m_Pipelines.Fog.GodRayRootSig.Get() );

        // --- Pass 1: mask (scene color + depth -> m_GodRayMask) ---
        GodRayMaskConsts maskConsts = { m_SceneColorSrvSlot, m_DepthSrvSlot, m_GodRayMaskUavSlot, 0 };
        m_CmdList->SetPipelineState( m_Pipelines.Fog.MaskPSO.Get() );
        m_CmdList->SetComputeRoot32BitConstants( 0, 4, &maskConsts, 0 );
        m_CmdList->Dispatch( gx, gy, 1 );

        // UAV write -> SRV read needs a real state transition, not just a UAV barrier (see RenderBloom).
        {
            auto b = TransitionBarrier( m_GodRayMask.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
            m_CmdList->ResourceBarrier( 1, &b );
        }

        // --- Pass 2: radial blur (m_GodRayMask -> m_GodRayZoom) ---
        m_CmdList->SetPipelineState( m_Pipelines.Fog.ZoomPSO.Get() );
        m_CmdList->SetComputeRoot32BitConstants( 0, 12, &zoomConsts, 0 );
        m_CmdList->Dispatch( gx, gy, 1 );

        // Result -> pixel-shader readable for the composition; mask back to its resting UNORDERED_ACCESS.
        {
            D3D12_RESOURCE_BARRIER b[2] = {
                TransitionBarrier( m_GodRayZoom.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ),
                TransitionBarrier( m_GodRayMask.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
            };
            m_CmdList->ResourceBarrier( 2, b );
        }
    }

    // ---------------------------------------------------------------------------------------------------
    // Composition: premultiplied fog + additive rays, blended straight onto the HDR scene color.
    // ---------------------------------------------------------------------------------------------------
    if ( heightFog ) {
        // Constant setup below is a line-for-line port of D3D11PfxRenderer::RenderPostFXComposition's
        // `if (settings.DrawFog)` block — same weight ramp, same weather/fog-override lerps, same clamps.
        HeightfogConstantBuffer cb = {};
        {
            auto& proj = Engine::GAPI->GetProjectionMatrix();
            cb.HF_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
            cb.HF_JitterOffset = float2( proj._13 * 0.5f, -proj._23 * 0.5f );
        }
        XMStoreFloat4x4( &cb.InvView, XMMatrixInverse( nullptr, Engine::GAPI->GetViewMatrixXM() ) );
        cb.CameraPosition = Engine::GAPI->GetCameraPosition();
        cb.HF_GlobalDensity = settings.FogGlobalDensity;
        cb.HF_HeightFalloff = settings.FogHeightFalloff;

        float height = settings.FogHeight;
        XMVECTOR color = XMLoadFloat3( &settings.FogColorMod );

        const float fnear = 15000.0f;
        const float ffar = 60000.0f;
        const float secScale = std::min<float>( settings.SectionDrawRadius, settings.FogRange );

        cb.HF_WeightZNear = std::max( 0.0f, WORLD_SECTION_SIZE * ( ( secScale - 0.5f ) * 0.7f ) - ( ffar - fnear ) );
        cb.HF_WeightZFar = WORLD_SECTION_SIZE * ( ( secScale - 0.5f ) * 0.8f );

        const float atmoMax = 83200.0f;
        const float atmoMin = 27799.9922f;
        cb.HF_WeightZFar = std::min( cb.HF_WeightZFar, atmoMax );
        cb.HF_WeightZNear = std::min( cb.HF_WeightZNear, atmoMin );

#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
        const float fogDensityFactor = 2;
        const float fogDensityFactorRain = ( 1.0f - Engine::GAPI->GetFogOverride() );
#else
        const float fogDensityFactor = pow( 15000.0f / Engine::GAPI->GetFarZ(), 4.0f );
        const float fogDensityFactorRain = 1.0f;
#endif

        if ( Engine::GAPI->GetFogOverride() > 0.0f ) {
            height = Toolbox::lerp( height, Engine::GAPI->GetCameraPosition().y + 10000, Engine::GAPI->GetFogOverride() );
            color = Engine::GAPI->GetFogColor();
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
            cb.HF_HeightFalloff = Toolbox::lerp( cb.HF_HeightFalloff, 0.000001f, Engine::GAPI->GetFogOverride() );
#endif
            cb.HF_GlobalDensity = Toolbox::lerp( cb.HF_GlobalDensity, cb.HF_GlobalDensity * fogDensityFactor, Engine::GAPI->GetFogOverride() );
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
            cb.HF_WeightZNear = Toolbox::lerp( cb.HF_WeightZNear, WORLD_SECTION_SIZE * 0.09f, Engine::GAPI->GetFogOverride() );
            cb.HF_WeightZFar = Toolbox::lerp( cb.HF_WeightZFar, WORLD_SECTION_SIZE * 0.8f, Engine::GAPI->GetFogOverride() );
#endif
        }

        cb.HF_FogHeight = height;
        cb.HF_ProjAB = float2( Engine::GAPI->GetProjectionMatrix()._33, Engine::GAPI->GetProjectionMatrix()._34 );

        const float rain = Engine::GAPI->GetRainFXWeight();
        XMFLOAT3 fogColorMod;
        XMStoreFloat3( &fogColorMod, XMVectorLerpV( color, XMLoadFloat3( &settings.RainFogColor ),
            XMVectorReplicate( std::min( 1.0f, rain * 2.0f ) ) ) );
        cb.HF_FogColorMod = fogColorMod;
        cb.HF_GlobalDensity = Toolbox::lerp( cb.HF_GlobalDensity, settings.RainFogDensity, rain * fogDensityFactorRain );

        memcpy( m_FogCBMapped[m_FrameIndex], &cb, sizeof( cb ) );

        // The atmosphere block is whatever GSky computed this frame (DrawSky calls GSky::RenderSky, which
        // refreshes AC_LightPos/AC_CameraPos/... even though D3D12 renders Gothic's fixed-function sky).
        GSky* sky = Engine::GAPI->GetSky();
        if ( sky ) {
            const auto& atmo = sky->GetAtmosphereCB();
            memcpy( m_FogCBMapped[m_FrameIndex] + kFogAtmosphereCbOffset, &atmo, sizeof( atmo ) );
        } else {
            memset( m_FogCBMapped[m_FrameIndex] + kFogAtmosphereCbOffset, 0, sizeof( AtmosphereConstantBuffer ) );
        }
    }

    // Scene color back to RENDER_TARGET (the god-ray mask pass may have flipped it to a read state).
    if ( m_SceneColorInPixelState ) {
        auto toRt = TransitionBarrier( m_SceneColor.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
        m_CmdList->ResourceBarrier( 1, &toRt );
        m_SceneColorInPixelState = false;
    }

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, nullptr );   // no DSV: depth is being read as an SRV
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    m_CmdList->IASetVertexBuffers( 0, 0, nullptr );

    FogCompositeConsts consts = {};
    consts.DepthIndex = m_DepthSrvSlot;
    consts.GodRaysIndex = godRays ? m_GodRayZoomSrvSlot : 0u;
    consts.Flags = ( heightFog ? kFogFlagHeightFog : 0u ) | ( godRays ? kFogFlagGodRays : 0u );

    m_CmdList->SetPipelineState( m_Pipelines.Fog.CompositePSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.Fog.CompositeRootSig.Get() );
    m_CmdList->SetGraphicsRootConstantBufferView( 0, m_FogCBGpu[m_FrameIndex] );                            // b0 height fog
    m_CmdList->SetGraphicsRootConstantBufferView( 1, m_FogCBGpu[m_FrameIndex] + kFogAtmosphereCbOffset );   // b1 atmosphere
    m_CmdList->SetGraphicsRoot32BitConstants( 2, 4, &consts, 0 );
    m_CmdList->DrawInstanced( 3, 1, 0, 0 );

    // Restore the resting states the rest of the frame (and the next one) expects: god-ray result back to
    // UNORDERED_ACCESS, depth back to DEPTH_WRITE. The scene color stays bound as the RTV, which is exactly
    // what RenderBloom (the next pass) assumes.
    {
        D3D12_RESOURCE_BARRIER b[2];
        UINT n = 0;
        if ( godRays )
            b[n++] = TransitionBarrier( m_GodRayZoom.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        b[n++] = TransitionBarrier( m_DepthBuffer.Get(), kDepthRead, D3D12_RESOURCE_STATE_DEPTH_WRITE );
        m_CmdList->ResourceBarrier( n, b );
    }

    m_ColorTargetIsHDR = true;
}
