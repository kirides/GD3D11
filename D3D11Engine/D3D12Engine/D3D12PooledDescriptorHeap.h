#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

// Small free-list allocator over a single fixed-capacity, CPU-only (non-shader-visible) descriptor
// heap. Used by D3D12TexturePool for its dedicated RTV heap.
//
// Deliberately fixed-capacity rather than growable: per CLAUDE.md's per-frame-allocation rule, a
// generous cap sized once up front is simpler and cheaper than reallocating/CopyDescriptorsSimple-ing a
// live heap, and pooled render targets are a few dozen at most even in the heaviest post-fx chain.
// Exhaustion is logged once (not per call) and returns UINT_MAX rather than crashing or silently
// dropping the caller's request without a trace.
class D3D12PooledDescriptorHeap {
public:
    static constexpr UINT kInvalidSlot = 0xFFFFFFFFu;

    bool Init( ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity, const wchar_t* debugName );

    UINT Allocate();
    void Free( UINT slot );
    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle( UINT slot ) const;

    void Reset();   // drops the heap; pool Clear() calls this on resize/level change

private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_Heap;
    UINT m_DescriptorSize = 0;
    UINT m_Capacity = 0;
    UINT m_NextFree = 0;
    std::vector<UINT> m_FreeList;
    bool m_LoggedExhaustion = false;
};
