#pragma once
// CSM sun shadows for the D3D12 backend (P2.9c) — extracted out of the engine monolith.
//
// Directional shadow map = a Texture2DArray, one D32 slice per cascade, R32_TYPELESS so each slice serves a
// D32_FLOAT DSV and the whole array serves one R32_FLOAT SRV for the lit pass's PCF sampling. NORMAL-Z here
// (clear 1.0, LESS_EQUAL) — NOT reversed-Z like the main camera (mirrors the D3D11 caster).
//
// The pass is split into four phases so the two expensive per-cascade jobs can run concurrently (plan item
// #7); the engine's shadow driver (PrepareShadowPasses / BeginShadowRecording / FinishShadowPasses) is what
// sequences them against the rest of the frame:
//   A) Prepare()             main thread: cascade matrices, the sampling CB, everything identical across
//                            cascades (the world-mesh caster set + its bindless materials, the grass wind CB)
//                            plus the ONE Gothic-mutating step that cannot be per-cascade — the near-cascade
//                            skeletal caster walk (see kSkeletalShadowCascades). Then LAUNCHES one job per
//                            cascade on the worker pool.
//   B) CullCascade(c)        concurrent: frustum tests + CollectVisibleVobs + grass box cull. Touches only
//                            cascade c's own state and read-only engine data.
//   C) BuildCascade(c)       concurrent: cascade c's VOB instance upload (into its OWN sub-range of the shadow
//                            instance ring, so there is no shared cursor) + its indirect-arg build.
//   D) RecordCascade(c,list) concurrent: the actual draws, into one command list per cascade.
// B, C and D run back-to-back as a SINGLE job per cascade, so recording starts the moment that cascade's cull
// finishes instead of waiting for the main thread to reach the join. The join itself stays where it always
// was — immediately before the lit geometry pass, which samples these cascades — but by then the work is done.
// Everything degrades to a serial in-order run when RendererSettings.ThreadedShadowCulling is off (or the
// per-slot command lists don't exist) — same output, same queue order, just no overlap.
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <D3D12MemAlloc.h>
#include <DirectXMath.h>

#include "../Frustum.h"
#include "D3D12EngineCommon.h"
#include "D3D12StateCache.h"
#include "InstancingUtils.h"   // RenderView — the per-cascade visible-VOB collection target

class D3D12GraphicsEngine;
class GVegetationBox;
class zCTexture;

// Cascade count. Free-standing (not a class member) because the shared skeletal collector in D3D12Scene.cpp
// sizes its multi-cascade mode against it.
inline constexpr UINT kShadowCascades = 3;

// How many of the NEAR cascades get skeletal (NPC/monster) casters. The skeletal caster walk is the one part
// of the cascade preparation that mutates Gothic state (animation update, texani, morph meshes) and writes the
// shared skeletal CB ring, so it cannot live inside the concurrent per-cascade job — it runs once on the main
// thread in Prepare(). Restricting it to the near cascade(s) keeps that walk cheap AND lets the far cascades
// skip RecordCascade's per-mesh skeletal/attachment draw loops entirely, which is most of their record cost.
// An NPC only shadows itself and its immediate surroundings at any believable size; by cascade 1+ the caster
// is a couple of texels. Raise to 2 if NPC shadows visibly pop at the cascade boundary.
inline constexpr UINT kSkeletalShadowCascades = 1;
static_assert( kSkeletalShadowCascades <= kShadowCascades );

class D3D12ShadowMap {
public:
    // Compile-time array bound of the per-cascade indirect-arg buffers; must match
    // D3D12GraphicsEngine::kBackBufferMax (static_assert'd in the .cpp).
    static constexpr UINT kBackBufferMax = 3;
    UINT kBackBufferCount = 2;   // synced from the engine in Attach() below

    // The engine back-reference, handed over in the engine's CONSTRUCTOR. Deliberately separate from Init():
    // the engine calls CreateWorldArgRings/CreateVobArgRings from its own indirect-command setup, which runs
    // BEFORE Init() (whose caster PSOs depend on the depth-prepass shader blobs existing first). Defined in
    // the .cpp: D3D12GraphicsEngine is only forward-declared here, so reading engine.kBackBufferCount needs
    // the complete type.
    void Attach( D3D12GraphicsEngine& engine );

