#include "pch.h"
#include "D3D11PointLight.h"
#include "D3D11TiledDeferredShading.h"
#include "RenderToTextureBuffer.h"
#include "D3D11GraphicsEngineBase.h"
#include "D3D11GraphicsEngine.h" // TODO: Remove and use newer system!
#include "Engine.h"
#include "D3D11PfxRenderer.h"
#include "zCVobLight.h"
#include "BaseLineRenderer.h"
#include "oCVisFX.h"
#include "WorldConverter.h"
#include "ThreadPool.h"

const float LIGHT_COLORCHANGE_POS_MOD = 0.1f;

namespace
{
    std::unordered_set<const zCVob*> vobsToExclude = {};
    std::move_only_function<bool(const zCVob*) const> excludeVobsToExclude = []( const zCVob* vob )
    {
        return vobsToExclude.contains(vob);
    };
    
    void CollectVobTreeToExclude(const zCVob* vob) {
        while (vob && vobsToExclude.emplace(vob).second) {
            if (auto vfx = vob->As<oCVisualFX>()) {
                if (auto origin = vfx->GetOrigin()) {
                    vobsToExclude.emplace(origin);
                    CollectVobTreeToExclude(origin);
                }
            }
        
            vob = vob->GetVobParent();
        }
    }
}

// This allows us to exclude ourselves from producing shadows.
// This is needed for example in Returning, where the Belt-Light otherwise
// draws a huge shadow from the player all around and 
static void SetupVobsToExclude(const VobLightInfo* LightInfo)
{
    if (Engine::GAPI->GetRendererState().RendererSettings.AllowSelfShadowingPointlights) {
        return;
    }
    vobsToExclude.clear();
    
    CollectVobTreeToExclude(LightInfo->Vob);
}

static bool GetHasOriginVob( VobLightInfo* info ) {
    if ( !info->IsPFXVobLight ) {
        zCVob* vob = info->Vob;
        while ( auto parent = vob->GetVobParent() ) {
            if ( auto visFx = parent->As<oCVisualFX>() ) {
                if ( auto origin = visFx->GetOrigin(); origin && origin->As<oCItem>() ) {
                    return true;
                }
            } else if ( parent->As<oCItem>() ) {
                return true;
            }
            vob = parent;
        }
    }
    return false;
}

D3D11PointLight::D3D11PointLight( VobLightInfo* info, bool dynamicLight ) {
    LightInfo = info;
    DynamicLight = dynamicLight;
    
    // Ensure this light is actually in the VobLightMap
    // some lights don't seem to be in here!
    Engine::GAPI->VobLightMap[info->Vob] = info;

    LastUpdatePosition = LightInfo->Vob->GetPositionWorld();

    m_DepthCubemap = nullptr;
    m_StaticDepthCubemap = nullptr;
    WorldCacheInvalid = true;

    StartReInit();

    DrawnOnce = false;
    m_PendingInit = {};
}

D3D11PointLight::~D3D11PointLight() {
    // Make sure we are out of the init-queue
    m_PendingInit.cancel( ); // ensure any pending job is cancelled such that we get to InitDone state

    if ( m_PendingInit.future.valid() ) {
        
        for ( size_t i = 0; i < 3; ++i) {
            LogInfo() << "Waiting for pending init to finish before destroying light... Attempt " << (i+1);
            m_PendingInit.future.wait_for( std::chrono::milliseconds(100) );
        }
        m_PendingInit.future.wait();
    }

    ClearTiledSlot();
    ReleaseShadowMap();

    for ( auto& [k, mesh] : WorldMeshCache ) {
        SAFE_DELETE( mesh )
    }
}

void D3D11PointLight::AcquireShadowMap( DepthStencilPool* pool, int resolution ) {
    if ( m_DepthCubemap && m_CurrentResolution == resolution ) return;

    // If we have a map but it's the wrong size, return it to the pool first
    if ( m_DepthCubemap ) {
        ReleaseShadowMap();
    }

    DepthStencilPool::Description desc;
    desc.Width = resolution;
    desc.Height = resolution;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.DSVFormat = DXGI_FORMAT_D16_UNORM;
    desc.SRVFormat = DXGI_FORMAT_R16_UNORM;
    desc.ArraySize = 6;

    m_DepthCubemap = pool->Acquire( desc );
    m_CurrentResolution = resolution;

    // don't reset DrawnOnce here, or NPCs won't show up in the first frame a shadow gets a different LOD
    // DrawnOnce = false;
    StartReInit();
}

