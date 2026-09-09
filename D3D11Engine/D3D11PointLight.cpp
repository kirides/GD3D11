#include "pch.h"
#include "D3D11PointLight.h"
#include "D3D11GraphicsEngine.h"
#include "Engine.h"
#include "PointShadow/LegacyCubeTechnique.h"
#include "PointShadow/PointShadowCasters.h"
#include "PointShadow/PointShadowPolicy.h"
#include "PointShadow/TiledCubeArrayTechnique.h"
#include "WorldConverter.h"
#include "zCVobLight.h"

namespace {
    IPointShadowTechnique* ActiveTechnique() {
        auto* engine = AsD3D11Engine( Engine::GraphicsEngine );
        if ( !engine ) return nullptr;
        D3D11ShadowMap* shadowMaps = engine->GetShadowMaps();
        return shadowMaps ? shadowMaps->GetPointShadowTechnique() : nullptr;
    }
}

D3D11PointLight::D3D11PointLight( VobLightInfo* info, bool dynamicLight ) {
    LightInfo = info;
    DynamicLight = dynamicLight;

    // Ensure this light is actually in the VobLightMap
    // some lights don't seem to be in here!
    Engine::GAPI->VobLightMap[info->Vob] = info;

    LastUpdatePosition = LightInfo->Vob->GetPositionWorld();

    StartReInit();
    EnsureState();

    DrawnOnce = false;
}

D3D11PointLight::~D3D11PointLight() {
    DropState();
}

void D3D11PointLight::EnsureState() {
    IPointShadowTechnique* technique = ActiveTechnique();
    if ( !technique ) return;   // created before the backend picked one - attaches on first use instead
    if ( m_State && m_State->Technique() == technique->Id() ) return;

    DropState();
    m_State = technique->CreateLightState( *this );
}

void D3D11PointLight::DropState() {
    if ( !m_State ) return;
    m_State->ReleaseResources();
    m_State.reset();
    // Nothing sampleable is left, so nothing baked is left either.
    m_StaticShadowReady = false;
    DrawnOnce = false;
    m_CurrentResolution = 0;
}

LegacyCubeLightState* D3D11PointLight::AsLegacy() const {
    if ( !m_State || m_State->Technique() != EPointShadowTechnique::LegacyPerLightCube ) return nullptr;
    return static_cast<LegacyCubeLightState*>( m_State.get() );
}

TiledCubeLightState* D3D11PointLight::AsTiled() const {
    if ( !m_State || m_State->Technique() != EPointShadowTechnique::TiledCubeArray ) return nullptr;
    return static_cast<TiledCubeLightState*>( m_State.get() );
}

bool D3D11PointLight::HasAnyShadowMap() const {
    return m_State && m_State->HasAnyShadowMap();
}

float D3D11PointLight::GetShadowRange() const {
    // Not folded in yet (a light that never reached this frame's candidate sweep) - fall back to the live one.
    if ( m_ShadowRange <= 0.0f ) return LightInfo && LightInfo->Vob ? LightInfo->Vob->GetLightRange() : 0.0f;
    return m_ShadowRange;
}

bool D3D11PointLight::RestrictsCastersToWorld() const {
    return PointShadowCasters::RestrictsCastersToWorld( LightInfo );
}

