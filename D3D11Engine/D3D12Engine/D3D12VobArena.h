#pragma once
#include <d3d12.h>
#include <mutex>
#include <vector>
#include <wrl/client.h>

#include "D3D12MemAlloc.h"

struct MeshInfo;
struct MeshVisualInfo;
class D3D12GraphicsEngine;

/** One DEFAULT-heap vertex buffer + one DEFAULT-heap index buffer holding EVERY static VOB sub-mesh in the
    world ("mega buffers").

    Why this exists: the instanced-VOB ExecuteIndirect command signature used to carry two
    VERTEX_BUFFER_VIEWs and an INDEX_BUFFER_VIEW per command, because every VOB visual owned its own
    buffers. That makes each of the frame's ~560 commands a full IA state change, and it was measurably the
    dominant cost of the four VOB passes (depth prepass, three shadow cascades, lit pass) — the same failure
    the world mesh had before its sections were consolidated into one wrapped buffer, and the same fix.

    With every sub-mesh living in one pair of buffers, a sub-mesh is just
    `{ BaseVertexLocation, StartIndexLocation, IndexCount }`, the signature collapses to
    `{ b6 material consts, b4 wind consts, DrawIndexed }`, and each pass binds the IA exactly once.

    16-bit indices survive consolidation untouched. `BaseVertexLocation` is a 32-bit value added AFTER the
    index fetch, so each sub-mesh's `R16_UINT` indices stay sub-mesh-relative (0..numVertices-1) exactly as
    they were authored — the index data is concatenated verbatim, with no reindexing and no widening to
    32 bit. The same holds for the position-welded SHADOW indices and the baked progressive-mesh LOD
    indices, which index the very same vertices and therefore the very same base.

    Ownership/threading contract:
    - `Reset()` on world load, `QueueVisual()` from OnAddVob (which fires per vob during world load, so the
      whole world's geometry is registered before the first frame renders).
    - Queueing only records a pointer. All GPU work — allocation, growth, uploads — happens in `Flush()`,
      which the engine calls ONCE per frame on the main thread from UploadFrameVobInstances, i.e. before
      anything reads the arena that frame and before the shadow cascades' concurrent command builds start.
      That is what lets `Find()` be a lock-free read from those worker threads.
    - Growth reallocates and re-uploads every resident sub-mesh from its (retained) CPU-side
      `MeshInfo::Vertices/Indices`, at the SAME offsets — so ranges handed out earlier stay valid and no
      caller has to be invalidated. The old resources are handed to the engine's fence-deferred cleanup.

    A second full CPU copy of the world's VOB geometry is deliberately never materialised (32-bit address
    space): uploads are fed sub-mesh by sub-mesh straight out of `MeshInfo`'s own vectors through the
    engine's pooled/batched copy-queue staging. */
class D3D12VobArena {
public:
    /** Where one sub-mesh lives in the arena. Counts of 0 mean "this level does not exist for this
        sub-mesh" — the caller falls through to the next-best index range, exactly as it did when these
        were separate buffers. */
    struct Range {
        UINT BaseVertex   = 0;   // first vertex, in vertices (goes into DrawIndexed::BaseVertexLocation)
        UINT IndexStart   = 0;   // full render indices, in indices (-> StartIndexLocation)
        UINT IndexCount   = 0;
        UINT ShadowStart  = 0;   // position-welded shadow indices
        UINT ShadowCount  = 0;
        UINT LodStart     = 0;   // baked progressive-mesh LOD indices
        UINT LodCount     = 0;
    };

    /** Drops every range and both buffers. Called from OnLoadWorld, before the new world's vobs arrive. */
    void Reset();

    /** Register every drawable sub-mesh of a static VOB visual. Cheap and idempotent: already-resident and
        already-pending sub-meshes are skipped, so calling it once per vob (i.e. many times per visual) is
        fine. Safe to call from any thread. */
    void QueueVisual( MeshVisualInfo* visual );

    /** Upload everything queued since the last call, growing the buffers if needed. Main thread, inside an
        open frame. Returns false if the arena is unusable this frame (allocation failure) — the caller
        should then simply not build VOB commands, since nothing would draw. */
    bool Flush( D3D12GraphicsEngine* engine );

    /** Look up a sub-mesh's range. nullptr = not resident (never queued, or its upload failed); the
        command build skips such a sub-mesh for the frame. Lock-free: only Flush() mutates the map, and it
        runs before any reader. */
    const Range* Find( const MeshInfo* mesh ) const {
        auto it = m_Ranges.find( mesh );
        return it == m_Ranges.end() ? nullptr : &it->second;
    }

