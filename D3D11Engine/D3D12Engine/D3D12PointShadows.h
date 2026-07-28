#pragma once
// Point-light shadow cubes for the D3D12 backend (P2.10) — extracted out of the engine monolith.
// Mirrors D3D11's shared TextureCubeArray Forward+ path: up to kMaxCubes shadowed point lights, each a 6-face
// 128^2 slot in one array. NORMAL-Z (clear 1.0, LESS_EQUAL) like the CSM. Faces are rendered single-pass via
// an instanced layered VS (6 instances -> the 6 faces via SV_RenderTargetArrayIndex, no geometry shader —
// needs VPAndRTArrayIndexFromAnyShaderFeedingRasterizer). Sampled in the tiled point-light loop when the
// light's ShadowCubeIndex >= 0.
//
// Static/dynamic split (P2.10g), the D3D11 static-aside model: per shadowed light the ACTIVE cube is built as
// (cached static-only depth) + (this frame's dynamic casters overlaid), so hundreds of shadowed lights can
// update their MOVING casters every frame without re-rendering static geometry. Three phases, plus a
// hand-back:
//   A) STATIC   — for slots whose light is fresh / moved / resized, (re)render the static casters
//                 (world mesh + instanced VOBs). Amortized: usually a no-op.
//   B) COPY     — per overlay-eligible slot, copy its 6 static-aside faces into the active cube.
//   C) DYNAMIC  — overlay the moving casters (skeletal NPCs + their node attachments) on top.
//   D) hand the touched slots back to PIXEL_SHADER_RESOURCE for the lit pass.
// A light that can NEVER receive an overlay (a static light, or ANY light at PLS_STATIC_ONLY) skips the aside
// cube and the copy entirely — its static casters render straight into the active cube and persist there.
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

class D3D12GraphicsEngine;
class zCVobLight;
class zCVob;
struct zTBBox3D;

class D3D12PointShadows {
public:
    static constexpr UINT kCubeSize = 128;
    static constexpr UINT kMaxCubes = 128;   // matches D3D11's persisted-light ceiling; each = 6 slices @128^2 R16
    static constexpr UINT kBackBufferCount = 2;   // must match D3D12GraphicsEngine::kBackBufferCount (asserted in the .cpp)

    // The engine back-reference, handed over in the engine's CONSTRUCTOR so it is valid before Init() runs
    // (mirrors D3D12ShadowMap::Attach — see the note there).
    void Attach( D3D12GraphicsEngine& engine ) { m_E = &engine; }

    // Both cube arrays + their per-slot DSVs + the array SRV + the per-frame face-CB / VOB-instance rings. The
    // caster PIPELINES live in m_Pipelines.PointShadow (D3D12PipelineState::CreatePointShadow). Non-fatal at
    // init: on failure the point lights simply stay unshadowed.
    bool Init();

    UINT GetSrvSlot() const { return m_SrvSlot; }
    bool IsPassReady() const { return m_PassReady; }

    // Picks this frame's shadowed lights out of the filled GPU light buffer and writes each winner's
    // ShadowCubeIndex back into it. Called from BuildFrameLightBuffer once the buffer is populated;
    // `lightVobs` is parallel to `lights` (the owning light Vob per GPULight index) and supplies the identity
    // that keys stable per-light slot ownership.
    void SelectShadowedLights( GPULight* lights, UINT count, const std::vector<zCVobLight*>& lightVobs );

    void Prepare();
    void Record( ID3D12GraphicsCommandList* cmdList );

    // Static-cube cache invalidation on world changes. The static depth is only re-rendered when the light is
    // fresh / moved / resized — not when geometry around it changes — so a VOB appearing or disappearing inside
    // a cached slot's light sphere must force a one-time static re-render next frame. Over-invalidation is
    // harmless (one extra static pass); under-invalidation freezes a stale shadow in the cache.
    void InvalidateStaticForVobAdded( const DirectX::XMFLOAT3& posWS, float extent );
    void InvalidateStaticForVobRemoved( const zTBBox3D& bbox );

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
    D3D12_RESOURCE_STATES m_ActiveSlotState[kMaxCubes] = {};   // active cube; PIXEL_SHADER_RESOURCE at rest
    D3D12_RESOURCE_STATES m_StaticSlotState[kMaxCubes] = {};   // static-aside cube; DEPTH_WRITE at rest

