#pragma once

/** Memoizes GfxVertexBuffer::OptimizeVertices (vertex-cache reorder, shadow-index weld, LOD
    simplification) - see WorldConverter.cpp's OptimizeMeshBuffers, the sole caller. Two tiers: an
    in-memory map and an on-disk SQLite DB (cache\meshes.db), each independently gated by the
    caller-supplied GothicRendererSettings::MeshOptimizeCacheFlags mask.

    Keyed on a content hash of the pre-optimization vertices/indices, not a Gothic-side pointer -
    zCProgMeshProto/zCSubMesh don't reliably survive a world change, but the bytes a given source
    asset produces don't.

    The memory tier is only armed for the span of one world load (SetMemoryCache, called from
    GothicAPI::OnLoadWorld/OnWorldLoaded) even when its flag is set - unbounded growth across a whole
    play session, not one load, is what exhausts the 32-bit address space. Disarming clears the map so
    the memory is actually freed.

    kFormatVersion guards the on-disk record layout, same idea as kDxbcCacheArgsRevision in
    D3D11ShaderManager.cpp - bump it whenever MeshShadowIndexBuilder.h/MeshLodBuilder.h/meshoptimizer
    change enough to invalidate old rows; stale rows are just orphaned, never misread. */

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

    // wantShadow/wantLod fold into the key so toggling those settings never serves a hit missing an
    // output it now needs.
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

    // Worker-pool concurrent: mesh conversion batches jobs across threads.
    inline std::mutex g_Mutex;
    inline gtl::flat_hash_map<uint64_t, Entry> g_Cache;
    inline std::atomic<uint32_t> g_MemHits{ 0 }, g_DiskHits{ 0 }, g_Misses{ 0 };
    inline std::atomic<bool> g_MemoryArmed{ true };

    // Arms/disarms the memory tier for the span of one world load - see the header comment. Disarming
    // clears the map so the memory is actually released, not just stopped from growing.
    inline void SetMemoryCache( bool on ) {
        g_MemoryArmed = on;
        if ( !on ) {
            std::scoped_lock lock( g_Mutex );
            g_Cache.clear();
        }
    }

    inline bool MemoryTierActive( int flags ) {
        return ( flags & GothicRendererSettings::MOC_MEMORY ) != 0 && g_MemoryArmed.load();
    }
    inline bool DiskTierActive( int flags ) {
        return ( flags & GothicRendererSettings::MOC_DISK ) != 0;
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
        // Bump on any change that invalidates previously-stored rows - see the header comment.
        constexpr uint32_t kFormatVersion = 1;
        constexpr uint32_t kMaxElements = 8u << 20;   // sanity cap against a corrupted/truncated record

        inline SqliteBlobStore& GetStore() {
            // Magic-static: constructed on first use, once GetStartDirectory() is valid.
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

        // Cursor over the blob SqliteBlobStore::TryGet hands back.
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

    // On a hit (memory, then disk), overwrites indices/vertices/shadowIndices/lodIndices with the cached
    // result and returns true. `flags` is RendererSettings.MeshOptimizeCacheFlags.
    inline bool TryGet( uint64_t key, std::vector<VERTEX_INDEX>& indices, std::vector<ExVertexStruct>& vertices,
        std::vector<VERTEX_INDEX>* shadowIndices, std::vector<VERTEX_INDEX>* lodIndices, int flags ) {
        if ( MemoryTierActive( flags ) ) {
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

        if ( DiskTierActive( flags ) ) {
            Entry disk;
            if ( Disk::TryLoad( key, disk ) ) {
                indices = disk.Indices;
                vertices = disk.Vertices;
                if ( shadowIndices ) *shadowIndices = disk.ShadowIndices;
                if ( lodIndices ) *lodIndices = disk.LodIndices;
                if ( MemoryTierActive( flags ) ) {
                    std::scoped_lock lock( g_Mutex );
                    g_Cache.emplace( key, std::move( disk ) );
                }
                ++g_DiskHits;
                ReportStats();
                return true;
            }
        }

        ++g_Misses;
        ReportStats();
        return false;
    }

    inline void Put( uint64_t key, const std::vector<VERTEX_INDEX>& indices, const std::vector<ExVertexStruct>& vertices,
        const std::vector<VERTEX_INDEX>* shadowIndices, const std::vector<VERTEX_INDEX>* lodIndices, int flags ) {
        if ( !MemoryTierActive( flags ) && !DiskTierActive( flags ) ) {
            return;
        }

        Entry e;
        e.Indices = indices;
        e.Vertices = vertices;
        if ( shadowIndices ) {
            e.ShadowIndices = *shadowIndices;
        }
        if ( lodIndices ) {
            e.LodIndices = *lodIndices;
        }

        if ( DiskTierActive( flags ) ) {
            Disk::Store( key, e );
        }

        if ( MemoryTierActive( flags ) ) {
            std::scoped_lock lock( g_Mutex );
            g_Cache.emplace( key, std::move( e ) );
        }
    }

}   // namespace MeshOptimizeCache
