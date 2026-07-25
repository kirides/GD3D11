#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <unordered_map>
#include <cstdint>
#include "D3D12Device.h"

class D3D12ShaderBackend;
struct GothicBlendStateInfo;
struct GothicDepthBufferStateInfo;

// Owns the D3D12 backend's pipeline state objects, root signatures, and compiled shader blobs,
// grouped per render pass. All PSO/root-signature creation lives here (out of the engine monolith);
// the engine holds one instance and binds these objects at draw time. GPU RESOURCE creation
// (vertex/instance/constant buffers, shadow textures) stays in the engine — this class is pipeline
// state only. Migrated incrementally: passes still living in the engine will move here over time.
class D3D12PipelineState {
public:
    // Grouped storage. Public so the engine can bind RootSig/PSO directly in the draw path.
    struct GraphicsPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlob;
    };
    struct ComputePipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
        Microsoft::WRL::ComPtr<ID3DBlob>            CsBlob;
    };
    // The shared opaque "world family": one root signature (b0 ViewProj, t0 diffuse, Forward+ light SRVs,
    // CSM + point-shadow tables, bindless material indices) anchors the lit world-mesh PSO, the lit
    // instanced-VOB PSO, and the depth-prepass PSOs. Skeletal + shadow-caster PSOs still living in the
    // engine also bind this RootSig, so it is the family's shared anchor and lives here.
    struct WorldPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        // Lit opaque world mesh
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlob;
        // Lit instanced static VOBs (reuses RootSig)
        Microsoft::WRL::ComPtr<ID3D12PipelineState> VobPSO;
        Microsoft::WRL::ComPtr<ID3DBlob>            VobVsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            VobPsBlob;
        // Depth prepass: world mesh (color write masked off, reversed-Z)
        Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassPSO;
        Microsoft::WRL::ComPtr<ID3DBlob>            DepthPrepassVsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            DepthPrepassPsBlob;
        // Depth prepass: instanced VOB (reuses RootSig)
        Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassVobPSO;
        Microsoft::WRL::ComPtr<ID3DBlob>            DepthPrepassVobVsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            DepthPrepassVobPsBlob;
        // Node-attachment variant (Fatness/Scaling instead of wind — see Vob.hlsl's VSMainAttach/VSDepthAttach).
        // Reuses RootSig + VobPsBlob/DepthPrepassVobPsBlob (PSMain/PSDepthClip are unchanged); only the VS + input
        // layout (needs NORMAL for the fatness inflate, unlike the leaner wind-only VOB depth-prepass layout) differ.
        Microsoft::WRL::ComPtr<ID3D12PipelineState> VobAttachPSO;
        Microsoft::WRL::ComPtr<ID3DBlob>            VobAttachVsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassVobAttachPSO;
        Microsoft::WRL::ComPtr<ID3DBlob>            DepthPrepassVobAttachVsBlob;
        // Bindless-diffuse instanced-VOB variants (ExecuteIndirect, P2.12). Same VS blobs + input layouts as
        // VobPSO/DepthPrepassVobPSO, but the PS reads the diffuse texture from the SRV heap by index (b6
        // MatDiffuseIndex) so the whole instanced-VOB color/depth pass submits as ONE ExecuteIndirect (a
        // descriptor-table diffuse bind can't ride an indirect command). Node attachments keep the t0 PSOs above.
        Microsoft::WRL::ComPtr<ID3D12PipelineState> VobIndirectPSO;
        Microsoft::WRL::ComPtr<ID3DBlob>            VobIndirectPsBlob;         // PSMainBindless
        Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassVobIndirectPSO;
        Microsoft::WRL::ComPtr<ID3DBlob>            DepthPrepassVobIndirectPsBlob; // PSDepthClipBindless
    };
    // 2D UI / HUD family: one root sig (b0 viewport consts, t0 SRV, b1 FF state) + one VS/PS pair.
    // PSOs are built per (blend,depth) key on demand and cached. Vertex ring buffers stay in the engine.
    struct UIPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;      // compiled once; reused for every blend PSO
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlobMaxZ;  // FORCE_MAX_Z variant — sky pass (STAGE_DRAW_SKY)
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlobHdr;   // LINEARIZE_OUTPUT variant — sky pass writes into the linear HDR scene target
        std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> Pipelines; // key = Blend | Depth<<32 | Cull<<34 | RtvIsHdr<<36 | MaxZ<<37
    };
    // Particle (PFX) billboards: one root sig + one VS/PS pair; PSOs built per BlendKey on demand.
    // Instance ring buffers stay in the engine.
    struct ParticlePipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;  // compiled once; reused for every blend PSO
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlob;
        std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> Pipelines; // key = BlendKey
    };
    // Decal sprites: own root sig (b0 ViewProj + t0 SRV + s0 clamp). Two pixel shaders — opaque/alpha-test
    // (fixed LitPSO, depth-write) and transparent (BlendPipelines cache per Gothic blend mode, depth-read-only).
    // The shared unit-quad VB + instance ring buffers stay in the engine.
    struct DecalPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            LitPsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            BlendPsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> LitPSO;   // opaque/alpha-test, depth-write on
        std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> BlendPipelines; // key = BlendKey
    };
    // Skinned skeletal meshes (animated NPCs/monsters): own root sig (b0 ViewProj consts, b1 instance CBV,
    // b2 bone-palette CBV, Forward+ light SRVs, CSM + point-shadow tables, bindless material indices). Lit PSO +
    // a depth-prepass PSO (color masked off). The depth-prepass VS also drives the CSM skeletal shadow caster
    // (built in the engine's CreateShadowMap). The per-frame skeletal CB ring (instance + bones) stays in the engine.
    struct SkeletalPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;                // lit opaque skinned
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassPSO;    // depth-only skinned (color write mask 0)
        Microsoft::WRL::ComPtr<ID3DBlob>            DepthPrepassVsBlob;  // also reused by the CSM skeletal shadow caster
        Microsoft::WRL::ComPtr<ID3DBlob>            DepthPrepassPsBlob;
    };
    // Point-light shadow cubes: two root sigs (world + VOB casters share one; the skeletal caster has its own,
    // with the 6-face view-proj CBV at b0), four VS/PS blobs, and three single-pass 6-face caster PSOs. The cube
    // ARRAY textures, per-slot DSV heaps, array SRV, and per-frame face-CB / VOB-instance rings are GPU resources
    // and stay in the engine.
    struct PointShadowPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;          // world + VOB casters (b0 face CBV, t0, s0)
        Microsoft::WRL::ComPtr<ID3D12RootSignature> SkeletalRootSig;  // skeletal caster (b0 faces, b1 inst, b2 bones)
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;            // VSCube (world)
        Microsoft::WRL::ComPtr<ID3DBlob>            VobVsBlob;         // VSCubeVob (step-rate-6 instance stream)
        Microsoft::WRL::ComPtr<ID3DBlob>            SkelVsBlob;        // VSCubeSkel (matrix-palette skinning)
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlob;            // PSCubeClip (void, alpha-clip) — shared
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CasterWorldPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CasterVobPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CasterSkeletalPSO;
    };

    // Bink video playback (zBinkPlayer): own root sig (b0 viewport consts, t0-t2 YUV planes SRV table, static
    // linear sampler). Single fixed PSO — no blend/depth variants needed (zBinkPlayer always draws an opaque,
    // non-depth-tested fullscreen-ish quad; see D3D12GraphicsEngine::DrawVertexArray's PS_Video branch).
    struct VideoPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlob;
    };

    // Bloom pyramid (P2.11): prefilter/downsample share one root sig+PSO pair each (t0 SRV table, u0 UAV table,
    // b0 8x32-bit BloomConstantBuffer), upsample has its own root sig (t0+t1 SRV table) + PSO. Composite is a
    // fullscreen-triangle graphics pass (additive blend) that adds the finished mip-0 pyramid onto the HDR scene
    // color. Pyramid textures (resolution-dependent, recreated on resize) stay in the engine.
    struct BloomPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> DownRootSig;   // prefilter + downsample (shared layout)
        Microsoft::WRL::ComPtr<ID3D12RootSignature> UpRootSig;
        Microsoft::WRL::ComPtr<ID3DBlob> PrefilterCsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> DownsampleCsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> UpsampleCsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PrefilterPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> DownsamplePSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> UpsamplePSO;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> CompositeRootSig;
        Microsoft::WRL::ComPtr<ID3DBlob> CompositeVsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> CompositePsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CompositePSO;
    };

    // Simple screen-space AO (SSAO/"SAO" — plan item #4). Forward+ has no GBuffer normals, so the main pass
    // reconstructs view-space normals from neighbouring depth samples (Shaders/D3D12/SSAO.hlsl CSMain); a
    // depth-aware separable blur (CSBlur, run horizontal then vertical) denoises the raw estimate. Both passes
    // share one root-sig SHAPE (b0 32-bit consts, one SRV descriptor table, one UAV descriptor table) but differ
    // in table width (main: 1 SRV; blur: 2 SRVs), so each gets its own root signature. Resolution-dependent
    // textures (m_AOMask/m_AOBlurTemp) and their heap slots stay in the engine, like the bloom pyramid.
    struct AOPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> MainRootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            MainCsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> MainPSO;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> BlurRootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            BlurCsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> BlurPSO;
    };

    // SMAA anti-aliasing (runtime toggle, RendererSettings.AntiAliasingMode == AA_SMAA). One bindless root
    // sig (b0 root consts { RT_METRICS + 5 SRV heap indices }, static linear+point clamp samplers,
    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) shared by all three fullscreen passes. Edge/Blend PSOs target an
    // R8G8B8A8 intermediate; the final Neighborhood PSO targets the swapchain (kBackBufferFormat). Area/search
    // LUTs and the resolution-dependent color/edges/blend textures are GPU resources and live in the engine.
    struct SmaaPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            EdgeVsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            EdgePsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            BlendVsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            BlendPsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            NeighborVsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            NeighborPsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> EdgePSO;      // pass 1: color -> edges (R8G8B8A8)
        Microsoft::WRL::ComPtr<ID3D12PipelineState> BlendPSO;     // pass 2: edges+LUTs -> blend weights (R8G8B8A8)
        Microsoft::WRL::ComPtr<ID3D12PipelineState> NeighborPSO;  // pass 3: color+blend -> swapchain (kBackBufferFormat)
    };

    // Stores non-owning device + shader-backend pointers; call once before any Create*().
    bool Init( D3D12Device* device, D3D12ShaderBackend* shaders );

    // --- Per-pass pipeline creation (pure pipeline state; no GPU buffers/textures) ---
    bool CreateWorld();        // shared world root sig + lit world-mesh PSO (must run before the two below)
    bool CreateDepthPrepass(); // depth-only world + instanced-VOB prepass PSOs (needs World.RootSig)
    bool CreateVob();          // lit instanced-VOB PSO (needs World.RootSig); buffers stay in the engine
    bool CreateUI();          // 2D/UI root sig + shaders; warms the default PSO (vertex buffers stay in engine)
    bool CreateParticle();    // particle root sig + shaders; warms the alpha PSO (instance buffers stay in engine)
    bool CreateDecal();       // decal root sig + shaders + fixed lit PSO; warms alpha (quad/instance VBs stay in engine)
    bool CreateSkeletal();    // skinned root sig + lit + depth-prepass PSOs (skeletal CB ring stays in the engine)
    bool CreatePointShadow(); // point-shadow root sigs + shaders + 3 caster PSOs (cube textures/DSVs/rings stay in engine)
    bool CreateTonemap();     // fullscreen HDR->swapchain resolve (dynamic exposure + ACES)
    bool CreateLumAdapt();    // two-pass GPU luminance reduction + temporal adaptation (feeds Tonemap's exposure)
    bool CreateWater();       // alpha-blended water (own root sig: b0 ViewProj, t0, b1 fog, b2 water)
    bool CreateLightCull();   // Forward+ tiled light-cull compute (global compute root sig)
    bool CreatePreview();     // single-VOB inventory-item preview (own root sig: b0 ViewProj, b1 World, t0 diffuse)
    bool CreateBloom();       // prefilter/downsample/upsample compute + additive composite graphics pipeline
    bool CreateGhost();       // ghost/transparency VOBs (own root sig: b0 ViewProj, b1 World, b2 GhostAlpha, t0 diffuse)
    bool CreateGhostSkeletal(); // skeletal ghost VOBs (invisible NPCs): own root sig (b0 ViewProj, b1 inst CBV,
                                // b2 bone-palette CBV, b7 GhostAlpha, t0 diffuse); reuses Skeletal.hlsl's VSDepth
    bool CreateGrass();       // GVegetationBox instanced grass cards (own root sig: b0 ViewProj, t0/t1 grass+ground
                              // textures, b1 GrassCB, b2 fog, Forward+ lights, b4 shadow CB, t5 CSM shadow map)
    bool CreateVideo();       // Bink YUV video playback (own root sig: b0 viewport consts, t0-t2 YUV planes)
    bool CreateSmaa();        // SMAA 3-pass AA (bindless root sig + edge/blend/neighborhood PSOs); textures stay in engine
    bool CreateAO();          // simple SSAO: main estimate + separable blur compute pipelines; textures stay in engine

    // --- On-demand PSO cache lookups (called from the engine's draw path; create+cache on miss) ---
    // cullMode/frontCCW/rtvIsHdr/forceMaxZ default to the plain 2D/UI case (cull-none, clockwise-front,
    // backbuffer RTV, no Z override); the sky pass (DrawSky, STAGE_DRAW_SKY) passes the real rasterizer
    // cull mode + winding (the D3D7 layer forces FrontCounterClockwise=true for sky FVF draws — Gothic's
    // sky geometry is wound CCW), the HDR scene-color format, and forceMaxZ=true so the sky VS pins z to
    // the reversed-Z far plane instead of passing the FF z through.
    ID3D12PipelineState* GetOrCreateUIPipeline( const GothicBlendStateInfo& blend, const GothicDepthBufferStateInfo& depth,
        D3D12_CULL_MODE cullMode = D3D12_CULL_MODE_NONE, bool rtvIsHdr = false, bool forceMaxZ = false, bool frontCCW = false );
    ID3D12PipelineState* GetOrCreateParticlePipeline( const GothicBlendStateInfo& blend );
    ID3D12PipelineState* GetOrCreateDecalBlendPipeline( const GothicBlendStateInfo& blend );

    // --- Storage (one per migrated pass) ---
    WorldPipeline       World;
    UIPipeline          UI;
    ParticlePipeline    Particle;
    DecalPipeline       Decal;
    SkeletalPipeline    Skeletal;
    PointShadowPipeline PointShadow;
    GraphicsPipeline Tonemap;
    GraphicsPipeline Water;
    ComputePipeline  LightCull;
    ComputePipeline  LumReduce;   // dynamic exposure, level 1: scene color -> per-group partial luminance sums
    ComputePipeline  LumAdapt;    // dynamic exposure, level 2: reduce partials + temporal-adapt -> Tonemap's exposure
    GraphicsPipeline Preview;
    BloomPipeline    Bloom;
    GraphicsPipeline Ghost;
    GraphicsPipeline GhostSkeletal;
    GraphicsPipeline Grass;
    VideoPipeline    Video;
    SmaaPipeline     Smaa;
    AOPipeline       AO;

private:
    D3D12Device*        m_Device = nullptr;
    D3D12ShaderBackend* m_Shaders = nullptr;
};
