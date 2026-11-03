#include "pch.h"
#include "StaticLightFarField.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "WorldObjects.h"
#include "WorldMeshSection.h"
#include "ThreadPool.h"
#include "VertexPacking.h"
#include "zCVob.h"
#include "zCVobLight.h"
#include "zCMaterial.h"
#include "zCTexture.h"
#include <mutex>

namespace {
    constexpr uint32_t kCacheMagic = 0x46464C53;   // 'SLFF'
    constexpr uint32_t kCacheVersion = 6;
    constexpr float kLightFactor = 1.2f;           // BuildFrameLightBuffer / CullLights RGB scale
    constexpr uint32_t kWorldOwner = UINT32_MAX;     // Tri::Owner of world-mesh triangles (VOBs: their index)
    constexpr uint32_t kSkipNone = UINT32_MAX - 1;   // Occluded(): no owner is skipped
    constexpr float kSurfaceBias = 4.0f;           // ray start off the surface, world units
    constexpr float kCornerPull = 15.0f;           // corner samples move this far into their triangle
    constexpr float kLightClearance = 8.0f;        // ignore hits this close to the light; wall torches sit ~20 from the wall
    constexpr float kMaxLampVobSize = 300.0f;      // bbox diagonal: a VOB this small around the light is its lamp
    constexpr float kLargeVobSize = 400.0f;        // bbox diagonal: sampled around its shell, not at its centre
    constexpr float kShellOffset = 20.0f;          // shell samples sit this far outside the bbox
    constexpr float kVobOmniFactor = 0.5f;         // a VOB has no normal: average facing
    constexpr float kVobFarMax = 4.0f;             // VOB irradiance range of the 8-bit sqrt encoding
    constexpr float kGridCell = 2000.0f;
    constexpr float kMinOccluderVobSize = 150.0f;  // bbox diagonal; props smaller than this cast nothing
    constexpr size_t kRecheckFrames = 120;
    constexpr double kMinRebakeSeconds = 5.0;

    /** Wall clock: ZenGin's timer stands still during a world load. */
    double NowSeconds() {
        return std::chrono::duration<double>( std::chrono::steady_clock::now().time_since_epoch() ).count();
    }

    uint32_t PackRGB9E5( float r, float g, float b ) {
        constexpr float kMax = 65408.0f;
        r = std::clamp( r, 0.0f, kMax ); g = std::clamp( g, 0.0f, kMax ); b = std::clamp( b, 0.0f, kMax );
        const float m = std::max( r, std::max( g, b ) );
        if ( m < 1.0e-6f ) return 0;
        int e = std::max( -16, static_cast<int>( std::floor( std::log2( m ) ) ) ) + 16;
        float denom = std::exp2( static_cast<float>( e - 15 - 9 ) );
        if ( static_cast<int>( std::floor( m / denom + 0.5f ) ) == 512 ) { denom *= 2.0f; ++e; }
        auto q = [denom]( float v ) { return std::min( 511u, static_cast<uint32_t>( std::floor( v / denom + 0.5f ) ) ); };
        return q( r ) | ( q( g ) << 9 ) | ( q( b ) << 18 ) | ( static_cast<uint32_t>( e ) << 27 );
    }

    /** Bytes of the instance color the D3D12 VOB shader decodes; green (byte 1) stays the ground light. */
    uint32_t PackVob( const XMFLOAT3& e ) {
        auto enc = []( float v, float levels ) {
            return static_cast<uint32_t>( std::sqrt( std::clamp( v / kVobFarMax, 0.0f, 1.0f ) ) * levels + 0.5f );
        };
        // byte 0 = blue, byte 2 = red, byte 3 = 0b10 tag + 6-bit green (a plain 0xFF alpha never decodes)
        return enc( e.z, 255.0f ) | ( enc( e.x, 255.0f ) << 16 ) | ( ( 0x80u | enc( e.y, 63.0f ) ) << 24 );
    }

    XMFLOAT3 DecodeOct( uint32_t packed ) {
        const float x = static_cast<int16_t>( packed & 0xFFFF ) / 32767.0f;
        const float y = static_cast<int16_t>( packed >> 16 ) / 32767.0f;
        XMFLOAT3 n( x, y, 1.0f - std::abs( x ) - std::abs( y ) );
        const float t = std::clamp( -n.z, 0.0f, 1.0f );
        n.x += n.x >= 0.0f ? -t : t;
        n.y += n.y >= 0.0f ? -t : t;
        XMStoreFloat3( &n, XMVector3Normalize( XMLoadFloat3( &n ) ) );
        return n;
    }

    uint64_t Fnv( uint64_t h, const void* data, size_t bytes ) {
        const uint8_t* p = static_cast<const uint8_t*>( data );
        for ( size_t i = 0; i < bytes; ++i ) h = ( h ^ p[i] ) * 1099511628211ull;
        return h;
    }

