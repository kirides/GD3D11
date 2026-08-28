#include "../pch.h"
#include "D3D12RenderGraph.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12EngineCommon.h"   // DXMarker
#include "D3D12StateCache.h"     // D3D12CmdList
#include "../Logger.h"
#include <algorithm>

// Implement D3D12RGBuilder methods (must be done after D3D12RenderGraph is fully defined)
RGResourceHandle D3D12RGBuilder::Read( RGResourceHandle handle, D3D12_RESOURCE_STATES state ) {
    m_pass.m_reads.push_back( { handle, state } );
    return handle;
}

RGResourceHandle D3D12RGBuilder::Write( RGResourceHandle handle, D3D12_RESOURCE_STATES state ) {
    m_pass.m_writes.push_back( { handle, state } );
    return handle;
}

RGResourceHandle D3D12RGBuilder::CreateTexture( const RGTextureDesc& desc, D3D12_RESOURCE_STATES initialState ) {
    RGResourceHandle handle = m_graph.RegisterResource( desc );
    // Creating it implies both writing it (obviously) AND reading it: a resource nobody ever intends to
    // use isn't worth creating, so "created" is treated as sufficient demand on its own — the same way an
    // externally-imported resource's Write() is (see D3D12IsExternalHandle in Execute()'s dead-pass check).
    // Without the implicit Read, a pass that creates AND consumes a resource purely within itself (see
    // D3D12DoF.cpp's composite pass, which UAV-writes its scratch texture then CopyResource's it out in
    // the same callback) has to remember an extra explicit Read() of its own Write() or the resource is
    // silently never allocated at all — isRead gates BOTH Compile()'s interval-coloring AND
    // AllocateResourcesForPass, so GetPhysicalTexture() quietly returns null with no error anywhere.
    // Both declarations carry the SAME initialState: the implicit Read isn't a second, different usage,
    // it's just what makes the resource count as "wanted" — Execute()'s automatic transition only actually
    // runs once for this pass (see TransitionPassResources' dedup-by-current-State check).
    Read( handle, initialState );
    return Write( handle, initialState );
}

void D3D12RGBuilder::MarkExternalEffect() {
    m_pass.m_hasExternalSideEffect = true;
}

void D3D12RGBuilder::TransitionExternal( ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after ) {
    m_pass.m_preTransitions.push_back( { resource, before, after } );
}

void D3D12RGBuilder::TransitionExternalAfter( ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after ) {
    m_pass.m_postTransitions.push_back( { resource, before, after } );
}

RGResourceHandle D3D12RenderGraph::ImportResource( const std::wstring& name, D3D12RenderTarget* externalTarget ) {
    uint32_t index = m_nextHandle++;

    m_externalTextures.resize( m_nextHandle, nullptr );
    m_activeTextures.resize( m_nextHandle, nullptr );
    m_resourceDescs.resize( m_nextHandle );
    m_resourceOffsets.resize( m_nextHandle, 0 );
    m_resourceHasOffset.resize( m_nextHandle, false );

    m_externalTextures[index] = externalTarget;
    m_resourceDescs[index] = { 0, 0, 0, name }; // Dummy desc for name tracking

    return D3D12MakeHandle( index, true ); // Sets the first bit to 1
}

RGResourceHandle D3D12RenderGraph::RegisterResource( const RGTextureDesc& desc ) {
    uint32_t index = m_nextHandle++;

    m_externalTextures.resize( m_nextHandle, nullptr );
    m_activeTextures.resize( m_nextHandle, nullptr );
    m_resourceDescs.resize( m_nextHandle );
    m_resourceOffsets.resize( m_nextHandle, 0 );
    m_resourceHasOffset.resize( m_nextHandle, false );

    m_resourceDescs[index] = desc;

    return D3D12MakeHandle( index, false ); // First bit remains 0
}

