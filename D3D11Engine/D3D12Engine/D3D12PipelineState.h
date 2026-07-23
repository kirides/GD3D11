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
    };
    // 2D UI / HUD family: one root sig (b0 viewport consts, t0 SRV, b1 FF state) + one VS/PS pair.
    // PSOs are built per (blend,depth) key on demand and cached. Vertex ring buffers stay in the engine.
    struct UIPipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3DBlob>            VsBlob;  // compiled once; reused for every blend PSO
        Microsoft::WRL::ComPtr<ID3DBlob>            PsBlob;
        std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> Pipelines; // key = Blend | Depth<<32
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

    // Stores non-owning device + shader-backend pointers; call once before any Create*().
    bool Init( D3D12Device* device, D3D12ShaderBackend* shaders );

    // --- Per-pass pipeline creation (pure pipeline state; no GPU buffers/textures) ---
    bool CreateWorld();        // shared world root sig + lit world-mesh PSO (must run before the two below)
    bool CreateDepthPrepass(); // depth-only world + instanced-VOB prepass PSOs (needs World.RootSig)
    bool CreateVob();          // lit instanced-VOB PSO (needs World.RootSig); buffers stay in the engine
    bool CreateUI();          // 2D/UI root sig + shaders; warms the default PSO (vertex buffers stay in engine)
    bool CreateParticle();    // particle root sig + shaders; warms the alpha PSO (instance buffers stay in engine)
    bool CreateDecal();       // decal root sig + shaders + fixed lit PSO; warms alpha (quad/instance VBs stay in engine)
    bool CreateTonemap();     // fullscreen HDR->swapchain resolve (exposure + ACES)
    bool CreateWater();       // alpha-blended water (own root sig: b0 ViewProj, t0, b1 fog, b2 water)
    bool CreateLightCull();   // Forward+ tiled light-cull compute (global compute root sig)

    // --- On-demand PSO cache lookups (called from the engine's draw path; create+cache on miss) ---
    ID3D12PipelineState* GetOrCreateUIPipeline( const GothicBlendStateInfo& blend, const GothicDepthBufferStateInfo& depth );
    ID3D12PipelineState* GetOrCreateParticlePipeline( const GothicBlendStateInfo& blend );
    ID3D12PipelineState* GetOrCreateDecalBlendPipeline( const GothicBlendStateInfo& blend );

    // --- Storage (one per migrated pass) ---
    WorldPipeline    World;
    UIPipeline       UI;
    ParticlePipeline Particle;
    DecalPipeline    Decal;
    GraphicsPipeline Tonemap;
    GraphicsPipeline Water;
    ComputePipeline  LightCull;

private:
    D3D12Device*        m_Device = nullptr;
    D3D12ShaderBackend* m_Shaders = nullptr;
};
