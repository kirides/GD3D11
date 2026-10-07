// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

/// @defgroup VKBackend Vulkan Backend
/// FidelityFX SDK native backend implementation for Vulkan.
///
/// Ported from SDK 1.1.4 to the 2.3.0 FfxInterface. The frame interpolation swapchain
/// API that 1.1.4 declared here is not carried over; see FORK_DEVIATIONS.md.
///
/// @ingroup Backends

#pragma once

#include <vulkan/vulkan.h>
#include "../../api/internal/ffx_interface.h"

#if defined(__cplusplus)
extern "C" {
#endif // #if defined(__cplusplus)

/// Query how much memory is required for the Vulkan backend's scratch buffer.
///
/// @param [in] physicalDevice              A pointer to the VkPhysicalDevice device.
/// @param [in] maxContexts                 The maximum number of simultaneous effect contexts that will share the backend.
///                                         (Note that some effects contain internal contexts which count towards this maximum)
///
/// @returns
/// The size (in bytes) of the required scratch memory buffer for the VK backend.
///
/// @ingroup VKBackend
FFX_API size_t ffxGetScratchMemorySizeVK(VkPhysicalDevice physicalDevice, size_t maxContexts);

/// Convenience structure to hold all VK-related device information
typedef struct VkDeviceContext {
    VkDevice                vkDevice;           /// The Vulkan device
    VkPhysicalDevice        vkPhysicalDevice;   /// The Vulkan physical device
    PFN_vkGetDeviceProcAddr vkDeviceProcAddr;   /// The device's function address table
} VkDeviceContext;

/// Create a <c><i>FfxDevice</i></c> from a <c><i>VkDevice</i></c>.
///
/// @param [in] vkDeviceContext             A pointer to a VKDeviceContext that holds all needed information
///
/// @returns
/// An abstract FidelityFX device.
///
/// @ingroup VKBackend
FFX_API FfxDevice ffxGetDeviceVK(VkDeviceContext* vkDeviceContext);

/// Populate an interface with pointers for the VK backend.
///
/// @param [out] backendInterface           A pointer to a <c><i>FfxInterface</i></c> structure to populate with pointers.
/// @param [in] device                      A pointer to the VkDevice device.
/// @param [in] scratchBuffer               A pointer to a buffer of memory which can be used by the Vulkan backend.
/// @param [in] scratchBufferSize           The size (in bytes) of the buffer pointed to by <c><i>scratchBuffer</i></c>.
/// @param [in] maxContexts                 The maximum number of simultaneous effect contexts that will share the backend.
///                                         (Note that some effects contain internal contexts which count towards this maximum)
///
/// @retval
/// FFX_OK                                  The operation completed successfully.
/// @retval
/// FFX_ERROR_CODE_INVALID_POINTER          The <c><i>interface</i></c> pointer was <c><i>NULL</i></c>.
///
/// @ingroup VKBackend
FFX_API FfxErrorCode ffxGetInterfaceVK(
    FfxInterface* backendInterface,
    FfxDevice device,
    void* scratchBuffer,
    size_t scratchBufferSize,
    size_t maxContexts);

/// Create a <c><i>FfxCommandList</i></c> from a <c><i>VkCommandBuffer</i></c>.
///
/// @param [in] cmdBuf                      A pointer to the Vulkan command buffer.
///
/// @returns
/// An abstract FidelityFX command list.
///
/// @ingroup VKBackend
FFX_API FfxCommandList ffxGetCommandListVK(VkCommandBuffer cmdBuf);

/// Create a <c><i>FfxPipeline</i></c> from a <c><i>VkPipeline</i></c>.
///
/// @param [in] pipeline                    A pointer to the Vulkan pipeline.
///
/// @returns
/// An abstract FidelityFX pipeline.
///
/// @ingroup VKBackend
FFX_API FfxPipeline ffxGetPipelineVK(VkPipeline pipeline);

/// Fetch a <c><i>FfxApiResource</i></c> from a <c><i>GPUResource</i></c>.
///
/// @param [in] vkResource                  A pointer to the (agnostic) VK resource.
/// @param [in] ffxResDescription           An <c><i>FfxApiResourceDescription</i></c> for the resource representation.
/// @param [in] ffxResName                  (optional) A name string to identify the resource in debug mode.
/// @param [in] state                       The state the resource is currently in.
///
/// @returns
/// An abstract FidelityFX resources.
///
/// @ingroup VKBackend
FFX_API FfxApiResource ffxGetResourceVK(void*  vkResource,
    FfxApiResourceDescription               ffxResDescription,
    const wchar_t*                          ffxResName,
    uint32_t                                state = FFX_API_RESOURCE_STATE_COMPUTE_READ);

/// Fetch a <c><i>FfxApiSurfaceFormat</i></c> from a VkFormat.
///
/// @param [in] format              The VkFormat to convert to <c><i>FfxApiSurfaceFormat</i></c>.
///
/// @returns
/// An <c><i>FfxApiSurfaceFormat</i></c>.
///
/// @ingroup VKBackend
FFX_API FfxApiSurfaceFormat ffxGetSurfaceFormatVK(VkFormat format);

/// Fetch a <c><i>FfxApiResourceDescription</i></c> from an existing VkBuffer.
///
/// @param [in] buffer              The VkBuffer resource to create a <c><i>FfxApiResourceDescription</i></c> for.
/// @param [in] createInfo          The VkBufferCreateInfo of the buffer
/// @param [in] additionalUsages    Optional <c><i>FfxApiResourceUsage</i></c> flags needed for select resource mapping.
///
/// @returns
/// An <c><i>FfxApiResourceDescription</i></c>.
///
/// @ingroup VKBackend
FFX_API FfxApiResourceDescription ffxGetBufferResourceDescriptionVK(const VkBuffer           buffer,
                                                                    const VkBufferCreateInfo createInfo,
                                                                    FfxApiResourceUsage      additionalUsages = FFX_API_RESOURCE_USAGE_READ_ONLY);

/// Fetch a <c><i>FfxApiResourceDescription</i></c> from an existing VkImage.
///
/// @param [in] image               The VkImage resource to create a <c><i>FfxApiResourceDescription</i></c> for.
/// @param [in] createInfo          The VkImageCreateInfo of the buffer
/// @param [in] additionalUsages    Optional <c><i>FfxApiResourceUsage</i></c> flags needed for select resource mapping.
///
/// @returns
/// An <c><i>FfxApiResourceDescription</i></c>.
///
/// @ingroup VKBackend
FFX_API FfxApiResourceDescription ffxGetImageResourceDescriptionVK(const VkImage           image,
                                                                   const VkImageCreateInfo createInfo,
                                                                   FfxApiResourceUsage     additionalUsages = FFX_API_RESOURCE_USAGE_READ_ONLY);

/// Estimate the memory a resource described by <c><i>createResourceDescription</i></c> would occupy.
///
/// Creates the image or buffer, queries its memory requirements and destroys it again,
/// without allocating memory for it.
///
/// @ingroup VKBackend
FFX_API FfxErrorCode ffxGetResourceSizeFromDescriptionVK(FfxDevice device, const FfxCreateResourceDescription* createResourceDescription, uint64_t* sizeInBytes, uint64_t* alignment = nullptr);

/// Replace the backend's own constant buffer allocator.
///
/// In SDK 1.1.4 this was reached through <c><i>FfxInterface::fpRegisterConstantBufferAllocator</i></c>;
/// 2.3.0 removed that callback, so it is a global setter here, as it is for DX12
/// (<c><i>ffxRegisterConstantBufferAllocatorDX12</i></c>). The allocator must be thread safe.
/// Pass <c><i>nullptr</i></c> to return to the built-in allocator.
///
/// @ingroup VKBackend
FFX_API void ffxRegisterConstantBufferAllocatorVK(FfxApiConstantBufferAllocator fpConstantAllocator);

#if defined(__cplusplus)
}
#endif // #if defined(__cplusplus)
