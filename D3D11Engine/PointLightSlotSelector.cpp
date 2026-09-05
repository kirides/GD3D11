#include "pch.h"
#include "PointLightSlotSelector.h"

#include <algorithm>
#include <cmath>

#include "Engine.h"
#include "GothicAPI.h"
#include "zCVob.h"

using namespace DirectX;


void PointLightSlotSelector::Configure( const Config& cfg ) {
    const size_t total = static_cast<size_t>( cfg.MaxHiSlots ) + cfg.MaxLowSlots;
    const bool resized = m_Slots.size() != total || m_Cfg.MaxHiSlots != cfg.MaxHiSlots;
    m_Cfg = cfg;
    if ( resized ) {
        m_Slots.clear();
        m_Slots.resize( total );
        m_SlotByKey.clear();
    }
}


int PointLightSlotSelector::FindSlotOf( uint64_t key ) const {
    if ( !key ) return -1;
    const auto it = m_SlotByKey.find( key );
    return it == m_SlotByKey.end() ? -1 : static_cast<int>( it->second );
}


void PointLightSlotSelector::ReleaseSlot( uint32_t slot ) {
    if ( slot >= m_Slots.size() ) return;
    if ( m_Slots[slot].ownerKey ) m_SlotByKey.erase( m_Slots[slot].ownerKey );
    m_Slots[slot] = Slot{};
}


void PointLightSlotSelector::ReleaseAllSlots() {
    for ( Slot& ss : m_Slots )
        if ( ss.ownerKey ) ss = Slot{};
    m_SlotByKey.clear();
    m_Assignments.clear();
    m_EncodedByKey.clear();
}


int32_t PointLightSlotSelector::GetEncodedIndex( uint64_t key ) const {
    if ( !key ) return -1;
    const auto it = m_EncodedByKey.find( key );
    return it == m_EncodedByKey.end() ? -1 : it->second;
}


void PointLightSlotSelector::CommitStatic( uint32_t slot ) {
    if ( slot >= m_Slots.size() ) return;
    Slot& ss = m_Slots[slot];
    if ( !ss.ownerKey ) return;   // slot released since the render was resolved - nothing to validate
    ss.staticValid = true;
    ss.staticPresent = true;
    m_HaveCachedStatic = true;
}


void PointLightSlotSelector::CommitDynamic( uint32_t slot, bool has ) {
    if ( slot >= m_Slots.size() ) return;
    Slot& ss = m_Slots[slot];
    if ( !ss.ownerKey ) return;
    ss.dynamicValid = has;
}


bool PointLightSlotSelector::IsNpcAttached( const zCVob* vob ) {
    for ( const zCVob* v = vob; v; v = v->GetVobParent() ) {
        if ( v->GetVobType() == zVOB_TYPE_NSC ) return true;
    }
    return false;
}


void PointLightSlotSelector::QueueVobChangedInvalidation( zCVob* vob ) {
    if ( !vob || !m_HaveCachedStatic ) return;
    // A falling or thrown vob reports several transform changes per frame; they would all resolve to the same
    // answer, so only the first is kept. Scanning is fine - the list holds one frame of world changes.
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
        // Riding an NPC rather than lying on the ground: it animates, so it is never baked and must not
        // invalidate anything. This is the read that is unreliable at report time - see the header.
        if ( IsNpcAttached( vi->Vob ) ) continue;
        // Below the movement threshold this is jitter, not a move. The stored reference is only advanced when
        // an invalidation actually happens, so repeated sub-eps steps still add up to one.
        const XMFLOAT3 pos = vi->Vob->GetPositionWorld();
        if ( auto posIt = m_LastInvalidationPos.find( vi->Vob ); posIt != m_LastInvalidationPos.end() ) {
            const float dx = pos.x - posIt->second.x, dy = pos.y - posIt->second.y, dz = pos.z - posIt->second.z;
            if ( dx * dx + dy * dy + dz * dz < m_Cfg.VobMoveEpsSq ) continue;
        }
        // Two halves, and a move needs both: every cube that baked it is now showing a shadow where the vob
        // no longer is, and every cube reaching where it is NOW is missing it. A fresh add matches nothing in
        // the first half, a vob that moved within one light's reach matches in both.
        InvalidateStaticForVobRemoved( vi->Vob );
        InvalidateStaticForVobAdded( pos, vi->VisualInfo->MeshSize * 0.5f );
        // After the removal half, which drops this vob's entry along with the caster records it matched.
        m_LastInvalidationPos.insert_or_assign( vi->Vob, pos );
    }
    m_DrainScratch.clear();
}


