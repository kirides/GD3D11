#pragma once
// Backend-neutral point-light shadow-cube SLOT SELECTION, shared by the D3D11 and D3D12 renderers.
//
// This is D3D12PointShadows::SelectShadowedLights lifted out verbatim - D3D12 is the reference
// implementation and D3D11 was moved onto it, not the other way round. Everything here is a DECISION:
// which lights get a cube, out of which of the two tiers, in which slot, when the cached static bake is
// re-rendered, when the per-frame dynamic overlay runs, and when a slot may be advertised to the shader.
// Nothing here touches a graphics API; the backends own their resources and draws and consume the
// Assignments this produces.
//
// Two tiers, ONE slot index space so all the ownership bookkeeping stays single-pool:
//   global slot <  MaxHiSlots  -> the full-res dynamic pool (a cube per moving/near light, per-frame overlay)
//   global slot >= MaxHiSlots  -> the low-res static pool at (slot - MaxHiSlots), baked once and cached forever
// What reaches the SHADER is the tier-ENCODED index (local index | tier bit), not the global slot - and the
// two backends do not agree on which bit is which, so both masks come from Config.
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

    // One visible light on its way into the frame's light buffer, in the order the backend will shade them.
    // Several entries may share a `key`: that is exactly how a CLUSTER of co-located static lights ends up
    // sharing one cube - they are one candidate for selection and every member gets the winner's index.
    struct Candidate {
        uint64_t          key = 0;          // 0 = never shadowed. The light's vob pointer, or a cluster cell id.
        void*             owner = nullptr;  // opaque to us: zCVobLight* (D3D12) / VobLightInfo* (D3D11).
                                            // nullptr for clusters - only ever handed back for the dynamic
                                            // overlay's exclude list, which no low-tier slot reaches.
        DirectX::XMFLOAT3 shadowOrigin{};   // the CUBE's centre - the light's own, or its cluster's
        float             shadowRange = 0;  // the CUBE's far-plane basis - likewise
        bool              isStatic = false; // routing-static: never receives a dynamic overlay
        bool              preferLow = false;// PREFERRED tier, not necessarily the one it ends up in
        bool              spatiallyStatic = false;  // never moves, whatever tier it was routed to. A full-res
                                            // candidate that cannot be given a full-res cube SPILLS into the
                                            // low-res tier when this holds - that tier bakes once and caches
                                            // forever, so a mover would re-enter its per-frame render budget
                                            // every single frame and starve everything else in it.
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

    // Slots are owned by light identity and kept stable across frames (not reassigned by proximity), so a
    // static winner whose light didn't move reuses its cached cube instead of re-culling + re-rendering.
    struct Slot {
        uint64_t          ownerKey = 0;      // identity owning this slot (0 = free). Compared for identity only;
                                             // never dereferenced.
        void*             owner = nullptr;   // the owning light when ownerKey is a single light, else nullptr.
        DirectX::XMFLOAT3 pos{};             // last static-rendered cube origin (move detection)
        float             range = 0.0f;      // last static-rendered range (range-change detection)
        bool              isStatic = false;  // gates the overlay - a static light never receives one
        bool              staticPresent = false; // this slot's static target physically holds depth rendered for
                                             // THIS owner, even if a later world change has since marked it out
                                             // of date. Kept apart from staticValid so an INVALIDATED slot keeps
                                             // being sampled (slightly stale) while it waits its turn in the
                                             // render budget, instead of dropping to unshadowed - a stale shadow
                                             // is a far smaller artifact than a light that briefly bleeds through
                                             // walls, and it is what makes over-invalidation genuinely cheap.
        bool              staticValid = false;  // the static depth is up to date; false => must re-render
        bool              dynamicValid = false; // the dynamic overlay holds a valid, already-rendered caster set.
                                             // Set on a frame whose overlay produced draws, cleared on a
                                             // SCHEDULED frame that produced none - deliberately left alone on an
                                             // unscheduled frame, so a round-robin light keeps its last overlay
                                             // between turns instead of flickering. Read one frame late by
                                             // design: Select() runs before the pass resolves this frame.
        uint32_t          dynamicStaleFrames = 0;  // frames since the overlay last ran (round-robin)
        uint32_t          missingFrames = 0; // consecutive frames this slot's owner was absent from the light
                                             // set; 0 while present. A slot is NOT released the moment its light
                                             // drops out (the light set is frustum-culled upstream, so merely
                                             // turning away drops it): its cached static depth is still good and
                                             // is exactly what should be reused when the light comes back.
        // Every caster identity the static bake put into this slot, in the order it gathered them. Rebuilt from
        // scratch on each static (re)render and consulted ONLY by InvalidateStaticForVobRemoved, which compares
        // pointers - by the time that fires the vob may be half torn down and unreadable.
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
        did NOT win a render slot this frame but still own a cube holding their own depth. */
    int32_t GetEncodedIndex( uint64_t key ) const;

    Slot& SlotAt( uint32_t slot ) { return m_Slots[slot]; }
    const Slot& SlotAt( uint32_t slot ) const { return m_Slots[slot]; }

    /** Slot index currently owned by `key`, or -1. */
    int FindSlotOf( uint64_t key ) const;

    /** Hand a slot back unconditionally (the light died, the backend tore its resources down). */
    void ReleaseSlot( uint32_t slot );
    /** Drop every slot, so whatever comes next re-renders from scratch instead of sampling stale depth. */
    void ReleaseAllSlots();

    // ---- static-bake cache bookkeeping ---------------------------------------------------------------------
    /** True once a static (re)render into this slot is known to have reached the GPU. Split from the decision
        so a frame that resolved a render but never issued it leaves the slot uncached and simply retries -
        stamping at resolve time meant "we intended to draw it", and a slot marked cached while its target held
        only the clear silently lost its static shadow FOREVER (nothing re-renders a slot that believes it is
        cached). */
    void CommitStatic( uint32_t slot );
    void CommitDynamic( uint32_t slot, bool has );

    // ---- world-change invalidation -------------------------------------------------------------------------
    /** True if this vob rides an NPC's transform - a held item, a torch, an attached effect. Such a caster
        moves with the animation every frame, so it is never baked into a cached static cube and its comings
        and goings must not invalidate one either. */
    static bool IsNpcAttached( const zCVob* vob );

    /** Park a vob that just entered the world or just moved; the invalidation itself happens at the top of the
        next Select(). Deferred rather than decided on the spot because the two things the decision reads - the
        vob's position and whether it hangs off an NPC - are not settled when Gothic reports the change. An item
        is inserted at the hand/waypoint it came from and placed afterwards, its parent link can still be the NPC
        that is letting go of it, and it then FALLS: several transform changes across several frames before it
        comes to rest. So this coalesces to one resolve per frame reading the position as it is by then, and a
        still-moving vob simply queues again next frame until it stops. Entries whose vob left the world
        meanwhile are dropped; still being in GothicAPI's vob map is the liveness test that makes the deferred
        read safe. */
    void QueueVobChangedInvalidation( zCVob* vob );

    void InvalidateStaticForVobAdded( const DirectX::XMFLOAT3& posWS, float extent );
    /** Removal is matched by POINTER against what each slot actually baked (Slot::bakedVobs), never by reading
        the vob: by the time this fires the object may already be half torn down, and its bbox/position are no
        longer trustworthy. Identity comparison only - `vob` is never dereferenced. */
    void InvalidateStaticForVobRemoved( const zCVob* vob );

private:
    void DrainPendingVobChanges();

    Config m_Cfg{};
    std::vector<Slot> m_Slots;
    std::vector<Assignment> m_Assignments;

    // Frame scratch. All static-lifetime capacity, reused frame to frame (32-bit address space rule).
    struct Cand { uint32_t srcIdx; uint64_t key; void* owner; float distSq; bool isStatic; bool lowRes;
                  bool spatiallyStatic; bool restrictToWorld; };
    std::vector<Cand> m_Cands;
    std::unordered_map<uint64_t, uint32_t> m_CandByKey;
    std::unordered_map<uint64_t, int32_t>  m_EncodedByKey;
    // Not scratch: the persistent key -> occupied-slot index that FindSlotOf answers from. Maintained wherever
    // a slot changes hands, and rebuilt from the table at the top of every Select.
    std::unordered_map<uint64_t, uint32_t> m_SlotByKey;
    gtl::flat_hash_set<uint64_t> m_FrameKeys;
    std::vector<Assignment*> m_Eligible;

    std::vector<zCVob*> m_PendingVobChanges;
    // Swapped with the above for the duration of a drain, so the invalidation helpers it calls can scrub the
    // pending list without the loop iterating a vector that is being erased from. Capacity is retained.
    std::vector<zCVob*> m_DrainScratch;
    // Where each vob was when it last actually invalidated something. The reference is that position, NOT the
    // previous frame's, so a slow drift keeps accumulating until it crosses the threshold instead of being
    // filtered out one sub-eps step at a time.
    gtl::flat_hash_map<const zCVob*, DirectX::XMFLOAT3> m_LastInvalidationPos;
    // Has any slot ever finished a static bake? Until one has there is no cache to invalidate, which is what
    // keeps world load (tens of thousands of AddVob calls before the first frame) from parking any of them.
    bool m_HaveCachedStatic = false;

    uint32_t m_StarvedThisFrame = 0;
};