    struct Aabb {
        XMFLOAT3 Min{ FLT_MAX, FLT_MAX, FLT_MAX }, Max{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
        void Add( const XMFLOAT3& p ) {
            Min = { std::min( Min.x, p.x ), std::min( Min.y, p.y ), std::min( Min.z, p.z ) };
            Max = { std::max( Max.x, p.x ), std::max( Max.y, p.y ), std::max( Max.z, p.z ) };
        }
        bool Overlaps( const Aabb& o ) const {
            return Min.x <= o.Max.x && Max.x >= o.Min.x && Min.y <= o.Max.y && Max.y >= o.Min.y
                && Min.z <= o.Max.z && Max.z >= o.Min.z;
        }
        bool Contains( const XMFLOAT3& p ) const {
            return p.x >= Min.x && p.x <= Max.x && p.y >= Min.y && p.y <= Max.y && p.z >= Min.z && p.z <= Max.z;
        }
    };

    struct Tri { XMFLOAT3 V0, E1, E2; uint32_t Owner; };

    /** Shadow-ray BVH over one light's occluders; built per light and thrown away. */
    class TriBvh {
    public:
        void Build( std::vector<Tri>&& tris ) {
            m_Tris = std::move( tris );
            m_Nodes.clear();
            m_Order.resize( m_Tris.size() );
            m_Centroid.resize( m_Tris.size() );
            for ( uint32_t i = 0; i < m_Tris.size(); ++i ) {
                m_Order[i] = i;
                const Tri& t = m_Tris[i];
                m_Centroid[i] = XMFLOAT3( t.V0.x + ( t.E1.x + t.E2.x ) / 3.0f, t.V0.y + ( t.E1.y + t.E2.y ) / 3.0f,
                    t.V0.z + ( t.E1.z + t.E2.z ) / 3.0f );
            }
            if ( m_Tris.empty() ) return;
            m_Nodes.reserve( m_Tris.size() / 2 + 1 );
            m_Nodes.emplace_back();
            Subdivide( 0, 0, static_cast<uint32_t>( m_Tris.size() ) );
            m_Centroid.clear(); m_Centroid.shrink_to_fit();
        }

        /** Any hit along origin + dir * t, t in (0, tMax), ignoring the triangles of `skipOwner`. */
        bool Occluded( const XMFLOAT3& o, const XMFLOAT3& d, float tMax, uint32_t skipOwner ) const {
            if ( m_Nodes.empty() ) return false;
            const XMFLOAT3 inv( 1.0f / ( d.x != 0.0f ? d.x : 1e-20f ), 1.0f / ( d.y != 0.0f ? d.y : 1e-20f ),
                1.0f / ( d.z != 0.0f ? d.z : 1e-20f ) );
            uint32_t stack[64];
            uint32_t sp = 0;
            stack[sp++] = 0;
            while ( sp ) {
                const Node& n = m_Nodes[stack[--sp]];
                if ( !RayBox( n, o, inv, tMax ) ) continue;
                if ( n.Count ) {
                    for ( uint32_t i = 0; i < n.Count; ++i ) {
                        const Tri& t = m_Tris[m_Order[n.First + i]];
                        if ( t.Owner != skipOwner && RayTri( t, o, d, tMax ) ) return true;
                    }
                } else if ( sp + 2 <= 64 ) {
                    stack[sp++] = n.First;
                    stack[sp++] = n.First + 1;
                }
            }
            return false;
        }

    private:
        struct Node { XMFLOAT3 Min; uint32_t First; XMFLOAT3 Max; uint32_t Count; };

        void Subdivide( uint32_t nodeIndex, uint32_t first, uint32_t count ) {
            Aabb box, cbox;
            for ( uint32_t i = first; i < first + count; ++i ) {
                const Tri& t = m_Tris[m_Order[i]];
                box.Add( t.V0 );
                box.Add( XMFLOAT3( t.V0.x + t.E1.x, t.V0.y + t.E1.y, t.V0.z + t.E1.z ) );
                box.Add( XMFLOAT3( t.V0.x + t.E2.x, t.V0.y + t.E2.y, t.V0.z + t.E2.z ) );
                cbox.Add( m_Centroid[m_Order[i]] );
            }
            m_Nodes[nodeIndex].Min = box.Min;
            m_Nodes[nodeIndex].Max = box.Max;
            if ( count <= 4 ) {
                m_Nodes[nodeIndex].First = first;
                m_Nodes[nodeIndex].Count = count;
                return;
            }
            const XMFLOAT3 ext( cbox.Max.x - cbox.Min.x, cbox.Max.y - cbox.Min.y, cbox.Max.z - cbox.Min.z );
            const int axis = ext.x > ext.y && ext.x > ext.z ? 0 : ( ext.y > ext.z ? 1 : 2 );
            auto key = [this, axis]( uint32_t i ) {
                const XMFLOAT3& c = m_Centroid[i];
                return axis == 0 ? c.x : ( axis == 1 ? c.y : c.z );
            };
            const uint32_t mid = first + count / 2;
            std::nth_element( m_Order.begin() + first, m_Order.begin() + mid, m_Order.begin() + first + count,
                [&]( uint32_t a, uint32_t b ) { return key( a ) < key( b ); } );
            const uint32_t left = static_cast<uint32_t>( m_Nodes.size() );
            m_Nodes.emplace_back();
            m_Nodes.emplace_back();
            m_Nodes[nodeIndex].First = left;
            m_Nodes[nodeIndex].Count = 0;
            Subdivide( left, first, mid - first );
            Subdivide( left + 1, mid, first + count - mid );
        }

