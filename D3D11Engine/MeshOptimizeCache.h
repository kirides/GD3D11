#pragma once

/** Memoization for the CPU cost of GfxVertexBuffer::OptimizeVertices (vertex-cache/fetch reorder,
    shadow-index welding, LOD simplification) - see WorldConverter.cpp's OptimizeMeshBuffers, the sole
    caller. Two tiers: an in-memory map for this session, and an on-disk cache under
    system\GD3D11\meshoptcache\ so the FIRST load of a session benefits too, on the second and later
    runs of the game.

    Gothic reloads the SAME world and VOB geometry many times, both within one session (leaving/
    re-entering a world, reloading a save, walking back into a camp) and across separate game launches,
    and meshopt's output is a pure function of the input vertex/index bytes plus which optional outputs
    were asked for. A hit skips the vertex-cache optimization, the shadow weld and the min-error
    simplification pass entirely, which is where load-time actually goes.
    RendererSettings.EnableMeshOptimization/EnableShadowIndexBuffers exist to let a player skip that work
    in the FIRST place; this is what stops any OTHER load - this session or a future one - from paying
    for it again.

    Keyed on a content hash of the PRE-optimization (vertices, indices), not on any Gothic-side pointer -
    zCProgMeshProto/zCSubMesh objects do not reliably survive a world change (ZenGin's own resource
    manager purges and reloads them independently of GD3D11), but the bytes a given source asset produces
    do not change. Self-validating: two different source meshes colliding on the same hash would have to
    produce identical bytes, in which case optimizing them identically is correct anyway.

    In-memory tier bounded by the number of DISTINCT sub-meshes the session has ever converted - same
    growth shape as SharedVisualRegistry. Acceptable because an entry holds only small CPU-side index/
    vertex arrays, never a GPU buffer, so it does not touch the 32-bit VA budget the pooling rules in
    CLAUDE.md are about. The on-disk tier is one small file per distinct sub-mesh ever seen, same shape as
    the D3D11/D3D12 shader caches (ShaderCacheHash.h) it's modeled on - deleting the meshoptcache
    directory is always safe, it just repays the CPU cost on the next load. kMeshCacheFormatVersion
    exists for the same reason kDxbcCacheArgsRevision does in D3D11ShaderManager.cpp: bump it whenever a
    change to MeshShadowIndexBuilder.h, MeshLodBuilder.h, this record layout, or the vendored
    meshoptimizer version would make an old file's bytes stop matching what a fresh run produces - old
    files are simply orphaned, never misread (the version is part of the validity check). */

