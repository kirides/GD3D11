#pragma once
// Backend-neutral point-light shadow-cube slot selection, shared by the D3D11 and D3D12 renderers.
// Decisions only, no graphics API: which lights get a cube, out of which tier, in which slot, when the
// cached static bake is re-rendered and when the dynamic overlay runs. Backends own the resources.
//
// Two tiers, one slot index space:
//   global slot <  MaxHiSlots  -> full-res dynamic pool (per-frame overlay)
//   global slot >= MaxHiSlots  -> low-res static pool at (slot - MaxHiSlots), baked once and cached
// The shader gets the tier-ENCODED index (local index | tier bit); the bits differ per backend, so both
// masks come from Config.
#include <cstdint>
#include <span>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>
#include <gtl/phmap.hpp>

#include "GothicGraphicsState.h"

class zCVob;

class PointLightSlotSelector {
public:
    struct Config {
        uint32_t MaxHiSlots = 64;                  // full-res dynamic pool size (D3D11 128 / D3D12 64)
        uint32_t MaxLowSlots = 340;                // low-res static pool size; 340 is one array's hard ceiling
        uint32_t RetentionFrames = 600;            // ~10 s at 60 fps before an absent owner loses its slot
        uint32_t SlotStealGraceFrames = 300;       // ~5 s an absent owner's cube is off limits to newcomers
        uint32_t HandoverMaxFrames = 120;          // ~2 s a tier switch may keep its old cube waiting on the new
        uint32_t LowStaticRendersPerFrame = 8;     // per-frame ceiling on low-tier static (re)bakes
        uint32_t AlwaysDynamicCount = 8;           // nearest winners that always get the overlay
        uint32_t DynamicRoundRobinBudget = 6;      // most-stale winners serviced per frame beyond those
        float    MinShadowDist = 3000.0f;          // absolute candidacy horizon floor, Gothic units (~100 = 1 m)
        float    EvictDistanceRatio = 2.0f;        // squared-distance margin a newcomer must beat an owner by
        float    MoveEps = 0.5f;                   // below this the cube origin has not meaningfully moved
        float    RangeEps = 1.0f;                  // below this the cube range has not meaningfully changed
        float    VobMoveEpsSq = 1.0f;              // squared world units a vob must move to invalidate a bake
        int32_t  TierLowBit = 0;                   // OR'd into the encoded index for a low-tier slot
        int32_t  HasDynamicBit = 0;                // OR'd in when the lit pass should also sample the overlay
    };

    // One visible light, in the backend's shading order. Entries sharing a `key` are a cluster of co-located
    // static lights: one candidate for selection, and every member gets the winner's index.
    struct Candidate {
        uint64_t          key = 0;          // 0 = never shadowed. The light's vob pointer, or a cluster cell id.
        void*             owner = nullptr;  // opaque: zCVobLight* (D3D12) / VobLightInfo* (D3D11). nullptr for
                                            // clusters, which never reach the dynamic overlay's exclude list.
        DirectX::XMFLOAT3 shadowOrigin{};   // the CUBE's centre - the light's own, or its cluster's
        float             shadowRange = 0;  // the CUBE's far-plane basis - likewise
        bool              isStatic = false; // routing-static: never receives a dynamic overlay
        bool              preferLow = false;// PREFERRED tier, not necessarily the one it ends up in
        bool              spatiallyStatic = false;  // never moves; only such a candidate may SPILL into the
                                            // low-res tier, which bakes once and would be starved by a mover
        bool              restrictToWorld = false;  // world-mesh-only casters (the PFX gate) - passed through
    };

    // One decision, per winning key. `slot` is the GLOBAL index; IsLowSlot(slot) picks the tier.
    struct Assignment {
        DirectX::XMFLOAT3 posWS{};
        float             range = 0;
        uint32_t          slot = 0;
        uint64_t          key = 0;
        void*             owner = nullptr;
        bool              lowRes = false;
        bool              renderStatic = false;
        bool              renderDynamic = false;
        bool              overlayEligible = false;
        bool              restrictToWorld = false;
    };

    // Slots are owned by light identity and stay stable across frames, so a static winner whose light didn't
    // move reuses its cached cube instead of re-culling + re-rendering.
    struct Slot {
        uint64_t          ownerKey = 0;      // identity owning this slot (0 = free). Never dereferenced.
        void*             owner = nullptr;   // the owning light when ownerKey is a single light, else nullptr.
        DirectX::XMFLOAT3 pos{};             // last static-rendered cube origin (move detection)
        float             range = 0.0f;      // last static-rendered range (range-change detection)
        bool              isStatic = false;  // gates the overlay - a static light never receives one
        bool              staticPresent = false; // the target holds depth rendered for THIS owner, however
                                             // stale. Split from staticValid so an invalidated slot keeps
                                             // being sampled while it waits its turn in the render budget.
        bool              staticValid = false;  // the static depth is up to date; false => must re-render
        bool              dynamicValid = false; // the overlay holds a rendered caster set. Cleared only on a
                                             // SCHEDULED frame that produced no draws, so a round-robin light
                                             // keeps its last overlay between turns. Read one frame late.
        uint32_t          dynamicStaleFrames = 0;  // frames since the overlay last ran (round-robin)
        uint32_t          missingFrames = 0; // consecutive frames the owner was absent from the light set. The
                                             // set is frustum-culled upstream, so merely turning away drops a
                                             // light; its cached depth is kept for its return.
        // A tier switch hands over rather than back: the old slot stays owned and keeps being sampled until
        // the new one is baked. Not in m_SlotByKey (the key points at the new slot) and never a donor.
        bool              handoverFallback = false;
        uint32_t          handoverTarget = 0;      // the slot being baked; the fallback dies once it holds depth
        uint32_t          handoverFrames = 0;      // bounded, so a target that never bakes cannot pin a slot
        // Casters this slot's static bake covered. Rebuilt on each static (re)render and compared by POINTER
        // only - InvalidateStaticForVobRemoved fires when the vob may already be half torn down.
        std::vector<const zCVob*> bakedVobs;
    };

