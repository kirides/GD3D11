#include "../pch.h"
// Compute skinning: every skinned sub-mesh PrepareFrameSkeletals met this frame is posed once, by
// Shaders/D3D12/SkinVertices.hlsl, into two world-space vertex streams. The depth prepass, the lit pass, the CSM
// cascades and the point-light cubes then draw those like static geometry with the skeletal arena's indices
// (BaseVertexLocation = the sub-mesh's posed base), instead of each re-skinning every vertex in its VS.
#include "D3D12GraphicsEngine.h"
#include "D3D12MeshArena.h"
#include "D3D12EngineCommon.h"
#include "../WorldObjects.h"
#include "../Logger.h"

std::array<uint32_t, kMaxSkinnedSubmeshes> g_SkinDst;

namespace {
    // A reservation; the arena range is resolved at dispatch, after the frame's last arena flush, which is also
    // what every draw checks — so a sub-mesh is drawn exactly when its job ran.
    struct PendingSkin {
        const SkeletalMeshInfo* Mesh;
        uint32_t DstBase;
        uint32_t InstanceRow;
    };
    std::vector<PendingSkin> g_PendingSkins;   // main thread only
    uint32_t g_SkinDstCount = 0;

    // Mirror SkinVertices.hlsl.
    struct SkinJobGPU {
        uint32_t SrcBase, VertexCount, DstBase, InstanceRow;
    };
    static_assert( sizeof( SkinJobGPU ) == 16, "SkinJobGPU must match SkinJob in SkinVertices.hlsl" );
    struct SkinGroupGPU {
        uint32_t Job, FirstVertex;
    };

    constexpr UINT kSkinThreadGroup = 64;   // numthreads in SkinVertices.hlsl
    constexpr UINT kMinSkinnedVertices = 128 * 1024;
    // Hard cap on the posed streams (72 MB of VRAM); with it the job ring can never overflow.
    constexpr UINT kMaxSkinnedVertices = 2 * 1024 * 1024;
    constexpr UINT kMaxSkinGroups = kMaxSkinnedSubmeshes + kMaxSkinnedVertices / kSkinThreadGroup;
    constexpr UINT kSkinJobBytes = kMaxSkinnedSubmeshes * sizeof( SkinJobGPU );
    constexpr UINT kSkinRingBytes = kSkinJobBytes + kMaxSkinGroups * sizeof( SkinGroupGPU );

    D3D12_RESOURCE_DESC BufferDesc( UINT64 bytes, D3D12_RESOURCE_FLAGS flags ) {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = flags;
        return bd;
    }
}


bool D3D12GraphicsEngine::CreateSkinningResources() {
    const D3D12_RESOURCE_DESC desc = BufferDesc( kSkinRingBytes, D3D12_RESOURCE_FLAG_NONE );
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_Rhi->CreateResource( DefaultUploadHeapType, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            m_SkinJobRing[i].ReleaseAndGetAddressOf() ) ) )
            return false;
        m_SkinJobRing[i]->SetName( L"SkinJobRing" );
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( m_SkinJobRing[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_SkinJobRingPtr[i] ) ) ) )
            return false;
    }
    return true;
}


