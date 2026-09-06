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

// Whether this light's category is allowed VOB/NPC casters (see RendererSettings.PointlightShadowCasterFlags).
// Lives in the shared selector so both backends route a light the same way; aliased here for readability.
static bool AllowsDynamicCasters( const VobLightInfo* info ) {
    return PointLightSlotSelector::AllowsDynamicCasters( info );
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
    ClearDynSlot();
    ClearStaticSlot();
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
}

void D3D11PointLight::CommitStaticBakeToSlot() {
    if ( !m_SlotSel || m_StaticSlot < 0 ) return;
    m_SlotSel->CommitStatic( static_cast<uint32_t>( m_StaticSlot ) );
    // The caster set this bake covered, so a vob later removed or moved can be matched against it by
    // pointer. These are the lists the render just used, so this is a copy rather than a second cull.
    auto& baked = m_SlotSel->StaticSlotAt( static_cast<uint32_t>( m_StaticSlot ) ).bakedVobs;
    baked.clear();
    for ( const VobInfo* v : VobCache ) if ( v && v->Vob ) baked.push_back( v->Vob );
    for ( const SkeletalVobInfo* v : SkeletalVobCache ) if ( v && v->Vob ) baked.push_back( v->Vob );
}

void D3D11PointLight::SetStaticSlot( int slot, RenderToDepthStencilBuffer* target, PointLightSlotSelector* sel ) {
    m_StaticSlot = slot;
    m_StaticTarget = target;
    m_SlotSel = sel;

    StartReInit();
    DrawnOnce = false;
    DropStaticBake();   // a new slot holds the previous occupant's depth until we render
}

void D3D11PointLight::ClearStaticSlot() {
    // No free list to give the slot back to: the selector owns who holds what, and this only lets go of the
    // light's end of it.
    DropStaticBake();   // before m_SlotSel goes, so the slot table stays the single invalidation counter
    m_StaticSlot = -1;
    m_StaticTarget = nullptr;
    m_SlotSel = nullptr;
}

void D3D11PointLight::SetDynSlot( int slot, RenderToDepthStencilBuffer* target ) {
    m_DynSlot = slot;
    m_DynTarget = target;
}

void D3D11PointLight::ClearDynSlot() {
    m_DynSlot = -1;
    m_DynTarget = nullptr;
}

bool D3D11PointLight::RestrictsCastersToWorld() const {
    return !AllowsDynamicCasters( LightInfo );
}

int D3D11PointLight::GetCurrentShadowMode() const {
    auto mode = static_cast<int>(Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows);
    // Only PLS_UPDATE_DYNAMIC downgrades a static-flagged light to PLS_STATIC_ONLY; PLS_FULL must stay FULL
    // (it's the no-caching-shortcuts escape hatch, and downgrading it would make it never re-render). Skipped
    // when PLSC_STATIC_LIGHTS opts static lights into real VOB/NPC casters - they need the full dynamic-overlay
    // machinery to actually receive one, not just the cheap cached path this downgrade exists for.
    if ( mode == GothicRendererSettings::PLS_UPDATE_DYNAMIC ) {
        if ( LightInfo->IsStaticVobLight && !AllowsDynamicCasters( LightInfo ) ) {
            return GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY;
        }
    }
    return mode;
}

float D3D11PointLight::GetShadowRange() const {
    // Not folded in yet (a light that never reached this frame's candidate sweep) - fall back to the live one.
    if ( m_ShadowRange <= 0.0f ) return LightInfo && LightInfo->Vob ? LightInfo->Vob->GetLightRange() : 0.0f;
    return m_ShadowRange;
}