void D3D12RenderGraph::Compile() {
    m_resourceLifetimes.assign( m_nextHandle, { UINT32_MAX, 0, false } );

    for ( size_t passIndex = 0; passIndex < m_passes.size(); ++passIndex ) {
        const auto& pass = m_passes[passIndex];

        // Track writes (creation/modification)
        for ( const RGResourceUsage& write : pass->m_writes ) {
            uint32_t index = D3D12GetHandleIndex( write.Handle );

            if ( m_resourceLifetimes[index].firstPass == UINT32_MAX ) {
                m_resourceLifetimes[index].firstPass = (uint32_t)passIndex;
            }
            m_resourceLifetimes[index].lastPass = (uint32_t)passIndex;
        }

        // Track reads (usage)
        for ( const RGResourceUsage& read : pass->m_reads ) {
            uint32_t index = D3D12GetHandleIndex( read.Handle );

            m_resourceLifetimes[index].lastPass = (uint32_t)passIndex;
            m_resourceLifetimes[index].isRead = true;
        }
    }

    // --- Offset assignment: give each internal, actually-read resource its arena byte range. ---
    // Delegated entirely to D3D12AliasedTextureArena::ReserveNamedRange, keyed by RGTextureDesc::name —
    // see that method's comment for why identity (a stable name) rather than a bump-allocation position
    // decides the offset: several real passes are conditionally active frame to frame (god rays only with
    // the sun up, DoF only if enabled, ...), so "whatever the running total happens to be when this
    // resource is first requested" drifts and forces spurious aliasing churn on unrelated passes. This
    // also means two DIFFERENT names never share memory through this graph any more (no more within-
    // Compile() best-fit reuse) — acceptable at the current scale; see ReserveNamedRange's comment.
    m_resourceOffsets.assign( m_nextHandle, 0 );
    m_resourceHasOffset.assign( m_nextHandle, false );
    if ( !m_arena ) return;

    for ( uint32_t i = 0; i < m_nextHandle; ++i ) {
        if ( m_externalTextures[i] != nullptr ) continue;   // external resources are never arena-backed
        if ( !m_resourceLifetimes[i].isRead ) continue;      // never allocated at all (Execute kills the pass)

        const RGTextureDesc& desc = m_resourceDescs[i];
        const bool needsUav = ( desc.textureFlags & 1u ) != 0;
        UINT64 size = 0, alignment = 0;
        m_arena->GetAllocationInfo( desc.width, desc.height, static_cast<DXGI_FORMAT>( desc.format ), needsUav, size, alignment );
        if ( size == 0 ) continue;   // GetAllocationInfo failed (no device) — leave unassigned, Execute skips it

        const UINT64 offset = m_arena->ReserveNamedRange( desc.name, size, alignment );
        if ( offset == UINT64_MAX ) {
            LogWarn() << "D3D12RenderGraph: aliasing arena exhausted (" << ( D3D12AliasedTextureArena::kArenaCapacityBytes / (1024*1024) )
                << " MB) — a transient resource will be skipped this frame.";
            continue;   // m_resourceHasOffset[i] stays false; AllocateResourcesForPass skips it
        }
        m_resourceOffsets[i] = offset;
        m_resourceHasOffset[i] = true;
    }
}

void D3D12RenderGraph::Execute( D3D12CmdList& cmdList ) {
    ZoneScopedN( "D3D12RenderGraph::Execute" );
    for ( size_t i = 0; i < m_passes.size(); ++i ) {
        const auto& pass = m_passes[i];

        AllocateResourcesForPass( i, cmdList );

        // Eliminate any passes whose writes are never read — unless the pass told us (MarkExternalEffect)
        // that it has a real effect the graph doesn't track as a Write in the first place.
        bool isPassDead = false;
        if ( !pass->m_hasExternalSideEffect && !pass->m_writes.empty() ) {
            isPassDead = true;
            for ( const RGResourceUsage& write : pass->m_writes ) {
                uint32_t index = D3D12GetHandleIndex( write.Handle );
                if ( D3D12IsExternalHandle( write.Handle ) || m_resourceLifetimes[index].isRead ) {
                    isPassDead = false;
                    break;
                }
            }
        }

        if ( !isPassDead && pass->m_executeCallback ) {
            DXMarker marker( cmdList.Get(), pass->m_name );
            ZoneScoped;
            ZoneName( pass->m_name.narrow, pass->m_name.len_narrow );
            TransitionPassResources( *pass, cmdList );
            pass->m_executeCallback( *this, cmdList );
            TransitionPostPassResources( *pass, cmdList );
        }
    }
}

