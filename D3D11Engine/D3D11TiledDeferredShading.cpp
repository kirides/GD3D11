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

    // Index counter: single uint for atomic allocation
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof( uint32_t ) * 4; // Pad to 16 bytes
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof( uint32_t );

        m_device->CreateBuffer( &desc, nullptr, m_IndexCounter.ReleaseAndGetAddressOf() );
        SetDebugName( m_IndexCounter.Get(), "TiledDeferred_IndexCounter" );

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = 4;

        m_device->CreateUnorderedAccessView( m_IndexCounter.Get(), &uavDesc, m_IndexCounterUAV.ReleaseAndGetAddressOf() );
        SetDebugName( m_IndexCounterUAV.Get(), "TiledDeferred_IndexCounter_UAV" );
    }

    // Shadow cube array is lazy-created on first AllocateSlot() to save memory when shadows are off
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

    m_device->CreateTexture2D( &desc, nullptr, m_ShadowCubeArray.ReleaseAndGetAddressOf() );
    SetDebugName( m_ShadowCubeArray.Get(), "TiledDeferred_ShadowCubeArray" );

    // SRV for sampling in the tiled shading CS
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    srvDesc.TextureCubeArray.MostDetailedMip = 0;
    srvDesc.TextureCubeArray.MipLevels = 1;
    srvDesc.TextureCubeArray.First2DArrayFace = 0;
    srvDesc.TextureCubeArray.NumCubes = MAX_SHADOW_CUBEMAPS;

    m_device->CreateShaderResourceView( m_ShadowCubeArray.Get(), &srvDesc, m_ShadowCubeArraySRV.ReleaseAndGetAddressOf() );
    SetDebugName( m_ShadowCubeArraySRV.Get(), "TiledDeferred_ShadowCubeArray_SRV" );

    // Per-slot DSVs (6 faces each) and RenderToDepthStencilBuffer view wrappers
    for ( uint32_t slot = 0; slot < MAX_SHADOW_CUBEMAPS; slot++ ) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = slot * 6;
        dsvDesc.Texture2DArray.ArraySize = 6;
        dsvDesc.Texture2DArray.MipSlice = 0;

        m_device->CreateDepthStencilView( m_ShadowCubeArray.Get(), &dsvDesc, m_SlotDSVs[slot].ReleaseAndGetAddressOf() );

        // View wrapper for RenderShadowCube() interface (uses GetSizeX() and GetDepthStencilView())
        m_SlotViews[slot] = std::make_unique<RenderToDepthStencilBuffer>(
            m_ShadowCubeArray, m_SlotDSVs[slot], nullptr,
            SHADOW_CUBE_SIZE, SHADOW_CUBE_SIZE );
    }
}

