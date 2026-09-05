#pragma once
// Point-light shadow cubes for the D3D12 backend (P2.10) — extracted out of the engine monolith.
// Mirrors D3D11's shared TextureCubeArray Forward+ path: up to kMaxCubes shadowed point lights, each a 6-face
// 128^2 slot in one array. NORMAL-Z (clear 1.0, LESS_EQUAL) like the CSM. Faces are rendered single-pass via
// an instanced layered VS (6 instances -> the 6 faces via SV_RenderTargetArrayIndex, no geometry shader —
// needs VPAndRTArrayIndexFromAnyShaderFeedingRasterizer). Sampled in the tiled point-light loop when the
// light's ShadowCubeIndex >= 0.
//
// Static/dynamic split: a shadowed light's depth lives in TWO cube arrays that the lit pass samples and mins —
// m_Cube (cached static-only depth) and m_DynCube (this frame's moving casters). Hundreds of shadowed lights
// can therefore update their MOVING casters every frame without re-rendering static geometry. Three phases:
//   A) STATIC   — for slots whose light is fresh / moved / resized, (re)render the static casters
//                 (world mesh + instanced VOBs + MOB bodies/attachments) into m_Cube. Usually a no-op.
//   C) DYNAMIC  — clear this slot's m_DynCube face-set and draw the moving casters (skeletal NPCs + their node
//                 attachments) into it. Only for slots that actually HAVE casters in range this frame.
//   D) hand the touched slots back to PIXEL_SHADER_RESOURCE for the lit pass.
// There is no phase B. It used to copy each overlay-eligible slot's 6 static faces into the active cube so the
// overlay could be composited on top — 6 CopyTextureRegion + 18 barriers per light EVERY frame, including for
// lights with no dynamic caster near them, because the copy doubled as the "wipe last frame's overlay" step.
// Two arrays and a min() in SamplePointShadow removes both jobs: nothing to composite, so nothing to copy, and
// a light that loses its casters just stops setting kShadowHasDynamic instead of needing to be wiped.
// A light that can NEVER receive an overlay (a static light, or ANY light at PLS_STATIC_ONLY) simply never
// carries that bit, so the lit pass reads only its static cube and phase C never touches it.
//
// Split for deferred recording like the CSM: Prepare() does every Gothic-touching step on the main thread
// (range culls, CacheIn, animation/texani, the face-CB and VOB-instance ring writes) and flattens the result
// into pure-D3D12 draw records; Record() issues them into whichever command list it is handed, on a pool
// thread or on the main list when the pool path is unavailable.
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <D3D12MemAlloc.h>
#include <DirectXMath.h>

#include "D3D12EngineCommon.h"
#include "D3D12StateCache.h"
#include "../PointLightSlotSelector.h"

class D3D12GraphicsEngine;
class zCVobLight;
class zCVob;
struct zTBBox3D;

class D3D12PointShadows {
public:
    static constexpr UINT kCubeSize = 128;
    static constexpr UINT kMaxCubes = 64;   // matches D3D11's persisted-light ceiling; each = 6 slices @128^2 R16

    // ---- Low-resolution STATIC tier -------------------------------------------------------------------------
    // Static lights (and the clusters they are merged into) get their own, much coarser cube array. A cube-less
    // point light is shaded UNSHADOWED, and BuildFrameLightBuffer then clamps its range hard so it cannot bleed
    // through walls - which makes losing a slot visible as the light popping between its full and its clamped
    // radius. So the slot count decides how many of a room's fill lights look right at all. But a static
    // light's cube is rendered ONCE and cached forever, and only exists to stop room-to-room bleed, so it does
    // not need 128^2:
    //     dynamic tier: 64 slots @128^2 = 12.6 MB (x2: static + overlay)   static tier: 340 slots @32^2 = 4.2 MB
    //
    // 340 slots is the hard ceiling of ONE array, and more than any Gothic scene has been measured to need
    // (~200 static, ~64 moving): a cube array is a Texture2DArray of slot*6 slices, and D3D12 caps that at
    // D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION (2048) slices - 2048/6 = 341. Going past it fails resource
    // creation outright, so more cubes would need MORE ARRAYS, not a bigger one. Paging was built once and
    // removed again as unused complexity; raise this only with an occupancy measurement in hand.
    //
    // The two pools share ONE slot index space so all the ownership/retention bookkeeping below stays single-pool:
    // global slot < kMaxCubes is the dynamic array, >= kMaxCubes is the static array at (slot - kMaxCubes). What
    // reaches the SHADER is the re-encoded GPULight::ShadowCubeIndex (kShadowTierLow | localIndex), not this.
    static constexpr UINT kStaticCubeSize = 32;
    static constexpr UINT kMaxStaticCubes = 340;
    static constexpr UINT kMaxSlots = kMaxCubes + kMaxStaticCubes;
    // Both tiers are Texture2DArrays of slot*6 slices - see the ceiling note above. Asserted for BOTH so raising
    // either one can never silently produce an undersized/failed resource again.
    static_assert( kMaxCubes * 6 <= 2048, "dynamic cube array exceeds D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION" );
    static_assert( kMaxStaticCubes * 6 <= 2048, "static cube array exceeds D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION" );
    static bool IsLowSlot( UINT globalSlot ) { return globalSlot >= kMaxCubes; }
    static UINT LowIndex( UINT globalSlot ) { return globalSlot - kMaxCubes; }

