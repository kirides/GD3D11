#pragma once
#include <d3d12.h>

// Enhanced-barrier support for D3D12CmdList (see D3D12StateCache.h). Kept in its own translation
// unit because the legacy-state -> (sync, access, layout) translation table is real domain logic,
// not a one-line filter forward like everything else D3D12CmdList wraps.

/** Sentinel meaning "no hint -- use the table's conservative default for this state." Not a valid
    D3D12_BARRIER_SYNC value on its own, so it can't collide with an intentional value. */
inline constexpr D3D12_BARRIER_SYNC kBarrierSyncUnspecified = static_cast<D3D12_BARRIER_SYNC>( ~0u );

/** One transition in a batched D3D12CmdList::TransitionBarriers() call. SyncBefore/SyncAfter are
    optional narrowing hints for callers that know precisely which pipeline stage(s) touch the
    resource on each side -- e.g. a UAV only ever touched from compute doesn't need the table's
    default ALL_SHADING scope. Ignored on the legacy-fallback path. */
struct D3D12ResourceTransition {
    ID3D12Resource* Resource = nullptr;
    D3D12_RESOURCE_STATES Before = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES After = D3D12_RESOURCE_STATE_COMMON;
    UINT Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    D3D12_BARRIER_SYNC SyncBefore = kBarrierSyncUnspecified;
    D3D12_BARRIER_SYNC SyncAfter = kBarrierSyncUnspecified;
};

/** The barrier recorder shared by D3D12CmdList and the RHI command list. `list7` is the enhanced-barrier
    interface when the device supports enhanced barriers, else null (legacy ResourceBarrier path). */
namespace D3D12Barriers {
    void Transition( ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList7* list7, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT subresource,
        D3D12_BARRIER_SYNC syncBeforeHint, D3D12_BARRIER_SYNC syncAfterHint );
    void Transitions( ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList7* list7,
        const D3D12ResourceTransition* transitions, UINT count );
    void UAV( ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList7* list7, ID3D12Resource* resource, D3D12_BARRIER_SYNC syncHint );
    void UAVs( ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList7* list7,
        ID3D12Resource* const* resources, UINT count, D3D12_BARRIER_SYNC syncHint );
    void Aliasing( ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList7* list7, ID3D12Resource* before,
        D3D12_RESOURCE_STATES beforeState, ID3D12Resource* after );
}