void D3D11PointLight::ReleaseShadowMap() {
    // This calls the custom deleter, returning the texture to the pool
    m_DepthCubemap.reset();
    ReleaseStaticAsideShadowMap();
    m_CurrentResolution = 0;
    DrawnOnce = false;
    m_HasDynamicOverlay = false;

}

void D3D11PointLight::SetTiledSlot( int slot, RenderToDepthStencilBuffer* target, D3D11TiledDeferredShading* owner ) {
    m_TiledSlotIndex = slot;
    m_TiledDepthTarget = target;
    m_TiledOwner = owner;

    StartReInit();
    DrawnOnce = false;
    m_StaticShadowReady = false;
    m_HasDynamicOverlay = false;
}

void D3D11PointLight::ClearTiledSlot() {
    if ( m_TiledSlotIndex >= 0 && m_TiledOwner ) {
        m_TiledOwner->FreeSlot( m_TiledSlotIndex );
    }
    m_TiledSlotIndex = -1;
    m_TiledDepthTarget = nullptr;
    m_TiledOwner = nullptr;
    m_StaticShadowReady = false;
    m_HasDynamicOverlay = false;
}

int D3D11PointLight::GetCurrentShadowMode() const {
    auto mode = static_cast<int>(Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows);
    if ( mode > 1 ) {
        if ( LightInfo->Vob->GetLightInfoFlags().isStatic ) {
            return GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY;
        }
    }
    return mode;
}

void D3D11PointLight::HandleShadowModeChange( int shadowMode ) {
    if ( m_LastShadowMode == shadowMode ) {
        return;
    }

    m_LastShadowMode = shadowMode;
    m_StaticShadowReady = false;
    DrawnOnce = false;
    m_HasDynamicOverlay = false;

    if ( shadowMode != GothicRendererSettings::PLS_UPDATE_DYNAMIC ) {
        ReleaseStaticAsideShadowMap();
    }
}

RenderToDepthStencilBuffer* D3D11PointLight::GetActiveShadowTarget() const {
    if ( m_TiledDepthTarget ) {
        return m_TiledDepthTarget;
    }
    if ( m_DepthCubemap ) {
        return m_DepthCubemap.get();
    }
    return nullptr;
}

void D3D11PointLight::AcquireStaticAsideShadowMap( DepthStencilPool* pool, int resolution ) {
    if ( !pool || resolution <= 0 ) {
        return;
    }

    if ( m_StaticDepthCubemap
        && static_cast<int>(m_StaticDepthCubemap->GetSizeX()) == resolution
        && static_cast<int>(m_StaticDepthCubemap->GetSizeY()) == resolution ) {
        return;
    }

    ReleaseStaticAsideShadowMap();

    DepthStencilPool::Description desc;
    desc.Width = resolution;
    desc.Height = resolution;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.DSVFormat = DXGI_FORMAT_D16_UNORM;
    desc.SRVFormat = DXGI_FORMAT_R16_UNORM;
    desc.ArraySize = 6;

    m_StaticDepthCubemap = pool->Acquire( desc );
    m_StaticShadowReady = false;
}

void D3D11PointLight::ReleaseStaticAsideShadowMap() {
    m_StaticDepthCubemap.reset();
    m_StaticShadowReady = false;
}

void D3D11PointLight::CopyStaticAsideToActiveTarget() const {
    if ( !m_StaticDepthCubemap ) {
        return;
    }

    RenderToDepthStencilBuffer* target = GetActiveShadowTarget();
    if ( !target ) {
        return;
    }

    ID3D11Texture2D* srcTexture = m_StaticDepthCubemap->GetTexture().Get();
    ID3D11Texture2D* dstTexture = target->GetTexture().Get();
    auto context = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext();
    if ( !srcTexture || !dstTexture || !context ) {
        return;
    }

    const UINT dstBaseSlice = m_TiledDepthTarget ? static_cast<UINT>(std::max( m_TiledSlotIndex, 0 ) * 6) : 0u;
    for ( UINT face = 0; face < 6; ++face ) {
        const UINT srcSubresource = D3D11CalcSubresource( 0, face, 1 );
        const UINT dstSubresource = D3D11CalcSubresource( 0, dstBaseSlice + face, 1 );
        context->CopySubresourceRegion( dstTexture, dstSubresource, 0, 0, 0, srcTexture, srcSubresource, nullptr );
    }
}

