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

    // Set via D3D12RGBuilder::MarkExternalEffect() by a pass whose execute callback affects something the
    // graph doesn't track as a Write — e.g. copying a graph-managed transient texture onto an externally-
    // owned resource the graph never imported (D3D12DoF.cpp's composite pass does exactly this: it copies
    // its CreateTexture()'d scratch onto m_SceneColor, a plain member, not a D3D12RenderTarget the graph
    // could ImportResource()). Without this, a pass whose only declared Writes are internal handles that
    // nothing else Reads looks like dead code to Execute()'s elimination and its callback never runs, even
    // though real GPU work was expected of it — the resource it wrote still gets silently allocated by
    // AllocateResourcesForPass (that runs unconditionally), so the failure mode is quiet: no error, no
    // crash, just the pass's visible effect never happening.
    bool m_hasExternalSideEffect = false;

    std::function<void( const D3D12RenderGraph& graph, D3D12CmdList& cmdList )> m_executeCallback;
};