    static constexpr UINT kBackBufferMax = 3;   // must match D3D12GraphicsEngine::kBackBufferMax (asserted in the .cpp)
    UINT kBackBufferCount = 2;   // synced from the engine in Attach() below

    // The engine back-reference, handed over in the engine's CONSTRUCTOR so it is valid before Init() runs
    // (mirrors D3D12ShadowMap::Attach — see the note there). Defined in the .cpp: D3D12GraphicsEngine is
    // only forward-declared here, so reading engine.kBackBufferCount needs the complete type.
    void Attach( D3D12GraphicsEngine& engine );

    // Both cube arrays + their per-slot DSVs + the array SRV + the per-frame face-CB / VOB-instance rings. The
    // caster PIPELINES live in m_Pipelines.PointShadow (D3D12PipelineState::CreatePointShadow). Non-fatal at
    // init: on failure the point lights simply stay unshadowed.
    bool Init();

    UINT GetSrvSlot() const { return m_SrvSlot; }
    UINT GetLowSrvSlot() const { return m_LowSrvSlot; }   // bindless heap slot of the low-res static cube array
    UINT GetDynSrvSlot() const { return m_DynSrvSlot; }   // bindless heap slot of the dynamic-overlay cube array
    bool IsPassReady() const { return m_PassReady; }

    // Slot-ownership identity for one entry of the GPU light buffer, parallel to `lights` in
    // SelectShadowedLights. Several lights may share a `key`: that is exactly how a CLUSTER of co-located static
    // lights ends up sharing one cube — they are one candidate for slot selection, and every member gets the
    // winner's ShadowCubeIndex written back.
    struct LightShadowKey {
        uint64_t    key;      // 0 = never shadowed. Else the light's own vob pointer, or a tagged cluster cell id.
        zCVobLight* vob;      // nullptr for clusters — only ever dereferenced for the dynamic overlay's exclude
                              // list, which a static/clustered (low-tier) slot never reaches.
        bool        lowRes;   // PREFERRED tier: take the slot from the low-res static pool
        bool        isStatic;
        bool        spatiallyStatic;   // never moves; only such a candidate may SPILL into the low-res tier,
                                       // which bakes once and would be starved by a mover re-entering its
                                       // per-frame render budget every frame.
        bool        restrictToWorld;   // world-mesh-only casters, mirroring D3D11's AllowsDynamicCasters PFX
                                       // gate: BuildExcludeList never excludes such a light's own carrier, so
                                       // it would otherwise throw a large self-shadow from whatever holds it.
    };

    // Picks this frame's shadowed lights out of the filled GPU light buffer and writes each winner's
    // ShadowCubeIndex back into it. Called from BuildFrameLightBuffer once the buffer is populated. Candidates
    // are ranked on GPULight::ShadowOrigin/ShadowRange (the CUBE's placement, which for a clustered static light
    // is its cluster's, not its own), so one cube is selected per cluster rather than per member light.
    void SelectShadowedLights( GPULight* lights, UINT count, const std::vector<LightShadowKey>& keys );

    void Prepare();
    void Record( D3D12CmdList& cmdList );