bool D3D11PointLight::HasAnimatedCastersInRange() const {
    if ( !LightInfo || !LightInfo->Vob ) return false;
    if ( !Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes ) return false;
    // Mirrors the animated pass's per-vob tests, minus the indoor/outdoor one - being wrong there only costs
    // one empty overlay render.
    const XMVECTOR pos = LightInfo->Vob->GetPositionWorldXM();
    const float range = GetShadowRange();
    const XMVECTOR rangeSq = XMVectorReplicate( range * range );
    for ( const SkeletalVobInfo* vob : Engine::GAPI->GetAnimatedSkeletalMeshVobs() ) {
        if ( !vob || !vob->VisualInfo || !vob->Vob ) continue;
        if ( vob->Vob->GetVisualAlpha() && vob->Vob->GetVobTransparency() < 0.7f ) continue;   // ghosts
        if ( XMVector3Greater( XMVector3LengthSq( pos - vob->Vob->GetPositionWorldXM() ), rangeSq ) ) continue;
        return true;
    }
    return false;
}

void D3D11PointLight::HandleShadowModeChange( int shadowMode ) {
    if ( m_LastShadowMode == shadowMode ) {
        return;
    }

    m_LastShadowMode = shadowMode;
    DropStaticBake( PLR_MODE_CHANGED );
    DrawnOnce = false;

    if ( shadowMode != GothicRendererSettings::PLS_UPDATE_DYNAMIC ) {
        ReleaseStaticAsideShadowMap();
    }
}

RenderToDepthStencilBuffer* D3D11PointLight::GetActiveShadowTarget() const {
    // Legacy path only: the tiled path renders its two tiers into m_StaticTarget / m_DynTarget directly.
    return m_DepthCubemap ? m_DepthCubemap.get() : nullptr;
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
    DropStaticBake( PLR_ASIDE_BUFFER );
}

void D3D11PointLight::ReleaseStaticAsideShadowMap() {
    m_StaticDepthCubemap.reset();
    DropStaticBake( PLR_ASIDE_BUFFER );
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

    for ( UINT face = 0; face < 6; ++face ) {
        const UINT srcSubresource = D3D11CalcSubresource( 0, face, 1 );
        const UINT dstSubresource = D3D11CalcSubresource( 0, face, 1 );
        context->CopySubresourceRegion( dstTexture, dstSubresource, 0, 0, 0, srcTexture, srcSubresource, nullptr );
    }
}