    // Static-aside cube: second persistent cube array, static-caster depth only. No SRV (never sampled); the
    // slots routed through it are copied into the active cube each frame. Only slots that can receive a dynamic
    // overlay live here at all — see Slot::usesAside.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_StaticCube;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_StaticCubeAlloc;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_StaticDsvHeap;   // one D16 6-slice DSV per slot (mirrors active)

    // Per-frame ring of the 6-face view-proj CB, one 512-aligned slot per shadowed light (bound as root CBV b0).
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_FaceCB[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_FaceCBAlloc[kBackBufferCount];
    uint8_t*                  m_FaceCBMapped[kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_FaceCBGpu[kBackBufferCount] = {};

    // Per-frame TIGHT (64-byte world matrix) VOB-instance ring for the point-shadow VOB caster: only the
    // instances range-culled into a shadowed light's sphere get packed here, so cube draws stay proportional to
    // nearby casters. Persistently mapped UPLOAD; offset reset at the top of Prepare(); drop+log on overflow
    // (never reallocates — see the 32-bit per-frame-allocation rule).
    static constexpr UINT kMaxVobInstances = 8192;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_VobInst[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobInstAlloc[kBackBufferCount];
    uint8_t*                  m_VobInstPtr[kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_VobInstGpu[kBackBufferCount] = {};
    UINT m_VobInstCapacity = 0;   // bytes
    UINT m_VobInstOffset = 0;     // reset each frame at the top of Prepare()
    bool m_VobInstOverflowLogged = false;

    // Slots are owned by light Vob identity and kept stable across frames (not reassigned by proximity), so a
    // static winner whose light didn't move reuses its cached cube instead of re-culling + re-rendering.
    struct Slot {
        zCVobLight*       owner = nullptr;   // light identity owning this slot (nullptr = free)
        DirectX::XMFLOAT3 pos = {};          // last static-rendered light position (move detection)
        float             range = 0.0f;      // last static-rendered range (range-change detection)
        bool              isStatic = false;  // Vob->IsStatic(): gates Prepare() — static lights get a
                                             // world-mesh-only static-aside cache (no VOB casters) and never
                                             // receive the per-frame skeletal dynamic overlay (mirrors D3D11's
                                             // GetCurrentShadowMode forcing PLS_STATIC_ONLY for static lights).
        bool              staticValid = false; // the slot's CURRENT static target (aside if usesAside, else the active
                                             // cube itself) holds valid static-only depth; false => must re-render static
        bool              usesAside = false; // last frame's routing for this slot: true = static goes to the aside cube
                                             // and is copied into active each frame (the slot can receive a dynamic
                                             // overlay); false = static is rendered DIRECTLY into the active cube and
                                             // never copied (no overlay possible — static light, or PLS_STATIC_ONLY).
                                             // A change here invalidates staticValid: the depth lives in the other cube.
        UINT32            dynamicStaleFrames = 0; // frames since this slot's skeletal dynamic overlay last ran (round-robin, P2.10h)
    };
    Slot m_Slots[kMaxCubes];

    // The shadowed lights chosen this frame — filled by SelectShadowedLights, consumed by Prepare().
    // renderStatic: also (re)render the STATIC casters into this slot's static target (fresh slot / light moved
    // / range changed / routing flipped); otherwise the cached static depth is reused.
    // renderDynamic (round-robin, P2.10h): whether the per-frame skeletal overlay runs THIS frame for this
    // winner — always for the closest kAlwaysDynamicCount, round-robined (oldest-serviced-first) for the rest,
    // so a large persisted-light count doesn't multiply the CPU cost of sphere-culling the full skeletal-vob
    // list against every shadowed light every frame.
    // overlayEligible: whether this light can EVER receive a dynamic overlay (not a static light, and the
    // global PointlightShadows setting is >= PLS_UPDATE_DYNAMIC).
    struct FrameLight { DirectX::XMFLOAT3 posWS; float range; UINT slot; bool renderStatic; bool renderDynamic; bool overlayEligible; };
    std::vector<FrameLight> m_FrameLights;

    bool m_PassReady = false;
};
