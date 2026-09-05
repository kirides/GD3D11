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

    // Shadow cube array is lazy-created on the first ClaimSlot() to save memory when shadows are off
}

void D3D11TiledDeferredShading::EnsureShadowArray() {
    if ( m_ShadowArrayCreated ) return;
    m_ShadowArrayCreated = true;

    // TextureCubeArray as depth render target + shader resource (no copies needed)
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = SHADOW_CUBE_SIZE;
    desc.Height = SHADOW_CUBE_SIZE;
    desc.MipLevels = 1;
    desc.ArraySize = MAX_SHADOW_CUBEMAPS * 6;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE; 
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    HRESULT hr;
    LE( m_device->CreateTexture2D( &desc, nullptr, m_ShadowCubeArray.ReleaseAndGetAddressOf() ));
    SetDebugName( m_ShadowCubeArray.Get(), "TiledDeferred_ShadowCubeArray" );

    // SRV for sampling in the tiled shading CS
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    srvDesc.TextureCubeArray.MostDetailedMip = 0;
    srvDesc.TextureCubeArray.MipLevels = 1;
    srvDesc.TextureCubeArray.First2DArrayFace = 0;
    srvDesc.TextureCubeArray.NumCubes = MAX_SHADOW_CUBEMAPS;

    LE(m_device->CreateShaderResourceView( m_ShadowCubeArray.Get(), &srvDesc, m_ShadowCubeArraySRV.ReleaseAndGetAddressOf() ));
    SetDebugName( m_ShadowCubeArraySRV.Get(), "TiledDeferred_ShadowCubeArray_SRV" );

    // Per-slot DSVs (6 faces each) and RenderToDepthStencilBuffer view wrappers
    for ( uint32_t slot = 0; slot < MAX_SHADOW_CUBEMAPS; slot++ ) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = slot * 6;
        dsvDesc.Texture2DArray.ArraySize = 6;
        dsvDesc.Texture2DArray.MipSlice = 0;

        LE(m_device->CreateDepthStencilView( m_ShadowCubeArray.Get(), &dsvDesc, m_SlotDSVs[slot].ReleaseAndGetAddressOf() ));

        // Single-slice (ArraySize=1) DSV per face - the NVIDIA layered-rendering fallback (see
        // RequiresNvidiaTiledShadowFaceFallback) binds these one at a time instead of relying on
        // SV_RenderTargetArrayIndex to route into the multi-slice view above.
        std::array<ComPtr<ID3D11DepthStencilView>, 6> faceDSVs;
        D3D11_DEPTH_STENCIL_VIEW_DESC faceDsvDesc = dsvDesc;
        faceDsvDesc.Texture2DArray.ArraySize = 1;
        for ( uint32_t face = 0; face < 6; face++ ) {
            faceDsvDesc.Texture2DArray.FirstArraySlice = slot * 6 + face;
            LE(m_device->CreateDepthStencilView( m_ShadowCubeArray.Get(), &faceDsvDesc, faceDSVs[face].ReleaseAndGetAddressOf() ));
        }

        // View wrapper for RenderShadowCube() interface (uses GetSizeX() and GetDepthStencilView())
        m_SlotViews[slot] = std::make_unique<RenderToDepthStencilBuffer>(
            m_ShadowCubeArray, m_SlotDSVs[slot], nullptr,
            SHADOW_CUBE_SIZE, SHADOW_CUBE_SIZE, faceDSVs.data() );
    }
}

