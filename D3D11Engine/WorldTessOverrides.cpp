#include "pch.h"
#include "WorldTessOverrides.h"
#include "WorldObjects.h"
#include "zCMaterial.h"
#include "zCTexture.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <unordered_map>

using namespace DirectX;

namespace {
    constexpr const char* kWtsHeader = "GD3D11-WTS 1";

    std::string LowerKey( std::string_view s ) {
        std::string r( s );
        std::transform( r.begin(), r.end(), r.begin(), []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        return r;
    }

    std::string MatchKey( int sx, int sy, std::string_view key ) {
        return std::to_string( sx ) + ' ' + std::to_string( sy ) + ' ' + LowerKey( key );
    }

    std::string_view Trim( std::string_view s ) {
        while ( !s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r') ) s.remove_prefix( 1 );
        while ( !s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r') ) s.remove_suffix( 1 );
        return s;
    }

    /** Splits off the last whitespace-delimited token. */
    std::string_view PopToken( std::string_view& s ) {
        s = Trim( s );
        const size_t p = s.find_last_of( " \t" );
        const std::string_view tok = p == std::string_view::npos ? s : s.substr( p + 1 );
        s = p == std::string_view::npos ? std::string_view{} : s.substr( 0, p );
        return tok;
    }

    std::string_view PopFrontToken( std::string_view& s ) {
        s = Trim( s );
        const size_t p = s.find_first_of( " \t" );
        const std::string_view tok = p == std::string_view::npos ? s : s.substr( 0, p );
        s = p == std::string_view::npos ? std::string_view{} : s.substr( p + 1 );
        return tok;
    }

    template<typename T>
    bool ParseNum( std::string_view s, T& out ) {
        return !s.empty() && std::from_chars( s.data(), s.data() + s.size(), out ).ec == std::errc{};
    }
}

WorldTessOverrides& GetWorldTessOverrides() {
    static WorldTessOverrides s_Instance;
    return s_Instance;
}

void WorldTessOverrides::Build( std::vector<WorldTessMeshRef>&& meshes, const std::vector<uint32_t>& boundaryOffsets, const std::vector<uint32_t>& boundaryMeshes ) {
    m_Meshes = std::move( meshes );
    m_Params.assign( m_Meshes.size(), WorldTessParams{} );
    m_BoundaryOffsets = boundaryOffsets;
    m_BoundaryMeshes16.clear();
    m_BoundaryMeshes32.clear();
    if ( m_Meshes.size() <= 0xFFFF ) {
        m_BoundaryMeshes16.assign( boundaryMeshes.begin(), boundaryMeshes.end() );
    } else {
        m_BoundaryMeshes32 = boundaryMeshes;
    }
    Touch();
    m_Dirty = false;

    const size_t bytes = m_Meshes.capacity() * sizeof( WorldTessMeshRef ) + m_Params.capacity() * sizeof( WorldTessParams )
        + m_BoundaryOffsets.capacity() * 4 + m_BoundaryMeshes16.capacity() * 2 + m_BoundaryMeshes32.capacity() * 4;
    Logging::Inf( "World tessellation overrides: {} meshes, {} boundary entries, {} KB retained", m_Meshes.size(), GetBoundaryCount(), bytes / 1024 );
}

void WorldTessOverrides::Reset() {
    m_Meshes = {};
    m_Params = {};
    m_BoundaryOffsets = {};
    m_BoundaryMeshes16 = {};
    m_BoundaryMeshes32 = {};
    m_UnknownLines = {};
    m_Table = {};
    Touch();
    m_Dirty = false;
}

WorldTessParams WorldTessOverrides::Get( uint32_t mesh ) const {
    return mesh < m_Params.size() ? m_Params[mesh] : WorldTessParams{};
}

void WorldTessOverrides::Set( uint32_t mesh, const WorldTessParams& p ) {
    if ( mesh >= m_Params.size() ) return;
    WorldTessParams c = p;
    c.Roundness = std::clamp( c.Roundness, 0.0f, 1.0f );
    c.Factor = std::clamp( c.Factor, 0.0f, 2.0f );
    m_Params[mesh] = c;
    Touch();
}

void WorldTessOverrides::SetForTexture( zCTexture* texture, const WorldTessParams& p ) {
    if ( !texture ) return;
    for ( uint32_t i = 0; i < m_Meshes.size(); ++i ) {
        const WorldTessMeshRef& m = m_Meshes[i];
        zCTexture* tex = m.Texture ? m.Texture : (m.Material ? m.Material->GetTextureSingle() : nullptr);
        if ( tex == texture ) Set( i, p );
    }
}

void WorldTessOverrides::ResetMesh( uint32_t mesh ) {
    Set( mesh, WorldTessParams{} );
}

bool WorldTessOverrides::GetMeshInfo( uint32_t mesh, int& sectionX, int& sectionY, std::string& key ) const {
    if ( mesh >= m_Meshes.size() ) return false;
    const WorldTessMeshRef& m = m_Meshes[mesh];
    sectionX = m.SectionX;
    sectionY = m.SectionY;
    zCTexture* tex = m.Texture ? m.Texture : (m.Material ? m.Material->GetTextureSingle() : nullptr);
    if ( tex ) {
        key = std::string( tex->GetNameWithoutExtView() );
    } else if ( m.Material ) {
        key = std::string( m.Material->GetNameView() );
    } else {
        key.clear();
    }
    return !key.empty();
}

const std::vector<XMFLOAT4>& WorldTessOverrides::GetGpuTable() {
    if ( m_TableVersion == m_Version ) return m_Table;
    m_TableVersion = m_Version;

    m_Table.clear();
    if ( m_Meshes.empty() ) return m_Table;

    const size_t meshCount = m_Meshes.size();
    const size_t boundaries = GetBoundaryCount();
    m_Table.resize( meshCount + boundaries );
    for ( size_t i = 0; i < meshCount; ++i ) {
        const WorldTessParams& p = m_Params[i];
        m_Table[i] = XMFLOAT4( p.Roundness, p.Factor, p.Reserved[0], p.Reserved[1] );
    }
    for ( size_t b = 0; b < boundaries; ++b ) {
        XMFLOAT4 r( FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX );
        for ( uint32_t k = m_BoundaryOffsets[b]; k < m_BoundaryOffsets[b + 1]; ++k ) {
            const uint32_t id = m_BoundaryMeshes16.empty() ? m_BoundaryMeshes32[k] : m_BoundaryMeshes16[k];
            const XMFLOAT4& v = m_Table[id];
            r.x = std::min( r.x, v.x ); r.y = std::min( r.y, v.y ); r.z = std::min( r.z, v.z ); r.w = std::min( r.w, v.w );
        }
        if ( m_BoundaryOffsets[b] == m_BoundaryOffsets[b + 1] ) r = XMFLOAT4( 1, 1, 0, 0 );
        m_Table[meshCount + b] = r;
    }
    return m_Table;
}

bool WorldTessOverrides::Load( const std::string& file ) {
    // Edits made since the last load/save are replaced by the file, like the other .spt/.veg resources.
    std::fill( m_Params.begin(), m_Params.end(), WorldTessParams{} );
    m_UnknownLines.clear();
    Touch();
    m_Dirty = false;

    std::ifstream f( file );
    if ( !f ) return false;

    std::unordered_multimap<std::string, uint32_t> byKey;
    byKey.reserve( m_Meshes.size() );
    for ( uint32_t i = 0; i < m_Meshes.size(); ++i ) {
        int sx, sy; std::string key;
        if ( GetMeshInfo( i, sx, sy, key ) ) byKey.emplace( MatchKey( sx, sy, key ), i );
    }

    size_t applied = 0;
    std::string line;
    bool first = true;
    while ( std::getline( f, line ) ) {
        std::string_view s = Trim( line );
        if ( s.empty() ) continue;
        if ( first ) {
            first = false;
            if ( s.substr( 0, 10 ) == "GD3D11-WTS" ) continue;
        }

        // "sx sy key roundness factor"; the key may contain spaces, so peel numbers off both ends.
        std::string_view rest = s;
        const std::string_view factorTok = PopToken( rest );
        const std::string_view roundTok = PopToken( rest );
        const std::string_view sxTok = PopFrontToken( rest );
        const std::string_view syTok = PopFrontToken( rest );
        int sx = 0, sy = 0;
        WorldTessParams p;
        const std::string_view key = Trim( rest );
        if ( key.empty() || !ParseNum( sxTok, sx ) || !ParseNum( syTok, sy ) || !ParseNum( roundTok, p.Roundness ) || !ParseNum( factorTok, p.Factor ) ) {
            m_UnknownLines.emplace_back( s );
            continue;
        }
        p.Roundness = std::clamp( p.Roundness, 0.0f, 1.0f );
        p.Factor = std::clamp( p.Factor, 0.0f, 2.0f );

        const auto range = byKey.equal_range( MatchKey( sx, sy, key ) );
        if ( range.first == range.second ) {
            m_UnknownLines.emplace_back( s );
            continue;
        }
        for ( auto it = range.first; it != range.second; ++it ) m_Params[it->second] = p;
        ++applied;
    }

    Logging::Inf( "World tessellation overrides: {} entries applied, {} kept unresolved", applied, m_UnknownLines.size() );
    return true;
}

bool WorldTessOverrides::Save( const std::string& file ) {
    bool any = !m_UnknownLines.empty();
    for ( const WorldTessParams& p : m_Params ) any = any || !p.IsDefault();

    if ( !any ) {
        // Nothing to store: drop a stale file rather than leave old overrides behind.
        std::error_code ec;
        std::filesystem::remove( file, ec );
        m_Dirty = false;
        return true;
    }

    std::ofstream f( file, std::ios::trunc );
    if ( !f ) return false;

    f << kWtsHeader << '\n';
    for ( uint32_t i = 0; i < m_Meshes.size(); ++i ) {
        if ( m_Params[i].IsDefault() ) continue;
        int sx, sy; std::string key;
        if ( !GetMeshInfo( i, sx, sy, key ) ) continue;
        f << sx << ' ' << sy << ' ' << key << ' ' << m_Params[i].Roundness << ' ' << m_Params[i].Factor << '\n';
    }
    for ( const std::string& l : m_UnknownLines ) f << l << '\n';

    m_Dirty = false;
    return static_cast<bool>( f );
}
