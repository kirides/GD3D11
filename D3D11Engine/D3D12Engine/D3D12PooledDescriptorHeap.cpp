#include "../pch.h"
#include "D3D12PooledDescriptorHeap.h"
#include "../Logger.h"

bool D3D12PooledDescriptorHeap::Init( ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity, const wchar_t* debugName ) {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;   // CPU-only: RTV/DSV heaps are never shader-visible
    if ( FAILED( device->CreateDescriptorHeap( &desc, IID_PPV_ARGS( m_Heap.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    if ( debugName ) m_Heap->SetName( debugName );

    m_DescriptorSize = device->GetDescriptorHandleIncrementSize( type );
    m_Capacity = capacity;
    m_NextFree = 0;
    m_FreeList.clear();
    m_LoggedExhaustion = false;
    return true;
}

UINT D3D12PooledDescriptorHeap::Allocate() {
    if ( !m_FreeList.empty() ) {
        const UINT slot = m_FreeList.back();
        m_FreeList.pop_back();
        return slot;
    }
    if ( m_NextFree < m_Capacity ) return m_NextFree++;

    if ( !m_LoggedExhaustion ) {
        LogWarn() << "D3D12PooledDescriptorHeap: exhausted (" << m_Capacity << " descriptors) — further pooled targets will fail to allocate.";
        m_LoggedExhaustion = true;
    }
    return kInvalidSlot;
}

void D3D12PooledDescriptorHeap::Free( UINT slot ) {
    if ( slot == kInvalidSlot ) return;
    m_FreeList.push_back( slot );
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12PooledDescriptorHeap::GetCpuHandle( UINT slot ) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_Heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>( slot ) * m_DescriptorSize;
    return handle;
}

void D3D12PooledDescriptorHeap::Reset() {
    m_Heap.Reset();
    m_DescriptorSize = 0;
    m_Capacity = 0;
    m_NextFree = 0;
    m_FreeList.clear();
    m_LoggedExhaustion = false;
}
