#include "../pch.h"
// The one translation unit that compiles VMA. Function pointers come from volk (vmaImportVulkanFunctionsFromVolk),
// since vulkan-1.dll is never linked.
#include "VulkanCommon.h"
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#pragma warning( push, 0 )
#include <vma/vk_mem_alloc.h>
#pragma warning( pop )
