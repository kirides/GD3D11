#include "pch.h"
#include "PointLightSlotSelector.h"

#include <algorithm>
#include <cmath>

#include "Engine.h"
#include "GothicAPI.h"
#include "zCVob.h"
#include "zCVobLight.h"
#include "BspPortalCuller.h"

using namespace DirectX;

namespace {
    // The cube's far plane snaps up to this and never shrinks: DoAnimation re-animates the light range every
    // frame, and any change to it would re-bake all six faces of a flickering torch's cube.
    constexpr float kShadowRangeQuantum = 128.0f;   // Gothic world units (~100 = 1 m)
    // Entries in the per-light range cache are dropped once their light has been out of the dome this long.
    constexpr uint32_t kRangeCacheSweepFrames = 1024;
    constexpr uint32_t kRangeCacheMaxUnseen = 600;
    // Overlay slots are released a little past the horizon they are acquired at, so a light sitting on
    // Config::MidDist cannot pick one up and drop it on alternating frames.
    constexpr float kOverlayReleaseSlack = 1.1f;

    /** Is any BSP leaf this light lives in reachable through this frame's active portals? Lights not in the
        BSP cache (freshly spawned) are never gated. */
    bool AnyLeafPortalVisible( const VobLightInfo* info ) {
        const BspPortalCuller& pc = Engine::GAPI->GetPortalCuller();
        if ( !pc.IsActive() || info->ParentBSPNodes.empty() ) return true;
        for ( const BspInfo* node : info->ParentBSPNodes )
            if ( node && pc.IsLeafVisible( *node ) ) return true;
        return false;
    }
}


void PointLightSlotSelector::Configure( const Config& cfg ) {
    const bool resized = m_Static.size() != cfg.MaxStaticSlots || m_Dyn.size() != cfg.MaxDynamicSlots;
    m_Cfg = cfg;
    if ( resized ) {
        m_Static.clear();
        m_Static.resize( cfg.MaxStaticSlots );
        m_Dyn.clear();
        m_Dyn.resize( cfg.MaxDynamicSlots );
        m_StaticByKey.clear();
        m_DynByKey.clear();
    }
}


bool PointLightSlotSelector::AllowsDynamicCasters( const VobLightInfo* info ) {
    if ( !info ) return false;
    const int flags = Engine::GAPI->GetRendererState().RendererSettings.PointlightShadowCasterFlags;
    if ( info->IsPFXVobLight ) return ( flags & GothicRendererSettings::PLSC_PARTICLE_FX ) != 0;
    if ( info->IsStaticVobLight ) return ( flags & GothicRendererSettings::PLSC_STATIC_LIGHTS ) != 0;
    return ( flags & GothicRendererSettings::PLSC_DYNAMIC_LIGHTS ) != 0;
}


int PointLightSlotSelector::FindStaticSlotOf( uint64_t key ) const {
    if ( !key ) return -1;
    const auto it = m_StaticByKey.find( key );
    return it == m_StaticByKey.end() ? -1 : static_cast<int>( it->second );
}


int PointLightSlotSelector::FindDynSlotOf( uint64_t key ) const {
    if ( !key ) return -1;
    const auto it = m_DynByKey.find( key );
    return it == m_DynByKey.end() ? -1 : static_cast<int>( it->second );
}


void PointLightSlotSelector::ReleaseStaticSlot( uint32_t slot ) {
    if ( slot >= m_Static.size() ) return;
    if ( const uint64_t key = m_Static[slot].ownerKey ) {
        m_StaticByKey.erase( key );
        // The slot is back in the pool, so whatever this frame published for it is no longer sampleable -
        // shading against the next owner's depth reads as black rather than merely unshadowed.
        m_EncodedByKey.erase( key );
    }
    m_Static[slot] = StaticSlot{};
}


void PointLightSlotSelector::ReleaseDynSlot( uint32_t slot ) {
    if ( slot >= m_Dyn.size() ) return;
    if ( const uint64_t key = m_Dyn[slot].ownerKey ) {
        m_DynByKey.erase( key );
        // Whatever this frame published for the owner named this slot in its HI half; the slot is back in the
        // pool now, so drop the index rather than let the light sample the next occupant's depth.
        m_EncodedByKey.erase( key );
    }
    m_Dyn[slot] = DynSlot{};
}


void PointLightSlotSelector::ReleaseFor( uint64_t key ) {
    if ( const int s = FindStaticSlotOf( key ); s >= 0 ) ReleaseStaticSlot( static_cast<uint32_t>( s ) );
    if ( const int d = FindDynSlotOf( key ); d >= 0 ) ReleaseDynSlot( static_cast<uint32_t>( d ) );
}


