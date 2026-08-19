#pragma once
#include <memory>
#include <string>
#include <vector>
#include "../RGTextureDesc.h"
#include "D3D12RenderPass.h"
#include "D3D12AliasedTextureArena.h"

class D3D12CmdList;

// Handle bit-packing, mirrors RenderGraph.h's free functions exactly (kept as a separate copy rather
// than shared with the D3D11 side: RGResourceHandle is a plain uint32_t typedef with no backend
// dependency, but duplicating four one-line helpers is cheaper than coupling this header to RenderGraph.h).
inline bool D3D12IsExternalHandle( RGResourceHandle handle ) { return ( handle & 1 ) != 0; }
inline uint32_t D3D12GetHandleIndex( RGResourceHandle handle ) { return handle >> 1; }
inline RGResourceHandle D3D12MakeHandle( uint32_t index, bool isExternal ) { return ( index << 1 ) | ( isExternal ? 1 : 0 ); }

class D3D12RGBuilder {
public:
    D3D12RGBuilder( class D3D12RenderGraph& graph, class D3D12RenderPass& pass )
        : m_graph( graph ), m_pass( pass ) {
    }

    // Declare that this pass READS from a resource (Source)
    RGResourceHandle Read( RGResourceHandle handle );

    // Declare that this pass WRITES to a resource (Sink)
    RGResourceHandle Write( RGResourceHandle handle );

    // Declare a brand new transient resource that lives only for this graph execution
    RGResourceHandle CreateTexture( const RGTextureDesc& desc );

    // Declare that this pass has a real effect the graph doesn't track as a Write (see
    // D3D12RenderPass::m_hasExternalSideEffect) — call this instead of inventing a fake Write handle when
    // a pass's only externally-visible output is, say, a CopyResource onto a plain member the graph never
    // imported. Exempts the pass from dead-pass elimination unconditionally.
    void MarkExternalEffect();

private:
    D3D12RenderGraph& m_graph;
    D3D12RenderPass& m_pass;
};

/** D3D12 counterpart of D3D11's RenderGraph (RenderGraph.h) — same handle/lifetime declaration API
    (Read/Write/CreateTexture, firstPass/lastPass from Compile()), so it stays a drop-in mental model
    for anyone who already knows the D3D11 side. Where it goes further: D3D11 has no placed-resource
    API, so its RenderGraph can only PACK transient textures into a free-list pool (one dedicated
    allocation per distinct Description, reused across frames — see TexturePool.h). D3D12 CAN alias two
    textures with non-overlapping lifetimes onto the exact same GPU memory, via D3D12AliasedTextureArena
    — Compile() asks the arena for each internal, actually-read resource's byte range.

    In active use today: ONE shared instance, `postFxGraph`, built once per frame in D3D12Scene.cpp's
    OnStartWorldRendering (same pattern D3D11's own per-frame `RenderGraph graph(...)` uses) and passed
    BY REFERENCE into RenderFogAndGodRays / RenderDepthOfField / DrawUnderwaterEffects / RenderSMAA, each
    of which registers its own passes directly onto it rather than building a private local graph. That
    used to be per-function local graphs (each constructing its own D3D12RenderGraph); unifying them was
    a deliberate correction once it became clear a single shared graph, not several independent ones, is
    what actually matches D3D11's own structure — see the functions themselves (D3D12DoF.cpp /
    D3D12Fog.cpp / D3D12Underwater.cpp / D3D12PostFX.cpp) for the pattern every one of them follows: any
    per-function state that used to be a stack local captured by reference now has to be heap-allocated
    (`std::make_shared`) and captured by value instead, because the function returns — and its stack
    frame dies — long before postFxGraph.Execute() actually invokes the deferred callbacks.

    Offset assignment is NAME-KEYED, not true interval-coloring: D3D12AliasedTextureArena::
    ReserveNamedRange gives each RGTextureDesc::name a byte range that is PERMANENT for the arena's
    current epoch (until the next resolution change), rather than recomputing packing per Compile() call.
    This was ALSO a deliberate correction, and it stays even under one shared graph — see
    ReserveNamedRange's own comment: several real passes (god rays, DoF, SMAA, underwater) are
    conditionally active per frame, so even within a single graph, pure position-based packing would
    still shift every pass registered after a toggled one and force spurious churn. The tradeoff: two
    DIFFERENT names no longer share memory through this graph, only a given name's own steady-state reuse
    across frames. Real cross-effect aliasing (the more ambitious "even better than D3D11" case) is
    future work for when the transient set is large enough that the packing actually matters.

    Resource STATE transitions (RENDER_TARGET <-> SHADER_RESOURCE etc) are still NOT automatic — each
    pass' execute callback remains responsible for transitioning whatever it binds, same as every other
    D3D12 pass in this backend (see D3D12RenderTarget::State). Only the ALIASING hazard (memory reuse
    between two different logical resources) is handled by the graph; the ordinary read/write hazard
    within one resource's own lifetime is not. */
