#pragma once
#include "D3D11_Helpers.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include <algorithm>
#include "RenderToTextureBuffer.h"

using Microsoft::WRL::ComPtr;

using TextureHandle = std::unique_ptr<RenderToTextureBuffer, std::function<void( RenderToTextureBuffer* )>>;

class TexturePool {
public:
    struct Description {
        int Width, Height;
        DXGI_FORMAT Format;

        // Comparator for finding matching textures in the pool
        bool operator==( const Description& other ) const {
            return Width == other.Width && Height == other.Height && Format == other.Format;
        }
    };

private:
    struct PooledTexture {
        std::shared_ptr<RenderToTextureBuffer> Texture;
        Description Desc;
        uint64_t LastFrameUsed;
        bool InUse;
    };

    ID3D11Device1* m_device;
    std::vector<std::unique_ptr<PooledTexture>> m_pool;
    uint64_t m_currentFrame = 0;
    const uint64_t m_maxUnusedFrames = 60; // Auto-clear threshold

public:
    TexturePool( ID3D11Device1* device ) : m_device( device ) {}

    // The RAII handle the user holds
    // using TextureHandle = std::unique_ptr<RenderToTextureBuffer, std::function<void( RenderToTextureBuffer* )>>;

    TextureHandle Acquire( const Description& desc ) {
        PooledTexture* found = nullptr;

        for ( auto& entry : m_pool ) {
            if ( !entry->InUse && entry->Desc == desc ) {
                found = entry.get();
                break;
            }
        }

        if ( !found ) {
            // Create new texture if none available in pool
            m_pool.push_back( std::make_unique<PooledTexture>( PooledTexture{ std::make_shared<RenderToTextureBuffer>(m_device, desc.Width, desc.Height, desc.Format), desc, m_currentFrame, true } ) );
            found = m_pool.back().get();
#if defined(_DEBUG) || defined(PROFILE)|| defined(DEBUG_D3D11)
            static uint64_t textureCounter = 0;
            textureCounter++;
            std::string prefix = "TexturePool_" + std::to_string( textureCounter );
            SetDebugName( found->Texture->GetTexture().Get(), prefix+"_TEX" );
            SetDebugName( found->Texture->GetShaderResView().Get(), prefix + "_SRV" );
            SetDebugName( found->Texture->GetRenderTargetView().Get(), prefix + "_RTV" );
#endif
        }

        found->InUse = true;
        found->LastFrameUsed = m_currentFrame;

        // Return RAII handle with custom deleter to "return" to pool
        return TextureHandle( found->Texture.get(), [this, found]( RenderToTextureBuffer* ) {
            found->InUse = false;
        } );
    }

    void GiveTick() {
        m_currentFrame++;

        // Remove textures that haven't been touched in 'm_maxUnusedFrames'
        m_pool.erase( std::remove_if( m_pool.begin(), m_pool.end(), [this]( const auto& entry ) {
            return !entry->InUse && (m_currentFrame - entry->LastFrameUsed > m_maxUnusedFrames);
            } ), m_pool.end() );
    }

    void Clear() {
        // prune texture cache on resize.
        m_pool.erase( std::remove_if( m_pool.begin(), m_pool.end(), []( const auto& entry ) { return true; } ), m_pool.end() );
    }

    size_t GetActiveCount() const {
        size_t count = 0;
        for ( const auto& e : m_pool ) if ( e->InUse ) count++;
        return count;
    }
};
