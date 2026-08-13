#include "../pch.h"
#include "D3D12Barrier.h"
#include "D3D12StateCache.h"
#include "../Logger.h"
#include <d3dx12_barriers.h>

// Implements the D3D12CmdList barrier methods declared in D3D12StateCache.h. Legacy
// D3D12_RESOURCE_STATES are the only thing any call site has to think in; everything past that
// point -- enhanced-barrier availability, the sync/access/layout translation, buffer-vs-texture
// dispatch -- is handled internally and is invisible to the caller.
//
// The state->(sync, access, layout) mapping below is deliberately conservative where a legacy state
// doesn't map cleanly onto a single enhanced-barrier concept -- broad sync scope, broad access --
// because a barrier that is too broad is merely slower, never wrong. Tightening individual call
// sites' sync scopes is a follow-up tuning pass, not part of this migration.

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

    CD3DX12_BARRIER_SUBRESOURCE_RANGE SubresourceRange( UINT subresource ) {
        // NumMipLevels/NumArraySlices/NumPlanes == 0 means "all", per the enhanced-barrier spec.
        if ( subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ) return CD3DX12_BARRIER_SUBRESOURCE_RANGE( 0, 0, 0, 0, 0, 0 );
        return CD3DX12_BARRIER_SUBRESOURCE_RANGE( subresource, 1, 0, 1, 0, 1 );
    }

    // Batch cap for TransitionBarriers()/UAVBarriers(): every call site in this backend batches a
    // handful of resources ahead of one dispatch/draw (post-FX ping-pong targets, cull UAV sets),
    // never more than a few. Fixed-size stack storage keeps this off the per-frame allocation path
    // (see CLAUDE.md's per-frame allocation rule); a batch that somehow exceeds the cap logs a
    // warning and degrades to one Barrier()/ResourceBarrier() call per element instead of silently
    // dropping any transition.
    constexpr UINT kMaxBatchedBarriers = 16;

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
                CD3DX12_BUFFER_BARRIER bufferBarrier( syncBefore, syncAfter, accessBefore, accessAfter, resource );
                CD3DX12_BARRIER_GROUP group( 1u, static_cast<const D3D12_BUFFER_BARRIER*>( &bufferBarrier ) );
                List7()->Barrier( 1, &group );
            } else {
                CD3DX12_TEXTURE_BARRIER textureBarrier( syncBefore, syncAfter, accessBefore, accessAfter,
                    layoutBefore, layoutAfter, resource, SubresourceRange( subresource ) );
                CD3DX12_BARRIER_GROUP group( 1u, static_cast<const D3D12_TEXTURE_BARRIER*>( &textureBarrier ) );
                List7()->Barrier( 1, &group );
            }
            return;
        }
    }
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition( resource, before, after, subresource );
    m_List->ResourceBarrier( 1, &barrier );
}

void D3D12CmdList::TransitionBarriers( const D3D12ResourceTransition* transitions, UINT count ) {
    if ( count == 0 ) return;
    if ( count > kMaxBatchedBarriers ) {
#ifdef DEBUG_D3D11
        LogWarn() << "D3D12Barrier: TransitionBarriers batch of " << count
                  << " exceeds kMaxBatchedBarriers (" << kMaxBatchedBarriers << "); issuing one barrier per element.";
#endif
        for ( UINT i = 0; i < count; ++i ) TransitionBarrier( transitions[i].Resource, transitions[i].Before, transitions[i].After, transitions[i].Subresource );
        return;
    }

    if ( s_DeviceSupportsEnhancedBarriers && List7() ) {
        D3D12_TEXTURE_BARRIER textureBarriers[kMaxBatchedBarriers];
        D3D12_BUFFER_BARRIER bufferBarriers[kMaxBatchedBarriers];
        UINT numTexture = 0, numBuffer = 0;
        bool ok = true;
        for ( UINT i = 0; i < count; ++i ) {
            const D3D12ResourceTransition& t = transitions[i];
            D3D12_BARRIER_SYNC syncBefore, syncAfter;
            D3D12_BARRIER_ACCESS accessBefore, accessAfter;
            D3D12_BARRIER_LAYOUT layoutBefore, layoutAfter;
            if ( !MapResourceState( t.Before, syncBefore, accessBefore, layoutBefore )
                || !MapResourceState( t.After, syncAfter, accessAfter, layoutAfter ) ) {
                ok = false;
                break;
            }
            if ( IsBufferResource( t.Resource ) ) {
                bufferBarriers[numBuffer++] = CD3DX12_BUFFER_BARRIER( syncBefore, syncAfter, accessBefore, accessAfter, t.Resource );
            } else {
                textureBarriers[numTexture++] = CD3DX12_TEXTURE_BARRIER( syncBefore, syncAfter, accessBefore, accessAfter,
                    layoutBefore, layoutAfter, t.Resource, SubresourceRange( t.Subresource ) );
            }
        }
        if ( ok ) {
            D3D12_BARRIER_GROUP groups[2];
            UINT numGroups = 0;
            if ( numTexture > 0 ) groups[numGroups++] = CD3DX12_BARRIER_GROUP( numTexture, textureBarriers );
            if ( numBuffer > 0 ) groups[numGroups++] = CD3DX12_BARRIER_GROUP( numBuffer, bufferBarriers );
            if ( numGroups > 0 ) List7()->Barrier( numGroups, groups );
            return;
        }
        // A mapping failure partway through: fall through to the legacy batch below rather than
        // issuing the enhanced barriers already built and skipping the rest.
    }

    D3D12_RESOURCE_BARRIER legacyBarriers[kMaxBatchedBarriers];
    UINT numLegacy = 0;
    for ( UINT i = 0; i < count; ++i ) {
        const D3D12ResourceTransition& t = transitions[i];
        legacyBarriers[numLegacy++] = CD3DX12_RESOURCE_BARRIER::Transition( t.Resource, t.Before, t.After, t.Subresource );
    }
    m_List->ResourceBarrier( numLegacy, legacyBarriers );
}

