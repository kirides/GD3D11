#include "pch.h"
#include "ShoreField.h"
#include "BaseGraphicsEngine.h"
#include "Engine.h"
#include "GfxTexture.h"
#include "GothicAPI.h"
#include "WorldMeshSection.h"
#include "WorldObjects.h"
#include "zCMaterial.h"
#include <DirectXPackedVector.h>
#include <chrono>

using namespace DirectX;

namespace {
    constexpr int   kMaxResolution = 1024;
    constexpr float kMinCellSize = 64.0f;          // world units per texel
    constexpr float kSeaMargin = 4000.0f;          // how far past the land the field reaches out to sea
    constexpr float kFlatWaterNormalY = 0.77f;     // steeper water is a waterfall, not a surface
    constexpr float kFloorBelowSurface = 5.0f;     // ground this far under the water is its floor
    constexpr float kOverhangClearance = 250.0f;   // ground this high over a floor is a bridge or arch, not shore
    constexpr float kNoFloorDepth = 5000.0f;
    constexpr float kMaxDistance = 30000.0f;
    constexpr float kNone = -FLT_MAX;
    constexpr float kFar = 1e20f;                  // distance transform: not a site
    constexpr int   kBlurRadius = 2;               // texels; smooths the seaward direction

    enum class ETriangleKind { Skip, Water, Ground };

    struct Grid {
        int Width = 0;
        int Height = 0;
        float MinX = 0.0f;
        float MinZ = 0.0f;
        float Cell = 1.0f;
        size_t Index( int x, int z ) const { return static_cast<size_t>( z ) * Width + x; }
        size_t Count() const { return static_cast<size_t>( Width ) * Height; }
    };

    ETriangleKind ClassifyMesh( const MeshKey& key ) {
        if ( !key.Info ) return ETriangleKind::Skip;
        if ( key.Info->IsWater() ) return ETriangleKind::Water;
        switch ( key.Info->MaterialType ) {
        case MaterialInfo::MT_Portal:
        case MaterialInfo::MT_WaterfallFoam:
        case MaterialInfo::MT_FullAlpha:
            return ETriangleKind::Skip;
        default:
            break;
        }
        if ( key.Material ) {
            const auto alphaFunc = key.Material->GetAlphaFunc();
            if ( alphaFunc > zRND_ALPHA_FUNC_NONE && alphaFunc != zRND_ALPHA_FUNC_TEST ) return ETriangleKind::Skip;
        }
        return ETriangleKind::Ground;
    }

    /** Calls visit( kind, a, b, c ) for every world mesh triangle; flat water only, waterfalls are skipped. */
    template <typename Visit>
    void ForEachTriangle( const std::map<int, std::map<int, WorldMeshSectionInfo>>& sections, Visit&& visit ) {
        for ( const auto& [sx, row] : sections ) {
            for ( const auto& [sz, section] : row ) {
                for ( const auto& [key, mesh] : section.WorldMeshes ) {
                    if ( !mesh ) continue;
                    const ETriangleKind kind = ClassifyMesh( key );
                    if ( kind == ETriangleKind::Skip ) continue;

                    const std::vector<WorldVertexCPU>* cpu = mesh->GetCpuVertices();
                    const size_t vertexCount = cpu ? cpu->size() : mesh->Vertices.size();
                    auto position = [&]( VERTEX_INDEX i ) -> const XMFLOAT3& {
                        return cpu ? ( *cpu )[i].Position : mesh->Vertices[i].Position;
                    };
                    for ( size_t i = 0; i + 2 < mesh->Indices.size(); i += 3 ) {
                        const VERTEX_INDEX i0 = mesh->Indices[i], i1 = mesh->Indices[i + 1], i2 = mesh->Indices[i + 2];
                        if ( i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount ) continue;
                        const XMFLOAT3& a = position( i0 );
                        const XMFLOAT3& b = position( i1 );
                        const XMFLOAT3& c = position( i2 );
                        if ( kind == ETriangleKind::Water ) {
                            const XMVECTOR n = XMVector3Cross( XMVectorSubtract( XMLoadFloat3( &b ), XMLoadFloat3( &a ) ),
                                                               XMVectorSubtract( XMLoadFloat3( &c ), XMLoadFloat3( &a ) ) );
                            const float len = XMVectorGetX( XMVector3Length( n ) );
                            if ( len <= 0.0f || std::abs( XMVectorGetY( n ) ) < kFlatWaterNormalY * len ) continue;
                        }
                        visit( kind, a, b, c );
                    }
                }
            }
        }
    }

