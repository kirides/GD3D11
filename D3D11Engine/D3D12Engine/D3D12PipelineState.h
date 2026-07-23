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

    // Stores non-owning device + shader-backend pointers; call once before any Create*().
    bool Init( D3D12Device* device, D3D12ShaderBackend* shaders );

    // --- Per-pass pipeline creation (pure pipeline state; no GPU buffers/textures) ---
    bool CreateTonemap();     // fullscreen HDR->swapchain resolve (exposure + ACES)
    bool CreateWater();       // alpha-blended water (own root sig: b0 ViewProj, t0, b1 fog, b2 water)
    bool CreateLightCull();   // Forward+ tiled light-cull compute (global compute root sig)

    // --- Storage (one per migrated pass) ---
    GraphicsPipeline Tonemap;
    GraphicsPipeline Water;
    ComputePipeline  LightCull;

private:
    D3D12Device*        m_Device = nullptr;
    D3D12ShaderBackend* m_Shaders = nullptr;
};