#include "VertexTypes.h"
#include "Logger.h"
#include "ShaderCacheHash.h"
#include "Engine.h"
#include "GothicAPI.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace MeshOptimizeCache {

    /** `wantShadow`/`wantLod` are folded into the key so a session that turns on
        EnableShadowIndexBuffers partway through - or that mixes D3D11/D3D12 results, the only thing that
        varies LOD - never gets served a hit that is missing an output it now needs. */
    inline uint64_t Hash( const std::vector<VERTEX_INDEX>& indices, const std::vector<ExVertexStruct>& vertices,
        bool wantShadow, bool wantLod ) {
        uint64_t h = ShaderCacheHash::HashBytes( indices.data(), indices.size() * sizeof( VERTEX_INDEX ) );
        h = ShaderCacheHash::HashBytes( vertices.data(), vertices.size() * sizeof( ExVertexStruct ), h );
        const uint8_t flags = static_cast<uint8_t>( wantShadow ) | ( static_cast<uint8_t>( wantLod ) << 1 );
        return ShaderCacheHash::HashBytes( &flags, sizeof( flags ), h );
    }

    struct Entry {
        std::vector<ExVertexStruct> Vertices;
        std::vector<VERTEX_INDEX> Indices;
        std::vector<VERTEX_INDEX> ShadowIndices;
        std::vector<VERTEX_INDEX> LodIndices;
    };

    // Worker-pool concurrent: BuildWorldMeshBuffers optimizes many sub-meshes at once (see
    // WorldConverter.cpp's batched jobs), and VOB visual extraction runs on the same pool.
    inline std::mutex g_Mutex;
    inline gtl::flat_hash_map<uint64_t, Entry> g_Cache;
    inline std::atomic<uint32_t> g_MemHits{ 0 }, g_DiskHits{ 0 }, g_Misses{ 0 };

    inline void ReportStats() {
        const uint32_t total = g_MemHits + g_DiskHits + g_Misses;
        // Same cadence rule as MeshLod::ReportStats / MeshShadow::ReportStats: first lookup, then every
        // 500, so silence is never ambiguous between "nothing ran" and "fewer than N lookups happened".
        if ( total != 1 && ( total == 0 || total % 500 != 0 ) ) return;
        LogInfo() << "Mesh optimize cache: " << g_MemHits.load() << " mem-hit, " << g_DiskHits.load()
            << " disk-hit / " << total << " lookups (" << g_Cache.size() << " resident, "
            << g_Misses.load() << " computed)";
    }

    namespace Disk {
        // Bump on any change that would make a previously-written file's bytes stop matching what a
        // fresh OptimizeFaces/OptimizeVertices run produces today - see the header comment.
        constexpr uint32_t kFormatVersion = 1;
        constexpr uint32_t kMaxElements = 8u << 20;   // sanity cap against a corrupted/truncated file

        inline std::string CacheFilePath( uint64_t key, bool createDir ) {
            const std::string dir = Engine::GAPI->GetStartDirectory() + "\\" + ENGINE_BASE_DIR + "meshoptcache\\";
            if ( createDir ) {
                const std::string parent = Engine::GAPI->GetStartDirectory() + "\\" + ENGINE_BASE_DIR;
                CreateDirectoryA( parent.c_str(), nullptr );
                if ( !CreateDirectoryA( dir.c_str(), nullptr ) && GetLastError() != ERROR_ALREADY_EXISTS ) {
                    return "";
                }
            }
            char name[32] = {};
            sprintf_s( name, "%016llx.meshopt", static_cast<unsigned long long>( key ) );
            return dir + name;
        }

        template<typename T> bool ReadPod( std::ifstream& in, T& out ) {
            return static_cast<bool>( in.read( reinterpret_cast<char*>( &out ), sizeof( T ) ) );
        }
        template<typename T> void WritePod( std::ofstream& out, const T& value ) {
            out.write( reinterpret_cast<const char*>( &value ), sizeof( T ) );
        }
        template<typename T> bool ReadVector( std::ifstream& in, std::vector<T>& out, uint32_t count ) {
            if ( count > kMaxElements ) return false;
            out.resize( count );
            return count == 0 || static_cast<bool>( in.read( reinterpret_cast<char*>( out.data() ),
                static_cast<std::streamsize>( count ) * sizeof( T ) ) );
        }
        template<typename T> void WriteVector( std::ofstream& out, const std::vector<T>& v ) {
            if ( !v.empty() ) {
                out.write( reinterpret_cast<const char*>( v.data() ),
                    static_cast<std::streamsize>( v.size() * sizeof( T ) ) );
            }
        }

        inline bool TryLoad( uint64_t key, Entry& out ) {
            const std::string path = CacheFilePath( key, false );
            if ( path.empty() ) return false;
            std::ifstream in( path, std::ios::binary );
            if ( !in ) return false;

            char magic[4] = {};
            uint32_t version = 0;
            uint64_t storedKey = 0;
            uint32_t vertexCount = 0, indexCount = 0, shadowCount = 0, lodCount = 0;
            if ( !in.read( magic, 4 ) || memcmp( magic, "GMOC", 4 ) != 0 ) return false;
            if ( !ReadPod( in, version ) || version != kFormatVersion ) return false;
            if ( !ReadPod( in, storedKey ) || storedKey != key ) return false;
            if ( !ReadPod( in, vertexCount ) || !ReadPod( in, indexCount ) ||
                !ReadPod( in, shadowCount ) || !ReadPod( in, lodCount ) ) return false;

            return ReadVector( in, out.Vertices, vertexCount ) && ReadVector( in, out.Indices, indexCount ) &&
                ReadVector( in, out.ShadowIndices, shadowCount ) && ReadVector( in, out.LodIndices, lodCount );
        }

        // Writes to a temp file and renames, so a crash (or another worker thread writing the same,
        // byte-identical, content) mid-write can't leave a torn entry.
        inline void Store( uint64_t key, const Entry& e ) {
            if ( e.Indices.empty() || e.Vertices.empty() ) return;
            const std::string path = CacheFilePath( key, true );
            if ( path.empty() ) return;
            const std::string tmp = path + ".tmp";

            {
                std::ofstream out( tmp, std::ios::binary | std::ios::trunc );
                if ( !out ) return;
                out.write( "GMOC", 4 );
                WritePod( out, kFormatVersion );
                WritePod( out, key );
                WritePod( out, static_cast<uint32_t>( e.Vertices.size() ) );
                WritePod( out, static_cast<uint32_t>( e.Indices.size() ) );
                WritePod( out, static_cast<uint32_t>( e.ShadowIndices.size() ) );
                WritePod( out, static_cast<uint32_t>( e.LodIndices.size() ) );
                WriteVector( out, e.Vertices );
                WriteVector( out, e.Indices );
                WriteVector( out, e.ShadowIndices );
                WriteVector( out, e.LodIndices );
                if ( !out ) { out.close(); DeleteFileA( tmp.c_str() ); return; }
            }
            if ( !MoveFileExA( tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING ) ) {
                DeleteFileA( tmp.c_str() );
            }
        }
    }   // namespace Disk

    /** On a hit (memory, then disk), overwrites indices/vertices/shadowIndices/lodIndices with the
        cached, already-optimized result and returns true - the caller skips OptimizeFaces/
        OptimizeVertices entirely. A disk hit is folded into the in-memory map so the rest of this
        session skips the file read too. */
    inline bool TryGet( uint64_t key, std::vector<VERTEX_INDEX>& indices, std::vector<ExVertexStruct>& vertices,
        std::vector<VERTEX_INDEX>* shadowIndices, std::vector<VERTEX_INDEX>* lodIndices ) {
        {
            std::scoped_lock lock( g_Mutex );
            auto it = g_Cache.find( key );
            if ( it != g_Cache.end() ) {
                indices = it->second.Indices;
                vertices = it->second.Vertices;
                if ( shadowIndices ) *shadowIndices = it->second.ShadowIndices;
                if ( lodIndices ) *lodIndices = it->second.LodIndices;
                ++g_MemHits;
                ReportStats();
                return true;
            }
        }

        Entry disk;
        if ( Disk::TryLoad( key, disk ) ) {
            indices = disk.Indices;
            vertices = disk.Vertices;
            if ( shadowIndices ) *shadowIndices = disk.ShadowIndices;
            if ( lodIndices ) *lodIndices = disk.LodIndices;
            {
                std::scoped_lock lock( g_Mutex );
                g_Cache.emplace( key, std::move( disk ) );
            }
            ++g_DiskHits;
            ReportStats();
            return true;
        }

        ++g_Misses;
        ReportStats();
        return false;
    }

    inline void Put( uint64_t key, const std::vector<VERTEX_INDEX>& indices, const std::vector<ExVertexStruct>& vertices,
        const std::vector<VERTEX_INDEX>* shadowIndices, const std::vector<VERTEX_INDEX>* lodIndices ) {
        Entry e;
        e.Indices = indices;
        e.Vertices = vertices;
        if ( shadowIndices ) {
            e.ShadowIndices = *shadowIndices;
        }
        if ( lodIndices ) {
            e.LodIndices = *lodIndices;
        }

        Disk::Store( key, e );

        std::scoped_lock lock( g_Mutex );
        g_Cache.emplace( key, std::move( e ) );
    }

}   // namespace MeshOptimizeCache
