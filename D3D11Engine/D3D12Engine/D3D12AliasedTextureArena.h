#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <vector>
#include "D3D12RenderTarget.h"
#include "D3D12PooledDescriptorHeap.h"

class D3D12GraphicsEngine;
class D3D12CmdList;

/** Backing store for D3D12RenderGraph's transient textures: ONE DEFAULT-heap ID3D12Heap that multiple,
    non-overlapping-lifetime resources are placed into at different byte offsets via CreatePlacedResource
    — the thing D3D11 fundamentally cannot do (D3D11 has no placed-resource / heap-aliasing API; every
    texture in D3D11's TexturePool, and D3D12's own D3D12TexturePool, gets its own dedicated memory).

    D3D12RenderGraph::Compile() runs a simple interval-coloring pass over the already-computed
    firstPass/lastPass lifetimes (AssignSlots) to decide WHICH byte offset each transient resource is
    given; this class only does the mechanical part — turning (offset, desc) into a live ID3D12Resource,
    including the mandatory ALIASING BARRIER between whatever resource previously occupied that offset
    (from earlier in the same frame, or held over from last frame) and the new one. Callers never need to
    barrier manually; see Acquire().

    Deliberately on the LEGACY D3D12_RESOURCE_BARRIER_TYPE_ALIASING path throughout (both here and in
    D3D12CmdList::AliasingBarrier) rather than the enhanced-barrier UNDEFINED-layout model — see
    AliasingBarrier's own comment for why. Resources are created via the plain (legacy-state)
    CreatePlacedResource to match; D3D12CmdList::TransitionBarrier still transparently upgrades their
    later transitions to enhanced barriers when the device supports them (see D3D12Barrier.cpp), so this
    does not opt a resource out of enhanced-barrier tracking for anything except the aliasing hazard
    itself. */
class D3D12AliasedTextureArena {
public:
    // Arena capacity. Fixed rather than growable — see D3D12PooledDescriptorHeap for the same reasoning
    // (CLAUDE.md's per-frame-allocation rule: size once, log+fail past the cap rather than reallocating
    // a live heap mid-session). 256 MB comfortably covers a Forward+ scratch set (a handful of full-res
    // R8/RG16F/RGBA16F targets, sized generously below the peak they'd need even WITHOUT aliasing) with
    // headroom; overflow just means a slot's resource creation fails and that pass' output is skipped,
    // exactly like today's DoF/Motion resource-creation-failure paths.
    static constexpr UINT64 kArenaCapacityBytes = 256ull * 1024 * 1024;
    // RTV heap sized like D3D12TexturePool's — a handful of transient targets per graph in practice.
    static constexpr UINT kMaxSlots = 64;

    bool Attach( D3D12GraphicsEngine& engine );

    /** Resets the persistent bump-allocation cursor (see ReserveBumpRange) for a new frame. Call ONCE per
        frame, before the first D3D12RenderGraph::Compile() call that might touch this arena — e.g.
        D3D12GraphicsEngine::OnBeginFrame, which fires every frame in the menu and in-game alike. */
    void BeginFrame() { m_FrameBumpOffset = 0; }

    /** {size, alignment} D3D12 would need for a texture matching this description, WITHOUT creating it
        — what D3D12RenderGraph's interval colorer sizes its packing against. */
    void GetAllocationInfo( UINT width, UINT height, DXGI_FORMAT format, bool needsUav, UINT64& outSize, UINT64& outAlignment ) const;

    /** Bump-allocates `size` bytes (aligned to `alignment`) from a cursor that is PERSISTENT ACROSS EVERY
        D3D12RenderGraph that touches this arena in the same frame — not reset per graph, only per
        BeginFrame(). This is the reason multiple independent per-function local graphs (DoF's, the god-ray
        pass's, ...) don't stomp on each other: if each graph's Compile() instead restarted its own bump
        pointer at 0, the FIRST resource of every graph would collide at the same offset and every one of
        them would see its occupant change EVERY SINGLE CALL — real GPU-correct (an aliasing barrier would
        still fire) but real waste (a fresh CreatePlacedResource + view pair every frame for every one of
        them, exactly the per-frame-allocation churn CLAUDE.md's central rule exists to avoid). Sharing one
        monotonically-advancing cursor for the whole frame means each graph's resources land at STABLE,
        non-overlapping offsets that repeat identically frame after frame (the post-fx call order is fixed),
        so after the first frame every Acquire() is a same-desc cache hit — steady state, no churn. Returns
        UINT64_MAX if the request would exceed kArenaCapacityBytes (caller logs and skips the resource). */
    UINT64 ReserveBumpRange( UINT64 size, UINT64 alignment );

    /** Places (or reuses) a texture at the given byte offset for THIS pass, as decided by
        D3D12RenderGraph::Compile()'s interval coloring. If a different resource previously occupied
        this offset (a different Description at the same offset, either from earlier this exact frame
        or held over from last frame) a fresh placed resource is created and an aliasing barrier against
        the previous occupant is recorded on cmdList before returning. If the same Description already
        occupies the offset, the existing resource is returned untouched — no barrier, no new views,
        exactly like D3D12TexturePool's steady-state case. Returns nullptr (logged once) on failure
        (arena exhausted, format+size rejected by the driver, ...). */
    D3D12RenderTarget* Acquire( UINT64 slotOffset, UINT width, UINT height, DXGI_FORMAT format, bool needsUav, D3D12CmdList& cmdList );

    void Clear();   // drops every placed resource + their SRV/RTV slots (resize / level change); keeps the heap

private:
    struct Slot {
        UINT64 Offset = 0;
        std::unique_ptr<D3D12RenderTarget> Texture;   // created once at first use; ReplaceResource() thereafter
        UINT Width = 0, Height = 0;
        DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
        bool NeedsUav = false;
    };

    Slot* FindOrCreateSlot( UINT64 offset );

    ID3D12Device* m_Device = nullptr;
    D3D12GraphicsEngine* m_Engine = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Heap> m_Heap;
    D3D12PooledDescriptorHeap m_RtvHeap;

    std::vector<std::unique_ptr<Slot>> m_Slots;
    bool m_LoggedExhaustion = false;
    UINT64 m_FrameBumpOffset = 0;   // see ReserveBumpRange / BeginFrame
};
