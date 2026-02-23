#pragma once
#include "RGTextureDesc.h"
#include <memory>

#include "BaseGraphicsEngine.h"
#include "Engine.h"
#include "RenderPass.h"
#include "RGBuilder.h"
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

class RenderGraph {
public:
    RenderGraph( TexturePool* pool ) : m_texturePool( pool ) {}

    // Bring an existing engine resource (like the DX11 BackBuffer) into the graph
    RGResourceHandle ImportResource(const std::wstring& name, RenderToTextureBuffer* externalBuffer) {
        uint32_t index = m_nextHandle++;
        
        // Resize vectors to accommodate the new index
        m_externalTextures.resize(m_nextHandle, nullptr);
        m_activeTextures.resize(m_nextHandle); 
        m_resourceDescs.resize(m_nextHandle);
        
        m_externalTextures[index] = externalBuffer;
        m_resourceDescs[index] = {0, 0, 0, name}; // Dummy desc for name tracking

        return MakeHandle(index, true); // Sets the first bit to 1
    }

    // Add a pass using modern C++ lambdas
    template<typename SetupFunc>
    void AddPass( const std::wstring& name, SetupFunc setupFunc ) {
        auto pass = std::make_unique<RenderPass>( name );
        RGBuilder builder( *this, *pass );

        // 1. Run the setup function to declare reads/writes
        setupFunc( builder, *pass );

        m_passes.push_back( std::move( pass ) );
    }

    // Called by RGBuilder to register handles
    RGResourceHandle RegisterResource(const RGTextureDesc& desc) {
        uint32_t index = m_nextHandle++;
        
        m_externalTextures.resize(m_nextHandle, nullptr);
        m_activeTextures.resize(m_nextHandle); 
        m_resourceDescs.resize(m_nextHandle);
        
        m_resourceDescs[index] = desc;

        return MakeHandle(index, false); // First bit remains 0
    }

    void Compile() {
        m_resourceLifetimes.assign(m_nextHandle, { UINT32_MAX, 0 });

        for ( size_t passIndex = 0; passIndex < m_passes.size(); ++passIndex ) {
            const auto& pass = m_passes[passIndex];

            // Track writes (creation/modification)
            for ( RGResourceHandle writeHandle : pass->m_writes ) {
                uint32_t index = GetHandleIndex(writeHandle);

                // If this is the first time we are writing to this resource, record its birth
                if ( m_resourceLifetimes[index].firstPass == UINT32_MAX ) {
                    m_resourceLifetimes[index].firstPass = (uint32_t)passIndex;
                }
                
                // Every write extends its lifetime to this pass
                m_resourceLifetimes[index].lastPass = (uint32_t)passIndex;
            }

            // Track reads (usage)
            for ( RGResourceHandle readHandle : pass->m_reads ) {
                uint32_t index = GetHandleIndex(readHandle);
                
                // Reads extend the lifetime of the resource to this pass
                m_resourceLifetimes[index].lastPass = (uint32_t)passIndex;
            }
        }

        // TODO: remove all passes whose writes are never read.
    }

    void Execute() {
        for ( size_t i = 0; i < m_passes.size(); ++i ) {
            const auto& pass = m_passes[i];

            AllocateResourcesForPass( i );

            if ( pass->m_executeCallback ) {
                auto _ = Engine::GraphicsEngine->RecordGraphicsEvent( pass->m_name.c_str() );
                pass->m_executeCallback(*this);
            }

            ReleaseResourcesForPass( i );
        }
    }

    RenderToTextureBuffer* GetPhysicalTexture(RGResourceHandle handle) const {
        uint32_t index = GetHandleIndex(handle);

        return IsExternalHandle(handle)
            ? m_externalTextures[index]
            : m_activeTextures[index].get();
    }
private:
    struct Lifetime { uint32_t firstPass; uint32_t lastPass; };

    TexturePool* m_texturePool;
    uint32_t m_nextHandle = 0;
    std::vector<std::unique_ptr<RenderPass>> m_passes;
    std::vector<RGTextureDesc> m_resourceDescs;
    std::vector<Lifetime> m_resourceLifetimes;
    
    // Physical resource storage mapped by the Handle Index
    std::vector<TextureHandle> m_activeTextures;
    std::vector<RenderToTextureBuffer*> m_externalTextures;
    
    void AllocateResourcesForPass(size_t passIndex) {
        for (uint32_t i = 0; i < m_resourceLifetimes.size(); ++i) {
            // We only allocate for Graph-Managed resources
            // (Assuming we track external handles in the lifetimes list too, we just skip them)

            if (m_resourceLifetimes[i].firstPass == (uint32_t)passIndex) {
                // If this index is meant to be external, m_externalTextures[i] will be populated,
                // so we don't allocate it from the pool.
                if (m_externalTextures[i] != nullptr) continue; 

                const RGTextureDesc& desc = m_resourceDescs[i];
                TexturePool::Description poolDesc{ (int)desc.width, (int)desc.height, static_cast<DXGI_FORMAT>(desc.format) };
                
                m_activeTextures[i] = std::move(m_texturePool->Acquire(poolDesc));
            }
        }
    }

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