void PointLightSlotSelector::ReleaseDynamicFor( uint64_t key ) {
    if ( const int d = FindDynSlotOf( key ); d >= 0 ) ReleaseDynSlot( static_cast<uint32_t>( d ) );
}


float PointLightSlotSelector::GetCubeRangeOf( uint64_t key ) const {
    const int s = FindStaticSlotOf( key );
    return s < 0 ? 0.0f : m_Static[s].range;
}


void PointLightSlotSelector::ReleaseAllSlots() {
    // Only reached by a mode change (PLS_DISABLED) and a pool resize, so every live bake this throws away
    // is attributable to that.
    for ( const StaticSlot& ss : m_Static )
        if ( ss.ownerKey && ss.valid )
            Engine::GAPI->GetRendererState().RendererInfo.NotePointLightRebake( PLR_MODE_CHANGED );
    for ( StaticSlot& ss : m_Static ) ss = StaticSlot{};
    for ( DynSlot& ds : m_Dyn ) ds = DynSlot{};
    m_StaticByKey.clear();
    m_DynByKey.clear();
    m_Assignments.clear();
    m_EncodedByKey.clear();
}


int32_t PointLightSlotSelector::GetEncodedIndex( uint64_t key ) const {
    if ( !key ) return 0;
    const auto it = m_EncodedByKey.find( key );
    return it == m_EncodedByKey.end() ? 0 : it->second;
}


void PointLightSlotSelector::CommitStatic( uint32_t slot ) {
    if ( slot >= m_Static.size() ) return;
    StaticSlot& ss = m_Static[slot];
    if ( !ss.ownerKey ) return;   // slot released since the render was resolved - nothing to validate
    ss.valid = true;
    ss.present = true;
    m_HaveCachedStatic = true;
}


void PointLightSlotSelector::CommitDynamic( uint32_t slot, bool has ) {
    if ( slot >= m_Dyn.size() ) return;
    DynSlot& ds = m_Dyn[slot];
    if ( !ds.ownerKey ) return;
    // `valid` is the ONLY thing that lets the lit pass sample this slot, and only a pass that actually drew
    // casters sets it. An overlay that found none may not be advertised on the strength of having been
    // scheduled: the backend skips the draw outright in that case, so the slot still holds whatever the
    // previous owner left, and a comparison sample against that reads as fully OCCLUDED, not unshadowed.
    ds.valid = has;
    if ( has ) {
        ds.emptyStreak = 0;
        ds.idleFrames = 0;
    } else {
        ++ds.emptyStreak;
    }
}


bool PointLightSlotSelector::IsNpcAttached( const zCVob* vob ) {
    for ( const zCVob* v = vob; v; v = v->GetVobParent() ) {
        if ( v->GetVobType() == zVOB_TYPE_NSC ) return true;
    }
    return false;
}


void PointLightSlotSelector::QueueVobChangedInvalidation( zCVob* vob ) {
    if ( !vob || !m_HaveCachedStatic ) return;
    // A falling vob reports several transform changes per frame, all resolving to the same answer.
    if ( std::ranges::contains( m_PendingVobChanges, vob ) ) return;
    m_PendingVobChanges.push_back( vob );
}


void PointLightSlotSelector::DrainPendingVobChanges() {
    if ( m_PendingVobChanges.empty() ) return;
    // Swapped out, not iterated in place: InvalidateStaticForVobRemoved below scrubs the pending list.
    m_DrainScratch.swap( m_PendingVobChanges );
    for ( zCVob* vob : m_DrainScratch ) {
        // OnRemovedVob erases the entry (and deletes the VobInfo), so a hit means the vob is still alive.
        VobInfo* vi = Engine::GAPI->GetVobByVob( vob );
        if ( !vi || !vi->Vob || !vi->VisualInfo ) continue;
        // Animates, so it is never baked - and this is the read that is unreliable at report time.
        if ( IsNpcAttached( vi->Vob ) ) continue;
        // Below the threshold this is jitter. The reference only advances on an actual invalidation, so
        // repeated sub-eps steps still add up to one.
        const XMFLOAT3 pos = vi->Vob->GetPositionWorld();
        if ( auto posIt = m_LastInvalidationPos.find( vi->Vob ); posIt != m_LastInvalidationPos.end() ) {
            const float dx = pos.x - posIt->second.x, dy = pos.y - posIt->second.y, dz = pos.z - posIt->second.z;
            if ( dx * dx + dy * dy + dz * dz < m_Cfg.VobMoveEpsSq ) continue;
        }
        // A move needs both halves: cubes that baked it show a shadow where it no longer is, and cubes
        // reaching where it is now are missing it.
        InvalidateStaticForVobRemoved( vi->Vob );
        InvalidateStaticForVobAdded( pos, vi->VisualInfo->MeshSize * 0.5f );
        // After the removal half, which drops this vob's entry.
        m_LastInvalidationPos.insert_or_assign( vi->Vob, pos );
    }
    m_DrainScratch.clear();
}