void D3D11PointLight::RenderStaticShadowPass( RenderToDepthStencilBuffer& target, bool clearDepth ) {
    D3D11GraphicsEngine* engine = AsD3D11Engine(Engine::GraphicsEngine);
    const float range = GetShadowRange();
    
    // PFX-driven lights (candles/torches/campfires - oCVisualFX-owned rather than a static level light) can
    // be parented anywhere in the vob tree, including onto NPCs/the player. GetOriginVob's self-exclusion
    // below only walks the oCItem-origin chain, so a PFX light whose origin ISN'T an item has no reliable way
    // to exclude its own carrier from the caster set - that's what used to make e.g. a belt-mounted light
    // draw a huge shadow from the player all around (see SetupVobsToExclude's comment). Restricting a light's
    // category to world-mesh-only casters (see AllowsDynamicCasters/PointlightShadowCasterFlags) sidesteps
    // that class of bug for PFX lights entirely instead of needing a more general fix, and separately keeps
    // static lights off the expensive VOB/MOB caster set by default.
    const bool restrictToWorld = !AllowsDynamicCasters( LightInfo );
    const unsigned int staticCasterMask = restrictToWorld
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
    const float range = GetShadowRange();

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

/** The 6 face view matrices and the shared projection, bound for the layered VS / cubemap GS. The CB comes
    from the per-frame ring pool, so nothing here may be held across frames. */
void D3D11PointLight::BeginCubeRender( const XMFLOAT3& vobPos ) {
    D3D11GraphicsEngine* engine = AsD3D11Engine( Engine::GraphicsEngine );

    const XMVECTOR vEyePt = XMLoadFloat3( &vobPos );
    const XMVECTOR c_XM_Right = XMVectorSet( 1.f, 0.f, 0.f, 0.f );
    const XMVECTOR c_XM_Left = XMVectorSet( -1.f, 0.f, 0.f, 0.f );
    const XMVECTOR c_XM_Up = XMVectorSet( 0.f, 1.f, 0.f, 0.f );
    const XMVECTOR c_XM_Down = XMVectorSet( 0.f, -1.f, 0.f, 0.f );
    const XMVECTOR c_XM_Forward = XMVectorSet( 0.f, 0.f, 1.f, 0.f );
    const XMVECTOR c_XM_Backward = XMVectorSet( 0.f, 0.f, -1.f, 0.f );

    // Update indoor/outdoor-state
    LightInfo->IsIndoorVob = LightInfo->Vob->IsIndoorVob();

    XMVECTOR vLookDir;
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

    float zNear = 15.0f;
    float zFar = GetShadowRange() * 2.0f;
    m_DebugLastZNear = zNear;
    m_DebugLastZFar = zFar;

    XMMATRIX proj = XMMatrixPerspectiveFovLH( XM_PIDIV2, 1.0f, zNear, zFar );
    proj = XMMatrixTranspose( proj );
    XMStoreFloat4x4( &CubeMapProjMatrix, proj );

    // The cube keeps the natural hyperbolic z of this projection, which PLS_PrepareShadowSampling
    // reconstructs from the same zNear/zFar - see Shaders/include/PointLightShadows.h.
    Engine::GAPI->GetRendererState().GraphicsState.FF_zNear = zNear;
    Engine::GAPI->GetRendererState().GraphicsState.FF_zFar = zFar;
    Engine::GAPI->GetRendererState().GraphicsState.SetGraphicsSwitch( GSWITCH_CUBE_SHADOW, true );

    m_SavedDepthClip = Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable;
    Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable = true;

    CubemapGSConstantBuffer gcb;
    for ( int i = 0; i < 6; i++ ) {
        gcb.PCR_View[i] = CubeMapViewMatrices[i];
        XMStoreFloat4x4( &gcb.PCR_ViewProj[i], proj * XMLoadFloat4x4( &CubeMapViewMatrices[i] ) );
    }

    // Allocate the cubemap view-matrices CB from the per-frame dynamic ring pool
    ConstantBufferAllocation viewMatricesCB = engine->AllocateDynamicCB( &gcb, sizeof( gcb ) );
    engine->BindDynamicCBToVertexShader( 3, viewMatricesCB ); // Layered vertex shader
    engine->BindDynamicCBToGeometryShader( 2, viewMatricesCB ); // Cubemap geometry shader
}

void D3D11PointLight::EndCubeRender( const XMFLOAT3& vobPos ) {
    Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable = m_SavedDepthClip;
    Engine::GAPI->GetRendererState().GraphicsState.SetGraphicsSwitch( GSWITCH_CUBE_SHADOW, false );

    LastUpdateColor = LightInfo->Vob->GetLightColor();
    LastUpdatePosition = vobPos;
    LightInfo->LastRenderedPosition = vobPos;
    DrawnOnce = true;
}

/** The tiled path's per-frame entry point. Everything about WHETHER to render was decided by
    PointLightSlotSelector, so this only asks whether it has somewhere to render to. */
void D3D11PointLight::RenderTiledShadow( bool renderStatic, bool renderDynamic ) {
    if ( !IsReady() ) return;
    // A world-mesh-only light has no movers to put in an overlay.
    if ( !AllowsDynamicCasters( LightInfo ) ) renderDynamic = false;
    if ( !m_StaticTarget ) renderStatic = false;
    if ( !m_DynTarget ) renderDynamic = false;
    if ( !renderStatic && !renderDynamic ) return;

    // Latches the global mode transition (which drops the cached bake) before anything renders.
    HandleShadowModeChange( GetCurrentShadowMode() );

    const XMFLOAT3 vobPos = LightInfo->Vob->GetPositionWorld();
    if ( !PositionEqualEps( LastUpdatePosition, vobPos ) ) {
        // Position changed, refresh our caches
        VobCache.clear();
        SkeletalVobCache.clear();
        DropStaticBake( PLR_LIGHT_MOVED );
    }

    auto _ = AsD3D11Engine( Engine::GraphicsEngine )->RecordGraphicsEvent( GE_NAME( "RenderTiledShadow" ) );
    BeginCubeRender( vobPos );

    if ( renderStatic ) {
        // Clear: the cube holds this bake alone, and the slot may have been someone else's.
        RenderStaticShadowPass( *m_StaticTarget, true );
        m_StaticShadowReady = true;
        CommitStaticBakeToSlot();
    }
    if ( renderDynamic ) {
        // Nothing to draw means not even a clear: an un-drawn overlay is not sampled at all (DynSlot::present).
        const bool hasCasters = HasAnimatedCastersInRange();
        if ( hasCasters ) RenderAnimatedShadowPass( *m_DynTarget, true );
        if ( m_SlotSel && m_DynSlot >= 0 ) m_SlotSel->CommitDynamic( static_cast<uint32_t>( m_DynSlot ), hasCasters );
    }

    EndCubeRender( vobPos );
}

/** Draws the surrounding scene into the LEGACY per-light cubemap (tiled lighting off) */
void D3D11PointLight::RenderCubemap( bool forceUpdate ) {
    if ( !IsReady() )
        return;
    if ( !m_DepthCubemap )
        return;

    const int shadowMode = GetCurrentShadowMode();
    HandleShadowModeChange( shadowMode );

    XMFLOAT3 vobPos = LightInfo->Vob->GetPositionWorld();
    const bool moved = !PositionEqualEps( LastUpdatePosition, vobPos );

    if ( moved ) {
        // Position changed, refresh our caches
        VobCache.clear();
        SkeletalVobCache.clear();

        // Invalidate worldcache
        DropStaticBake( PLR_LIGHT_MOVED );
    }

    if ( shadowMode == GothicRendererSettings::PLS_STATIC_ONLY && !moved && m_StaticShadowReady && DrawnOnce ) {
        return;
    }

    if ( !NeedsUpdate() && !WantsUpdate() ) {
        if ( !forceUpdate )
            return; // Don't update when we don't need to
    }

    BeginCubeRender( vobPos );
    RenderFullCubemap();
    EndCubeRender( vobPos );
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
    // See RenderStaticShadowPass's comment: a light whose category isn't opted into
    // PointlightShadowCasterFlags skips animated/VOB casters entirely, staying world-mesh-only.
    const bool restrictToWorld = !AllowsDynamicCasters( LightInfo );
    if ( !m_StaticShadowReady && shadowMode == GothicRendererSettings::PLS_STATIC_ONLY ) {
        RenderStaticShadowPass( *activeTarget, true );
        m_StaticShadowReady = true;
        CommitStaticBakeToSlot();
        return;
    }

    if ( shadowMode == GothicRendererSettings::PLS_UPDATE_DYNAMIC ) {
        DepthStencilPool* dsPool = engine->GetPfxRenderer()->GetDepthStencilPool();
        AcquireStaticAsideShadowMap( dsPool, m_CurrentResolution );

        if ( !m_StaticShadowReady ) {
            if ( m_StaticDepthCubemap ) {
                RenderStaticShadowPass( *m_StaticDepthCubemap, true );
                m_StaticShadowReady = true;
                CommitStaticBakeToSlot();
            } else {
                // No aside buffer, we can't cache the static shadows.
                RenderStaticShadowPass( *activeTarget, true );
            }
        }

        if ( m_StaticDepthCubemap ) {
            CopyStaticAsideToActiveTarget();
        }

        // See the PLS_UPDATE_DYNAMIC/tiled branch above: a world-mesh-only light has no animated casters to add.
        if ( !restrictToWorld ) {
            RenderAnimatedShadowPass( *activeTarget, false );
        }
        return;
    }

    if ( shadowMode == GothicRendererSettings::PLS_FULL ) {
        ReleaseStaticAsideShadowMap();
        DropStaticBake( PLR_NO_CACHE );

        // FULL never reuses the world-mesh candidate cache - it always re-collects the whole scene fresh.
        std::vector<MeshDrawRange>* wc = nullptr;

        // Keep RenderStaticShadowPass's world-only restriction.
        const unsigned int casterMask = restrictToWorld ? SHADOW_CASTER_WORLD : SHADOW_CASTER_ALL;

        const bool excludeSelf = GetOriginVob( LightInfo ) != nullptr;
        if ( excludeSelf ) {
            SetupVobsToExclude( LightInfo );
        }

        if ( RequiresNvidiaTiledShadowFaceFallback && IsTiledArrayTarget( *activeTarget ) ) {
            RenderShadowCubeFacePasses( *activeTarget, true, casterMask, &VobCache, &SkeletalVobCache, wc,
                excludeSelf ? &excludeVobsToExclude : nullptr );
        } else if ( excludeSelf ) {
            engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), GetShadowRange(), *activeTarget,
                nullptr, nullptr, false, LightInfo->IsIndoorVob, false, &VobCache, &SkeletalVobCache, wc, true, casterMask,
                excludeVobsToExclude);
        } else {
            engine->RenderShadowCube( LightInfo->Vob->GetPositionWorldXM(), GetShadowRange(), *activeTarget,
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

void D3D11PointLight::DropStaticBake() {
    // Counted only on the legacy path: in the tiled one the slot table is the single counter, and every
    // drop here is downstream of one it has already noted.
    if ( m_StaticShadowReady && !m_SlotSel ) {
        Engine::GAPI->GetRendererState().RendererInfo.NotePointLightRebake( PLR_SLOT_TAKEN );
        m_LastRebakeCause = PLR_SLOT_TAKEN;
    }
    m_StaticShadowReady = false;
}

void D3D11PointLight::DropStaticBake( EPointLightRebakeCause cause ) {
    // Always counted: these are the light's OWN reasons, which the slot table cannot see - it still reads
    // this slot's depth as valid.
    if ( m_StaticShadowReady ) {
        Engine::GAPI->GetRendererState().RendererInfo.NotePointLightRebake( cause );
        m_LastRebakeCause = cause;
    }
    m_StaticShadowReady = false;
}

void D3D11PointLight::Invalidate() {
    DrawnOnce = false;
    DropStaticBake();
    VobCache.clear();
    SkeletalVobCache.clear();
    WorldMeshCache.clear();
}

void D3D11PointLight::StartReInit() {
    InitResources();
}

bool D3D11PointLight::IsTiledArrayTarget( const RenderToDepthStencilBuffer& target ) const {
    return ( m_StaticTarget && &target == m_StaticTarget ) || ( m_DynTarget && &target == m_DynTarget );
}

void D3D11PointLight::RenderShadowCubeFacePasses(
    RenderToDepthStencilBuffer& target, bool clearDepth, unsigned int casterMask,
    std::list<VobInfo*>* renderedVobs, std::list<SkeletalVobInfo*>* renderedMobs,
    std::vector<MeshDrawRange>* worldMeshCache,
    const std::move_only_function<bool(const zCVob*) const>* ignoreVob ) {

    D3D11GraphicsEngine* engine = AsD3D11Engine(Engine::GraphicsEngine);
    auto _ = engine->RecordGraphicsEvent( GE_NAME( "RenderFullCubemap->RenderShadowCubeFacePasses" ) );

    const auto lightPos = LightInfo->Vob->GetPositionWorldXM();
    const float range = GetShadowRange();
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
        DropStaticBake( PLR_VOB_REMOVED );
    }

    if (vob->Vob == LightInfo->Vob) {
        // Our light got removed, release shadowmap
        ReleaseShadowMap();
        ClearDynSlot();
        ClearStaticSlot();
    }

    //Engine::GAPI->LeaveResourceCriticalSection();
}
