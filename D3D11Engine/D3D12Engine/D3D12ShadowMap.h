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
//                            cascades (the world-mesh caster set + its bindless materials, the grass wind CB),
//                            then LAUNCHES the per-cascade culls on the worker pool.
//   B) CullCascade(c)        concurrent: frustum tests + CollectVisibleVobs + grass box cull. Touches only
//                            cascade c's own state and read-only engine data.
//   C) FinishPrepare()       main thread: joins B, then the steps that mutate Gothic state or a SHARED upload
//                            ring — per-cascade VOB instance uploads + indirect args + one multi-cascade
//                            skeletal preparation pass.
//   D) RecordCascade(c,list) concurrent: the actual draws, into one command list per cascade.
// Everything degrades to a serial in-order run when RendererSettings.ThreadedShadowCulling is off (or the
// per-slot command lists don't exist) — same output, same queue order, just no overlap.
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <D3D12MemAlloc.h>
#include <DirectXMath.h>

#include "../Frustum.h"
#include "D3D12EngineCommon.h"
#include "InstancingUtils.h"   // RenderView — the per-cascade visible-VOB collection target

class D3D12GraphicsEngine;
class GVegetationBox;
class zCTexture;

// Cascade count. Free-standing (not a class member) because the shared skeletal collector in D3D12Scene.cpp
// sizes its multi-cascade mode against it.
inline constexpr UINT kShadowCascades = 3;

class D3D12ShadowMap {
public:
    // Per-frame ring depth of the per-cascade indirect-arg buffers; must match
    // D3D12GraphicsEngine::kBackBufferCount (static_assert'd in the .cpp).
    static constexpr UINT kBackBufferCount = 2;

    // The engine back-reference, handed over in the engine's CONSTRUCTOR. Deliberately separate from Init():
    // the engine calls CreateWorldArgRings/CreateVobArgRings from its own indirect-command setup, which runs
    // BEFORE Init() (whose caster PSOs depend on the depth-prepass shader blobs existing first).
    void Attach( D3D12GraphicsEngine& engine ) { m_E = &engine; }

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

    // ---- The four phases (see the header comment) ----
    void Prepare();
    void CullCascade( UINT cascade );
    void FinishPrepare();
    void RecordCascade( UINT cascade, ID3D12GraphicsCommandList* cmdList, bool sunUp );
    // The single join point for the concurrent culls (mirrors D3D11ShadowMap::WaitShadowCullingComplete).
    void WaitCullingComplete();
    // Hand the cascade array to PIXEL_SHADER_RESOURCE for the lit-pass sampling (reverted at the top of the
    // next Prepare()). Issued on the main command list, after every cascade list has been executed.
    void TransitionToReadState( ID3D12GraphicsCommandList* cmdList );

    bool IsPassReady() const { return m_PassReady; }
    bool IsSunUp() const { return m_SunUp; }
    // World-space direction TOWARD the sun (this frame, temporally smoothed). Read by the sky-IBL pass.
    const DirectX::XMFLOAT3& GetSunDirWS() const { return m_SunDirWS; }
    const Frustum* CascadeFrusta() const { return m_CascadeFrustum; }
    // The rain shadowmap (D3D12Rain.cpp) renders its own single-slice depth map with the same normal-Z
    // caster state, so it reuses these PSOs rather than duplicating them — the VOB one likewise submits
    // through the engine's shared VOB command signature, so the two passes are byte-compatible.
    ID3D12PipelineState* GetWorldCasterPSO() const { return m_CasterWorldPSO.Get(); }
    ID3D12PipelineState* GetVobIndirectCasterPSO() const { return m_CasterVobIndirectPSO.Get(); }

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
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_WorldDrawArgs[kShadowCascades][kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_WorldDrawArgsAlloc[kShadowCascades][kBackBufferCount];
    uint8_t*                  m_WorldDrawArgsPtr[kShadowCascades][kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_WorldDrawArgsGpu[kShadowCascades][kBackBufferCount] = {};
    UINT                      m_WorldDrawCount[kShadowCascades] = {};
    // Per-cascade instanced-VOB arg rings — the VOB analogue of the above (engine sig m_VobIndirectCmdSig).
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_VobDrawArgs[kShadowCascades][kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobDrawArgsAlloc[kShadowCascades][kBackBufferCount];
    uint8_t* m_VobDrawArgsPtr[kShadowCascades][kBackBufferCount] = {};
    UINT     m_VobDrawCount[kShadowCascades] = {};   // built by FinishPrepare, consumed by RecordCascade

    bool m_CullingPending = false;   // cull jobs are in flight and must be joined before the results are read
    bool m_SunUp = false;            // resolved in Prepare(); RecordCascade may run on a pool thread and can't re-read the sky
    bool m_PassReady = false;        // Prepare() ran to completion this frame — the cascades have something to record
};
