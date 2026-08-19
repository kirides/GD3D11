#pragma once
#include <d3d12.h>
#include <D3D12MemAlloc.h>
#include <cstdint>
#include <memory>
#include <vector>
#include "D3D12RenderTarget.h"
#include "D3D12PooledDescriptorHeap.h"

class D3D12GraphicsEngine;

/** Free-list pool of transient D3D12 render targets — the D3D12 counterpart of D3D11's TexturePool
    (see TexturePool.h). Backs D3D12RenderGraph: a pass asks for a Description and gets back a texture
    that may be a fresh allocation or one recycled from an earlier pass whose lifetime has already
    ended (see D3D12RenderGraph::Compile/Execute). Owns one dedicated CPU-only RTV heap, fixed at
    kMaxPooledTargets slots (see D3D12PooledDescriptorHeap for why fixed-capacity). */
class D3D12TexturePool {
public:
    struct Description {
        UINT Width = 0, Height = 0;
        DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
        bool NeedsUav = false;

        bool operator==( const Description& other ) const {
            return Width == other.Width && Height == other.Height
                && Format == other.Format && NeedsUav == other.NeedsUav;
        }
    };

    struct PooledTexture {
        std::unique_ptr<D3D12RenderTarget> Texture;
        Description Desc;
        uint64_t LastFrameUsed;
        bool InUse;
    };

    // RAII handle: releasing it just clears the pooled entry's InUse flag rather than destroying the
    // resource, so "returning" a texture to the pool is O(1) and allocation-free. Mirrors TexturePool::Handle.
    class Handle {
    public:
        Handle() noexcept = default;
        Handle( std::nullptr_t ) noexcept {}
        explicit Handle( PooledTexture* entry ) noexcept : m_entry( entry ) {}
        Handle( const Handle& ) = delete;
        Handle& operator=( const Handle& ) = delete;
        Handle( Handle&& other ) noexcept : m_entry( other.m_entry ) { other.m_entry = nullptr; }
        Handle& operator=( Handle&& other ) noexcept {
            if ( this != &other ) {
                Release();
                m_entry = other.m_entry;
                other.m_entry = nullptr;
            }
            return *this;
        }
        ~Handle() { Release(); }

        D3D12RenderTarget* operator->() const { return m_entry->Texture.get(); }
        D3D12RenderTarget& operator*() const { return *m_entry->Texture; }
        D3D12RenderTarget* get() const { return m_entry ? m_entry->Texture.get() : nullptr; }
        explicit operator bool() const { return m_entry != nullptr; }
        friend bool operator==( const Handle& h, std::nullptr_t ) { return h.m_entry == nullptr; }
        friend bool operator!=( const Handle& h, std::nullptr_t ) { return h.m_entry != nullptr; }
        void reset() { Release(); m_entry = nullptr; }

    private:
        void Release() { if ( m_entry ) m_entry->InUse = false; }
        PooledTexture* m_entry = nullptr;
    };

    // RTV heap is a plain CPU-only heap sized once in Attach() — see D3D12PooledDescriptorHeap for why
    // this stays fixed-capacity rather than growable.
    static constexpr UINT kMaxPooledTargets = 64;

    /** One-time setup: grabs the device/allocator off the engine and creates the RTV heap. Safe to call
        even though nothing consumes the pool yet (see D3D12RenderGraph.h) — this is pure infrastructure. */
    bool Attach( D3D12GraphicsEngine& engine );

    Handle Acquire( const Description& desc );

    void GiveTick();   // call once per frame (ages out long-unused entries)
    void Clear();      // drops every pooled texture (resize / level change)

    size_t GetActiveCount() const;

private:
    ID3D12Device* m_Device = nullptr;
    D3D12MA::Allocator* m_Allocator = nullptr;
    D3D12GraphicsEngine* m_Engine = nullptr;
    D3D12PooledDescriptorHeap m_RtvHeap;

    std::vector<std::unique_ptr<PooledTexture>> m_Pool;
    uint64_t m_CurrentFrame = 0;
    static constexpr uint64_t kMaxUnusedFrames = 60;
};

using D3D12TextureHandle = D3D12TexturePool::Handle;