bool D3D11PointLight::HasAnimatedCastersInRange() const {
    return PointShadowCasters::HasAnimatedCastersInRange( LightInfo, GetShadowRange() );
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

bool D3D11PointLight::IsReady() const {
    return InitDone
        && LightInfo
        && LightInfo->Vob;
}

bool D3D11PointLight::HasMoved() const {
    return !PointShadowPolicy::PositionEqualEps( LastUpdatePosition, LightInfo->Vob->GetPositionWorld() );
}

bool D3D11PointLight::NeedsUpdate() {
    if ( !IsReady() )
        return false;

    PointShadowPolicy::LightUpdateState state;
    state.Moved = HasMoved();
    state.StaticShadowReady = m_StaticShadowReady;
    state.DrawnOnce = DrawnOnce;
    return PointShadowPolicy::NeedsUpdate( PointShadowPolicy::CurrentShadowMode( LightInfo ), m_LastShadowMode, state );
}

bool D3D11PointLight::WantsUpdate() {
    if ( !IsReady() )
        return false;
    return PointShadowPolicy::WantsUpdate( PointShadowPolicy::CurrentShadowMode( LightInfo ), LightInfo, LastUpdateColor );
}

int D3D11PointLight::HandleShadowModeChange( int shadowMode ) {
    if ( m_LastShadowMode == shadowMode ) {
        return shadowMode;
    }

    m_LastShadowMode = shadowMode;
    DropStaticBake( PLR_MODE_CHANGED );
    DrawnOnce = false;
    return shadowMode;
}

void D3D11PointLight::NoteRendered( const XMFLOAT3& vobPos ) {
    LastUpdateColor = LightInfo->Vob->GetLightColor();
    LastUpdatePosition = vobPos;
    LightInfo->LastRenderedPosition = vobPos;
    DrawnOnce = true;
}

void D3D11PointLight::ClearCasterCaches() {
    VobCache.clear();
    SkeletalVobCache.clear();
}

void D3D11PointLight::DropStaticBake( EPointLightRebakeCause cause ) {
    // Counted only where this light is the counter: on the tiled path the slot table has already noted
    // every drop the light can see, and would count it twice.
    if ( m_StaticShadowReady && ( !m_State || m_State->CountsOwnRebakes() ) ) {
        Engine::GAPI->GetRendererState().RendererInfo.NotePointLightRebake( cause );
        m_LastRebakeCause = cause;
    }
    m_StaticShadowReady = false;
}

void D3D11PointLight::Invalidate() {
    DrawnOnce = false;
    DropStaticBake( PLR_SLOT_TAKEN );
    VobCache.clear();
    SkeletalVobCache.clear();
    WorldMeshCache.clear();
}

void D3D11PointLight::StartReInit() {
    InitResources();
}

/** Called when a vob got removed from the world */
void D3D11PointLight::OnVobRemovedFromWorld( BaseVobInfo* vob ) {
    // See if we have this vob registered
    if ( std::ranges::contains( VobCache, vob )
        || std::ranges::contains( SkeletalVobCache, vob ) ) {
        // Clear cache, if so
        VobCache.clear();
        SkeletalVobCache.clear();
        DrawnOnce = false;
        DropStaticBake( PLR_VOB_REMOVED );
    }

    if ( m_State ) m_State->OnVobRemovedFromWorld( vob->Vob );

    if ( vob->Vob == LightInfo->Vob ) {
        // Our light got removed, release everything the technique gave it
        DropState();
    }
}

// ---- Debug forwarders ------------------------------------------------------------------------------------

ID3D11Texture2D* D3D11PointLight::GetShadowCubeTexture() const {
    LegacyCubeLightState* legacy = AsLegacy();
    return legacy ? legacy->GetCubeTexture() : nullptr;
}

ID3D11Texture2D* D3D11PointLight::GetTiledShadowCubeTexture() const {
    TiledCubeLightState* tiled = AsTiled();
    return tiled ? tiled->GetStaticCubeTexture() : nullptr;
}

int D3D11PointLight::GetTiledFaceBaseSlice() const {
    TiledCubeLightState* tiled = AsTiled();
    return tiled && tiled->GetStaticSlot() >= 0 ? tiled->GetStaticSlot() * 6 : -1;
}

int D3D11PointLight::GetStaticSlot() const {
    TiledCubeLightState* tiled = AsTiled();
    return tiled ? tiled->GetStaticSlot() : -1;
}

int D3D11PointLight::GetDynSlot() const {
    TiledCubeLightState* tiled = AsTiled();
    return tiled ? tiled->GetDynSlot() : -1;
}

int D3D11PointLight::GetMissingFrames() const {
    LegacyCubeLightState* legacy = AsLegacy();
    return legacy ? legacy->GetMissingFrames() : 0;
}

const PointLightSlotSelector* D3D11PointLight::GetSlotSelector() const {
    TiledCubeLightState* tiled = AsTiled();
    return tiled ? tiled->GetSlotSelector() : nullptr;
}
