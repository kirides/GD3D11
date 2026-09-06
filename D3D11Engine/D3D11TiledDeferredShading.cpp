#include "pch.h"
#include "D3D11TiledDeferredShading.h"

#include "D3D11GraphicsEngine.h"
#include "D3D11LegacyDeferredShading.h"
#include "D3D11PointLight.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "ConstantBufferStructs.h"
#include "D3D11PfxRenderer.h"
#include "D3D11_Helpers.h"
#include "RenderToTextureBuffer.h"
#include "zCVobLight.h"
#include "D3D11Effect.h"
#include "D3D11ShadowMap.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

void D3D11TiledDeferredShading::Init(
    const ComPtr<ID3D11Device1>& device,
    const ComPtr<ID3D11DeviceContext1>& context ) {
    m_device = device;
    m_context = context;

    // Light buffer: dynamic structured buffer for uploading per-frame light data
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = MAX_TILED_LIGHTS * sizeof( TiledPointLight );
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof( TiledPointLight );

        m_device->CreateBuffer( &desc, nullptr, m_LightBuffer.ReleaseAndGetAddressOf() );
        SetDebugName( m_LightBuffer.Get(), "TiledDeferred_LightBuffer" );

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.ElementWidth = MAX_TILED_LIGHTS;

        m_device->CreateShaderResourceView( m_LightBuffer.Get(), &srvDesc, m_LightBufferSRV.ReleaseAndGetAddressOf() );
        SetDebugName( m_LightBufferSRV.Get(), "TiledDeferred_LightBuffer_SRV" );
    }

    // The clustered cull writes a bitmask straight into the grid, so it needs no flat index list and no
    // atomic allocation counter - both are gone with the old per-tile cull.

    // Both cube arrays are lazy-created on the first Claim*Slot() to save memory when shadows are off
}

