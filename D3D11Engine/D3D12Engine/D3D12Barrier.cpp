#include "../pch.h"
#include "D3D12Barrier.h"
#include "D3D12StateCache.h"
#include "../Logger.h"

// Implements the D3D12CmdList barrier methods declared in D3D12StateCache.h. Legacy
// D3D12_RESOURCE_STATES are the only thing any call site (present or future) has to think in;
// everything past that point -- enhanced-barrier availability, the sync/access/layout translation,
// buffer-vs-texture dispatch -- is handled internally and is invisible to the caller.
//
// FOUNDATION ONLY: nothing calls these methods yet (see D3D12Barrier.h). The state->(sync, access,
// layout) mapping below is deliberately conservative where a legacy state doesn't map cleanly onto a
// single enhanced-barrier concept -- broad sync scope, broad access -- because a barrier that is too
// broad is merely slower, never wrong, and the precision that enhanced barriers make possible is a
// per-call-site tuning exercise for the future migration, not this pass.

namespace {

    struct StateMapEntry {
        D3D12_RESOURCE_STATES Bit;
        D3D12_BARRIER_SYNC Sync;
        D3D12_BARRIER_ACCESS Access;
        D3D12_BARRIER_LAYOUT Layout;   // ignored when the resource is a buffer
    };

    // One row per legacy state bit this backend actually uses. Multi-bit inputs (e.g.
    // PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE) OR every matching row's sync/access
    // together; SHADER_RESOURCE is the layout for all shader-read rows so a combined mask never
    // produces a layout conflict.
    constexpr StateMapEntry kStateMap[] = {
        { D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_BARRIER_SYNC_ALL_SHADING,
            D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER, D3D12_BARRIER_LAYOUT_GENERIC_READ },
        { D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_BARRIER_SYNC_INDEX_INPUT,
            D3D12_BARRIER_ACCESS_INDEX_BUFFER, D3D12_BARRIER_LAYOUT_GENERIC_READ },
        { D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_BARRIER_SYNC_RENDER_TARGET,
            D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET },
        { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_ALL_SHADING,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS },
        { D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
            D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE },
        { D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
            D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ },
        { D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_BARRIER_SYNC_NON_PIXEL_SHADING,
            D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE },
        { D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_BARRIER_SYNC_PIXEL_SHADING,
            D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE },
        { D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_BARRIER_SYNC_VERTEX_SHADING,
            D3D12_BARRIER_ACCESS_STREAM_OUTPUT, D3D12_BARRIER_LAYOUT_COMMON },
        { D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
            D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT, D3D12_BARRIER_LAYOUT_GENERIC_READ },
        { D3D12_RESOURCE_STATE_COPY_DEST, D3D12_BARRIER_SYNC_COPY,
            D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_DEST },
        { D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_BARRIER_SYNC_COPY,
            D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_SOURCE },
        { D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_BARRIER_SYNC_RESOLVE,
            D3D12_BARRIER_ACCESS_RESOLVE_DEST, D3D12_BARRIER_LAYOUT_RESOLVE_DEST },
        { D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_BARRIER_SYNC_RESOLVE,
            D3D12_BARRIER_ACCESS_RESOLVE_SOURCE, D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE },
    };

    /** Maps a legacy D3D12_RESOURCE_STATES mask to (sync, access, layout). D3D12_RESOURCE_STATE_COMMON
        (== PRESENT == 0) has no bits to look up and is special-cased. Returns false, leaving the
        out-params untouched, if `state` carries a bit this table doesn't recognize -- the caller
        must fall back to a legacy transition for that call rather than emit a wrong barrier. */
    bool MapResourceState( D3D12_RESOURCE_STATES state, D3D12_BARRIER_SYNC& sync, D3D12_BARRIER_ACCESS& access,
        D3D12_BARRIER_LAYOUT& layout ) {
        if ( state == D3D12_RESOURCE_STATE_COMMON ) {
            sync = D3D12_BARRIER_SYNC_ALL;
            access = D3D12_BARRIER_ACCESS_NO_ACCESS;
            layout = D3D12_BARRIER_LAYOUT_COMMON;
            return true;
        }

        D3D12_BARRIER_SYNC outSync = D3D12_BARRIER_SYNC_NONE;
        D3D12_BARRIER_ACCESS outAccess = D3D12_BARRIER_ACCESS_COMMON;
        D3D12_BARRIER_LAYOUT outLayout = D3D12_BARRIER_LAYOUT_UNDEFINED;
        D3D12_RESOURCE_STATES remaining = state;

        for ( const StateMapEntry& entry : kStateMap ) {
            if ( ( remaining & entry.Bit ) != entry.Bit ) continue;
            remaining &= ~entry.Bit;
            outSync |= entry.Sync;
            outAccess |= entry.Access;
            outLayout = entry.Layout;   // every matched row in practice shares one layout family
        }

        if ( remaining != 0 ) {
#ifdef DEBUG_D3D11
            LogWarn() << "D3D12Barrier: unmapped D3D12_RESOURCE_STATES bit(s) 0x" << std::hex
                      << static_cast<UINT>( remaining ) << std::dec << "; falling back to a legacy transition.";
#endif
            return false;
        }

        sync = outSync;
        access = outAccess;
        layout = outLayout;
        return true;
    }

    bool IsBufferResource( ID3D12Resource* resource ) {
        return resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    }

