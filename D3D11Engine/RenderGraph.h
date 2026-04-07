#pragma once
#include "pch.h"
#include "RGTextureDesc.h"
#include <memory>
#include <vector>

#include "Logger.h"
#include "RenderPass.h"
#include "TexturePool.h"

// Helpers for bit-packing
inline bool IsExternalHandle(RGResourceHandle handle) {
    return (handle & 1) != 0; // Check if the first bit is 1
}

inline uint32_t GetHandleIndex(RGResourceHandle handle) {
    return handle >> 1;       // Shift right by 1 to get the actual ID
}

inline RGResourceHandle MakeHandle(uint32_t index, bool isExternal) {
    return (index << 1) | (isExternal ? 1 : 0);
}

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

class RenderGraph {
public:
    RenderGraph( TexturePool* pool ) : m_texturePool( pool ) {}

    // Bring an existing engine resource (like the DX11 BackBuffer) into the graph
    RGResourceHandle ImportResource( const std::wstring& name, RenderToTextureBuffer* externalBuffer );

    // Add a pass using modern C++ lambdas
    template<typename SetupFunc>
        requires std::invocable<SetupFunc, RGBuilder&, RenderPass&>
    void AddPass( RGPassName name, SetupFunc setupFunc );

    // Called by RGBuilder to register handles
    RGResourceHandle RegisterResource( const RGTextureDesc& desc );

    void Compile();

    void Execute();

    RenderToTextureBuffer* GetPhysicalTexture(RGResourceHandle handle) const {
        uint32_t index = GetHandleIndex(handle);

        return IsExternalHandle(handle)
            ? m_externalTextures[index]
            : m_activeTextures[index].get();
    }
private:
    struct Lifetime { uint32_t firstPass; uint32_t lastPass; bool isRead; };

    TexturePool* m_texturePool;
    uint32_t m_nextHandle = 0;
    std::vector<std::unique_ptr<RenderPass>> m_passes;
    std::vector<RGTextureDesc> m_resourceDescs;
    std::vector<Lifetime> m_resourceLifetimes;
    
    // Physical resource storage mapped by the Handle Index
    std::vector<TextureHandle> m_activeTextures;
    std::vector<RenderToTextureBuffer*> m_externalTextures;
    
    void AllocateResourcesForPass( size_t passIndex );

    void ReleaseResourcesForPass(size_t passIndex) {
        for (uint32_t i = 0; i < m_resourceLifetimes.size(); ++i) {
            if (m_resourceLifetimes[i].lastPass == (uint32_t)passIndex) {
                if (m_externalTextures[i] != nullptr) continue;

                // Resetting the unique_ptr triggers 
                // returning it to the TexturePool automatically.
                m_activeTextures[i].reset(); 
            }
        }
    }
};

template<typename SetupFunc>
    requires std::invocable<SetupFunc, RGBuilder&, RenderPass&>
inline void RenderGraph::AddPass( RGPassName name, SetupFunc setupFunc ) {
    auto pass = std::make_unique<RenderPass>( name );
    RGBuilder builder = RGBuilder( *this, *pass );

    // 1. Run the setup function to declare reads/writes
    setupFunc( builder, *pass );

    m_passes.push_back( std::move( pass ) );
}
