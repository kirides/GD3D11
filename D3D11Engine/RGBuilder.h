#pragma once
#include "RGTextureDesc.h"
#include "RenderGraph.h"
#include "RenderPass.h"

class RGBuilder {
public:
    RGBuilder( class RenderGraph& graph, class RenderPass& pass )
        : m_graph( graph ), m_pass( pass ) {
    }

    // Declare that this pass READS from a resource (Source)
    RGResourceHandle Read( RGResourceHandle handle );

    // Declare that this pass WRITES to a resource (Sink)
    RGResourceHandle Write( RGResourceHandle handle );

    // Declare a brand new transient resource that lives only for this graph execution
    RGResourceHandle CreateTexture( const RGTextureDesc& desc );

private:
    RenderGraph& m_graph;
    RenderPass& m_pass;
};
