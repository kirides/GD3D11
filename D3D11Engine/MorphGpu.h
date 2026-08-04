#pragma once
#include "pch.h"

class zCMorphMesh;
class zCMorphMeshAni;
class zCMorphMeshProto;
struct MeshInfo;
struct MeshVisualInfo;

/** GPU morph fold: the compute-shader form of MorphBlend's sequential channel fold.
 *
 *  This is the module that took morph attachments off the per-frame CPU path. It replaces, per
 *  deforming instance and per animation frame:
 *      zCMorphMesh::CalcVertexPositions (or MorphBlend::Apply)   -- the fold itself
 *      the per-wedge ExVertexStruct expansion in UpdateMorphMeshVisual
 *      GfxVertexBuffer::UpdateBuffer                             -- a full vertex-buffer re-upload
 *  with one Dispatch per submesh that rewrites only the 12-byte Position of each vertex already in
 *  that submesh's vertex buffer. Everything else in the vertex (normal, tangent, UVs, color) is
 *  wedge data written once at conversion and never touched again, so there is nothing to copy.
 *
 *  Why that also killed the buffer-lifetime machinery: a folded submesh's vertex buffer is a
 *  DEFAULT-heap (VRAM) UAV, created once with the mesh and destroyed with it. It is never CPU-mapped,
 *  so it costs no 32-bit address space, and on D3D12 it is ONE copy rather than kBackBufferCount
 *  persistently-mapped UPLOAD copies (the CPU-write-vs-GPU-read race that forced the ring is gone -
 *  the writer is the GPU, ordered by a barrier). That is what made the release/recreate hysteresis
 *  (MeshVisualInfo::ReleaseIdleMorphVertexBuffers and MeshInfo's self-healing accessor) unnecessary.
 *
 *  Data model. Everything the fold reads that is not per-frame is per PROTOTYPE (per .MMS, so per head
 *  TYPE, not per NPC) and immutable, built once on first sight and never freed:
 *      Positions  float3[] : the instance rest bases followed by every ani's vertPosMatrix, concatenated
 *      Indices    uint[]   : per ani a DENSE vertex -> slot table (the gather-friendly inversion of the
 *                            sparse vertIndexList; 0xFFFFFFFF = this ani does not touch that vertex),
 *                            then per submesh a wedge -> mesh-vertex table
 *  Measured at ~1 MB of Positions plus ~110 KB of Indices across all 26 prototypes of a G2 world
 *  (MorphBlend::LogPrototypeBudget), which is why they are uploaded eagerly rather than streamed.
 *
 *  Per frame the only upload is one ChannelRecord per active blend channel per instance - the fold
 *  state MorphBlend::CaptureChannels already snapshots, with the engine's double zSinusEase baked in,
 *  so the shader never evaluates the easing (or ZENGIN's quantised zSinApprox) at all.
 *
 *  Threading: Register() can be reached from the shadow-cascade / point-shadow collection running on
 *  the worker pool, so it takes a lock. It only READS Gothic state (the prototype arrays are written
 *  once at load; the channel list is the same read UpdateMorphMeshVisual already did on that thread). */
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
     *  A MOVE rather than a peek, for two reasons. Register() can run on the worker pool, so a borrowed
     *  reference could be reallocated under the dispatch mid-walk; and Job::ChannelFirst indexes the channel
     *  array, so the two containers may only ever be emptied together - clearing the channels while a Job
     *  survived would repoint it at some other instance's channels.
     *
     *  A Job registered AFTER this returns is therefore not lost and not misindexed: it lands in the now-empty
     *  queue and folds at the NEXT frame's dispatch. The ghost-VOB pass collects its attachments that late (and
     *  a shadow-only instance can too), so those instances run one frame behind on facial animation.
     *
     *  The caller should reuse the same two vectors every frame; they come back with their capacity intact, so
     *  this settles into zero per-frame allocations. */
    void TakeJobs( std::vector<Job>& outJobs, std::vector<ChannelRecord>& outChannels );

    /** Total bytes of prototype tables built so far, for the diagnostic log. */
    size_t ResidentTableBytes();
}
