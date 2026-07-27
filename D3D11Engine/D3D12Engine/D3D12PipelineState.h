#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <unordered_map>
#include <map>
#include <string>
#include <cstdint>
#include "D3D12Device.h"
#include "D3D12RootLayout.h"

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
        // Lit quad marks (zCQuadMark). Reuses RootSig AND PsBlob (World.hlsl PSMain) — only the VS and the
        // input layout differ (unpacked ExVertexStruct, CPU-transformed to world space; see VSQuadMark).
        // Blend-keyed like the FX cache because the marks carry Gothic's per-material alpha funcs.
        Microsoft::WRL::ComPtr<ID3DBlob>            QuadMarkVsBlob;
        std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> QuadMarkPipelines;
    };
    // Water (transparent world surfaces): own root sig, the alpha-blended color PSO, plus a depth-only
    // prepass PSO (same root sig + VB/IB, lean position-only layout) that lays the water surface down in
    // the main depth buffer before the blended pass — mirrors D3D11's water Z-prepass, which the depth-
    // reading post passes (height fog, god rays) depend on.
    struct WaterPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassPSO;      // reuses VsBlob — see CreateWater()
        Microsoft::WRL::ComPtr<ID3DBlob>            DepthPrepassPsBlob;   // PSDepth (writes nothing)
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

    // Sky image-based lighting — the indirect-light source for the Forward+ PBR shaders (Shaders/D3D12/
    // SkyIbl.hlsl). Two root sigs: the radiance pass writes mip 0 from constants only (b0 20 root consts,
    // u0 UAV table); the prefilter and irradiance passes share one shape (b1 4 root consts, t0 mip-0 SRV
    // table, u0 UAV table) since both read the same source cube and write one face slice. The cubes and
    // their heap slots live in the engine (D3D12SkyIbl.cpp), like the bloom/AO pyramids.
    struct SkyIblPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RadianceRootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            RadianceCsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> RadiancePSO;     // analytic sky -> env cube mip 0
        Microsoft::WRL::ComPtr<ID3D12RootSignature> FilterRootSig;   // shared by both passes below
        Microsoft::WRL::ComPtr<ID3DBlob>            PrefilterCsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PrefilterPSO;    // GGX importance sample -> env cube mips 1..N
        Microsoft::WRL::ComPtr<ID3DBlob>            IrradianceCsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> IrradiancePSO;   // cosine convolve -> irradiance cube
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

    // Gothic FX geometry — quad marks (zCQuadMark) and poly strips (weapon/spell trails, lightning). Both are
    // CPU-built ExVertexStruct triangle lists drawn unlit with a per-draw blend mode, so they share one root
    // signature (b0 ViewProj, b1 World, b2 bindless diffuse + alpha-test flag, static aniso-wrap s0,
    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) and one blend-keyed PSO cache — same pattern as the decal/particle/
    // world-transparency caches. See Shaders/D3D12/Fx.hlsl and D3D12Fx.cpp.
    struct FxPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlob;
        std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> BlendPipelines;
    };

    // Post-tonemap sharpening (RendererSettings.SharpeningMode; SHARPEN_CAS is the shipped default on both
    // backends). One bindless root sig (b0 root consts { CAS const0/const1, source SRV heap index, strength,
    // resolution }, static linear-clamp sampler, CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) shared by both modes;
    // both are fullscreen-triangle passes writing the swapchain (kBackBufferFormat) from the LDR copy.
    struct SharpenPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            SimplePsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            CasPsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> SimplePSO;   // SHARPEN_SIMPLE (unsharp mask)
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CasPSO;      // SHARPEN_CAS (FidelityFX CAS)
    };

    // Height fog + god rays (plan item #5) — the D3D12 port of D3D11's PostFX composition pass. Two
    // quarter-res compute passes build the god-ray texture (Shaders/D3D12/GodRays.hlsl CSMask/CSZoom, sharing
    // one bindless compute root sig: b0 12 root consts + one static linear-clamp sampler), then a single
    // fullscreen premultiplied-alpha graphics pass (Shaders/D3D12/HeightFog.hlsl) blends the fog and adds the
    // rays straight onto m_SceneColor — no scene-color copy, unlike D3D11 (see the shader's file header).
    // The quarter-res textures live in the engine (resolution-dependent), like the bloom/AO pyramids.
    struct FogPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> GodRayRootSig;   // shared by both compute passes
        Microsoft::WRL::ComPtr<ID3DBlob>            MaskCsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> MaskPSO;         // scene color + depth -> mask
        Microsoft::WRL::ComPtr<ID3DBlob>            ZoomCsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> ZoomPSO;         // mask -> radially blurred rays
        Microsoft::WRL::ComPtr<ID3D12RootSignature> CompositeRootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            CompositeVsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>            CompositePsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CompositePSO;    // fog (alpha) + rays (additive) -> scene color
    };

    // GPU-driven instanced-VOB culling. The CPU-side collection only distance-culls (see
    // RndCullContext::drawFlags.SkipVobFrustumCull); these four compute passes do the rest on the GPU:
    // Shaders/D3D12/HiZ.hlsl builds a min-reduced (conservative, reversed-Z) depth pyramid from the
    // WORLD-MESH depth prepass, then Shaders/D3D12/VobCull.hlsl frustum+occlusion-tests every instance,
    // compacts the survivors, and rewrites DrawIndexedInstanced::InstanceCount in the ExecuteIndirect
    // argument buffer. Resolution-dependent Hi-Z textures + the cull buffers live in the engine.
    struct CullPipeline {
        // Hi-Z build: one bindless root sig (b0 = 4 root consts, no tables), two entry points.
        Microsoft::WRL::ComPtr<ID3D12RootSignature> HiZRootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            HiZCopyCsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> HiZCopyPSO;      // full-res depth -> mip 0 (half res)
        Microsoft::WRL::ComPtr<ID3DBlob>            HiZReduceCsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> HiZReducePSO;    // mip N-1 -> mip N
        // Cull + compact: b0 24 root consts (ViewProj + Hi-Z params), t0/t1 root SRVs, u0/u1 root UAVs.
        Microsoft::WRL::ComPtr<ID3D12RootSignature> VobCullRootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            VobCullCsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> VobCullPSO;
        // Indirect-arg patch: b0 4 root consts, t0 root SRV (per-visual counts), u0 root UAV (arg buffer).
        Microsoft::WRL::ComPtr<ID3D12RootSignature> PatchRootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            PatchCsBlob;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PatchPSO;
    };

    // Debug/editor lines (D3D12LineRenderer): one root sig (b0 ViewProj, b1 viewport) + one VS/PS set,
    // two PSOs. WorldPSO is the depth-tested world-space list (reversed-Z, GREATER_EQUAL, no depth write);
    // ScreenPSO is the pre-transformed xyzrhw list with depth off. Both are LINE topology, alpha-blended,
    // and target the swapchain backbuffer (they run after the tonemap resolve). The per-frame vertex ring
    // is a GPU resource and stays in the engine.
    struct LinePipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;        // VSMain   (world space)
        Microsoft::WRL::ComPtr<ID3DBlob>            ScreenVsBlob;  // VSScreen (xyzrhw)
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlob;        // PSMain   (pass-through vertex color)
        Microsoft::WRL::ComPtr<ID3D12PipelineState> WorldPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> ScreenPSO;
    };

    // Alpha-blended world-mesh surfaces (ice, glass, magic barriers) peeled out of the opaque world pass —
    // the port of D3D11's DrawMeshInfoListAlphablended. Own (small) root sig so the shared World.RootSig's
    // root-DWORD budget and its ExecuteIndirect command signatures stay untouched: b0 ViewProj, b5 the
    // per-material FF texture factor, b6 the bindless material indices (only MatDiffuseIndex is read).
    // One VS/PS pair; the blend PSOs are built per Gothic blend mode on demand (like Decal/Particle), and
    // DepthFillPSO is the color-masked depth-only re-draw D3D11 issues after the blended list so the
    // depth-reading post passes (height fog, god rays) see these surfaces.
    struct WorldTransparencyPipeline {
        // Which D3D11 pixel shader this draw stands in for. D3D11 picks these in BindShaderForTexture off
        // the material TYPE, then runs all three through the same DrawMeshInfoListAlphablended body.
        enum class EKind : uint32_t {
            Simple = 0,   // PS_Simple_FF     — blended world surfaces (ice, glass); reads the FF texture factor
            Foam   = 1,   // PS_WaterfallFoam — MT_WaterfallFoam; own day/night tint, ignores the texture factor
            Portal = 2,   // PS_PortalDiffuse — MT_Portal (G1 forest portals); distance fade, needs a VIEW-space VS
        };
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;         // VSTransparent — Simple + Foam + the depth fill
        Microsoft::WRL::ComPtr<ID3DBlob>            PortalVsBlob;   // VSTransparentPortal (also outputs view-space pos)
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlob;         // PSTransparent
        Microsoft::WRL::ComPtr<ID3DBlob>            FoamPsBlob;     // PSTransparentFoam
        Microsoft::WRL::ComPtr<ID3DBlob>            PortalPsBlob;   // PSTransparentPortal
        Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthFillPSO;
        std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> BlendPipelines; // key = BlendKey | kind<<29 | depthWrite<<31
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
    bool CreateSharpen();     // post-tonemap sharpen (bindless root sig + simple/CAS PSOs); LDR copy stays in engine
    bool CreateFx();          // MUL quad marks + poly strips (own unlit root sig; warms the default blend PSO)
    // Lit quad marks: World.RootSig + World.hlsl VSQuadMark/PSMain, blend-keyed like the FX cache above.
    ID3D12PipelineState* GetOrCreateQuadMarkPipeline( const GothicBlendStateInfo& blend, bool depthWrite );
    // Blend-keyed PSO cache for the FX pass. depthWrite rides the key's top bit (BlendKey uses bits 0..28).
    ID3D12PipelineState* GetOrCreateFxPipeline( const GothicBlendStateInfo& blend, bool depthWrite );
    bool CreateAO();          // simple SSAO: main estimate + separable blur compute pipelines; textures stay in engine
    bool CreateSkyIbl();      // sky IBL: analytic radiance + GGX prefilter + irradiance compute pipelines; cubes stay in engine
    bool CreateFog();         // height fog + god rays: 2 god-ray compute PSOs + the fullscreen composition PSO
    bool CreateAdvanceRain(); // rain/snow particle advance compute (b0 32-bit consts, t0 static SRV, u0 dynamic UAV)
    bool CreateRainDraw();    // rain/snow billboard draw (b0 ViewProj, b1 particle info, t0/t1 root SRVs, no IA)
    bool CreateCull();        // Hi-Z build + GPU VOB cull/compact + indirect-arg patch compute pipelines
    bool CreateLines();       // debug/editor line lists (world-space depth-tested + screen-space xyzrhw)
    bool CreateWorldTransparency(); // alpha-blended world-mesh surfaces (own root sig; warms the alpha + depth-fill PSOs)

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
    // Alpha-blended world mesh. depthWrite mirrors D3D11's state machine: the list starts from
    // SetDefaultStates() (depth-write ON, blending off) and every alpha-func change turns depth-write off.
    ID3D12PipelineState* GetOrCreateWorldTransparencyPipeline( const GothicBlendStateInfo& blend, bool depthWrite,
        WorldTransparencyPipeline::EKind kind = WorldTransparencyPipeline::EKind::Simple );

    // --- Storage (one per migrated pass) ---
    WorldPipeline       World;
    UIPipeline          UI;
    ParticlePipeline    Particle;
    DecalPipeline       Decal;
    SkeletalPipeline    Skeletal;
    PointShadowPipeline PointShadow;
    GraphicsPipeline Tonemap;
    WaterPipeline    Water;
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
    SharpenPipeline  Sharpen;       // post-tonemap sharpening (Shaders/D3D12/Sharpen.hlsl)
    FxPipeline       Fx;            // quad marks + poly strips (Shaders/D3D12/Fx.hlsl)
    AOPipeline       AO;
    SkyIblPipeline   SkyIbl;        // sky image-based lighting (Shaders/D3D12/SkyIbl.hlsl)
    FogPipeline      Fog;        // height fog + god rays (Shaders/D3D12/HeightFog.hlsl + GodRays.hlsl)
    ComputePipeline  AdvanceRain;   // rain/snow particle advance (Shaders/D3D12/AdvanceRain.hlsl)
    GraphicsPipeline RainDraw;      // rain/snow billboard draw (Shaders/D3D12/Rain.hlsl)
    CullPipeline     Cull;          // Hi-Z build + GPU VOB cull (Shaders/D3D12/HiZ.hlsl + VobCull.hlsl)
    LinePipeline     Lines;         // debug/editor line lists (Shaders/D3D12/Lines.hlsl)
    WorldTransparencyPipeline WorldTransparency;  // alpha-blended world surfaces (Shaders/D3D12/World.hlsl VSTransparent/PSTransparent)

private:
    // Returns the retained root layout registered under `name`, cleared and ready to declare into.
    // Keyed by name (rather than appended) so re-running a Create*() — a shader hot-reload, say —
    // reuses its slot instead of accumulating layouts. std::map keeps references stable, which
    // matters because Create*() holds the reference across the whole function.
    D3D12RootLayout& Layout( const char* name );
    // Looks up an already-declared layout WITHOUT clearing it, for the Create*()s that build PSOs
    // against a root signature another pass owns (the World family: CreateVob/CreateDepthPrepass
    // share World.RootSig). Returns nullptr if the owning Create*() hasn't run.
    D3D12RootLayout* GetLayout( const char* name );

    D3D12Device*        m_Device = nullptr;
    D3D12ShaderBackend* m_Shaders = nullptr;
    // Retained root-signature declarations, one per root sig. Kept past Build() so
    // D3D12RootLayout::ValidateShaders can check each pass's shaders against what it declared.
    std::map<std::string, D3D12RootLayout> m_Layouts;
};