void D3D11TiledDeferredShading::EnsureDynShadowArray() {
    if ( m_ShadowDynArrayCreated ) return;
    m_ShadowDynArrayCreated = true;

    // The overlay tier: MAX_DYN_SHADOW_CUBEMAPS full-res cubes holding ONLY the moving casters of their slot.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = DYN_SHADOW_CUBE_SIZE;
    desc.Height = DYN_SHADOW_CUBE_SIZE;
    desc.MipLevels = 1;
    desc.ArraySize = MAX_DYN_SHADOW_CUBEMAPS * 6;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    // Declining this one only costs the movers: the lights keep their static cube. Failing QUIETLY would be
    // much worse - a null SRV reads as 0, i.e. every light carrying an overlay index goes fully shadowed.
    if ( FAILED( m_device->CreateTexture2D( &desc, nullptr, m_ShadowDynCubeArray.ReleaseAndGetAddressOf() ) ) ) {
        LogWarn() << "Failed to create the point-light dynamic shadow overlay array; moving casters will not "
            "appear in point-light shadows.";
        m_ShadowDynCubeArray.Reset();
        return;
    }
    SetDebugName( m_ShadowDynCubeArray.Get(), "TiledDeferred_ShadowDynCubeArray" );

    // 32-bit address space is scarce, so log what this costs (size^2 * 2 bytes * 6 faces * slots).
    LogInfo() << "Allocated point-light dynamic shadow overlay array: " << MAX_DYN_SHADOW_CUBEMAPS << " cubes @ "
        << DYN_SHADOW_CUBE_SIZE << "^2 R16 ("
        << ( static_cast<size_t>(DYN_SHADOW_CUBE_SIZE) * DYN_SHADOW_CUBE_SIZE * 2 * 6 * MAX_DYN_SHADOW_CUBEMAPS ) / (1024 * 1024 )
        << " MB)";

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    srvDesc.TextureCubeArray.MostDetailedMip = 0;
    srvDesc.TextureCubeArray.MipLevels = 1;
    srvDesc.TextureCubeArray.First2DArrayFace = 0;
    srvDesc.TextureCubeArray.NumCubes = MAX_DYN_SHADOW_CUBEMAPS;

    HRESULT hr;
    LE(m_device->CreateShaderResourceView( m_ShadowDynCubeArray.Get(), &srvDesc, m_ShadowDynCubeArraySRV.ReleaseAndGetAddressOf() ));
    SetDebugName( m_ShadowDynCubeArraySRV.Get(), "TiledDeferred_ShadowDynCubeArray_SRV" );

    for ( uint32_t slot = 0; slot < MAX_DYN_SHADOW_CUBEMAPS; slot++ ) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = slot * 6;
        dsvDesc.Texture2DArray.ArraySize = 6;
        dsvDesc.Texture2DArray.MipSlice = 0;

        LE(m_device->CreateDepthStencilView( m_ShadowDynCubeArray.Get(), &dsvDesc, m_SlotDynDSVs[slot].ReleaseAndGetAddressOf() ));

        // Single-slice DSV per face for the NVIDIA layered-rendering fallback (see
        // RequiresNvidiaTiledShadowFaceFallback), which binds them one at a time.
        std::array<ComPtr<ID3D11DepthStencilView>, 6> faceDSVs;
        D3D11_DEPTH_STENCIL_VIEW_DESC faceDsvDesc = dsvDesc;
        faceDsvDesc.Texture2DArray.ArraySize = 1;
        for ( uint32_t face = 0; face < 6; face++ ) {
            faceDsvDesc.Texture2DArray.FirstArraySlice = slot * 6 + face;
            LE(m_device->CreateDepthStencilView( m_ShadowDynCubeArray.Get(), &faceDsvDesc, faceDSVs[face].ReleaseAndGetAddressOf() ));
        }

        m_SlotDynViews[slot] = std::make_unique<RenderToDepthStencilBuffer>(
            m_ShadowDynCubeArray, m_SlotDynDSVs[slot], nullptr,
            DYN_SHADOW_CUBE_SIZE, DYN_SHADOW_CUBE_SIZE, faceDSVs.data() );

        // A fresh D3D11 texture holds UNDEFINED depth, and a comparison sample against 0 reads as fully
        // OCCLUDED - a slot nothing has drawn into yet would shade its light solid black. Nothing should
        // sample an undrawn slot (see PointLightSlotSelector::DynSlot::valid), so this is belt and braces:
        // it makes any future hole of that class a no-op instead of a black light.
        m_context->ClearDepthStencilView( m_SlotDynDSVs[slot].Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );
    }
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::ClaimDynSlot( int slot ) {
    if ( slot < 0 || static_cast<uint32_t>(slot) >= MAX_DYN_SHADOW_CUBEMAPS ) return nullptr;
    EnsureDynShadowArray();
    return GetDynSlotTarget( slot );
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::GetDynSlotTarget( int slot ) {
    if ( slot >= 0 && static_cast<uint32_t>(slot) < MAX_DYN_SHADOW_CUBEMAPS && m_ShadowDynCubeArray )
        return m_SlotDynViews[slot].get();
    return nullptr;
}


void D3D11TiledDeferredShading::EnsureStaticShadowArray() {
    if ( m_StaticShadowArrayCreated ) return;
    m_StaticShadowArrayCreated = true;

    // The core tier: MAX_STATIC_SHADOW_CUBEMAPS cubes, one per light, baked once and cached.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = STATIC_SHADOW_CUBE_SIZE;
    desc.Height = STATIC_SHADOW_CUBE_SIZE;
    desc.MipLevels = 1;
    desc.ArraySize = MAX_STATIC_SHADOW_CUBEMAPS * 6;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    // Fail closed rather than build an SRV/DSVs from a null resource - see MAX_STATIC_SHADOW_CUBEMAPS in the header.
    if ( FAILED( m_device->CreateTexture2D( &desc, nullptr, m_ShadowStaticCubeArray.ReleaseAndGetAddressOf() ) ) ) {
        LogWarn() << "Failed to create the point-light static shadow array; point lights will render unshadowed.";
        m_ShadowStaticCubeArray.Reset();
        return;
    }
    SetDebugName( m_ShadowStaticCubeArray.Get(), "TiledDeferred_ShadowStaticCubeArray" );

    LogInfo() << "Allocated point-light static shadow array: " << MAX_STATIC_SHADOW_CUBEMAPS << " cubes @ "
        << STATIC_SHADOW_CUBE_SIZE << "^2 R16 ("
        << ( static_cast<size_t>(STATIC_SHADOW_CUBE_SIZE) * STATIC_SHADOW_CUBE_SIZE * 2 * 6 * MAX_STATIC_SHADOW_CUBEMAPS ) / (1024 * 1024 )
        << " MB)";

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    srvDesc.TextureCubeArray.MostDetailedMip = 0;
    srvDesc.TextureCubeArray.MipLevels = 1;
    srvDesc.TextureCubeArray.First2DArrayFace = 0;
    srvDesc.TextureCubeArray.NumCubes = MAX_STATIC_SHADOW_CUBEMAPS;

    HRESULT hr;
    LE(m_device->CreateShaderResourceView( m_ShadowStaticCubeArray.Get(), &srvDesc, m_ShadowStaticCubeArraySRV.ReleaseAndGetAddressOf() ));
    SetDebugName( m_ShadowStaticCubeArraySRV.Get(), "TiledDeferred_ShadowStaticCubeArray_SRV" );

    for ( uint32_t slot = 0; slot < MAX_STATIC_SHADOW_CUBEMAPS; slot++ ) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = slot * 6;
        dsvDesc.Texture2DArray.ArraySize = 6;
        dsvDesc.Texture2DArray.MipSlice = 0;

        LE(m_device->CreateDepthStencilView( m_ShadowStaticCubeArray.Get(), &dsvDesc, m_StaticSlotDSVs[slot].ReleaseAndGetAddressOf() ));

        std::array<ComPtr<ID3D11DepthStencilView>, 6> faceDSVs;
        D3D11_DEPTH_STENCIL_VIEW_DESC faceDsvDesc = dsvDesc;
        faceDsvDesc.Texture2DArray.ArraySize = 1;
        for ( uint32_t face = 0; face < 6; face++ ) {
            faceDsvDesc.Texture2DArray.FirstArraySlice = slot * 6 + face;
            LE(m_device->CreateDepthStencilView( m_ShadowStaticCubeArray.Get(), &faceDsvDesc, faceDSVs[face].ReleaseAndGetAddressOf() ));
        }

        m_StaticSlotViews[slot] = std::make_unique<RenderToDepthStencilBuffer>(
            m_ShadowStaticCubeArray, m_StaticSlotDSVs[slot], nullptr,
            STATIC_SHADOW_CUBE_SIZE, STATIC_SHADOW_CUBE_SIZE, faceDSVs.data() );

        // See the identical note in EnsureDynShadowArray: undefined depth comparison-samples as fully
        // occluded, so every slot starts at "nothing occludes".
        m_context->ClearDepthStencilView( m_StaticSlotDSVs[slot].Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );
    }
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::ClaimStaticSlot( int slot ) {
    if ( slot < 0 || static_cast<uint32_t>(slot) >= MAX_STATIC_SHADOW_CUBEMAPS ) return nullptr;
    EnsureStaticShadowArray();
    return GetStaticSlotTarget( slot );
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::GetStaticSlotTarget( int slot ) {
    if ( slot >= 0 && static_cast<uint32_t>(slot) < MAX_STATIC_SHADOW_CUBEMAPS && m_ShadowStaticCubeArray )
        return m_StaticSlotViews[slot].get();
    return nullptr;
}

void D3D11TiledDeferredShading::EnsureBuffers( uint32_t numTilesX, uint32_t numTilesY ) {
    uint32_t totalTiles = numTilesX * numTilesY;

    if ( numTilesX == m_lastNumTilesX && numTilesY == m_lastNumTilesY )
        return;

    m_lastNumTilesX = numTilesX;
    m_lastNumTilesY = numTilesY;


    // One grid entry per CLUSTER: every tile column carries CLUSTER_Z_SLICES of them, contiguously, which is
    // the layout CS_LightCulling's flush and the consumers' clusterBase arithmetic both assume.
    const uint32_t totalClusters = totalTiles * CLUSTER_Z_SLICES;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = totalClusters * sizeof( LightGrid );
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof( LightGrid );

    HRESULT hr;

    LE(m_device->CreateBuffer( &desc, nullptr, m_LightGrid.ReleaseAndGetAddressOf() ));
    SetDebugName( m_LightGrid.Get(), "TiledDeferred_LightGrid" );

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.ElementWidth = totalClusters;

    LE(m_device->CreateShaderResourceView( m_LightGrid.Get(), &srvDesc, m_LightGridSRV.ReleaseAndGetAddressOf() ));
    SetDebugName( m_LightGridSRV.Get(), "TiledDeferred_LightGrid_SRV" );

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = totalClusters;

    LE(m_device->CreateUnorderedAccessView( m_LightGrid.Get(), &uavDesc, m_LightGridUAV.ReleaseAndGetAddressOf() ));
    SetDebugName( m_LightGridUAV.Get(), "TiledDeferred_LightGrid_UAV" );
}

XRESULT D3D11TiledDeferredShading::DrawPointlightLights(
    std::vector<VobLightInfo*>& lights,
    RenderToTextureBuffer& color,
    RenderToTextureBuffer& normals,
    RenderToTextureBuffer& specular,
    RenderToTextureBuffer& depthCopy ) {

    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "TiledPointlightLights" ) );
    auto& context = graphicsEngine->GetContext();

    // ---- Pass 1: Pack lights + cull ----
    auto cullResult = CullLights( lights, depthCopy );

    INT2 resolution = Engine::GraphicsEngine->GetResolution();
    uint32_t numTilesX = (resolution.x + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t numTilesY = (resolution.y + TILE_SIZE - 1) / TILE_SIZE;

    // ---- Pass 2: Tiled Shading (compute) ----
    if ( cullResult.TiledLightCount > 0 ) {
        auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
        XMMATRIX viewRaw = Engine::GAPI->GetViewMatrixXM();

        // Unbind HDR as RTV before binding as UAV
        ID3D11RenderTargetView* nullRTV = nullptr;
        context->OMSetRenderTargets( 1, &nullRTV, nullptr );

        auto csTiledShading = graphicsEngine->GetShaderManager().GetCShader( CShaderID::CS_TiledShading );
        csTiledShading->Apply();

        // Fill and bind shading constant buffer
        TiledShadingConstantBuffer shadeCB = {};
        shadeCB.ViewportSize = float2( static_cast<float>(resolution.x), static_cast<float>(resolution.y) );
        {
            auto& proj = Engine::GAPI->GetProjectionMatrix();
            shadeCB.ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
            shadeCB.JitterOffset = float2( proj._13 * 0.5f, -proj._23 * 0.5f );
        }
        shadeCB.LimitLightIntensity = settings.LimitLightIntesity ? 1 : 0;
        shadeCB.NumTilesX = numTilesX;
        shadeCB.ClusterNearZ = Engine::GAPI->GetNearPlane();
        shadeCB.ClusterFarZ = std::max( CLUSTER_MIN_FAR_Z, settings.VisualFXDrawRadius );
        XMStoreFloat4x4( &shadeCB.InvView, XMMatrixInverse( nullptr, viewRaw ) );

        // Rain wetness (same source data PS_DS_AtmosphericScattering.hlsl's SQ_RainViewProj is filled
        // from — see D3D11ShadowMap.cpp) so this, the PRIMARY point-light path, darkens/dampens wet
        // ground consistently with the sun/ambient pass instead of ignoring wetness entirely (see
        // RainWetnessSample.h).
        XMStoreFloat4x4( &shadeCB.RainViewProj,
            XMLoadFloat4x4( &graphicsEngine->Effects->GetRainShadowmapCameraRepl().ProjectionReplacement ) *
            XMLoadFloat4x4( &graphicsEngine->Effects->GetRainShadowmapCameraRepl().ViewReplacement ) );
        shadeCB.SceneWettness = Engine::GAPI->GetSceneWetness();

        csTiledShading->UpdateBuffer("TiledShadingConstantBuffer", &shadeCB, sizeof(shadeCB));

        // Bind GBuffer SRVs to CS
        context->CSSetShaderResources( 0, 1, color.GetShaderResView().GetAddressOf() );
        context->CSSetShaderResources( 1, 1, normals.GetShaderResView().GetAddressOf() );
        context->CSSetShaderResources( 2, 1, depthCopy.GetShaderResView().GetAddressOf() );
        context->CSSetShaderResources( 7, 1, specular.GetShaderResView().GetAddressOf() );

        // Bind linear sampler to CS slot 0 (required for GBuffer SampleLevel calls)
        ID3D11SamplerState* linearSampler = graphicsEngine->GetDefaultSamplerState();
        context->CSSetSamplers( 0, 1, &linearSampler );

        // Bind tiled data SRVs
        context->CSSetShaderResources( 8, 1, m_LightBufferSRV.GetAddressOf() );
        context->CSSetShaderResources( 9, 1, m_LightGridSRV.GetAddressOf() );

        // Rain shadowmap for ApplyPointLightWetness — same texture PS_DS_AtmosphericScattering.hlsl binds
        // at D3D11ShadowMap.h's TX_RainShadowmap slot, here on the CS stage instead of PS.
        if ( RenderToDepthStencilBuffer* rainShadowmap = graphicsEngine->Effects->GetRainShadowmap() )
            context->CSSetShaderResources( TX_RainShadowmap, 1, rainShadowmap->GetShaderResView().GetAddressOf() );

        // Bind comparison sampler unconditionally — the runtime validates at Dispatch
        // even if the shader branches around SampleCmpLevelZero
        graphicsEngine->GetShadowMaps()->BindSamplerToCS( context.Get(), 2 );

        // Overlay tier at t12, core tier at t13; either may be null.
        if ( cullResult.HasShadowedTiledLights ) {
            ID3D11ShaderResourceView* dynSRV = m_ShadowDynCubeArray ? m_ShadowDynCubeArraySRV.Get() : nullptr;
            context->CSSetShaderResources( 12, 1, &dynSRV );
            ID3D11ShaderResourceView* staticSRV = m_ShadowStaticCubeArray ? m_ShadowStaticCubeArraySRV.Get() : nullptr;
            context->CSSetShaderResources( 13, 1, &staticSRV );
        }

        // Bind HDR UAV
        auto& hdrUAV = graphicsEngine->GetHDRBackBuffer().GetUnorderedAccessView();
        context->CSSetUnorderedAccessViews( 0, 1, hdrUAV.GetAddressOf(), nullptr );

        context->Dispatch( numTilesX, numTilesY, 1 );

        // Unbind everything
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
        ID3D11ShaderResourceView* nullSRVs[14] = {};
        context->CSSetShaderResources( 0, 14, nullSRVs ); // t0-t13
        context->CSSetShader( nullptr, nullptr, 0 );

        // Restore HDR as RTV
        context->OMSetRenderTargets( 1, graphicsEngine->GetHDRBackBuffer().GetRenderTargetView().GetAddressOf(),
            graphicsEngine->GetDepthBuffer()->GetDepthStencilView().Get() );
    }

    // Draw lights that couldn't go through the tiled path (mismatched shadow cube size, overflow)
    if ( !cullResult.LegacyLights.empty() ) {
        D3D11LegacyDeferredShading legacy;
        legacy.DrawPointlightLights( cullResult.LegacyLights, color, normals, specular, depthCopy );
    }

    return XR_SUCCESS;
}

D3D11TiledDeferredShading::CullResult D3D11TiledDeferredShading::CullLights(
    std::vector<VobLightInfo*>& lights,
    RenderToTextureBuffer& depthCopy ) {

    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "CullLights" ) );
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    auto& context = graphicsEngine->GetContext();

    XMMATRIX viewRaw = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX view = XMMatrixTranspose( viewRaw );

    INT2 resolution = Engine::GraphicsEngine->GetResolution();
    uint32_t numTilesX = (resolution.x + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t numTilesY = (resolution.y + TILE_SIZE - 1) / TILE_SIZE;

    EnsureBuffers( numTilesX, numTilesY );

    CullResult result = {};

    // The single source of truth for which cubes a light owns and may sample this frame; filled by
    // DrawPointlightShadows earlier in the frame (PointLightSlotSelector::Select).
    const PointLightSlotSelector& pointSlots = graphicsEngine->GetShadowMaps()->GetPointSlots();

    bool hasShadowedTiledLights = false;

    // Map light buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    if ( !SUCCEEDED( context->Map( m_LightBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped ) ) ) {
        LogError() << "Failed to map light buffer.";
        return result;
    }
    TiledPointLight* lightData = reinterpret_cast<TiledPointLight*>(mapped.pData);

    const auto camPos = Engine::GAPI->GetCameraPositionXM();

    // Mirrors D3D12 BuildFrameLightBuffer's post-selection range clamp (D3D12Scene.cpp): a light that
    // ends up with no shadow cube shades unshadowed and bleeds through walls. Clamping its range to a
    // small fraction keeps it lighting its own alcove instead of the next room - or, worse, the
    // outside of the building it's sealed inside when the camera is outdoors and nothing can occlude
    // it. Gated on IsStatic() OR IsIndoorVob, not IsStatic() alone: zCVobLight's "static" bit is
    // Gothic's own IsStatic() (colour-animated fine, never repositioned), so a candle or brazier with
    // a colour animation reads as non-static and would otherwise bleed through walls completely
    // unshadowed - same failure mode as an atmospheric fill light. Outdoor dynamic lights (the
    // player's torch, spell effects) are still exempt: open air has no walls to bleed through.
    // Shrinking Range here (not just Color) also keeps the cluster cull from assigning the light to
    // distant tiles it can no longer reach.
    constexpr float kUnshadowedStaticScale = 0.35f;      // still lights its own alcove
    constexpr float kIndoorSeenFromOutsideScale = 0.15f; // worst bleed case - clamp it much harder
    const zCVob* playerVob = Engine::GAPI->GetPlayerVob();
    const bool cameraIndoors = playerVob && playerVob->IsIndoorVob();
    // Applied through a per-light eased scale rather than switched on and off - see
    // VobLightInfo::UnshadowedRangeScale. Framerate-independent exponential approach.
    constexpr float kClampEaseSeconds = 0.3f;
    const float clampDt = std::clamp( Engine::GAPI->GetFrameTimeSec(), 0.0f, 0.1f );
    const float clampEase = 1.0f - std::exp( -clampDt / kClampEaseSeconds );

    for ( auto const& light : lights ) {
        zCVobLight* vob = light->Vob;

        if ( !vob->IsEnabled() ) continue;

        // Entirely the slot table's answer: which cubes this light owns and whether they hold its own depth
        // yet. 0 = unshadowed. A slot never rendered for this owner would shade it black, so it is withheld.
        const int32_t shadowIndex = settings.EnablePointlightShadows > 0
            ? pointSlots.GetEncodedIndex( reinterpret_cast<uint64_t>( vob ) ) : 0;
        const bool hasShadow = shadowIndex != 0;

        if ( result.TiledLightCount >= MAX_TILED_LIGHTS )
            continue;

        vob->DoAnimation();

        float4 lightColor = float4( vob->GetLightColor() );
        float lightRange = vob->GetLightRange();
        if ( vob->IsStatic() || light->IsIndoorVob ) {
            const bool leakingOutdoors = light->IsIndoorVob && !cameraIndoors;
            const float target = hasShadow ? 1.0f
                : ( leakingOutdoors ? kIndoorSeenFromOutsideScale : kUnshadowedStaticScale );
            if ( light->UnshadowedRangeScale < 0.0f ) light->UnshadowedRangeScale = target;   // first sight
            else light->UnshadowedRangeScale += ( target - light->UnshadowedRangeScale ) * clampEase;
            if ( light->UnshadowedRangeScale < 0.999f ) lightRange *= light->UnshadowedRangeScale;
        }
        float3 posWorld = vob->GetPositionWorld();

        // Distance fade
        float dist;
        XMStoreFloat( &dist, XMVector3Length( XMLoadFloat3( &posWorld ) - camPos ) );

        if ( dist + lightRange < settings.VisualFXDrawRadius ) {
            float fadeEnd = settings.VisualFXDrawRadius;
            float fadeFactor = std::min( 1.0f, std::max( 0.0f, ((fadeEnd - (dist + lightRange)) / lightRange) ) );
            lightColor.x *= fadeFactor;
            lightColor.y *= fadeFactor;
            lightColor.z *= fadeFactor;
        }

        float lightFactor = 1.2f;
        lightColor.x *= lightFactor;
        lightColor.y *= lightFactor;
        lightColor.z *= lightFactor;

        if ( lightColor.x <= 0.0f && lightColor.y <= 0.0f && lightColor.z <= 0.0f )
            continue;

        XMVECTOR posWorldVec = XMLoadFloat3( &posWorld );
        XMFLOAT3 posView;
        XMStoreFloat3( &posView, XMVector3TransformCoord( posWorldVec, view ) );

        TiledPointLight& tl = lightData[result.TiledLightCount];
        tl.PositionView = posView;
        tl.Range = lightRange;
        tl.Color = XMFLOAT4( lightColor.x, lightColor.y, lightColor.z,
            settings.PointLightSpecularScale( vob->IsStatic() ) );
        tl.PositionWorld = XMFLOAT3( posWorld.x, posWorld.y, posWorld.z );
        // The range the cube was actually baked with, so the depth compare normalizes by the same far plane
        // the bake used - neither the animated range read above nor its unshadowed clamp.
        const float bakedRange = hasShadow ? pointSlots.GetCubeRangeOf( reinterpret_cast<uint64_t>( vob ) ) : 0.0f;
        tl.ShadowRange = bakedRange > 0.0f ? bakedRange : lightRange;

        tl.ShadowCubeIndex = shadowIndex;
        if ( hasShadow ) hasShadowedTiledLights = true;

        result.TiledLightCount++;
        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnLights++;
    }

    context->Unmap( m_LightBuffer.Get(), 0 );

    // Dispatch CS_LightCulling if we have lights
    if ( result.TiledLightCount > 0 ) {
        auto csLightCull = graphicsEngine->GetShaderManager().GetCShader( CShaderID::CS_LightCulling );
        csLightCull->Apply();

        const XMFLOAT4X4& proj = Engine::GAPI->GetProjectionMatrix();

        LightCullingConstantBuffer cullCB = {};
        cullCB.ProjScaleX = proj._11;
        cullCB.ProjScaleY = proj._22;
        cullCB.ScreenWidth = static_cast<uint32_t>(resolution.x);
        cullCB.ScreenHeight = static_cast<uint32_t>(resolution.y);
        cullCB.TotalLights = result.TiledLightCount;
        cullCB.NumTilesX = numTilesX;
        cullCB.NearZ = Engine::GAPI->GetNearPlane();
        // Tracks the range point lights are collected out to, so a light past the floor still lands in a
        // cluster instead of silently lighting nothing. Buffer size does not depend on this.
        cullCB.FarZ = std::max( CLUSTER_MIN_FAR_Z,
            Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius );

        csLightCull->UpdateBuffer("LightCullingConstantBuffer", &cullCB, sizeof(cullCB));

        // No depth input: the cluster grid comes from the frustum alone, which is what lets one grid serve
        // the opaque pass and anything drawn in front of it.
        context->CSSetShaderResources( 1, 1, m_LightBufferSRV.GetAddressOf() );

        ID3D11UnorderedAccessView* uavs[1] = { m_LightGridUAV.Get() };
        context->CSSetUnorderedAccessViews( 0, 1, uavs, nullptr );

        context->Dispatch( numTilesX, numTilesY, 1 );

        // Unbind
        ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
        context->CSSetUnorderedAccessViews( 0, 1, nullUAVs, nullptr );
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        context->CSSetShaderResources( 0, 2, nullSRVs );
        context->CSSetShader( nullptr, nullptr, 0 );
    }

    result.HasShadowedTiledLights = hasShadowedTiledLights;
    return result;
}
