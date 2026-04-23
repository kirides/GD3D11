#pragma once
#include "RGTextureDesc.h"
#include <memory>
#include <vector>

#include "Logger.h"
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
    RGResourceHandle ImportResource( const std::wstring& name, RenderToTextureBuffer* externalBuffer );

    // Add a pass using modern C++ lambdas
    template<typename SetupFunc>
    void AddPass( const wchar_t* name, SetupFunc setupFunc ) {
        auto pass = std::make_unique<RenderPass>( name );
        RGBuilder builder( *this, *pass );

        // 1. Run the setup function to declare reads/writes
        setupFunc( builder, *pass );

        m_passes.push_back( std::move( pass ) );
    }

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