void D3D12RenderGraph::AllocateResourcesForPass( size_t passIndex, D3D12CmdList& cmdList ) {
    for ( uint32_t i = 0; i < m_resourceLifetimes.size(); ++i ) {
        if ( m_resourceLifetimes[i].firstPass != (uint32_t)passIndex ) continue;
        if ( m_externalTextures[i] != nullptr ) continue;
        if ( !m_resourceLifetimes[i].isRead ) continue;
        if ( !m_resourceHasOffset[i] ) continue;   // Compile() couldn't fit it — leave m_activeTextures[i] null

        const RGTextureDesc& desc = m_resourceDescs[i];
        const bool needsUav = ( desc.textureFlags & 1u ) != 0;
        m_activeTextures[i] = m_arena->Acquire( m_resourceOffsets[i], desc.width, desc.height,
            static_cast<DXGI_FORMAT>( desc.format ), needsUav, cmdList );
    }
}

namespace {
    // Shared per-pass batch cap for D3D12RenderGraph's automatic pre/post transition calls: every pass
    // converted so far touches at most a couple of resources. A pass with more just takes an extra
    // TransitionBarriers() call rather than losing a transition — see PushTransition's overflow branch.
    constexpr UINT kMaxTransitionsPerBatch = 16;

    // Appends `t` to the batch[count]/count scratch pair (sized kMaxTransitionsPerBatch, local to a single
    // TransitionPassResources/TransitionPostPassResources call), or — once that array is full — issues an
    // individual TransitionBarrier() for it right away rather than dropping it. Shared by both call sites so
    // the overflow behavior can't drift between them.
    void PushTransition( D3D12CmdList& cmdList, D3D12ResourceTransition* batch, UINT& count, const D3D12ResourceTransition& t ) {
        if ( count < kMaxTransitionsPerBatch ) {
            batch[count++] = t;
        } else {
            cmdList.TransitionBarrier( t.Resource, t.Before, t.After, t.Subresource, t.SyncBefore, t.SyncAfter );
        }
    }
}

void D3D12RenderGraph::TransitionPassResources( const D3D12RenderPass& pass, D3D12CmdList& cmdList ) {
    D3D12ResourceTransition transitions[kMaxTransitionsPerBatch];
    UINT n = 0;

    auto consider = [&]( const RGResourceUsage& usage ) {
        if ( D3D12IsExternalHandle( usage.Handle ) ) return;   // not graph-tracked; caller manages state manually
        D3D12RenderTarget* tex = GetPhysicalTexture( usage.Handle );
        if ( !tex || tex->State == usage.State ) return;       // absent (skipped resource) or already there
        PushTransition( cmdList, transitions, n, { tex->GetResource(), tex->State, usage.State } );
        tex->State = usage.State;
        };

    for ( const RGResourceUsage& write : pass.m_writes ) consider( write );
    for ( const RGResourceUsage& read : pass.m_reads ) consider( read );

    // Non-graph-tracked transitions folded into this same batch (D3D12RGBuilder::TransitionExternal) — skip
    // true no-ops exactly like the tex->State check above; `Before` stays the caller's responsibility.
    for ( const D3D12ResourceTransition& ext : pass.m_preTransitions ) {
        if ( ext.Before == ext.After ) continue;
        PushTransition( cmdList, transitions, n, ext );
    }

    if ( n > 0 ) cmdList.TransitionBarriers( transitions, n );
}

void D3D12RenderGraph::TransitionPostPassResources( const D3D12RenderPass& pass, D3D12CmdList& cmdList ) {
    if ( pass.m_postTransitions.empty() ) return;
    D3D12ResourceTransition transitions[kMaxTransitionsPerBatch];
    UINT n = 0;
    for ( const D3D12ResourceTransition& ext : pass.m_postTransitions ) {
        if ( ext.Before == ext.After ) continue;
        PushTransition( cmdList, transitions, n, ext );
    }
    if ( n > 0 ) cmdList.TransitionBarriers( transitions, n );
}
