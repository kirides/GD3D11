#include "../pch.h"
#include "D3D12Barrier.h"
#include "D3D12StateCache.h"
#include "../Logger.h"
#include <d3dx12_barriers.h>

// Call sites think only in legacy D3D12_RESOURCE_STATES; enhanced-barrier translation and
// buffer-vs-texture dispatch happen here. Where a legacy state doesn't map cleanly onto one
// enhanced-barrier concept, the mapping below is deliberately conservative (broad sync/access) --
// a barrier that's too broad is merely slower, never wrong.

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
            // Spec rule: ACCESS_NO_ACCESS may only pair with a non-UNDEFINED layout when Sync is
            // SYNC_NONE (debug layer: INCOMPATIBLE_BARRIER_VALUES otherwise).
            sync = D3D12_BARRIER_SYNC_NONE;
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

    // `subresource` is a flat D3D12CalcSubresource-style index (MipSlice + ArraySlice*MipLevels +
    // PlaneSlice*MipLevels*ArraySize), not a mip level.
    CD3DX12_BARRIER_SUBRESOURCE_RANGE SubresourceRange( ID3D12Resource* resource, UINT subresource ) {
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        const UINT mipLevels = desc.MipLevels;
        const UINT arraySize = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
        if ( subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ) {
            // NumMipLevels == 0 is not a wildcard for "all subresources" in CD3DX12_BARRIER_SUBRESOURCE_RANGE
            // -- it's the sentinel that makes IndexOrFirstMipLevel select a single subresource instead. The
            // real extents must be spelled out to cover every slice/mip. Plane count is hardcoded to 1: every
            // depth format this backend creates (D32_FLOAT, D16_UNORM) is single-plane.
            return CD3DX12_BARRIER_SUBRESOURCE_RANGE( 0, mipLevels, 0, arraySize, 0, 1 );
        }
        const UINT mipSlice = subresource % mipLevels;
        const UINT arraySlice = ( subresource / mipLevels ) % arraySize;
        const UINT planeSlice = subresource / ( mipLevels * arraySize );
        return CD3DX12_BARRIER_SUBRESOURCE_RANGE( mipSlice, 1, arraySlice, 1, planeSlice, 1 );
    }

    // Chunk size for TransitionBarriers()/UAVBarriers(); fixed-size stack storage to stay off the
    // per-frame allocation path. Batches larger than this (e.g. point-shadow slot transitions,
    // up to kMaxCubes*6 faces) are split into multiple grouped Barrier() calls rather than
    // degrading to one Barrier() per element -- that would defeat the point of batching.
    constexpr UINT kMaxBatchedBarriers = 64;

} // namespace

