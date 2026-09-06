#pragma once
// Backend-neutral point-light shadow-cube slot selection, shared by the D3D11 and D3D12 renderers. Decisions
// only, no graphics API: which lights get cubes, in which slots, what is re-rendered and what is sampled.
//
// TWO INDEPENDENT TIERS - see POINTLIGHT_TWO_TIER_PLAN.md:
//   static  (MaxStaticSlots)  - the core shadow: world mesh, static VOBs, static MOBs. EVERY light wants one.
//   dynamic (MaxDynamicSlots) - an overlay holding only the movers (dynamic VOBs, NPCs, NPC-attached VOBs),
//                               sampled and min'd on top of the static cube. Optional, and much scarcer.
// A light holds a static slot, a dynamic slot, both or neither; the two index spaces are unrelated.
#include <cstdint>
#include <span>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>
#include <gtl/phmap.hpp>

#include "GothicGraphicsState.h"

class zCVob;
class zCVobLight;
struct VobLightInfo;

class PointLightSlotSelector {
public:
    // ShadowCubeIndex encoding, identical in both backends and in every shader that samples a point shadow:
    // LO 16 bits = static slot + 1, HI 16 bits = dynamic slot + 1, so 0 in a half means "no cube in that tier".
    static constexpr int32_t kSlotShift = 16;
    static constexpr int32_t kSlotMask = 0xFFFF;
    static int32_t EncodeIndex( int staticSlot, int dynSlot ) {
        return ( ( staticSlot + 1 ) & kSlotMask ) | ( ( ( dynSlot + 1 ) & kSlotMask ) << kSlotShift );
    }

    struct Config {
        uint32_t MaxStaticSlots = 340;    // 340 is one array's hard ceiling: 2048 slices / 6 faces
        uint32_t MaxDynamicSlots = 64;
        // At most this many LIGHTS do shadow work per frame; a light needing both a bake and an overlay still
        // costs one. NearBudget + MidBudget must stay below it, which is what guarantees the far bucket a turn.
        uint32_t RendersPerFrame = 8;
        uint32_t NearBudget = 4;
        uint32_t MidBudget = 3;
        float    NearDist = 1500.0f;      // Gothic world units (~100 = 1 m)
        float    MidDist = 3000.0f;       // also the overlay horizon - the far bucket gets no overlay at all
        // Inside DomeRadius a light competes for slots and budget; past DomeRadius + DomeMargin it hands its
        // slots back. Deliberately NOT a frustum test - see the plan doc.
        float    DomeRadius = 0.0f;       // 0 = track RendererSettings::VisualFXDrawRadius
        float    DomeMargin = 1000.0f;
        float    EvictDistanceRatio = 2.0f;   // squared-distance margin a newcomer must beat an owner by
        float    MoveEps = 0.5f;          // below this the cube origin has not meaningfully moved
        float    RangeEps = 1.0f;         // below this the cube range has not meaningfully changed
        float    VobMoveEpsSq = 1.0f;     // squared world units a vob must move to invalidate a bake
        // How far a light may drift off its bake origin and still sample it while the re-bake waits its turn.
        // Past this the cube is somewhere else entirely and unshadowed is the lesser evil.
        float    StaleBakeMaxMoveFrac = 0.25f;
        // An overlay that keeps coming back empty backs off exponentially (1, 2, 4 ... DynamicMaxBackoff
        // frames) instead of re-culling every frame, and gives its scarce slot up after DynamicIdleFrames.
        uint32_t DynamicIdleFrames = 120;
        uint32_t DynamicMaxBackoff = 30;
    };

    // One light, as the selector sees it. Entries sharing a `key` are a cluster of co-located static lights:
    // one candidate for selection, and every member gets the winner's index.
    struct Candidate {
        uint64_t          key = 0;              // 0 = never shadowed. The light's vob pointer, or a cluster id.
        VobLightInfo*     light = nullptr;      // a member light; never null
        DirectX::XMFLOAT3 shadowOrigin{};       // the CUBE's centre - the light's own, or its cluster's
        float             shadowRange = 0;      // the CUBE's far-plane basis - likewise
        float             distSq = 0;           // to the camera, from shadowOrigin
        bool              active = false;       // inside the ACTIVATION dome: may compete for slots and budget
        bool              wantsDynamic = false; // eligible for an overlay slot
        bool              restrictToWorld = false;  // world-mesh-only casters (the PFX/static gate)
        // The player vob is standing inside this light's sphere. Such a light is served before the frame
        // budget and without a ceiling, and may take a slot without the usual eviction margin.
        bool              forced = false;
    };

