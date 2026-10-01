#pragma once
#include "../RHI/Rhi.h"
#include <cstdint>
#include <d3d12.h>
#include <mutex>
#include <vector>
#include <wrl/client.h>

class D3D12GraphicsEngine;
struct GpuArenaSlot;

/** One DEFAULT-heap vertex buffer + one R16 index buffer shared by meshes that come and go while a world is
    loaded (skinned NPC bodies, node attachments). A draw addresses its mesh through BaseVertexLocation/
    StartIndexLocation, so an ExecuteIndirect command needs no buffer views — which is what lets Vulkan run it
    as device-generated commands instead of a CPU replay.

    Unlike D3D12VobArena, space is recycled: a released mesh's ranges go back to first-fit free lists once the
    frame that released them has retired on the GPU. The mesh holds its slot index (GpuArenaSlot), so a
    recycled mesh address can't alias a slot.

    Threading: Request and Flush run on the main thread, Flush only while no recorder thread is running (frame
    start, and before the cascade jobs launch). Find is lock-free (only Flush changes what it reads). Forget may
    run on any thread and blocks while a Flush is re-uploading, which keeps the source vectors alive for it. */
class D3D12MeshArena {
public:
    struct Range {
        UINT BaseVertex = 0;
        UINT StartIndex = 0;
        UINT IndexCount = 0;   // 0 = not resident
    };

    D3D12MeshArena( UINT vertexStride, const wchar_t* vbName, const wchar_t* ibName, const char* logName );

    /** Queues a mesh for the next Flush; no-op once it has a slot. The vectors behind `vertices`/`indices`
        must stay unchanged until Forget. Main thread. */
    void Request( GpuArenaSlot& slot, const void* vertices, UINT vertexCount, const uint16_t* indices, UINT indexCount );

    /** Releases the mesh's slot. Call before its vertex/index data is freed. Any thread. */
    void Forget( GpuArenaSlot& slot );

    /** Uploads everything requested since the last call, growing the buffers if needed. Main thread, frame open.
        Returns Ready(). */
    bool Flush( D3D12GraphicsEngine* engine );

    /** nullptr until the mesh's upload has been flushed. */
    const Range* Find( const GpuArenaSlot& slot ) const;

    bool Ready() const { return m_VertexBuffer != nullptr && m_IndexBuffer != nullptr; }
    Rhi::Resource* GetVertexBuffer() const { return m_VertexBuffer.Get(); }
    Rhi::Resource* GetIndexBuffer() const { return m_IndexBuffer.Get(); }
    UINT GetVertexBytes() const { return m_VertexCapacity * m_VertexStride; }
    UINT GetIndexBytes() const { return m_IndexCapacity * kIndexStride; }
    UINT VertexStride() const { return m_VertexStride; }

    D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const;
    D3D12_INDEX_BUFFER_VIEW IndexBufferView() const;

    UINT GetMeshCount() const { return m_ResidentCount; }
    UINT64 GetBytes() const { return static_cast<UINT64>( GetVertexBytes() ) + GetIndexBytes(); }

private:
    static constexpr UINT kIndexStride = 2;   // R16_UINT

    /** First-fit allocator over [0, capacity) with coalescing frees. */
    class FreeList {
    public:
        bool Allocate( UINT count, UINT& outOffset );
        void Free( UINT offset, UINT count );
        void Clear() { m_Blocks.clear(); }
    private:
        struct Block { UINT Offset; UINT Count; };
        std::vector<Block> m_Blocks;   // sorted by offset, never adjacent
    };

    enum class State : uint8_t { Free, Pending, Resident, Released };
    struct Record {
        const void* Vertices = nullptr;
        const uint16_t* Indices = nullptr;
        UINT VertexCount = 0;
        UINT IndexCount = 0;
        UINT BaseVertex = 0;
        UINT StartIndex = 0;
        State Kind = State::Free;
    };
    struct Retired {
        UINT BaseVertex, VertexCount, StartIndex, IndexCount;
    };

    /** Grows `list`/`capacity` until `count` fits, then allocates it. */
    static void AllocateGrowing( FreeList& list, UINT& capacity, UINT count, UINT minSpare, UINT& outOffset );
    /** Flush's work under m_Mutex; true if it recorded uploads. */
    bool FlushLocked( D3D12GraphicsEngine* engine );
    bool Reallocate( D3D12GraphicsEngine* engine );
    bool Upload( D3D12GraphicsEngine* engine, const Record& r );

    const UINT m_VertexStride;
    const wchar_t* m_VbName;
    const wchar_t* m_IbName;
    const char* m_LogName;

    Microsoft::WRL::ComPtr<Rhi::Resource> m_VertexBuffer;
    Microsoft::WRL::ComPtr<Rhi::Resource> m_IndexBuffer;
    UINT m_VertexCapacity = 0;   // in vertices
    UINT m_IndexCapacity = 0;    // in indices
    UINT m_ResidentCount = 0;

    // Read lock-free by Find; resized and written only by Flush.
    std::vector<Range> m_Published;

    std::mutex m_Mutex;   // guards everything below
    std::vector<Record> m_Records;          // indexed by slot
    std::vector<uint32_t> m_FreeSlots;     // reusable slot indices (recycled by Flush only)
    std::vector<uint32_t> m_Pending;
    std::vector<uint32_t> m_Released;
    std::vector<uint32_t> m_Uploads;       // Flush scratch
    FreeList m_FreeVertices;
    FreeList m_FreeIndices;
    bool m_AllocFailed = false;

    // Freed ranges whose frame has retired, handed back by the engine's fence-deferred cleanup.
    std::mutex m_ReclaimMutex;
    std::vector<Retired> m_Reclaimable;
};