        static bool RayBox( const Node& n, const XMFLOAT3& o, const XMFLOAT3& inv, float tMax ) {
            float t0 = 0.0f, t1 = tMax;
            const float lo[3] = { ( n.Min.x - o.x ) * inv.x, ( n.Min.y - o.y ) * inv.y, ( n.Min.z - o.z ) * inv.z };
            const float hi[3] = { ( n.Max.x - o.x ) * inv.x, ( n.Max.y - o.y ) * inv.y, ( n.Max.z - o.z ) * inv.z };
            for ( int a = 0; a < 3; ++a ) {
                t0 = std::max( t0, std::min( lo[a], hi[a] ) );
                t1 = std::min( t1, std::max( lo[a], hi[a] ) );
            }
            return t0 <= t1;
        }

        static bool RayTri( const Tri& t, const XMFLOAT3& o, const XMFLOAT3& d, float tMax ) {
            const XMVECTOR D = XMLoadFloat3( &d ), E1 = XMLoadFloat3( &t.E1 ), E2 = XMLoadFloat3( &t.E2 );
            const XMVECTOR P = XMVector3Cross( D, E2 );
            const float det = XMVectorGetX( XMVector3Dot( E1, P ) );
            if ( std::abs( det ) < 1e-8f ) return false;
            const float invDet = 1.0f / det;
            const XMVECTOR T = XMVectorSubtract( XMLoadFloat3( &o ), XMLoadFloat3( &t.V0 ) );
            const float u = XMVectorGetX( XMVector3Dot( T, P ) ) * invDet;
            if ( u < 0.0f || u > 1.0f ) return false;
            const XMVECTOR Q = XMVector3Cross( T, E1 );
            const float v = XMVectorGetX( XMVector3Dot( D, Q ) ) * invDet;
            if ( v < 0.0f || u + v > 1.0f ) return false;
            const float dist = XMVectorGetX( XMVector3Dot( E2, Q ) ) * invDet;
            return dist > 0.0f && dist < tMax;
        }

        std::vector<Tri> m_Tris;
        std::vector<uint32_t> m_Order;
        std::vector<XMFLOAT3> m_Centroid;
        std::vector<Node> m_Nodes;
    };

    struct LightSnap {
        zCVobLight* Vob;
        XMFLOAT3 Pos;
        float Range;
        XMFLOAT3 Color;   // linear-ish, already scaled by kLightFactor like the GPU light
    };

    struct ClusterRef {
        const WorldMeshInfo* Mesh;
        uint32_t IndexOffset, IndexCount;
        Aabb Bounds;
        bool Occluder;
    };

    struct VobRef {
        VobInfo* Info;
        XMFLOAT4X4 World;
        Aabb Bounds;
        XMFLOAT3 Center;
        float Diagonal;
        bool Occluder;
        std::vector<const MeshInfo*> Meshes;   // opaque sub-meshes, only for occluders
    };

    /** Uniform XZ grid of item indices; an item is listed in every cell its box overlaps. */
    class Grid {
    public:
        void Insert( const Aabb& b, uint32_t item ) {
            const int x0 = Cell( b.Min.x ), x1 = Cell( b.Max.x ), z0 = Cell( b.Min.z ), z1 = Cell( b.Max.z );
            for ( int x = x0; x <= x1; ++x )
                for ( int z = z0; z <= z1; ++z ) m_Cells[Key( x, z )].push_back( item );
        }
        void Query( const Aabb& b, std::vector<uint32_t>& out ) const {
            out.clear();
            const int x0 = Cell( b.Min.x ), x1 = Cell( b.Max.x ), z0 = Cell( b.Min.z ), z1 = Cell( b.Max.z );
            for ( int x = x0; x <= x1; ++x )
                for ( int z = z0; z <= z1; ++z ) {
                    auto it = m_Cells.find( Key( x, z ) );
                    if ( it != m_Cells.end() ) out.insert( out.end(), it->second.begin(), it->second.end() );
                }
            std::sort( out.begin(), out.end() );
            out.erase( std::unique( out.begin(), out.end() ), out.end() );
        }
        void Clear() { m_Cells.clear(); }
    private:
        static int Cell( float v ) { return static_cast<int>( std::floor( v / kGridCell ) ); }
        static uint64_t Key( int x, int z ) { return ( static_cast<uint64_t>( static_cast<uint32_t>( x ) ) << 32 ) | static_cast<uint32_t>( z ); }
        gtl::flat_hash_map<uint64_t, std::vector<uint32_t>> m_Cells;
    };

    // Alpha-tested surfaces occlude too: many opaque walls carry an alpha channel, and a leak is worse than a
    // fence that shadows a little too much. Only water-like and blended materials let light through.
    bool IsOccluderMaterial( const MeshKey& key ) {
        if ( key.Info && key.Info->MaterialType != MaterialInfo::MT_None ) return false;   // water, lava, ...
        if ( key.Material ) {
            const int f = key.Material->GetAlphaFunc();
            if ( f == zMAT_ALPHA_FUNC_BLEND || f == zMAT_ALPHA_FUNC_ADD ) return false;
        }
        return true;
    }

    XMFLOAT3 Xform( const XMFLOAT4X4& m, const XMFLOAT3& p ) {
        // ZenGin matrices are column-vector: row k dotted with (p, 1)
        return XMFLOAT3( m._11 * p.x + m._12 * p.y + m._13 * p.z + m._14,
            m._21 * p.x + m._22 * p.y + m._23 * p.z + m._24,
            m._31 * p.x + m._32 * p.y + m._33 * p.z + m._34 );
    }

