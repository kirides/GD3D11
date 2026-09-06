#include "../pch.h"
#include "TiledCubeArrayTechnique.h"

#include "../D3D11GraphicsEngine.h"
#include "../D3D11PointLight.h"
#include "../D3D11TiledDeferredShading.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../RenderToTextureBuffer.h"
#include "../WorldObjects.h"
#include "../zCVobLight.h"
#include "PointShadowCasters.h"
#include "PointShadowPolicy.h"

using PointShadowCasters::CasterPass;
using PointShadowCasters::CubeRenderScope;

// ---- TiledCubeLightState --------------------------------------------------------------------------------

TiledCubeLightState::~TiledCubeLightState() {
    ReleaseResources();
}

void TiledCubeLightState::ReleaseResources() {
    ClearDynSlot();
    ClearStaticSlot();
}

ID3D11Texture2D* TiledCubeLightState::GetStaticCubeTexture() const {
    return m_StaticTarget ? m_StaticTarget->GetTexture().Get() : nullptr;
}

void TiledCubeLightState::SetStaticSlot( int slot, RenderToDepthStencilBuffer* target, PointLightSlotSelector* sel ) {
    m_StaticSlot = slot;
    m_StaticTarget = target;
    m_SlotSel = sel;

    m_Light.InitResources();
    m_Light.MarkNotDrawn();
    m_Light.DropStaticBake( PLR_SLOT_TAKEN );   // a new slot holds the previous occupant's depth until we render
}

void TiledCubeLightState::ClearStaticSlot() {
    // No free list to give the slot back to: the selector owns who holds what, and this only lets go of the
    // light's end of it. Drop the bake before m_SlotSel goes, so the slot table stays the single counter.
    m_Light.DropStaticBake( PLR_SLOT_TAKEN );
    m_StaticSlot = -1;
    m_StaticTarget = nullptr;
    m_SlotSel = nullptr;
}

void TiledCubeLightState::SetDynSlot( int slot, RenderToDepthStencilBuffer* target ) {
    m_DynSlot = slot;
    m_DynTarget = target;
}

void TiledCubeLightState::ClearDynSlot() {
    m_DynSlot = -1;
    m_DynTarget = nullptr;
}

void TiledCubeLightState::CommitStaticBakeToSlot() {
    if ( !m_SlotSel || m_StaticSlot < 0 ) return;
    m_SlotSel->CommitStatic( static_cast<uint32_t>( m_StaticSlot ) );
    // The caster set this bake covered, so a vob later removed or moved can be matched against it by
    // pointer. These are the lists the render just used, so this is a copy rather than a second cull.
    auto& baked = m_SlotSel->StaticSlotAt( static_cast<uint32_t>( m_StaticSlot ) ).bakedVobs;
    baked.clear();
    for ( const VobInfo* v : m_Light.VobCache ) if ( v && v->Vob ) baked.push_back( v->Vob );
    for ( const SkeletalVobInfo* v : m_Light.SkeletalVobCache ) if ( v && v->Vob ) baked.push_back( v->Vob );
}

/** Everything about WHETHER to render was decided by PointLightSlotSelector, so this only asks whether it
    has somewhere to render to. */