void D3D12CmdList::TransitionBarrier( ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after, UINT subresource, D3D12_BARRIER_SYNC syncBeforeHint, D3D12_BARRIER_SYNC syncAfterHint ) {
    if ( s_DeviceSupportsEnhancedBarriers && List7() ) {
        D3D12_BARRIER_SYNC syncBefore, syncAfter;
        D3D12_BARRIER_ACCESS accessBefore, accessAfter;
        D3D12_BARRIER_LAYOUT layoutBefore, layoutAfter;
        if ( MapResourceState( before, syncBefore, accessBefore, layoutBefore )
            && MapResourceState( after, syncAfter, accessAfter, layoutAfter ) ) {
            // A caller hint narrows the table's conservative default sync scope only; access/layout
            // describe the resource state itself, not which stage performs it.
            if ( syncBeforeHint != kBarrierSyncUnspecified ) syncBefore = syncBeforeHint;
            if ( syncAfterHint != kBarrierSyncUnspecified ) syncAfter = syncAfterHint;
            if ( IsBufferResource( resource ) ) {
                CD3DX12_BUFFER_BARRIER bufferBarrier( syncBefore, syncAfter, accessBefore, accessAfter, resource );
                CD3DX12_BARRIER_GROUP group( 1u, static_cast<const D3D12_BUFFER_BARRIER*>( &bufferBarrier ) );
                List7()->Barrier( 1, &group );
            } else {
                CD3DX12_TEXTURE_BARRIER textureBarrier( syncBefore, syncAfter, accessBefore, accessAfter,
                    layoutBefore, layoutAfter, resource, SubresourceRange( resource, subresource ) );
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
    // Batches larger than kMaxBatchedBarriers are issued as multiple grouped Barrier() calls
    // (still far fewer than one call per element) rather than degrading to one Barrier() per
    // transition, which would defeat the point of batching for e.g. point-shadow slot updates.
    for ( UINT offset = 0; offset < count; offset += kMaxBatchedBarriers ) {
        const UINT chunk = std::min( count - offset, kMaxBatchedBarriers );
        TransitionBarriersChunk( transitions + offset, chunk );
    }
}

void D3D12CmdList::TransitionBarriersChunk( const D3D12ResourceTransition* transitions, UINT count ) {
    if ( count == 0 ) return;

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
            if ( t.SyncBefore != kBarrierSyncUnspecified ) syncBefore = t.SyncBefore;
            if ( t.SyncAfter != kBarrierSyncUnspecified ) syncAfter = t.SyncAfter;
            if ( IsBufferResource( t.Resource ) ) {
                bufferBarriers[numBuffer++] = CD3DX12_BUFFER_BARRIER( syncBefore, syncAfter, accessBefore, accessAfter, t.Resource );
            } else {
                textureBarriers[numTexture++] = CD3DX12_TEXTURE_BARRIER( syncBefore, syncAfter, accessBefore, accessAfter,
                    layoutBefore, layoutAfter, t.Resource, SubresourceRange( t.Resource, t.Subresource ) );
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

void D3D12CmdList::UAVBarrier( ID3D12Resource* resource, D3D12_BARRIER_SYNC syncHint ) {
    if ( s_DeviceSupportsEnhancedBarriers && List7() ) {
        const D3D12_BARRIER_SYNC kSync = ( syncHint != kBarrierSyncUnspecified ) ? syncHint : D3D12_BARRIER_SYNC_ALL_SHADING;
        constexpr D3D12_BARRIER_ACCESS kAccess = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        if ( resource && IsBufferResource( resource ) ) {
            CD3DX12_BUFFER_BARRIER bufferBarrier( kSync, kSync, kAccess, kAccess, resource );
            CD3DX12_BARRIER_GROUP group( 1u, static_cast<const D3D12_BUFFER_BARRIER*>( &bufferBarrier ) );
            List7()->Barrier( 1, &group );
        } else if ( resource ) {
            CD3DX12_TEXTURE_BARRIER textureBarrier( kSync, kSync, kAccess, kAccess,
                D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, resource,
                SubresourceRange( resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ) );
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

void D3D12CmdList::UAVBarriers( ID3D12Resource* const* resources, UINT count, D3D12_BARRIER_SYNC syncHint ) {
    // See TransitionBarriers() -- chunk rather than degrade to one Barrier() per element.
    for ( UINT offset = 0; offset < count; offset += kMaxBatchedBarriers ) {
        const UINT chunk = std::min( count - offset, kMaxBatchedBarriers );
        UAVBarriersChunk( resources + offset, chunk, syncHint );
    }
}

void D3D12CmdList::UAVBarriersChunk( ID3D12Resource* const* resources, UINT count, D3D12_BARRIER_SYNC syncHint ) {
    if ( count == 0 ) return;

    if ( s_DeviceSupportsEnhancedBarriers && List7() ) {
        const D3D12_BARRIER_SYNC kSync = ( syncHint != kBarrierSyncUnspecified ) ? syncHint : D3D12_BARRIER_SYNC_ALL_SHADING;
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
                    SubresourceRange( resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ) );
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
