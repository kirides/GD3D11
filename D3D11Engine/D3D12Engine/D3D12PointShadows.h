#pragma once
// Point-light shadow cubes for the D3D12 backend - the resources and the draws only. Every DECISION (which
// lights get cubes, in which slots, what is re-rendered this frame, what is sampleable) belongs to
// PointLightSlotSelector, shared verbatim with D3D11 - see POINTLIGHT_TWO_TIER_PLAN.md.
//
// TWO INDEPENDENT TIERS, two independent slot spaces. A light's depth lives in a STATIC core cube (world mesh
// + static VOBs + static MOBs, baked once and cached) and, when it holds one of the much scarcer overlay
// slots, in a DYNAMIC cube (only this frame's movers); the lit pass mins the two, which is "occluded by
// either". NORMAL-Z (clear 1.0, LESS_EQUAL) like the CSM. Faces render single-pass via an instanced layered VS
// (6 instances -> the 6 faces via SV_RenderTargetArrayIndex; needs
// VPAndRTArrayIndexFromAnyShaderFeedingRasterizer). Three phases:
//   A) STATIC   - for slots whose bake is stale and that the frame budget served, (re)render the static
//                 casters into m_StaticCube. Usually a no-op: this is a cache.
//   C) DYNAMIC  - clear this slot's m_DynCube face-set and draw the movers into it. Only for slots that
//                 actually HAVE casters in range this frame.
//   D) hand the touched slots of both arrays back to PIXEL_SHADER_RESOURCE for the lit pass.
// There is no phase B: it used to copy each slot's 6 static faces into the active cube so the overlay could
// be composited on top, 6 CopyTextureRegion + 18 barriers per light every frame. Two arrays and a min()
// remove both that and the wipe it doubled as.
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
    // Two independent tiers, two independent slot spaces - see POINTLIGHT_TWO_TIER_PLAN.md.
    //
    //     static: 340 slots @64^2 = 16.7 MB      dynamic: 64 slots @128^2 = 12.6 MB
    //
    // 340 is the hard ceiling of ONE array: a cube array is a Texture2DArray of slot*6 slices, and D3D12 caps
    // that at D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION (2048) - 2048/6 = 341. Past it resource creation fails
    // outright, so more cubes would need MORE ARRAYS. Raise this only with an occupancy measurement in hand.
    static constexpr UINT kStaticCubeSize = 64;
    static constexpr UINT kMaxStaticCubes = 340;
    static constexpr UINT kDynCubeSize = 128;
    static constexpr UINT kMaxDynCubes = 64;
    // Asserted for BOTH so raising either can never silently produce an undersized/failed resource.
    static_assert( kMaxStaticCubes * 6 <= 2048, "static cube array exceeds D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION" );
    static_assert( kMaxDynCubes * 6 <= 2048, "dynamic cube array exceeds D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION" );

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

    UINT GetStaticSrvSlot() const { return m_StaticSrvSlot; }   // bindless heap slot of the static core cube array
    UINT GetDynSrvSlot() const { return m_DynSrvSlot; }         // bindless heap slot of the overlay cube array
    bool IsPassReady() const { return m_PassReady; }

    // Exposed so the static-light CLUSTERING pass in D3D12Scene can redirect co-located members onto one
    // shared cube before Select() runs; nothing else may touch it.
    std::vector<PointLightSlotSelector::Candidate>& GetCandidates() { return m_Candidates; }
    void BuildCandidates();

    /** Decides this frame's cubes and writes each light's ShadowCubeIndex back into the GPU light buffer.
        `keys[i]` is the ownership identity of light i - its own vob, or the cluster it was redirected onto. */
    void SelectShadowedLights( GPULight* lights, UINT count, const std::vector<uint64_t>& keys );

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

    // --- STATIC (core) cube array. Baked once per light and cached; nothing is ever composited into it, so
    // its depth stays valid for as long as StaticSlot::valid says it does.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_StaticCube;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_StaticCubeAlloc;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_StaticDsvHeap;
    UINT m_DsvSize = 0;
    UINT m_StaticSrvSlot = UINT_MAX;   // R16_UNORM TextureCubeArray SRV, fetched bindlessly

    // --- DYNAMIC overlay cube array: ONLY this frame's moving casters, never a composite. A slot is cleared
    // and redrawn only on a frame that actually has movers in range; a light with no NPC nearby carries a zero
    // HI half and the shader never reads this array for it.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_DynCube;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_DynCubeAlloc;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_DynDsvHeap;
    UINT m_DynSrvSlot = UINT_MAX;

    // PER-SLOT resource state (a slot's DSV views exactly its 6 faces). An ALL_SUBRESOURCES transition out of
    // DEPTH_WRITE makes the driver resummarize depth metadata for EVERY slice whether the frame touched 5
    // slots or 300, so EVERY barrier on these two resources must be per-subresource.
    D3D12_RESOURCE_STATES m_StaticSlotState[kMaxStaticCubes] = {};   // static core cube; PSR at rest
    D3D12_RESOURCE_STATES m_DynSlotState[kMaxDynCubes] = {};         // overlay cube; PSR at rest

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

    // Every slot decision - the dome, ownership, eviction, the importance buckets, the frame budget, the
    // static-bake cache - lives in PointLightSlotSelector, shared verbatim with D3D11. This class owns only
    // the resources and the draws.
    PointLightSlotSelector m_Sel;
    using FrameLight = PointLightSlotSelector::Assignment;
    // Kept across frames so its capacity is reused (32-bit per-frame allocation rule).
    std::vector<PointLightSlotSelector::Candidate> m_Candidates;

    // Slots whose static target was (re)rendered by THIS frame's records, pending the CommitStaticCache() that
    // turns them into cache hits. Kept out of Slot so an uncommitted frame simply leaves staticValid false and
    // the slot retries next frame. Capacity is retained across frames (frame-path allocation rule).
    struct PendingStatic { UINT slot; };   // static-tier slot
    std::vector<PendingStatic> m_PendingStatic;
    // Same deal for the dynamic side: `has` is what Slot::dynamicValid becomes once the overlay is known to have
    // been recorded AND submitted. Only slots whose overlay was actually SCHEDULED this frame appear here, so an
    // unscheduled round-robin slot keeps whatever it had.
    struct PendingDynamic { UINT slot; bool has; };   // overlay-tier slot
    std::vector<PendingDynamic> m_PendingDynamic;

    bool m_PassReady = false;
    // Both cube arrays are born with undefined depth; Record() clears every slot once - see the note there.
    bool m_NeedsInitialClear = true;
};
