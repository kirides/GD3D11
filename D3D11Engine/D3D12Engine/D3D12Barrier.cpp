#include "../pch.h"
#include "D3D12Barrier.h"
#include "D3D12StateCache.h"
#include "../Logger.h"
#include <d3dx12_barriers.h>
#include <mutex>

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
            // Spec rule (confirmed by the debug layer's INCOMPATIBLE_BARRIER_VALUES error): ACCESS_NO_ACCESS
            // may only pair with a non-UNDEFINED layout when Sync is SYNC_NONE. SYNC_ALL here was invalid
            // and closed the command list on literally every frame's back-buffer PRESENT<->RENDER_TARGET
            // transition -- the total render hang this table entry caused.
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
    // PlaneSlice*MipLevels*ArraySize), NOT a mip level -- decoding it as a bare mip level (the previous
    // version of this function) put e.g. PointShadowCubeArray's face/slot index straight into
    // IndexOrFirstMipLevel and blew past its real (1) mip count on every barrier past subresource 0.
    CD3DX12_BARRIER_SUBRESOURCE_RANGE SubresourceRange( ID3D12Resource* resource, UINT subresource ) {
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        const UINT mipLevels = desc.MipLevels;
        const UINT arraySize = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
        if ( subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ) {
            // NumMipLevels == 0 is NOT a wildcard for "all subresources" -- it is the sentinel that makes
            // IndexOrFirstMipLevel mean a single plain subresource index instead (see
            // CD3DX12_BARRIER_SUBRESOURCE_RANGE's single-UINT constructor in d3dx12_barriers.h, which
            // passes {Subresource, 0, 0, 0, 0, 0} specifically to select ONE subresource). Using (0,0,0,0,0,0)
            // here therefore only ever covered subresource 0 -- every array slice/mip past index 0 on any
            // multi-subresource resource (shadow cascade arrays, cube arrays, mip chains) silently never
            // received an enhanced barrier at all. Covering "all" means spelling out the real extents.
            // Plane count is hardcoded to 1: every depth format this backend actually creates (D32_FLOAT,
            // D16_UNORM) is single-plane; a stencil-bearing format would need a real plane count query.
            return CD3DX12_BARRIER_SUBRESOURCE_RANGE( 0, mipLevels, 0, arraySize, 0, 1 );
        }
        const UINT mipSlice = subresource % mipLevels;
        const UINT arraySlice = ( subresource / mipLevels ) % arraySize;
        const UINT planeSlice = subresource / ( mipLevels * arraySize );
        return CD3DX12_BARRIER_SUBRESOURCE_RANGE( mipSlice, 1, arraySlice, 1, planeSlice, 1 );
    }

    // Batch cap for TransitionBarriers()/UAVBarriers(): every call site in this backend batches a
    // handful of resources ahead of one dispatch/draw (post-FX ping-pong targets, cull UAV sets),
    // never more than a few. Fixed-size stack storage keeps this off the per-frame allocation path
    // (see CLAUDE.md's per-frame allocation rule); a batch that somehow exceeds the cap logs a
    // warning and degrades to one Barrier()/ResourceBarrier() call per element instead of silently
    // dropping any transition.
    constexpr UINT kMaxBatchedBarriers = 16;

    // Legacy-to-enhanced handoff (BARRIER_INTEROP_INVALID_LAYOUT):
    //
    // Every render target/UAV texture in this backend is created via D3D12MA::Allocator::CreateResource
    // (-> legacy CreateCommittedResource) with an InitialResourceState equal to its steady-state (e.g.
    // GtaoWorkingDepth is born D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12GTAO.cpp:144) rather than
    // COMMON -- a legacy-barrier-era optimization to skip a throwaway first transition. Per the
    // enhanced-barrier spec, a texture created through the legacy path stays "legacy-tracked" until its
    // first enhanced Barrier() call, and that handoff is only legal when the resource's real
    // (legacy-model) state at that moment is D3D12_RESOURCE_STATE_COMMON. Since none of these textures
    // ever pass through COMMON, their first enhanced transition was rejected outright
    // (STATE_SETTING ERROR #1350) and the runtime silently kept applying none of our barriers to them
    // from then on (confirmed via GPU-based validation: GtaoWorkingDepth read as SRV by a shader while
    // still tracked UNORDERED_ACCESS).
    //
    // Fix: the first time (ever, per resource object) an enhanced Barrier() touches a texture, issue one
    // genuine LEGACY ResourceBarrier transitioning it from its true current state to COMMON first, then
    // treat COMMON as the enhanced barrier's real "before". This requires no changes to any of the ~140
    // existing TransitionBarrier/UAVBarrier call sites -- they keep passing the resource's actual state
    // exactly as before; the bridge is entirely internal to BridgeLegacyResourceToCommon.
    //
    // "First touch, ever" is tracked via SetPrivateData directly on the ID3D12Resource COM object rather
    // than an external pointer-keyed set: the marker's lifetime is then tied exactly to that object's
    // lifetime, so a resource recreated at the same address on resize (GTAO/AO buffers are, per
    // D3D12GraphicsEngine.h:1318's "recreated on resize" comment) correctly needs (and gets) a fresh
    // bridge rather than aliasing a stale "already bridged" entry for a destroyed object. Buffers are
    // untouched by any of this -- D3D12_BUFFER_BARRIER has no Layout, so the interop rule (a LAYOUT
    // requirement) never applies to them.
    const GUID kEnhancedHandoffGuid = { 0xa6c1f1b0, 0x9d3e, 0x4b7a, { 0x8c, 0x2f, 0x6e, 0x1d, 0x9a, 0x3b, 0x5c, 0x7d } };

    // Guards the check-and-mark below. Only ever contended by two threads racing the very first-ever
    // touch of the SAME resource (e.g. a shadow-cascade recorder and the main list); infrequent enough
    // (once per resource, for the resource's entire lifetime) that a global mutex costs nothing measurable.
    std::mutex g_HandoffMutex;

    bool NeedsLegacyHandoff( ID3D12Resource* resource ) {
        std::lock_guard<std::mutex> lock( g_HandoffMutex );
        UINT8 marker = 0;
        UINT size = sizeof( marker );
        if ( SUCCEEDED( resource->GetPrivateData( kEnhancedHandoffGuid, &size, &marker ) ) ) return false;
        marker = 1;
        resource->SetPrivateData( kEnhancedHandoffGuid, sizeof( marker ), &marker );
        return true;
    }

} // namespace