void D3D11PointLight::RenderStaticShadowPass( RenderToDepthStencilBuffer& target, bool clearDepth ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    const float range = LightInfo->Vob->GetLightRange();

    auto wc = &WorldMeshCache;
    if ( WorldCacheInvalid ) {
        wc = nullptr;
    }

    const unsigned int staticCasterMask = LightInfo->Vob->GetLightInfoFlags().isStatic
        ? SHADOW_CASTER_WORLD // static light? only draw world mesh.
        : SHADOW_CASTER_WORLD | SHADOW_CASTER_VOBS | SHADOW_CASTER_MOBS;

    if ( GetHasOriginVob( LightInfo ) ) {
        SetupVobsToExclude(LightInfo);
        engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), range, target, nullptr, nullptr, false, LightInfo->IsIndoorVob, false,
            &VobCache, &SkeletalVobCache, wc, clearDepth, staticCasterMask, excludeVobsToExclude );
        vobsToExclude.clear();
    } else {
        engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), range, target, nullptr, nullptr, false, LightInfo->IsIndoorVob, false,
            &VobCache, &SkeletalVobCache, wc, clearDepth, staticCasterMask );
    }
}

void D3D11PointLight::RenderAnimatedShadowPass( RenderToDepthStencilBuffer& target, bool clearDepth ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    const float range = LightInfo->Vob->GetLightRange();

    const unsigned int animatedCasterMask = SHADOW_CASTER_ANIMATED;
    if ( GetHasOriginVob( LightInfo ) ) {
        SetupVobsToExclude(LightInfo);
        engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), range, target, nullptr, nullptr, false, LightInfo->IsIndoorVob, false,
            nullptr, nullptr, nullptr, clearDepth, animatedCasterMask, excludeVobsToExclude );
        vobsToExclude.clear();
    } else {
        engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), range, target, nullptr, nullptr, false, LightInfo->IsIndoorVob, false,
            nullptr, nullptr, nullptr, clearDepth, animatedCasterMask );
    }
}

/** Returns true if this is the first time that light is being rendered */
bool D3D11PointLight::NotYetDrawn() {
    return !DrawnOnce;
}

