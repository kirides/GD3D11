#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include "D3D12Device.h"

struct GothicBlendStateInfo;
struct GothicDepthBufferStateInfo;

// Stores RootSig, PSO and Shader blobs for a pipeline
// some of the ComPtrs may be shared from other pipelines
struct D3D12PipelineStateInfo {
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  RootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  PSO;
    Microsoft::WRL::ComPtr<ID3DBlob>             VsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob>             PsBlob;
};

class D3D12PipelineState
{
public:
    // Init the static pipelines
    bool Init( D3D12Device* device );
    ID3D12PipelineState* GetOrCreateParticlePipeline( const GothicBlendStateInfo& blend );
    ID3D12PipelineState* GetOrCreateDecalBlendPipeline( const GothicBlendStateInfo& blend );
    ID3D12PipelineState* GetOrCreateUIPipeline( const GothicBlendStateInfo& blend, const GothicDepthBufferStateInfo& depth );

private:
    D3D12Device* m_Device;
};