bool D3D12CmdList::BridgeLegacyResourceToCommon( ID3D12Resource* resource, D3D12_RESOURCE_STATES trueBefore ) {
    if ( trueBefore == D3D12_RESOURCE_STATE_COMMON ) return false;   // already common; nothing to bridge
    if ( resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ) return false;   // no Layout on buffers
    if ( !NeedsLegacyHandoff( resource ) ) return false;
    // ALL_SUBRESOURCES regardless of which specific subresource the real transition targets: every
    // subresource of a resource shares one InitialResourceState at creation, so `trueBefore` describes
    // all of them uniformly -- one bridge per resource, not one per subresource, correctly hands the
    // whole thing off in one call.
    CD3DX12_RESOURCE_BARRIER handoff = CD3DX12_RESOURCE_BARRIER::Transition( resource, trueBefore,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES );
    m_List->ResourceBarrier( 1, &handoff );
    return true;
}

void D3D12CmdList::TransitionBarrier( ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after, UINT subresource, D3D12_BARRIER_SYNC syncBeforeHint, D3D12_BARRIER_SYNC syncAfterHint ) {
    if ( s_DeviceSupportsEnhancedBarriers && List7() ) {
        D3D12_BARRIER_SYNC syncBefore, syncAfter;
        D3D12_BARRIER_ACCESS accessBefore, accessAfter;
        D3D12_BARRIER_LAYOUT layoutBefore, layoutAfter;
        if ( MapResourceState( before, syncBefore, accessBefore, layoutBefore )
            && MapResourceState( after, syncAfter, accessAfter, layoutAfter ) ) {
            // A caller-supplied hint narrows the table's conservative default; access/layout are left alone
            // since those describe the operation and resource-state itself, not which stage performs it.
            if ( syncBeforeHint != kBarrierSyncUnspecified ) syncBefore = syncBeforeHint;
            if ( syncAfterHint != kBarrierSyncUnspecified ) syncAfter = syncAfterHint;
            if ( IsBufferResource( resource ) ) {
                CD3DX12_BUFFER_BARRIER bufferBarrier( syncBefore, syncAfter, accessBefore, accessAfter, resource );
                CD3DX12_BARRIER_GROUP group( 1u, static_cast<const D3D12_BUFFER_BARRIER*>( &bufferBarrier ) );
                List7()->Barrier( 1, &group );
            } else {
                if ( BridgeLegacyResourceToCommon( resource, before ) ) {
                    syncBefore = D3D12_BARRIER_SYNC_NONE;
                    accessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
                    layoutBefore = D3D12_BARRIER_LAYOUT_COMMON;
                }
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
    if ( count == 0 ) return;
    if ( count > kMaxBatchedBarriers ) {
#ifdef DEBUG_D3D11
        LogWarn() << "D3D12Barrier: TransitionBarriers batch of " << count
                  << " exceeds kMaxBatchedBarriers (" << kMaxBatchedBarriers << "); issuing one barrier per element.";
#endif
        for ( UINT i = 0; i < count; ++i )
            TransitionBarrier( transitions[i].Resource, transitions[i].Before, transitions[i].After,
                transitions[i].Subresource, transitions[i].SyncBefore, transitions[i].SyncAfter );
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
            if ( t.SyncBefore != kBarrierSyncUnspecified ) syncBefore = t.SyncBefore;
            if ( t.SyncAfter != kBarrierSyncUnspecified ) syncAfter = t.SyncAfter;
            if ( IsBufferResource( t.Resource ) ) {
                bufferBarriers[numBuffer++] = CD3DX12_BUFFER_BARRIER( syncBefore, syncAfter, accessBefore, accessAfter, t.Resource );
            } else {
                if ( BridgeLegacyResourceToCommon( t.Resource, t.Before ) ) {
                    syncBefore = D3D12_BARRIER_SYNC_NONE;
                    accessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
                    layoutBefore = D3D12_BARRIER_LAYOUT_COMMON;
                }
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
            // A UAV barrier only ever makes sense on a resource already in UNORDERED_ACCESS, so that's
            // the only possible "true before" state a bridge needs to reason about here.
            const bool bridged = BridgeLegacyResourceToCommon( resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
            CD3DX12_TEXTURE_BARRIER textureBarrier(
                bridged ? D3D12_BARRIER_SYNC_NONE : kSync, kSync,
                bridged ? D3D12_BARRIER_ACCESS_NO_ACCESS : kAccess, kAccess,
                bridged ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, resource,
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
    if ( count == 0 ) return;
    if ( count > kMaxBatchedBarriers ) {
#ifdef DEBUG_D3D11
        LogWarn() << "D3D12Barrier: UAVBarriers batch of " << count
                  << " exceeds kMaxBatchedBarriers (" << kMaxBatchedBarriers << "); issuing one barrier per element.";
#endif
        for ( UINT i = 0; i < count; ++i ) UAVBarrier( resources[i], syncHint );
        return;
    }

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
                const bool bridged = BridgeLegacyResourceToCommon( resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
                textureBarriers[numTexture++] = CD3DX12_TEXTURE_BARRIER(
                    bridged ? D3D12_BARRIER_SYNC_NONE : kSync, kSync,
                    bridged ? D3D12_BARRIER_ACCESS_NO_ACCESS : kAccess, kAccess,
                    bridged ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                    D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, resource,
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