void TiledCubeLightState::Render( bool renderStatic, bool renderDynamic ) {
    if ( !m_Light.IsReady() ) return;
    VobLightInfo* info = m_Light.GetLightInfo();
    // A world-mesh-only light has no movers to put in an overlay.
    if ( m_Light.RestrictsCastersToWorld() ) renderDynamic = false;
    if ( !m_StaticTarget ) renderStatic = false;
    if ( !m_DynTarget ) renderDynamic = false;
    if ( !renderStatic && !renderDynamic ) return;

    // Latches the global mode transition (which drops the cached bake) before anything renders.
    m_Light.HandleShadowModeChange( PointShadowPolicy::CurrentShadowMode( info ) );

    const XMFLOAT3 vobPos = info->Vob->GetPositionWorld();
    if ( !PointShadowPolicy::PositionEqualEps( m_Light.GetLastUpdatePosition(), vobPos ) ) {
        // Position changed, refresh our caches
        m_Light.ClearCasterCaches();
        m_Light.DropStaticBake( PLR_LIGHT_MOVED );
    }

    auto _ = AsD3D11Engine( Engine::GraphicsEngine )->RecordGraphicsEvent( GE_NAME( "RenderTiledShadow" ) );
    CubeRenderScope scope( info, m_Light.GetShadowRange() );
    m_Light.NoteDebugCubePlanes( scope.ZNear(), scope.ZFar() );

    CasterPass pass;
    pass.Light = info;
    pass.Range = m_Light.GetShadowRange();
    pass.TargetIsSharedArray = true;   // both tiers are windows into a shared array - see the NVIDIA fallback
    pass.ClearDepth = true;

    if ( renderStatic ) {
        // Clear: the cube holds this bake alone, and the slot may have been someone else's.
        CasterPass staticPass = pass;
        staticPass.Target = m_StaticTarget;
        staticPass.VobCache = &m_Light.VobCache;
        staticPass.MobCache = &m_Light.SkeletalVobCache;
        staticPass.WorldMeshCache = &m_Light.WorldMeshCache;
        PointShadowCasters::RenderStatic( scope, staticPass );
        m_Light.MarkStaticBakeReady();
        CommitStaticBakeToSlot();
    }
    if ( renderDynamic ) {
        // Nothing to draw means not even a clear: an un-drawn overlay is not sampled at all (DynSlot::present).
        const bool hasCasters = m_Light.HasAnimatedCastersInRange();
        if ( hasCasters ) {
            CasterPass dynPass = pass;
            dynPass.Target = m_DynTarget;
            PointShadowCasters::RenderAnimated( scope, dynPass );
        }
        if ( m_SlotSel && m_DynSlot >= 0 ) m_SlotSel->CommitDynamic( static_cast<uint32_t>( m_DynSlot ), hasCasters );
    }

    m_Light.NoteRendered( vobPos );
}

// ---- TiledCubeArrayTechnique ----------------------------------------------------------------------------

TiledCubeArrayTechnique::TiledCubeArrayTechnique( D3D11TiledDeferredShading& tiled, PointLightSlotSelector& slots )
    : m_Tiled( tiled ), m_Slots( slots ) {
    m_Info.UsesSharedArrayTargets = true;
}

XRESULT TiledCubeArrayTechnique::OnActivate() {
    // Idempotent: the pool sizes are compile-time constants, so only the first call actually sizes the tables.
    PointLightSlotSelector::Config cfg;
    cfg.MaxStaticSlots = MAX_STATIC_SHADOW_CUBEMAPS;
    cfg.MaxDynamicSlots = MAX_DYN_SHADOW_CUBEMAPS;
    m_Slots.Configure( cfg );
    return XR_SUCCESS;
}

void TiledCubeArrayTechnique::OnDeactivate() {
    m_Slots.ReleaseAllSlots();
    m_Candidates.clear();
}

std::unique_ptr<IPointShadowLightState> TiledCubeArrayTechnique::CreateLightState( D3D11PointLight& light ) {
    return std::make_unique<TiledCubeLightState>( light );
}

bool TiledCubeArrayTechnique::ProvidesShadowFor( const D3D11PointLight& light ) const {
    TiledCubeLightState* state = light.AsTiled();
    return state && state->HasAnyShadowMap();
}

int32_t TiledCubeArrayTechnique::EncodeShadowIndexFor( const VobLightInfo& light ) const {
    return m_Slots.GetEncodedIndex( reinterpret_cast<uint64_t>( light.Vob ) );
}

void TiledCubeArrayTechnique::OnLightVobRemoved( const zCVob* lightVob ) {
    m_Slots.ReleaseFor( reinterpret_cast<uint64_t>( lightVob ) );
}