void PointLightSlotSelector::InvalidateStaticForVobAdded( const XMFLOAT3& posWS, float extent ) {
    // A VOB added after a nearby point light already cached its static shadow cube would otherwise cast no
    // point-light shadow: the static cube is only re-rendered when the light is fresh / moved / resized, not when
    // world geometry around it changes. Walk the active slots and invalidate any whose light range the new VOB
    // reaches. Slots are empty during world load (ownerKey==0) so this is a no-op then; the margin mirrors the
    // static-VOB gather's cull (range + visual->MeshSize * 0.5f).
    for ( Slot& ss : m_Slots ) {
        if ( !ss.ownerKey || !ss.staticValid ) continue;
        const float r = ss.range + extent;
        const float dx = posWS.x - ss.pos.x, dy = posWS.y - ss.pos.y, dz = posWS.z - ss.pos.z;
        if ( dx * dx + dy * dy + dz * dz < r * r ) {
            ss.staticValid = false;   // re-render this slot's static depth next frame to include the new VOB
            Engine::GAPI->GetRendererState().RendererInfo.PointLightStaticInvalidations.Note();
        }
    }
}


void PointLightSlotSelector::InvalidateStaticForVobRemoved( const zCVob* vob ) {
    // Symmetric to the add case: a VOB removed from the world (an item picked up, a container emptied) must
    // stop casting into any light's cached static cube. Matched against the exact caster set the bake gathered,
    // by POINTER - the vob is being torn down, so its bbox and position can no longer be read, and identity is
    // the only thing left that is still meaningful. Nothing was baked => nothing to invalidate, which is what
    // keeps an NPC's throwaway held item (never baked, see IsNpcAttached) from re-rendering every cube nearby.
    if ( !vob ) return;
    // It may still be parked for a deferred resolve - drop it, the drain must never look it up.
    std::erase( m_PendingVobChanges, const_cast<zCVob*>( vob ) );
    m_LastInvalidationPos.erase( vob );   // the address may be reused by a different vob later
    for ( Slot& ss : m_Slots ) {
        if ( !ss.ownerKey || !ss.staticValid || ss.bakedVobs.empty() ) continue;
        if ( std::ranges::contains( ss.bakedVobs, vob ) ) {
            ss.staticValid = false;   // this slot's cube holds the removed vob - re-cache without it
            Engine::GAPI->GetRendererState().RendererInfo.PointLightStaticInvalidations.Note();
        }
    }
}


