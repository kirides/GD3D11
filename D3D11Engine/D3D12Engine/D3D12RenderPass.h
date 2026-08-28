#pragma once
#include <d3d12.h>
#include <functional>
#include <vector>
#include "../RenderPass.h"    // reuse RGPassName / RG_PASS_NAME — both fully backend-neutral
#include "../RGTextureDesc.h"
#include "D3D12Barrier.h"      // D3D12ResourceTransition — reused verbatim for external (non-graph-tracked) transitions

class D3D12RenderGraph;
class D3D12CmdList;

/** One resource usage a pass declares (via D3D12RGBuilder::Read/Write) — the handle plus the resource
    STATE the pass needs it in. D3D12RenderGraph::Execute() uses this to insert transition barriers
    automatically for graph-owned (internal, CreateTexture()'d) resources: right before a pass's callback
    runs, every one of its declared reads/writes whose current D3D12RenderTarget::State doesn't already
    match gets transitioned, batched into one D3D12CmdList::TransitionBarriers() call. A pass's callback
    therefore no longer needs to check/transition a graph resource into the state it declared for itself
    — only for state changes it makes MID-callback that no other pass's declaration models (e.g. DoF's
    composite pass transitioning its UAV-written scratch to COPY_SOURCE right before copying it out; see
    D3D12DoF.cpp). External (imported) handles are skipped — the graph never tracks their state, exactly
    as before. */
struct RGResourceUsage {
    RGResourceHandle Handle;
    D3D12_RESOURCE_STATES State;
};

/** D3D12 counterpart of RenderPass (RenderPass.h) — same Reads/Writes declaration shape as the D3D11
    side. Differs in the execute callback: D3D12 has no implicit "current context" the way D3D11's
    ID3D11DeviceContext is, so the callback is handed the command list to record into explicitly. */
class D3D12RenderPass {
    friend class D3D12RenderGraph;

public:
    D3D12RenderPass( RGPassName name ) : m_name( std::move( name ) ) {}

    RGPassName m_name;
    std::vector<RGResourceUsage> m_reads;   // Sources
    std::vector<RGResourceUsage> m_writes;  // Sinks

    // Non-graph-tracked (external) resource transitions a pass wants folded into the graph's own batched
    // barrier calls, via D3D12RGBuilder::TransitionExternal()/TransitionExternalAfter() — e.g. the depth
    // buffer, m_SceneColor, or one of GTAO's manually-managed intermediates (m_GtaoWorkingDepth, m_AOMask).
    // m_preTransitions is folded into the SAME TransitionPassResources() batch as m_reads/m_writes, right
    // before the callback runs; m_postTransitions fires in its own batched call immediately after the
    // callback returns (D3D12RenderGraph::TransitionPostPassResources) — for a transition that must wait
    // until the pass's GPU work has actually been recorded. Reuses D3D12ResourceTransition verbatim rather
    // than a parallel struct: its Subresource/SyncBefore/SyncAfter already default to whole-resource/
    // unspecified, exactly what an external caller wants, and the graph can copy entries straight into its
    // batch array with no per-field conversion. As with Read()/Write(), the graph never tracks external
    // resource state — `Before` stays entirely the caller's responsibility, unchanged from a standalone
    // TransitionBarrier() call.
    std::vector<D3D12ResourceTransition> m_preTransitions;
    std::vector<D3D12ResourceTransition> m_postTransitions;

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
