#pragma once
#include <d3d12.h>
#include <D3D12MemAlloc.h>
#include <wrl/client.h>

class D3D12GraphicsEngine;
class D3D12PooledDescriptorHeap;

/** Physical D3D12 color render target — the D3D12 counterpart of D3D11's RenderToTextureBuffer. Owns
    an RTV (in a caller-supplied dedicated CPU-only descriptor heap) and an SRV slot in the engine's
    shader-visible bindless heap (so a later pass can sample what an earlier one rendered — the same
    "write here, read there" contract D3D11's RenderGraph passes rely on). Optionally also gets a UAV
    slot for compute-written targets. Backing memory comes from one of two places:

      * Init()        — a dedicated DEFAULT-heap committed allocation. Used by D3D12TexturePool: one
                         physical texture per pooled Description, reused across frames by matching
                         Description on Acquire() (see TexturePool.h for the rationale).
      * InitPlaced()   — a resource placed at a caller-owned offset inside an existing ID3D12Heap. Used
                         by D3D12AliasedTextureArena, whose whole point is that TWO different
                         Descriptions can share the same physical memory at different points in time
                         (see D3D12RenderGraph.h's interval-coloring pass) — something Init()'s
                         one-resource-per-Description model can't express.

    ReplaceResource() swaps a placed instance's backing resource IN PLACE, keeping the RTV heap slot
    (safe — see its own comment) while retiring the old resource + its SRV/UAV slot(s) through the
    engine's DEFERRED release queue rather than freeing them immediately: a bindless SRV/UAV descriptor
    is resolved by the GPU at DRAW time, not at CPU record time, so overwriting one while an earlier,
    not-yet-executed command list still expects its old contents would corrupt that earlier pass's
    read. Only D3D12AliasedTextureArena calls this — see its Acquire(). */
class D3D12RenderTarget {
public:
    D3D12RenderTarget() = default;
    ~D3D12RenderTarget();
    D3D12RenderTarget( const D3D12RenderTarget& ) = delete;
    D3D12RenderTarget& operator=( const D3D12RenderTarget& ) = delete;

    bool Init( ID3D12Device* device, D3D12MA::Allocator* allocator, D3D12GraphicsEngine* engine,
        D3D12PooledDescriptorHeap* rtvHeap, UINT width, UINT height, DXGI_FORMAT format, bool needsUav,
        const wchar_t* debugName );

    bool InitPlaced( ID3D12Device* device, ID3D12Resource* resource, D3D12GraphicsEngine* engine,
        D3D12PooledDescriptorHeap* rtvHeap, UINT width, UINT height, DXGI_FORMAT format, bool needsUav,
        const wchar_t* debugName );

    bool ReplaceResource( ID3D12Device* device, ID3D12Resource* newResource, UINT width, UINT height,
        DXGI_FORMAT format, bool needsUav );

    ID3D12Resource* GetResource() const { return m_Texture.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV() const { return m_Rtv; }
    UINT GetSrvSlot() const { return m_SrvSlot; }
    UINT GetUavSlot() const { return m_UavSlot; }   // 0xFFFFFFFF when not requested
    UINT GetWidth() const { return m_Width; }
    UINT GetHeight() const { return m_Height; }
    DXGI_FORMAT GetFormat() const { return m_Format; }

    /** Tracked resource state. NOT maintained automatically by the pool or the render graph — D3D12
        needs explicit barriers, and this first cut leaves them to whoever records the pass (exactly
        like every other D3D12 pass in this backend). Defaults to the creation-time state. A pass that
        transitions this resource should update State so the next pass sharing it knows where it left
        off; see D3D12RenderGraph.h for the follow-up that would automate this. Reset to RENDER_TARGET
        by ReplaceResource() too — CreatePlacedResource always creates at that state, same as Init(). */
    D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_RENDER_TARGET;

private:
    bool CreateViews( ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, bool needsUav );

    D3D12GraphicsEngine* m_Engine = nullptr;
    D3D12PooledDescriptorHeap* m_RtvHeap = nullptr;
    UINT m_RtvSlot = 0xFFFFFFFFu;

    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_Allocation;   // null for a placed instance (arena owns the heap)
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Texture;
    D3D12_CPU_DESCRIPTOR_HANDLE m_Rtv{};
    UINT m_SrvSlot = 0xFFFFFFFFu;
    UINT m_UavSlot = 0xFFFFFFFFu;
    UINT m_Width = 0, m_Height = 0;
    DXGI_FORMAT m_Format = DXGI_FORMAT_UNKNOWN;
};