    // The shadow map itself + its caster PSOs, created once. Fails soft: on failure the engine simply renders
    // without sun shadows (Prepare() guards on the resources existing) instead of falling back to D3D11.
    bool Init();
    // Grass caster PSO. Separate from Init() because it reuses m_Pipelines.Grass.RootSig, so it can only be
    // built after D3D12PipelineState::CreateGrass() — non-fatal (grass just casts no shadow).
    bool CreateGrassCaster();
    // Per-cascade ExecuteIndirect argument rings. Called from the engine's CreateWorldIndirect/CreateVobIndirect
    // so the command-signature layout and the ring layout stay defined in one place each.
    bool CreateWorldArgRings( const D3D12_RESOURCE_DESC& bufferDesc );
    bool CreateVobArgRings( UINT commandStride );

    // Live resolution change (settings): snap the request to the shared step set, then recreate the texture in
    // place (the DSV heap + SRV slot are resolution-independent and reused).
    static UINT ClampSize( int desired );   // {512,1024,2048,4096,8192} — matches D3D11's ImGui combo
    bool Resize( UINT newSize );
    bool HasResources() const { return m_DsvHeap != nullptr; }
    UINT GetSize() const { return m_MapSize; }
    UINT GetSrvSlot() const { return m_SrvSlot; }

    // ---- The four phases (see the header comment). B/C/D run back-to-back inside ONE job per cascade. ----
    void Prepare();
    void CullCascade( UINT cascade );
    void BuildCascade( UINT cascade );
    void RecordCascade( UINT cascade, D3D12CmdList& cmdList, bool sunUp );
    // The single join point for the concurrent per-cascade jobs (mirrors D3D11ShadowMap::WaitShadowCullingComplete).
    void WaitCascadeJobs();
    // True when this frame's cascades recorded into their OWN command lists inside their jobs. False whenever
    // no job could do that (threading off, sun down, per-slot lists missing, or Prepare() bailed), in which
    // case FinishShadowPasses still has to emit each cascade's draws inline on the main command list.
    bool RecordedInJob() const { return m_RecordedInJob; }
    // Hand the cascade array to PIXEL_SHADER_RESOURCE for the lit-pass sampling (reverted at the top of the
    // next Prepare()). Issued on the main command list, after every cascade list has been executed.
    void TransitionToReadState( D3D12CmdList& cmdList );

    bool IsPassReady() const { return m_PassReady; }
    bool IsSunUp() const { return m_SunUp; }
    // False for a cascade that is being re-used from an earlier frame this frame (lazy update, see
    // m_ShouldUpdateCascade). Such a cascade is neither culled, built nor recorded — its slice keeps the depth
    // it already holds and its matrices stay frozen so the lit pass keeps sampling it correctly.
    bool ShouldUpdateCascade( UINT cascade ) const { return m_ShouldUpdateCascade[cascade]; }
    // World-space direction TOWARD the sun (this frame, temporally smoothed). Read by the sky-IBL pass.
    const DirectX::XMFLOAT3& GetSunDirWS() const { return m_SunDirWS; }
    const Frustum* CascadeFrusta() const { return m_CascadeFrustum; }
    // The rain shadowmap (D3D12Rain.cpp) renders its own single-slice depth map with the same normal-Z
    // caster state, so it reuses these PSOs rather than duplicating them — the VOB one likewise submits
    // through the engine's shared VOB command signature, so the two passes are byte-compatible.
    ID3D12PipelineState* GetWorldCasterPSO() const { return m_CasterWorldPSO.Get(); }
    ID3D12PipelineState* GetVobIndirectCasterPSO() const { return m_CasterVobIndirectPSO.Get(); }
    // ...and their no-pixel-shader twins, for the leading opaque run of a partitioned caster command set.
    // Null when PSO creation failed, in which case the caller submits everything through the clipping PSO.
    ID3D12PipelineState* GetWorldCasterNoAlphaPSO() const { return m_CasterWorldNoAlphaPSO.Get(); }
    ID3D12PipelineState* GetVobIndirectCasterNoAlphaPSO() const { return m_CasterVobIndirectNoAlphaPSO.Get(); }

