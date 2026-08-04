#include "pch.h"
#include "MorphGpu.h"
#include "MorphBlend.h"
#include "WorldObjects.h"
#include "GothicAPI.h"
#include "Engine.h"
#include "zCMorphMesh.h"

namespace MorphGpu {

    namespace {
        /** Guards the prototype cache and the frame queue. Register() is reachable from the shadow-cascade
            and point-shadow collection, which run on the worker pool. Contention is negligible - a handful
            of instances per frame, and the expensive half (building a prototype's tables) happens once per
            .MMS for the whole session. */
        std::mutex s_Mutex;

        gtl::flat_hash_map<zCMorphMeshProto*, std::unique_ptr<Prototype>> s_Prototypes;
        /** Prototypes displaced by a recycled zCMorphMeshProto address (see BuildPrototype). Kept alive
            rather than freed so a Prototype* is stable for the session: the backends cache their uploaded
            GPU tables under that pointer, and a reused address would silently alias them. Only ever grows on
            a world change that recycles an address, i.e. essentially never. */
        std::vector<std::unique_ptr<Prototype>> s_Retired;
        size_t s_TableBytes = 0;

        std::vector<Job> s_Jobs;
        std::vector<ChannelRecord> s_Channels;
        std::vector<MorphBlend::ChannelState> s_Capture;   // scratch, reused (holds its capacity)

        std::atomic<bool> s_BackendAvailable{ false };

        constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

