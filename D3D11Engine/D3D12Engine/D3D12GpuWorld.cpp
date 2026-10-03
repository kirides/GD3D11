// D3D12GpuWorld — the world mesh culled per view on the GPU. See GPU_SCENE_PLAN.md and WorldCull.hlsl.
#include "../pch.h"
#include "D3D12GpuWorld.h"
#include "D3D12GraphicsEngine.h"
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
    static_assert( sizeof( SectionGpu ) == 32 && sizeof( MeshGpu ) == 40 && sizeof( ClusterGpu ) == 32, "WorldCull.hlsl structs" );
    static_assert( sizeof( WorldCullCB ) == 31 * sizeof( uint32_t ), "CreateWorldCull's 31 root constants" );

    constexpr uint32_t kMeshMain = 1u, kMeshCaster = 2u, kMeshPoint = 4u;
    constexpr uint32_t kMaterialAlpha = 1u, kMaterialReady = 2u;
    constexpr uint32_t kViewMainFlag = 1u, kViewNoFrustum = 2u, kViewFeedback = 4u, kViewGridRadius = 8u;
    constexpr uint32_t kViewSphere = 16u, kViewCube = 32u;
    constexpr uint32_t kTouchInterval = 30;       // frames between CacheIn touches of a seen material
    constexpr uint32_t kResolvesPerFrame = 64;
    constexpr uint32_t kRefreshPerFrame = 32;     // unresolved materials re-polled without CacheIn
    constexpr uint32_t kOrmIndexMask = 0x0FFFFFFFu;
    constexpr UINT kCommandStride = 36;   // WorldDrawCommand, WorldCull.hlsl WORLD_DRAW_COMMAND_STRIDE
    constexpr UINT kCubeCommandStride = 24;   // PointShadowCasterCommand, WorldCull.hlsl CUBE_COMMAND_STRIDE

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
              std::addressof( m_MaterialBuffer ), std::addressof( m_Seen ) } )
        if ( *r ) m_E.QueueResourceForRelease( std::move( *r ) );
    for ( UINT v = 0; v < kViews; ++v ) {
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
                } else {
                    for ( const MeshCluster& c : sorted )
                        clusters.push_back( { c.Bounds.Min, mesh->BaseIndexLocation + c.IndexOffset, c.Bounds.Max, c.IndexCount } );
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
    bool ok = make( sectionBytes, false, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuWorldSections", m_Sections )
        && make( meshBytes, false, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuWorldMeshes", m_Meshes )
        && make( clusterBytes, false, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuWorldClusters", m_Clusters )
        && make( materialBytes, false, D3D12_RESOURCE_STATE_COPY_DEST, L"GpuWorldMaterials", m_MaterialBuffer )
        && make( m_Materials.size() * sizeof( uint32_t ), true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuWorldSeen", m_Seen );
    for ( UINT v = 0; ok && v < kViews; ++v ) {
        ok = make( static_cast<UINT64>( CommandCapacity( v ) ) * 2u * CommandStride( v ), true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                L"GpuWorldArgs", m_Args[v] )
            && make( 2u * sizeof( uint32_t ), true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"GpuWorldArgCount", m_ArgCount[v] );
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
    const D3D12_RESOURCE_DESC ud = BufferDesc( sectionBytes + meshBytes + clusterBytes + materialBytes, false );
    if ( FAILED( rhi->CreateResource( D3D12_HEAP_TYPE_UPLOAD, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, upload.GetAddressOf() ) ) )
        return false;
    uint8_t* p = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    if ( FAILED( upload->Map( 0, &noRead, reinterpret_cast<void**>( &p ) ) ) ) return false;
    UINT64 at = 0;
    const struct { Rhi::Resource* Dst; const void* Src; UINT64 Bytes; } parts[] = {
        { m_Sections.Get(), sections.data(), sectionBytes }, { m_Meshes.Get(), meshes.data(), meshBytes },
        { m_Clusters.Get(), clusters.data(), clusterBytes }, { m_MaterialBuffer.Get(), materials.data(), materialBytes } };
    for ( const auto& part : parts ) {
        memcpy( p + at, part.Src, part.Bytes );
        cmd.CopyBufferRegion( part.Dst, 0, upload.Get(), at, part.Bytes );
        at += part.Bytes;
    }
    upload->Unmap( 0, nullptr );
    cmd.TransitionBarriers( {
        { m_Sections.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { m_Meshes.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { m_Clusters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { m_MaterialBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
    } );
    m_E.QueueResourceForRelease( std::move( upload ) );

    Logging::Inf( "D3D12: GPU world built: {} meshes, {} clusters, {} materials, {} sections, {} meshes on the CPU path",
        m_MeshCount, m_ClusterCount, m_Materials.size(), m_SectionCount, m_Specials.size() );
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
    const bool main = first == kViewMain && views[0].Active;

    // Rest states the previous frame's draws and feedback copy left.
    {
        D3D12ResourceTransition pre[2 * kViews + 1] = {};
        UINT n = 0;
        for ( UINT i = 0; i < count; ++i ) {
            const UINT v = first + i;
            if ( !views[i].Active || !m_ArgsDrawable[v] ) continue;
            pre[n++] = { m_Args[v].Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            pre[n++] = { m_ArgCount[v].Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            m_ArgsDrawable[v] = false;
        }
        if ( main && m_SeenReadable ) {
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
    cmd.SetComputeRootSignature( pipe.WorldCullRootSig.Get() );
    cmd.SetComputeRootShaderResourceView( 1, m_Sections->GetGPUVirtualAddress() );
    cmd.SetComputeRootShaderResourceView( 2, m_Meshes->GetGPUVirtualAddress() );
    cmd.SetComputeRootShaderResourceView( 3, m_Clusters->GetGPUVirtualAddress() );
    cmd.SetComputeRootShaderResourceView( 4, m_MaterialBuffer->GetGPUVirtualAddress() );
    for ( UINT i = 0; i < count; ++i ) {
        if ( !views[i].Active ) continue;
        const UINT v = first + i;
        const bool isMain = v == kViewMain;
        cb.CullViewProj = views[i].CullViewProj;
        cb.ViewFlags = ( isMain ? kViewMainFlag | kViewFeedback | ( gridRadius ? kViewGridRadius : 0u ) : 0u )
            | ( views[i].NoFrustum ? kViewNoFrustum : 0u ) | ( views[i].Sphere ? kViewSphere | kViewCube : 0u );
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
        cmd.Dispatch( isMain ? std::max<UINT>( 1u, ( cb.MaterialCount + 63 ) / 64 ) : 1u, 1, 1 );
        cmd.UAVBarrier( m_ArgCount[v].Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING );
        if ( isMain ) cmd.UAVBarrier( m_Seen.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING );
        cmd.SetPipelineState( pipe.WorldCullPSO.Get() );
        cmd.Dispatch( static_cast<UINT>( m_MeshCount ), 1, 1 );
    }

    {
        D3D12ResourceTransition post[2 * kViews + 1] = {};
        UINT n = 0;
        for ( UINT i = 0; i < count; ++i ) {
            const UINT v = first + i;
            if ( !views[i].Active ) continue;
            post[n++] = { m_Args[v].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
            post[n++] = { m_ArgCount[v].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
            m_ArgsDrawable[v] = true;
        }
        if ( main ) post[n++] = { m_Seen.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
        cmd.TransitionBarriers( post, n );
    }
    if ( main ) {
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
    return view >= kViewPointFirst ? kPointCommandCapacity : m_Capacity;
}


UINT D3D12GpuWorld::CommandStride( UINT view ) {
    return view >= kViewPointFirst ? kCubeCommandStride : kCommandStride;
}