void PointLightSlotSelector::InvalidateStaticForVobAdded( const XMFLOAT3& posWS, float extent ) {
    // A cached static cube is only re-rendered when its light is fresh / moved / resized, never when the
    // geometry around it changes - so a new VOB in range has to say so. The margin mirrors the static-VOB
    // gather's cull (range + visual->MeshSize * 0.5f).
    for ( StaticSlot& ss : m_Static ) {
        if ( !ss.ownerKey || !ss.valid ) continue;
        const float r = ss.range + extent;
        const float dx = posWS.x - ss.pos.x, dy = posWS.y - ss.pos.y, dz = posWS.z - ss.pos.z;
        if ( dx * dx + dy * dy + dz * dz < r * r ) {
            ss.valid = false;
            ss.lastCause = PLR_VOB_ADDED;
            Engine::GAPI->GetRendererState().RendererInfo.NotePointLightRebake( PLR_VOB_ADDED );
        }
    }
}


void PointLightSlotSelector::InvalidateStaticForVobRemoved( const zCVob* vob ) {
    // Symmetric to the add case, but matched by POINTER against what each bake actually gathered: the vob is
    // being torn down, so its bbox and position can no longer be read. Never baked => nothing to invalidate,
    // which is what keeps an NPC's throwaway held item from re-rendering every cube nearby.
    if ( !vob ) return;
    // It may still be parked for a deferred resolve - the drain must never look it up.
    std::erase( m_PendingVobChanges, const_cast<zCVob*>( vob ) );
    m_LastInvalidationPos.erase( vob );   // the address may be reused by a different vob later
    m_Stationary.erase( vob );
    for ( StaticSlot& ss : m_Static ) {
        if ( !ss.ownerKey || !ss.valid || ss.bakedVobs.empty() ) continue;
        if ( std::ranges::contains( ss.bakedVobs, vob ) ) {
            ss.valid = false;
            ss.lastCause = PLR_VOB_REMOVED;
            Engine::GAPI->GetRendererState().RendererInfo.NotePointLightRebake( PLR_VOB_REMOVED );
        }
    }
}


