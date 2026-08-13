#pragma once
#include <d3d12.h>

// Enhanced-barrier support for D3D12CmdList (see D3D12StateCache.h). Kept in its own translation
// unit because the legacy-state -> (sync, access, layout) translation table is real domain logic,
// not a one-line filter forward like everything else D3D12CmdList wraps.
//
// Every D3D12CmdList::TransitionBarrier/TransitionBarriers/UAVBarrier/UAVBarriers call site in the
// backend speaks legacy D3D12_RESOURCE_STATES; whether the machine actually has enhanced barriers is
// decided once per process and is invisible at the call site. When it does, the state pair is mapped
// through a conservative table (broadest sync/access that's still correct for that state) unless the
// caller supplies an explicit sync-scope hint -- see SyncScope below -- to narrow it for a call site
// whose actual pipeline-stage usage is known (e.g. a UAV only ever touched from compute).

/** Sentinel meaning "no hint -- use the table's conservative default for this state." Not a valid
    D3D12_BARRIER_SYNC value on its own (every real table entry is a small handful of set bits, never
    all of them), so it can't collide with an intentional value. */
inline constexpr D3D12_BARRIER_SYNC kBarrierSyncUnspecified = static_cast<D3D12_BARRIER_SYNC>( ~0u );

/** One transition in a batched D3D12CmdList::TransitionBarriers() call. SyncBefore/SyncAfter are
    optional narrowing hints (see kBarrierSyncUnspecified) for callers that know precisely which
    pipeline stage(s) touch the resource on each side of the transition -- e.g. a bloom mip that is
    only ever read/written from a compute dispatch doesn't need the table's default ALL_SHADING scope
    for D3D12_RESOURCE_STATE_UNORDERED_ACCESS. Ignored on the legacy-fallback path: legacy transitions
    have no sync-scope concept, so an unsupported machine gets the same barrier either way. */
struct D3D12ResourceTransition {
    ID3D12Resource* Resource = nullptr;
    D3D12_RESOURCE_STATES Before = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES After = D3D12_RESOURCE_STATE_COMMON;
    UINT Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    D3D12_BARRIER_SYNC SyncBefore = kBarrierSyncUnspecified;
    D3D12_BARRIER_SYNC SyncAfter = kBarrierSyncUnspecified;
};