/** Initializes the resources of this light */
void D3D11PointLight::InitResources() {
    InitDone = false;
    if (!LightInfo || !LightInfo->Vob) {
        // Light got removed before we could init, just return
        InitDone = true;
        return;
    }

    //Engine::GAPI->EnterResourceCriticalSection();

    // Generate worldmesh cache if we aren't a dynamically added light
    if ( !DynamicLight ) {
        WorldConverter::WorldMeshCollectPolyRange( LightInfo->Vob->GetPositionWorld(), LightInfo->Vob->GetLightRange(), Engine::GAPI->GetWorldSections(), WorldMeshCache );
        std::ranges::sort(WorldMeshCache, []( const auto& a, const auto& b ) {
            return std::tie(a.first.Material, a.first.Texture) < std::tie(b.first.Material, b.first.Texture);
        });
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

namespace {
    bool PositionEqualEps( const XMFLOAT3& a, const XMFLOAT3& b, float eps = 1.0f ) {
        const XMVECTOR va = XMLoadFloat3( &a );
        const XMVECTOR vb = XMLoadFloat3( &b );
        const XMVECTOR vEps = XMVectorReplicate( eps );
        return XMVector3NearEqual( va, vb, vEps );
    }
}

/** Returns if this light needs an update */
bool D3D11PointLight::NeedsUpdate() {
    if ( !IsReady() )
        return false;

    const int shadowMode = GetCurrentShadowMode();
    // Report the pending mode switch, but do NOT latch it here. HandleShadowModeChange() is what acts on the
    // transition (drops the cached static depth and the dynamic overlay), and it only runs once RenderCubemap()
    // is actually called - which is *after* DrawPointlightShadows() has asked us this question. Consuming
    // m_LastShadowMode here made that handler a no-op, so a light toggled to static kept advertising its
    // frozen overlay and burned that NPC into the shadow permanently.
    if ( shadowMode != m_LastShadowMode ) {
        return shadowMode > 0;
    }

    const bool moved = !PositionEqualEps(LastUpdatePosition,  LightInfo->Vob->GetPositionWorld());

    if ( shadowMode == GothicRendererSettings::PLS_STATIC_ONLY ) {
        return moved || !m_StaticShadowReady || NotYetDrawn();
    }

    if ( shadowMode == GothicRendererSettings::PLS_UPDATE_DYNAMIC ) {
        return moved || !m_StaticShadowReady || NotYetDrawn();
    }

    return moved || NotYetDrawn();
}

/** Returns true if the light could need an update, but it's not very important */
bool D3D11PointLight::WantsUpdate() {
    if ( !IsReady() )
        return false;

    const int shadowMode = GetCurrentShadowMode();
    if ( shadowMode <= GothicRendererSettings::PLS_STATIC_ONLY ) {
        return false;
    }

    // If dynamic, update colorchanging lights too, because they are mostly lamps and campfires
    // They wouldn't need an update just because of the colorchange, but most of them are dominant lights so it looks better
    if ( shadowMode >= GothicRendererSettings::PLS_UPDATE_DYNAMIC )
        if ( LightInfo->Vob->GetLightColor() != LastUpdateColor )
            return true;

    return false;
}

/** Draws the surrounding scene into the cubemap */
void D3D11PointLight::RenderCubemap( bool forceUpdate ) {
    if ( !IsReady() )
        return;
    if ( !m_DepthCubemap && !m_TiledDepthTarget )
        return;

    const int shadowMode = GetCurrentShadowMode();
    HandleShadowModeChange( shadowMode );

    //if (!GetAsyncKeyState('X'))
    //	return;
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine); // TODO: Remove and use newer system!

    XMFLOAT3 vobPos = LightInfo->Vob->GetPositionWorld();
    const bool moved = !PositionEqualEps(LastUpdatePosition, vobPos);

    if ( moved ) {
        // Position changed, refresh our caches
        VobCache.clear();
        SkeletalVobCache.clear();

        // Invalidate worldcache
        WorldCacheInvalid = true;
        m_StaticShadowReady = false;
        m_HasDynamicOverlay = false;
    }

    if ( shadowMode == GothicRendererSettings::PLS_STATIC_ONLY && !moved && m_StaticShadowReady && DrawnOnce ) {
        return;
    }

    if ( !NeedsUpdate() && !WantsUpdate() ) {
        if ( !forceUpdate )
            return; // Don't update when we don't need to
    }

    const XMVECTOR vEyePt = XMLoadFloat3( &vobPos );
    //vEyePt += XMVectorSet(0, 1, 0, 0) * 20.0f; // Move lightsource out of the ground or other objects (torches!)
    // TODO: Move the actual lightsource up too!

    const XMVECTOR c_XM_Right = XMVectorSet( 1.f, 0.f, 0.f, 0.f );
    const XMVECTOR c_XM_Left = XMVectorSet( -1.f, 0.f, 0.f, 0.f );
    const XMVECTOR c_XM_Up = XMVectorSet( 0.f, 1.f, 0.f, 0.f );
    const XMVECTOR c_XM_Down = XMVectorSet( 0.f, -1.f, 0.f, 0.f );
    const XMVECTOR c_XM_Forward = XMVectorSet( 0.f, 0.f, 1.f, 0.f );
    const XMVECTOR c_XM_Backward = XMVectorSet( 0.f, 0.f, -1.f, 0.f );

    // Update indoor/outdoor-state
    LightInfo->IsIndoorVob = LightInfo->Vob->IsIndoorVob();

    XMVECTOR vLookDir;
    // Generate cubemap view-matrices
    vLookDir = XMVectorAdd( c_XM_Right, vEyePt );
    XMStoreFloat4x4( &CubeMapViewMatrices[0], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Up ) ) );

    vLookDir = XMVectorAdd( c_XM_Left, vEyePt );
    XMStoreFloat4x4( &CubeMapViewMatrices[1], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Up ) ) );

    vLookDir = XMVectorAdd( c_XM_Up, vEyePt );
    XMStoreFloat4x4( &CubeMapViewMatrices[2], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Backward ) ) );

    vLookDir = XMVectorAdd( c_XM_Down, vEyePt );
    XMStoreFloat4x4( &CubeMapViewMatrices[3], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Forward ) ) );

    vLookDir = XMVectorAdd( c_XM_Forward, vEyePt );
    XMStoreFloat4x4( &CubeMapViewMatrices[4], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Up ) ) );

    vLookDir = XMVectorAdd( c_XM_Backward, vEyePt );
    XMStoreFloat4x4( &CubeMapViewMatrices[5], XMMatrixTranspose( XMMatrixLookAtLH( vEyePt, vLookDir, c_XM_Up ) ) );

    // Create the projection matrix
    float zNear = 15.0f;
    float zFar = LightInfo->Vob->GetLightRange()*2.0f;

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

    // Allocate the cubemap view-matrices CB from the per-frame dynamic ring pool
    ConstantBufferAllocation viewMatricesCB = engine->AllocateDynamicCB( &gcb, sizeof( gcb ) );
    engine->BindDynamicCBToVertexShader( 3, viewMatricesCB ); // Layered vertex shader
    engine->BindDynamicCBToGeometryShader( 2, viewMatricesCB ); // Cubemap geometry shader

    RenderFullCubemap();

    Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable = oldDepthClip;
    Engine::GAPI->GetRendererState().GraphicsState.SetGraphicsSwitch( GSWITCH_LINEAR_DEPTH, false );

    LastUpdateColor = LightInfo->Vob->GetLightColor();
    LastUpdatePosition = vobPos;
    DrawnOnce = true;
}