    float Falloff( float dist, float range ) {
        const float nd = std::clamp( 1.0f - dist / range, 0.0f, 1.0f );
        return nd * ( nd * 0.2f + 0.8f );   // PLS_ComputeRangeFalloff, as ApplyTiledLight
    }
}

struct StaticLightFarField::Impl {
    std::vector<uint32_t> Normals;        // oct-encoded, wrapped vertex order
    std::vector<LightSnap> Lights;        // what the current bake contains
    bool Baked = false;
    size_t LastCheckFrame = 0;
    double LastBakeTime = -1.0e9;
    bool RebakePending = false;
    bool Attempted = false;   // Bake() ran for this world: the vobs are all registered
};

StaticLightFarField::StaticLightFarField() : m( std::make_unique<Impl>() ) {}
StaticLightFarField::~StaticLightFarField() = default;

StaticLightFarField& GetStaticLightFarField() {
    static StaticLightFarField s_FarField;
    return s_FarField;
}

void StaticLightFarField::CaptureWorldVertices( const std::vector<ExVertexStruct>& wrapped ) {
    m->Normals.resize( wrapped.size() );
    for ( size_t i = 0; i < wrapped.size(); ++i ) {
        float ox, oy;
        VertexPacking::EncodeOctNormal( wrapped[i].Normal, ox, oy );
        const auto q = []( float v ) { return static_cast<uint32_t>( static_cast<uint16_t>( static_cast<int16_t>( std::lround( std::clamp( v, -1.0f, 1.0f ) * 32767.0f ) ) ) ); };
        m->Normals[i] = q( ox ) | ( q( oy ) << 16 );
    }
}

static void ClearLightFlags() {
    for ( auto& [vob, info] : Engine::GAPI->GetVobLightMap() )
        if ( info ) info->FarFieldBaked = false;
}

void StaticLightFarField::Reset() {
    ClearLightFlags();
    m->Lights.clear();
    m->Normals.clear();
    m->Normals.shrink_to_fit();
    m->Baked = false;
    m->RebakePending = false;
    m->Attempted = false;
    m_VertexIrradiance.clear();
    m_VertexIrradiance.shrink_to_fit();
    ++m_Version;
}

void StaticLightFarField::Crossfade( bool active, float& nearRadius, float& invFade ) {
    const GothicRendererSettings& s = Engine::GAPI->GetRendererState().RendererSettings;
    nearRadius = s.StaticLightFarFieldBakeOnly ? -1.0e7f : s.StaticLightNearRadius;   // debug: the bake everywhere
    invFade = active ? 1.0f / std::max( 1.0f, s.StaticLightFadeWidth ) : 0.0f;
}

bool StaticLightFarField::IsActive() const {
    return m->Baked && Engine::GAPI->GetRendererState().RendererSettings.StaticLightFarField;
}

void StaticLightFarField::Bake( const std::vector<VobInfo*>& vobs ) {
    ZoneScopedN( "StaticLightFarField::Bake" );
    m->RebakePending = false;
    m->Attempted = true;
    m->LastBakeTime = NowSeconds();
    ClearLightFlags();
    for ( VobInfo* vi : vobs ) vi->FarFieldColor = 0;
    m->Lights.clear();
    m->Baked = false;
    m_VertexIrradiance.clear();
    ++m_Version;
    if ( !Engine::GAPI->GetRendererState().RendererSettings.StaticLightFarField || m->Normals.empty() ) return;

    const auto t0 = std::chrono::steady_clock::now();

    // Static level lights: never moved, not carried, not a PFX
    std::vector<VobLightInfo*> lightInfos;
    for ( auto& [vob, info] : Engine::GAPI->GetVobLightMap() ) {
        if ( !info || !vob || info->IsPFXVobLight || !vob->IsStatic() || !vob->IsEnabled() || vob->GetVobParent() ) continue;
        const float range = vob->GetLightRange();
        if ( range <= 1.0f ) continue;
        const DWORD c = vob->GetLightColor();
        LightSnap s;
        s.Vob = vob;
        s.Pos = vob->GetPositionWorld();
        s.Range = range;
        s.Color = XMFLOAT3( ( ( c >> 16 ) & 0xFF ) / 255.0f * kLightFactor, ( ( c >> 8 ) & 0xFF ) / 255.0f * kLightFactor,
            ( c & 0xFF ) / 255.0f * kLightFactor );
        m->Lights.push_back( s );
        lightInfos.push_back( info );
    }

    // World clusters, in GPU vertex order terms (mesh base vertex + local index)
    std::vector<ClusterRef> clusters;
    for ( auto& [x, row] : Engine::GAPI->GetWorldSections() ) {
        for ( auto& [z, section] : row ) {
            for ( auto& [key, mesh] : section.WorldMeshes ) {
                if ( !mesh || mesh->Indices.empty() || mesh->CpuVertices.empty() ) continue;
                const bool occluder = IsOccluderMaterial( key );
                auto add = [&]( uint32_t off, uint32_t cnt ) {
                    ClusterRef c{ mesh, off, cnt, {}, occluder };
                    for ( uint32_t i = off; i < off + cnt && i < mesh->Indices.size(); ++i )
                        if ( mesh->Indices[i] < mesh->CpuVertices.size() ) c.Bounds.Add( mesh->CpuVertices[mesh->Indices[i]].Position );
                    clusters.push_back( c );
                };
                if ( mesh->Clusters.empty() ) add( 0, static_cast<uint32_t>( mesh->Indices.size() ) );
                else for ( const MeshCluster& mc : mesh->Clusters ) add( mc.IndexOffset, mc.IndexCount );
            }
        }
    }

    // VOBs: every one is a target, the large opaque static ones also occlude
    std::vector<VobRef> vobRefs;
    vobRefs.reserve( vobs.size() );
    const double waitUntil = NowSeconds() + 10.0;
    for ( VobInfo* vi : vobs ) {
        if ( !vi || !vi->Vob ) continue;
        VobRef r;
        r.Info = vi;
        r.World = *vi->Vob->GetWorldMatrixPtr();
        const zTBBox3D& bb = vi->Vob->GetBBox();
        r.Bounds.Min = bb.Min;
        r.Bounds.Max = bb.Max;
        r.Center = XMFLOAT3( ( bb.Min.x + bb.Max.x ) * 0.5f, ( bb.Min.y + bb.Max.y ) * 0.5f, ( bb.Min.z + bb.Max.z ) * 0.5f );
        const float diag = XMVectorGetX( XMVector3Length( XMVectorSubtract( XMLoadFloat3( &bb.Max ), XMLoadFloat3( &bb.Min ) ) ) );
        auto* visual = static_cast<MeshVisualInfo*>( vi->VisualInfo );
        r.Diagonal = diag;
        // In a BSP leaf = placed in the level; Gothic's staticVob flag isn't reliable on buildings
        r.Occluder = visual && diag >= kMinOccluderVobSize && ( !vi->ParentBSPNodes.empty() || vi->Vob->GetFlags().StaticVob )
            && !visual->MorphMeshVisual;
        if ( r.Occluder ) {
            // Extraction was queued during the vob traversal; it finishes on the worker pool shortly
            while ( !visual->GetIsReady() && NowSeconds() < waitUntil ) std::this_thread::yield();
            if ( !visual->GetIsReady() ) r.Occluder = false;
        }
        if ( r.Occluder ) {
            for ( auto& [key, meshes] : visual->MeshesByTexture )
                if ( IsOccluderMaterial( key ) )
                    for ( const MeshInfo* mi : meshes ) if ( mi && !mi->Indices.empty() ) r.Meshes.push_back( mi );
            r.Occluder = !r.Meshes.empty();
        }
        vobRefs.push_back( std::move( r ) );
    }
    // Stable order for the cache: by position
    std::sort( vobRefs.begin(), vobRefs.end(), []( const VobRef& a, const VobRef& b ) {
        if ( a.Center.x != b.Center.x ) return a.Center.x < b.Center.x;
        if ( a.Center.y != b.Center.y ) return a.Center.y < b.Center.y;
        return a.Center.z < b.Center.z;
        } );

    // Cache key: geometry, lights, VOB placement
    uint64_t hash = 1469598103934665603ull;
    hash = Fnv( hash, &kCacheVersion, sizeof( kCacheVersion ) );
    hash = Fnv( hash, m->Normals.data(), m->Normals.size() * sizeof( uint32_t ) );
    for ( const LightSnap& l : m->Lights ) {
        hash = Fnv( hash, &l.Pos, sizeof( l.Pos ) );
        hash = Fnv( hash, &l.Range, sizeof( l.Range ) );
        hash = Fnv( hash, &l.Color, sizeof( l.Color ) );
    }
    for ( const VobRef& r : vobRefs ) {
        hash = Fnv( hash, &r.Center, sizeof( r.Center ) );
        hash = Fnv( hash, &r.Occluder, sizeof( r.Occluder ) );
    }
    for ( const ClusterRef& c : clusters ) hash = Fnv( hash, &c.Bounds, sizeof( c.Bounds ) );

    const WorldInfo* world = Engine::GAPI->GetLoadedWorldInfo();
    const std::string cacheDir = std::string( ENGINE_BASE_DIR ) + "cache";
    const std::string cachePath = cacheDir + "\\FarField_" + ( world ? world->WorldName : std::string( "unknown" ) ) + ".bin";
    const uint32_t vertexCount = static_cast<uint32_t>( m->Normals.size() );
    const uint32_t vobCount = static_cast<uint32_t>( vobRefs.size() );

    std::vector<uint32_t> vobPacked( vobCount, 0 );
    bool fromCache = false;
    if ( FILE* f = fopen( cachePath.c_str(), "rb" ) ) {
        uint32_t hdr[4] = {};
        uint64_t fileHash = 0;
        if ( fread( hdr, sizeof( hdr ), 1, f ) == 1 && fread( &fileHash, sizeof( fileHash ), 1, f ) == 1
            && hdr[0] == kCacheMagic && hdr[1] == kCacheVersion && hdr[2] == vertexCount && hdr[3] == vobCount && fileHash == hash ) {
            m_VertexIrradiance.resize( vertexCount );
            fromCache = fread( m_VertexIrradiance.data(), sizeof( uint32_t ), vertexCount, f ) == vertexCount
                && ( vobCount == 0 || fread( vobPacked.data(), sizeof( uint32_t ), vobCount, f ) == vobCount );
        }
        fclose( f );
    }

    if ( !fromCache ) {
        Grid clusterGrid, vobGrid;
        for ( uint32_t i = 0; i < clusters.size(); ++i ) clusterGrid.Insert( clusters[i].Bounds, i );
        for ( uint32_t i = 0; i < vobRefs.size(); ++i ) vobGrid.Insert( vobRefs[i].Bounds, i );

        std::vector<XMFLOAT3> vertexAccum( vertexCount, XMFLOAT3( 0, 0, 0 ) );
        std::vector<XMFLOAT3> vobAccum( vobCount, XMFLOAT3( 0, 0, 0 ) );
        std::mutex accumMutex;
        const std::vector<uint32_t>& normals = m->Normals;

        auto bakeLight = [&]( const LightSnap& L, std::vector<uint32_t>& scratch ) {
            Aabb sphereBox;
            sphereBox.Min = XMFLOAT3( L.Pos.x - L.Range, L.Pos.y - L.Range, L.Pos.z - L.Range );
            sphereBox.Max = XMFLOAT3( L.Pos.x + L.Range, L.Pos.y + L.Range, L.Pos.z + L.Range );
            const float rangeSq = L.Range * L.Range;
            auto inRange = [&]( const XMFLOAT3& p ) {
                const float dx = p.x - L.Pos.x, dy = p.y - L.Pos.y, dz = p.z - L.Pos.z;
                return dx * dx + dy * dy + dz * dz < rangeSq;
            };

            std::vector<Tri> tris;
            // One sample per triangle corner, pulled into that triangle: a vertex on a wall seam is shared by
            // (or sits beside) surfaces on both sides, and must only be lit if every one of them sees the light
            struct VertTarget { uint32_t Id; XMFLOAT3 Pos; XMFLOAT3 Inward; };
            std::vector<VertTarget> targets;
            clusterGrid.Query( sphereBox, scratch );
            for ( uint32_t ci : scratch ) {
                const ClusterRef& c = clusters[ci];
                if ( !c.Bounds.Overlaps( sphereBox ) ) continue;
                const auto& verts = c.Mesh->CpuVertices;
                const auto& idx = c.Mesh->Indices;
                for ( uint32_t i = c.IndexOffset; i + 2 < c.IndexOffset + c.IndexCount && i + 2 < idx.size(); i += 3 ) {
                    if ( idx[i] >= verts.size() || idx[i + 1] >= verts.size() || idx[i + 2] >= verts.size() ) continue;
                    const XMFLOAT3& p0 = verts[idx[i]].Position;
                    const XMFLOAT3& p1 = verts[idx[i + 1]].Position;
                    const XMFLOAT3& p2 = verts[idx[i + 2]].Position;
                    Aabb tb; tb.Add( p0 ); tb.Add( p1 ); tb.Add( p2 );
                    if ( !tb.Overlaps( sphereBox ) ) continue;
                    if ( c.Occluder )
                        tris.push_back( { p0, XMFLOAT3( p1.x - p0.x, p1.y - p0.y, p1.z - p0.z ),
                            XMFLOAT3( p2.x - p0.x, p2.y - p0.y, p2.z - p0.z ), kWorldOwner } );
                    const XMFLOAT3 centroid( ( p0.x + p1.x + p2.x ) / 3.0f, ( p0.y + p1.y + p2.y ) / 3.0f, ( p0.z + p1.z + p2.z ) / 3.0f );
                    for ( int k = 0; k < 3; ++k ) {
                        const XMFLOAT3& p = verts[idx[i + k]].Position;
                        if ( !inRange( p ) ) continue;
                        XMFLOAT3 in( centroid.x - p.x, centroid.y - p.y, centroid.z - p.z );
                        const float len = std::sqrt( in.x * in.x + in.y * in.y + in.z * in.z );
                        const float pull = len > 1e-3f ? std::min( kCornerPull, len * 0.5f ) / len : 0.0f;
                        targets.push_back( { c.Mesh->BaseVertexLocation + idx[i + k], p, XMFLOAT3( in.x * pull, in.y * pull, in.z * pull ) } );
                    }
                }
            }
            std::vector<uint32_t> vobTargets;
            vobGrid.Query( sphereBox, scratch );
            for ( uint32_t vi : scratch ) {
                const VobRef& r = vobRefs[vi];
                if ( !r.Bounds.Overlaps( sphereBox ) ) continue;
                if ( r.Diagonal > kLargeVobSize || inRange( r.Center ) ) vobTargets.push_back( vi );
                if ( !r.Occluder || ( r.Diagonal <= kMaxLampVobSize && r.Bounds.Contains( L.Pos ) ) ) continue;   // its lamp, not its house
                for ( const MeshInfo* mi : r.Meshes ) {
                    const size_t vertexCount = mi->Vertices.size();
                    for ( size_t i = 0; i + 2 < mi->Indices.size(); i += 3 ) {
                        if ( mi->Indices[i] >= vertexCount || mi->Indices[i + 1] >= vertexCount || mi->Indices[i + 2] >= vertexCount ) continue;
                        const XMFLOAT3 p0 = Xform( r.World, mi->Vertices[mi->Indices[i]].Position );
                        const XMFLOAT3 p1 = Xform( r.World, mi->Vertices[mi->Indices[i + 1]].Position );
                        const XMFLOAT3 p2 = Xform( r.World, mi->Vertices[mi->Indices[i + 2]].Position );
                        Aabb tb; tb.Add( p0 ); tb.Add( p1 ); tb.Add( p2 );
                        if ( !tb.Overlaps( sphereBox ) ) continue;
                        tris.push_back( { p0, XMFLOAT3( p1.x - p0.x, p1.y - p0.y, p1.z - p0.z ),
                            XMFLOAT3( p2.x - p0.x, p2.y - p0.y, p2.z - p0.z ), vi } );
                    }
                }
            }
            if ( targets.empty() && vobTargets.empty() ) return;

            std::sort( targets.begin(), targets.end(), []( const VertTarget& a, const VertTarget& b ) { return a.Id < b.Id; } );

            TriBvh bvh;
            bvh.Build( std::move( tris ) );

            // Traced from the light outward, so the clearance skips the lamp's own geometry
            auto visible = [&]( const XMFLOAT3& p, float dist, uint32_t owner ) {
                const XMFLOAT3 dir( ( p.x - L.Pos.x ) / dist, ( p.y - L.Pos.y ) / dist, ( p.z - L.Pos.z ) / dist );
                const float start = std::min( kLightClearance, dist * 0.5f );
                const XMFLOAT3 o( L.Pos.x + dir.x * start, L.Pos.y + dir.y * start, L.Pos.z + dir.z * start );
                return !bvh.Occluded( o, dir, dist - start, owner );
            };

            std::vector<std::pair<uint32_t, XMFLOAT3>> vertOut;
            vertOut.reserve( targets.size() );
            for ( size_t g = 0; g < targets.size(); ) {
                size_t end = g + 1;
                while ( end < targets.size() && targets[end].Id == targets[g].Id ) ++end;
                const VertTarget& t = targets[g];
                const size_t first = g;
                g = end;
                if ( t.Id >= normals.size() ) continue;
                const XMFLOAT3 n = DecodeOct( normals[t.Id] );
                const XMFLOAT3 lp( L.Pos.x - t.Pos.x, L.Pos.y - t.Pos.y, L.Pos.z - t.Pos.z );
                const float dist = std::sqrt( lp.x * lp.x + lp.y * lp.y + lp.z * lp.z );
                if ( dist < 1.0f || dist >= L.Range ) continue;
                const float ndl = ( n.x * lp.x + n.y * lp.y + n.z * lp.z ) / dist;
                if ( ndl <= 0.0f ) continue;
                bool lit = true;
                for ( size_t c = first; c < end && lit; ++c ) {
                    const XMFLOAT3& in = targets[c].Inward;
                    const XMFLOAT3 p( t.Pos.x + in.x + n.x * kSurfaceBias, t.Pos.y + in.y + n.y * kSurfaceBias,
                        t.Pos.z + in.z + n.z * kSurfaceBias );
                    const XMFLOAT3 lb( L.Pos.x - p.x, L.Pos.y - p.y, L.Pos.z - p.z );
                    const float distB = std::sqrt( lb.x * lb.x + lb.y * lb.y + lb.z * lb.z );
                    lit = distB >= 1.0f && visible( p, distB, kSkipNone );
                }
                if ( !lit ) continue;
                const float w = Falloff( dist, L.Range ) * ndl;
                vertOut.emplace_back( t.Id, XMFLOAT3( L.Color.x * w, L.Color.y * w, L.Color.z * w ) );
            }
            std::vector<std::pair<uint32_t, XMFLOAT3>> vobOut;
            for ( uint32_t vi : vobTargets ) {
                const VobRef& r = vobRefs[vi];
                float w = 0.0f;
                if ( r.Diagonal > kLargeVobSize ) {
                    // A building's centre is inside it: average 8 corners + 6 face centres just outside its shell,
                    // with its own walls occluding, so interior lights stay inside
                    const XMFLOAT3 half( ( r.Bounds.Max.x - r.Bounds.Min.x ) * 0.5f + kShellOffset,
                        ( r.Bounds.Max.y - r.Bounds.Min.y ) * 0.5f + kShellOffset, ( r.Bounds.Max.z - r.Bounds.Min.z ) * 0.5f + kShellOffset );
                    static const float kDirs[14][3] = {
                        { -1, -1, -1 }, { 1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 }, { -1, -1, 1 }, { 1, -1, 1 }, { -1, 1, 1 }, { 1, 1, 1 },
                        { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
                    for ( const auto& d : kDirs ) {
                        const XMFLOAT3 p( r.Center.x + d[0] * half.x, r.Center.y + d[1] * half.y, r.Center.z + d[2] * half.z );
                        const XMFLOAT3 lp( L.Pos.x - p.x, L.Pos.y - p.y, L.Pos.z - p.z );
                        const float dist = std::sqrt( lp.x * lp.x + lp.y * lp.y + lp.z * lp.z );
                        if ( dist >= L.Range || ( dist > 1.0f && !visible( p, dist, kSkipNone ) ) ) continue;
                        w += Falloff( dist, L.Range );
                    }
                    w *= kVobOmniFactor / 14.0f;
                } else {
                    const XMFLOAT3& p = r.Center;
                    const XMFLOAT3 lp( L.Pos.x - p.x, L.Pos.y - p.y, L.Pos.z - p.z );
                    const float dist = std::sqrt( lp.x * lp.x + lp.y * lp.y + lp.z * lp.z );
                    if ( dist >= L.Range || ( dist > 1.0f && !visible( p, dist, vi ) ) ) continue;
                    w = Falloff( dist, L.Range ) * kVobOmniFactor;
                }
                if ( w > 0.0f ) vobOut.emplace_back( vi, XMFLOAT3( L.Color.x * w, L.Color.y * w, L.Color.z * w ) );
            }

            std::scoped_lock lock( accumMutex );
            for ( const auto& [id, c] : vertOut ) {
                XMFLOAT3& a = vertexAccum[id];
                a.x += c.x; a.y += c.y; a.z += c.z;
            }
            for ( const auto& [id, c] : vobOut ) {
                XMFLOAT3& a = vobAccum[id];
                a.x += c.x; a.y += c.y; a.z += c.z;
            }
        };

        // Lights in a strided split so neighbours (similar cost) spread across the workers
        const size_t lightCount = m->Lights.size();
        const size_t jobs = std::max<size_t>( 1, std::min<size_t>( lightCount, Engine::WorkerThreadPool->getNumThreads() * 2 ) );
        std::vector<std::future<void>> futures;
        futures.reserve( jobs );
        for ( size_t j = 0; j < jobs; ++j ) {
            futures.emplace_back( Engine::WorkerThreadPool->enqueue( [&, j]( const std::stop_token& ) {
                ZoneScopedN( "StaticLightFarField bake job" );
                std::vector<uint32_t> scratch;
                for ( size_t i = j; i < lightCount; i += jobs ) bakeLight( m->Lights[i], scratch );
                } ).future );
        }
        for ( auto& f : futures ) if ( f.valid() ) f.wait();

        m_VertexIrradiance.resize( vertexCount );
        for ( uint32_t i = 0; i < vertexCount; ++i )
            m_VertexIrradiance[i] = PackRGB9E5( vertexAccum[i].x, vertexAccum[i].y, vertexAccum[i].z );
        for ( uint32_t i = 0; i < vobCount; ++i ) vobPacked[i] = PackVob( vobAccum[i] );

        CreateDirectoryA( cacheDir.c_str(), nullptr );
        if ( FILE* f = fopen( cachePath.c_str(), "wb" ) ) {
            const uint32_t hdr[4] = { kCacheMagic, kCacheVersion, vertexCount, vobCount };
            fwrite( hdr, sizeof( hdr ), 1, f );
            fwrite( &hash, sizeof( hash ), 1, f );
            fwrite( m_VertexIrradiance.data(), sizeof( uint32_t ), vertexCount, f );
            if ( vobCount ) fwrite( vobPacked.data(), sizeof( uint32_t ), vobCount, f );
            fclose( f );
        }
    }

    for ( uint32_t i = 0; i < vobCount; ++i ) vobRefs[i].Info->FarFieldColor = vobPacked[i];
    for ( VobLightInfo* info : lightInfos ) info->FarFieldBaked = true;
    m->Baked = true;
    ++m_Version;

    const double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
    Logging::Inf( "StaticLightFarField: {} lights, {} vertices, {} vobs {} in {:.0f} ms",
        m->Lights.size(), vertexCount, vobCount, fromCache ? "loaded from cache" : "baked", ms );
}

bool StaticLightFarField::NeedsRebake() {
    const GothicRendererSettings& s = Engine::GAPI->GetRendererState().RendererSettings;
    if ( !s.StaticLightFarField ) {
        // Switched off: forget the bake so every light shades normally again
        if ( m->Baked ) {
            ClearLightFlags();
            m->Baked = false;
            m->Lights.clear();
            m_VertexIrradiance.clear();
            ++m_Version;
        }
        return false;
    }
    if ( m->Normals.empty() || !m->Attempted ) return false;   // no world, or still loading it
    const double now = NowSeconds();
    if ( !m->Baked ) return now - m->LastBakeTime >= kMinRebakeSeconds;

    const size_t frame = Engine::GAPI->GetFrameNumber();
    if ( frame - m->LastCheckFrame >= kRecheckFrames ) {
        m->LastCheckFrame = frame;
        const auto& lightMap = Engine::GAPI->GetVobLightMap();
        for ( const LightSnap& l : m->Lights ) {
            const auto it = lightMap.find( l.Vob );
            bool changed = it == lightMap.end() || !it->second;
            if ( !changed ) {
                zCVobLight* vob = l.Vob;
                const XMFLOAT3 p = vob->GetPositionWorld();
                // Not the range: range animations change it every frame
                changed = !vob->IsEnabled() || std::abs( p.x - l.Pos.x ) > 1.0f || std::abs( p.y - l.Pos.y ) > 1.0f
                    || std::abs( p.z - l.Pos.z ) > 1.0f;
                // Until the re-bake it shades as a normal light; the stale bake still holds it
                if ( changed ) it->second->FarFieldBaked = false;
            }
            if ( changed ) m->RebakePending = true;
        }
    }
    return m->RebakePending && now - m->LastBakeTime >= kMinRebakeSeconds;
}
