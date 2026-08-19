#pragma once
#include <memory>
#include <string>
#include <vector>
#include "../RGTextureDesc.h"
#include "D3D12RenderPass.h"
#include "D3D12TexturePool.h"

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

private:
    D3D12RenderGraph& m_graph;
    D3D12RenderPass& m_pass;
};

/** D3D12 counterpart of D3D11's RenderGraph (RenderGraph.h) — same handle/lifetime API and the same
    single-pass-over-a-list execution model, deliberately NOT reimplemented as something smarter so it
    stays a drop-in mental model for anyone who already knows the D3D11 side. Backed by D3D12TexturePool
    instead of TexturePool.

    NOT YET WIRED INTO THE D3D12 FRAME LOOP. No pass constructs one today: the D3D12 backend's post-fx
    resources (DoF, motion G-buffer, bloom, ...) are long-lived by design — allocated once, lazily, and
    reused every frame (see D3D12DoF.cpp / D3D12Motion.cpp) — which is deliberately NOT what a render
    graph is for. D3D12 also has no Forward+ pass-graph structure yet (D3D12Scene.cpp is still a
    monolith of direct calls). This lands the infrastructure ahead of the first genuinely transient
    per-frame scratch resource that needs it — the natural first consumer is a Forward+ screen-space
    shadow-mask / AO-mask texture, mirroring D3D11ForwardPlusRenderer::AddGeometryPasses.

    UNLIKE D3D11, transitioning a physical resource's state is NOT automatic here. D3D12 needs explicit
    barriers, and inferring them correctly from Read/Write declarations (and — the actual payoff of
    doing this on D3D12 — aliasing non-overlapping-lifetime resources into shared heap memory) is real
    design work, deferred to a follow-up increment. Until then, each pass' execute callback is
    responsible for transitioning whatever it binds, exactly like every other D3D12 pass in this
    backend (see D3D12RenderTarget::State). */
class D3D12RenderGraph {
public:
    D3D12RenderGraph( D3D12TexturePool* pool ) : m_texturePool( pool ) {}

    // Bring an existing engine resource (e.g. the scene-color target) into the graph
    RGResourceHandle ImportResource( const std::wstring& name, D3D12RenderTarget* externalTarget );

    // Add a pass using modern C++ lambdas
    template<typename SetupFunc>
        requires std::invocable<SetupFunc, D3D12RGBuilder&, D3D12RenderPass&>
    void AddPass( RGPassName name, SetupFunc setupFunc );

    // Called by D3D12RGBuilder to register handles
    RGResourceHandle RegisterResource( const RGTextureDesc& desc );

    void Compile();

    void Execute( D3D12CmdList& cmdList );

    D3D12RenderTarget* GetPhysicalTexture( RGResourceHandle handle ) const {
        const uint32_t index = D3D12GetHandleIndex( handle );
        return D3D12IsExternalHandle( handle ) ? m_externalTextures[index] : m_activeTextures[index].get();
    }

private:
    struct Lifetime { uint32_t firstPass; uint32_t lastPass; bool isRead; };

    D3D12TexturePool* m_texturePool;
    uint32_t m_nextHandle = 0;
    std::vector<std::unique_ptr<D3D12RenderPass>> m_passes;
    std::vector<RGTextureDesc> m_resourceDescs;
    std::vector<Lifetime> m_resourceLifetimes;

    // Physical resource storage mapped by the Handle Index
    std::vector<D3D12TextureHandle> m_activeTextures;
    std::vector<D3D12RenderTarget*> m_externalTextures;

    void AllocateResourcesForPass( size_t passIndex );
    void ReleaseResourcesForPass( size_t passIndex );
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
