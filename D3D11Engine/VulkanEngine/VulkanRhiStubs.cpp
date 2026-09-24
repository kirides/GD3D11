#include "../pch.h"
#include "VulkanRhiInternal.h"

// Not implemented yet: the swapchain lands in its own commit.
namespace VulkanRhi {
    HRESULT DeviceImpl::CreateSwapchain( const Rhi::SwapchainDesc&, Rhi::Swapchain** ) { return E_NOTIMPL; }
}
