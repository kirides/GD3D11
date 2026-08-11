#pragma once
#include "pch.h"

class zCMorphMesh;
class zCMorphMeshAni;
class zCMorphMeshProto;
struct MeshInfo;
struct MeshVisualInfo;

/** GPU morph fold: the compute-shader form of MorphBlend's sequential channel fold.
 *
 *  Takes morph attachments off the per-frame CPU path. Per deforming instance and animation frame it
 *  replaces zCMorphMesh::CalcVertexPositions (or MorphBlend::Apply), the per-wedge ExVertexStruct expansion
 *  in UpdateMorphMeshVisual and a full GfxVertexBuffer::UpdateBuffer with one Dispatch per submesh that
 *  rewrites only the 12-byte Position of each vertex already in that submesh's vertex buffer. Everything
 *  else in the vertex is wedge data written once at conversion, so there is nothing to copy.
 *
 *  That buffer is then a DEFAULT-heap UAV created and destroyed with the mesh: never CPU-mapped (so no
 *  32-bit address space), and ONE copy rather than kBackBufferCount, since the CPU-write-vs-GPU-read race
 *  that forced the ring is gone. That is what made the old release/recreate hysteresis unnecessary.
 *
 *  Data model. Everything the fold reads that is not per-frame is per PROTOTYPE (per .MMS, so per head
 *  TYPE, not per NPC) and immutable, built once on first sight and never freed:
 *      Positions  float3[] : the instance rest bases followed by every ani's vertPosMatrix, concatenated
 *      Indices    uint[]   : per ani a DENSE vertex -> slot table (the gather-friendly inversion of the
 *                            sparse vertIndexList; 0xFFFFFFFF = this ani does not touch that vertex),
 *                            then per submesh a wedge -> mesh-vertex table
 *  ~1 MB total across a G2 world's prototypes (MorphBlend::LogPrototypeBudget), so they are uploaded
 *  eagerly rather than streamed.
 *
 *  Per frame the only upload is one ChannelRecord per active blend channel per instance, with the engine's
 *  double zSinusEase already baked in by MorphBlend::CaptureChannels.
 *
 *  Threading: Register() can be reached from the shadow-cascade / point-shadow collection on the worker
 *  pool, so it takes a lock. It only READS Gothic state. */
namespace MorphGpu {

    /** One active blend channel, resolved to buffer offsets. MUST match MorphFold.hlsl's ChannelRecord. */
    struct ChannelRecord {
        uint32_t FrameA;     // float3 index into Positions of this channel's current frame, slot 0
        uint32_t FrameB;     // ...and of the frame it lerps towards (== FrameA for a single-frame ani)
        uint32_t Inverse;    // uint index into Indices of this ani's dense vertex -> slot table
        float    Frac;       // FrameA -> FrameB lerp; exactly 0 for a single-frame ani
        float    Weight;     // zSinusEase applied twice, as CalcVertPositions does
        float    Weight1M;   // 1 - Weight, or exactly 1 for a shape ani (it replaces, it does not blend)
    };
    static_assert( sizeof( ChannelRecord ) == 24, "MorphFold.hlsl's ChannelRecord mirrors this" );

    /** Immutable per-.MMS tables. Owned by the module for the process lifetime - there are a handful of
        head prototypes per world and they are shared by every NPC wearing one. */
    struct Prototype {
        std::vector<float3>   Positions;
        std::vector<uint32_t> Indices;

        /** Where each ani of this prototype landed. Keyed by the ani pointer because that is what a
            captured channel carries, and the pointers are owned by the shared prototype. */
        struct AniInfo {
            uint32_t FrameBase;    // float3 index of frame 0, slot 0
            uint32_t InverseBase;  // uint index of the dense vertex -> slot table
            int      NumVert;      // slots per frame
            int      NumFrames;
        };
        gtl::flat_hash_map<zCMorphMeshAni*, AniInfo> Anis;

        /** Per submesh: where its wedge -> mesh-vertex table starts, and how long it is. Indexed by
            zCProgMeshProto submesh index, which is what MeshInfo::MeshIndex holds. */
        std::vector<uint32_t> WedgeBase;
        std::vector<uint32_t> WedgeCount;

        uint32_t RestBase = 0;      // float3 index of morphRefMeshVertPos (the no-refShape rest base)
        int      MeshNumVert = 0;
        bool     Valid = false;     // false => this prototype can never be folded on the GPU
    };

    /** One submesh to fold this frame. The backend turns each of these into a single Dispatch. */
    struct Job {
        MeshInfo*        Mesh;            // fold target: its vertex buffer is the UAV
        const Prototype* Proto;
        uint32_t         RestBase;        // this INSTANCE's rest base (refShapeAni's shape, or Proto->RestBase)
        uint32_t         WedgeBase;
        uint32_t         OutVertexCount;
        uint32_t         ChannelFirst;    // first ChannelRecord of this instance
        uint32_t         ChannelCount;
    };

    /** Whether folding on the GPU is the active path. Frozen on first call (see the .cpp): the answer
        decides how morph vertex buffers are CREATED, so it may not change while any exist. */
    bool IsActive();

    /** Called once by a backend that has a working fold pipeline, before any world is converted.
        Without this IsActive() is false and the CPU deform stays the path. */
    void SetBackendAvailable( bool available );

    /** Snapshots this instance's channels and queues one Job per submesh. Returns false if this
        prototype/instance cannot be folded on the GPU, in which case the caller must fall back to the
        CPU deform for it (the reason is logged once per prototype). Thread-safe. */
    bool Register( zCMorphMesh* mm, MeshVisualInfo* mvi );

    /** Hands the queued work to the backend's dispatch and empties the queue, in one locked step.
     *
     *  A MOVE rather than a peek: Register() can run on the worker pool, so a borrowed reference could be
     *  reallocated under the dispatch mid-walk, and Job::ChannelFirst indexes the channel array, so the two
     *  containers may only ever be emptied together.
     *
     *  A Job registered AFTER this returns lands in the now-empty queue and folds at the next frame's
     *  dispatch — the ghost-VOB pass collects its attachments that late, so those instances run one frame
     *  behind on facial animation.
     *
     *  Reuse the same two vectors every frame; they come back with their capacity intact. */
    void TakeJobs( std::vector<Job>& outJobs, std::vector<ChannelRecord>& outChannels );

    /** Total bytes of prototype tables built so far, for the diagnostic log. */
    size_t ResidentTableBytes();
}
