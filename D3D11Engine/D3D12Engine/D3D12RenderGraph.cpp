#include "../pch.h"
#include "D3D12RenderGraph.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12EngineCommon.h"   // DXMarker
#include "D3D12StateCache.h"     // D3D12CmdList

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
    m_activeTextures.resize( m_nextHandle );
    m_resourceDescs.resize( m_nextHandle );

    m_externalTextures[index] = externalTarget;
    m_resourceDescs[index] = { 0, 0, 0, name }; // Dummy desc for name tracking

    return D3D12MakeHandle( index, true ); // Sets the first bit to 1
}

RGResourceHandle D3D12RenderGraph::RegisterResource( const RGTextureDesc& desc ) {
    uint32_t index = m_nextHandle++;

    m_externalTextures.resize( m_nextHandle, nullptr );
    m_activeTextures.resize( m_nextHandle );
    m_resourceDescs.resize( m_nextHandle );

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
}

void D3D12RenderGraph::Execute( D3D12CmdList& cmdList ) {
    ZoneScopedN( "D3D12RenderGraph::Execute" );
    for ( size_t i = 0; i < m_passes.size(); ++i ) {
        const auto& pass = m_passes[i];

        AllocateResourcesForPass( i );

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

        ReleaseResourcesForPass( i );
    }
}

void D3D12RenderGraph::AllocateResourcesForPass( size_t passIndex ) {
    for ( uint32_t i = 0; i < m_resourceLifetimes.size(); ++i ) {
        if ( m_resourceLifetimes[i].firstPass == (uint32_t)passIndex ) {
            if ( m_externalTextures[i] != nullptr ) continue;
            if ( !m_resourceLifetimes[i].isRead ) continue;

            const RGTextureDesc& desc = m_resourceDescs[i];
            D3D12TexturePool::Description poolDesc{
                desc.width, desc.height, static_cast<DXGI_FORMAT>( desc.format ),
                ( desc.textureFlags & 1u ) != 0   // bit 0 == NeedsUav
            };

            m_activeTextures[i] = m_texturePool->Acquire( poolDesc );
        }
    }
}

void D3D12RenderGraph::ReleaseResourcesForPass( size_t passIndex ) {
    for ( uint32_t i = 0; i < m_resourceLifetimes.size(); ++i ) {
        if ( m_resourceLifetimes[i].lastPass == (uint32_t)passIndex ) {
            if ( m_externalTextures[i] != nullptr ) continue;

            // Resetting the handle triggers returning it to the D3D12TexturePool automatically.
            m_activeTextures[i].reset();
        }
    }
}