    // Commits the static-cube cache stamps Prepare() resolved this frame. MUST be called only once the pass is
    // known to have been recorded AND submitted (end of FinishShadowPasses) — never from Prepare().
    // `staticValid` means "the slot's static target physically holds this light's static depth"; stamping it at
    // resolve time made it mean "we intended to draw it", and any frame that resolved a static (re)render but
    // never issued it — the pass bailing on !m_FrameOpen, a list that failed to record and re-issue — left the
    // slot marked cached while its target held nothing but the Phase-A clear. Depth 1.0 sampled LESS_EQUAL is
    // "nothing occludes", so the light silently lost its static shadow FOREVER (nothing re-renders a slot that
    // believes it is cached). Aside-routed slots showed it worst: Phase B re-lays that empty base under the
    // dynamic overlay every single frame, so their static never came back at all.
    void CommitStaticCache();

    // Static-cube cache invalidation on world changes. The static depth is only re-rendered when the light is
    // fresh / moved / resized — not when geometry around it changes — so a VOB appearing or disappearing inside
    // a cached slot's light sphere must force a one-time static re-render next frame. Over-invalidation is
    // harmless (one extra static pass); under-invalidation freezes a stale shadow in the cache.
    /** True if this vob rides an NPC's transform. Such a caster is never baked into a cached static cube,
        so its comings and goings must not invalidate one. Mirrors D3D11's IsAttachedToNpc. */
    static bool IsNpcAttached( const zCVob* vob );

    /** Park a vob that entered the world or moved; the invalidation happens at the top of the next
        SelectShadowedLights. Deferred because neither the position nor the NPC parent link is settled when
        Gothic reports the change - a dropped item is inserted, re-parented and then falls for several
        frames. Entries whose vob left the world meanwhile are dropped. */
    void QueueVobChangedInvalidation( zCVob* vob );

    void InvalidateStaticForVobAdded( const DirectX::XMFLOAT3& posWS, float extent );
    // Matched by POINTER against Slot::bakedVobs - the vob may already be half torn down here, so it is
    // never dereferenced.
    void InvalidateStaticForVobRemoved( const zCVob* vob );

    // Skeletal-caster scratch lists for the per-light sphere cull. PrepareFrameSkeletals(..., shadowCascade=-2)
    // routes into these: each shadowed light sphere-culls the FULL registered skeletal-vob list against itself
    // (a caster invisible to the player, but inside a torch's range, still casts into it), refilled once per
    // light inside Prepare()'s dynamic-overlay phase. Public because the collector lives in the engine.
    std::vector<FrameSkelDraw>   SkelScratch;
    std::vector<FrameAttachDraw> AttachScratch;

private:
    // Self-shadow exclusion for point lights attached to a carried item/NPC — without this, e.g. a torch light
    // held in an NPC's hand casts a huge shadow blob from that NPC's own body onto itself. Mirrors D3D11's
    // GetHasOriginVob + SetupVobsToExclude/CollectVobTreeToExclude (D3D11PointLight.cpp). Populates excludeOut
    // with the light vob's ancestor chain (+ any oCVisualFX origin) and returns true when non-empty; returns
    // false (excludeOut left empty) when self-shadowing is allowed, the light isn't attached to a carried item,
    // or it's a PFX-spawned light (those aren't excluded, matching D3D11's GetHasOriginVob gate).
    bool BuildExcludeList( zCVobLight* lightVob, std::vector<const zCVob*>& excludeOut );

    D3D12GraphicsEngine* m_E = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource>       m_Cube;        // Texture2DArray(R16_TYPELESS), kMaxCubes*6 slices
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_CubeAlloc;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_DsvHeap;     // one D16 Texture2DArray DSV (6 slices) per cube slot
    UINT m_DsvSize = 0;
    UINT m_SrvSlot = UINT_MAX;   // R16_UNORM TextureCubeArray SRV (all cubes), for the point-light sampler

    // PER-SLOT resource state for both cube arrays (6 subresources per slot move together — a slot's DSV views
    // exactly its 6 faces). These MUST stay per-slot rather than one whole-resource state: an ALL_SUBRESOURCES
    // transition out of DEPTH_WRITE makes the driver decompress/resummarize depth metadata for every one of the
    // kMaxCubes*6 = 768 slices (24 MB) whether the frame touched 5 slots or 128 — a ~0.6 ms FIXED cost that
    // dwarfed the 192 KB/slot copies it was guarding and did not scale down with the light count. Scoping each
    // barrier to the 6 subresources actually being copied makes the cost proportional to the work done.
    // Consequence: EVERY barrier on these two resources must be per-subresource. A single ALL_SUBRESOURCES
    // transition asserts all 768 slices share one state, which they no longer do.
    D3D12_RESOURCE_STATES m_ActiveSlotState[kMaxCubes] = {};   // static cube; PIXEL_SHADER_RESOURCE at rest
    D3D12_RESOURCE_STATES m_DynSlotState[kMaxCubes] = {};      // dynamic-overlay cube; PIXEL_SHADER_RESOURCE at rest

