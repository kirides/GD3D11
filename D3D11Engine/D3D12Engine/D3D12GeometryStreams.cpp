#include "../pch.h"
#include "D3D12GeometryStreams.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12VobArena.h"
#include "D3D12MeshArena.h"
#include "D3D12VertexBuffer.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"

static_assert( D3D12GraphicsEngine::kBackBufferMax == 3, "D3D12GeometryStreams::m_Base mirrors kBackBufferMax" );

// include/SkinnedVertexPull.hlsl hard-codes these stream offsets.
static_assert( static_cast<UINT>( D3D12GeometryStreams::Stream::SkelVB ) == 6 && static_cast<UINT>( D3D12GeometryStreams::Stream::SkinnedPosUv ) == 8
    && static_cast<UINT>( D3D12GeometryStreams::Stream::SkinnedNrmPrev ) == 9, "SkinnedVertexPull.hlsl stream offsets" );
static_assert( static_cast<UINT>( D3D12GeometryStreams::Stream::WorldTess ) == 10, "WorldTess.hlsl kWorldTessStreamOffset" );
static_assert( static_cast<UINT>( D3D12GeometryStreams::Stream::WorldTessParams ) == 11 && static_cast<UINT>( D3D12GeometryStreams::Stream::Count ) == 12, "WorldTess.hlsl kWorldTessParamsStreamOffset" );

namespace {
    constexpr UINT kCount = static_cast<UINT>( D3D12GeometryStreams::Stream::Count );

    // Whole-buffer raw view (Vulkan ignores minStorageBufferOffsetAlignment); a null resource gives a view that reads zeros.
    void RawSrv( D3D12GraphicsEngine& e, UINT slot, Rhi::Resource* r ) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        d.Buffer.NumElements = r ? static_cast<UINT>( r->GetDesc().Width / 4 ) : 1u;
        e.GetRhi()->CreateShaderResourceView( r, &d, e.GetSrvCpuHandle( slot ) );
    }

    Rhi::Resource* VbResource( GfxVertexBuffer* b ) { return b ? D3D12VertexBuffer::From( b )->GetResource() : nullptr; }
}

D3D12GeometryStreams::D3D12GeometryStreams( D3D12GraphicsEngine& engine ) : E( engine ) {}

void D3D12GeometryStreams::Update() {
    if ( m_Failed ) return;
    UINT& stored = m_Base[E.m_FrameIndex];
    if ( stored == 0 ) {
        const UINT allocated = E.AllocateSrvRange( kCount );
        if ( allocated == UINT_MAX ) {
            m_Failed = true;
            Logging::Wrn( "D3D12GeometryStreams: no SRV range for the stream table; vertex pulling unavailable." );
            return;
        }
        stored = allocated + 1;
    }
    const UINT base = stored - 1;

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    const D3D12VobArena* vob = E.m_VobArena.get();
    const D3D12MeshArena* attach = E.m_AttachArena.get();
    const D3D12MeshArena* skel = E.m_SkelArena.get();
    const bool vobOk = vob && vob->Ready(), attachOk = attach && attach->Ready(), skelOk = skel && skel->Ready();

    RawSrv( E, base + static_cast<UINT>( Stream::WorldVB ), wm ? VbResource( wm->GetMeshVertexBuffer() ) : nullptr );
    RawSrv( E, base + static_cast<UINT>( Stream::WorldIB ), wm ? VbResource( wm->GetMeshIndexBuffer() ) : nullptr );
    RawSrv( E, base + static_cast<UINT>( Stream::VobVB ), vobOk ? vob->GetVertexBuffer() : nullptr );
    RawSrv( E, base + static_cast<UINT>( Stream::VobIB ), vobOk ? vob->GetIndexBuffer() : nullptr );
    RawSrv( E, base + static_cast<UINT>( Stream::AttachVB ), attachOk ? attach->GetVertexBuffer() : nullptr );
    RawSrv( E, base + static_cast<UINT>( Stream::AttachIB ), attachOk ? attach->GetIndexBuffer() : nullptr );
    RawSrv( E, base + static_cast<UINT>( Stream::SkelVB ), skelOk ? skel->GetVertexBuffer() : nullptr );
    RawSrv( E, base + static_cast<UINT>( Stream::SkelIB ), skelOk ? skel->GetIndexBuffer() : nullptr );
    RawSrv( E, base + static_cast<UINT>( Stream::SkinnedPosUv ), E.m_SkinnedPosUv.Get() );
    RawSrv( E, base + static_cast<UINT>( Stream::SkinnedNrmPrev ), E.m_SkinnedNrmPrev.Get() );
    RawSrv( E, base + static_cast<UINT>( Stream::WorldTess ), wm ? VbResource( wm->GetMeshTessBuffer() ) : nullptr );
    RawSrv( E, base + static_cast<UINT>( Stream::WorldTessParams ), E.m_WorldTessTable.Get() );
}

UINT D3D12GeometryStreams::Slot( Stream s ) const {
    const UINT stored = m_Base[E.m_FrameIndex];
    return stored ? stored - 1 + static_cast<UINT>( s ) : UINT_MAX;
}