void TiledCubeArrayTechnique::ReconcileSlots() {
    // The WHOLE light map, not just this frame's candidates: a light whose slot was handed to somebody else
    // must let go of its end of it, and it can be anywhere at all when that happens.
    for ( auto& it : Engine::GAPI->VobLightMap ) {
        VobLightInfo* light = it.second;
        if ( !light || !light->Vob ) continue;
        D3D11PointLight* pl = light->LightShadowBuffers
            ? dynamic_cast<D3D11PointLight*>( light->LightShadowBuffers.get() ) : nullptr;
        if ( !pl ) continue;
        pl->EnsureState();
        TiledCubeLightState* state = pl->AsTiled();
        if ( !state ) continue;

        const uint64_t key = reinterpret_cast<uint64_t>( light->Vob );
        const int staticSlot = m_Slots.FindStaticSlotOf( key );
        if ( staticSlot < 0 ) {
            state->ClearDynSlot();
            state->ClearStaticSlot();
            continue;
        }

        if ( state->GetStaticSlot() != staticSlot ) {
            RenderToDepthStencilBuffer* target = m_Tiled.ClaimStaticSlot( staticSlot );
            if ( !target ) {
                // The array could not be created - give the slot back rather than hold it out of the pool.
                m_Slots.ReleaseFor( key );
                state->ClearDynSlot();
                state->ClearStaticSlot();
                continue;
            }
            state->ClearStaticSlot();
            state->SetStaticSlot( staticSlot, target, &m_Slots );
            pl->SetCurrentResolution( STATIC_SHADOW_CUBE_SIZE );
        }

        const int dynSlot = m_Slots.FindDynSlotOf( key );
        if ( dynSlot < 0 ) {
            state->ClearDynSlot();
        } else if ( state->GetDynSlot() != dynSlot ) {
            if ( RenderToDepthStencilBuffer* dynTarget = m_Tiled.ClaimDynSlot( dynSlot ) ) {
                state->SetDynSlot( dynSlot, dynTarget );
            } else {
                // No overlay array (creation declined): the light keeps its static cube and nothing else.
                m_Slots.ReleaseDynamicFor( key );
                state->ClearDynSlot();
            }
        }
    }
}

XRESULT TiledCubeArrayTechnique::DrawShadows( std::vector<VobLightInfo*>& lights ) {
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    if ( settings.EnablePointlightShadows <= 0 ) {
        // Re-enabling mid-session must re-render from scratch, not sample however stale depth is left.
        m_Slots.Select( {}, GothicRendererSettings::PLS_DISABLED );
        ReconcileSlots();
        return XR_SUCCESS;
    }

    auto graphicsEngine = AsD3D11Engine( Engine::GraphicsEngine );
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawPointlightShadows" ) );
    graphicsEngine->SetDefaultStates();

    // Distance sweep of every registered light against the domes - deliberately NOT this frame's visible set,
    // so turning away from a light cannot cost it its cube.
    m_Slots.BuildCandidates( m_Candidates );
    m_Slots.Select( m_Candidates, settings.EnablePointlightShadows );

    // Every slot owner needs a renderer object and the right two targets before anything renders.
    for ( const PointLightSlotSelector::Assignment& a : m_Slots.GetAssignments() ) {
        VobLightInfo* light = a.light;
        if ( !light || !light->Vob ) continue;
        if ( !light->LightShadowBuffers ) {
            BaseShadowedPointLight* bpl = nullptr;
            graphicsEngine->CreateShadowedPointLight( &bpl, light, /*dynamic light*/ true );
            light->LightShadowBuffers.reset( bpl );
        }
        // The quantized, grow-only cube range the selector decided this bake with, folded in before the
        // render so the projection and the shader's depth compare use the same far plane.
        if ( auto* pl = dynamic_cast<D3D11PointLight*>( light->LightShadowBuffers.get() ) )
            pl->SetShadowRange( a.range );
    }
    ReconcileSlots();

    // Render exactly what the frame budget granted; the assignments are in candidate (nearest-first) order.
    // Nothing here decides anything - see PointLightSlotSelector::Select.
    for ( const PointLightSlotSelector::Assignment& a : m_Slots.GetAssignments() ) {
        VobLightInfo* light = a.light;
        if ( !light || !light->LightShadowBuffers ) continue;
        light->UpdateShadows = false;   // the selector is the only scheduler now; drop Gothic's hint
        if ( !a.renderStatic && !a.renderDynamic ) continue;
        auto* pl = static_cast<D3D11PointLight*>( light->LightShadowBuffers.get() );
        TiledCubeLightState* state = pl->AsTiled();
        if ( !state || state->GetStaticSlot() < 0 ) continue;   // ReconcileSlots claimed no target for it
        state->Render( a.renderStatic, a.renderDynamic );
        graphicsEngine->DebugPointlight = pl;
    }

    return XR_SUCCESS;
}
