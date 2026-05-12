#include "RenderGraph.h"
#include "BaseGraphicsEngine.h"
#include "Engine.h"
#include "GothicAPI.h"

// Implement RGBuilder methods (must be done after RenderGraph is fully defined)
RGResourceHandle RGBuilder::Read( RGResourceHandle handle ) {
    m_pass.m_reads.push_back( handle );
    return handle;
}

RGResourceHandle RGBuilder::Write( RGResourceHandle handle ) {
    m_pass.m_writes.push_back( handle );
    return handle;
}

RGResourceHandle RGBuilder::CreateTexture( const RGTextureDesc& desc ) {
    RGResourceHandle handle = m_graph.RegisterResource( desc );
    return Write( handle ); // Creating it implies we are writing to it
}

RGResourceHandle RenderGraph::ImportResource( const std::wstring& name, RenderToTextureBuffer* externalBuffer )
{
    uint32_t index = m_nextHandle++;

    // Resize vectors to accommodate the new index
    m_externalTextures.resize( m_nextHandle, nullptr );
    m_activeTextures.resize( m_nextHandle );
    m_resourceDescs.resize( m_nextHandle );

    m_externalTextures[index] = externalBuffer;
    m_resourceDescs[index] = { 0, 0, 0, name }; // Dummy desc for name tracking

    return MakeHandle( index, true ); // Sets the first bit to 1
}

RGResourceHandle RenderGraph::RegisterResource( const RGTextureDesc& desc )
{
    uint32_t index = m_nextHandle++;

    m_externalTextures.resize( m_nextHandle, nullptr );
    m_activeTextures.resize( m_nextHandle );
    m_resourceDescs.resize( m_nextHandle );

    m_resourceDescs[index] = desc;

    return MakeHandle( index, false ); // First bit remains 0
}

void RenderGraph::Compile()
{
    m_resourceLifetimes.assign( m_nextHandle, { UINT32_MAX, 0, false } );

    for ( size_t passIndex = 0; passIndex < m_passes.size(); ++passIndex ) {
        const auto& pass = m_passes[passIndex];

        // Track writes (creation/modification)
        for ( RGResourceHandle writeHandle : pass->m_writes ) {
            uint32_t index = GetHandleIndex( writeHandle );

            // If this is the first time we are writing to this resource, record its birth
            if ( m_resourceLifetimes[index].firstPass == UINT32_MAX ) {
                m_resourceLifetimes[index].firstPass = (uint32_t)passIndex;
            }

            // Every write extends its lifetime to this pass
            m_resourceLifetimes[index].lastPass = (uint32_t)passIndex;
        }

        // Track reads (usage)
        for ( RGResourceHandle readHandle : pass->m_reads ) {
            uint32_t index = GetHandleIndex( readHandle );

            // Reads extend the lifetime of the resource to this pass
            m_resourceLifetimes[index].lastPass = (uint32_t)passIndex;

            // Mark that this resource actually serves a read dependency downstream
            m_resourceLifetimes[index].isRead = true;
        }
    }
}

void RenderGraph::Execute()
{
    ZoneScopedN( "RenderGraph::Execute" );
    for ( size_t i = 0; i < m_passes.size(); ++i ) {
        const auto& pass = m_passes[i];

        AllocateResourcesForPass( i );

        // Eliminate any passes whose writes are never read
        bool isPassDead = false;
        if ( !pass->m_writes.empty() ) {
            isPassDead = true;
            for ( RGResourceHandle writeHandle : pass->m_writes ) {
                uint32_t index = GetHandleIndex( writeHandle );
                // A pass is alive if it writes to an external resource OR an internal resource that gets read
                if ( IsExternalHandle( writeHandle ) || m_resourceLifetimes[index].isRead ) {
                    isPassDead = false;
                    break;
                }
            }
        }

        // Only execute if it provides a meaningful side-effect/write
        if ( !isPassDead && pass->m_executeCallback ) {
            auto _ = Engine::GraphicsEngine->RecordGraphicsEvent( { pass->m_name.wide, pass->m_name.narrow } );
            ZoneScoped;
            ZoneName( pass->m_name.narrow, strlen( pass->m_name.narrow ) );
            pass->m_executeCallback( *this );
        }

        ReleaseResourcesForPass( i );
    }
}

void RenderGraph::AllocateResourcesForPass( size_t passIndex )
{
    for ( uint32_t i = 0; i < m_resourceLifetimes.size(); ++i ) {
        // We only allocate for Graph-Managed resources
        if ( m_resourceLifetimes[i].firstPass == (uint32_t)passIndex ) {
            // If this index is meant to be external, we don't allocate it from the pool.
            if ( m_externalTextures[i] != nullptr ) continue;

            // Do NOT allocate if the resource is completely unread downstream
            if ( !m_resourceLifetimes[i].isRead ) continue;

            const RGTextureDesc& desc = m_resourceDescs[i];
            TexturePool::Description poolDesc{ (int)desc.width, (int)desc.height, static_cast<DXGI_FORMAT>( desc.format ), (DXGI_USAGE)desc.textureFlags };

            m_activeTextures[i] = m_texturePool->Acquire( poolDesc );
        }
    }
}