    // ---- Low-res static cube array (kStaticCubeSize^2, kMaxStaticCubes slots) -------------------------------
    // ONE array, no aside twin: everything routed here is a static light or a static cluster, and those are never
    // overlay-eligible (D3D11's GetCurrentShadowMode forces static lights to PLS_STATIC_ONLY). Their static depth
    // therefore renders straight into this array and stays there — no per-frame copy, no dynamic overlay. Phases
    // B and C skip these slots entirely; only Phase A (on a cache miss) and Phase D ever touch them.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_LowCube;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_LowCubeAlloc;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_LowDsvHeap;   // one D16 6-slice DSV per low-res slot
    UINT m_LowSrvSlot = UINT_MAX;                                // R16_UNORM TextureCubeArray SRV, fetched bindlessly
    D3D12_RESOURCE_STATES m_LowSlotState[kMaxStaticCubes] = {};  // PIXEL_SHADER_RESOURCE at rest, per-slot like above

    // Dynamic-overlay cube: second persistent full-res cube array holding ONLY this frame's skeletal overlay
    // depth, never a composite. m_Cube above holds the static base; the lit pass samples BOTH and takes the min
    // (see SamplePointShadow), which is what removed the old per-slot static->active copy: 6 CopyTextureRegion
    // + 18 barriers per overlay-eligible light EVERY frame, which is what the main thread used to block on.
    // A slot here is cleared and redrawn only on a frame that actually has dynamic casters in range; a light
    // with no NPC nearby simply doesn't set kShadowHasDynamic and the shader never reads this array for it.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_DynCube;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_DynCubeAlloc;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_DynDsvHeap;   // one D16 6-slice DSV per slot (mirrors the static one)
    UINT m_DynSrvSlot = UINT_MAX;   // R16_UNORM TextureCubeArray SRV, fetched bindlessly via LightCB's PointShadowDynIndex

    // Per-frame ring of the 6-face view-proj CB, one 512-aligned slot per shadowed light (bound as root CBV b0).
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_FaceCB[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_FaceCBAlloc[kBackBufferMax];
    uint8_t*                  m_FaceCBMapped[kBackBufferMax] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_FaceCBGpu[kBackBufferMax] = {};

    // Per-frame TIGHT (64-byte world matrix) VOB-instance ring for the point-shadow VOB caster: only the
    // instances range-culled into a shadowed light's sphere get packed here, so cube draws stay proportional to
    // nearby casters. Persistently mapped UPLOAD; offset reset at the top of Prepare(); drop+log on overflow
    // (never reallocates — see the 32-bit per-frame-allocation rule).
    static constexpr UINT kMaxVobInstances = 8192;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_VobInst[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobInstAlloc[kBackBufferMax];
    uint8_t*                  m_VobInstPtr[kBackBufferMax] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_VobInstGpu[kBackBufferMax] = {};
    UINT m_VobInstCapacity = 0;   // bytes
    UINT m_VobInstOffset = 0;     // reset each frame at the top of Prepare()
    bool m_VobInstOverflowLogged = false;

    // Every slot decision - ownership, retention, eviction, tier spill/promotion, cache stamps, the dynamic
    // round-robin - lives in PointLightSlotSelector, shared verbatim with D3D11. This class owns only the
    // resources and the draws.
    PointLightSlotSelector m_Sel;
    using Slot = PointLightSlotSelector::Slot;
    using FrameLight = PointLightSlotSelector::Assignment;

    // Slots whose static target was (re)rendered by THIS frame's records, pending the CommitStaticCache() that
    // turns them into cache hits. Kept out of Slot so an uncommitted frame simply leaves staticValid false and
    // the slot retries next frame. Capacity is retained across frames (frame-path allocation rule).
    struct PendingStatic { UINT slot; };
    std::vector<PendingStatic> m_PendingStatic;
    // Same deal for the dynamic side: `has` is what Slot::dynamicValid becomes once the overlay is known to have
    // been recorded AND submitted. Only slots whose overlay was actually SCHEDULED this frame appear here, so an
    // unscheduled round-robin slot keeps whatever it had.
    struct PendingDynamic { UINT slot; bool has; };
    std::vector<PendingDynamic> m_PendingDynamic;

    bool m_PassReady = false;
};
