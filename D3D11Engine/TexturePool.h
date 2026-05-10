#pragma once
#include "D3D11_Helpers.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include <algorithm>
#include "RenderToTextureBuffer.h"

using Microsoft::WRL::ComPtr;

namespace {
    using TextureHandle = std::unique_ptr<RenderToTextureBuffer, std::function<void( RenderToTextureBuffer* )>>;
    using DepthStencilHandle = std::unique_ptr<RenderToDepthStencilBuffer, std::function<void( RenderToDepthStencilBuffer* )>>;
}

class TexturePool {
public:
    struct Description {
        int Width, Height;
        DXGI_FORMAT Format;
        DXGI_USAGE textureFlags; // Custom flags for special usage (e.g., render target, shader resource, etc.)

        // Comparator for finding matching textures in the pool
        bool operator==( const Description& other ) const {
            return Width == other.Width 
                && Height == other.Height 
                && Format == other.Format
                && textureFlags == other.textureFlags;
        }
    };

private:
    struct PooledTexture {
        std::shared_ptr<RenderToTextureBuffer> Texture;
        Description Desc;
        uint64_t LastFrameUsed;
        bool InUse;
    };

    ID3D11Device* m_device;
    std::vector<std::unique_ptr<PooledTexture>> m_pool;
    uint64_t m_currentFrame = 0;
    const uint64_t m_maxUnusedFrames = 60; // Auto-clear threshold

public:
    TexturePool( ID3D11Device* device ) : m_device( device ) {}

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
            m_pool.push_back( std::make_unique<PooledTexture>( PooledTexture{ std::make_shared<RenderToTextureBuffer>(m_device, desc.Width, desc.Height, desc.Format, nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1, desc.textureFlags), desc, m_currentFrame, true } ) );
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
        return TextureHandle( found->Texture.get(), [found]( RenderToTextureBuffer* ) {
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


class DepthStencilPool {
public:
    struct Description {
        UINT Width, Height;
        DXGI_FORMAT Format;
        DXGI_FORMAT DSVFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT SRVFormat = DXGI_FORMAT_UNKNOWN;
        UINT ArraySize = 1;

        // Comparator for finding matching depth buffers in the pool
        bool operator==( const Description& other ) const {
            return Width == other.Width &&
                Height == other.Height &&
                Format == other.Format &&
                DSVFormat == other.DSVFormat &&
                SRVFormat == other.SRVFormat &&
                ArraySize == other.ArraySize;
        }
    };

private:
    struct PooledDepthStencil {
        std::shared_ptr<RenderToDepthStencilBuffer> Buffer;
        Description Desc;
        uint64_t LastFrameUsed;
        bool InUse;
    };

    ID3D11Device* m_device;
    std::vector<std::unique_ptr<PooledDepthStencil>> m_pool;
    uint64_t m_currentFrame = 0;
    const uint64_t m_maxUnusedFrames = 60; // Auto-clear threshold

public:
    DepthStencilPool( ID3D11Device* device ) : m_device( device ) {}

    DepthStencilHandle Acquire( const Description& desc ) {
        PooledDepthStencil* found = nullptr;

        for ( auto& entry : m_pool ) {
            if ( !entry->InUse && entry->Desc == desc ) {
                found = entry.get();
                break;
            }
        }

        if ( !found ) {
            // Create new depth stencil buffer if none available in pool
            m_pool.push_back( std::make_unique<PooledDepthStencil>( PooledDepthStencil{
                std::make_shared<RenderToDepthStencilBuffer>( m_device, desc.Width, desc.Height, desc.Format, nullptr, desc.DSVFormat, desc.SRVFormat, desc.ArraySize ),
                desc,
                m_currentFrame,
                true
                } ) );
            found = m_pool.back().get();

#if defined(_DEBUG) || defined(PROFILE)|| defined(DEBUG_D3D11)
            static uint64_t dsCounter = 0;
            dsCounter++;
            std::string prefix = "DepthStencilPool_" + std::to_string( dsCounter );
            SetDebugName( found->Buffer->GetTexture().Get(), prefix + "_TEX" );
            SetDebugName( found->Buffer->GetShaderResView().Get(), prefix + "_SRV" );
            SetDebugName( found->Buffer->GetDepthStencilView().Get(), prefix + "_DSV" );

            if ( desc.ArraySize == 6 ) {
                for ( UINT i = 0; i < 6; i++ ) {
                    SetDebugName( found->Buffer->GetDSVCubemapFace( i ).Get(), prefix + "_DSV_Face" + std::to_string( i ) );
                }
            }
#endif
        }

        found->InUse = true;
        found->LastFrameUsed = m_currentFrame;

        // Return RAII handle with custom deleter to "return" to pool
        return DepthStencilHandle( found->Buffer.get(), [found]( RenderToDepthStencilBuffer* ) {
            found->InUse = false;
        } );
    }

    void GiveTick() {
        m_currentFrame++;

        // we won't have many unique descriptors
        const int MAX_UNIQUE_DESCS = 8;
        struct DescTracker {
            Description desc;
            int count = 0;
        };
        DescTracker trackers[MAX_UNIQUE_DESCS];
        int uniqueDescCount = 0;

        // Remove buffers that haven't been touched in 'm_maxUnusedFrames'
        m_pool.erase( std::remove_if( m_pool.begin(), m_pool.end(), [&]( const auto& entry ) {
            if ( entry->InUse ) return false; // Always keep active textures

            int trackerIdx = -1;
            for ( int i = 0; i < uniqueDescCount; ++i ) {
                if ( trackers[i].desc == entry->Desc ) {
                    trackerIdx = i;
                    break;
                }
            }

            if ( trackerIdx == -1 && uniqueDescCount < MAX_UNIQUE_DESCS ) {
                trackerIdx = uniqueDescCount++;
                trackers[trackerIdx].desc = entry->Desc;
            }

            int maxKeepAlive = 0; // Strict default for unknown/large formats

            // Point Light Shadow Cubemaps (ArraySize == 6)
            if ( entry->Desc.ArraySize == 6 ) {
                // ensure we keep a few of those around
                // TODO: only if we have dynamic shadows enabled maybe? but players would always atleast want static pointlights anyways.
                if ( entry->Desc.Width <= 64 ) {
                    maxKeepAlive = 16; // Distant/fallback shadows: keep plenty to avoid thrashing
                } else if ( entry->Desc.Width <= 128 ) {
                    maxKeepAlive = 8;  // Medium distance
                } else if ( entry->Desc.Width <= 256 ) {
                    maxKeepAlive = 4;  // Close shadows: expensive, keep fewer
                }
            }

            // Evaluate if we should protect this texture
            if ( trackerIdx != -1 ) {
                trackers[trackerIdx].count++;

                if ( trackers[trackerIdx].count <= maxKeepAlive ) {
                    // Refresh timestamp to protect it from aging out while in the safe zone
                    entry->LastFrameUsed = m_currentFrame;
                    return false;
                }
            }

            // If the texture is not protected, execute it if it's old
            return (m_currentFrame - entry->LastFrameUsed > m_maxUnusedFrames);

            } ), m_pool.end() );
    }

    void Clear() {
        // prune cache on resize or level change
        m_pool.clear();
    }

    size_t GetActiveCount() const {
        size_t count = 0;
        for ( const auto& e : m_pool ) if ( e->InUse ) count++;
        return count;
    }
};
