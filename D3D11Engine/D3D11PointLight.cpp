#include "pch.h"
#include "D3D11PointLight.h"
#include "RenderToTextureBuffer.h"
#include "D3D11GraphicsEngineBase.h"
#include "D3D11GraphicsEngine.h" // TODO: Remove and use newer system!
#include "Engine.h"
#include "zCVobLight.h"
#include "BaseLineRenderer.h"
#include "WorldConverter.h"
#include "ThreadPool.h"

const float LIGHT_COLORCHANGE_POS_MOD = 0.1f;

D3D11PointLight::D3D11PointLight( VobLightInfo* info, bool dynamicLight ) {
    LightInfo = info;
    DynamicLight = dynamicLight;

    XMStoreFloat3( &LastUpdatePosition, LightInfo->Vob->GetPositionWorldXM() );

    DepthCubemap = nullptr;
    ViewMatricesCB = nullptr;

    if ( !dynamicLight ) {
        InitDone = false;

        // Add to queue
        Engine::WorkerThreadPool->enqueue( [this] { InitResources(); } );

    } else {
        InitResources();
    }

    DrawnOnce = false;
}

D3D11PointLight::~D3D11PointLight() {
    // Make sure we are out of the init-queue
    while ( !InitDone );

    DepthCubemap.reset();
    ViewMatricesCB.reset();

    for ( auto& [k, mesh] : WorldMeshCache ) {
        SAFE_DELETE( mesh );
    }
}

/** Returns true if this is the first time that light is being rendered */
bool D3D11PointLight::NotYetDrawn() {
    return !DrawnOnce;
}

/** Initializes the resources of this light */
void D3D11PointLight::InitResources() {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    //Engine::GAPI->EnterResourceCriticalSection();

    // Create texture-cube for this light
    DepthCubemap = std::make_unique<RenderToDepthStencilBuffer>( engine->GetDevice().Get(),
        POINTLIGHT_SHADOWMAP_SIZE,
        POINTLIGHT_SHADOWMAP_SIZE,
        DXGI_FORMAT_R16_TYPELESS,
        nullptr,
        DXGI_FORMAT_D16_UNORM,
        DXGI_FORMAT_R16_UNORM,
        6 );

    // Create constantbuffer for the view-matrices
    D3D11ConstantBuffer* cb = nullptr;
    engine->CreateConstantBuffer( &cb, nullptr, sizeof( CubemapGSConstantBuffer ) );
    ViewMatricesCB.reset( cb );

    // Generate worldmesh cache if we aren't a dynamically added light
    if ( !DynamicLight ) {
        WorldConverter::WorldMeshCollectPolyRange( LightInfo->Vob->GetPositionWorld(), LightInfo->Vob->GetLightRange(), Engine::GAPI->GetWorldSections(), WorldMeshCache );
        WorldCacheInvalid = false;
    } else {
        WorldCacheInvalid = true;
    }

    InitDone = true;

    //Engine::GAPI->LeaveResourceCriticalSection();
}

/** Returns if this light is inited already */
bool D3D11PointLight::IsInited() {
    return InitDone.load();
}

/** Returns if this light needs an update */
bool D3D11PointLight::NeedsUpdate() {
    FXMVECTOR lastPos = XMLoadFloat3( &LastUpdatePosition );
    return !XMVector3Equal( LightInfo->Vob->GetPositionWorldXM(), lastPos ) || NotYetDrawn();
}

/** Returns true if the light could need an update, but it's not very important */
bool D3D11PointLight::WantsUpdate() {
    // If dynamic, update colorchanging lights too, because they are mostly lamps and campfires
    // They wouldn't need an update just because of the colorchange, but most of them are dominant lights so it looks better
    if ( Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows >= GothicRendererSettings::PLS_UPDATE_DYNAMIC )
        if ( LightInfo->Vob->GetLightColor() != LastUpdateColor )
            return true;

    return false;
}

