// D3D12GpuWorld — the world mesh culled per view on the GPU. See GPU_SCENE_PLAN.md and WorldCull.hlsl.
#include "../pch.h"
#include "D3D12GpuWorld.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12GpuScene.h"
#include "D3D12GpuTimers.h"
#include "D3D12VertexBuffer.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"
#include "../WorldMeshSection.h"
#include "../WorldConverter.h"
#include "../zCMaterial.h"
#include "../MaterialFx.h"
#include "../zCTexture.h"
#include "../zTypes.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static_assert( D3D12GraphicsEngine::kBackBufferMax == 3, "D3D12GpuWorld::kFrames mirrors kBackBufferMax" );

namespace {
    // Mirror WorldCull.hlsl
    struct SectionGpu { XMFLOAT3 Min; int32_t GridX; XMFLOAT3 Max; int32_t GridY; };
    struct MeshGpu { uint32_t ClusterFirst, ClusterCount, MaterialIndex, FlagsSection; XMFLOAT3 Min, Max; };
    struct ClusterGpu { XMFLOAT3 Min; uint32_t IndexStart; XMFLOAT3 Max; uint32_t IndexCount; };
    struct WorldCullCB {
        XMFLOAT4X4 CullViewProj;
        XMFLOAT3 CamPos;
        float    SectionRadiusSq;
        uint32_t MeshCount, ViewFlags, OpaqueCapacity, AlphaCapacity, WetNormalSlot;
        float    WetNormalStrength;
        uint32_t DefaultOrm, MaterialCount;
        int32_t  CamSectionX, CamSectionY, SectionGridRadius;
    };
    struct ConeGpu { XMFLOAT3 Apex; float Cutoff; XMFLOAT3 Axis; float Pad; };
    struct WorldCullExtCB {
        XMFLOAT4X4 HiZViewProj;
        uint32_t HiZIndex, HiZWidth, HiZHeight, HiZMipCount;
        XMFLOAT3 ConeEye;
        uint32_t ExtFlags;
    };
    static_assert( sizeof( SectionGpu ) == 32 && sizeof( MeshGpu ) == 40 && sizeof( ClusterGpu ) == 32, "WorldCull.hlsl structs" );
    static_assert( sizeof( ConeGpu ) == 32 && sizeof( WorldCullExtCB ) == 96, "WorldCull.hlsl ClusterCone / WorldCullExtCB" );
    static_assert( sizeof( WorldCullCB ) == 31 * sizeof( uint32_t ), "CreateWorldCull's 31 root constants" );

    constexpr uint32_t kMeshMain = 1u, kMeshCaster = 2u, kMeshPoint = 4u;
    constexpr uint32_t kMaterialAlpha = 1u, kMaterialReady = 2u;
    constexpr uint32_t kViewMainFlag = 1u, kViewNoFrustum = 2u, kViewFeedback = 4u, kViewGridRadius = 8u;
    constexpr uint32_t kViewSphere = 16u, kViewCube = 32u, kViewEarly = 64u, kViewLate = 128u, kViewCompact = 256u;
    constexpr uint32_t kExtCones = 1u;
    constexpr UINT kArgCountBytes = 16;   // opaque count, alpha count, drawn indices, pad
    constexpr uint32_t kTouchInterval = 30;       // frames between CacheIn touches of a seen material
    constexpr uint32_t kResolvesPerFrame = 64;
    constexpr uint32_t kRefreshPerFrame = 32;     // unresolved materials re-polled without CacheIn
    constexpr uint32_t kOrmIndexMask = 0x0FFFFFFFu;
    constexpr UINT kCommandStride = 36;   // WorldDrawCommand, WorldCull.hlsl WORLD_DRAW_COMMAND_STRIDE
    constexpr UINT kCubeCommandStride = 32;   // PointShadowCasterCommand, WorldCull.hlsl CUBE_COMMAND_STRIDE
    static_assert( kCubeCommandStride == D3D12GpuScene::kPointCommandStride, "both build PointShadowCasterCommands" );

    D3D12_RESOURCE_DESC BufferDesc( UINT64 bytes, bool uav ) {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = std::max<UINT64>( bytes, 16 );
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        return bd;
    }

    /** D3D12ShadowMap's world caster filter: no typed materials, nothing translucent. */
    bool IsCaster( const MeshKey& key ) {
        if ( key.Info && key.Info->MaterialType != MaterialInfo::MT_None ) return false;
        const int func = key.Material->GetAlphaFunc();
        if ( func > zMAT_ALPHA_FUNC_NONE && func != zMAT_ALPHA_FUNC_TEST ) return false;
        return !( func == 0 && zColor( key.Material->GetColor() ).bgra.alpha < 255 );
    }

    bool IsValidBox( const zTBBox3D& b ) { return b.Min.x <= b.Max.x && b.Min.y <= b.Max.y && b.Min.z <= b.Max.z; }