    // One decision, per key that owns a static slot this frame. `dynSlot` is -1 when it holds no overlay.
    struct Assignment {
        DirectX::XMFLOAT3 posWS{};
        float             range = 0;
        uint64_t          key = 0;
        VobLightInfo*     light = nullptr;
        uint32_t          staticSlot = 0;
        int32_t           dynSlot = -1;
        bool              renderStatic = false;   // (re)bake the static cube this frame
        bool              renderDynamic = false;  // (re)render the overlay this frame
        bool              restrictToWorld = false;
    };

    // Slots are owned by light identity and stay stable across frames, so a light that has not moved reuses
    // its cached cube instead of re-culling and re-rendering every caster.
    struct StaticSlot {
        uint64_t          ownerKey = 0;     // identity owning this slot (0 = free). Never dereferenced.
        VobLightInfo*     owner = nullptr;
        DirectX::XMFLOAT3 pos{};            // last static-rendered cube origin (move detection)
        float             range = 0.0f;     // last static-rendered range (range-change detection)
        // The target holds depth rendered for THIS owner, however stale. Split from `valid` so an invalidated
        // slot keeps being sampled while it waits its turn in the frame budget.
        bool              present = false;
        bool              valid = false;    // the static depth is up to date; false => must re-render
        uint32_t          staleFrames = 0;  // frames since this light last did any shadow work (fairness)
        // Why this slot's cached bake was last thrown away. Debug only (the ImGui point-light overlay names
        // it for the nearest light); nothing in the selection logic reads it.
        EPointLightRebakeCause lastCause = PLR_NUM_CAUSES;   // PLR_NUM_CAUSES = never invalidated
        // Casters this bake covered, compared by POINTER only - InvalidateStaticForVobRemoved fires when the
        // vob may already be half torn down.
        std::vector<const zCVob*> bakedVobs;
    };

    struct DynSlot {
        uint64_t      ownerKey = 0;
        VobLightInfo* owner = nullptr;
        // The one gate on sampling this slot, and only a pass that actually DREW casters sets it. There is
        // deliberately no "was scheduled" flag: an overlay that found none is not drawn at all, so the slot
        // still holds the previous owner's depth and comparison-samples as fully OCCLUDED.
        bool          valid = false;
        uint32_t      staleFrames = 0;      // frames since the overlay last ran
        uint32_t      emptyStreak = 0;      // consecutive scheduled runs that found no casters (back-off)
        uint32_t      idleFrames = 0;       // frames since it last found any; past DynamicIdleFrames the slot
                                            // goes back so a light that HAS movers can have it
    };

    /** Sizes the slot tables. Safe to call again with the same config; a changed pool size wipes them. */
    void Configure( const Config& cfg );
    const Config& GetConfig() const { return m_Cfg; }

    /** True when this light's category is opted into VOB/NPC casters (PointlightShadowCasterFlags). Off, its
        cube holds the world mesh alone and it never receives an overlay. Shared by both backends. */
    static bool AllowsDynamicCasters( const VobLightInfo* info );

    /** This frame's candidate set: every registered light inside the RELEASE dome, with its quantized cube
        range folded in. Distance only - no frustum, no portal test, so turning away from a light cannot cost
        it its cube. Keep `out` across frames so its capacity is reused. */
    void BuildCandidates( std::vector<Candidate>& out );

    /** The whole decision. Read back through GetAssignments()/GetEncodedIndex(). */
    void Select( std::span<const Candidate> cands, GothicRendererSettings::EPointLightShadowMode mode,
        bool resourcesReady = true );

    std::span<Assignment> GetAssignments() { return m_Assignments; }
    std::span<const Assignment> GetAssignments() const { return m_Assignments; }

    /** The HI-LO ShadowCubeIndex this key may advertise, or 0 for unshadowed. Includes keys that did no work
        this frame but still own a cube holding their own depth. */
    int32_t GetEncodedIndex( uint64_t key ) const;

    /** The far-plane basis the key's static cube was BAKED with, or 0. The shader must normalize its depth
        compare by this and not by the light's live range, or the shadow detaches from its caster. */
    float GetCubeRangeOf( uint64_t key ) const;

    uint32_t NumStaticSlots() const { return static_cast<uint32_t>( m_Static.size() ); }
    uint32_t NumDynSlots() const { return static_cast<uint32_t>( m_Dyn.size() ); }
    StaticSlot& StaticSlotAt( uint32_t slot ) { return m_Static[slot]; }
    const StaticSlot& StaticSlotAt( uint32_t slot ) const { return m_Static[slot]; }
    DynSlot& DynSlotAt( uint32_t slot ) { return m_Dyn[slot]; }
    const DynSlot& DynSlotAt( uint32_t slot ) const { return m_Dyn[slot]; }

