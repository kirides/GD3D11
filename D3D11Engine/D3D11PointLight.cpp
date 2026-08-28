#include "pch.h"
#include "D3D11PointLight.h"
#include "D3D11TiledDeferredShading.h"
#include "RenderToTextureBuffer.h"
#include "D3D11GraphicsEngineBase.h"
#include "D3D11GraphicsEngine.h" // TODO: Remove and use newer system!
#include "Engine.h"
#include "D3D11PfxRenderer.h"
#include "zCVobLight.h"
#include "oCVisFX.h"
#include "WorldConverter.h"

const float LIGHT_COLORCHANGE_POS_MOD = 0.1f;

extern bool RequiresNvidiaTiledShadowFaceFallback;

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

static const zCVob* GetOriginVob( VobLightInfo* info ) {
    if (Engine::GAPI->GetRendererState().RendererSettings.AllowSelfShadowingPointlights) {
        return nullptr;
    }
    if ( !info->IsPFXVobLight ) {
        thread_local std::unordered_set<const zCVob*> seen{};
        seen.clear();

        zCVob* vob = info->Vob;
        while ( vob ) {
            if (!seen.emplace(vob).second) {
                break;
            }
            if ( auto visFx = vob->As<oCVisualFX>() ) {
                if ( auto origin = visFx->GetOrigin(); origin && origin->As<oCItem>() ) {
                    return origin;
                }
            } else if ( vob->As<oCItem>() ) {
                return vob;
            }
            vob = vob->GetVobParent();
        }
    }
    return nullptr;
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

    StartReInit();

    DrawnOnce = false;
}

D3D11PointLight::~D3D11PointLight() {
    ClearTiledSlot();
    ReleaseShadowMap();
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

void D3D11PointLight::SetTiledSlot( int slot, RenderToDepthStencilBuffer* target, D3D11TiledDeferredShading* owner, bool lowRes ) {
    m_TiledSlotIndex = slot;
    m_TiledSlotLowRes = lowRes;
    m_TiledDepthTarget = target;
    m_TiledOwner = owner;

    StartReInit();
    DrawnOnce = false;
    m_StaticShadowReady = false;
    m_HasDynamicOverlay = false;
}

void D3D11PointLight::ClearTiledSlot() {
    if ( m_TiledSlotIndex >= 0 && m_TiledOwner ) {
        if ( m_TiledSlotLowRes ) {
            m_TiledOwner->FreeStaticSlot( m_TiledSlotIndex );
        } else {
            m_TiledOwner->FreeSlot( m_TiledSlotIndex );
        }
    }
    m_TiledSlotIndex = -1;
    m_TiledSlotLowRes = false;
    m_TiledDepthTarget = nullptr;
    m_TiledOwner = nullptr;
    m_StaticShadowReady = false;
    m_HasDynamicOverlay = false;
}

int D3D11PointLight::GetCurrentShadowMode() const {
    auto mode = static_cast<int>(Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows);
    // Only PLS_UPDATE_DYNAMIC downgrades a static-flagged light to PLS_STATIC_ONLY; PLS_FULL must stay FULL
    // (it's the no-caching-shortcuts escape hatch, and downgrading it would make it never re-render).
    if ( mode == GothicRendererSettings::PLS_UPDATE_DYNAMIC ) {
        if ( LightInfo->IsStaticVobLight ) {
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
    auto context = AsD3D11Engine(Engine::GraphicsEngine)->GetContext();
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
    D3D11GraphicsEngine* engine = AsD3D11Engine(Engine::GraphicsEngine);
    const float range = LightInfo->Vob->GetLightRange();
    
    // PFX-driven lights (candles/torches/campfires - oCVisualFX-owned rather than a static level light) can
    // be parented anywhere in the vob tree, including onto NPCs/the player. GetOriginVob's self-exclusion
    // below only walks the oCItem-origin chain, so a PFX light whose origin ISN'T an item has no reliable way
    // to exclude its own carrier from the caster set - that's what used to make e.g. a belt-mounted light
    // draw a huge shadow from the player all around (see SetupVobsToExclude's comment). Restricting these to
    // world-mesh-only casters sidesteps that class of bug entirely instead of needing a more general fix, at
    // the cost of PFX lights never getting VOB/NPC shadows.
    const unsigned int staticCasterMask = (LightInfo->IsPFXVobLight || LightInfo->IsStaticVobLight) 
        ? SHADOW_CASTER_WORLD
        : SHADOW_CASTER_WORLD | SHADOW_CASTER_VOBS | SHADOW_CASTER_MOBS;

    const bool excludeSelf = GetOriginVob( LightInfo ) != nullptr;
    if ( excludeSelf ) {
        SetupVobsToExclude( LightInfo );
    }

    if ( RequiresNvidiaTiledShadowFaceFallback && IsTiledArrayTarget( target ) ) {
        RenderShadowCubeFacePasses( target, clearDepth, staticCasterMask, &VobCache, &SkeletalVobCache, &WorldMeshCache,
            excludeSelf ? &excludeVobsToExclude : nullptr );
    } else if ( excludeSelf ) {
        engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), range, target, nullptr, nullptr, false, LightInfo->IsIndoorVob, false,
            &VobCache, &SkeletalVobCache, &WorldMeshCache, clearDepth, staticCasterMask, excludeVobsToExclude );
    } else {
        engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), range, target, nullptr, nullptr, false, LightInfo->IsIndoorVob, false,
            &VobCache, &SkeletalVobCache, &WorldMeshCache, clearDepth, staticCasterMask );
    }

    if ( excludeSelf ) {
        vobsToExclude.clear();
    }
}

