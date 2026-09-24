#pragma once
#include <d3d12.h>
#include "../RHI/Rhi.h"

// Enhanced-barrier recording for the D3D12 RHI command list: the legacy-state -> (sync, access, layout)
// translation table plus the legacy ResourceBarrier fallback.

/** Sentinel meaning "no hint -- use the table's conservative default for this state." */
inline constexpr D3D12_BARRIER_SYNC kBarrierSyncUnspecified = Rhi::kBarrierSyncUnspecified;

/** One transition in a batched TransitionBarriers() call; the renderer builds these (see Rhi.h). */
using D3D12ResourceTransition = Rhi::ResourceTransition;

/** The same transition on the native resource, for the recorder below. */
struct D3D12NativeTransition {
    ID3D12Resource* Resource = nullptr;
    D3D12_RESOURCE_STATES Before = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES After = D3D12_RESOURCE_STATE_COMMON;
    UINT Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    D3D12_BARRIER_SYNC SyncBefore = kBarrierSyncUnspecified;
    D3D12_BARRIER_SYNC SyncAfter = kBarrierSyncUnspecified;
};

/** The barrier recorder behind the RHI command list. `list7` is the enhanced-barrier
    interface when the device supports enhanced barriers, else null (legacy ResourceBarrier path). */
namespace D3D12Barriers {
    void Transition( ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList7* list7, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT subresource,
        D3D12_BARRIER_SYNC syncBeforeHint, D3D12_BARRIER_SYNC syncAfterHint );
    void Transitions( ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList7* list7,
        const D3D12NativeTransition* transitions, UINT count );
    void UAV( ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList7* list7, ID3D12Resource* resource, D3D12_BARRIER_SYNC syncHint );
    void UAVs( ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList7* list7,
        ID3D12Resource* const* resources, UINT count, D3D12_BARRIER_SYNC syncHint );
    void Aliasing( ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList7* list7, ID3D12Resource* before,
        D3D12_RESOURCE_STATES beforeState, ID3D12Resource* after );
}