void PointLightSlotSelector::BuildCandidates( std::vector<Candidate>& out ) {
    out.clear();
    ++m_SweepFrame;

    const GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;
    // Inner = ACTIVATION dome (compete for slots and budget), outer = DEACTIVATION dome (hand them back). The
    // band is the hysteresis: a light that leaves must travel its width back before it can re-acquire.
    const float activate = m_Cfg.DomeRadius > 0.0f ? m_Cfg.DomeRadius : settings.VisualFXDrawRadius;
    const float release = activate + m_Cfg.DomeMargin;
    const float activateSq = activate * activate;
    const float releaseSq = release * release;

    const XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
    const bool overlayMode = settings.EnablePointlightShadows >= GothicRendererSettings::PLS_UPDATE_DYNAMIC;
    const bool dropStaticLights = settings.DisableStaticPointlights;

    // The player's own bounding sphere, for the "standing inside this light" test below. Its position, not
    // the camera's: the shadow that must never be deferred is the one cast around the player.
    const zCVob* playerVob = Engine::GAPI->GetPlayerVob();
    XMVECTOR playerPos = XMVectorZero();
    float playerRadius = 0.0f;
    if ( playerVob ) {
        playerPos = playerVob->GetPositionWorldXM();
        const zTBBox3D& bb = playerVob->GetBBox();
        const XMVECTOR half = XMVectorScale(
            XMVectorSubtract( XMLoadFloat3( &bb.Max ), XMLoadFloat3( &bb.Min ) ), 0.5f );
        playerRadius = XMVectorGetX( XMVector3Length( half ) );
    }

    // Every registered light, not this frame's visible set: a light you turned away from must keep its cube.
    for ( const auto& [vob, info] : Engine::GAPI->VobLightMap ) {
        if ( !info || !info->Vob ) continue;
        if ( info->IsStaticVobLight && dropStaticLights ) continue;

        const XMFLOAT3 pos = info->Vob->GetPositionWorld();
        const float distSq = XMVectorGetX( XMVector3LengthSq( XMVectorSubtract( XMLoadFloat3( &pos ), camPos ) ) );
        if ( distSq > releaseSq ) continue;

        RangeState& st = m_Stationary[info->Vob];
        st.lastSeen = m_SweepFrame;
        const float snapped = std::ceil( info->Vob->GetLightRange() / kShadowRangeQuantum ) * kShadowRangeQuantum;
        if ( snapped > st.shadowRange ) st.shadowRange = snapped;
        if ( st.shadowRange <= 0.0f ) continue;   // a zero-range light has no cube to render

        const bool allowsCasters = AllowsDynamicCasters( info );
        // Gothic's IsStatic() bit: a pre-placed level fill light, which never moves and so bakes once and
        // costs nothing afterwards. Everything else can be carried across sectors and re-bakes as it goes.
        const bool isDynamicLight = !info->IsStaticVobLight;
        Candidate c;
        c.key = reinterpret_cast<uint64_t>( info->Vob );
        c.light = info;
        c.shadowOrigin = pos;
        c.shadowRange = st.shadowRange;
        c.distSq = distSq;
        // A disabled light stays a candidate so a flicker cannot cost it its slot, but `active` stops it
        // acquiring one or spending budget.
        c.active = distSq <= activateSq && info->Vob->IsEnabled();
        c.wantsDynamic = overlayMode && allowsCasters;
        c.restrictToWorld = !allowsCasters;
        // Standing inside this light's sphere: this is the shadow the player is actually looking at, so it
        // is served ahead of the frame budget and without a ceiling. Deliberately NOT gated on
        // isDynamicLight - `isStatic` is set on virtually every pre-placed level light, so gating it there
        // left the whole forced path dead for the wall torches and fireplaces the player walks into. The
        // gate belongs to the portal test below, which is the only thing that needs it.
        if ( c.active && playerVob ) {
            const float reach = c.shadowRange + playerRadius;
            const XMVECTOR d = XMVectorSubtract( XMLoadFloat3( &pos ), playerPos );
            c.forced = XMVectorGetX( XMVector3LengthSq( d ) ) <= reach * reach;
        }
        // A DYNAMIC light additionally has to be in a sector this frame's portals reach: it re-bakes every
        // time it moves, so budget spent on one sealed behind a door is waste, where a static light's cube is
        // baked once and then free. Checked AFTER the player test, which overrides it - a third-person camera
        // can sit in a different sector than the player, and the light around the player still matters.
        if ( c.active && isDynamicLight && !c.forced && !AnyLeafPortalVisible( info ) ) c.active = false;
        out.push_back( c );
    }

    // Drop tracking entries for lights long out of the dome. Swept rarely; walks the whole map.
    if ( ( m_SweepFrame % kRangeCacheSweepFrames ) == 0 ) {
        const uint32_t now = m_SweepFrame;
        gtl::erase_if( m_Stationary, [now]( const auto& e ) { return now - e.second.lastSeen > kRangeCacheMaxUnseen; } );
    }
}


int PointLightSlotSelector::PickStaticSlot( float distSq, FXMVECTOR camPos, bool forced ) {
    const uint32_t n = static_cast<uint32_t>( m_Static.size() );
    for ( uint32_t s = 0; s < n; ++s ) if ( !m_Static[s].ownerKey ) return static_cast<int>( s );
    // Nothing free: take the farthest owner, and only if this candidate beats it by EvictDistanceRatio -
    // without that margin two lights at similar distance trade a slot, and a bake, every frame. A forced
    // light needs only to be nearer, which is still a total order and so still cannot cycle.
    int best = -1;
    float bar = forced ? distSq : distSq * m_Cfg.EvictDistanceRatio;
    for ( uint32_t s = 0; s < n; ++s ) {
        const StaticSlot& os = m_Static[s];
        const XMVECTOR d = XMVectorSubtract( XMLoadFloat3( &os.pos ), camPos );
        const float dsq = XMVectorGetX( XMVector3LengthSq( d ) );
        if ( dsq > bar ) { bar = dsq; best = static_cast<int>( s ); }
    }
    return best;
}


int PointLightSlotSelector::PickDynSlot( float distSq, FXMVECTOR camPos, bool forced ) {
    const uint32_t n = static_cast<uint32_t>( m_Dyn.size() );
    for ( uint32_t d = 0; d < n; ++d ) if ( !m_Dyn[d].ownerKey ) return static_cast<int>( d );
    // An owner with no mover near it for a while donates first, ahead of the distance rule: it is holding
    // one of only MaxDynamicSlots overlays to draw nothing with.
    for ( uint32_t d = 0; d < n; ++d )
        if ( m_Dyn[d].idleFrames > m_Cfg.DynamicIdleFrames ) return static_cast<int>( d );
    int best = -1;
    float bar = forced ? distSq : distSq * m_Cfg.EvictDistanceRatio;
    for ( uint32_t d = 0; d < n; ++d ) {
        const int s = FindStaticSlotOf( m_Dyn[d].ownerKey );
        if ( s < 0 ) return static_cast<int>( d );   // owner lost its static cube; the overlay is orphaned
        const XMVECTOR od = XMVectorSubtract( XMLoadFloat3( &m_Static[s].pos ), camPos );
        const float dsq = XMVectorGetX( XMVector3LengthSq( od ) );
        if ( dsq > bar ) { bar = dsq; best = static_cast<int>( d ); }
    }
    return best;
}