void D3D11PointLight::RenderAnimatedShadowPass( RenderToDepthStencilBuffer& target, bool clearDepth ) {
    D3D11GraphicsEngine* engine = AsD3D11Engine(Engine::GraphicsEngine);
    const float range = LightInfo->Vob->GetLightRange();

    const unsigned int animatedCasterMask = SHADOW_CASTER_ANIMATED;

    const bool excludeSelf = GetOriginVob( LightInfo ) != nullptr;
    if ( excludeSelf ) {
        SetupVobsToExclude( LightInfo );
    }

    if ( RequiresNvidiaTiledShadowFaceFallback && IsTiledArrayTarget( target ) ) {
        RenderShadowCubeFacePasses( target, clearDepth, animatedCasterMask, nullptr, nullptr, nullptr,
            excludeSelf ? &excludeVobsToExclude : nullptr );
    } else if ( excludeSelf ) {
        engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), range, target, nullptr, nullptr, false, LightInfo->IsIndoorVob, false,
            nullptr, nullptr, nullptr, clearDepth, animatedCasterMask, excludeVobsToExclude );
    } else {
        engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), range, target, nullptr, nullptr, false, LightInfo->IsIndoorVob, false,
            nullptr, nullptr, nullptr, clearDepth, animatedCasterMask );
    }

    if ( excludeSelf ) {
        vobsToExclude.clear();
    }
}

/** Returns true if this is the first time that light is being rendered */
bool D3D11PointLight::NotYetDrawn() {
    return !DrawnOnce;
}

