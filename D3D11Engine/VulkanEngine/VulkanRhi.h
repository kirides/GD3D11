#pragma once
#include "../RHI/Rhi.h"
#include <wrl/client.h>
#include <mutex>

class VulkanDevice;
struct VkCommandBuffer_T;

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
    /** Queue lock for code that submits on its own (imgui_impl_vulkan's texture uploads). */
    std::mutex& QueueMutex( Rhi::Device* device );
    /** The image behind a shader-visible SRV, its VkImageView and the VkImageLayout to sample it in (imgui_impl_vulkan). */
    bool SampledImageOf( Rhi::Device* device, D3D12_GPU_DESCRIPTOR_HANDLE srv, Microsoft::WRL::ComPtr<Rhi::Resource>& outResource,
        uint64_t& outView, int& outLayout );
}