/** Draws the surrounding scene into the cubemap */
void D3D11PointLight::RenderCubemap( bool forceUpdate ) {
    if ( !InitDone )
        return;

    //if (!GetAsyncKeyState('X'))
    //	return;
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine); // TODO: Remove and use newer system!

    if ( !NeedsUpdate() && !WantsUpdate() ) {
        if ( !forceUpdate )
            return; // Don't update when we don't need to
    } else {
        FXMVECTOR xmlastPos = XMLoadFloat3( &LastUpdatePosition );
        if ( !XMVector3Equal( LightInfo->Vob->GetPositionWorldXM(), xmlastPos ) ) {
            // Position changed, refresh our caches
            VobCache.clear();
            SkeletalVobCache.clear();

            // Invalidate worldcache
            WorldCacheInvalid = true;
        }
    }

    FXMVECTOR vEyePt = LightInfo->Vob->GetPositionWorldXM();
    //vEyePt += XMVectorSet(0, 1, 0, 0) * 20.0f; // Move lightsource out of the ground or other objects (torches!)
    // TODO: Move the actual lightsource up too!

    XMVECTOR vLookDir;
    constexpr XMVECTORF32 c_XM_Right = { { {  1,  0,  0, 0 } } };
    constexpr XMVECTORF32 c_XM_Left = { { { -1,  0,  0, 0 } } };
    constexpr XMVECTORF32 c_XM_Up = { { {  0,  1,  0, 0 } } };
    constexpr XMVECTORF32 c_XM_Down = { { {  0, -1,  0, 0 } } };
    constexpr XMVECTORF32 c_XM_Forward = { { {  0,  0,  1, 0 } } };
    constexpr XMVECTORF32 c_XM_Backward = { { {  0,  0, -1, 0 } } };

    // Update indoor/outdoor-state
    LightInfo->IsIndoorVob = LightInfo->Vob->IsIndoorVob();

    // Generate cubemap view-matrices
    vLookDir = c_XM_Right + vEyePt;
    XMStoreFloat4x4( &CubeMapViewMatrices[0], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Up ) ) );

    vLookDir = c_XM_Left + vEyePt;
    XMStoreFloat4x4( &CubeMapViewMatrices[1], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Up ) ) );

    vLookDir = c_XM_Up + vEyePt;
    XMStoreFloat4x4( &CubeMapViewMatrices[2], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Backward ) ) );

    vLookDir = c_XM_Down + vEyePt;
    XMStoreFloat4x4( &CubeMapViewMatrices[3], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Forward ) ) );

    vLookDir = c_XM_Forward + vEyePt;
    XMStoreFloat4x4( &CubeMapViewMatrices[4], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Up ) ) );

    vLookDir = c_XM_Backward + vEyePt;
    XMStoreFloat4x4( &CubeMapViewMatrices[5], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Up ) ) );

    // Create the projection matrix
    float zNear = 15.0f;
    float zFar = LightInfo->Vob->GetLightRange() * 2.0f;

    XMMATRIX proj = XMMatrixPerspectiveFovLH( XM_PIDIV2, 1.0f, zNear, zFar );
    proj = XMMatrixTranspose( proj );

    // Setup near/far-planes. We need linear viewspace depth for the cubic shadowmaps.
    Engine::GAPI->GetRendererState().GraphicsState.FF_zNear = zNear;
    Engine::GAPI->GetRendererState().GraphicsState.FF_zFar = zFar;
    Engine::GAPI->GetRendererState().GraphicsState.SetGraphicsSwitch( GSWITCH_LINEAR_DEPTH, true );

    bool oldDepthClip = Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable;
    Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable = true;

    // Upload view-matrices to the GPU
    CubemapGSConstantBuffer gcb;
    for ( int i = 0; i < 6; i++ ) {
        gcb.PCR_View[i] = CubeMapViewMatrices[i];
        XMStoreFloat4x4( &gcb.PCR_ViewProj[i], proj * XMLoadFloat4x4( &CubeMapViewMatrices[i] ) );
    }

    ViewMatricesCB->UpdateBuffer( &gcb );
    ViewMatricesCB->BindToVertexShader( 3 ); // Layered vertex shader
    ViewMatricesCB->BindToGeometryShader( 2 ); // Cubemap geometry shader

    RenderFullCubemap();

    Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable = oldDepthClip;
    Engine::GAPI->GetRendererState().GraphicsState.SetGraphicsSwitch( GSWITCH_LINEAR_DEPTH, false );

    LastUpdateColor = LightInfo->Vob->GetLightColor();
    XMStoreFloat3( &LastUpdatePosition, vEyePt );
    DrawnOnce = true;
}

