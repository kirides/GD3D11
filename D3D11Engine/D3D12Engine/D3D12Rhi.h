#pragma once
#include "../RHI/Rhi.h"
#include <D3D12MemAlloc.h>
#include <wrl/client.h>

class D3D12Device;

/** The D3D12 implementation of the RHI: thin forwarders over the native objects, so the renderer behaves
    exactly as it did when it called D3D12 directly. */
namespace D3D12Rhi {

    /** Wraps the engine's device + allocator (non-owning: both outlive every Rhi object the engine holds). */
    Microsoft::WRL::ComPtr<Rhi::Device> CreateDevice( D3D12Device& device, D3D12MA::Allocator* allocator );

    /** Adopts a native resource created outside the RHI (swapchain buffers, FFX interop). */
    Microsoft::WRL::ComPtr<Rhi::Resource> WrapResource( ID3D12Resource* resource, D3D12MA::Allocation* allocation = nullptr );

    // Escape hatches for the D3D12-only code paths (FSR3, DRED, imgui_impl_dx12, Tracy, PIX markers). Only
    // valid on objects of the D3D12 backend.
    ID3D12Resource*            Native( Rhi::Resource* resource );
    ID3D12DescriptorHeap*      Native( Rhi::DescriptorHeap* heap );
    ID3D12RootSignature*       Native( Rhi::RootSignature* rootSig );
    ID3D12PipelineState*       Native( Rhi::PipelineState* pso );
    ID3D12CommandSignature*    Native( Rhi::CommandSignature* sig );
    ID3D12Heap*                Native( Rhi::Heap* heap );
    ID3D12Fence*               Native( Rhi::Fence* fence );
    ID3D12CommandAllocator*    Native( Rhi::CommandAllocator* allocator );
    ID3D12GraphicsCommandList* Native( Rhi::CommandList* list );
    ID3D12CommandQueue*        Native( Rhi::CommandQueue* queue );
    ID3D12Device*              NativeDevice( Rhi::Device* device );
    D3D12MA::Allocator*        NativeAllocator( Rhi::Device* device );
}
