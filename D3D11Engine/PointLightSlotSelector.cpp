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


int PointLightSlotSelector::FindFallbackSlotOf( uint64_t key ) const {
    if ( !key ) return -1;
    const auto it = m_FallbackByKey.find( key );
    return it == m_FallbackByKey.end() ? -1 : static_cast<int>( it->second );
}


void PointLightSlotSelector::ReleaseSlot( uint32_t slot ) {
    if ( slot >= m_Slots.size() ) return;
    // A fallback slot's key belongs to the slot it is handing over TO; only its own side of the pair goes.
    if ( m_Slots[slot].handoverFallback ) m_FallbackByKey.erase( m_Slots[slot].ownerKey );
    else if ( m_Slots[slot].ownerKey ) m_SlotByKey.erase( m_Slots[slot].ownerKey );
    m_Slots[slot] = Slot{};
}


void PointLightSlotSelector::ReleaseAllSlots() {
    for ( Slot& ss : m_Slots )
        if ( ss.ownerKey ) ss = Slot{};
    m_SlotByKey.clear();
    m_FallbackByKey.clear();
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
    for ( Slot& ss : m_Slots ) {
        if ( !ss.ownerKey || !ss.staticValid ) continue;
        const float r = ss.range + extent;
        const float dx = posWS.x - ss.pos.x, dy = posWS.y - ss.pos.y, dz = posWS.z - ss.pos.z;
        if ( dx * dx + dy * dy + dz * dz < r * r ) {
            ss.staticValid = false;
            Engine::GAPI->GetRendererState().RendererInfo.PointLightStaticInvalidations.Note();
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
    for ( Slot& ss : m_Slots ) {
        if ( !ss.ownerKey || !ss.staticValid || ss.bakedVobs.empty() ) continue;
        if ( std::ranges::contains( ss.bakedVobs, vob ) ) {
            ss.staticValid = false;
            Engine::GAPI->GetRendererState().RendererInfo.PointLightStaticInvalidations.Note();
        }
    }
}


void PointLightSlotSelector::Select( std::span<const Candidate> cands,
    GothicRendererSettings::EPointLightShadowMode shadowMode, bool resourcesReady ) {
    // Before anything decides renderStatic below, so a vob parked last frame lands in this frame's re-bake.
    DrainPendingVobChanges();

    // The closest in-range lights (up to the tier budget) win, but each gets a STABLE slot keyed by light
    // identity, so a static winner that didn't move reuses its cached cube (renderStatic=false) instead of
    // re-culling and re-rendering every caster. The global PointlightShadows setting gates the whole thing:
    //   PLS_DISABLED       - no winners at all; every index stays -1 and the pass never arms.
    //   PLS_STATIC_ONLY    - winners get their static cube but never a dynamic overlay.
    //   PLS_UPDATE_DYNAMIC - overlay for the near winners + a round-robin budget for the rest.
    //   PLS_FULL           - overlay for every winner, every frame (no round-robin).
    m_Assignments.clear();
    m_EncodedByKey.clear();
    m_StarvedThisFrame = 0;
    if ( shadowMode == GothicRendererSettings::PLS_DISABLED ) {
        // Re-enabling mid-session must re-render from scratch, not sample however stale depth is left.
        ReleaseAllSlots();
        return;
    }
    // NOT gated on cands being non-empty: retention/eviction still has to tick with zero visible lights.
    if ( !resourcesReady || m_Slots.empty() ) return;

    const uint32_t maxHi = m_Cfg.MaxHiSlots;
    const uint32_t maxSlots = static_cast<uint32_t>( m_Slots.size() );
    const uint32_t count = static_cast<uint32_t>( cands.size() );

    const XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
    // One candidate per ownership KEY, not per light: a cluster shares a key and wins one cube between all
    // its members. `srcIdx` is the first member found; the write-back at the end fans the result out.
    m_Cands.clear();
    m_CandByKey.clear();
    // key -> slot for every occupied slot; rebuilt here, then maintained by hand wherever a slot changes
    // hands. Without it the incumbency/ageing lookups are a per-frame O(slots*lights) scan. A slot
    // mid-handover goes in m_FallbackByKey instead: its key belongs to the NEW slot.
    m_SlotByKey.clear();
    m_FallbackByKey.clear();
    for ( uint32_t s = 0; s < maxSlots; ++s ) {
        if ( !m_Slots[s].ownerKey ) continue;
        if ( m_Slots[s].handoverFallback ) m_FallbackByKey.emplace( m_Slots[s].ownerKey, s );
        else m_SlotByKey.emplace( m_Slots[s].ownerKey, s );
    }
    // Every key the frame's light set carries, winner or not. Ageing keys off THIS: a light being shaded
    // still wants the cube it paid for, even on frames it is too far to compete for a new one.
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
        // range*9 alone puts a candle's horizon at ~13 m, and a light off this list is shaded unshadowed,
        // which the caller's range clamp then reads as switched OFF while it is still plainly visible. Hence
        // the absolute floor; the low tier can afford it, its cubes are tiny and cached forever.
        const float maxDist = std::max( range * 9.0f, m_Cfg.MinShadowDist );
        if ( distSq >= maxDist * maxDist ) continue;
        // Ranked on RAW distance, with no incumbency discount: the trim below cuts at this ranking, so a
        // discount let distant incumbents push a light in front of the camera off the list for good. The
        // hysteresis lives in the eviction rule instead.
        m_CandByKey.emplace( c.key, static_cast<uint32_t>( m_Cands.size() ) );
        m_Cands.push_back( { i, c.key, c.owner, distSq, c.isStatic, c.preferLow, c.spatiallyStatic,
            c.restrictToWorld } );
    }
    std::sort( m_Cands.begin(), m_Cands.end(), []( const Cand& a, const Cand& b ) {
        if ( a.distSq != b.distSq ) return a.distSq < b.distSq;
        return a.key < b.key;   // total order: equal distances must not permute between frames
        } );

    // Trim PER TIER, so a room full of static clusters can never crowd a dynamic torch out of the full-res
    // pool. Nearest-first within each tier; the losers go unshadowed.
    {
        uint32_t keptHi = 0, keptLow = 0;
        size_t out = 0;
        for ( size_t idx = 0; idx < m_Cands.size(); ++idx ) {
            Cand& c = m_Cands[idx];
            if ( !c.lowRes ) {
                // SPILL: a light that never moves has a cacheable cube, and a tiny cached cube beats no cube
                // at all (no cube means the range clamp, which reads as the light being switched off).
                if ( keptHi >= maxHi && c.spatiallyStatic ) c.lowRes = true;
            }
            uint32_t& kept = c.lowRes ? keptLow : keptHi;
            const uint32_t budget = c.lowRes ? m_Cfg.MaxLowSlots : maxHi;
            if ( kept >= budget ) { ++m_StarvedThisFrame; continue; }
            ++kept;
            m_Cands[out++] = c;
        }
        m_Cands.resize( out );
        // m_CandByKey meant "already has a candidate" above; from here on it is the winner set.
        m_CandByKey.clear();
        for ( size_t idx = 0; idx < m_Cands.size(); ++idx )
            m_CandByKey.emplace( m_Cands[idx].key, static_cast<uint32_t>( idx ) );
    }

    // Age slots whose owner is absent from the light set, but do NOT release them: that set is frustum-culled
    // upstream, so releasing on absence made every camera pan a fresh occupant and a full static re-render.
    // Presence, not winning - a light past the candidacy horizon still samples the cube it owns.
    for ( uint32_t s = 0; s < maxSlots; ++s ) {
        Slot& ss = m_Slots[s];
        if ( !ss.ownerKey ) continue;
        if ( ss.handoverFallback ) continue;   // bounded by the handover loop below
        if ( m_FrameKeys.contains( ss.ownerKey ) ) { ss.missingFrames = 0; continue; }
        if ( ++ss.missingFrames > m_Cfg.RetentionFrames ) {
            // Counted like any other invalidation, so slot churn shows up in the stat too.
            if ( ss.staticValid ) Engine::GAPI->GetRendererState().RendererInfo.PointLightStaticInvalidations.Note();
            m_SlotByKey.erase( ss.ownerKey );
            ss = Slot{};
        }
    }

    // Retire tier handovers whose new slot has arrived. The bake that fills a new cube lands a frame or more
    // later, and dropping the old one at the moment of the switch left the light range-clamped until then.
    for ( uint32_t s = 0; s < maxSlots; ++s ) {
        Slot& ss = m_Slots[s];
        if ( !ss.handoverFallback ) continue;
        const Slot& tgt = m_Slots[ss.handoverTarget];
        // Target baked, taken off us again, or the wait ran out of patience.
        const bool done = tgt.ownerKey != ss.ownerKey || tgt.staticPresent
            || ++ss.handoverFrames > m_Cfg.HandoverMaxFrames;
        if ( done ) {
            m_FallbackByKey.erase( ss.ownerKey );
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

        // Would this light actually USE the full-res tier? Only the per-frame overlay makes it worth anything,
        // and this is what decides whether a light in the low-res tier is promoted back out of it.
        const bool wantsOverlay = !c.isStatic && !c.restrictToWorld
            && shadowMode >= GothicRendererSettings::PLS_UPDATE_DYNAMIC;

        // Pick a slot in [b,e): a free one, else the longest-absent owner, else the farthest present owner and
        // only if this candidate beats it by EvictDistanceRatio - the anti-oscillation margin. -1 when every
        // owner is about as close as this light, i.e. genuine oversubscription.
        auto pickSlot = [&]( uint32_t b, uint32_t e ) -> int {
            for ( uint32_t s = b; s < e; ++s ) if ( !m_Slots[s].ownerKey ) return static_cast<int>( s );
            // Both the grace period and the distance test are what stop a camera pan from strip-mining the
            // pool: a light leaves the frustum the instant you turn past it, so without them every occupied
            // slot is a donor on every frame and turning in a circle loses every bake.
            int best = -1;
            uint32_t worst = m_Cfg.SlotStealGraceFrames;
            for ( uint32_t s = b; s < e; ++s ) {
                const Slot& os = m_Slots[s];
                if ( os.handoverFallback ) continue;         // mid-switch, and being sampled right now
                if ( os.missingFrames <= worst ) continue;   // free (0), or not absent long enough yet
                const XMVECTOR od = XMVectorSubtract( XMLoadFloat3( &os.pos ), camPos );
                if ( XMVectorGetX( XMVector3LengthSq( od ) ) <= c.distSq ) continue;   // nearer than us: leave it be
                worst = os.missingFrames;
                best = static_cast<int>( s );
            }
            if ( best >= 0 ) return best;
            float bar = c.distSq * m_Cfg.EvictDistanceRatio;
            for ( uint32_t s = b; s < e; ++s ) {
                const Slot& os = m_Slots[s];
                if ( !os.ownerKey || os.handoverFallback ) continue;
                const XMVECTOR d = XMVectorSubtract( XMLoadFloat3( &os.pos ), camPos );
                const float dsq = XMVectorGetX( XMVector3LengthSq( d ) );
                if ( dsq > bar ) { bar = dsq; best = static_cast<int>( s ); }
            }
            return best;
            };

        int slot = -1;
        // A light keeps whatever slot it owns, in either tier: tier preference decides where it LOOKS for a
        // slot, not where it may keep one, or a spilled light would ping-pong (and re-render) on every flip.
        // Two things force a low-res holder out, both one-directional:
        if ( auto it = m_SlotByKey.find( c.key ); it != m_SlotByKey.end() ) {
            const uint32_t owned = it->second;
            const bool ownedLow = IsLowSlot( owned );
            // 1. It STARTED MOVING. That tier bakes a cube once and caches it forever, and never runs the
            //    dynamic overlay, so a mover cannot stay in it.
            // 2. It now ranks into the full-res tier AND would get an overlay there - without this, a light
            //    that spilled while far away stayed static-only however close you walked up to it.
            const bool leaveLow = ownedLow && ( !c.spatiallyStatic || ( !c.lowRes && wantsOverlay ) );
            // The full-res slot is taken FIRST and the low one only then handed over: giving up a cube before
            // there is a replacement range-clamps the light. With nothing to give it stays put and asks again.
            slot = static_cast<int>( owned );
            if ( leaveLow ) {
                const int hi = pickSlot( 0, maxHi );
                if ( hi >= 0 ) {
                    slot = hi;
                    // Not released: it keeps this light's depth and stays sampled until `hi` is baked too.
                    Slot& old = m_Slots[owned];
                    old.handoverFallback = old.staticPresent;
                    old.handoverTarget = static_cast<uint32_t>( hi );
                    old.handoverFrames = 0;
                    if ( old.handoverFallback ) {
                        m_FallbackByKey[c.key] = owned;
                    } else {
                        // It never held this owner's depth, so there is nothing to hand over.
                        if ( old.staticValid ) Engine::GAPI->GetRendererState().RendererInfo.PointLightStaticInvalidations.Note();
                        old = Slot{};
                    }
                    m_SlotByKey.erase( c.key );
                }
            }
        }
        if ( slot < 0 ) slot = pickSlot( poolBegin, poolEnd );
        // SPILL, second half. The trim above cuts at a RANK, but ranking into the tier is not the same as
        // finding a slot in it - with stable ownership an admitted newcomer routinely finds nothing takeable
        // and would starve while the low tier sits empty. Availability is only knowable here.
        if ( slot < 0 && !c.lowRes && c.spatiallyStatic ) slot = pickSlot( maxHi, maxSlots );
        if ( slot < 0 ) { ++m_StarvedThisFrame; continue; }
        if ( m_Slots[slot].ownerKey != c.key ) {
            if ( m_Slots[slot].staticValid ) Engine::GAPI->GetRendererState().RendererInfo.PointLightStaticInvalidations.Note();
            if ( m_Slots[slot].ownerKey ) m_SlotByKey.erase( m_Slots[slot].ownerKey );
            m_SlotByKey[c.key] = static_cast<uint32_t>( slot );
            m_Slots[slot] = Slot{};
            m_Slots[slot].ownerKey = c.key;
            m_Slots[slot].owner = c.owner;        // nullptr for a cluster
            m_Slots[slot].staticValid = false;    // fresh occupant -> must render static
            // Stamped now, not when the render happens: a budget-deferred acquisition would otherwise sit at
            // {0,0,0}, read as infinitely far to the eviction scan, and be taken straight back off the light.
            m_Slots[slot].pos = cands[c.srcIdx].shadowOrigin;
            m_Slots[slot].range = cands[c.srcIdx].shadowRange;
        }
        Slot& ss = m_Slots[slot];
        ss.isStatic = c.isStatic;
        // Everything below follows the slot actually held, not the tier asked for.
        const bool slotLow = IsLowSlot( static_cast<uint32_t>( slot ) );

        // `!slotLow` is load-bearing: the low-res array has no dynamic twin, so an eligible low slot would
        // point the overlay pass at a target that does not exist. A spilled light gives its overlay up.
        const bool overlayEligible = wantsOverlay && !slotLow;
        // An ineligible slot must not keep advertising a stale overlay.
        if ( !overlayEligible ) ss.dynamicValid = false;

        const Candidate& src = cands[c.srcIdx];
        // Move/resize detection tracks the CUBE, not the light: a clustered light wandering inside its
        // cluster does not move the shared cube.
        const XMFLOAT3& np = src.shadowOrigin;
        const bool moved = std::fabs( np.x - ss.pos.x ) > m_Cfg.MoveEps
            || std::fabs( np.y - ss.pos.y ) > m_Cfg.MoveEps
            || std::fabs( np.z - ss.pos.z ) > m_Cfg.MoveEps;
        const bool rangeChanged = std::fabs( src.shadowRange - ss.range ) > m_Cfg.RangeEps;
        bool renderStatic = !ss.staticValid || moved || rangeChanged;

        // Amortize LOW-TIER static renders: acquisitions arrive in bursts (world load, a teleport, rounding a
        // corner), and a cube costs a full cull plus draws even though it is only rendered once. Nearest-first,
        // m_Cands already being sorted. The dynamic tier is deliberately un-budgeted - it holds few lights.
        if ( renderStatic && slotLow ) {
            if ( lowStaticBudget == 0 ) renderStatic = false;   // deferred; staticValid stays false so it retries
            else --lowStaticBudget;
        }

        // Publish the cube index only once the slot holds this owner's depth: a fresh slot whose render the
        // budget deferred still holds the PREVIOUS occupant's, which reads as a wrong shadow. staticPresent,
        // not staticValid - a slot invalidated by a world change is merely stale, and stale beats unshadowed.
        // A handover in flight advertises the OLD cube, which holds that depth right now. Gated on the light
        // still standing where it was baked from, since the shader looks a cube up from the current origin -
        // so this covers a promotion but not a light that left the low tier because it started moving.
        const auto fbIt = m_FallbackByKey.find( c.key );
        const bool useFallback = fbIt != m_FallbackByKey.end() && !ss.staticPresent
            && std::fabs( np.x - m_Slots[fbIt->second].pos.x ) <= m_Cfg.MoveEps
            && std::fabs( np.y - m_Slots[fbIt->second].pos.y ) <= m_Cfg.MoveEps
            && std::fabs( np.z - m_Slots[fbIt->second].pos.z ) <= m_Cfg.MoveEps
            && std::fabs( src.shadowRange - m_Slots[fbIt->second].range ) <= m_Cfg.RangeEps;
        if ( useFallback ) {
            // Never the has-dynamic bit: nothing refreshed an overlay into a slot being left behind.
            m_EncodedByKey[c.key] = IsLowSlot( fbIt->second )
                ? static_cast<int32_t>( LowIndex( fbIt->second ) ) | m_Cfg.TierLowBit
                : static_cast<int32_t>( fbIt->second );
        } else if ( ss.staticValid || ss.staticPresent || renderStatic ) {
            // Tier-encoded (local slot | tier bit), not the global slot. The has-dynamic bit tells the lit
            // pass to also sample the overlay array; only full-res slots can carry it.
            m_EncodedByKey[c.key] = slotLow
                ? static_cast<int32_t>( LowIndex( static_cast<uint32_t>( slot ) ) ) | m_Cfg.TierLowBit
                : ( static_cast<int32_t>( slot ) | ( ss.dynamicValid ? m_Cfg.HasDynamicBit : 0 ) );
        }
        m_Assignments.push_back( { np, src.shadowRange, static_cast<uint32_t>( slot ), c.key, c.owner,
            slotLow, renderStatic, false, overlayEligible, c.restrictToWorld } );
        if ( renderStatic ) { ss.pos = np; ss.range = src.shadowRange; }   // staticValid stamped once drawn
    }

    // ---- Non-winners that STILL OWN a valid cube keep sampling it -------------------------------------------
    // Winning decides who may spend slots and render passes, not what is sampleable: a light that fell off the
    // candidate list still owns cached depth, and dropping it to -1 range-clamps the light for free.
    // Both tiers, but only while the light still sits where the cube was baked from, since the shader looks it
    // up from the current origin. No has-dynamic bit - nothing refreshed the overlay this frame.
    for ( uint32_t i = 0; i < count; ++i ) {
        const uint64_t key = cands[i].key;
        if ( !key || m_EncodedByKey.contains( key ) ) continue;
        const auto it = m_SlotByKey.find( key );
        if ( it == m_SlotByKey.end() ) continue;
        const Slot& ss = m_Slots[it->second];
        if ( !ss.staticPresent ) continue;
        const XMFLOAT3& np = cands[i].shadowOrigin;
        if ( std::fabs( np.x - ss.pos.x ) > m_Cfg.MoveEps || std::fabs( np.y - ss.pos.y ) > m_Cfg.MoveEps
            || std::fabs( np.z - ss.pos.z ) > m_Cfg.MoveEps
            || std::fabs( cands[i].shadowRange - ss.range ) > m_Cfg.RangeEps )
            continue;
        m_EncodedByKey[key] = IsLowSlot( it->second )
            ? static_cast<int32_t>( LowIndex( it->second ) ) | m_Cfg.TierLowBit
            : static_cast<int32_t>( it->second );
    }

    // Occupancy + starvation for the ImGui point-light window; m_StarvedThisFrame is only knowable here.
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

    // Round-robin the skeletal overlay across eligible winners: the full sphere cull for every winner every
    // frame would multiply CPU cost with light count. The nearest AlwaysDynamicCount always get it, the rest
    // take turns by stale-frames count. Un-budgeted at PLS_FULL, skipped entirely below UPDATE_DYNAMIC.
    if ( shadowMode < GothicRendererSettings::PLS_UPDATE_DYNAMIC ) return;

    m_Eligible.clear();
    for ( Assignment& ps : m_Assignments ) if ( ps.overlayEligible ) m_Eligible.push_back( &ps );

    if ( shadowMode >= GothicRendererSettings::PLS_FULL ) {
        // "Very expensive. Don't use unless you encounter visual bugs." - no ranking and no budget.
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

    // A slot whose static base is re-rendered must refresh its overlay too, turn or not: the overlay was
    // resolved against the old depth, so keeping it ghosts stale skeletal shadows onto the new base.
    for ( Assignment& ps : m_Assignments ) {
        if ( ps.overlayEligible && ps.renderStatic && !ps.renderDynamic ) {
            ps.renderDynamic = true;
            m_Slots[ps.slot].dynamicStaleFrames = 0;
        }
    }
}
