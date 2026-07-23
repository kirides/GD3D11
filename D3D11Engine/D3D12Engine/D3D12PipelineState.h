#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include "D3D12Device.h"

class D3D12ShaderBackend;

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

    // Stores non-owning device + shader-backend pointers; call once before any Create*().
    bool Init( D3D12Device* device, D3D12ShaderBackend* shaders );

    // --- Per-pass pipeline creation (pure pipeline state; no GPU buffers/textures) ---
    bool CreateWorld();        // shared world root sig + lit world-mesh PSO (must run before the two below)
    bool CreateDepthPrepass(); // depth-only world + instanced-VOB prepass PSOs (needs World.RootSig)
    bool CreateVob();          // lit instanced-VOB PSO (needs World.RootSig); buffers stay in the engine
    bool CreateTonemap();     // fullscreen HDR->swapchain resolve (exposure + ACES)
    bool CreateWater();       // alpha-blended water (own root sig: b0 ViewProj, t0, b1 fog, b2 water)
    bool CreateLightCull();   // Forward+ tiled light-cull compute (global compute root sig)

    // --- Storage (one per migrated pass) ---
    WorldPipeline    World;
    GraphicsPipeline Tonemap;
    GraphicsPipeline Water;
    ComputePipeline  LightCull;

private:
    D3D12Device*        m_Device = nullptr;
    D3D12ShaderBackend* m_Shaders = nullptr;
};
