// volk's implementation, compiled with the Win32 surface functions enabled (the vcpkg volk.lib is built
// without platform defines). Every volk symbol is defined here, so the linker never pulls volk.lib in.
#include "../pch.h"
#define VOLK_IMPLEMENTATION
#include "VulkanCommon.h"

namespace VkUtil {
    const char* ResultToString( VkResult result ) {
        switch ( result ) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
        default: return "VK_ERROR_(unknown)";
        }
    }

    std::string VersionToString( uint32_t version ) {
        return std::format( "{}.{}.{}", VK_API_VERSION_MAJOR( version ), VK_API_VERSION_MINOR( version ),
            VK_API_VERSION_PATCH( version ) );
    }

    bool Failed( VkResult result, const char* what ) {
        if ( result >= VK_SUCCESS ) return false;
        Logging::Wrn( "Vulkan: {} failed ({}).", what, ResultToString( result ) );
        return true;
    }

    void LogAddressSpace( const char* stage ) {
        MEMORYSTATUSEX status = { sizeof( status ) };
        if ( !GlobalMemoryStatusEx( &status ) ) return;
        Logging::Inf( "Vulkan: address space {}: {} MiB free of {} MiB.", stage,
            status.ullAvailVirtual / ( 1024ull * 1024ull ), status.ullTotalVirtual / ( 1024ull * 1024ull ) );
    }
}
