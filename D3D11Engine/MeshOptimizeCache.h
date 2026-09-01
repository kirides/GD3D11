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

    In-memory tier is only kept ARMED for the duration of one world load (GothicAPI::OnLoadWorld ->
    OnWorldLoaded calls SetMemoryCache(true)/(false) below) - unbounded growth across an entire play
    session, not one load, is what was actually exhausting the 32-bit address space, since Gothic
    reloads worlds/VOBs repeatedly over hours of play. Disarming clears the map immediately so the
    memory is freed, not just stopped from growing; every lookup after that falls through to the
    on-disk tier at a small IO cost instead of being memoized in RAM. While armed, an entry holds only
    small CPU-side index/vertex arrays, never a GPU buffer, so it does not touch the 32-bit VA budget
    the pooling rules in CLAUDE.md are about. The on-disk tier is one row per distinct sub-mesh ever seen
    in a single SQLite
    database (cache\meshes.db, via SqliteBlobStore) - tens of thousands of mostly-tiny entries is exactly
    the shape a single DB beats a directory of loose files on (fewer file-system objects for the AV
    scanner/NTFS to chew through during a load burst). Deleting cache\meshes.db is always safe, it just
    repays the CPU cost on the next load. kFormatVersion below exists for the same reason
    kDxbcCacheArgsRevision does in D3D11ShaderManager.cpp: bump it whenever a change to
    MeshShadowIndexBuilder.h, MeshLodBuilder.h, this record layout, or the vendored meshoptimizer version
    would make a previously-stored row's bytes stop matching what a fresh run produces today - old rows
    are simply orphaned (dead weight, harmless), never misread, since the version is part of the record
    and checked before anything else is trusted. */

#include "VertexTypes.h"
#include "Logger.h"
#include "ShaderCacheHash.h"
#include "SqliteBlobStore.h"
#include "ByteCursor.h"
#include "Engine.h"
#include "GothicAPI.h"

#include <atomic>
#include <cstdint>
#include <cstring>
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
    inline std::atomic<bool> g_MemoryCacheEnabled{ true };

    /** Arms/disarms the in-memory tier - see the header comment for why. Call with true right before a
        world load's mesh conversion work starts and false once it's done; disarming drops any entries
        already resident so the memory is actually released. Safe to call from the main thread while
        worker-pool lookups are in flight (TryGet/Put) - both paths take g_Mutex around the memory
        tier and read g_MemoryCacheEnabled fresh on every call, so a race just means a lookup right at
        the boundary happens to land on one side or the other of it, never a torn read. */
    inline void SetMemoryCache( bool on ) {
        g_MemoryCacheEnabled = on;
        if ( !on ) {
            std::scoped_lock lock( g_Mutex );
            g_Cache.clear();
        }
    }

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
        // Bump on any change that would make a previously-stored row's bytes stop matching what a fresh
        // OptimizeFaces/OptimizeVertices run produces today - see the header comment.
        constexpr uint32_t kFormatVersion = 1;
        constexpr uint32_t kMaxElements = 8u << 20;   // sanity cap against a corrupted/truncated record

        inline SqliteBlobStore& GetStore() {
            // Constructed on first use (magic-static, thread-safe) - by the time any mesh conversion
            // runs, GetStartDirectory() is already valid.
            static SqliteBlobStore s_store( Engine::GAPI->GetStartDirectory() + R"(\system\GD3D11\cache\meshes.db)" );
            return s_store;
        }

        template<typename T> void AppendPod( std::vector<uint8_t>& buf, const T& value ) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>( &value );
            buf.insert( buf.end(), p, p + sizeof( T ) );
        }
        template<typename T> void AppendVector( std::vector<uint8_t>& buf, const std::vector<T>& v ) {
            if ( !v.empty() ) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>( v.data() );
                buf.insert( buf.end(), p, p + v.size() * sizeof( T ) );
            }
        }

        // Cursor over the blob TryGet hands back - the on-disk record layout is unchanged from the old
        // per-file cache, just read from memory now instead of an ifstream.
        struct Cursor {
            const uint8_t* p;
            const uint8_t* end;
            template<typename T> bool ReadPod( T& out ) {
                if ( static_cast<size_t>( end - p ) < sizeof( T ) ) return false;
                memcpy( &out, p, sizeof( T ) );
                p += sizeof( T );
                return true;
            }
            template<typename T> bool ReadVector( std::vector<T>& out, uint32_t count ) {
                if ( count > kMaxElements ) return false;
                const size_t bytes = static_cast<size_t>( count ) * sizeof( T );
                if ( static_cast<size_t>( end - p ) < bytes ) return false;
                out.resize( count );
                if ( bytes ) memcpy( out.data(), p, bytes );
                p += bytes;
                return true;
            }
        };

        inline bool TryLoad( uint64_t key, Entry& out ) {
            std::vector<uint8_t> blob;
            if ( !GetStore().TryGet( key, blob ) || blob.size() < 4 ) return false;
            if ( memcmp( blob.data(), "GMOC", 4 ) != 0 ) return false;

            Cursor c{ blob.data() + 4, blob.data() + blob.size() };
            uint32_t version = 0;
            uint64_t storedKey = 0;
            uint32_t vertexCount = 0, indexCount = 0, shadowCount = 0, lodCount = 0;
            if ( !c.ReadPod( version ) || version != kFormatVersion ) return false;
            if ( !c.ReadPod( storedKey ) || storedKey != key ) return false;
            if ( !c.ReadPod( vertexCount ) || !c.ReadPod( indexCount ) ||
                !c.ReadPod( shadowCount ) || !c.ReadPod( lodCount ) ) return false;

            return c.ReadVector( out.Vertices, vertexCount ) && c.ReadVector( out.Indices, indexCount ) &&
                c.ReadVector( out.ShadowIndices, shadowCount ) && c.ReadVector( out.LodIndices, lodCount );
        }

        inline void Store( uint64_t key, const Entry& e ) {
            if ( e.Indices.empty() || e.Vertices.empty() ) return;

            std::vector<uint8_t> blob;
            blob.reserve( 32 + e.Vertices.size() * sizeof( ExVertexStruct ) +
                ( e.Indices.size() + e.ShadowIndices.size() + e.LodIndices.size() ) * sizeof( VERTEX_INDEX ) );
            blob.insert( blob.end(), reinterpret_cast<const uint8_t*>( "GMOC" ), reinterpret_cast<const uint8_t*>( "GMOC" ) + 4 );
            AppendPod( blob, kFormatVersion );
            AppendPod( blob, key );
            AppendPod( blob, static_cast<uint32_t>( e.Vertices.size() ) );
            AppendPod( blob, static_cast<uint32_t>( e.Indices.size() ) );
            AppendPod( blob, static_cast<uint32_t>( e.ShadowIndices.size() ) );
            AppendPod( blob, static_cast<uint32_t>( e.LodIndices.size() ) );
            AppendVector( blob, e.Vertices );
            AppendVector( blob, e.Indices );
            AppendVector( blob, e.ShadowIndices );
            AppendVector( blob, e.LodIndices );

            GetStore().Put( key, blob.data(), blob.size() );
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
            if ( g_MemoryCacheEnabled ) {
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

        if ( g_MemoryCacheEnabled ) {
            std::scoped_lock lock( g_Mutex );
            g_Cache.emplace( key, std::move( e ) );
        }
    }

}   // namespace MeshOptimizeCache