/** Renders all cubemap faces at once, using the geometry shader */
void D3D11PointLight::RenderFullCubemap() {
    if ( !IsReady() )
        return;
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine); // TODO: Remove and use newer system!
    auto _ = engine->RecordGraphicsEvent( GE_NAME("RenderFullCubemap->RenderFullCubemap") );

    RenderToDepthStencilBuffer* activeTarget = GetActiveShadowTarget();
    if ( !activeTarget ) {
        return;
    }

    const int shadowMode = GetCurrentShadowMode();
    if ( !m_StaticShadowReady && shadowMode == GothicRendererSettings::PLS_STATIC_ONLY ) {
        RenderStaticShadowPass( *activeTarget, true );
        m_StaticShadowReady = true;
        return;
    }

    if ( shadowMode == GothicRendererSettings::PLS_UPDATE_DYNAMIC ) {
        // Tiled path: static depth stays resident in this slot of the main cube array and is only re-rendered
        // when the light moves, while the moving casters go into the SAME slot of the dynamic-overlay array.
        // The shader min's the two, which reproduces the old composited cube without the per-update 6-face
        // CopySubresourceRegion (and without the per-light aside cube that fed it).
        if ( m_TiledSlotIndex >= 0 && m_TiledOwner ) {
            if ( RenderToDepthStencilBuffer* dynTarget = m_TiledOwner->GetDynSlotTarget( m_TiledSlotIndex ) ) {
                // Nothing composites out of the aside cube on this path, so give its slab back to the pool if
                // this light still holds one from the fallback path. Its clearing of m_StaticShadowReady is
                // wanted, not incidental: the active slot would still hold the fallback path's COMPOSITED
                // depth, so keeping it would bake that update's NPC in as a permanent static shadow.
                if ( m_StaticDepthCubemap ) {
                    ReleaseStaticAsideShadowMap();
                }

                if ( !m_StaticShadowReady ) {
                    RenderStaticShadowPass( *activeTarget, true );
                    m_StaticShadowReady = true;
                }

                // Clear: the overlay must hold ONLY this update's movers, never the previous one's.
                RenderAnimatedShadowPass( *dynTarget, true );
                m_HasDynamicOverlay = true;
                return;
            }
            // Overlay array unavailable - fall through to the composited single-cube path below.
        }

        DepthStencilPool* dsPool = engine->GetPfxRenderer()->GetDepthStencilPool();
        AcquireStaticAsideShadowMap( dsPool, m_CurrentResolution );

        if ( !m_StaticShadowReady) {
            if ( m_StaticDepthCubemap ) {
                RenderStaticShadowPass( *m_StaticDepthCubemap, true );
                m_StaticShadowReady = true;
            } else {
                // No aside buffer, we can't cache the static shadows.
                RenderStaticShadowPass( *activeTarget, true );
            }
        }

        if ( m_StaticDepthCubemap ) {
            CopyStaticAsideToActiveTarget();
        }

        RenderAnimatedShadowPass( *activeTarget, false );
        return;
    }
    
    if ( shadowMode == GothicRendererSettings::PLS_FULL ) {
        auto wc = &WorldMeshCache;
        if ( WorldCacheInvalid ) {
            wc = nullptr;
        }

        if ( GetHasOriginVob( LightInfo ) ) {
            SetupVobsToExclude(LightInfo);

            engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), LightInfo->Vob->GetLightRange(), *activeTarget,
                nullptr, nullptr, false, LightInfo->IsIndoorVob, false, &VobCache, &SkeletalVobCache, wc, true, SHADOW_CASTER_ALL,
                excludeVobsToExclude);
            vobsToExclude.clear();
        } else {
            engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), LightInfo->Vob->GetLightRange(), *activeTarget,
                nullptr, nullptr, false, LightInfo->IsIndoorVob, false, &VobCache, &SkeletalVobCache, wc, true, SHADOW_CASTER_ALL );
        }
    }
}