void D3D12GraphicsEngine::BeginSkinningFrame() {
    g_PendingSkins.clear();
    g_SkinDstCount = 0;

    // Grow to what last frame asked for; a frame that outgrew the streams drops the excess once.
    if ( !m_SkinnedPosUv || ( m_SkinnedDemand > m_SkinnedCapacity && m_SkinnedCapacity < kMaxSkinnedVertices ) ) {
        const UINT want = std::min( kMaxSkinnedVertices,
            std::max( kMinSkinnedVertices, m_SkinnedDemand + m_SkinnedDemand / 4 ) );
        Microsoft::WRL::ComPtr<Rhi::Resource> posUv, nrmPrev;
        const D3D12_RESOURCE_DESC posDesc = BufferDesc( static_cast<UINT64>( want ) * kSkinnedPosUvStride,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
        const D3D12_RESOURCE_DESC nrmDesc = BufferDesc( static_cast<UINT64>( want ) * kSkinnedNrmPrevStride,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
        if ( SUCCEEDED( m_Rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &posDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, posUv.ReleaseAndGetAddressOf() ) )
            && SUCCEEDED( m_Rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &nrmDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, nrmPrev.ReleaseAndGetAddressOf() ) ) ) {
            posUv->SetName( L"SkinnedPosUv" );
            nrmPrev->SetName( L"SkinnedNrmPrev" );
            if ( m_SkinnedPosUv ) {
                // Frames in flight may still draw from the old pair.
                QueueCleanupJob( [oldPos = std::move( m_SkinnedPosUv ), oldNrm = std::move( m_SkinnedNrmPrev )]() mutable {} );
            }
            m_SkinnedPosUv = std::move( posUv );
            m_SkinnedNrmPrev = std::move( nrmPrev );
            m_SkinnedCapacity = want;
            Logging::Inf( "D3D12: posed skinned-vertex streams now {} vertices ({} MB)", want,
                static_cast<UINT64>( want ) * ( kSkinnedPosUvStride + kSkinnedNrmPrevStride ) / ( 1024 * 1024 ) );
        } else if ( !m_SkinnedOverflowLogged ) {
            Logging::Wrn( "D3D12: allocating {} posed skinned vertices failed; some NPCs will not render.", want );
            m_SkinnedOverflowLogged = true;
        }
    }
    m_SkinnedUsed = 0;
    m_SkinnedDemand = 0;
}


uint32_t D3D12GraphicsEngine::ReserveSkinned( const SkeletalMeshVisualInfo* visual, uint32_t instanceRow ) {
    size_t entries = 0;
    for ( auto const& [mat, meshList] : visual->SkeletalMeshes ) entries += meshList.size();
    if ( g_SkinDstCount + entries > kMaxSkinnedSubmeshes ) {
        static bool logged = false;
        if ( !std::exchange( logged, true ) )
            Logging::Wrn( "D3D12: more than {} skinned sub-meshes in one frame; the rest are not drawn.", kMaxSkinnedSubmeshes );
        return kNoSkinnedOutput;
    }

    const uint32_t first = g_SkinDstCount;
    for ( auto const& [mat, meshList] : visual->SkeletalMeshes ) {
        for ( auto const& mesh : meshList ) {
            uint32_t dst = kNoSkinnedOutput;
            if ( mesh && !mesh->Indices.empty() ) {
                const UINT count = static_cast<UINT>( mesh->Vertices.size() );
                m_SkinnedDemand += count;
                if ( m_SkinnedPosUv && m_SkinnedUsed + count <= m_SkinnedCapacity ) {
                    dst = m_SkinnedUsed;
                    m_SkinnedUsed += count;
                    g_PendingSkins.push_back( { mesh.get(), dst, instanceRow } );
                } else if ( !m_SkinnedOverflowLogged ) {
                    Logging::Wrn( "D3D12: posed skinned-vertex streams full ({} vertices); growing them next frame.", m_SkinnedCapacity );
                    m_SkinnedOverflowLogged = true;
                }
            }
            g_SkinDst[g_SkinDstCount++] = dst;
        }
    }
    return first;
}