    /** meshopt_computeClusterBounds' normal cone without its 256-vertex limit; the axis is the mean normal. With
        clockwise front faces in a left-handed space, cross(p1 - p0, p2 - p0) points out of the front face. */
    ConeGpu ComputeCone( const WorldMeshInfo& mesh, uint32_t first, uint32_t count, const zTBBox3D& box ) {
        const ConeGpu none = { {}, 2.0f, { 0.0f, 1.0f, 0.0f }, 0.0f };
        const std::vector<WorldVertexCPU>* cpu = mesh.GetCpuVertices();
        const size_t vertexCount = cpu ? cpu->size() : mesh.Vertices.size();
        auto position = [&]( VERTEX_INDEX i ) {
            const float3& p = cpu ? ( *cpu )[i].Position : mesh.Vertices[i].Position;
            return XMVectorSet( p.x, p.y, p.z, 0.0f );
            };
        static thread_local std::vector<XMFLOAT4> planes;
        planes.clear();
        XMVECTOR sum = XMVectorZero();
        const size_t end = std::min<size_t>( static_cast<size_t>( first ) + count, mesh.Indices.size() );
        for ( size_t t = first; t + 2 < end; t += 3 ) {
            const VERTEX_INDEX i0 = mesh.Indices[t], i1 = mesh.Indices[t + 1], i2 = mesh.Indices[t + 2];
            if ( i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount ) return none;
            const XMVECTOR p0 = position( i0 );
            const XMVECTOR n = XMVector3Cross( XMVectorSubtract( position( i1 ), p0 ), XMVectorSubtract( position( i2 ), p0 ) );
            const float area = XMVectorGetX( XMVector3Length( n ) );
            if ( area <= 1e-12f ) continue;   // degenerate triangles are invisible anyway
            const XMVECTOR nn = XMVectorScale( n, 1.0f / area );
            XMFLOAT4 plane;
            XMStoreFloat4( &plane, XMVectorSetW( nn, -XMVectorGetX( XMVector3Dot( nn, p0 ) ) ) );
            planes.push_back( plane );
            sum = XMVectorAdd( sum, nn );
        }
        const float sumLength = XMVectorGetX( XMVector3Length( sum ) );
        if ( planes.empty() || sumLength <= 1e-6f ) return none;
        const XMVECTOR axis = XMVectorScale( sum, 1.0f / sumLength );
        float minDot = 1.0f;
        for ( const XMFLOAT4& pl : planes ) minDot = std::min( minDot, XMVectorGetX( XMVector3Dot( XMLoadFloat4( &pl ), axis ) ) );
        if ( minDot <= 0.1f ) return none;   // wider than ~168 degrees: useless, and the apex gets unstable

        // The apex lies behind every triangle's plane, on the axis through the box centre.
        const XMVECTOR center = XMVectorScale( XMVectorAdd( XMLoadFloat3( &box.Min ), XMLoadFloat3( &box.Max ) ), 0.5f );
        float maxT = 0.0f;
        for ( const XMFLOAT4& pl : planes ) {
            const XMVECTOR n = XMLoadFloat4( &pl );
            const float dc = XMVectorGetX( XMVector3Dot( center, n ) ) + pl.w;
            maxT = std::max( maxT, dc / XMVectorGetX( XMVector3Dot( axis, n ) ) );
        }
        ConeGpu cone;
        XMStoreFloat3( &cone.Apex, XMVectorSubtract( center, XMVectorScale( axis, maxT ) ) );
        XMStoreFloat3( &cone.Axis, axis );
        cone.Cutoff = sqrtf( 1.0f - minDot * minDot );
        cone.Pad = 0.0f;
        return cone;
    }
}

D3D12GpuWorld::D3D12GpuWorld( D3D12GraphicsEngine& engine ) : m_E( engine ) {}

D3D12GpuWorld::~D3D12GpuWorld() = default;


bool D3D12GpuWorld::IsSpecial( const MeshKey& key ) {
    if ( key.Info && ( key.Info->IsWater() || key.Info->MaterialType == MaterialInfo::MT_Portal
        || key.Info->MaterialType == MaterialInfo::MT_WaterfallFoam ) ) return true;
    return D3D12GraphicsEngine::IsWorldMeshAlphaBlended( key.Material );
}


void D3D12GpuWorld::Reset() {
    m_Built = false;
    m_BuildFailed = false;
    m_Specials.clear();
    m_Materials.clear();
    m_DirtyMaterials.clear();
    m_Queue.clear();
    m_RefreshCursor = 0;
    m_MeshCount = m_ClusterCount = m_SectionCount = 0;
    m_Capacity = 0;
    for ( ComPtr<Rhi::Resource>* r : { std::addressof( m_Sections ), std::addressof( m_Meshes ), std::addressof( m_Clusters ),
              std::addressof( m_MaterialBuffer ), std::addressof( m_Seen ), std::addressof( m_Cones ), std::addressof( m_History ) } )
        if ( *r ) m_E.QueueResourceForRelease( std::move( *r ) );
    for ( UINT v = 0; v < kViews; ++v ) {
        if ( m_Compact[v] ) m_E.QueueResourceForRelease( std::move( m_Compact[v] ) );
        m_Compacted[v] = m_CompactReadable[v] = false;
        if ( m_Args[v] ) m_E.QueueResourceForRelease( std::move( m_Args[v] ) );
        if ( m_ArgCount[v] ) m_E.QueueResourceForRelease( std::move( m_ArgCount[v] ) );
        m_ArgsDrawable[v] = false;
    }
    for ( UINT i = 0; i < kFrames; ++i ) {
        if ( m_Readback[i] ) m_E.QueueResourceForRelease( std::move( m_Readback[i] ) );
        m_ReadbackPtr[i] = nullptr;
        m_ReadbackPending[i] = false;
    }
    m_SeenReadable = false;
}


