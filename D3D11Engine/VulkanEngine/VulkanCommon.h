#pragma once
// Every Vulkan translation unit includes this instead of <vulkan/vulkan.h>: vulkan-1.dll is loaded
// dynamically through volk (no vulkan-1.lib link), which keeps the D3D11 path's Windows 7 floor.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>
#include <string>
#include <type_traits>

namespace VkUtil {
    const char* ResultToString( VkResult result );
    std::string VersionToString( uint32_t version );

    /** Logs a warning naming `what` when `result` is an error. Returns true on failure. */
    bool Failed( VkResult result, const char* what );

    /** Logs the 32-bit process's free virtual address space; the Vulkan driver and swapchain reserve it too. */
    void LogAddressSpace( const char* stage );

    /** Any handle as the uint64_t debug utils wants: on 32-bit, non-dispatchable handles already are uint64_t. */
    template <typename T>
    uint64_t HandleToU64( T handle ) {
        if constexpr ( std::is_pointer_v<T> ) return static_cast<uint64_t>( reinterpret_cast<uintptr_t>( handle ) );
        else return static_cast<uint64_t>( handle );
    }
}