        /** Builds the immutable tables for one .MMS. Reads only arrays ZENGIN writes once at load
            (morphRefMeshVertPos, every ani's vertPosMatrix/vertIndexList, the progmesh wedge lists), so it
            is safe from a worker thread - unlike the live position list, which CalcVertPositions deforms.
            Caller holds s_Mutex. */
        Prototype* BuildPrototype( zCMorphMesh* mm ) {
            zCMorphMeshProto* protoObj = mm->GetMorphProto();
            if ( !protoObj ) {
                return nullptr;
            }

            zCProgMeshProto* ref = protoObj->GetMorphRefMesh();
            zCArrayAdapt<float3>* posList = ref ? ref->GetPositionList() : nullptr;
            const int meshNumVert = posList ? posList->NumInArray : 0;

            auto it = s_Prototypes.find( protoObj );
            if ( it != s_Prototypes.end() ) {
                // Nothing frees these, but ZENGIN's resource manager can drop a .MMS prototype on a world
                // change and hand the same ADDRESS back for a different one. Cheap identity re-check rather
                // than trusting the pointer: a recycled entry would fold heads against another head's tables.
                Prototype* cached = it->second.get();
                if ( !cached->Valid || cached->MeshNumVert == meshNumVert ) {
                    return cached;
                }
                s_TableBytes -= cached->Positions.size() * sizeof( float3 )
                    + cached->Indices.size() * sizeof( uint32_t );
                s_Retired.push_back( std::move( it->second ) );
                s_Prototypes.erase( it );
            }

            auto owned = std::make_unique<Prototype>();
            Prototype& p = *owned;
            const char* name = protoObj->GetName() && protoObj->GetName()->ToChar()
                ? protoObj->GetName()->ToChar() : "?";

            const float3* refPos = protoObj->GetMorphRefMeshVertPos();
            if ( !ref || !refPos || meshNumVert <= 0 ) {
                LogWarn() << "MorphGpu: " << name << " has no usable rest mesh - folding it on the CPU.";
                Prototype* raw = owned.get();
                s_Prototypes.emplace( protoObj, std::move( owned ) );
                return raw;   // Valid stays false
            }
            p.MeshNumVert = meshNumVert;

            // --- Positions: the rest base first, then every ani's vertPosMatrix ---
            // A refShape ani's own block doubles as a rest base (CalcVertPositions adds
            // refShapeAni->vertPosMatrix[i] instead of morphRefMeshVertPos[i]), which is why Register can
            // just point RestBase at it - see zCMorphMesh::GetRestPositions.
            const int numAnis = protoObj->GetNumAnis();
            size_t positionCount = static_cast<size_t>( meshNumVert );
            size_t indexCount = 0;
            for ( int i = 0; i < numAnis; i++ ) {
                zCMorphMeshAni* ani = protoObj->GetAni( i );
                if ( !ani ) continue;
                const int aniNumVert = ani->GetNumVert();
                const int numFrames = ani->GetNumFrames();
                if ( aniNumVert <= 0 || numFrames <= 0 || !ani->GetVertPosMatrix() || !ani->GetVertIndexList() ) continue;
                positionCount += static_cast<size_t>( numFrames ) * aniNumVert;
                indexCount += static_cast<size_t>( meshNumVert );
            }
            p.Positions.reserve( positionCount );
            p.Indices.reserve( indexCount );

            p.RestBase = 0;
            p.Positions.insert( p.Positions.end(), refPos, refPos + meshNumVert );

            for ( int i = 0; i < numAnis; i++ ) {
                zCMorphMeshAni* ani = protoObj->GetAni( i );
                if ( !ani ) continue;
                const int aniNumVert = ani->GetNumVert();
                const int numFrames = ani->GetNumFrames();
                const float3* vertPos = ani->GetVertPosMatrix();
                const int* vertIndexList = ani->GetVertIndexList();
                if ( aniNumVert <= 0 || numFrames <= 0 || !vertPos || !vertIndexList ) continue;

                Prototype::AniInfo info{};
                info.NumVert = aniNumVert;
                info.NumFrames = numFrames;
                info.FrameBase = static_cast<uint32_t>( p.Positions.size() );
                p.Positions.insert( p.Positions.end(), vertPos,
                    vertPos + static_cast<size_t>( numFrames ) * aniNumVert );

                // Dense inversion of the sparse vertIndexList. The CPU fold walks slots and scatters; a
                // thread-per-vertex shader has to gather, so it needs the mapping the other way round.
                info.InverseBase = static_cast<uint32_t>( p.Indices.size() );
                p.Indices.resize( p.Indices.size() + meshNumVert, kNoSlot );
                uint32_t* inverse = p.Indices.data() + info.InverseBase;
                bool duplicate = false;
                for ( int j = 0; j < aniNumVert; j++ ) {
                    const int v = vertIndexList[j];
                    if ( v < 0 || v >= meshNumVert ) continue;   // an ani wider than the mesh, as MorphBlend::Apply skips
                    if ( inverse[v] != kNoSlot ) {
                        duplicate = true;
                        break;
                    }
                    inverse[v] = static_cast<uint32_t>( j );
                }
                if ( duplicate ) {
                    // Two slots of one ani touching the same vertex means the CPU fold applies that channel
                    // TWICE to it, which a single-slot gather cannot reproduce. Not observed in shipped
                    // content; refuse the whole prototype rather than render it subtly differently.
                    LogWarn() << "MorphGpu: " << name << " ani #" << i
                        << " has a duplicated vertIndexList entry - folding this prototype on the CPU.";
                    Prototype* raw = owned.get();
                    s_Prototypes.emplace( protoObj, std::move( owned ) );
                    return raw;   // Valid stays false
                }
                p.Anis.emplace( ani, info );
            }

            // --- Per-submesh wedge -> mesh-vertex tables ---
            // The fold writes one vertex per WEDGE, in wedge order, because that is exactly the order
            // Extract3DSMeshFromVisual2 built the vertex buffer in (and morph submeshes deliberately skip
            // the optimizers, so it still holds). Several wedges share a position; each just gathers it.
            const int numSubmeshes = ref->GetNumSubmeshes();
            p.WedgeBase.resize( numSubmeshes, 0 );
            p.WedgeCount.resize( numSubmeshes, 0 );
            for ( int i = 0; i < numSubmeshes; i++ ) {
                zCSubMesh* s = ref->GetSubmesh( i );
                const int numWedges = s ? s->WedgeList.NumInArray : 0;
                p.WedgeBase[i] = static_cast<uint32_t>( p.Indices.size() );
                p.WedgeCount[i] = static_cast<uint32_t>( numWedges );
                for ( int v = 0; v < numWedges; v++ ) {
                    p.Indices.push_back( static_cast<uint32_t>( s->WedgeList.Array[v].position ) );
                }
            }

            p.Valid = !p.Positions.empty() && !p.Indices.empty();
            const size_t bytes = p.Positions.size() * sizeof( float3 ) + p.Indices.size() * sizeof( uint32_t );
            s_TableBytes += bytes;
            LogInfo() << "MorphGpu: " << name << " tables built - " << meshNumVert << " verts, "
                << p.Anis.size() << " anis, " << numSubmeshes << " submeshes, " << ( bytes / 1024 )
                << " KB (" << ( s_TableBytes / 1024 ) << " KB over " << ( s_Prototypes.size() + 1 ) << " prototypes)";

            Prototype* raw = owned.get();
            s_Prototypes.emplace( protoObj, std::move( owned ) );
            return raw;
        }
    }

    void SetBackendAvailable( bool available ) {
        s_BackendAvailable.store( available, std::memory_order_release );
    }

    bool IsActive() {
        // Frozen on first query. WorldConverter picks a morph submesh's buffer USAGE from this (DEFAULT+UAV
        // for the fold vs DYNAMIC+CA_WRITE for the CPU deform), so an answer that changed mid-session would
        // leave already-converted heads with a buffer the active path cannot write.
        // A function-local static, because Extract3DSMeshFromVisual2 (and therefore this) runs on the worker
        // pool - ExtractNodeVisualAsync - so the freeze has to be race-free, not just once.
        static const bool active = []() {
            const bool a = s_BackendAvailable.load( std::memory_order_acquire )
                && Engine::GAPI->GetRendererState().RendererSettings.UseGpuMorphFold;
            LogInfo() << "MorphGpu: GPU morph fold " << ( a ? "ACTIVE" : "off" ) << " (backend "
                << ( s_BackendAvailable.load( std::memory_order_relaxed ) ? "supports" : "does not support" ) << " it)";
            return a;
        }();
        return active;
    }