    /** Visits every texel center inside the triangle's xz projection with its height there, plus the texels
        of its corners, so slivers (cliff faces) and small triangles still leave a mark. */
    template <typename Write>
    void RasterizeTopDown( const Grid& g, const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c, Write&& write ) {
        // Grid space: texel centers sit on integer coordinates
        const float ax = ( a.x - g.MinX ) / g.Cell - 0.5f, az = ( a.z - g.MinZ ) / g.Cell - 0.5f;
        const float bx = ( b.x - g.MinX ) / g.Cell - 0.5f, bz = ( b.z - g.MinZ ) / g.Cell - 0.5f;
        const float cx = ( c.x - g.MinX ) / g.Cell - 0.5f, cz = ( c.z - g.MinZ ) / g.Cell - 0.5f;

        auto splat = [&]( float x, float z, float y ) {
            const int ix = static_cast<int>( std::lround( x ) );
            const int iz = static_cast<int>( std::lround( z ) );
            if ( ix >= 0 && iz >= 0 && ix < g.Width && iz < g.Height ) write( g.Index( ix, iz ), y );
        };
        splat( ax, az, a.y );
        splat( bx, bz, b.y );
        splat( cx, cz, c.y );

        const float area = ( bx - ax ) * ( cz - az ) - ( bz - az ) * ( cx - ax );
        if ( std::abs( area ) < 1e-6f ) return;
        const int x0 = std::max( 0, static_cast<int>( std::ceil( std::min( { ax, bx, cx } ) ) ) );
        const int x1 = std::min( g.Width - 1, static_cast<int>( std::floor( std::max( { ax, bx, cx } ) ) ) );
        const int z0 = std::max( 0, static_cast<int>( std::ceil( std::min( { az, bz, cz } ) ) ) );
        const int z1 = std::min( g.Height - 1, static_cast<int>( std::floor( std::max( { az, bz, cz } ) ) ) );
        const float invArea = 1.0f / area;
        for ( int z = z0; z <= z1; ++z ) {
            const float pz = static_cast<float>( z );
            for ( int x = x0; x <= x1; ++x ) {
                const float px = static_cast<float>( x );
                const float w0 = ( ( cx - bx ) * ( pz - bz ) - ( cz - bz ) * ( px - bx ) ) * invArea;
                const float w1 = ( ( ax - cx ) * ( pz - cz ) - ( az - cz ) * ( px - cx ) ) * invArea;
                const float w2 = 1.0f - w0 - w1;
                if ( w0 < 0.0f || w1 < 0.0f || w2 < 0.0f ) continue;
                write( g.Index( x, z ), w0 * a.y + w1 * b.y + w2 * c.y );
            }
        }
    }

    /** Felzenszwalb-Huttenlocher squared distance transform of one row or column, in texels. */
    void DistanceTransform1D( const float* f, float* d, int n, int* v, double* z ) {
        int k = 0;
        v[0] = 0;
        z[0] = -1e30;
        z[1] = 1e30;
        for ( int q = 1; q < n; ++q ) {
            double s;
            for ( ;; ) {
                const int p = v[k];
                s = ( ( static_cast<double>( f[q] ) + static_cast<double>( q ) * q )
                    - ( static_cast<double>( f[p] ) + static_cast<double>( p ) * p ) ) / ( 2.0 * q - 2.0 * p );
                if ( s > z[k] ) break;
                --k;   // z[0] is -inf, so this stops at 0
            }
            ++k;
            v[k] = q;
            z[k] = s;
            z[k + 1] = 1e30;
        }
        k = 0;
        for ( int q = 0; q < n; ++q ) {
            while ( z[k + 1] < q ) ++k;
            const double dq = static_cast<double>( q - v[k] );
            d[q] = static_cast<float>( dq * dq + f[v[k]] );
        }
    }

    /** In place: 0 at sites, kFar elsewhere -> squared distance in texels to the nearest site. */
    void DistanceTransform2D( std::vector<float>& field, const Grid& g ) {
        const int n = std::max( g.Width, g.Height );
        std::vector<float> f( n ), d( n );
        std::vector<int> v( n );
        std::vector<double> z( n + 1 );
        for ( int x = 0; x < g.Width; ++x ) {
            for ( int iz = 0; iz < g.Height; ++iz ) f[iz] = field[g.Index( x, iz )];
            DistanceTransform1D( f.data(), d.data(), g.Height, v.data(), z.data() );
            for ( int iz = 0; iz < g.Height; ++iz ) field[g.Index( x, iz )] = d[iz];
        }
        for ( int iz = 0; iz < g.Height; ++iz ) {
            float* rowData = &field[g.Index( 0, iz )];
            std::copy( rowData, rowData + g.Width, f.begin() );
            DistanceTransform1D( f.data(), rowData, g.Width, v.data(), z.data() );
        }
    }

