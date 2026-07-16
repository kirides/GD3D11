#pragma once
#include "../GfxVertexBuffer.h"
#include <d3d12.h>
#include <wrl/client.h>

/** D3D12 vertex / index / dynamic buffer.

    Backed by a persistently-mapped UPLOAD-heap committed resource (created in GENERIC_READ, the only
    legal state for an upload buffer, and kept there for the resource's lifetime). This single class
    serves two consumers:

    - **Dynamic D3D7 buffers** (MyDirect3DVertexBuffer7, U_DYNAMIC + CA_WRITE): Gothic Lock()s -> Map()
      returns the persistent CPU pointer, writes verts, Unlock()s. These are never bound to the GPU:
      DrawVertexBufferFF snapshots the CPU bytes into the per-frame UI ring (see the engine), so there
      is no GPU-in-flight hazard on this resource despite the single persistent mapping.
    - **Static buffers** (initData at Init, world/VOB geometry — Phase 2): data is memcpy'd once at Init
      and the resource can be bound directly via GetGpuVirtualAddress()/GetResource().

    NOTE (perf, not correctness): static geometry currently also lives on the UPLOAD heap (system
    memory) rather than a DEFAULT-heap (VRAM) resource fed by the copy queue. That DEFAULT-heap upload
    path is a Phase-2 follow-up; for now UPLOAD keeps the code boring and is fine for the small dynamic
    D3D7 buffers that are the only live consumer. */
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

    /** Native resource + GPU address for direct IA binding (static geometry / Phase 2). */
    ID3D12Resource* GetResource() const { return m_Resource.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress() const {
        return m_Resource ? m_Resource->GetGPUVirtualAddress() : 0;
    }

    /** Persistent CPU pointer to the buffer contents (upload heap). Valid for the resource's whole
        lifetime; used by DrawVertexBufferFF to snapshot dynamic D3D7 verts into the frame ring. */
    const void* GetMappedData() const { return m_MappedPtr; }

    /** Backend downcast from the neutral base. Safe by construction: the only concrete
        GfxVertexBuffer implementation is D3D12VertexBuffer while the D3D12 backend is active. */
    static D3D12VertexBuffer* From( GfxVertexBuffer* buffer ) { return static_cast<D3D12VertexBuffer*>( buffer ); }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Resource;
    uint8_t*     m_MappedPtr = nullptr;   // persistent map (upload heap)
    unsigned int m_SizeInBytes = 0;
};
