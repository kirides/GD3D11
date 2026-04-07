#include "RenderGraph.h"

// Implement RGBuilder methods (must be done after RenderGraph is fully defined)
RGResourceHandle RGBuilder::Read( RGResourceHandle handle ) {
    m_pass.m_reads.push_back( handle );
    return handle;
}

RGResourceHandle RGBuilder::Write( RGResourceHandle handle ) {
    m_pass.m_writes.push_back( handle );
    return handle;
}

RGResourceHandle RGBuilder::CreateTexture( const RGTextureDesc& desc ) {
    RGResourceHandle handle = m_graph.RegisterResource( desc );
    return Write( handle ); // Creating it implies we are writing to it
}
