#pragma once
#include <d3d12.h>

// Enhanced-barrier support for D3D12CmdList (see D3D12StateCache.h). Kept in its own translation
// unit because the legacy-state -> (sync, access, layout) translation table is real domain logic,
// not a one-line filter forward like everything else D3D12CmdList wraps.
//
// This is a FOUNDATION ONLY: nothing in the backend calls D3D12CmdList::TransitionBarrier /
// UAVBarrier / AliasingBarrier yet. All 146 existing call sites keep using the legacy
// CD3DX12_RESOURCE_BARRIER + ResourceBarrier() path untouched. New/future call sites can adopt the
// methods declared here without needing to know whether enhanced barriers are actually available on
// the running machine -- that's decided once per process and handled internally.

/** One transition in a batched D3D12CmdList::TransitionBarriers() call. */
struct D3D12ResourceTransition {
    ID3D12Resource* Resource = nullptr;
    D3D12_RESOURCE_STATES Before = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES After = D3D12_RESOURCE_STATE_COMMON;
    UINT Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
};