class D3D12RenderGraph {
public:
    D3D12RenderGraph( D3D12AliasedTextureArena* arena ) : m_arena( arena ) {}

    // Bring an existing engine resource (e.g. the scene-color target) into the graph
    RGResourceHandle ImportResource( const std::wstring& name, D3D12RenderTarget* externalTarget );

    // Add a pass using modern C++ lambdas
    template<typename SetupFunc>
        requires std::invocable<SetupFunc, D3D12RGBuilder&, D3D12RenderPass&>
    void AddPass( RGPassName name, SetupFunc setupFunc );

    // Called by D3D12RGBuilder to register handles
    RGResourceHandle RegisterResource( const RGTextureDesc& desc );

    /** Runs the reads/writes lifetime analysis (mirrors RenderGraph::Compile) AND, for every internal
        resource that lifetime analysis says is actually read by something, the arena interval-coloring
        pass described above. Must run before Execute(). */
    void Compile();

    /** Records every live pass's draws, in order, onto cmdList. Aliasing barriers for any resource
        whose arena byte range changes hands this call are inserted immediately before that resource's
        first-writing pass — see D3D12AliasedTextureArena::Acquire. */
    void Execute( D3D12CmdList& cmdList );

    D3D12RenderTarget* GetPhysicalTexture( RGResourceHandle handle ) const {
        const uint32_t index = D3D12GetHandleIndex( handle );
        return D3D12IsExternalHandle( handle ) ? m_externalTextures[index] : m_activeTextures[index];
    }

private:
    struct Lifetime { uint32_t firstPass; uint32_t lastPass; bool isRead; };

    D3D12AliasedTextureArena* m_arena;
    uint32_t m_nextHandle = 0;
    std::vector<std::unique_ptr<D3D12RenderPass>> m_passes;
    std::vector<RGTextureDesc> m_resourceDescs;
    std::vector<Lifetime> m_resourceLifetimes;
    std::vector<UINT64> m_resourceOffsets;   // arena byte offset per internal resource, set by Compile()
    std::vector<bool> m_resourceHasOffset;   // false if Compile() couldn't fit this resource (arena exhausted)

    // Physical resource storage mapped by the Handle Index. Non-owning: internal textures live in the
    // arena's own Slot list (see D3D12AliasedTextureArena), which persists across graph executions —
    // there is nothing for this graph to release.
    std::vector<D3D12RenderTarget*> m_activeTextures;
    std::vector<D3D12RenderTarget*> m_externalTextures;

    void AllocateResourcesForPass( size_t passIndex, D3D12CmdList& cmdList );
};

template<typename SetupFunc>
    requires std::invocable<SetupFunc, D3D12RGBuilder&, D3D12RenderPass&>
inline void D3D12RenderGraph::AddPass( RGPassName name, SetupFunc setupFunc ) {
    auto pass = std::make_unique<D3D12RenderPass>( name );
    D3D12RGBuilder builder = D3D12RGBuilder( *this, *pass );

    // 1. Run the setup function to declare reads/writes
    setupFunc( builder, *pass );

    m_passes.push_back( std::move( pass ) );
}