    /** Drops `mesh` from every list the arena tracks it in - called from D3D12GraphicsEngine::
        OnMeshInfoDestroyed right before the MeshInfo (and its Vertices/Indices) are freed. A MeshInfo can be
        deleted independently of Reset() (e.g. a shared MeshVisualInfo's last VOB reference dropping via
        SharedVisualRegistry::Release() while the world stays loaded), and without this the next Reallocate()
        walks m_Resident and re-uploads from freed memory - a use-after-free that surfaces as a garbage
        `sizeInBytes` in UploadBufferData. Leaves the mesh's reserved vertex/index range as dead space in the
        buffers (no compaction) rather than reflow every later range; cheap and safe, and the next full world
        reload (Reset()) reclaims it anyway. Safe to call from any thread - takes the same lock QueueVisual
        does and only otherwise touches state that only Flush() (main thread) mutates, exactly like Find(). */
    void Forget( const MeshInfo* mesh ) {
        {
            std::lock_guard<std::mutex> lock( m_PendingMutex );
            std::erase_if( m_Pending, [mesh]( const PendingMesh& p ) { return p.Mesh == mesh; } );
        }
        m_Ranges.erase( mesh );
        std::erase( m_Resident, mesh );
        std::erase( m_Dynamic, mesh );
    }

    /** Sub-meshes whose vertices are rewritten every frame (animated static VOBs built from an .MMS —
        either ZENGIN's CPU deform or the GPU morph fold). Their arena range holds the CONVERSION pose
        after upload and has to be refreshed from the mesh's own vertex buffer once per frame; the engine
        does that right after DispatchMorphFold. Empty in the overwhelmingly common case. */
    const std::vector<MeshInfo*>& DynamicMeshes() const { return m_Dynamic; }

    bool Ready() const { return m_VertexBuffer != nullptr && m_IndexBuffer != nullptr && m_VertexCursor > 0; }
    ID3D12Resource* GetVertexBuffer() const { return m_VertexBuffer.Get(); }
    ID3D12Resource* GetIndexBuffer() const { return m_IndexBuffer.Get(); }
    UINT GetVertexBytes() const { return m_VertexCapacity * kVertexStride; }
    UINT GetIndexBytes() const { return m_IndexCapacity * kIndexStride; }
    static constexpr UINT VertexStride() { return kVertexStride; }

    /** Stats for the ImGui panel — resident sub-meshes and how much VRAM the two buffers cost. */
    UINT GetMeshCount() const { return static_cast<UINT>( m_Ranges.size() ); }
    UINT64 GetBytes() const { return static_cast<UINT64>( GetVertexBytes() ) + GetIndexBytes(); }

private:
    static const UINT kVertexStride;      // sizeof(ExVertexStruct) — out of line, WorldObjects.h isn't pulled in here
    static constexpr UINT kIndexStride = 2;   // R16_UINT

    /** Assigns `mesh` a range at the current cursors and records it. Does no GPU work. */
    bool Reserve( MeshInfo* mesh, bool dynamic );
    /** (Re)creates both buffers at m_VertexCapacity/m_IndexCapacity and re-uploads every resident
        sub-mesh. Old resources go to the engine's fence-deferred cleanup. */
    bool Reallocate( D3D12GraphicsEngine* engine );
    bool UploadMesh( D3D12GraphicsEngine* engine, MeshInfo* mesh, const Range& range );

    Microsoft::WRL::ComPtr<ID3D12Resource>      m_VertexBuffer;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VertexAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_IndexBuffer;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_IndexAlloc;

    UINT m_VertexCapacity = 0;   // vertices the buffer can hold
    UINT m_IndexCapacity = 0;    // indices the buffer can hold
    UINT m_VertexCursor = 0;     // vertices handed out
    UINT m_IndexCursor = 0;      // indices handed out

    gtl::flat_hash_map<const MeshInfo*, Range> m_Ranges;
    std::vector<MeshInfo*> m_Resident;     // insertion order — re-upload order on growth
    std::vector<MeshInfo*> m_Dynamic;      // subset of m_Resident, see DynamicMeshes()

    // Queued-but-not-yet-uploaded. `Dynamic` rides along from QueueVisual because only the VISUAL knows it
    // (MeshVisualInfo::MorphMeshVisual); the MeshInfo itself looks static.
    struct PendingMesh {
        MeshInfo* Mesh;
        bool Dynamic;
    };
    std::mutex m_PendingMutex;             // guards m_Pending only; the rest is main-thread/Flush-only
    std::vector<PendingMesh> m_Pending;

    bool m_AllocFailed = false;            // logged once; stops us retrying a doomed allocation every frame
};