/** Renders all cubemap faces at once, using the geometry shader */
void D3D11PointLight::RenderFullCubemap() {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine); // TODO: Remove and use newer system!

    // Disable shadows for NPCs
    // TODO: Only for the player himself, because his shadows look ugly when using a torch
    //bool oldDrawSkel = Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes;
    //Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes = false;

    float range = LightInfo->Vob->GetLightRange() * 1.1f;

    // Draw no npcs if this is a static light. This is archived by simply not drawing them in the first update
    bool noNPCs = !DrawnOnce;//!LightInfo->Vob->IsStatic();

    // Draw cubemap
    std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>* wc = &WorldMeshCache;

    // Don't use the cache if we have moved
    if ( WorldCacheInvalid )
        wc = nullptr;

    engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), range, *DepthCubemap, nullptr, nullptr, false, LightInfo->IsIndoorVob, noNPCs, &VobCache, &SkeletalVobCache, wc );

    //Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes = oldDrawSkel;
}

/** Renders the scene with the given view-proj-matrices */
void D3D11PointLight::RenderCubemapFace( const XMFLOAT4X4& view, const XMFLOAT4X4& proj, UINT faceIdx ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine); // TODO: Remove and use newer system!
    CameraReplacement cr;
    XMStoreFloat3( &cr.PositionReplacement, LightInfo->Vob->GetPositionWorldXM() );
    cr.ProjectionReplacement = proj;
    cr.ViewReplacement = view;

    // Replace gothics camera
    Engine::GAPI->SetCameraReplacementPtr( &cr );

    if ( engine->GetDummyCubeRT() )
        engine->GetContext()->ClearRenderTargetView( engine->GetDummyCubeRT()->GetRTVCubemapFace( faceIdx ).Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );

    // Disable shadows for NPCs
    // TODO: Only for the player himself, because his shadows look ugly when using a torch
    //bool oldDrawSkel = Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes;
    //Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes = false;

    float range = LightInfo->Vob->GetLightRange() * 1.1f;

    // Draw cubemap face
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV = engine->GetDummyCubeRT() != nullptr ? engine->GetDummyCubeRT()->GetRTVCubemapFace( faceIdx ) : nullptr;
    engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), range, *DepthCubemap, DepthCubemap->GetDSVCubemapFace( faceIdx ).Get(), debugRTV.Get(), false );

    //Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes = oldDrawSkel;

    // Reset settings
    Engine::GAPI->SetCameraReplacementPtr( nullptr );
}

/** Binds the shadowmap to the pixelshader */
void D3D11PointLight::OnRenderLight() {
    if ( !InitDone )
        return;

    DepthCubemap->BindToPixelShader( reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext(), 3 );
}

/** Called when a vob got removed from the world */
void D3D11PointLight::OnVobRemovedFromWorld( BaseVobInfo* vob ) {
    // Wait for cache initialization to finish first
    //Engine::GAPI->EnterResourceCriticalSection();

    // See if we have this vob registered
    if ( std::find( VobCache.begin(), VobCache.end(), vob ) != VobCache.end()
        || std::find( SkeletalVobCache.begin(), SkeletalVobCache.end(), vob ) != SkeletalVobCache.end() ) {
        // Clear cache, if so
        VobCache.clear();
        SkeletalVobCache.clear();
        DrawnOnce = false;
    }

    //Engine::GAPI->LeaveResourceCriticalSection();
}