    bool Register( zCMorphMesh* mm, MeshVisualInfo* mvi ) {
        if ( !mm || !mvi ) {
            return false;
        }
        zCProgMeshProto* morphMesh = mm->GetMorphMesh();
        if ( !morphMesh ) {
            return false;
        }

        std::scoped_lock lock( s_Mutex );

        Prototype* proto = BuildPrototype( mm );
        if ( !proto || !proto->Valid ) {
            return false;
        }

        // This instance's rest base. A refShape ani replaces morphRefMeshVertPos as the base the deltas are
        // added to, and it must therefore cover every mesh vertex - anything narrower cannot be indexed by
        // mesh vertex and falls back to the CPU (which reads the same array through GetRestPositions).
        uint32_t restBase = proto->RestBase;
        if ( zCMorphMeshAni* shape = mm->GetRefShapeAni() ) {
            auto it = proto->Anis.find( shape );
            if ( it == proto->Anis.end() || it->second.NumVert < proto->MeshNumVert ) {
                return false;
            }
            restBase = it->second.FrameBase;
        }

        // Resolve the captured channels to buffer offsets. An ani that is not in the prototype's table (it
        // was rejected above, or the channel points at something the prototype does not own) makes the whole
        // instance fall back: dropping a single channel would silently change the fold, since every later
        // channel attenuates what the earlier ones left behind.
        MorphBlend::CaptureChannels( mm, s_Capture );
        const size_t channelFirst = s_Channels.size();
        for ( const MorphBlend::ChannelState& c : s_Capture ) {
            auto it = proto->Anis.find( c.Ani );
            if ( it == proto->Anis.end() ) {
                s_Channels.resize( channelFirst );
                return false;
            }
            const Prototype::AniInfo& ani = it->second;

            // CaptureChannels resolved the frames as POINTERS into the ani's own vertPosMatrix; recover the
            // frame indices from them so the shader can index the concatenated pool. Deriving them here
            // rather than re-reading the channel keeps this consistent with whatever CaptureChannels decided
            // (notably its single-frame branch, which pins both frames to frame 0).
            const float3* aniBase = c.Ani->GetVertPosMatrix();
            if ( !aniBase || !c.FrameA || !c.FrameB ) {
                s_Channels.resize( channelFirst );
                return false;
            }
            const ptrdiff_t frameA = ( c.FrameA - aniBase ) / ( c.NumVert > 0 ? c.NumVert : 1 );
            const ptrdiff_t frameB = ( c.FrameB - aniBase ) / ( c.NumVert > 0 ? c.NumVert : 1 );
            if ( frameA < 0 || frameB < 0 || frameA >= ani.NumFrames || frameB >= ani.NumFrames
                || c.NumVert != ani.NumVert ) {
                s_Channels.resize( channelFirst );
                return false;
            }

            ChannelRecord& rec = s_Channels.emplace_back();
            rec.FrameA = ani.FrameBase + static_cast<uint32_t>( frameA ) * static_cast<uint32_t>( ani.NumVert );
            rec.FrameB = ani.FrameBase + static_cast<uint32_t>( frameB ) * static_cast<uint32_t>( ani.NumVert );
            rec.Inverse = ani.InverseBase;
            rec.Frac = c.Frac;
            rec.Weight = c.Weight;
            rec.Weight1M = c.Weight1M;
        }
        const uint32_t channelCount = static_cast<uint32_t>( s_Channels.size() - channelFirst );

        // One job per submesh. A submesh whose converted vertex count disagrees with its wedge count is not
        // the mesh these tables describe (nothing in the current conversion path can produce that, but the
        // fold would write the wrong vertices if it ever did) - fall back for the whole instance.
        const size_t jobFirst = s_Jobs.size();
        for ( auto& [material, meshes] : mvi->Meshes ) {
            for ( auto& mi : meshes ) {
                if ( !mi || mi->Vertices.empty() ) continue;
                const unsigned int sub = mi->MeshIndex;
                if ( sub >= proto->WedgeCount.size()
                    || proto->WedgeCount[sub] != mi->Vertices.size() ) {
                    s_Jobs.resize( jobFirst );
                    s_Channels.resize( channelFirst );
                    return false;
                }
                Job& job = s_Jobs.emplace_back();
                job.Mesh = mi.get();
                job.Proto = proto;
                job.RestBase = restBase;
                job.WedgeBase = proto->WedgeBase[sub];
                job.OutVertexCount = proto->WedgeCount[sub];
                job.ChannelFirst = static_cast<uint32_t>( channelFirst );
                job.ChannelCount = channelCount;
            }
        }

        if ( s_Jobs.size() == jobFirst ) {
            s_Channels.resize( channelFirst );   // nothing to fold (all submeshes empty) - not a failure
        }
        return true;
    }

    void TakeJobs( std::vector<Job>& outJobs, std::vector<ChannelRecord>& outChannels ) {
        std::scoped_lock lock( s_Mutex );
        outJobs.clear();
        outChannels.clear();
        s_Jobs.swap( outJobs );
        s_Channels.swap( outChannels );
    }

    size_t ResidentTableBytes() { return s_TableBytes; }
}
