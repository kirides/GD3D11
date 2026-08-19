#pragma once
#include <functional>
#include <vector>
#include "../RenderPass.h"    // reuse RGPassName / RG_PASS_NAME — both fully backend-neutral
#include "../RGTextureDesc.h"

class D3D12RenderGraph;
class D3D12CmdList;

/** D3D12 counterpart of RenderPass (RenderPass.h) — same Reads/Writes declaration shape as the D3D11
    side. Differs in the execute callback: D3D12 has no implicit "current context" the way D3D11's
    ID3D11DeviceContext is, so the callback is handed the command list to record into explicitly. */
class D3D12RenderPass {
    friend class D3D12RenderGraph;

public:
    D3D12RenderPass( RGPassName name ) : m_name( std::move( name ) ) {}

    RGPassName m_name;
    std::vector<RGResourceHandle> m_reads;   // Sources
    std::vector<RGResourceHandle> m_writes;  // Sinks

    std::function<void( const D3D12RenderGraph& graph, D3D12CmdList& cmdList )> m_executeCallback;
};