void PointLightSlotSelector::Select( std::span<const Candidate> cands,
    GothicRendererSettings::EPointLightShadowMode shadowMode, bool resourcesReady ) {
    // Before anything decides renderStatic below, so a vob parked last frame lands in this frame's re-bake.
    DrainPendingVobChanges();

    m_Assignments.clear();
    m_EncodedByKey.clear();
    m_StarvedThisFrame = 0;
    if ( shadowMode == GothicRendererSettings::PLS_DISABLED ) {
        // Re-enabling mid-session must re-render from scratch, not sample however stale depth is left.
        ReleaseAllSlots();
        auto& offInfo = Engine::GAPI->GetRendererState().RendererInfo;
        offInfo.PointLightSlotsUsed = offInfo.PointLightStaticSlotsUsed = offInfo.PointLightSlotsStarved = 0;
        offInfo.PointLightSlotsMax = m_Cfg.MaxDynamicSlots;
        offInfo.PointLightStaticSlotsMax = m_Cfg.MaxStaticSlots;
        return;
    }
    // NOT gated on cands being non-empty: the dome release below still has to tick with zero lights in range.
    if ( !resourcesReady || m_Static.empty() ) return;

    const XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
    // PLS_FULL is the no-caching escape hatch: it never trusts a cached bake, so every light asks to
    // re-render and the frame budget round-robins them.
    const bool noCache = shadowMode >= GothicRendererSettings::PLS_FULL;

    // One candidate per ownership KEY, not per light: a cluster shares a key and wins one cube between all
    // its members, the nearest of which stands for it.
    m_Cands.clear();
    m_FrameKeys.clear();
    for ( uint32_t i = 0; i < static_cast<uint32_t>( cands.size() ); ++i ) {
        const Candidate& c = cands[i];
        if ( c.key == 0 || c.shadowRange <= 0.0f ) continue;
        if ( !m_FrameKeys.insert( c.key ).second ) continue;   // another cluster member already stands for it
        m_Cands.push_back( { i, c.key, c.light, c.distSq, c.active, c.wantsDynamic, c.restrictToWorld,
            c.forced } );
    }
    std::sort( m_Cands.begin(), m_Cands.end(), []( const Cand& a, const Cand& b ) {
        if ( a.distSq != b.distSq ) return a.distSq < b.distSq;
        return a.key < b.key;   // total order: equal distances must not permute between frames
        } );

    // Rebuilt here, then maintained by hand wherever a slot changes hands.
    m_StaticByKey.clear();
    m_DynByKey.clear();
    for ( uint32_t s = 0; s < static_cast<uint32_t>( m_Static.size() ); ++s )
        if ( m_Static[s].ownerKey ) m_StaticByKey.emplace( m_Static[s].ownerKey, s );
    for ( uint32_t d = 0; d < static_cast<uint32_t>( m_Dyn.size() ); ++d )
        if ( m_Dyn[d].ownerKey ) m_DynByKey.emplace( m_Dyn[d].ownerKey, d );

    // ---- Dome release ---------------------------------------------------------------------------------------
    // The candidate set is a pure distance sweep, so a key missing from it is out past the DEACTIVATION dome
    // or gone from the world - never merely off-screen. That is what makes an unconditional release safe.
    for ( uint32_t s = 0; s < static_cast<uint32_t>( m_Static.size() ); ++s ) {
        StaticSlot& ss = m_Static[s];
        if ( !ss.ownerKey || m_FrameKeys.contains( ss.ownerKey ) ) continue;
        if ( ss.valid ) Engine::GAPI->GetRendererState().RendererInfo.NotePointLightRebake( PLR_SLOT_AGED );
        ReleaseStaticSlot( s );
    }
    for ( uint32_t d = 0; d < static_cast<uint32_t>( m_Dyn.size() ); ++d ) {
        if ( !m_Dyn[d].ownerKey || m_FrameKeys.contains( m_Dyn[d].ownerKey ) ) continue;
        ReleaseDynSlot( d );   // an overlay is re-rendered from scratch, so losing one costs no cached work
    }
    // staleFrames is the fairness key the budget serves on, so it ticks for lights waiting their turn too.
    for ( StaticSlot& ss : m_Static ) if ( ss.ownerKey ) ++ss.staleFrames;
    for ( DynSlot& ds : m_Dyn ) if ( ds.ownerKey ) { ++ds.staleFrames; ++ds.idleFrames; }

    // ---- Slot assignment, nearest first ---------------------------------------------------------------------
    m_Wants.clear();   // parallel to m_Assignments; capacity reused, no per-frame realloc
    m_Near.clear();
    m_Mid.clear();
    m_Far.clear();
    m_Forced.clear();
    const float midDistSq = m_Cfg.MidDist * m_Cfg.MidDist;
    const float overlayDropSq = midDistSq * kOverlayReleaseSlack * kOverlayReleaseSlack;

    for ( const Cand& c : m_Cands ) {
        const Candidate& src = cands[c.srcIdx];

        // --- static tier: every light wants one, and keeps whatever it owns ---
        int sslot = FindStaticSlotOf( c.key );
        if ( sslot < 0 ) {
            if ( !c.active ) continue;   // in the hysteresis band, or disabled: keeps nothing, asks for nothing
            sslot = PickStaticSlot( c.distSq, camPos, c.forced );
            if ( sslot < 0 ) { ++m_StarvedThisFrame; continue; }
            StaticSlot& ns = m_Static[sslot];
            if ( ns.ownerKey ) {
                if ( ns.valid ) Engine::GAPI->GetRendererState().RendererInfo.NotePointLightRebake( PLR_SLOT_TAKEN );
                m_StaticByKey.erase( ns.ownerKey );
                // An overlay with no static cube under it has nothing to overlay ONTO, and the slot is one of
                // only MaxDynamicSlots - the evicted light gives it up with the cube it was drawn against.
                if ( const int od = FindDynSlotOf( ns.ownerKey ); od >= 0 ) ReleaseDynSlot( static_cast<uint32_t>( od ) );
            }
            ns = StaticSlot{};
            ns.ownerKey = c.key;
            ns.owner = c.light;
            ns.lastCause = PLR_SLOT_TAKEN;
            // Stamped now, not when the render happens: a budget-deferred acquisition would otherwise sit at
            // {0,0,0}, read as infinitely far to the eviction scan, and be taken straight back off the light.
            ns.pos = src.shadowOrigin;
            ns.range = src.shadowRange;
            m_StaticByKey[c.key] = static_cast<uint32_t>( sslot );
        }
        StaticSlot& ss = m_Static[sslot];
        ss.owner = c.light;

        // --- dynamic tier: only the near and mid buckets, and only for lights whose casters are opted in.
        // The far bucket gets no overlay at all: an NPC shadow 30 m away is not worth one of 64 slots, and
        // excluding them is what keeps the pool available to the lights you are standing among.
        int dslot = FindDynSlotOf( c.key );
        const bool overlayInRange = c.forced || c.distSq < midDistSq;
        if ( !c.wantsDynamic || ( dslot >= 0 && !c.forced && c.distSq >= overlayDropSq ) ) {
            if ( dslot >= 0 ) { ReleaseDynSlot( static_cast<uint32_t>( dslot ) ); dslot = -1; }
        } else if ( dslot < 0 && c.active && overlayInRange && !m_Dyn.empty() ) {
            dslot = PickDynSlot( c.distSq, camPos, c.forced );
            if ( dslot >= 0 ) {
                DynSlot& nd = m_Dyn[dslot];
                if ( nd.ownerKey ) m_DynByKey.erase( nd.ownerKey );
                nd = DynSlot{};
                nd.ownerKey = c.key;
                nd.owner = c.light;
                // Overdue on arrival, so a fresh slot draws on its acquisition frame instead of idling one.
                nd.staleFrames = m_Cfg.DynamicMaxBackoff;
                m_DynByKey[c.key] = static_cast<uint32_t>( dslot );
            }
        }

        // --- what does this light want done? ---
        const XMFLOAT3& np = src.shadowOrigin;
        const bool moved = std::fabs( np.x - ss.pos.x ) > m_Cfg.MoveEps
            || std::fabs( np.y - ss.pos.y ) > m_Cfg.MoveEps
            || std::fabs( np.z - ss.pos.z ) > m_Cfg.MoveEps;
        const bool rangeChanged = std::fabs( src.shadowRange - ss.range ) > m_Cfg.RangeEps;
        // Only when the bake was still considered good: a slot invalidated elsewhere already named its cause,
        // and counting this as well would report every re-bake twice under the wrong heading.
        if ( ss.valid && ( moved || rangeChanged ) ) {
            ss.lastCause = moved ? PLR_LIGHT_MOVED : PLR_RANGE_CHANGED;
            Engine::GAPI->GetRendererState().RendererInfo.NotePointLightRebake( ss.lastCause );
        }
        const bool wantStatic = c.active && ( !ss.valid || moved || rangeChanged || noCache );
        // An overlay tracks NPCs, so it wants a refresh every frame it holds a slot. One that keeps coming
        // back empty backs off exponentially instead - the cost of an overlay is its per-light caster cull,
        // which is paid whether or not anything is found.
        bool wantDyn = false;
        if ( dslot >= 0 && c.active ) {
            const DynSlot& ds = m_Dyn[dslot];
            // A forced light has the player standing inside it, so the premise the back-off rests on - that
            // this cull keeps finding nothing - is known to be false, and it refreshes every frame. Without
            // this the player walking into a light whose overlay had gone empty waited out its whole
            // back-off (up to DynamicMaxBackoff frames) before casting a shadow, which is the one thing
            // `forced` exists to stop.
            const uint32_t backoff = ( c.forced || ds.emptyStreak == 0 ) ? 1u
                : std::min( m_Cfg.DynamicMaxBackoff, 1u << std::min( ds.emptyStreak, 16u ) );
            // A slot that has never been drawn for this owner holds the PREVIOUS owner's casters and is not
            // sampled at all until it has, so it never waits its turn behind the back-off.
            wantDyn = ds.staleFrames >= backoff;
        }

        const uint32_t assignIdx = static_cast<uint32_t>( m_Assignments.size() );
        m_Assignments.push_back( { np, src.shadowRange, c.key, c.light, static_cast<uint32_t>( sslot ),
            dslot, false, false, c.restrictToWorld } );
        // Fairness key: how long this light has waited. A slot that has never held its owner's depth outranks
        // any amount of waiting, in either tier - it is content the light currently cannot sample at all.
        const bool unrendered = !ss.present || ( wantDyn && dslot >= 0 && !m_Dyn[dslot].valid );
        const uint32_t prio = unrendered ? UINT32_MAX
            : std::max( wantStatic ? ss.staleFrames : 0u, wantDyn ? m_Dyn[dslot].staleFrames : 0u );
        m_Wants.push_back( { wantStatic, wantDyn, prio } );
        if ( !wantStatic && !wantDyn ) continue;
        if ( c.forced ) m_Forced.push_back( assignIdx );
        ( c.distSq < m_Cfg.NearDist * m_Cfg.NearDist ? m_Near
            : c.distSq < midDistSq ? m_Mid : m_Far ).push_back( assignIdx );
    }

    // ---- Frame budget ---------------------------------------------------------------------------------------
    // At most RendersPerFrame LIGHTS do work, a light needing both a bake and an overlay still costing one.
    // The near buckets are CAPPED rather than prioritised outright, which is what guarantees the far bucket a
    // turn every frame and so lets a distant light eventually finish its first bake.
    {
        auto byPriority = [&]( uint32_t a, uint32_t b ) { return m_Wants[a].prio > m_Wants[b].prio; };
        std::stable_sort( m_Near.begin(), m_Near.end(), byPriority );
        std::stable_sort( m_Mid.begin(), m_Mid.end(), byPriority );
        std::stable_sort( m_Far.begin(), m_Far.end(), byPriority );

        uint32_t left = m_Cfg.RendersPerFrame;
        uint32_t served = 0;
        // Returns false for an entry already served this frame, so a forced light sitting in its bucket too
        // costs that bucket no cap.
        auto grant = [&]( uint32_t idx ) {
            Assignment& a = m_Assignments[idx];
            if ( a.renderStatic || a.renderDynamic ) return false;
            a.renderStatic = m_Wants[idx].wantStatic;
            a.renderDynamic = m_Wants[idx].wantDyn;
            if ( a.renderStatic ) {
                StaticSlot& ss = m_Static[a.staticSlot];
                ss.staleFrames = 0;
                // The bake about to run uses THIS origin/range, so stamping them here is what makes the
                // move/resize test above compare against what the cube actually holds. Deliberately not
                // stamped for a light the budget passed over, or its move would be forgotten.
                ss.pos = a.posWS;
                ss.range = a.range;
            }
            if ( a.renderDynamic ) m_Dyn[a.dynSlot].staleFrames = 0;
            if ( left ) --left;
            ++served;
            return true;
            };
        auto serve = [&]( const std::vector<uint32_t>& bucket, uint32_t cap ) {
            for ( uint32_t idx : bucket ) {
                if ( cap == 0 || left == 0 ) break;
                if ( grant( idx ) ) --cap;
            }
            };
        // Forced first and with no ceiling at all: a light the player is standing inside is the one shadow
        // that cannot be deferred. It still spends budget, so the buckets below get what is left of it.
        for ( uint32_t idx : m_Forced ) grant( idx );
        // The near/mid caps are what the far bucket's guaranteed share is carved out of; whatever they leave
        // unspent goes to the far bucket first and only then back to them, so the budget is never wasted.
        const uint32_t reserved = m_Cfg.NearBudget + m_Cfg.MidBudget < m_Cfg.RendersPerFrame
            ? m_Cfg.RendersPerFrame - m_Cfg.NearBudget - m_Cfg.MidBudget : 0u;
        serve( m_Near, std::min( m_Cfg.NearBudget, left > reserved ? left - reserved : 0u ) );
        serve( m_Mid, std::min( m_Cfg.MidBudget, left > reserved ? left - reserved : 0u ) );
        serve( m_Far, left );
        serve( m_Near, left );
        serve( m_Mid, left );

        auto& info = Engine::GAPI->GetRendererState().RendererInfo;
        info.PointLightStaticRendersWanted = static_cast<unsigned int>(
            m_Near.size() + m_Mid.size() + m_Far.size() );
        info.PointLightRendersGranted = served;
        // A wanted bake the budget passed over is postponed, not lost: `valid` stays false so it asks again
        // next frame. Counted so a sustained backlog is visible instead of just looking like slow shadows.
        for ( size_t i = 0; i < m_Assignments.size(); ++i )
            if ( m_Wants[i].wantStatic && !m_Assignments[i].renderStatic )
                info.NotePointLightRebake( PLR_BUDGET_DEFER );
    }

    // ---- Publish --------------------------------------------------------------------------------------------
    // Only ever on the strength of a render that DEMONSTRABLY happened, never of one this frame intends: a
    // slot still holding the previous occupant's depth comparison-samples as fully OCCLUDED, so getting this
    // wrong shades the light solid black rather than merely unshadowed. `present`, not `valid` - a bake a
    // world change invalidated is merely stale, and stale beats unshadowed while it waits its turn.
    for ( size_t i = 0; i < m_Assignments.size(); ++i ) {
        const Assignment& a = m_Assignments[i];
        const StaticSlot& ss = m_Static[a.staticSlot];
        if ( !ss.present ) continue;
        // A re-bake granted THIS frame runs before anything samples the cube, so it may skip the drift test
        // below - by sample time the cube really is at the new origin. A deferred one may not.
        if ( !a.renderStatic ) {
            // The shader looks the cube up from the light's CURRENT origin, so a light that has drifted off
            // its bake is sampling somebody else's geometry. A little drift is invisible and much better than
            // popping unshadowed; past a quarter of the cube's radius it is not.
            const float slack = std::max( m_Cfg.MoveEps, a.range * m_Cfg.StaleBakeMaxMoveFrac );
            if ( std::fabs( a.posWS.x - ss.pos.x ) > slack || std::fabs( a.posWS.y - ss.pos.y ) > slack
                || std::fabs( a.posWS.z - ss.pos.z ) > slack
                || std::fabs( a.range - ss.range ) > m_Cfg.RangeEps )
                continue;
        }
        // The overlay half only while its slot demonstrably holds drawn casters (CommitDynamic(has=true)).
        // Never on the strength of a scheduled render: see the note there.
        int dyn = -1;
        if ( a.dynSlot >= 0 ) {
            const DynSlot& ds = m_Dyn[a.dynSlot];
            if ( ds.valid ) dyn = a.dynSlot;
        }
        m_EncodedByKey[a.key] = EncodeIndex( static_cast<int>( a.staticSlot ), dyn );
    }

    // Occupancy + starvation for the ImGui point-light window; m_StarvedThisFrame is only knowable here.
    {
        auto& info = Engine::GAPI->GetRendererState().RendererInfo;
        unsigned int usedStatic = 0, usedDyn = 0;
        for ( const StaticSlot& ss : m_Static ) if ( ss.ownerKey ) ++usedStatic;
        for ( const DynSlot& ds : m_Dyn ) if ( ds.ownerKey ) ++usedDyn;
        info.PointLightSlotsUsed = usedDyn;
        info.PointLightSlotsMax = m_Cfg.MaxDynamicSlots;
        info.PointLightStaticSlotsUsed = usedStatic;
        info.PointLightStaticSlotsMax = m_Cfg.MaxStaticSlots;
        info.PointLightSlotsStarved = m_StarvedThisFrame;
    }
}