    // ---- Per-cascade caster records, filled by the engine's shared collectors ----
    // The skeletal/attachment records are written by D3D12GraphicsEngine::PrepareFrameSkeletals (multi-cascade
    // mode) on the main thread and consumed by RecordCascade; PassVobs receives CollectVisibleVobs' output in
    // CullCascade and is drained by FinishPrepare's instance upload. Public because the collectors live in the
    // engine (they serve the main view too) and route into whichever destination list the pass asked for.
    std::vector<FrameSkelDraw>   SkelDraws[kShadowCascades];
    std::vector<FrameAttachDraw> AttachDraws[kShadowCascades];
    RenderView                   PassVobs[kShadowCascades];

private:
    bool CreateTextureAndViews( UINT size );
    void ComputeCascadeMatrices();   // fills m_CascadeViewProj/m_CascadeFrustum/m_CascadeTexelWorld/m_SunDirWS
    void UploadSamplingConstants( bool sunUp );   // the head of the engine's shared shadow CB (b3 in the lit passes)

    D3D12GraphicsEngine* m_E = nullptr;

    UINT m_MapSize = 2048;   // per-cascade slice resolution; mirrors RendererSettings.ShadowMapSize (clamped
                             // 512..8192, power-of-two steps), re-checked every frame in OnBeginFrame
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_Map;        // Texture2DArray(R32_TYPELESS), kShadowCascades slices
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_MapAlloc;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_DsvHeap;    // one D32 DSV per cascade slice
    UINT m_DsvSize = 0;
    UINT m_SrvSlot = UINT_MAX;         // R32_FLOAT Texture2DArray SRV (all cascades), bound by the lit passes
    bool m_InPixelState = false;       // DEPTH_WRITE (casters write) <-> PIXEL_SHADER_RESOURCE (lit reads)

    // Caster PSOs. All reuse the depth-prepass VS blobs (m_Pipelines.World/Skeletal.DepthPrepass*) with a
    // normal-Z, front-cull, depth-biased state and a void PS (PSShadowClip, alpha-clip only), fed the
    // per-cascade light view-proj instead of the camera's.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_CasterWorldPSO;
    Microsoft::WRL::ComPtr<ID3DBlob>            m_CasterPsBlob;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_CasterVobPSO;
    Microsoft::WRL::ComPtr<ID3DBlob>            m_CasterVobPsBlob;
    // Bindless-diffuse VOB caster (PSShadowClipBindless): lets a cascade's instanced-VOB casters submit as one
    // ExecuteIndirect through the engine's VOB command signature.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_CasterVobIndirectPSO;
    // Node-attachment variant (VSDepthAttach: Fatness/Scaling inflate instead of wind — needs NORMAL in the
    // layout). Reuses m_CasterVobPsBlob.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_CasterVobAttachPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_CasterSkeletalPSO;
    Microsoft::WRL::ComPtr<ID3DBlob>            m_CasterSkeletalPsBlob;
    // NO-PIXEL-SHADER caster twins (`PS = {}`) of the four above. A caster whose diffuse has no alpha channel
    // cannot be clipped by PSShadowClip's `clip(a - 0.5)` — binding a PS that merely *might* discard costs the
    // whole draw the hardware's double-rate depth-only path, over three cascades plus the rain map plus the
    // point-shadow cubes. Every caster list is partitioned opaque-first so each pass can submit the leading
    // run through these and only the tail through the clipping PSOs. Mirror of the main-view
    // World.DepthPrepassNoAlphaPSO family; likewise optional (null => no split, everything clips as before).
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_CasterWorldNoAlphaPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_CasterVobIndirectNoAlphaPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_CasterVobAttachNoAlphaPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_CasterSkeletalNoAlphaPSO;
    // CULL_NONE (not front-cull): grass cards are thin double-sided planes, matching Grass.PSO's own culling.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_CasterGrassPSO;
    Microsoft::WRL::ComPtr<ID3DBlob>            m_CasterGrassVsBlob;   // VSDepth (Vegetation.hlsl)
    Microsoft::WRL::ComPtr<ID3DBlob>            m_CasterGrassPsBlob;   // PSShadowClip (Vegetation.hlsl)

