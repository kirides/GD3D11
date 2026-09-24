#include "../pch.h"
#include "VulkanRhiInternal.h"

// Not implemented yet: command lists and the swapchain land in their own commits.
namespace VulkanRhi {
    VkCommandBuffer CommandBufferOf( Rhi::CommandList* ) { return VK_NULL_HANDLE; }

    HRESULT DeviceImpl::CreateCommandList( D3D12_COMMAND_LIST_TYPE, Rhi::CommandAllocator*, Rhi::PipelineState*, Rhi::CommandList** ) { return E_NOTIMPL; }
    HRESULT DeviceImpl::CreateSwapchain( const Rhi::SwapchainDesc&, Rhi::Swapchain** ) { return E_NOTIMPL; }
}