/** Initializes the resources of this light */
void D3D11PointLight::InitResources() {
    InitDone = false;
    if ( !LightInfo || !LightInfo->Vob ) {
        // Light got removed before we could init, just return
        InitDone = true;
        return;
    }
    InitDone = true;
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

    const bool moved = !PositionEqualEps( LastUpdatePosition, LightInfo->Vob->GetPositionWorld() );

    if ( shadowMode == GothicRendererSettings::PLS_STATIC_ONLY ) {
        return moved || !m_StaticShadowReady || NotYetDrawn();
    }

    if ( shadowMode == GothicRendererSettings::PLS_UPDATE_DYNAMIC ) {
        return moved || !m_StaticShadowReady || NotYetDrawn();
    }

    if ( shadowMode == GothicRendererSettings::PLS_FULL ) {
        // PLS_FULL always re-renders, even for a stationary light, since it never latches DrawnOnce.
        return true;
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
    D3D11GraphicsEngine* engine = AsD3D11Engine( Engine::GraphicsEngine ); // TODO: Remove and use newer system!

    XMFLOAT3 vobPos = LightInfo->Vob->GetPositionWorld();
    const bool moved = !PositionEqualEps( LastUpdatePosition, vobPos );

    if ( moved ) {
        // Position changed, refresh our caches
        VobCache.clear();
        SkeletalVobCache.clear();

        // Invalidate worldcache
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
    float zFar = LightInfo->Vob->GetLightRange() * 2.0f;
    m_DebugLastZNear = zNear;
    m_DebugLastZFar = zFar;

    XMMATRIX proj = XMMatrixPerspectiveFovLH( XM_PIDIV2, 1.0f, zNear, zFar );
    proj = XMMatrixTranspose( proj );
    XMStoreFloat4x4( &CubeMapProjMatrix, proj );

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
    // Was declared but never actually assigned anywhere in the codebase - always read back (0,0,0)
    // regardless of whether a render actually happened, so it was not trustworthy debug signal.
    LightInfo->LastRenderedPosition = vobPos;
    DrawnOnce = true;
}

/** Renders all cubemap faces at once, using the geometry shader */
void D3D11PointLight::RenderFullCubemap() {
    if ( !IsReady() )
        return;
    D3D11GraphicsEngine* engine = AsD3D11Engine( Engine::GraphicsEngine ); // TODO: Remove and use newer system!
    auto _ = engine->RecordGraphicsEvent( GE_NAME( "RenderFullCubemap->RenderFullCubemap" ) );

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

                // PFX lights are restricted to world-mesh casters only (see RenderStaticShadowPass) - there
                // is nothing animated to put in the overlay, and this shared array slot may still hold a
                // DIFFERENT light's stale movers from whichever light last held it. Leave m_HasDynamicOverlay
                // false so the shader never composites against that leftover data.
                if ( !LightInfo->IsPFXVobLight ) {
                    // Clear: the overlay must hold ONLY this update's movers, never the previous one's.
                    RenderAnimatedShadowPass( *dynTarget, true );
                    m_HasDynamicOverlay = true;
                }
                return;
            }
            // Overlay array unavailable - fall through to the composited single-cube path below.
        }

        DepthStencilPool* dsPool = engine->GetPfxRenderer()->GetDepthStencilPool();
        AcquireStaticAsideShadowMap( dsPool, m_CurrentResolution );

        if ( !m_StaticShadowReady ) {
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

        // See the PLS_UPDATE_DYNAMIC/tiled branch above: PFX lights have no animated casters to add.
        if ( !LightInfo->IsPFXVobLight ) {
            RenderAnimatedShadowPass( *activeTarget, false );
        }
        return;
    }

    if ( shadowMode == GothicRendererSettings::PLS_FULL ) {
        ReleaseStaticAsideShadowMap();
        m_StaticShadowReady = false;
        m_HasDynamicOverlay = false;

        // FULL never reuses the world-mesh candidate cache - it always re-collects the whole scene fresh.
        std::vector<MeshDrawRange>* wc = nullptr;

        // Keep RenderStaticShadowPass's world-only restriction for PFX/static lights.
        const unsigned int casterMask = (LightInfo->IsPFXVobLight || LightInfo->IsStaticVobLight) ? SHADOW_CASTER_WORLD : SHADOW_CASTER_ALL;

        const bool excludeSelf = GetOriginVob( LightInfo ) != nullptr;
        if ( excludeSelf ) {
            SetupVobsToExclude( LightInfo );
        }

        if ( RequiresNvidiaTiledShadowFaceFallback && IsTiledArrayTarget( *activeTarget ) ) {
            RenderShadowCubeFacePasses( *activeTarget, true, casterMask, &VobCache, &SkeletalVobCache, wc,
                excludeSelf ? &excludeVobsToExclude : nullptr );
        } else if ( excludeSelf ) {
            engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), LightInfo->Vob->GetLightRange(), *activeTarget,
                nullptr, nullptr, false, LightInfo->IsIndoorVob, false, &VobCache, &SkeletalVobCache, wc, true, casterMask,
                excludeVobsToExclude);
        } else {
            engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), LightInfo->Vob->GetLightRange(), *activeTarget,
                nullptr, nullptr, false, LightInfo->IsIndoorVob, false, &VobCache, &SkeletalVobCache, wc, true, casterMask );
        }

        if ( excludeSelf ) {
            vobsToExclude.clear();
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
    WorldMeshCache.clear();
}