void D3D11TiledDeferredShading::EnsureDynShadowArray() {
    if ( m_ShadowDynArrayCreated ) return;
    m_ShadowDynArrayCreated = true;

    // Same shape and slot indexing as m_ShadowCubeArray - the shader addresses both with one slot index.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = SHADOW_CUBE_SIZE;
    desc.Height = SHADOW_CUBE_SIZE;
    desc.MipLevels = 1;
    desc.ArraySize = MAX_SHADOW_CUBEMAPS * 6;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    // Unlike the main array this one can be declined without losing shadows entirely: GetDynSlotTarget()
    // then returns null and the lights fall back to the composited single-cube path. Failing quietly would
    // be much worse - a null SRV reads as 0, i.e. every light carrying the flag goes fully shadowed.
    if ( FAILED( m_device->CreateTexture2D( &desc, nullptr, m_ShadowDynCubeArray.ReleaseAndGetAddressOf() ) ) ) {
        LogWarn() << "Failed to create the point-light dynamic shadow overlay array; falling back to the "
            "composited static+dynamic cube path.";
        m_ShadowDynCubeArray.Reset();
        return;
    }
    SetDebugName( m_ShadowDynCubeArray.Get(), "TiledDeferred_ShadowDynCubeArray" );

    // 32-bit address space is scarce, so log what this costs (SHADOW_CUBE_SIZE^2 * 2 bytes * 6 faces * slots).
    LogInfo() << "Allocated point-light dynamic shadow overlay array: " << MAX_SHADOW_CUBEMAPS << " cubes @ "
        << SHADOW_CUBE_SIZE << "^2 R16 ("
        << ( static_cast<size_t>(SHADOW_CUBE_SIZE) * SHADOW_CUBE_SIZE * 2 * 6 * MAX_SHADOW_CUBEMAPS ) / (1024 * 1024 )
        << " MB)";

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    srvDesc.TextureCubeArray.MostDetailedMip = 0;
    srvDesc.TextureCubeArray.MipLevels = 1;
    srvDesc.TextureCubeArray.First2DArrayFace = 0;
    srvDesc.TextureCubeArray.NumCubes = MAX_SHADOW_CUBEMAPS;

    HRESULT hr;
    LE(m_device->CreateShaderResourceView( m_ShadowDynCubeArray.Get(), &srvDesc, m_ShadowDynCubeArraySRV.ReleaseAndGetAddressOf() ));
    SetDebugName( m_ShadowDynCubeArraySRV.Get(), "TiledDeferred_ShadowDynCubeArray_SRV" );
    
    for ( uint32_t slot = 0; slot < MAX_SHADOW_CUBEMAPS; slot++ ) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = slot * 6;
        dsvDesc.Texture2DArray.ArraySize = 6;
        dsvDesc.Texture2DArray.MipSlice = 0;

        LE(m_device->CreateDepthStencilView( m_ShadowDynCubeArray.Get(), &dsvDesc, m_SlotDynDSVs[slot].ReleaseAndGetAddressOf() ));

        std::array<ComPtr<ID3D11DepthStencilView>, 6> faceDSVs;
        D3D11_DEPTH_STENCIL_VIEW_DESC faceDsvDesc = dsvDesc;
        faceDsvDesc.Texture2DArray.ArraySize = 1;
        for ( uint32_t face = 0; face < 6; face++ ) {
            faceDsvDesc.Texture2DArray.FirstArraySlice = slot * 6 + face;
            LE(m_device->CreateDepthStencilView( m_ShadowDynCubeArray.Get(), &faceDsvDesc, faceDSVs[face].ReleaseAndGetAddressOf() ));
        }

        m_SlotDynViews[slot] = std::make_unique<RenderToDepthStencilBuffer>(
            m_ShadowDynCubeArray, m_SlotDynDSVs[slot], nullptr,
            SHADOW_CUBE_SIZE, SHADOW_CUBE_SIZE, faceDSVs.data() );
    }
}

