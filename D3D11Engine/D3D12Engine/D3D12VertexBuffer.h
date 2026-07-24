#pragma once
#include "../GfxVertexBuffer.h"
#include <d3d12.h>
#include <wrl/client.h>

#include "D3D12MemAlloc.h"

/** D3D12 vertex / index / dynamic buffer.

    Backed by a persistently-mapped UPLOAD-heap committed resource (created in GENERIC_READ, the only
    legal state for an upload buffer, and kept there for the resource's lifetime). This single class
    serves three consumers:

    - **Dynamic D3D7 buffers** (MyDirect3DVertexBuffer7, U_DYNAMIC + CA_WRITE): Gothic Lock()s -> Map()
      returns the persistent CPU pointer, writes verts, Unlock()s. These are never bound to the GPU:
      DrawVertexBufferFF snapshots the CPU bytes into the per-frame UI ring (see the engine), so there
      is no GPU-in-flight hazard on this resource despite the single persistent mapping.
    - **Static buffers** (initData at Init, world/VOB geometry — Phase 2): data is memcpy'd once at Init
      and the resource can be bound directly via GetGpuVirtualAddress()/GetResource().
    - **Dynamic buffers that ARE directly GPU-bound** (morph-mesh MeshInfo::MeshVertexBuffer — U_DYNAMIC +
      CA_WRITE, but re-`UpdateBuffer`'d every animation frame AND read straight off the IA in the depth
      prepass/color/CSM-caster passes): a single persistently-mapped resource here would be a real CPU-write
      vs GPU-read race — the fence-wait policy only guarantees frame N-2 is complete before frame N's CPU
      work starts (kBackBufferCount frames of slack), so `UpdateBuffer`'s memcpy could tear a vertex the
      GPU is still fetching from an in-flight draw of frame N-1. Fixed by ring-buffering: any buffer created
      with BOTH U_DYNAMIC and initData-less-or-updated-post-Init gets `kBackBufferCount` backing
      allocations; `UpdateBuffer` always writes the CURRENT frame's copy, `GetResource()`/
      `GetGpuVirtualAddress()`/`GetMappedData()` always read the CURRENT frame's copy — so a frame N write
      lands in a slot no in-flight (frame N-1/N-2) GPU command can be reading. `Init`'s initData (if any) is
      replicated into every copy so whichever slot a not-yet-updated first frame binds is valid, not garbage.
      Detected via `IsGpuBoundDynamic()` — currently: has CA_WRITE and NOT a D3D7 buffer (B_VERTEXBUFFER only,
      no structured/index-only distinction needed since B_VERTEXBUFFER covers both VB and IB creation paths
      already used for morph meshes; the D3D7 dynamic-buffer case is CPU-only per the class doc above, so
      ring-buffering it too is a small, harmless memory cost, not a correctness requirement).

    NOTE (perf, not correctness): static geometry currently also lives on the UPLOAD heap (system
    memory) rather than a DEFAULT-heap (VRAM) resource fed by the copy queue. That DEFAULT-heap upload
    path is a Phase-2 follow-up. */
class D3D12VertexBuffer : public GfxVertexBuffer {
public:
    D3D12VertexBuffer() = default;
    ~D3D12VertexBuffer() override;

    XRESULT Init( void* initData, unsigned int sizeInBytes, EBindFlags bindFlags = B_VERTEXBUFFER,
        EUsageFlags usage = U_DEFAULT, ECPUAccessFlags cpuAccess = CA_NONE,
        const std::string& fileName = "", unsigned int structuredByteSize = 0 ) override;

    XRESULT UpdateBuffer( void* data, unsigned int size = 0 ) override;

    XRESULT Map( int flags, void** dataPtr, unsigned int* size ) override;
    XRESULT Unmap() override;

    XRESULT OptimizeVertices( VERTEX_INDEX* indices, uint8_t* vertices, unsigned int numIndices,
        unsigned int numVertices, unsigned int stride, std::vector<VERTEX_INDEX>* outShadowIndices = nullptr ) override;
    XRESULT OptimizeFaces( VERTEX_INDEX* indices, uint8_t* vertices, unsigned int numIndices,
        unsigned int numVertices, unsigned int stride ) override;

    unsigned int GetSizeInBytes() const override { return m_SizeInBytes; }

    /** Native resource + GPU address for direct IA binding. For a ring-buffered dynamic buffer these
        return the CURRENT frame's copy (stable for the whole frame, so multiple passes drawing the same
        mesh in one frame all see the same data). */
    ID3D12Resource* GetResource() const { return Current().Resource.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress() const {
        ID3D12Resource* res = Current().Resource.Get();
        return res ? res->GetGPUVirtualAddress() : 0;
    }

    /** Persistent CPU pointer to the CURRENT frame's copy (upload heap). Valid for the resource's whole
        lifetime; used by DrawVertexBufferFF to snapshot dynamic D3D7 verts into the frame ring. */
    const void* GetMappedData() const { return Current().MappedPtr; }

    /** Backend downcast from the neutral base. Safe by construction: the only concrete
        GfxVertexBuffer implementation is D3D12VertexBuffer while the D3D12 backend is active. */
    static D3D12VertexBuffer* From( GfxVertexBuffer* buffer ) { return static_cast<D3D12VertexBuffer*>( buffer ); }

private:
    static constexpr UINT kMaxCopies = 3;   // == D3D12GraphicsEngine::kBackBufferCount (checked at Init)

    struct Copy {
        Microsoft::WRL::ComPtr<D3D12MA::Allocation> Allocation;
        Microsoft::WRL::ComPtr<ID3D12Resource>      Resource;
        uint8_t* MappedPtr = nullptr;
    };
    Copy m_Copies[kMaxCopies];
    UINT m_NumCopies = 1;      // 1 for static/CPU-only-dynamic buffers, kBackBufferCount for GPU-bound dynamic ones
    unsigned int m_SizeInBytes = 0;

    const Copy& Current() const { return m_Copies[m_NumCopies > 1 ? CurrentFrameSlot() : 0]; }
    Copy& Current() { return m_Copies[m_NumCopies > 1 ? CurrentFrameSlot() : 0]; }
    static UINT CurrentFrameSlot();
};
