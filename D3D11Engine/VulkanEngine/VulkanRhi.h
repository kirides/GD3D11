#pragma once
#include "../RHI/Rhi.h"
#include <wrl/client.h>

/** The Vulkan implementation of the RHI: translates the D3D12-shaped calls (VULKAN_IMPLEMENTATION_PLAN.md 5). */
namespace VulkanRhi {
    /** Creates instance, device, queues and the memory allocator; null (logged) on failure. */
    Microsoft::WRL::ComPtr<Rhi::Device> CreateDevice();
}