void D3D11TiledDeferredShading::EnsureStaticShadowArray() {
    if ( m_StaticShadowArrayCreated ) return;
    m_StaticShadowArrayCreated = true;

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
        LogWarn() << "Failed to create the low-res static-only point-light shadow array; static point lights "
            "will render unshadowed.";
        m_ShadowStaticCubeArray.Reset();
        return;
    }
    SetDebugName( m_ShadowStaticCubeArray.Get(), "TiledDeferred_ShadowStaticCubeArray" );

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
    }
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::ClaimStaticSlot( int slot ) {
    if ( slot < 0 || static_cast<uint32_t>(slot) >= MAX_STATIC_SHADOW_CUBEMAPS ) return nullptr;
    EnsureStaticShadowArray();
    return GetStaticSlotTarget( slot );
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::GetStaticSlotTarget( int slot ) {
    if ( slot >= 0 && static_cast<uint32_t>(slot) < MAX_STATIC_SHADOW_CUBEMAPS && m_StaticShadowArrayCreated )
        return m_StaticSlotViews[slot].get();
    return nullptr;
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::ClaimSlot( int slot ) {
    if ( slot < 0 || static_cast<uint32_t>(slot) >= MAX_SHADOW_CUBEMAPS ) return nullptr;
    EnsureShadowArray();
    return GetSlotTarget( slot );
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::GetSlotTarget( int slot ) {
    if ( slot >= 0 && static_cast<uint32_t>(slot) < MAX_SHADOW_CUBEMAPS && m_ShadowArrayCreated )
        return m_SlotViews[slot].get();
    return nullptr;
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::GetDynSlotTarget( int slot ) {
    if ( slot < 0 || static_cast<uint32_t>(slot) >= MAX_SHADOW_CUBEMAPS )
        return nullptr;
    EnsureDynShadowArray();
    return m_SlotDynViews[slot].get();
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

        // Static at t11, dynamic overlay at t12, low-res static-only tier at t13.
        if ( cullResult.HasShadowedTiledLights && m_ShadowArrayCreated ) {
            context->CSSetShaderResources( 11, 1, m_ShadowCubeArraySRV.GetAddressOf() );
            ID3D11ShaderResourceView* dynSRV = m_ShadowDynArrayCreated ? m_ShadowDynCubeArraySRV.Get() : nullptr;
            context->CSSetShaderResources( 12, 1, &dynSRV );
        }
        if ( cullResult.HasShadowedTiledLights && m_StaticShadowArrayCreated ) {
            context->CSSetShaderResources( 13, 1, m_ShadowStaticCubeArraySRV.GetAddressOf() );
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

    // Partition lights: all lights go tiled where possible.
    // Shadowed lights with a tiled slot render directly into the shared array (no copies).
    // Shadowed lights without a tiled slot (256x256 or overflow) fall back to legacy.
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
    // The clamp is applied THROUGH a per-light eased scale rather than switched on and off - see
    // VobLightInfo::UnshadowedRangeScale. Framerate-independent exponential approach, ~0.3 s.
    constexpr float kClampEaseSeconds = 0.3f;
    const float clampDt = std::clamp( Engine::GAPI->GetFrameTimeSec(), 0.0f, 0.1f );
    const float clampEase = 1.0f - std::exp( -clampDt / kClampEaseSeconds );

    for ( auto const& light : lights ) {
        zCVobLight* vob = light->Vob;

        if ( !vob->IsEnabled() ) continue;

        // Check if this light has shadows
        D3D11PointLight* pl = nullptr;
        bool hasShadow = false;
        if ( settings.EnablePointlightShadows > 0 ) {
            pl = light->LightShadowBuffers ? static_cast<D3D11PointLight*>(light->LightShadowBuffers.get()) : nullptr;
            // HasOwnDepthInSlot() is load-bearing, not a formality: owning a slot is not the same as that
            // slot holding THIS light's depth. A freshly assigned (or re-assigned) slot still contains
            // whatever the previous occupant rendered there, or nothing at all, and the render that fills it
            // is rationed to a handful of lights per frame in DrawPointlightShadows. Advertising the slot in
            // the meantime samples that foreign depth and shades the light as fully occluded - it goes BLACK,
            // which is far worse than the range-clamped unshadowed look it gets by waiting for its turn.
            // Mirrors D3D12's "publish the cube index only once the slot holds this owner's depth" - and,
            // like that rule, it deliberately keeps advertising a slot whose bake has merely been
            // INVALIDATED: stale depth is a small artefact, no depth at all is a light that switches off.
            // HasSampleableDepth, not HasOwnDepthInSlot: a light mid-tier-handover has no depth in its NEW
            // slot yet but its old one is being held back for exactly this - see SetSlotFallback.
            if ( pl && pl->IsInited() && pl->HasShadowMap( 1 ) && pl->HasSampleableDepth() ) {
                hasShadow = true;
            }
        }

        // Shadowed lights need a tiled slot (assigned earlier in DrawPointlightShadows).
        if ( hasShadow && pl->GetSampleSlot() < 0 ) {
            continue;
        }

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

        if ( hasShadow ) {
            // The overlay bit belongs to the light's OWN slot only - the one it is handing over from was
            // never refreshed for this frame and its dynamic twin holds somebody else's casters.
            tl.ShadowCubeIndex = pl->GetSampleSlot()
                | ( pl->IsSampleSlotLowRes() ? SHADOW_CUBE_TIER_LOW : 0 )
                | ( pl->HasOwnDepthInSlot() && pl->HasDynamicShadowOverlay() ? SHADOW_CUBE_HAS_DYNAMIC : 0 );
            hasShadowedTiledLights = true;
        } else {
            tl.ShadowCubeIndex = -1;
        }

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