void D3D12CmdList::UAVBarrier( ID3D12Resource* resource ) {
    if ( s_DeviceSupportsEnhancedBarriers && List7() ) {
        constexpr D3D12_BARRIER_SYNC kSync = D3D12_BARRIER_SYNC_ALL_SHADING;
        constexpr D3D12_BARRIER_ACCESS kAccess = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        if ( resource && IsBufferResource( resource ) ) {
            CD3DX12_BUFFER_BARRIER bufferBarrier( kSync, kSync, kAccess, kAccess, resource );
            CD3DX12_BARRIER_GROUP group( 1u, static_cast<const D3D12_BUFFER_BARRIER*>( &bufferBarrier ) );
            List7()->Barrier( 1, &group );
        } else if ( resource ) {
            CD3DX12_TEXTURE_BARRIER textureBarrier( kSync, kSync, kAccess, kAccess,
                D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, resource,
                CD3DX12_BARRIER_SUBRESOURCE_RANGE( 0, 0, 0, 0, 0, 0 ) );
            CD3DX12_BARRIER_GROUP group( 1u, static_cast<const D3D12_TEXTURE_BARRIER*>( &textureBarrier ) );
            List7()->Barrier( 1, &group );
        } else {
            CD3DX12_GLOBAL_BARRIER globalBarrier( kSync, kSync, kAccess, kAccess );
            CD3DX12_BARRIER_GROUP group( 1u, static_cast<const D3D12_GLOBAL_BARRIER*>( &globalBarrier ) );
            List7()->Barrier( 1, &group );
        }
        return;
    }
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV( resource );
    m_List->ResourceBarrier( 1, &barrier );
}

void D3D12CmdList::UAVBarriers( ID3D12Resource* const* resources, UINT count ) {
    if ( count == 0 ) return;
    if ( count > kMaxBatchedBarriers ) {
#ifdef DEBUG_D3D11
        LogWarn() << "D3D12Barrier: UAVBarriers batch of " << count
                  << " exceeds kMaxBatchedBarriers (" << kMaxBatchedBarriers << "); issuing one barrier per element.";
#endif
        for ( UINT i = 0; i < count; ++i ) UAVBarrier( resources[i] );
        return;
    }

    if ( s_DeviceSupportsEnhancedBarriers && List7() ) {
        constexpr D3D12_BARRIER_SYNC kSync = D3D12_BARRIER_SYNC_ALL_SHADING;
        constexpr D3D12_BARRIER_ACCESS kAccess = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        D3D12_TEXTURE_BARRIER textureBarriers[kMaxBatchedBarriers];
        D3D12_BUFFER_BARRIER bufferBarriers[kMaxBatchedBarriers];
        D3D12_GLOBAL_BARRIER globalBarriers[kMaxBatchedBarriers];
        UINT numTexture = 0, numBuffer = 0, numGlobal = 0;
        for ( UINT i = 0; i < count; ++i ) {
            ID3D12Resource* resource = resources[i];
            if ( !resource ) {
                globalBarriers[numGlobal++] = CD3DX12_GLOBAL_BARRIER( kSync, kSync, kAccess, kAccess );
            } else if ( IsBufferResource( resource ) ) {
                bufferBarriers[numBuffer++] = CD3DX12_BUFFER_BARRIER( kSync, kSync, kAccess, kAccess, resource );
            } else {
                textureBarriers[numTexture++] = CD3DX12_TEXTURE_BARRIER( kSync, kSync, kAccess, kAccess,
                    D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, resource,
                    CD3DX12_BARRIER_SUBRESOURCE_RANGE( 0, 0, 0, 0, 0, 0 ) );
            }
        }
        D3D12_BARRIER_GROUP groups[3];
        UINT numGroups = 0;
        if ( numTexture > 0 ) groups[numGroups++] = CD3DX12_BARRIER_GROUP( numTexture, textureBarriers );
        if ( numBuffer > 0 ) groups[numGroups++] = CD3DX12_BARRIER_GROUP( numBuffer, bufferBarriers );
        if ( numGlobal > 0 ) groups[numGroups++] = CD3DX12_BARRIER_GROUP( numGlobal, globalBarriers );
        if ( numGroups > 0 ) List7()->Barrier( numGroups, groups );
        return;
    }

    D3D12_RESOURCE_BARRIER legacyBarriers[kMaxBatchedBarriers];
    UINT numLegacy = 0;
    for ( UINT i = 0; i < count; ++i ) {
        legacyBarriers[numLegacy++] = CD3DX12_RESOURCE_BARRIER::UAV( resources[i] );
    }
    m_List->ResourceBarrier( numLegacy, legacyBarriers );
}

void D3D12CmdList::AliasingBarrier( ID3D12Resource* before, ID3D12Resource* after ) {
    // Enhanced barriers don't model resource aliasing 1:1 (aliasing is instead expressed through
    // D3D12_BARRIER_LAYOUT_UNDEFINED on the first use of the aliased resource); no call site needs
    // that yet, so this stays on the well-understood legacy path unconditionally rather than guessing
    // at a translation nothing has exercised.
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Aliasing( before, after );
    m_List->ResourceBarrier( 1, &barrier );
}
