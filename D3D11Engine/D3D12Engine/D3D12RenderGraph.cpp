#include "../pch.h"
#include "D3D12RenderGraph.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12EngineCommon.h"   // DXMarker
#include "D3D12StateCache.h"     // D3D12CmdList
#include "../Logger.h"
#include <algorithm>

// Implement D3D12RGBuilder methods (must be done after D3D12RenderGraph is fully defined)
RGResourceHandle D3D12RGBuilder::Read( RGResourceHandle handle ) {
    m_pass.m_reads.push_back( handle );
    return handle;
}

RGResourceHandle D3D12RGBuilder::Write( RGResourceHandle handle ) {
    m_pass.m_writes.push_back( handle );
    return handle;
}

RGResourceHandle D3D12RGBuilder::CreateTexture( const RGTextureDesc& desc ) {
    RGResourceHandle handle = m_graph.RegisterResource( desc );
    return Write( handle ); // Creating it implies we are writing to it
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
        for ( RGResourceHandle writeHandle : pass->m_writes ) {
            uint32_t index = D3D12GetHandleIndex( writeHandle );

            if ( m_resourceLifetimes[index].firstPass == UINT32_MAX ) {
                m_resourceLifetimes[index].firstPass = (uint32_t)passIndex;
            }
            m_resourceLifetimes[index].lastPass = (uint32_t)passIndex;
        }

        // Track reads (usage)
        for ( RGResourceHandle readHandle : pass->m_reads ) {
            uint32_t index = D3D12GetHandleIndex( readHandle );

            m_resourceLifetimes[index].lastPass = (uint32_t)passIndex;
            m_resourceLifetimes[index].isRead = true;
        }
    }

    // --- Interval coloring: decide WHICH arena byte range each internal, actually-read resource gets. ---
    // A simple best-fit "free list keyed by when its current occupant's lifetime ends" allocator. Resource
    // counts here are a handful to a few dozen even in a heavy post-fx graph, so the O(n^2) scan below is
    // negligible CPU cost run once per Compile() (i.e. once per graph rebuild, typically once per frame).
    m_resourceOffsets.assign( m_nextHandle, 0 );
    m_resourceHasOffset.assign( m_nextHandle, false );
    if ( !m_arena ) return;

    struct ArenaRange { UINT64 offset; UINT64 size; uint32_t occupantLastPass; };
    static thread_local std::vector<ArenaRange> ranges;   // reused scratch, cleared below — not per-frame growth
    ranges.clear();
    UINT64 bumpOffset = 0;

    // Process in firstPass order so "has this range's occupant already finished by the time I start"
    // is a meaningful question — an index-order scan would let a later-starting resource steal a range
    // out from under one that starts earlier but was registered with a higher handle index.
    std::vector<uint32_t> order;
    order.reserve( m_nextHandle );
    for ( uint32_t i = 0; i < m_nextHandle; ++i ) {
        if ( m_externalTextures[i] != nullptr ) continue;   // external resources are never arena-backed
        if ( !m_resourceLifetimes[i].isRead ) continue;      // never allocated at all (RenderGraph::Execute kills the pass)
        order.push_back( i );
    }
    std::sort( order.begin(), order.end(), [this]( uint32_t a, uint32_t b ) {
        return m_resourceLifetimes[a].firstPass < m_resourceLifetimes[b].firstPass;
        } );

    for ( uint32_t i : order ) {
        const RGTextureDesc& desc = m_resourceDescs[i];
        const bool needsUav = ( desc.textureFlags & 1u ) != 0;
        UINT64 size = 0, alignment = 0;
        m_arena->GetAllocationInfo( desc.width, desc.height, static_cast<DXGI_FORMAT>( desc.format ), needsUav, size, alignment );
        if ( size == 0 ) continue;   // GetAllocationInfo failed (no device) — leave unassigned, Execute skips it

        const uint32_t firstPass = m_resourceLifetimes[i].firstPass;
        const uint32_t lastPass = m_resourceLifetimes[i].lastPass;

        int best = -1;
        for ( size_t r = 0; r < ranges.size(); ++r ) {
            if ( ranges[r].occupantLastPass >= firstPass ) continue;   // still in use when this one needs to start
            if ( ranges[r].size < size ) continue;
            if ( best == -1 || ranges[r].size < ranges[(size_t)best].size ) best = (int)r;   // best fit
        }

        if ( best != -1 ) {
            ranges[(size_t)best].occupantLastPass = lastPass;
            m_resourceOffsets[i] = ranges[(size_t)best].offset;
        } else {
            UINT64 offset = alignment ? ( ( bumpOffset + alignment - 1 ) / alignment ) * alignment : bumpOffset;
            if ( offset + size > D3D12AliasedTextureArena::kArenaCapacityBytes ) {
                LogWarn() << "D3D12RenderGraph: aliasing arena exhausted (" << ( D3D12AliasedTextureArena::kArenaCapacityBytes / (1024*1024) )
                    << " MB) — a transient resource will be skipped this frame.";
                continue;   // m_resourceHasOffset[i] stays false; AllocateResourcesForPass skips it
            }
            ranges.push_back( { offset, size, lastPass } );
            m_resourceOffsets[i] = offset;
            bumpOffset = offset + size;
        }
        m_resourceHasOffset[i] = true;
    }
}

void D3D12RenderGraph::Execute( D3D12CmdList& cmdList ) {
    ZoneScopedN( "D3D12RenderGraph::Execute" );
    for ( size_t i = 0; i < m_passes.size(); ++i ) {
        const auto& pass = m_passes[i];

        AllocateResourcesForPass( i, cmdList );

        // Eliminate any passes whose writes are never read
        bool isPassDead = false;
        if ( !pass->m_writes.empty() ) {
            isPassDead = true;
            for ( RGResourceHandle writeHandle : pass->m_writes ) {
                uint32_t index = D3D12GetHandleIndex( writeHandle );
                if ( D3D12IsExternalHandle( writeHandle ) || m_resourceLifetimes[index].isRead ) {
                    isPassDead = false;
                    break;
                }
            }
        }

        if ( !isPassDead && pass->m_executeCallback ) {
            DXMarker marker( cmdList.Get(), pass->m_name.wide );
            ZoneScoped;
            ZoneName( pass->m_name.narrow, strlen( pass->m_name.narrow ) );
            pass->m_executeCallback( *this, cmdList );
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
