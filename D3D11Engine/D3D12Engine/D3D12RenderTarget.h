#pragma once
#include <d3d12.h>
#include <D3D12MemAlloc.h>
#include <wrl/client.h>

class D3D12GraphicsEngine;
class D3D12PooledDescriptorHeap;

/** Physical D3D12 color render target owned by D3D12TexturePool — the D3D12 counterpart of D3D11's
    RenderToTextureBuffer. Owns a DEFAULT-heap 2D texture, an RTV in the pool's dedicated CPU-only
    descriptor heap, and an SRV slot in the engine's shader-visible bindless heap (so a later pass can
    sample what an earlier one rendered — the same "write here, read there" contract D3D11's RenderGraph
    passes rely on). Optionally also gets a UAV slot for compute-written targets.

    Not resizable/reinitializable in place: a resolution or format change goes through the pool, which
    simply stops matching this Description on Acquire() and lets the old entry age out (GiveTick). */
class D3D12RenderTarget {
public:
    D3D12RenderTarget() = default;
    ~D3D12RenderTarget();
    D3D12RenderTarget( const D3D12RenderTarget& ) = delete;
    D3D12RenderTarget& operator=( const D3D12RenderTarget& ) = delete;

    bool Init( ID3D12Device* device, D3D12MA::Allocator* allocator, D3D12GraphicsEngine* engine,
        D3D12PooledDescriptorHeap* rtvHeap, UINT width, UINT height, DXGI_FORMAT format, bool needsUav,
        const wchar_t* debugName );

    ID3D12Resource* GetResource() const { return m_Texture.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV() const { return m_Rtv; }
    UINT GetSrvSlot() const { return m_SrvSlot; }
    UINT GetUavSlot() const { return m_UavSlot; }   // kInvalidSlot equivalent (0xFFFFFFFF) when not requested
    UINT GetWidth() const { return m_Width; }
    UINT GetHeight() const { return m_Height; }
    DXGI_FORMAT GetFormat() const { return m_Format; }

    /** Tracked resource state. NOT maintained automatically by the pool or the render graph — D3D12
        needs explicit barriers, and this first cut leaves them to whoever records the pass (exactly
        like every other D3D12 pass in this backend). Defaults to the creation-time state. A pass that
        transitions this resource should update State so the next pass sharing it knows where it left
        off; see D3D12RenderGraph.h for the follow-up that would automate this. */
    D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_RENDER_TARGET;

private:
    D3D12GraphicsEngine* m_Engine = nullptr;
    D3D12PooledDescriptorHeap* m_RtvHeap = nullptr;
    UINT m_RtvSlot = 0xFFFFFFFFu;

    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_Allocation;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Texture;
    D3D12_CPU_DESCRIPTOR_HANDLE m_Rtv{};
    UINT m_SrvSlot = 0xFFFFFFFFu;
    UINT m_UavSlot = 0xFFFFFFFFu;
    UINT m_Width = 0, m_Height = 0;
    DXGI_FORMAT m_Format = DXGI_FORMAT_UNKNOWN;
};
