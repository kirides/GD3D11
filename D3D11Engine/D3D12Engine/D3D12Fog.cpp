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
#include "D3D12ResourceCreate.h"
#include "D3D12RenderGraph.h"
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


// God-ray mask + zoom textures used to be built here as members (see CreateFogResources's former home) — both
// are purely single-frame scratch (written then read once then dead, no cross-frame data dependency), so they
// are now D3D12RenderGraph-managed transient textures acquired fresh every call inside RenderFogAndGodRays
// instead. Same conversion DoF's scratch textures got — see D3D12DoF.cpp.


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
    // in the (settings-wise possible) DrawFog=off case. No resource-readiness check any more: the mask/zoom
    // textures are graph-managed transients acquired on demand below, not built ahead of time.
    bool godRays = settings.EnableGodRays
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

                // MaskIndex/OutputIndex are resolved once the graph actually places the two textures below
                // (their SRV/UAV slots don't exist yet at this point).
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
        m_CmdList->TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, kDepthRead );
    }

    // ---------------------------------------------------------------------------------------------------
    // God rays: quarter-res mask -> quarter-res radial blur (both compute, both bindless), through a local
    // D3D12RenderGraph — same conversion DoF's scratch textures got (see D3D12DoF.cpp), and for the same
    // reason: both textures are written then read once then dead, with no cross-frame data dependency.
    // ---------------------------------------------------------------------------------------------------
    UINT godRayZoomSrvSlot = UINT_MAX;   // resolved by the Zoom pass below; read by the composition further down
    if ( godRays ) {
        // Scene color must be readable by the mask CS; compute can't run with it bound as an RTV. Not
        // graph-managed (m_SceneColor is a plain member, not imported), so this happens before the graph runs.
        if ( !m_SceneColorInPixelState ) {
            m_CmdList->TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
            m_SceneColorInPixelState = true;
        }
        m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );

        const INT2 godRaySize = { std::max( 1, m_Resolution.x / 4 ), std::max( 1, m_Resolution.y / 4 ) };
        const UINT gx = ( static_cast<UINT>( godRaySize.x ) + 7 ) / 8;
        const UINT gy = ( static_cast<UINT>( godRaySize.y ) + 7 ) / 8;

        D3D12RenderGraph fogGraph( &m_AliasArena );
        RGResourceHandle maskHandle = RG_INVALID_HANDLE;
        RGResourceHandle zoomHandle = RG_INVALID_HANDLE;

        // --- Pass 1: mask (scene color + depth -> mask) ---
        fogGraph.AddPass( RG_PASS_NAME( "God Ray Mask" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
            maskHandle = builder.CreateTexture( { static_cast<uint32_t>( godRaySize.x ), static_cast<uint32_t>( godRaySize.y ),
                static_cast<int>( kSceneColorFormat ), L"GodRayMask", 1u } );

            pass.m_executeCallback = [this, gx, gy, maskHandle]( const D3D12RenderGraph& graph, D3D12CmdList& cmdList ) {
                D3D12RenderTarget* mask = graph.GetPhysicalTexture( maskHandle );
                if ( !mask ) return;

                if ( mask->State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) {
                    cmdList.TransitionBarrier( mask->GetResource(), mask->State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
                    mask->State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                }

                GodRayMaskConsts maskConsts = { m_SceneColorSrvSlot, m_DepthSrvSlot, mask->GetUavSlot(), 0 };
                cmdList.SetComputeRootSignature( m_Pipelines.Fog.GodRayRootSig.Get() );
                cmdList.SetPipelineState( m_Pipelines.Fog.MaskPSO.Get() );
                cmdList.SetComputeRoot32BitConstants( 0, 4, &maskConsts, 0 );
                cmdList.Dispatch( gx, gy, 1 );

                // UAV write -> SRV read needs a real state transition, not just a UAV barrier (see RenderBloom).
                cmdList.TransitionBarrier( mask->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
                mask->State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                };
            } );

        // --- Pass 2: radial blur (mask -> zoom) ---
        fogGraph.AddPass( RG_PASS_NAME( "God Ray Zoom" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
            builder.Read( maskHandle );
            zoomHandle = builder.CreateTexture( { static_cast<uint32_t>( godRaySize.x ), static_cast<uint32_t>( godRaySize.y ),
                static_cast<int>( kSceneColorFormat ), L"GodRayZoom", 1u } );
            // The composition draw further down reads this pass's result via godRayZoomSrvSlot, a plain
            // local captured by reference — not a graph Read(), so mark the side effect explicitly (see
            // D3D12RenderPass::m_hasExternalSideEffect).
            builder.MarkExternalEffect();

            pass.m_executeCallback = [this, gx, gy, &zoomConsts, &godRayZoomSrvSlot, maskHandle, zoomHandle]( const D3D12RenderGraph& graph, D3D12CmdList& cmdList ) {
                D3D12RenderTarget* mask = graph.GetPhysicalTexture( maskHandle );
                D3D12RenderTarget* zoom = graph.GetPhysicalTexture( zoomHandle );
                if ( !mask || !zoom ) return;

                if ( zoom->State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) {
                    cmdList.TransitionBarrier( zoom->GetResource(), zoom->State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
                    zoom->State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                }

                zoomConsts.MaskIndex = mask->GetSrvSlot();
                zoomConsts.OutputIndex = zoom->GetUavSlot();
                cmdList.SetComputeRootSignature( m_Pipelines.Fog.GodRayRootSig.Get() );
                cmdList.SetPipelineState( m_Pipelines.Fog.ZoomPSO.Get() );
                cmdList.SetComputeRoot32BitConstants( 0, 12, &zoomConsts, 0 );
                cmdList.Dispatch( gx, gy, 1 );

                // Result -> pixel-shader readable for the composition below.
                cmdList.TransitionBarrier( zoom->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
                zoom->State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                godRayZoomSrvSlot = zoom->GetSrvSlot();
                };
            } );

        fogGraph.Compile();
        fogGraph.Execute( m_CmdList );
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
        m_CmdList->TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
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
    consts.GodRaysIndex = godRays ? godRayZoomSrvSlot : 0u;
    consts.Flags = ( heightFog ? kFogFlagHeightFog : 0u ) | ( godRays ? kFogFlagGodRays : 0u );

    m_CmdList->SetPipelineState( m_Pipelines.Fog.CompositePSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.Fog.CompositeRootSig.Get() );
    m_CmdList->SetGraphicsRootConstantBufferView( 0, m_FogCBGpu[m_FrameIndex] );                            // b0 height fog
    m_CmdList->SetGraphicsRootConstantBufferView( 1, m_FogCBGpu[m_FrameIndex] + kFogAtmosphereCbOffset );   // b1 atmosphere
    m_CmdList->SetGraphicsRoot32BitConstants( 2, 4, &consts, 0 );
    m_CmdList->DrawInstanced( 3, 1, 0, 0 );

    // Restore the resting state the rest of the frame (and the next one) expects: depth back to DEPTH_WRITE.
    // The scene color stays bound as the RTV, which is exactly what RenderBloom (the next pass) assumes. The
    // god-ray zoom texture needs no explicit reset any more — D3D12RenderTarget::State is caller-maintained
    // and self-correcting: next frame's Zoom pass checks it and transitions from whatever it actually finds
    // (see the pass callback above), the same way DoF's scratch textures work.
    m_CmdList->TransitionBarrier( m_DepthBuffer.Get(), kDepthRead, D3D12_RESOURCE_STATE_DEPTH_WRITE );

    m_ColorTargetIsHDR = true;
}