void PointLightSlotSelector::Select( std::span<const Candidate> cands,
    GothicRendererSettings::EPointLightShadowMode shadowMode, bool resourcesReady ) {
    // Before anything decides renderStatic below: apply the world changes that were parked last frame, so a
    // vob added or moved since then lands in this frame's re-bake rather than the next one.
    DrainPendingVobChanges();

    // Pick the closest-to-camera in-range lights (up to the tier budget) as this frame's "winners", but assign
    // each winner a STABLE slot keyed by its light identity (kept across frames, not reassigned by proximity
    // every frame). A slot's rendered content persists in the cube array, so a STATIC winner whose light didn't
    // move can reuse its cached cube (renderStatic=false) instead of re-culling + re-rendering all world/VOB/
    // skeletal casters each frame. Dynamic (moving) lights, newly-assigned slots, and moved/range-changed
    // lights render.
    //
    // The global PointlightShadows setting (ini [Shadows] PointlightShadows / the ImGui combo) gates the whole
    // thing:
    //   PLS_DISABLED       - no winners at all; every index stays -1 and the pass never arms.
    //   PLS_STATIC_ONLY    - winners get their static cube but never a dynamic overlay.
    //   PLS_UPDATE_DYNAMIC - overlay for the near winners + a round-robin budget for the rest.
    //   PLS_FULL           - overlay for every winner, every frame (no round-robin).
    m_Assignments.clear();
    m_EncodedByKey.clear();
    m_StarvedThisFrame = 0;
    if ( shadowMode == GothicRendererSettings::PLS_DISABLED ) {
        // Release every slot so re-enabling mid-session re-renders from scratch instead of sampling depth that
        // has been stale for however long the setting was off.
        ReleaseAllSlots();
        return;
    }
    // NOT gated on cands being non-empty: with zero visible lights every slot's owner is absent, and the
    // retention/eviction bookkeeping below still has to tick for them.
    if ( !resourcesReady || m_Slots.empty() ) return;

    const uint32_t maxHi = m_Cfg.MaxHiSlots;
    const uint32_t maxSlots = static_cast<uint32_t>( m_Slots.size() );
    const uint32_t count = static_cast<uint32_t>( cands.size() );

    const XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
    // One candidate per distinct ownership KEY, not per light: a cluster of co-located static lights shares a
    // key (and a shadowOrigin/shadowRange), so it competes for - and wins - exactly ONE cube between all its
    // members. `srcIdx` is just the first member found; the write-back at the end fans the result out to all.
    m_Cands.clear();
    m_CandByKey.clear();
    // key -> slot, for every OCCUPIED slot. It is what FindSlotOf answers from, so it outlives this call and is
    // maintained by hand wherever a slot changes hands (here and in ReleaseSlot). Rebuilt from the table at the
    // top of every Select anyway, so a desync could only ever last one frame. Without it the incumbency, ageing
    // and "does this winner already own a slot" lookups each walked all slots per light - fine at 192 slots, but
    // the static tier runs into the hundreds and that product is a per-frame O(slots*lights) scan.
    m_SlotByKey.clear();
    for ( uint32_t s = 0; s < maxSlots; ++s )
        if ( m_Slots[s].ownerKey ) m_SlotByKey.emplace( m_Slots[s].ownerKey, s );
    // Every ownership key the frame's light set carries, winner or not. Slot ageing keys off THIS, not off the
    // winner set: a light that is on screen and being shaded still wants the cube it already paid for, even on
    // frames it is too far away to compete for a new one.
    m_FrameKeys.clear();
    for ( uint32_t i = 0; i < count; ++i ) {
        const Candidate& c = cands[i];
        if ( c.key == 0 ) continue;                     // caller marked this light as never-shadowed
        m_FrameKeys.insert( c.key );
        if ( m_CandByKey.contains( c.key ) ) continue;  // another member of this cluster already stands for it
        // Ranked on the CUBE's placement, which for a clustered light is its cluster's.
        const float range = c.shadowRange;
        if ( range <= 0.0f ) continue;
        XMVECTOR d = XMVectorSubtract( XMLoadFloat3( &c.shadowOrigin ), camPos );
        const float distSq = XMVectorGetX( XMVector3LengthSq( d ) );
        // D3D11's historical distMaxShadowSq is range*9, which for a CANDLE (range ~150 units) puts the horizon
        // at ~13 m - and a light that falls off this list is shaded unshadowed, which the caller then range-
        // clamps to 0.35x/0.15x. The lit patch around the candle collapses and the light reads as switched OFF,
        // at a distance where it is still plainly visible. The horizon has to be about where the light stops
        // being *seen*, not where its own falloff sphere stops reaching the camera, so it gets an absolute
        // floor as well. The low tier can afford one: its cubes are tiny, rendered once and cached forever.
        const float maxDist = std::max( range * 9.0f, m_Cfg.MinShadowDist );
        if ( distSq >= maxDist * maxDist ) continue;
        // Ranked on RAW distance. This used to discount an incumbent's distance before ranking, as hysteresis
        // against a newcomer bumping an established light out on a marginal difference - but the ranking is also
        // what the per-tier TRIM cuts at, so with more candidate keys than slots the discount let far-away
        // incumbents outrank a light right in front of the camera and push it off the list entirely. Since a
        // light that is off the list cannot take a slot back (eviction only ever considered ABSENT owners), that
        // was permanent: the candle stayed unshadowed, hence range-clamped, hence dark, until something flushed
        // the light set. Nearest-first here; the hysteresis now lives where it belongs, in the eviction rule
        // below, which demands a newcomer be substantially closer than the owner it takes a slot from.
        m_CandByKey.emplace( c.key, static_cast<uint32_t>( m_Cands.size() ) );
        m_Cands.push_back( { i, c.key, c.owner, distSq, c.isStatic, c.preferLow, c.spatiallyStatic,
            c.restrictToWorld } );
    }
    std::sort( m_Cands.begin(), m_Cands.end(), []( const Cand& a, const Cand& b ) {
        if ( a.distSq != b.distSq ) return a.distSq < b.distSq;
        return a.key < b.key;   // total order: equal distances must not permute between frames
        } );

    // Trim PER TIER: the two pools are independent budgets, so a room full of static clusters can never crowd a
    // dynamic torch out of the full-res pool (and vice versa). Nearest-first within each tier; the losers simply
    // go unshadowed here - the caller has already range-clamped the statics so they cannot bleed far.
    {
        uint32_t keptHi = 0, keptLow = 0;
        size_t out = 0;
        for ( size_t idx = 0; idx < m_Cands.size(); ++idx ) {
            Cand& c = m_Cands[idx];
            if ( !c.lowRes ) {
                // SPILL. A light whose preferred tier is full does not go dark while the other tier sits empty:
                // if it never moves, its cube is cacheable, and a tiny cached cube is enormously better than no
                // cube at all (no cube means the range clamp, which reads as the light being switched off).
                if ( keptHi >= maxHi && c.spatiallyStatic ) c.lowRes = true;
            }
            uint32_t& kept = c.lowRes ? keptLow : keptHi;
            const uint32_t budget = c.lowRes ? m_Cfg.MaxLowSlots : maxHi;
            if ( kept >= budget ) { ++m_StarvedThisFrame; continue; }   // more candidate keys than either tier holds
            ++kept;
            m_Cands[out++] = c;
        }
        m_Cands.resize( out );
        // m_CandByKey stood for "already has a candidate" during the build above; from here on it is the winner
        // set, which the ageing loop below tests every occupied slot against.
        m_CandByKey.clear();
        for ( size_t idx = 0; idx < m_Cands.size(); ++idx )
            m_CandByKey.emplace( m_Cands[idx].key, static_cast<uint32_t>( idx ) );
    }

    // Age slots whose owner is not in this frame's light set at all - but do NOT release them. That set is
    // frustum-culled upstream, so a light drops out merely because the camera turned away; its cached static
    // depth is still valid and is exactly what should be reused the moment it comes back. Releasing on absence
    // made every frustum blink a fresh occupant, i.e. a full static re-cull + re-render of the world sections
    // and VOB instances around that light. Slots are surrendered only under real pressure (below) or once the
    // absence outlives RetentionFrames.
    // Presence, NOT winning: a light past the candidacy horizon keeps being SHADED every frame, so it keeps
    // sampling the cube it owns (see the cached-cube publish below) and must not have it aged out from under it.
    for ( uint32_t s = 0; s < maxSlots; ++s ) {
        Slot& ss = m_Slots[s];
        if ( !ss.ownerKey ) continue;
        if ( m_FrameKeys.contains( ss.ownerKey ) ) { ss.missingFrames = 0; continue; }
        if ( ++ss.missingFrames > m_Cfg.RetentionFrames ) {
            // Retention expired - the cached depth goes with the slot. Counted like any other invalidation, so
            // slot churn shows up in the stat too.
            if ( ss.staticValid ) Engine::GAPI->GetRendererState().RendererInfo.PointLightStaticInvalidations.Note();
            m_SlotByKey.erase( ss.ownerKey );
            ss = Slot{};
        }
    }

    // Per-frame ceiling on low-tier static (re)renders - see the deferral in the assignment loop below.
    uint32_t lowStaticBudget = m_Cfg.LowStaticRendersPerFrame;

    // Assign each winner a stable slot (keep its existing one, else grab a free one) and decide render vs cache.
    // The search is confined to the winner's TIER so the two budgets stay genuinely independent.
    for ( const Cand& c : m_Cands ) {
        const uint32_t poolBegin = c.lowRes ? maxHi : 0u;
        const uint32_t poolEnd = c.lowRes ? maxSlots : maxHi;

        // Would this light actually USE the full-res tier? Only one that receives the per-frame skeletal overlay
        // there has anything to gain from it. Computed before slot assignment because it is what decides whether
        // a light sitting in the low-res tier gets promoted back out of it.
        const bool wantsOverlay = !c.isStatic && !c.restrictToWorld
            && shadowMode >= GothicRendererSettings::PLS_UPDATE_DYNAMIC;

        // Pick a slot in [b,e): a free one; else the longest-absent owner (an absent light's cache is worth
        // keeping, but not at the cost of a light on screen now); else the FARTHEST present owner, and only if
        // this candidate is substantially closer than it - EvictDistanceRatio is the anti-oscillation margin, so
        // two lights at similar distances can never trade a slot back and forth. -1 when every owner is about as
        // close as this light, i.e. genuine oversubscription rather than arrival order.
        auto pickSlot = [&]( uint32_t b, uint32_t e ) -> int {
            for ( uint32_t s = b; s < e; ++s ) if ( !m_Slots[s].ownerKey ) return static_cast<int>( s );
            int best = -1;
            uint32_t worst = 0;
            for ( uint32_t s = b; s < e; ++s )
                if ( m_Slots[s].missingFrames > worst ) { worst = m_Slots[s].missingFrames; best = static_cast<int>( s ); }
            if ( best >= 0 ) return best;
            float bar = c.distSq * m_Cfg.EvictDistanceRatio;
            for ( uint32_t s = b; s < e; ++s ) {
                const Slot& os = m_Slots[s];
                if ( !os.ownerKey ) continue;
                const XMVECTOR d = XMVectorSubtract( XMLoadFloat3( &os.pos ), camPos );
                const float dsq = XMVectorGetX( XMVector3LengthSq( d ) );
                if ( dsq > bar ) { bar = dsq; best = static_cast<int>( s ); }
            }
            return best;
            };

        int slot = -1;
        // A light keeps whatever slot it already owns, in EITHER tier. Tier preference decides where a light
        // LOOKS for a slot, not where it is allowed to keep one: re-homing a spilled light the moment pressure
        // eased would make it ping-pong between tiers, and every flip is a fresh cube render. Two things do
        // force a low-res holder out, both of them one-directional:
        if ( auto it = m_SlotByKey.find( c.key ); it != m_SlotByKey.end() ) {
            const uint32_t owned = it->second;
            const bool ownedLow = IsLowSlot( owned );
            // 1. It STARTED MOVING. That tier bakes a cube once and caches it forever, and never runs the
            //    dynamic overlay, so a mover cannot stay in it.
            bool handBack = ownedLow && !c.spatiallyStatic;
            // 2. It now ranks into the full-res tier (the trim left c.lowRes false) AND would get a dynamic
            //    overlay there. Without this a light that spilled while far away stayed in the cached tier for
            //    good: walk right up to it under UPDATE DYNAMIC and it still drew static-only shadows, because
            //    ownership alone kept it there. Promotion is attempted only when a full-res slot can ACTUALLY
            //    be had - giving up the cube it holds for nothing would leave it unshadowed, and unshadowed is
            //    what the range clamp turns into a light that looks switched off.
            if ( !handBack && ownedLow && !c.lowRes && wantsOverlay ) {
                const int hi = pickSlot( 0, maxHi );
                if ( hi >= 0 ) { handBack = true; slot = hi; }
            }
            if ( handBack ) {
                if ( m_Slots[owned].staticValid ) Engine::GAPI->GetRendererState().RendererInfo.PointLightStaticInvalidations.Note();
                m_Slots[owned] = Slot{};
                m_SlotByKey.erase( c.key );
            } else {
                slot = static_cast<int>( owned );
            }
        }
        if ( slot < 0 ) slot = pickSlot( poolBegin, poolEnd );
        if ( slot < 0 ) { ++m_StarvedThisFrame; continue; }
        if ( m_Slots[slot].ownerKey != c.key ) {
            if ( m_Slots[slot].staticValid ) Engine::GAPI->GetRendererState().RendererInfo.PointLightStaticInvalidations.Note();
            if ( m_Slots[slot].ownerKey ) m_SlotByKey.erase( m_Slots[slot].ownerKey );
            m_SlotByKey[c.key] = static_cast<uint32_t>( slot );
            m_Slots[slot] = Slot{};
            m_Slots[slot].ownerKey = c.key;
            m_Slots[slot].owner = c.owner;        // nullptr for a cluster - see the Slot::owner comment
            m_Slots[slot].staticValid = false;    // fresh occupant -> must render static (the slot changed hands)
            // Stamp the intended cube origin NOW, not when the static render finally happens: an acquisition
            // whose render the per-frame budget defers would otherwise sit at pos {0,0,0}, read as infinitely
            // far to the eviction scan above, and be taken straight back off the light that just got it.
            m_Slots[slot].pos = cands[c.srcIdx].shadowOrigin;
            m_Slots[slot].range = cands[c.srcIdx].shadowRange;
        }
        Slot& ss = m_Slots[slot];
        ss.isStatic = c.isStatic;
        // Everything below follows the slot this light ACTUALLY holds, not the tier it asked for - a spilled
        // light sits in the low-res array and must be encoded, budgeted and overlay-gated as such.
        const bool slotLow = IsLowSlot( static_cast<uint32_t>( slot ) );

        // Can this slot EVER receive the skeletal overlay? Static lights never do, and neither does anything
        // below PLS_UPDATE_DYNAMIC. `!slotLow` is load-bearing, not belt-and-braces: the low-res array has no
        // dynamic twin, so a low slot becoming overlay-eligible would point the overlay pass at a target that
        // does not exist for it. Since the SPILL, a non-static light can end up in that tier too - it gives up
        // its overlay in exchange for a cube.
        const bool overlayEligible = wantsOverlay && !slotLow;
        // An ineligible slot must not keep advertising a stale overlay: drop the bit so the lit pass stops
        // sampling the dynamic array for it the moment the setting (or the light's IsStatic) changes.
        if ( !overlayEligible ) ss.dynamicValid = false;

        const Candidate& src = cands[c.srcIdx];
        // Move/resize detection tracks the CUBE, not the light: a clustered light wandering inside its cluster
        // does not move the shared cube, and must not invalidate its cached static depth.
        const XMFLOAT3& np = src.shadowOrigin;
        const bool moved = std::fabs( np.x - ss.pos.x ) > m_Cfg.MoveEps
            || std::fabs( np.y - ss.pos.y ) > m_Cfg.MoveEps
            || std::fabs( np.z - ss.pos.z ) > m_Cfg.MoveEps;
        const bool rangeChanged = std::fabs( src.shadowRange - ss.range ) > m_Cfg.RangeEps;
        // The static cube is re-rendered only when fresh / the light moved / the range changed; otherwise reused.
        bool renderStatic = !ss.staticValid || moved || rangeChanged;

        // Amortize LOW-TIER static renders across frames. A static cube is rendered once and then cached forever,
        // but "once" still costs a full world-section cull plus its draws - and acquisitions arrive in BURSTS
        // (world load, a teleport, rounding a corner into a lit district), which without a budget means up to
        // MaxLowSlots full static renders in a single frame and a very visible hitch. Nearest-first, because
        // m_Cands is already sorted by distance. The dynamic tier is deliberately NOT budgeted: it holds few
        // lights, they are the ones the player is looking at.
        if ( renderStatic && slotLow ) {
            if ( lowStaticBudget == 0 ) renderStatic = false;   // deferred; staticValid stays false so it retries
            else --lowStaticBudget;
        }

        // Publish the cube index ONLY once the slot actually holds this owner's depth - either it was already
        // cached, or it is being rendered this very frame (the cube pass runs before the lit pass). A fresh slot
        // whose render got deferred by the budget above must NOT be sampled yet: it still holds the previous
        // occupant's depth, which would read as a wrong shadow. Leaving it -1 makes the light unshadowed for a
        // few frames instead, and the caller's range clamp keeps it from bleeding meanwhile.
        // staticPresent, not just staticValid: a slot that was invalidated by a world change still holds this
        // owner's depth and stays sampleable until the re-render lands. Only a FRESH slot (holding the previous
        // occupant's depth) is withheld.
        if ( ss.staticValid || ss.staticPresent || renderStatic ) {
            // What the shader sees is the TIER-ENCODED index (local slot | tier bit), not the global slot. The
            // has-dynamic bit additionally tells the lit pass to sample the dynamic-overlay array for this light
            // and min it with the static one. Only full-res slots can carry it (the low tier has no dynamic
            // twin), and it reflects the last SUBMITTED overlay - see Slot::dynamicValid on the one-frame lag.
            m_EncodedByKey[c.key] = slotLow
                ? static_cast<int32_t>( LowIndex( static_cast<uint32_t>( slot ) ) ) | m_Cfg.TierLowBit
                : ( static_cast<int32_t>( slot ) | ( ss.dynamicValid ? m_Cfg.HasDynamicBit : 0 ) );
        }
        m_Assignments.push_back( { np, src.shadowRange, static_cast<uint32_t>( slot ), c.key, c.owner,
            slotLow, renderStatic, false, overlayEligible, c.restrictToWorld } );
        if ( renderStatic ) { ss.pos = np; ss.range = src.shadowRange; }   // staticValid stamped once actually drawn
    }

    // ---- Non-winners that STILL OWN a valid cube keep sampling it -------------------------------------------
    // Winning is about who may SPEND slots and render passes this frame; it is not what makes a cube sampleable.
    // A static light that fell off the candidate list (past the horizon above, or beaten to the last free slot)
    // used to drop to index -1 while its own depth sat there in a slot it still owns, cached and resting - and
    // going unshadowed is what triggers the caller's range clamp, i.e. the light visibly switching off.
    // Publishing the cube it already owns costs nothing: no re-render, no barrier, no entry in the assignments,
    // just one shadow sample the light was worth anyway.
    //
    // BOTH tiers, but only while the light still sits where the cube was rendered from: a cube is valid for the
    // ORIGIN it was baked at, and the shader looks it up from the light's CURRENT origin, so a light that moved
    // away from its bake would sample a cube centred somewhere else. That check is what makes this safe for the
    // full-res tier too, whose lights can move. What a full-res non-winner does NOT get is the has-dynamic bit:
    // nothing refreshed its skeletal overlay this frame, and a stale one would leave an NPC's shadow standing
    // where the NPC no longer is. Its static depth is still exactly right.
    for ( uint32_t i = 0; i < count; ++i ) {
        const uint64_t key = cands[i].key;
        if ( !key || m_EncodedByKey.contains( key ) ) continue;
        const auto it = m_SlotByKey.find( key );
        if ( it == m_SlotByKey.end() ) continue;
        const Slot& ss = m_Slots[it->second];
        if ( !ss.staticPresent ) continue;   // slot holds no depth for this owner yet
        const XMFLOAT3& np = cands[i].shadowOrigin;
        if ( std::fabs( np.x - ss.pos.x ) > m_Cfg.MoveEps || std::fabs( np.y - ss.pos.y ) > m_Cfg.MoveEps
            || std::fabs( np.z - ss.pos.z ) > m_Cfg.MoveEps
            || std::fabs( cands[i].shadowRange - ss.range ) > m_Cfg.RangeEps )
            continue;
        m_EncodedByKey[key] = IsLowSlot( it->second )
            ? static_cast<int32_t>( LowIndex( it->second ) ) | m_Cfg.TierLowBit
            : static_cast<int32_t>( it->second );
    }

    // Occupancy + starvation, for the ImGui point-light window. Counted here rather than derived later:
    // m_StarvedThisFrame is only knowable inside the trim and assignment loops.
    {
        auto& info = Engine::GAPI->GetRendererState().RendererInfo;
        unsigned int usedHi = 0, usedLow = 0;
        for ( uint32_t s = 0; s < maxSlots; ++s )
            if ( m_Slots[s].ownerKey ) ( IsLowSlot( s ) ? usedLow : usedHi )++;
        info.PointLightSlotsUsed = usedHi;
        info.PointLightSlotsMax = maxHi;
        info.PointLightStaticSlotsUsed = usedLow;
        info.PointLightStaticSlotsMax = m_Cfg.MaxLowSlots;
        info.PointLightSlotsStarved = m_StarvedThisFrame;
    }

    // Round-robin the per-frame skeletal DYNAMIC overlay across overlay-eligible winners. With many persisted
    // lights, running the full sphere-cull-against-registered-skeletal-vobs pass for every single winner every
    // frame would multiply CPU cost with light count. The nearest AlwaysDynamicCount winners (where a moving
    // caster's shadow lag would be most visible) always get it; the rest take turns via a stale-frames counter
    // so every dynamic light's overlay still refreshes periodically instead of never. Static geometry shadows
    // are unaffected - those persist via the static cache regardless. Skipped wholesale below
    // PLS_UPDATE_DYNAMIC (nothing is eligible), and un-budgeted at PLS_FULL.
    if ( shadowMode < GothicRendererSettings::PLS_UPDATE_DYNAMIC ) return;

    m_Eligible.clear();
    for ( Assignment& ps : m_Assignments ) if ( ps.overlayEligible ) m_Eligible.push_back( &ps );

    if ( shadowMode >= GothicRendererSettings::PLS_FULL ) {
        // "Very expensive. Don't use unless you encounter visual bugs." - every eligible winner overlays every
        // frame, no distance ranking and no budget.
        for ( Assignment* ps : m_Eligible ) {
            ps->renderDynamic = true;
            m_Slots[ps->slot].dynamicStaleFrames = 0;
        }
    } else {
        std::sort( m_Eligible.begin(), m_Eligible.end(), [&]( const Assignment* a, const Assignment* b ) {
            XMVECTOR da = XMVectorSubtract( XMLoadFloat3( &a->posWS ), camPos );
            XMVECTOR db = XMVectorSubtract( XMLoadFloat3( &b->posWS ), camPos );
            return XMVectorGetX( XMVector3LengthSq( da ) ) < XMVectorGetX( XMVector3LengthSq( db ) );
            } );

        const size_t closeCount = std::min<size_t>( m_Cfg.AlwaysDynamicCount, m_Eligible.size() );
        for ( size_t idx = 0; idx < closeCount; ++idx ) {
            m_Eligible[idx]->renderDynamic = true;
            m_Slots[m_Eligible[idx]->slot].dynamicStaleFrames = 0;
        }
        for ( size_t idx = closeCount; idx < m_Eligible.size(); ++idx ) ++m_Slots[m_Eligible[idx]->slot].dynamicStaleFrames;

        // Among the "far" set, service the most-stale slots first, up to this frame's round-robin budget.
        std::sort( m_Eligible.begin() + closeCount, m_Eligible.end(), [&]( const Assignment* a, const Assignment* b ) {
            return m_Slots[a->slot].dynamicStaleFrames > m_Slots[b->slot].dynamicStaleFrames;
            } );
        for ( size_t idx = closeCount, serviced = 0;
            idx < m_Eligible.size() && serviced < m_Cfg.DynamicRoundRobinBudget; ++idx, ++serviced ) {
            m_Eligible[idx]->renderDynamic = true;
            m_Slots[m_Eligible[idx]->slot].dynamicStaleFrames = 0;
        }
    }

    // An ELIGIBLE slot whose STATIC base is (re)rendered this frame must also refresh its dynamic overlay,
    // round-robin turn or not: the overlay was resolved against the OLD static depth, which is invalid once the
    // static geometry/position changes. Not forcing this would either ghost stale skeletal shadows onto a new
    // depth base, or silently drop the overlay for a light that's actually due for a refresh. Ineligible slots
    // have no overlay to reconcile.
    for ( Assignment& ps : m_Assignments ) {
        if ( ps.overlayEligible && ps.renderStatic && !ps.renderDynamic ) {
            ps.renderDynamic = true;
            m_Slots[ps.slot].dynamicStaleFrames = 0;
        }
    }
}