void D3D12GraphicsEngine::DispatchSkinning() {
    const UINT frame = m_FrameIndex;
    if ( g_PendingSkins.empty() || !m_FrameOpen || !m_Pipelines.Skinning.PSO || !m_SkinnedPosUv
        || !m_SkelArena->Ready() || !m_SkinJobRingPtr[frame] || !m_SkeletalCBBuffer[frame] )
        return;

    // Written straight into the persistently mapped ring: sequential writes, never read back.
    SkinJobGPU* jobs = reinterpret_cast<SkinJobGPU*>( m_SkinJobRingPtr[frame] );
    SkinGroupGPU* groups = reinterpret_cast<SkinGroupGPU*>( m_SkinJobRingPtr[frame] + kSkinJobBytes );
    UINT jobCount = 0, groupCount = 0, vertexCount = 0;
    for ( const PendingSkin& p : g_PendingSkins ) {
        const D3D12MeshArena::Range* range = m_SkelArena->Find( p.Mesh->ArenaSlot );
        if ( !range ) continue;   // not uploaded yet — no pass draws it either
        const UINT count = static_cast<UINT>( p.Mesh->Vertices.size() );
        jobs[jobCount] = { range->BaseVertex, count, p.DstBase, p.InstanceRow };
        for ( UINT v = 0; v < count; v += kSkinThreadGroup )
            groups[groupCount++] = { jobCount, v };
        ++jobCount;
        vertexCount += count;
    }
    TracyPlot( "Skinned vertices", static_cast<int64_t>( vertexCount ) );
    if ( jobCount == 0 ) return;

    DX_ZONE( m_CmdList.Get(), "Compute skinning" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "Compute skinning" );

    m_CmdList->TransitionBarriers( {
        { m_SkinnedPosUv.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
        { m_SkinnedNrmPrev.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
    } );

    const UINT groupsPerRow = std::min<UINT>( groupCount, 65535u );
    const UINT rows = ( groupCount + groupsPerRow - 1 ) / groupsPerRow;
    const UINT constants[4] = { groupCount, groupsPerRow, MotionGBufferActive() ? 1u : 0u, 0u };
    const D3D12_GPU_VIRTUAL_ADDRESS ring = m_SkinJobRing[frame]->GetGPUVirtualAddress();

    m_CmdList->SetPipelineState( m_Pipelines.Skinning.PSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.Skinning.RootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 4, constants, 0 );
    m_CmdList->SetComputeRootShaderResourceView( 1, ring );
    m_CmdList->SetComputeRootShaderResourceView( 2, ring + kSkinJobBytes );
    m_CmdList->SetComputeRootShaderResourceView( 3, m_SkelArena->GetVertexBuffer()->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootShaderResourceView( 4, m_SkeletalCBBuffer[frame]->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootUnorderedAccessView( 5, m_SkinnedPosUv->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootUnorderedAccessView( 6, m_SkinnedNrmPrev->GetGPUVirtualAddress() );
    m_CmdList->Dispatch( groupsPerRow, rows, 1 );

    m_CmdList->TransitionBarriers( {
        { m_SkinnedPosUv.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER },
        { m_SkinnedNrmPrev.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER },
    } );
}


D3D12_VERTEX_BUFFER_VIEW D3D12GraphicsEngine::SkinnedPosUvView() const {
    return { m_SkinnedPosUv->GetGPUVirtualAddress(), m_SkinnedCapacity * kSkinnedPosUvStride, kSkinnedPosUvStride };
}


bool D3D12GraphicsEngine::BindSkinnedGeometry( D3D12CmdList& cmdList, UINT skelDataParam ) {
    if ( !m_SkelArena->Ready() || !m_SkinnedPosUv || !m_SkeletalCBBuffer[m_FrameIndex] ) return false;
    const D3D12_VERTEX_BUFFER_VIEW views[2] = {
        SkinnedPosUvView(),
        { m_SkinnedNrmPrev->GetGPUVirtualAddress(), m_SkinnedCapacity * kSkinnedNrmPrevStride, kSkinnedNrmPrevStride },
    };
    const D3D12_INDEX_BUFFER_VIEW ibv = m_SkelArena->IndexBufferView();
    cmdList->IASetVertexBuffers( 0, 2, views );
    cmdList->IASetIndexBuffer( &ibv );
    cmdList->SetGraphicsRootShaderResourceView( skelDataParam, m_SkeletalCBBuffer[m_FrameIndex]->GetGPUVirtualAddress() );
    return true;
}