    /** Sizes the slot table. Safe to call again with the same config; a changed pool size wipes the table. */
    void Configure( const Config& cfg );
    const Config& GetConfig() const { return m_Cfg; }

    bool IsLowSlot( uint32_t globalSlot ) const { return globalSlot >= m_Cfg.MaxHiSlots; }
    uint32_t LowIndex( uint32_t globalSlot ) const { return globalSlot - m_Cfg.MaxHiSlots; }

    /** The whole decision. `cands` is parallel to the backend's light list, one entry per light (cluster
        members included); results are read back through GetAssignments()/GetEncodedIndex(). */
    void Select( std::span<const Candidate> cands, GothicRendererSettings::EPointLightShadowMode mode,
        bool resourcesReady = true );

    std::span<Assignment> GetAssignments() { return m_Assignments; }
    std::span<const Assignment> GetAssignments() const { return m_Assignments; }

    /** The tier-encoded ShadowCubeIndex this key may advertise this frame, or -1 for none. Includes keys that
        won no render this frame but still own a cube holding their own depth. */
    int32_t GetEncodedIndex( uint64_t key ) const;

    Slot& SlotAt( uint32_t slot ) { return m_Slots[slot]; }
    const Slot& SlotAt( uint32_t slot ) const { return m_Slots[slot]; }

    /** Slot index currently owned by `key`, or -1. */
    int FindSlotOf( uint64_t key ) const;
    /** The slot `key` is handing over FROM while its new slot bakes, or -1. It still holds that light's depth
        and is what should be sampled meanwhile - see Slot::handoverFallback. */
    int FindFallbackSlotOf( uint64_t key ) const;

    /** Hand a slot back unconditionally (the light died, the backend tore its resources down). */
    void ReleaseSlot( uint32_t slot );
    /** Drop every slot, so whatever comes next re-renders from scratch instead of sampling stale depth. */
    void ReleaseAllSlots();

    // ---- static-bake cache bookkeeping ---------------------------------------------------------------------
    /** Stamps a static (re)render that actually reached the GPU. Split from the decision so a frame that
        resolved a render but never issued it retries instead of caching an empty cube forever. */
    void CommitStatic( uint32_t slot );
    void CommitDynamic( uint32_t slot, bool has );

    // ---- world-change invalidation -------------------------------------------------------------------------
    /** True if this vob rides an NPC's transform - a held item, a torch, an attached effect. Such a caster is
        never baked into a cached static cube, so its comings and goings must not invalidate one either. */
    static bool IsNpcAttached( const zCVob* vob );

    /** Park a vob that just entered the world or just moved; the invalidation happens at the top of the next
        Select(). Deferred because neither of the two things it reads - the vob's position and whether it hangs
        off an NPC - is settled when Gothic reports the change: a dropped item is inserted where it came from,
        still parented to the NPC letting go of it, and then falls for several frames before coming to rest.
        Entries whose vob left the world meanwhile are dropped. */
    void QueueVobChangedInvalidation( zCVob* vob );

    void InvalidateStaticForVobAdded( const DirectX::XMFLOAT3& posWS, float extent );
    /** Matched by POINTER against what each slot baked (Slot::bakedVobs): by the time this fires the object
        may be half torn down, so `vob` is never dereferenced. */
    void InvalidateStaticForVobRemoved( const zCVob* vob );

private:
    void DrainPendingVobChanges();

    Config m_Cfg{};
    std::vector<Slot> m_Slots;
    std::vector<Assignment> m_Assignments;

    // Frame scratch; capacity reused frame to frame (32-bit address space rule).
    struct Cand { uint32_t srcIdx; uint64_t key; void* owner; float distSq; bool isStatic; bool lowRes;
                  bool spatiallyStatic; bool restrictToWorld; };
    std::vector<Cand> m_Cands;
    std::unordered_map<uint64_t, uint32_t> m_CandByKey;
    std::unordered_map<uint64_t, int32_t>  m_EncodedByKey;
    // Persistent key -> occupied-slot index that FindSlotOf answers from. Maintained wherever a slot changes
    // hands, and rebuilt from the table at the top of every Select.
    std::unordered_map<uint64_t, uint32_t> m_SlotByKey;
    // key -> the slot it is handing over FROM; m_SlotByKey holds the other half.
    std::unordered_map<uint64_t, uint32_t> m_FallbackByKey;
    gtl::flat_hash_set<uint64_t> m_FrameKeys;
    std::vector<Assignment*> m_Eligible;

    std::vector<zCVob*> m_PendingVobChanges;
    // Swapped with the above during a drain, so the invalidation helpers can scrub the pending list without
    // the loop iterating a vector that is being erased from. Capacity is retained.
    std::vector<zCVob*> m_DrainScratch;
    // Where each vob was when it last actually invalidated something - not last frame, so a slow drift keeps
    // accumulating until it crosses the threshold instead of being filtered out one sub-eps step at a time.
    gtl::flat_hash_map<const zCVob*, DirectX::XMFLOAT3> m_LastInvalidationPos;
    // Until some slot has finished a bake there is no cache to invalidate; this keeps world load (tens of
    // thousands of AddVob calls before the first frame) from parking any of them.
    bool m_HaveCachedStatic = false;

    uint32_t m_StarvedThisFrame = 0;
};