int D3D11TiledDeferredShading::AllocateSlot() {
    EnsureShadowArray();
    for ( uint32_t i = 0; i < MAX_SHADOW_CUBEMAPS; i++ ) {
        if ( !m_SlotInUse[i] ) {
            m_SlotInUse[i] = true;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void D3D11TiledDeferredShading::FreeSlot( int slot ) {
    if ( slot >= 0 && static_cast<uint32_t>(slot) < MAX_SHADOW_CUBEMAPS )
        m_SlotInUse[slot] = false;
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::GetSlotTarget( int slot ) {
    if ( slot >= 0 && static_cast<uint32_t>(slot) < MAX_SHADOW_CUBEMAPS && m_ShadowArrayCreated )
        return m_SlotViews[slot].get();
    return nullptr;
}

void D3D11TiledDeferredShading::EnsureBuffers( uint32_t numTilesX, uint32_t numTilesY ) {
    uint32_t totalTiles = numTilesX * numTilesY;

    if ( numTilesX == m_lastNumTilesX && numTilesY == m_lastNumTilesY )
        return;

    m_lastNumTilesX = numTilesX;
    m_lastNumTilesY = numTilesY;


    // Light index list: global flat array of light indices
    {
        const uint32_t MAX_LIGHT_INDEX_ENTRIES = MAX_LIGHTS_PER_TILE * totalTiles;

        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = MAX_LIGHT_INDEX_ENTRIES * sizeof( uint32_t );
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof( uint32_t );

        m_device->CreateBuffer( &desc, nullptr, m_LightIndexList.ReleaseAndGetAddressOf() );
        SetDebugName( m_LightIndexList.Get(), "TiledDeferred_LightIndexList" );

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.ElementWidth = MAX_LIGHT_INDEX_ENTRIES;

        m_device->CreateShaderResourceView( m_LightIndexList.Get(), &srvDesc, m_LightIndexListSRV.ReleaseAndGetAddressOf() );
        SetDebugName( m_LightIndexListSRV.Get(), "TiledDeferred_LightIndexList_SRV" );

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = MAX_LIGHT_INDEX_ENTRIES;

        m_device->CreateUnorderedAccessView( m_LightIndexList.Get(), &uavDesc, m_LightIndexListUAV.ReleaseAndGetAddressOf() );
        SetDebugName( m_LightIndexListUAV.Get(), "TiledDeferred_LightIndexList_UAV" );
    }

    // Recreate light grid buffer for new tile count
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = totalTiles * sizeof( LightGrid );
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof( LightGrid );

    m_device->CreateBuffer( &desc, nullptr, m_LightGrid.ReleaseAndGetAddressOf() );
    SetDebugName( m_LightGrid.Get(), "TiledDeferred_LightGrid" );

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.ElementWidth = totalTiles;

    m_device->CreateShaderResourceView( m_LightGrid.Get(), &srvDesc, m_LightGridSRV.ReleaseAndGetAddressOf() );
    SetDebugName( m_LightGridSRV.Get(), "TiledDeferred_LightGrid_SRV" );

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = totalTiles;

    m_device->CreateUnorderedAccessView( m_LightGrid.Get(), &uavDesc, m_LightGridUAV.ReleaseAndGetAddressOf() );
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
        }
        shadeCB.LimitLightIntensity = settings.LimitLightIntesity ? 1 : 0;
        shadeCB.NumTilesX = numTilesX;
        XMStoreFloat4x4( &shadeCB.InvView, XMMatrixInverse( nullptr, viewRaw ) );

        csTiledShading->GetBuffer( "TiledShadingConstantBuffer" ).Update( &shadeCB ).Bind();

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
        context->CSSetShaderResources( 10, 1, m_LightIndexListSRV.GetAddressOf() );

        // Bind comparison sampler unconditionally — the runtime validates at Dispatch
        // even if the shader branches around SampleCmpLevelZero
        graphicsEngine->GetShadowMaps()->BindSamplerToCS( context.Get(), 2 );

        // Bind shadow cubemap array SRV
        if ( cullResult.HasShadowedTiledLights && m_ShadowArrayCreated ) {
            context->CSSetShaderResources( 11, 1, m_ShadowCubeArraySRV.GetAddressOf() );
        }

        // Bind HDR UAV
        auto& hdrUAV = graphicsEngine->GetHDRBackBuffer().GetUnorderedAccessView();
        context->CSSetUnorderedAccessViews( 0, 1, hdrUAV.GetAddressOf(), nullptr );

        context->Dispatch( numTilesX, numTilesY, 1 );

        // Unbind everything
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
        ID3D11ShaderResourceView* nullSRVs[12] = {};
        context->CSSetShaderResources( 0, 12, nullSRVs );
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

    for ( auto const& light : lights ) {
        zCVobLight* vob = light->Vob;

        if ( !vob->IsEnabled() ) continue;

        // Check if this light has shadows
        D3D11PointLight* pl = nullptr;
        bool hasShadow = false;
        if ( settings.EnablePointlightShadows > 0 ) {
            pl = light->LightShadowBuffers ? static_cast<D3D11PointLight*>(light->LightShadowBuffers.get()) : nullptr;
            if ( pl && pl->IsInited() && pl->HasShadowMap( 1 ) ) {
                hasShadow = true;
            }
        }

        // Shadowed lights need a tiled slot (assigned earlier in DrawPointlightShadows).
        if ( hasShadow && pl->GetTiledSlot() < 0 ) {
            continue;
        }

        if ( result.TiledLightCount >= MAX_TILED_LIGHTS )
            continue;

        vob->DoAnimation();

        float4 lightColor = float4( vob->GetLightColor() );
        float lightRange = vob->GetLightRange();
        float3 posWorld = vob->GetPositionWorld();

        // Distance fade
        float dist;
        XMStoreFloat( &dist, XMVector3Length( XMLoadFloat3( posWorld.toXMFLOAT3() ) - camPos ) );

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

        FXMVECTOR posWorldVec = XMLoadFloat3( posWorld.toXMFLOAT3() );
        XMFLOAT3 posView;
        XMStoreFloat3( &posView, XMVector3TransformCoord( posWorldVec, view ) );

        TiledPointLight& tl = lightData[result.TiledLightCount];
        tl.PositionView = posView;
        tl.Range = lightRange;
        tl.Color = XMFLOAT4( lightColor.x, lightColor.y, lightColor.z, lightColor.w );
        tl.PositionWorld = XMFLOAT3( posWorld.x, posWorld.y, posWorld.z );

        if ( hasShadow ) {
            tl.ShadowCubeIndex = pl->GetTiledSlot();
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

        LightCullingConstantBuffer cullCB = {};
        cullCB.Proj = Engine::GAPI->GetProjectionMatrix();
        cullCB.ScreenWidth = static_cast<uint32_t>(resolution.x);
        cullCB.ScreenHeight = static_cast<uint32_t>(resolution.y);
        cullCB.TotalLights = result.TiledLightCount;
        cullCB.MaxBufferIndices = (numTilesX * numTilesY) * MAX_LIGHTS_PER_TILE;

        csLightCull->GetBuffer( "LightCullingConstantBuffer" ).Update( &cullCB ).Bind();

        context->CSSetShaderResources( 0, 1, depthCopy.GetShaderResView().GetAddressOf() );
        context->CSSetShaderResources( 1, 1, m_LightBufferSRV.GetAddressOf() );

        UINT clearVal[4] = { 0, 0, 0, 0 };
        context->ClearUnorderedAccessViewUint( m_IndexCounterUAV.Get(), clearVal );

        ID3D11UnorderedAccessView* uavs[3] = { m_LightGridUAV.Get(), m_LightIndexListUAV.Get(), m_IndexCounterUAV.Get() };
        context->CSSetUnorderedAccessViews( 0, 3, uavs, nullptr );

        context->Dispatch( numTilesX, numTilesY, 1 );

        // Unbind
        ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
        context->CSSetUnorderedAccessViews( 0, 3, nullUAVs, nullptr );
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        context->CSSetShaderResources( 0, 2, nullSRVs );
        context->CSSetShader( nullptr, nullptr, 0 );
    }

    result.HasShadowedTiledLights = hasShadowedTiledLights;
    return result;
}