bool D3D11PointLight::IsReady()
{
    return InitDone
        && LightInfo
        && LightInfo->Vob;
}

void D3D11PointLight::Invalidate() {
    DrawnOnce = false;
    m_StaticShadowReady = false;
    m_HasDynamicOverlay = false;
    VobCache.clear();
    SkeletalVobCache.clear();
    WorldCacheInvalid = true;
}

void D3D11PointLight::StartReInit() {
    if ( !WorldCacheInvalid ) {
        return;
    }

    if ( !DynamicLight ) {
        // cancel() only sets the stop flag for a job that hasn't started reading it yet - it can't
        // interrupt one that's already inside InitResources(). If that job is still running when we
        // get called again (e.g. AcquireShadowMap re-evaluating resolution every frame, or a nearby
        // vob move invalidating the cache again before the previous refresh landed), enqueuing another
        // one here would race a second InitResources() against the first on the same WorldMeshCache/
        // VobCache/SkeletalVobCache vectors - unsynchronized concurrent mutation, real heap corruption,
        // not just staleness. Let the in-flight job finish; WorldCacheInvalid stays true until it does,
        // so the next StartReInit() call (from wherever) will pick up the invalidation then.
        if ( m_PendingInit.future.valid()
            && m_PendingInit.future.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready ) {
            return;
        }

        InitDone = false;

        // Add to queue
        m_PendingInit.cancel( ); // Cancel any pending init first, we only care about the latest one
        m_PendingInit = Engine::WorkerThreadPool->enqueue( [this] (const std::stop_token& token)
        {
            if (token.stop_requested()) {
                InitDone = true;
                return;
            }
            InitResources();
        } );

    } else {
        InitResources();
    }
}

/** Renders the scene with the given view-proj-matrices */
void D3D11PointLight::RenderCubemapFace( const XMFLOAT4X4& view, const XMFLOAT4X4& proj, UINT faceIdx ) {
    if ( !IsReady() )
        return;

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine); // TODO: Remove and use newer system!
    auto lightPos = LightInfo->Vob->GetPositionWorldXM();
    float range = LightInfo->Vob->GetLightRange();
    
    CameraReplacement cr;
    XMStoreFloat3( &cr.PositionReplacement, lightPos );
    cr.ProjectionReplacement = proj;
    cr.ViewReplacement = view;
    Frustum f;
    f.BuildCubemapFace( lightPos, range, faceIdx );
    cr.frustum = f;

    // Replace gothics camera
    Engine::GAPI->SetCameraReplacementPtr( &cr );

    if ( engine->GetDummyCubeRT() ) {
        const float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
        engine->GetContext()->ClearRenderTargetView( engine->GetDummyCubeRT()->GetRTVCubemapFace( faceIdx ).Get(), clearColor );
    }

    // Disable shadows for NPCs
    // TODO: Only for the player himself, because his shadows look ugly when using a torch
    //bool oldDrawSkel = Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes;
    //Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes = false;

    // Draw cubemap face
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV = engine->GetDummyCubeRT() != nullptr ? engine->GetDummyCubeRT()->GetRTVCubemapFace( faceIdx ) : nullptr;
    auto _ = engine->RecordGraphicsEvent( GE_NAME( "RenderFullCubemap->RenderCubemapFace" ) );
    engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), range, *m_DepthCubemap, m_DepthCubemap->GetDSVCubemapFace( faceIdx ).Get(), debugRTV.Get(), false );

    //Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes = oldDrawSkel;

    // Reset settings
    Engine::GAPI->SetCameraReplacementPtr( nullptr );
}

/** Binds the shadowmap to the pixelshader */
void D3D11PointLight::OnRenderLight() {
    if ( !IsReady() || !m_DepthCubemap)
        return;

    m_DepthCubemap->BindToPixelShader( reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext(), 3 );
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
        m_StaticShadowReady = false;
        m_HasDynamicOverlay = false;
    }

    if (vob->Vob == LightInfo->Vob) {
        // Our light got removed, release shadowmap
        ReleaseShadowMap();
        ClearTiledSlot();
    }

    //Engine::GAPI->LeaveResourceCriticalSection();
}