    DirectX::XMFLOAT4X4 m_CascadeViewProj[kShadowCascades] = {};   // light-space view*proj per cascade (this frame)
    Frustum             m_CascadeFrustum[kShadowCascades] = {};
    float               m_CascadeTexelWorld[kShadowCascades] = {};  // world units / shadow texel (sampling normal bias)

    DirectX::XMFLOAT3 m_SunDirWS = { 0.0f, 1.0f, 0.0f };
    // Temporal light-direction smoothing (P2.9c-3c): the origin-anchored texel-snap grid amplifies tiny
    // per-frame sun drift into a large lateral texel shift for players far from the origin (lever arm) ->
    // crawl. Lerp toward the live value so the grid orientation changes gradually, not per-frame.
    DirectX::XMFLOAT3 m_SmoothedSunDir = { 0.0f, 1.0f, 0.0f };
    bool m_SunDirInitialized = false;

    // Per-cascade world-mesh ExecuteIndirect arg rings (engine command sig m_WorldIndirectCmdSig).
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_WorldDrawArgs[kShadowCascades][kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_WorldDrawArgsAlloc[kShadowCascades][kBackBufferMax];
    uint8_t*                  m_WorldDrawArgsPtr[kShadowCascades][kBackBufferMax] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_WorldDrawArgsGpu[kShadowCascades][kBackBufferMax] = {};
    UINT                      m_WorldDrawCount[kShadowCascades] = {};
    // Alpha-test partition point: [0, opaque) needs no cutout, [opaque, total) does. See m_CasterWorldNoAlphaPSO.
    UINT                      m_WorldOpaqueDrawCount[kShadowCascades] = {};
    // Per-cascade instanced-VOB arg rings — the VOB analogue of the above (engine sig m_VobIndirectCmdSig).
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_VobDrawArgs[kShadowCascades][kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobDrawArgsAlloc[kShadowCascades][kBackBufferMax];
    uint8_t* m_VobDrawArgsPtr[kShadowCascades][kBackBufferMax] = {};
    UINT     m_VobDrawCount[kShadowCascades] = {};   // built by FinishPrepare, consumed by RecordCascade
    UINT     m_VobOpaqueDrawCount[kShadowCascades] = {};   // alpha-test partition — see m_WorldOpaqueDrawCount

    // Lazy cascade update (parity with D3D11ShadowMap's RendererSettings.DebugSettings.ShadowCascades.
    // LazyCascadeUpdate): the LAST cascade covers the whole world and is by far the most expensive to cull and
    // record, while also being the one where a two-frame-old shadow is least visible (it starts thousands of
    // units out, at a couple of texels per caster). So it only refreshes every kLazyLastCascadeInterval-th
    // frame; on the other frames its matrices, its frustum and its depth slice are all left exactly as they
    // were, and the whole cull -> build -> record chain for it is skipped. Only valid because each cascade owns
    // its own array slice and clears it itself in RecordCascade — skipping the record simply preserves the
    // contents (this is why D3D11 has to disable the same optimization on its atlas path, which clears wholesale).
    static constexpr size_t kLazyLastCascadeInterval = 3;
    size_t m_LazyFrameCounter = 0;
    bool m_ShouldUpdateCascade[kShadowCascades] = {};   // resolved in ComputeCascadeMatrices, read everywhere else
    bool m_CascadeMatricesValid = false;                // first frame (and after a Resize) nothing may be frozen

    bool m_CullingPending = false;   // cascade jobs are in flight and must be joined before the results are read
    bool m_RecordedInJob = false;    // this frame's jobs also RECORDED (not just culled/built) — see RecordedInJob()
    bool m_SunUp = false;            // resolved in Prepare(); RecordCascade may run on a pool thread and can't re-read the sky
    bool m_PassReady = false;        // Prepare() ran to completion this frame — the cascades have something to record
};