void D3D12GpuWorld::OnSrvSlotFreed( UINT slot ) {
    std::lock_guard<std::mutex> lock( m_FreedMutex );
    if ( m_Built ) m_FreedSlots.push_back( slot );
}


void D3D12GpuWorld::Resolve( uint32_t i, bool cacheIn ) {
    Material& m = m_Materials[i];
    const D3D12GraphicsEngine::WorldMaterial r = m_E.ResolveWorldMaterial( *m.Key, cacheIn );
    MaterialGpu g = { r.Normal, r.Orm, r.Diffuse, r.NormalStrength,
        ( r.AlphaTested ? kMaterialAlpha : 0u ) | ( r.TextureReady ? kMaterialReady : 0u ), {} };
    m.Ready = r.TextureReady;
    if ( cacheIn ) m.LastTouch = m_Frame;
    if ( memcmp( &g, &m.Gpu, sizeof( g ) ) != 0 ) {
        m.Gpu = g;
        m_DirtyMaterials.push_back( i );
    }
}


bool D3D12GpuWorld::Build( Rhi::CmdList& cmd ) {
    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    if ( !wm || !wm->GetMeshVertexBuffer() || !wm->GetMeshIndexBuffer() ) return false;
    auto& worldSections = Engine::GAPI->GetWorldSections();
    if ( worldSections.empty() ) return false;
    ZoneScopedN( "GpuWorld build" )
    static_assert( sizeof( D3D12GraphicsEngine::WorldDrawCommand ) == kCommandStride, "WorldCull.hlsl's command stride" );

    std::vector<SectionGpu> sections;
    std::vector<MeshGpu> meshes;
    std::vector<ClusterGpu> clusters;
    std::vector<ConeGpu> cones;
    std::map<std::pair<zCMaterial*, MaterialInfo*>, uint32_t> materialIndex;
    std::vector<MeshCluster> sorted;
    for ( auto& [x, row] : worldSections ) {
        for ( auto& [y, section] : row ) {
            const uint32_t sectionIndex = static_cast<uint32_t>( sections.size() );
            sections.push_back( { section.BoundingBox.Min, x, section.BoundingBox.Max, y } );
            for ( auto const& [key, mesh] : section.WorldMeshes ) {
                if ( !mesh || mesh->Indices.empty() || !key.Material ) continue;
                const bool special = IsSpecial( key );
                if ( special ) m_Specials.push_back( { &section, mesh, &key, x, y } );
                // Point-light bakes take everything but water, as their CPU gather did.
                const uint32_t flags = ( special ? 0u : kMeshMain ) | ( IsCaster( key ) ? kMeshCaster : 0u )
                    | ( key.Info && key.Info->IsWater() ? 0u : kMeshPoint );
                const zTBBox3D& box = mesh->HasBoundingBox ? mesh->BoundingBox : section.BoundingBox;
                if ( !flags || !IsValidBox( box ) ) continue;

                auto [it, inserted] = materialIndex.try_emplace( std::pair<zCMaterial*, MaterialInfo*>( key.Material, key.Info ),
                    static_cast<uint32_t>( m_Materials.size() ) );
                if ( inserted ) {
                    Material m;
                    m.Key = &key;
                    m.Animated = key.Material->HasAnimatedTexture();
                    m_Materials.push_back( m );
                }

                MeshGpu mg = {};
                mg.ClusterFirst = static_cast<uint32_t>( clusters.size() );
                mg.MaterialIndex = it->second;
                mg.FlagsSection = flags | ( sectionIndex << 8 );
                mg.Min = box.Min;
                mg.Max = box.Max;
                // Runs merge across neighbouring clusters, so they must tile the mesh's indices in order.
                sorted.assign( mesh->Clusters.begin(), mesh->Clusters.end() );
                std::ranges::sort( sorted, {}, &MeshCluster::IndexOffset );
                uint32_t next = 0;
                for ( const MeshCluster& c : sorted ) next = ( c.IndexOffset == next ) ? next + c.IndexCount : UINT32_MAX;
                if ( sorted.empty() || next != mesh->Indices.size() ) {
                    clusters.push_back( { box.Min, mesh->BaseIndexLocation, box.Max, static_cast<uint32_t>( mesh->Indices.size() ) } );
                    cones.push_back( ComputeCone( *mesh, 0, static_cast<uint32_t>( mesh->Indices.size() ), box ) );
                } else {
                    for ( const MeshCluster& c : sorted ) {
                        clusters.push_back( { c.Bounds.Min, mesh->BaseIndexLocation + c.IndexOffset, c.Bounds.Max, c.IndexCount } );
                        cones.push_back( ComputeCone( *mesh, c.IndexOffset, c.IndexCount, c.Bounds ) );
                    }
                }
                mg.ClusterCount = static_cast<uint32_t>( clusters.size() ) - mg.ClusterFirst;
                meshes.push_back( mg );
            }
        }
    }
    if ( meshes.empty() ) return false;
    if ( meshes.size() > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ) {   // one group per mesh
        Logging::Wrn( "D3D12: {} world meshes exceed one dispatch; the world stays CPU-collected.", meshes.size() );
        return false;
    }
    m_MeshCount = meshes.size();
    m_ClusterCount = clusters.size();
    m_SectionCount = sections.size();
    // A run covers at least one cluster, so neither list can hold more commands than there are clusters.
    m_Capacity = static_cast<UINT>( clusters.size() );

    std::vector<MaterialGpu> materials( m_Materials.size() );
    for ( uint32_t i = 0; i < m_Materials.size(); ++i ) {
        Resolve( i, false );
        materials[i] = m_Materials[i].Gpu;
    }
    m_DirtyMaterials.clear();
    m_HeightEpoch = m_E.m_HeightPartitionEpoch;

    Rhi::Device* rhi = m_E.GetRhi();
    auto make = [&]( UINT64 bytes, bool uav, D3D12_RESOURCE_STATES state, const wchar_t* name, ComPtr<Rhi::Resource>& out ) {
        const D3D12_RESOURCE_DESC bd = BufferDesc( bytes, uav );
        if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &bd, state, nullptr, out.ReleaseAndGetAddressOf() ) ) ) return false;
        out->SetName( name );
        return true;
        };
    const UINT64 sectionBytes = sections.size() * sizeof( SectionGpu );
    const UINT64 meshBytes = meshes.size() * sizeof( MeshGpu );
    const UINT64 clusterBytes = clusters.size() * sizeof( ClusterGpu );
    const UINT64 materialBytes = materials.size() * sizeof( MaterialGpu );
    const UINT64 coneBytes = cones.size() * sizeof( ConeGpu );
    const UINT64 historyBytes = clusters.size() * sizeof( uint32_t );
    bool ok = make( sectionBytes, false, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuWorldSections", m_Sections )
        && make( meshBytes, false, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuWorldMeshes", m_Meshes )
        && make( clusterBytes, false, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuWorldClusters", m_Clusters )
        && make( materialBytes, false, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuWorldMaterials", m_MaterialBuffer )
        && make( m_Materials.size() * sizeof( uint32_t ), true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuWorldSeen", m_Seen )
        && make( coneBytes, false, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuWorldCones", m_Cones )
        && make( historyBytes, true, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuWorldHistory", m_History );
    for ( UINT v = 0; ok && v < kViews; ++v ) {
        ok = make( static_cast<UINT64>( CommandCapacity( v ) ) * 2u * CommandStride( v ), true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                L"GpuWorldArgs", m_Args[v] )
            && make( kArgCountBytes, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuWorldArgCount", m_ArgCount[v] );
        m_ArgsDrawable[v] = false;
    }
    for ( UINT i = 0; ok && i < kFrames; ++i ) {
        if ( !m_Staging[i] ) {
            const D3D12_RESOURCE_DESC sd = BufferDesc( kStagingBytes, false );
            ok = SUCCEEDED( rhi->CreateResource( D3D12_HEAP_TYPE_UPLOAD, &sd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_Staging[i].ReleaseAndGetAddressOf() ) );
            D3D12_RANGE noRead = { 0, 0 };
            void* mapped = nullptr;
            ok = ok && SUCCEEDED( m_Staging[i]->Map( 0, &noRead, &mapped ) );
            m_StagingPtr[i] = static_cast<uint8_t*>( mapped );
        }
        if ( !m_ExtCb[i] ) {
            const D3D12_RESOURCE_DESC cd = BufferDesc( static_cast<UINT64>( kViews ) * kExtCbStride, false );
            ok = ok && SUCCEEDED( rhi->CreateResource( D3D12_HEAP_TYPE_UPLOAD, &cd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_ExtCb[i].ReleaseAndGetAddressOf() ) );
            D3D12_RANGE noRead = { 0, 0 };
            void* mapped = nullptr;
            ok = ok && SUCCEEDED( m_ExtCb[i]->Map( 0, &noRead, &mapped ) );
            m_ExtCbPtr[i] = static_cast<uint8_t*>( mapped );
        }
        if ( !m_Stats[i] ) {
            const D3D12_RESOURCE_DESC sd = BufferDesc( kStatViews * kArgCountBytes, false );
            ok = ok && SUCCEEDED( rhi->CreateResource( D3D12_HEAP_TYPE_READBACK, &sd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, m_Stats[i].ReleaseAndGetAddressOf() ) );
            void* mapped = nullptr;
            ok = ok && SUCCEEDED( m_Stats[i]->Map( 0, nullptr, &mapped ) );
            m_StatsPtr[i] = static_cast<const uint32_t*>( mapped );
        }
        m_StatsPending[i] = false;
        const D3D12_RESOURCE_DESC rd = BufferDesc( m_Materials.size() * sizeof( uint32_t ), false );
        ok = ok && SUCCEEDED( rhi->CreateResource( D3D12_HEAP_TYPE_READBACK, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, m_Readback[i].ReleaseAndGetAddressOf() ) );
        void* mapped = nullptr;
        ok = ok && SUCCEEDED( m_Readback[i]->Map( 0, nullptr, &mapped ) );
        m_ReadbackPtr[i] = static_cast<const uint32_t*>( mapped );
        m_ReadbackPending[i] = false;
    }
    m_SeenReadable = false;
    if ( !ok ) {
        Logging::Wrn( "D3D12: GPU world buffers could not be created; the world stays CPU-collected." );
        return false;
    }

    // One-off upload through a temporary buffer, released once this frame retires.
    ComPtr<Rhi::Resource> upload;
    const D3D12_RESOURCE_DESC ud = BufferDesc( sectionBytes + meshBytes + clusterBytes + materialBytes + coneBytes + historyBytes, false );
    if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_UPLOAD, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, upload.GetAddressOf() ) ) )
        return false;
    uint8_t* p = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    if ( FAILED( upload->Map( 0, &noRead, reinterpret_cast<void**>( &p ) ) ) ) return false;
    UINT64 at = 0;
    const struct { Rhi::Resource* Dst; const void* Src; UINT64 Bytes; } parts[] = {
        { m_Sections.Get(), sections.data(), sectionBytes }, { m_Meshes.Get(), meshes.data(), meshBytes },
        { m_Clusters.Get(), clusters.data(), clusterBytes }, { m_MaterialBuffer.Get(), materials.data(), materialBytes },
        { m_Cones.Get(), cones.data(), coneBytes }, { m_History.Get(), nullptr, historyBytes } };
    for ( const auto& part : parts ) {
        if ( part.Src ) memcpy( p + at, part.Src, part.Bytes );
        else memset( p + at, 0, part.Bytes );   // no cluster was visible last frame
        cmd.CopyBufferRegion( part.Dst, 0, upload.Get(), at, part.Bytes );
        at += part.Bytes;
    }
    upload->Unmap( 0, nullptr );
    cmd.TransitionBarriers( {
        { m_Sections.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { m_Meshes.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { m_Clusters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { m_MaterialBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { m_Cones.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { m_History.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
    } );
    m_E.QueueResourceForRelease( std::move( upload ) );

    const size_t withCone = std::ranges::count_if( cones, []( const ConeGpu& c ) { return c.Cutoff <= 1.0f; } );
    Logging::Inf( "D3D12: GPU world built: {} meshes, {} clusters ({} with a normal cone), {} materials, {} sections, {} meshes on the CPU path",
        m_MeshCount, m_ClusterCount, withCone, m_Materials.size(), m_SectionCount, m_Specials.size() );
    return true;
}


void D3D12GpuWorld::ProcessFeedback() {
    // Freed bindless slots first: a material still naming one would sample whatever texture reuses it.
    {
        std::lock_guard<std::mutex> lock( m_FreedMutex );
        m_FreedScratch.swap( m_FreedSlots );
    }
    if ( !m_FreedScratch.empty() ) {
        const gtl::flat_hash_set<UINT> freed( m_FreedScratch.begin(), m_FreedScratch.end() );
        for ( uint32_t i = 0; i < m_Materials.size(); ++i ) {
            const MaterialGpu& g = m_Materials[i].Gpu;
            if ( freed.contains( g.Diffuse & MaterialFx::kDiffuseSlotMask ) || freed.contains( g.Normal ) || freed.contains( g.Orm & kOrmIndexMask ) )
                Resolve( i, false );
        }
        m_FreedScratch.clear();
    }

    // A height map arrived or the POM setting flipped: the alpha-run partition bit changes, a few hundred cheap resolves
    if ( m_HeightEpoch != m_E.m_HeightPartitionEpoch ) {
        m_HeightEpoch = m_E.m_HeightPartitionEpoch;
        for ( uint32_t i = 0; i < m_Materials.size(); ++i ) Resolve( i, false );
    }

    // The main view's seen flags, copied kBackBufferCount frames ago; that frame's fence was waited on.
    const UINT f = m_E.GetFrameIndex();
    if ( m_ReadbackPending[f] && m_ReadbackPtr[f] ) {
        m_ReadbackPending[f] = false;
        const uint32_t* seen = m_ReadbackPtr[f];
        for ( uint32_t i = 0; i < m_Materials.size(); ++i ) {
            Material& m = m_Materials[i];
            if ( !seen[i] || m.Queued ) continue;
            if ( m.Ready && !m.Animated && m_Frame - m.LastTouch < kTouchInterval ) continue;
            m.Queued = true;
            m_Queue.push_back( i );
        }
    }
    // Unresolved first: they draw with the fallback slots until then.
    std::stable_partition( m_Queue.begin(), m_Queue.end(), [this]( uint32_t i ) { return !m_Materials[i].Ready; } );
    const size_t n = std::min<size_t>( m_Queue.size(), kResolvesPerFrame );
    for ( size_t q = 0; q < n; ++q ) {
        m_Materials[m_Queue[q]].Queued = false;
        Resolve( m_Queue[q], true );
    }
    m_Queue.erase( m_Queue.begin(), m_Queue.begin() + n );

    // What only a cascade sees is never cached in here; pick it up once something else has.
    const uint32_t count = static_cast<uint32_t>( m_Materials.size() );
    for ( uint32_t k = 0; k < std::min( kRefreshPerFrame, count ); ++k ) {
        const uint32_t i = m_RefreshCursor;
        m_RefreshCursor = ( m_RefreshCursor + 1 ) % count;
        if ( !m_Materials[i].Ready ) Resolve( i, false );
    }
}


void D3D12GpuWorld::Upload( Rhi::CmdList& cmd ) {
    if ( m_DirtyMaterials.empty() ) return;
    std::ranges::sort( m_DirtyMaterials );
    m_DirtyMaterials.erase( std::ranges::unique( m_DirtyMaterials ).begin(), m_DirtyMaterials.end() );

    const UINT f = m_E.GetFrameIndex();
    uint8_t* const staging = m_StagingPtr[f];
    UINT cursor = 0;
    size_t done = 0;
    bool transitioned = false;
    while ( done < m_DirtyMaterials.size() ) {
        size_t run = 1;
        while ( done + run < m_DirtyMaterials.size() && m_DirtyMaterials[done + run] == m_DirtyMaterials[done] + run ) ++run;
        const UINT bytes = static_cast<UINT>( run * sizeof( MaterialGpu ) );
        if ( cursor + bytes > kStagingBytes ) break;
        if ( !transitioned ) {
            cmd.TransitionBarrier( m_MaterialBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST );
            transitioned = true;
        }
        for ( size_t i = 0; i < run; ++i )
            memcpy( staging + cursor + i * sizeof( MaterialGpu ), &m_Materials[m_DirtyMaterials[done + i]].Gpu, sizeof( MaterialGpu ) );
        cmd.CopyBufferRegion( m_MaterialBuffer.Get(), static_cast<UINT64>( m_DirtyMaterials[done] ) * sizeof( MaterialGpu ),
            m_Staging[f].Get(), cursor, bytes );
        cursor += bytes;
        done += run;
    }
    if ( transitioned )
        cmd.TransitionBarrier( m_MaterialBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
    m_DirtyMaterials.erase( m_DirtyMaterials.begin(), m_DirtyMaterials.begin() + done );
}


bool D3D12GpuWorld::BeginFrame( Rhi::CmdList& cmd ) {
    ++m_Frame;
    if ( !m_Built ) {
        if ( m_BuildFailed ) return false;
        if ( !Build( cmd ) ) {
            // With a world loaded the failure is final until the next one; without, it is just early.
            const bool worldLoaded = !Engine::GAPI->GetWorldSections().empty() && Engine::GAPI->GetWrappedWorldMesh();
            Reset();
            m_BuildFailed = worldLoaded;
            return false;
        }
        m_Built = true;
    }
    ZoneScopedN( "GpuWorld prepare" )
    const UINT f = m_E.GetFrameIndex();
    if ( m_StatsPending[f] && m_StatsPtr[f] && m_E.m_GpuTimers ) {
        static constexpr const char* kNames[kStatViews] = { "tris early", "tris late", "tris csm0", "tris csm1", "tris csm2" };
        for ( UINT s = 0; s < kStatViews; ++s ) m_E.m_GpuTimers->Note( kNames[s], m_StatsPtr[f][s * 4 + 2] / 3.0 );
    }
    m_StatsPending[f] = false;
    ProcessFeedback();
    Upload( cmd );
    return true;
}


bool D3D12GpuWorld::Cull( Rhi::CmdList& cmd, const View* views, UINT first, UINT count ) {
    const auto& pipe = m_E.m_Pipelines.Cull;
    if ( !m_Built || !pipe.WorldCullPSO || !pipe.WorldClearPSO || !pipe.WorldCullRootSig || m_MeshCount == 0 ) return false;
    count = std::min( count, kViews - std::min( first, kViews ) );
    bool any = false;
    for ( UINT i = 0; i < count; ++i ) any = any || views[i].Active;
    if ( !any ) return false;
    // The main view's material feedback is cleared by its first phase and copied out after its last.
    bool mainFirst = false, mainLast = false;
    for ( UINT i = 0; i < count; ++i ) {
        if ( !views[i].Active ) continue;
        const UINT v = first + i;
        if ( v == kViewMain ) {
            mainFirst = true;
            mainLast = mainLast || views[i].MainPhase == Phase::Single;
        }
        if ( v == kViewMainLate ) mainLast = true;
    }
    const bool main = mainFirst || mainLast;

    // Triangle compaction reads the world IB/VB; each compacting view owns an index buffer the size of the world IB.
    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    D3D12VertexBuffer* wvb = wm && wm->GetMeshVertexBuffer() ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
    D3D12VertexBuffer* wib = wm && wm->GetMeshIndexBuffer() ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;
    const bool worldBuffers = wvb && wib && wvb->GetResource() && wib->GetResource();
    bool compact[kViews] = {};
    for ( UINT i = 0; i < count; ++i ) {
        const UINT v = first + i;
        if ( !views[i].Active ) continue;
        m_Compacted[v] = false;
        if ( !views[i].Compact || !worldBuffers || views[i].CompactMapSize == 0 ) continue;
        if ( !m_Compact[v] ) {
            const D3D12_RESOURCE_DESC bd = BufferDesc( wib->GetSizeInBytes(), true );
            if ( FAILED( m_E.GetRhi()->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                    m_Compact[v].ReleaseAndGetAddressOf() ) ) ) {
                Logging::Wrn( "D3D12: the compacted cascade index buffer could not be created; triangle culling stays off." );
                continue;
            }
            m_Compact[v]->SetName( L"GpuWorldCompactIndices" );
            m_CompactReadable[v] = false;
        }
        compact[v] = true;
    }

    // Rest states the previous frame's draws and feedback copy left.
    {
        D3D12ResourceTransition pre[3 * kViews + 1] = {};
        UINT n = 0;
        for ( UINT i = 0; i < count; ++i ) {
            const UINT v = first + i;
            if ( !views[i].Active || !m_ArgsDrawable[v] ) continue;
            pre[n++] = { m_Args[v].Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            pre[n++] = { m_ArgCount[v].Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            m_ArgsDrawable[v] = false;
        }
        for ( UINT v = 0; v < kViews; ++v ) {
            if ( !compact[v] || !m_CompactReadable[v] ) continue;
            pre[n++] = { m_Compact[v].Get(), D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            m_CompactReadable[v] = false;
        }
        if ( mainFirst && m_SeenReadable ) {
            pre[n++] = { m_Seen.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            m_SeenReadable = false;
        }
        if ( n ) cmd.TransitionBarriers( pre, n );
    }

    // The main view mirrors CollectVisibleSections: a world-space radius around the camera, or whole sections
    // around its own when neither section intersections nor the section BVH are on.
    const GothicRendererSettings& rs = Engine::GAPI->GetRendererState().RendererSettings;
    const XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();
    const bool gridRadius = !rs.DrawSectionIntersections
        && !( rs.DebugSettings.FeatureSet.UseWorldSectionBVH && rs.DebugSettings.Culling.CullBspSections );
    const float radius = rs.SectionDrawRadius * WORLD_SECTION_SIZE;
    const INT2 camSection = WorldConverter::GetSectionOfPos( camPos );

    WorldCullCB cb = {};
    cb.CamPos = camPos;
    cb.MeshCount = static_cast<uint32_t>( m_MeshCount );
    cb.OpaqueCapacity = m_Capacity;
    cb.AlphaCapacity = m_Capacity;
    m_E.WetNormalFallback( cb.WetNormalSlot, cb.WetNormalStrength );
    cb.DefaultOrm = m_E.GetDefaultOrmSrvSlot();
    cb.MaterialCount = static_cast<uint32_t>( m_Materials.size() );
    cb.CamSectionX = camSection.x;
    cb.CamSectionY = camSection.y;
    cb.SectionGridRadius = rs.SectionDrawRadius;

    DX_ZONE( cmd.Get(), "GPU world cull" );
    // Early reads the history the last late phase wrote, late rewrites what early read.
    if ( main ) cmd.UAVBarrier( m_History.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING );
    cmd.SetComputeRootSignature( pipe.WorldCullRootSig.Get() );
    cmd.SetComputeRootShaderResourceView( 1, m_Sections->GetGPUVirtualAddress() );
    cmd.SetComputeRootShaderResourceView( 2, m_Meshes->GetGPUVirtualAddress() );
    cmd.SetComputeRootShaderResourceView( 3, m_Clusters->GetGPUVirtualAddress() );
    cmd.SetComputeRootShaderResourceView( 4, m_MaterialBuffer->GetGPUVirtualAddress() );
    cmd.SetComputeRootUnorderedAccessView( 9, m_History->GetGPUVirtualAddress() );
    cmd.SetComputeRootShaderResourceView( 10, m_Cones->GetGPUVirtualAddress() );
    // Real buffers even when no view compacts, since the reads are bounded by them only through the cluster table.
    cmd.SetComputeRootShaderResourceView( 11, worldBuffers ? wib->GetGpuVirtualAddress() : m_Clusters->GetGPUVirtualAddress() );
    cmd.SetComputeRootShaderResourceView( 12, worldBuffers ? wvb->GetGpuVirtualAddress() : m_Clusters->GetGPUVirtualAddress() );
    const UINT frame = m_E.GetFrameIndex();
    for ( UINT i = 0; i < count; ++i ) {
        if ( !views[i].Active ) continue;
        const UINT v = first + i;
        const bool isMain = v == kViewMain || v == kViewMainLate;
        const bool late = v == kViewMainLate;
        const uint32_t phase = !isMain ? 0u : late ? kViewLate : views[i].MainPhase == Phase::Early ? kViewEarly : 0u;
        cb.CullViewProj = views[i].CullViewProj;
        cb.ViewFlags = ( isMain ? kViewMainFlag | kViewFeedback | ( gridRadius ? kViewGridRadius : 0u ) : 0u ) | phase
            | ( views[i].NoFrustum ? kViewNoFrustum : 0u ) | ( views[i].Sphere ? kViewSphere | kViewCube : 0u )
            | ( compact[v] ? kViewCompact : 0u );

        WorldCullExtCB ext = {};
        ext.HiZViewProj = views[i].HiZViewProj;
        // A real texture slot even when unused, in case the compiler hoists the fetch out of its branch.
        ext.HiZIndex = late ? views[i].HiZIndex : ( m_E.m_HiZSrvSlot != UINT_MAX ? m_E.m_HiZSrvSlot : 0u );
        ext.HiZWidth = views[i].HiZWidth;
        ext.HiZHeight = views[i].HiZHeight;
        ext.HiZMipCount = late ? views[i].HiZMipCount : 0u;
        ext.ConeEye = views[i].ConeEye;
        ext.ExtFlags = isMain && views[i].Cones ? kExtCones : 0u;
        if ( compact[v] ) {
            ext.HiZViewProj = views[i].CompactViewProj;
            ext.HiZWidth = ext.HiZHeight = views[i].CompactMapSize;
        }
        cmd.SetComputeRootUnorderedAccessView( 13, compact[v] ? m_Compact[v]->GetGPUVirtualAddress() : m_History->GetGPUVirtualAddress() );
        memcpy( m_ExtCbPtr[frame] + v * kExtCbStride, &ext, sizeof( ext ) );
        cmd.SetComputeRootConstantBufferView( 8, m_ExtCb[frame]->GetGPUVirtualAddress() + v * kExtCbStride );
        cb.SectionRadiusSq = isMain && !gridRadius ? radius * radius : 0.0f;
        // A sphere view's sections and boxes are both tested against the light.
        cb.CamPos = views[i].Sphere ? views[i].Center : camPos;
        if ( views[i].Sphere ) cb.SectionRadiusSq = views[i].Radius * views[i].Radius;
        cb.OpaqueCapacity = cb.AlphaCapacity = CommandCapacity( v );
        cmd.SetComputeRoot32BitConstants( 0, sizeof( cb ) / sizeof( uint32_t ), &cb, 0 );
        cmd.SetComputeRootUnorderedAccessView( 5, m_Args[v]->GetGPUVirtualAddress() );
        cmd.SetComputeRootUnorderedAccessView( 6, m_ArgCount[v]->GetGPUVirtualAddress() );
        // u2: the main view's seen flags, a sphere view's bake report, else a stand-in (m_Seen may sit in COPY_SOURCE).
        const D3D12_GPU_VIRTUAL_ADDRESS feedback = isMain ? m_Seen->GetGPUVirtualAddress()
            : views[i].Report ? views[i].Report : m_ArgCount[v]->GetGPUVirtualAddress();
        cmd.SetComputeRootUnorderedAccessView( 7, feedback );
        cmd.SetPipelineState( pipe.WorldClearPSO.Get() );
        cmd.Dispatch( isMain && !late ? std::max<UINT>( 1u, ( cb.MaterialCount + 63 ) / 64 ) : 1u, 1, 1 );
        cmd.UAVBarrier( m_ArgCount[v].Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING );
        if ( isMain ) cmd.UAVBarrier( m_Seen.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING );
        cmd.SetPipelineState( pipe.WorldCullPSO.Get() );
        cmd.Dispatch( static_cast<UINT>( m_MeshCount ), 1, 1 );
    }

    {
        D3D12ResourceTransition post[3 * kViews + 1] = {};
        UINT n = 0;
        for ( UINT i = 0; i < count; ++i ) {
            const UINT v = first + i;
            if ( !views[i].Active ) continue;
            if ( compact[v] ) {
                post[n++] = { m_Compact[v].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDEX_BUFFER };
                m_CompactReadable[v] = m_Compacted[v] = true;
            }
            post[n++] = { m_Args[v].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
            post[n++] = { m_ArgCount[v].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
            m_ArgsDrawable[v] = true;
        }
        if ( mainLast ) post[n++] = { m_Seen.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
        cmd.TransitionBarriers( post, n );
    }
    if ( mainLast ) {
        m_SeenReadable = true;
        const UINT f = m_E.GetFrameIndex();
        if ( m_Readback[f] ) {
            cmd.CopyBufferRegion( m_Readback[f].Get(), 0, m_Seen.Get(), 0, m_Materials.size() * sizeof( uint32_t ) );
            m_ReadbackPending[f] = true;
        }
    }
    return true;
}


bool D3D12GpuWorld::RequestResident( uint32_t material ) {
    if ( material >= m_Materials.size() ) return true;   // the tables were rebuilt since it was reported
    Material& m = m_Materials[material];
    if ( m.Ready ) return true;
    if ( !m.Queued ) {
        m.Queued = true;
        m_Queue.push_back( material );   // resolved with CacheIn by ProcessFeedback
    }
    return false;
}


void D3D12GpuWorld::RecordStats( Rhi::CmdList& cmd ) {
    const UINT f = m_E.GetFrameIndex();
    if ( !m_Built || !m_Stats[f] ) return;
    static constexpr UINT kStatView[kStatViews] = { kViewMain, kViewMainLate, 1, 2, 3 };
    D3D12ResourceTransition pre[kStatViews] = {}, post[kStatViews] = {};
    UINT n = 0;
    for ( UINT s = 0; s < kStatViews; ++s ) {
        if ( !m_ArgsDrawable[kStatView[s]] ) continue;
        pre[n] = { m_ArgCount[kStatView[s]].Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_SOURCE };
        post[n++] = { m_ArgCount[kStatView[s]].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
    }
    if ( n == 0 ) return;
    cmd.TransitionBarriers( pre, n );
    for ( UINT s = 0; s < kStatViews; ++s ) {
        if ( m_ArgsDrawable[kStatView[s]] )
            cmd.CopyBufferRegion( m_Stats[f].Get(), s * kArgCountBytes, m_ArgCount[kStatView[s]].Get(), 0, kArgCountBytes );
    }
    cmd.TransitionBarriers( post, n );
    m_StatsPending[f] = true;
}


void D3D12GpuWorld::Draw( Rhi::CmdList& cmd, UINT view, bool alphaTested ) const {
    cmd.ExecuteIndirect( m_E.m_WorldIndirectCmdSig.Get(), m_Capacity, m_Args[view].Get(),
        alphaTested ? static_cast<UINT64>( m_Capacity ) * kCommandStride : 0ull,
        m_ArgCount[view].Get(), alphaTested ? sizeof( uint32_t ) : 0u );
}


void D3D12GpuWorld::DrawCube( Rhi::CmdList& cmd, UINT view, bool alphaTested, Rhi::CommandSignature* sig ) const {
    cmd.ExecuteIndirect( sig, kPointCommandCapacity, m_Args[view].Get(),
        alphaTested ? static_cast<UINT64>( kPointCommandCapacity ) * kCubeCommandStride : 0ull,
        m_ArgCount[view].Get(), alphaTested ? sizeof( uint32_t ) : 0u );
}


UINT D3D12GpuWorld::CommandCapacity( UINT view ) const {
    // A sphere around one light holds a few hundred runs; the other views can need one per cluster.
    return IsPointView( view ) ? kPointCommandCapacity : m_Capacity;
}


UINT D3D12GpuWorld::CommandStride( UINT view ) {
    return IsPointView( view ) ? kCubeCommandStride : kCommandStride;
}
