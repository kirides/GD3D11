#pragma once
#include "../RHI/Rhi.h"
#include <wrl/client.h>
#include <mutex>

class VulkanDevice;
struct VkCommandBuffer_T;
struct VkImageCreateInfo;

/** The Vulkan implementation of the RHI: translates the D3D12-shaped calls (VULKAN_IMPLEMENTATION_PLAN.md 5). */
namespace VulkanRhi {
    /** Creates instance, device, queues and the memory allocator; null (logged) on failure. */
    Microsoft::WRL::ComPtr<Rhi::Device> CreateDevice();

    // Escape hatches for Vulkan-only code (imgui_impl_vulkan). Only valid on objects of this backend.
    VulkanDevice& NativeDevice( Rhi::Device* device );
    /** VkFormat (as int) of a DXGI format. */
    int VkFormatOf( DXGI_FORMAT format );
    /** Opens the list's rendering scope on its current targets and returns the command buffer for raw recording. */
    VkCommandBuffer_T* BeginNativeRendering( Rhi::CommandList* list );
    /** After raw recording: forget the pipeline, descriptor and dynamic state the list believed bound. */
    void EndNativeRendering( Rhi::CommandList* list );
    /** Closes the list's rendering scope and returns the command buffer for raw compute (FidelityFX). */
    VkCommandBuffer_T* BeginNativeCompute( Rhi::CommandList* list );
    /** Moves every subresource of an image to `layout` (a VkImageLayout) and tracks it, for raw recorders
        that expect a layout no D3D12 state maps to. */
    void SetImageLayout( Rhi::CommandList* list, Rhi::Resource* image, int layout );
    /** The VkImage of a texture (0 for buffers) and a create info describing it (pNext left null). */
    uint64_t NativeImage( Rhi::Resource* resource, VkImageCreateInfo* outInfo );
    /** Queue lock for code that submits on its own (imgui_impl_vulkan's texture uploads). */
    std::mutex& QueueMutex( Rhi::Device* device );
    /** The image behind a shader-visible SRV, its VkImageView and the VkImageLayout to sample it in (imgui_impl_vulkan). */
    bool SampledImageOf( Rhi::Device* device, D3D12_GPU_DESCRIPTOR_HANDLE srv, Microsoft::WRL::ComPtr<Rhi::Resource>& outResource,
        uint64_t& outView, int& outLayout );
}