void D3D11PointLight::StartReInit() {
    InitResources();
}

bool D3D11PointLight::IsTiledArrayTarget( const RenderToDepthStencilBuffer& target ) const {
    if ( m_TiledSlotIndex < 0 || !m_TiledOwner ) {
        return false;
    }
    if ( &target == m_TiledDepthTarget ) {
        return true;
    }
    return &target == m_TiledOwner->GetDynSlotTarget( m_TiledSlotIndex );
}

void D3D11PointLight::RenderShadowCubeFacePasses(
    RenderToDepthStencilBuffer& target, bool clearDepth, unsigned int casterMask,
    std::list<VobInfo*>* renderedVobs, std::list<SkeletalVobInfo*>* renderedMobs,
    std::vector<MeshDrawRange>* worldMeshCache,
    const std::move_only_function<bool(const zCVob*) const>* ignoreVob ) {

    D3D11GraphicsEngine* engine = AsD3D11Engine(Engine::GraphicsEngine);
    auto _ = engine->RecordGraphicsEvent( GE_NAME( "RenderFullCubemap->RenderShadowCubeFacePasses" ) );

    const auto lightPos = LightInfo->Vob->GetPositionWorldXM();
    const float range = LightInfo->Vob->GetLightRange();
    Frustum f;
    f.BuildCubemapFace( lightPos, range, 0 ); // cubemap frustum is a sphere. not per face.
    CameraReplacement cr;
    cr.frustum = f;
    XMStoreFloat3( &cr.PositionReplacement, lightPos );
    cr.ProjectionReplacement = CubeMapProjMatrix;

    // No GS bound on this path, so skeletal draws must skip VS_ExSkeletalCube/VS_ExNodeCube (GS-dependent).
    engine->SetCubeFaceFallbackActive( true );

    for ( UINT face = 0; face < 6; ++face ) {
        cr.ViewReplacement = CubeMapViewMatrices[face];
        Engine::GAPI->SetCameraReplacementPtr( &cr );

        const auto& faceDsv = target.GetDSVCubemapFace( face );
        if ( ignoreVob ) {
            engine->RenderShadowCube( lightPos, range, target, faceDsv, nullptr, false, LightInfo->IsIndoorVob, false,
                renderedVobs, renderedMobs, worldMeshCache, clearDepth, casterMask, *ignoreVob );
        } else {
            engine->RenderShadowCube( lightPos, range, target, faceDsv, nullptr, false, LightInfo->IsIndoorVob, false,
                renderedVobs, renderedMobs, worldMeshCache, clearDepth, casterMask );
        }
    }

    engine->SetCubeFaceFallbackActive( false );
    Engine::GAPI->SetCameraReplacementPtr( nullptr );
}

/** Binds the shadowmap to the pixelshader */
void D3D11PointLight::OnRenderLight() {
    if ( !IsReady() || !m_DepthCubemap)
        return;

    m_DepthCubemap->BindToPixelShader( AsD3D11Engine(Engine::GraphicsEngine)->GetContext(), 3 );
}

/** Called when a vob got removed from the world */
void D3D11PointLight::OnVobRemovedFromWorld( BaseVobInfo* vob ) {
    // Wait for cache initialization to finish first
    //Engine::GAPI->EnterResourceCriticalSection();

    // See if we have this vob registered
    if ( std::ranges::contains(VobCache, vob )
        || std::ranges::contains(SkeletalVobCache, vob ) ) {
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
