#pragma once
#include <cctype>
#include <cstring>
#include <string>

/** Command-line switches for GPU fault hunting (D3D12 and Vulkan). All off by default; each costs GPU time. */
namespace GpuDebug {
    /** Case-insensitive search of the process command line for `flag` (e.g. "-GPUHEAPCHECK"). */
    inline bool CommandLineHas( const char* flag ) {
        static const std::string s_upper = [] {
            std::string s = GetCommandLineA();
            for ( char& c : s ) c = static_cast<char>( toupper( static_cast<unsigned char>( c ) ) );
            return s;
        }();
        std::string upper = flag;
        for ( char& c : upper ) c = static_cast<char>( toupper( static_cast<unsigned char>( c ) ) );
        return s_upper.find( upper ) != std::string::npos;
    }

    /** Every bindless heap index is checked against the live-slot bitmap; violations are logged by shader and line. */
    inline bool HeapCheck() { static const bool s = CommandLineHas( "-GPUHEAPCHECK" ); return s; }
    /** Vulkan: a breadcrumb named after the pipeline around every draw and dispatch. */
    inline bool VkDrawCrumbs() { static const bool s = CommandLineHas( "-VKDRAWCRUMBS" ); return s; }
    /** Vulkan on NVIDIA: VK_NV_device_diagnostics_config for Nsight Aftermath dumps, SPIR-V with debug info. */
    inline bool VkAftermath() { static const bool s = CommandLineHas( "-VKAFTERMATH" ); return s; }
    /** D3D12: debug layer with GPU-based validation, also in release builds. */
    inline bool D3D12Gbv() { static const bool s = CommandLineHas( "-D3D12GBV" ); return s; }
}