    /** Separable box blur with clamped edges. */
    void BoxBlur( const std::vector<float>& src, std::vector<float>& dst, const Grid& g, int radius ) {
        std::vector<float> tmp( src.size() );
        const float norm = 1.0f / static_cast<float>( 2 * radius + 1 );
        for ( int iz = 0; iz < g.Height; ++iz ) {
            for ( int x = 0; x < g.Width; ++x ) {
                float sum = 0.0f;
                for ( int k = -radius; k <= radius; ++k ) sum += src[g.Index( std::clamp( x + k, 0, g.Width - 1 ), iz )];
                tmp[g.Index( x, iz )] = sum * norm;
            }
        }
        for ( int iz = 0; iz < g.Height; ++iz ) {
            for ( int x = 0; x < g.Width; ++x ) {
                float sum = 0.0f;
                for ( int k = -radius; k <= radius; ++k ) sum += tmp[g.Index( x, std::clamp( iz + k, 0, g.Height - 1 ) )];
                dst[g.Index( x, iz )] = sum * norm;
            }
        }
    }

    template <typename T>
    void Release( std::vector<T>& v ) { std::vector<T>().swap( v ); }
}

ShoreField::~ShoreField() = default;

std::unique_ptr<ShoreField> ShoreField::Bake( const std::map<int, std::map<int, WorldMeshSectionInfo>>& sections ) {
    ZoneScoped;
    const auto start = std::chrono::steady_clock::now();

    // Bounds: where water and ground overlap, plus a margin out to sea
    XMFLOAT2 waterMin( FLT_MAX, FLT_MAX ), waterMax( -FLT_MAX, -FLT_MAX );
    XMFLOAT2 groundMin( FLT_MAX, FLT_MAX ), groundMax( -FLT_MAX, -FLT_MAX );
    ForEachTriangle( sections, [&]( ETriangleKind kind, const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c ) {
        XMFLOAT2& lo = kind == ETriangleKind::Water ? waterMin : groundMin;
        XMFLOAT2& hi = kind == ETriangleKind::Water ? waterMax : groundMax;
        for ( const XMFLOAT3* p : { &a, &b, &c } ) {
            lo.x = std::min( lo.x, p->x ); lo.y = std::min( lo.y, p->z );
            hi.x = std::max( hi.x, p->x ); hi.y = std::max( hi.y, p->z );
        }
    } );
    if ( waterMin.x > waterMax.x || groundMin.x > groundMax.x ) {
        Logging::Inf( "Shore field: no flat water next to ground in this world" );
        return nullptr;
    }

    Grid g;
    g.MinX = std::max( waterMin.x, groundMin.x ) - kSeaMargin;
    g.MinZ = std::max( waterMin.y, groundMin.y ) - kSeaMargin;
    const float maxX = std::min( waterMax.x, groundMax.x ) + kSeaMargin;
    const float maxZ = std::min( waterMax.y, groundMax.y ) + kSeaMargin;
    if ( maxX <= g.MinX || maxZ <= g.MinZ ) {
        Logging::Inf( "Shore field: water and ground don't overlap" );
        return nullptr;
    }
    g.Cell = std::max( kMinCellSize, std::max( maxX - g.MinX, maxZ - g.MinZ ) / kMaxResolution );
    g.Width = std::clamp( static_cast<int>( std::ceil( ( maxX - g.MinX ) / g.Cell ) ), 1, kMaxResolution );
    g.Height = std::clamp( static_cast<int>( std::ceil( ( maxZ - g.MinZ ) / g.Cell ) ), 1, kMaxResolution );

    std::vector<uint16_t> texels;
    size_t landCount = 0;
    size_t waterCount = 0;
    try {
        // Water surface height, then the ground: the highest point under the water is its floor, anything at
        // or above the surface is dry land unless it is a bridge or arch high above a floor
        std::vector<float> water( g.Count(), kNone );
        std::vector<float> top( g.Count(), kNone );
        std::vector<float> floor( g.Count(), kNone );
        ForEachTriangle( sections, [&]( ETriangleKind kind, const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c ) {
            if ( kind != ETriangleKind::Water ) return;
            RasterizeTopDown( g, a, b, c, [&]( size_t i, float y ) { water[i] = std::max( water[i], y ); } );
        } );
        ForEachTriangle( sections, [&]( ETriangleKind kind, const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c ) {
            if ( kind != ETriangleKind::Ground ) return;
            RasterizeTopDown( g, a, b, c, [&]( size_t i, float y ) {
                if ( water[i] != kNone && y < water[i] - kFloorBelowSurface ) floor[i] = std::max( floor[i], y );
                else top[i] = std::max( top[i], y );
            } );
        } );

        // Classify; `water` becomes the depth, `top`/`floor` the distance-transform inputs
        std::vector<uint8_t> land( g.Count() );
        for ( size_t i = 0; i < g.Count(); ++i ) {
            land[i] = top[i] != kNone && ( floor[i] == kNone || top[i] - floor[i] < kOverhangClearance );
            float depth = 0.0f;
            if ( !land[i] ) {
                depth = ( water[i] != kNone && floor[i] != kNone ) ? std::min( water[i] - floor[i], kNoFloorDepth ) : kNoFloorDepth;
                ++waterCount;
            } else {
                ++landCount;
            }
            water[i] = depth;
            top[i] = land[i] ? 0.0f : kFar;
            floor[i] = land[i] ? kFar : 0.0f;
        }
        if ( landCount == 0 || waterCount == 0 ) {
            Logging::Inf( "Shore field: no shoreline ({} land, {} water texels)", landCount, waterCount );
            return nullptr;
        }

        DistanceTransform2D( top, g );     // to the nearest land
        DistanceTransform2D( floor, g );   // to the nearest water
        // Signed distance in world units, zero halfway between a land and a water texel
        for ( size_t i = 0; i < g.Count(); ++i ) {
            const float d = land[i] ? -( std::sqrt( floor[i] ) - 0.5f ) : ( std::sqrt( top[i] ) - 0.5f );
            top[i] = std::clamp( d * g.Cell, -kMaxDistance, kMaxDistance );
        }
        Release( land );

        // Seaward direction from the blurred distance; it shortens where two shores meet
        BoxBlur( top, floor, g, kBlurRadius );
        texels.resize( g.Count() * 4 );
        for ( int iz = 0; iz < g.Height; ++iz ) {
            for ( int x = 0; x < g.Width; ++x ) {
                const size_t i = g.Index( x, iz );
                const int xl = std::max( x - 1, 0 ), xr = std::min( x + 1, g.Width - 1 );
                const int zd = std::max( iz - 1, 0 ), zu = std::min( iz + 1, g.Height - 1 );
                const float gx = ( floor[g.Index( xr, iz )] - floor[g.Index( xl, iz )] ) / ( static_cast<float>( xr - xl ) * g.Cell );
                const float gz = ( floor[g.Index( x, zu )] - floor[g.Index( x, zd )] ) / ( static_cast<float>( zu - zd ) * g.Cell );
                texels[i * 4 + 0] = PackedVector::XMConvertFloatToHalf( top[i] );
                texels[i * 4 + 1] = PackedVector::XMConvertFloatToHalf( water[i] );
                texels[i * 4 + 2] = PackedVector::XMConvertFloatToHalf( std::clamp( gx, -1.0f, 1.0f ) );
                texels[i * 4 + 3] = PackedVector::XMConvertFloatToHalf( std::clamp( gz, -1.0f, 1.0f ) );
            }
        }
    } catch ( const std::bad_alloc& ) {
        Logging::Wrn( "Shore field: out of memory for {}x{} texels, shore waves and foam fall back to screen space", g.Width, g.Height );
        return nullptr;
    }

    auto field = std::make_unique<ShoreField>();
    if ( Engine::GraphicsEngine->CreateTexture( field->Texture ) != XR_SUCCESS || !field->Texture
        || field->Texture->Init( INT2( g.Width, g.Height ), GfxTexture::TF_R16G16B16A16_FLOAT, 1, texels.data(), "ShoreField" ) != XR_SUCCESS ) {
        Logging::Wrn( "Shore field: texture creation failed ({}x{})", g.Width, g.Height );
        return nullptr;
    }
    field->Texture->SetDebugName( "ShoreField" );
    field->Mapping = XMFLOAT4( g.MinX, g.MinZ, 1.0f / ( g.Width * g.Cell ), 1.0f / ( g.Height * g.Cell ) );

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - start ).count();
    Logging::Inf( "Shore field: {}x{} texels of {:.0f} units at ({:.0f}, {:.0f}), {} land / {} water, {} ms",
        g.Width, g.Height, g.Cell, g.MinX, g.MinZ, landCount, waterCount, ms );
    return field;
}