    void LegacyTransition( ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT subresource ) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = subresource;
        list->ResourceBarrier( 1, &barrier );
    }

} // namespace

void D3D12CmdList::TransitionBarrier( ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after, UINT subresource ) {
    if ( s_DeviceSupportsEnhancedBarriers && List7() ) {
        D3D12_BARRIER_SYNC syncBefore, syncAfter;
        D3D12_BARRIER_ACCESS accessBefore, accessAfter;
        D3D12_BARRIER_LAYOUT layoutBefore, layoutAfter;
        if ( MapResourceState( before, syncBefore, accessBefore, layoutBefore )
            && MapResourceState( after, syncAfter, accessAfter, layoutAfter ) ) {
            if ( IsBufferResource( resource ) ) {
                D3D12_BUFFER_BARRIER bufferBarrier = { syncBefore, syncAfter, accessBefore, accessAfter,
                    resource, 0, UINT64_MAX };
                D3D12_BARRIER_GROUP group = {};
                group.Type = D3D12_BARRIER_TYPE_BUFFER;
                group.NumBarriers = 1;
                group.pBufferBarriers = &bufferBarrier;
                List7()->Barrier( 1, &group );
            } else {
                // NumMipLevels/NumArraySlices/NumPlanes == 0 means "all", per the enhanced-barrier spec.
                D3D12_BARRIER_SUBRESOURCE_RANGE range = { 0, 0, 0, 0, 0, 0 };
                if ( subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ) {
                    range = { subresource, 1, 0, 1, 0, 1 };
                }
                D3D12_TEXTURE_BARRIER textureBarrier = { syncBefore, syncAfter, accessBefore, accessAfter,
                    layoutBefore, layoutAfter, resource, range, D3D12_TEXTURE_BARRIER_FLAG_NONE };
                D3D12_BARRIER_GROUP group = {};
                group.Type = D3D12_BARRIER_TYPE_TEXTURE;
                group.NumBarriers = 1;
                group.pTextureBarriers = &textureBarrier;
                List7()->Barrier( 1, &group );
            }
            return;
        }
    }
    LegacyTransition( m_List.Get(), resource, before, after, subresource );
}

void D3D12CmdList::TransitionBarriers( std::initializer_list<D3D12ResourceTransition> transitions ) {
    // Kept as a simple loop over the single-transition path rather than batching into one Barrier()/
    // ResourceBarrier() call -- this is unused foundation code with no perf-sensitive caller yet, and
    // a real batching implementation needs separate texture/buffer/legacy arrays which is easy to add
    // once an actual multi-transition call site exists to shape the API around.
    for ( const D3D12ResourceTransition& t : transitions ) {
        TransitionBarrier( t.Resource, t.Before, t.After, t.Subresource );
    }
}

void D3D12CmdList::UAVBarrier( ID3D12Resource* resource ) {
    if ( s_DeviceSupportsEnhancedBarriers && List7() ) {
        constexpr D3D12_BARRIER_SYNC kSync = D3D12_BARRIER_SYNC_ALL_SHADING;
        constexpr D3D12_BARRIER_ACCESS kAccess = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        if ( resource && IsBufferResource( resource ) ) {
            D3D12_BUFFER_BARRIER bufferBarrier = { kSync, kSync, kAccess, kAccess, resource, 0, UINT64_MAX };
            D3D12_BARRIER_GROUP group = {};
            group.Type = D3D12_BARRIER_TYPE_BUFFER;
            group.NumBarriers = 1;
            group.pBufferBarriers = &bufferBarrier;
            List7()->Barrier( 1, &group );
        } else if ( resource ) {
            D3D12_BARRIER_SUBRESOURCE_RANGE range = { 0, 0, 0, 0, 0, 0 };
            D3D12_TEXTURE_BARRIER textureBarrier = { kSync, kSync, kAccess, kAccess,
                D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, resource, range, D3D12_TEXTURE_BARRIER_FLAG_NONE };
            D3D12_BARRIER_GROUP group = {};
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = 1;
            group.pTextureBarriers = &textureBarrier;
            List7()->Barrier( 1, &group );
        } else {
            D3D12_GLOBAL_BARRIER globalBarrier = { kSync, kSync, kAccess, kAccess };
            D3D12_BARRIER_GROUP group = {};
            group.Type = D3D12_BARRIER_TYPE_GLOBAL;
            group.NumBarriers = 1;
            group.pGlobalBarriers = &globalBarrier;
            List7()->Barrier( 1, &group );
        }
        return;
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    m_List->ResourceBarrier( 1, &barrier );
}

void D3D12CmdList::UAVBarriers( std::initializer_list<ID3D12Resource*> resources ) {
    for ( ID3D12Resource* resource : resources ) {
        UAVBarrier( resource );
    }
}

void D3D12CmdList::AliasingBarrier( ID3D12Resource* before, ID3D12Resource* after ) {
    // Enhanced barriers don't model resource aliasing 1:1 (aliasing is instead expressed through
    // D3D12_BARRIER_LAYOUT_UNDEFINED on the first use of the aliased resource); no call site needs
    // that yet, so this stays on the well-understood legacy path unconditionally rather than guessing
    // at a translation nothing has exercised.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Aliasing.pResourceBefore = before;
    barrier.Aliasing.pResourceAfter = after;
    m_List->ResourceBarrier( 1, &barrier );
}