    /** Slot index currently owned by `key`, or -1. */
    int FindStaticSlotOf( uint64_t key ) const;
    int FindDynSlotOf( uint64_t key ) const;

    /** Hand both of a key's slots back (the light died, the backend tore its resources down). */
    void ReleaseFor( uint64_t key );
    /** Hand back only the overlay slot, keeping the static cube (the backend has no overlay array). */
    void ReleaseDynamicFor( uint64_t key );
    /** Drop every slot, so whatever comes next re-renders instead of sampling stale depth. */
    void ReleaseAllSlots();

    // ---- render-completion stamps --------------------------------------------------------------------------
    /** Stamps a static (re)render that actually reached the GPU. Split from the decision so a frame that
        resolved a render but never issued it retries instead of caching an empty cube forever. */
    void CommitStatic( uint32_t slot );
    /** `has` = the overlay pass actually DREW casters into the slot, which is the only thing that makes it
        sampleable. False drives the back-off and, eventually, the release of this scarce slot. */
    void CommitDynamic( uint32_t slot, bool has );

    // ---- world-change invalidation -------------------------------------------------------------------------
    /** True if this vob rides an NPC's transform. Such a caster is never baked into a cached static cube, so
        its comings and goings must not invalidate one either. */
    static bool IsNpcAttached( const zCVob* vob );

    /** Park a vob that just entered the world or moved; the invalidation happens at the top of the next
        Select(). Deferred because neither its position nor its NPC parent link is settled when Gothic reports
        the change - a dropped item is inserted, re-parented, and then falls for several frames. */
    void QueueVobChangedInvalidation( zCVob* vob );

    void InvalidateStaticForVobAdded( const DirectX::XMFLOAT3& posWS, float extent );
    /** Matched by POINTER against StaticSlot::bakedVobs: by the time this fires the object may be half torn
        down, so `vob` is never dereferenced. */
    void InvalidateStaticForVobRemoved( const zCVob* vob );

private:
    void DrainPendingVobChanges();
    void ReleaseStaticSlot( uint32_t slot );
    void ReleaseDynSlot( uint32_t slot );
    /** Free slot in the table, else the farthest owner this candidate beats by EvictDistanceRatio, else -1.
        `forced` drops that margin to "merely farther" - still a total order, so it cannot cycle. */
    int PickStaticSlot( float distSq, DirectX::FXMVECTOR camPos, bool forced );
    int PickDynSlot( float distSq, DirectX::FXMVECTOR camPos, bool forced );

    Config m_Cfg{};
    std::vector<StaticSlot> m_Static;
    std::vector<DynSlot> m_Dyn;
    std::vector<Assignment> m_Assignments;

    // Frame scratch; capacity reused frame to frame (32-bit address space rule).
    struct Cand {
        uint32_t srcIdx; uint64_t key; VobLightInfo* light; float distSq;
        bool active; bool wantsDynamic; bool restrictToWorld; bool forced;
    };
    std::vector<Cand> m_Cands;
    // What each light WANTS, parallel to m_Assignments, before the frame budget decides what it gets.
    struct Want { bool wantStatic; bool wantDyn; uint32_t prio; };
    std::vector<Want> m_Wants;
    // Indices into m_Assignments for the lights that want work, bucketed by importance. m_Forced is served
    // ahead of all three and is not capped.
    std::vector<uint32_t> m_Near, m_Mid, m_Far, m_Forced;
    std::unordered_map<uint64_t, int32_t> m_EncodedByKey;
    // key -> occupied slot, rebuilt from the tables each Select and then maintained by hand wherever a slot
    // changes hands. Without it the incumbency lookups are a per-frame O(slots*lights) scan.
    std::unordered_map<uint64_t, uint32_t> m_StaticByKey;
    std::unordered_map<uint64_t, uint32_t> m_DynByKey;
    gtl::flat_hash_set<uint64_t> m_FrameKeys;

    // The stabilized cube range, keyed by vob and swept rarely. It used to live in each backend separately
    // (D3D11PointLight::UpdateShadowRange / D3D12Scene's s_stationary) and had to agree for the two to match.
    struct RangeState {
        uint32_t lastSeen = 0;
        float    shadowRange = 0.0f;
    };
    gtl::flat_hash_map<const zCVob*, RangeState> m_Stationary;
    uint32_t m_SweepFrame = 0;

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
