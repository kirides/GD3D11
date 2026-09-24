#include "../pch.h"
#include "VulkanDevice.h"
#include "../D3D12Engine/D3D12ShaderBackend.h"

#include <dxgi1_6.h>
#include <algorithm>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
    constexpr uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;
    // The widest D3D12 root signature lowers to 18 push descriptors (plan 5.3); the spec minimum is 32.
    constexpr uint32_t kRequiredPushDescriptors = 18;

    bool LoadLoader( std::string* reason ) {
        static const VkResult s_result = volkInitialize();
        if ( s_result != VK_SUCCESS ) {
            if ( reason ) *reason = "vulkan-1.dll could not be loaded (no Vulkan driver installed)";
            return false;
        }
        return true;
    }

    bool HasExtension( const std::vector<VkExtensionProperties>& list, const char* name ) {
        return std::any_of( list.begin(), list.end(),
            [name]( const VkExtensionProperties& e ) { return strcmp( e.extensionName, name ) == 0; } );
    }

    std::vector<VkExtensionProperties> InstanceExtensions( const char* layer ) {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties( layer, &count, nullptr );
        std::vector<VkExtensionProperties> list( count );
        vkEnumerateInstanceExtensionProperties( layer, &count, list.data() );
        list.resize( count );
        return list;
    }

    bool HasValidationLayer() {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties( &count, nullptr );
        std::vector<VkLayerProperties> layers( count );
        vkEnumerateInstanceLayerProperties( &count, layers.data() );
        return std::any_of( layers.begin(), layers.begin() + count,
            []( const VkLayerProperties& l ) { return strcmp( l.layerName, "VK_LAYER_KHRONOS_validation" ) == 0; } );
    }

    /** Layers can be injected from outside (overlays, Vulkan Configurator, env vars); a forced validation layer is very slow. */
    void LogInstanceLayers() {
        static bool s_Logged = false;
        if ( s_Logged ) return;
        s_Logged = true;
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties( &count, nullptr );
        std::vector<VkLayerProperties> layers( count );
        vkEnumerateInstanceLayerProperties( &count, layers.data() );
        std::string names;
        bool configurator = false;
        for ( uint32_t i = 0; i < count; ++i ) {
            if ( !names.empty() ) names += ", ";
            names += layers[i].layerName;
            configurator |= strcmp( layers[i].layerName, "VK_LAYER_LUNARG_override" ) == 0;
        }
        Logging::Inf( "Vulkan: instance layers available: {}.", names.empty() ? "none" : names );
        if ( configurator )
            Logging::Wrn( "Vulkan: Vulkan Configurator is overriding layers (VK_LAYER_LUNARG_override); forced validation is very slow." );
        for ( const char* env : { "VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE" } ) {
            char value[512] = {};
            if ( GetEnvironmentVariableA( env, value, sizeof( value ) ) )
                Logging::Wrn( "Vulkan: {}={} forces layers into the game.", env, value );
        }
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback( VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* ) {
        const char* id = data && data->pMessageIdName ? data->pMessageIdName : "";
        const char* msg = data && data->pMessage ? data->pMessage : "";
        if ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ) {
            Logging::Err( "Vulkan validation [{}]: {}", id, msg );
        } else if ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ) {
            Logging::Wrn( "Vulkan validation [{}]: {}", id, msg );
        }
        return VK_FALSE;
    }

    struct InstanceSetup {
        VkInstance Instance = VK_NULL_HANDLE;
        bool DebugUtils = false;
        bool SwapchainColorSpace = false;
        bool Validation = false;
    };

    bool CreateInstance( bool allowValidation, InstanceSetup& out, std::string* reason ) {
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        if ( vkEnumerateInstanceVersion ) vkEnumerateInstanceVersion( &loaderVersion );
        if ( loaderVersion < kRequiredApiVersion ) {
            if ( reason ) *reason = "the Vulkan loader only supports " + VkUtil::VersionToString( loaderVersion ) + " (1.3 is required)";
            return false;
        }
        LogInstanceLayers();

        const auto available = InstanceExtensions( nullptr );
        std::vector<const char*> extensions;
        for ( const char* required : { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME } ) {
            if ( !HasExtension( available, required ) ) {
                if ( reason ) *reason = std::string( "the Vulkan loader lacks " ) + required;
                return false;
            }
            extensions.push_back( required );
        }
        out.DebugUtils = HasExtension( available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
        if ( out.DebugUtils ) extensions.push_back( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
        out.SwapchainColorSpace = HasExtension( available, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME );
        if ( out.SwapchainColorSpace ) extensions.push_back( VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME );

        // Validation needs the SDK's 32-bit layer, which usually isn't installed; skip quietly without it.
        std::vector<const char*> layers;
        bool validationFeatures = false;
#ifdef DEBUG_D3D11
        if ( allowValidation && HasValidationLayer() ) {
            layers.push_back( "VK_LAYER_KHRONOS_validation" );
            out.Validation = true;
            validationFeatures = HasExtension( InstanceExtensions( "VK_LAYER_KHRONOS_validation" ),
                VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME );
            if ( validationFeatures ) extensions.push_back( VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME );
        }
#else
        (void)allowValidation;
#endif

        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "Gothic";
        app.pEngineName = "GD3D11";
        app.engineVersion = VK_MAKE_API_VERSION( 0, 17, 0, 0 );
        app.apiVersion = kRequiredApiVersion;

        // Synchronization validation on, GPU-assisted validation off (plan 5.10).
        const VkValidationFeatureEnableEXT enables[] = { VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT };
        VkValidationFeaturesEXT validation = { VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT };
        validation.enabledValidationFeatureCount = 1;
        validation.pEnabledValidationFeatures = enables;

        VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ci.pNext = validationFeatures ? &validation : nullptr;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = static_cast<uint32_t>( extensions.size() );
        ci.ppEnabledExtensionNames = extensions.data();
        ci.enabledLayerCount = static_cast<uint32_t>( layers.size() );
        ci.ppEnabledLayerNames = layers.data();

        const VkResult r = vkCreateInstance( &ci, nullptr, &out.Instance );
        if ( r != VK_SUCCESS ) {
            if ( reason ) *reason = std::string( "vkCreateInstance failed (" ) + VkUtil::ResultToString( r ) + ")";
            return false;
        }
        volkLoadInstance( out.Instance );
        Logging::Inf( "Vulkan: instance created (loader {}{}{}{}).", VkUtil::VersionToString( loaderVersion ),
            out.DebugUtils ? ", debug utils" : "", out.SwapchainColorSpace ? ", swapchain colour spaces" : "",
            out.Validation ? ", VALIDATION LAYER" : "" );
        return true;
    }

    /** Everything we query about one physical device. Heap-allocated: the query chain points into itself. */
    struct DeviceInfo {
        VkPhysicalDevice Device = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties2 Props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        VkPhysicalDeviceVulkan11Properties Props11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES };
        VkPhysicalDeviceVulkan12Properties Props12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
        VkPhysicalDeviceVulkan13Properties Props13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES };
        VkPhysicalDevicePushDescriptorPropertiesKHR PushProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR };
        VkPhysicalDeviceFeatures2 Features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        VkPhysicalDeviceVulkan11Features Features11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features Features12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features Features13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT Mutable = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT };
        VkPhysicalDeviceRobustness2FeaturesKHR Robustness2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR };
        VkPhysicalDeviceFaultFeaturesEXT Fault = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
        VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT Dgc = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT };
        VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT DgcProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_PROPERTIES_EXT };
        VkPhysicalDeviceMaintenance5FeaturesKHR Maintenance5 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR };
        VkPhysicalDeviceMemoryProperties Memory = {};
        std::vector<VkExtensionProperties> Extensions;

        const char* MutableExtension = nullptr;       // EXT, or the older VALVE alias
        const char* Robustness2Extension = nullptr;   // KHR, or the older EXT
        uint32_t GraphicsFamily = UINT32_MAX;
        uint32_t TransferFamily = UINT32_MAX;
        bool     TimestampQueries = false;
        uint64_t DeviceLocalBytes = 0;
        std::vector<std::string> Missing;

        bool Has( const char* ext ) const { return HasExtension( Extensions, ext ); }
        const char* Name() const { return Props.properties.deviceName; }
        uint32_t ApiVersion() const { return Props.properties.apiVersion; }
    };

    const char* DeviceTypeName( VkPhysicalDeviceType type ) {
        switch ( type ) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "other";
        }
    }

    void QueryDevice( VkPhysicalDevice device, DeviceInfo& info ) {
        info.Device = device;
        vkGetPhysicalDeviceProperties( device, &info.Props.properties );
        vkGetPhysicalDeviceMemoryProperties( device, &info.Memory );
        for ( uint32_t i = 0; i < info.Memory.memoryHeapCount; ++i )
            if ( info.Memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT )
                info.DeviceLocalBytes += info.Memory.memoryHeaps[i].size;

        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties( device, nullptr, &count, nullptr );
        info.Extensions.resize( count );
        vkEnumerateDeviceExtensionProperties( device, nullptr, &count, info.Extensions.data() );
        info.Extensions.resize( count );

        // The 1.2/1.3 structs are only valid on a device that reports that version.
        if ( info.ApiVersion() < kRequiredApiVersion ) return;

        info.MutableExtension = info.Has( VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME ) ? VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME
            : info.Has( VK_VALVE_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME ) ? VK_VALVE_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME : nullptr;
        info.Robustness2Extension = info.Has( VK_KHR_ROBUSTNESS_2_EXTENSION_NAME ) ? VK_KHR_ROBUSTNESS_2_EXTENSION_NAME
            : info.Has( VK_EXT_ROBUSTNESS_2_EXTENSION_NAME ) ? VK_EXT_ROBUSTNESS_2_EXTENSION_NAME : nullptr;

        info.Props.pNext = &info.Props11;
        info.Props11.pNext = &info.Props12;
        info.Props12.pNext = &info.Props13;
        void** propsTail = &info.Props13.pNext;
        if ( info.Has( VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME ) ) { *propsTail = &info.PushProps; propsTail = &info.PushProps.pNext; }
        if ( info.Has( VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME ) ) { *propsTail = &info.DgcProps; propsTail = &info.DgcProps.pNext; }
        vkGetPhysicalDeviceProperties2( device, &info.Props );

        void** tail = &info.Features13.pNext;
        info.Features.pNext = &info.Features11;
        info.Features11.pNext = &info.Features12;
        info.Features12.pNext = &info.Features13;
        if ( info.MutableExtension ) { *tail = &info.Mutable; tail = &info.Mutable.pNext; }
        if ( info.Robustness2Extension ) { *tail = &info.Robustness2; tail = &info.Robustness2.pNext; }
        if ( info.Has( VK_EXT_DEVICE_FAULT_EXTENSION_NAME ) ) { *tail = &info.Fault; tail = &info.Fault.pNext; }
        if ( info.Has( VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME ) ) { *tail = &info.Dgc; tail = &info.Dgc.pNext; }
        if ( info.Has( VK_KHR_MAINTENANCE_5_EXTENSION_NAME ) ) { *tail = &info.Maintenance5; tail = &info.Maintenance5.pNext; }
        vkGetPhysicalDeviceFeatures2( device, &info.Features );

        vkGetPhysicalDeviceQueueFamilyProperties( device, &count, nullptr );
        std::vector<VkQueueFamilyProperties> families( count );
        vkGetPhysicalDeviceQueueFamilyProperties( device, &count, families.data() );
        for ( uint32_t i = 0; i < count; ++i ) {
            const VkQueueFlags flags = families[i].queueFlags;
            if ( info.GraphicsFamily == UINT32_MAX && ( flags & VK_QUEUE_GRAPHICS_BIT ) && ( flags & VK_QUEUE_COMPUTE_BIT )
                && vkGetPhysicalDeviceWin32PresentationSupportKHR( device, i ) ) {
                info.GraphicsFamily = i;
                info.TimestampQueries = families[i].timestampValidBits > 0;
            }
            if ( info.TransferFamily == UINT32_MAX && ( flags & VK_QUEUE_TRANSFER_BIT )
                && !( flags & ( VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT ) ) ) {
                info.TransferFamily = i;
            }
        }
        if ( info.TransferFamily == UINT32_MAX ) info.TransferFamily = info.GraphicsFamily;
    }

    /** Records every missing requirement (plan 5.2, cross-checked against the SPIR-V capabilities the
        D3D12 shaders actually emit). An empty list means the device is usable. */
    void CheckRequirements( DeviceInfo& info ) {
        auto require = [&info]( bool ok, const char* what ) { if ( !ok ) info.Missing.emplace_back( what ); };

        if ( info.ApiVersion() < kRequiredApiVersion ) {
            info.Missing.push_back( "Vulkan 1.3 (device reports " + VkUtil::VersionToString( info.ApiVersion() ) + ")" );
            return;
        }
        require( info.GraphicsFamily != UINT32_MAX, "a graphics+compute queue that can present to a window" );
        require( info.Has( VK_KHR_SWAPCHAIN_EXTENSION_NAME ), VK_KHR_SWAPCHAIN_EXTENSION_NAME );
        require( info.Has( VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME ), VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME );
        require( info.MutableExtension && info.Mutable.mutableDescriptorType, VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME );

        const VkPhysicalDeviceFeatures& f = info.Features.features;
        require( f.multiDrawIndirect, "multiDrawIndirect" );
        require( f.drawIndirectFirstInstance, "drawIndirectFirstInstance" );
        require( f.samplerAnisotropy, "samplerAnisotropy" );
        require( f.textureCompressionBC, "textureCompressionBC" );
        require( f.imageCubeArray, "imageCubeArray" );
        require( f.independentBlend, "independentBlend" );
        require( f.depthClamp, "depthClamp" );
        require( f.shaderClipDistance, "shaderClipDistance" );
        require( f.shaderStorageImageReadWithoutFormat, "shaderStorageImageReadWithoutFormat" );
        require( f.shaderStorageImageWriteWithoutFormat, "shaderStorageImageWriteWithoutFormat" );

        require( info.Features11.shaderDrawParameters, "shaderDrawParameters" );

        const VkPhysicalDeviceVulkan12Features& f12 = info.Features12;
        require( f12.timelineSemaphore, "timelineSemaphore" );
        require( f12.descriptorIndexing, "descriptorIndexing" );
        require( f12.runtimeDescriptorArray, "runtimeDescriptorArray" );
        require( f12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound" );
        require( f12.descriptorBindingVariableDescriptorCount, "descriptorBindingVariableDescriptorCount" );
        require( f12.descriptorBindingUpdateUnusedWhilePending, "descriptorBindingUpdateUnusedWhilePending" );
        require( f12.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind" );
        require( f12.descriptorBindingStorageImageUpdateAfterBind, "descriptorBindingStorageImageUpdateAfterBind" );
        require( f12.shaderSampledImageArrayNonUniformIndexing, "shaderSampledImageArrayNonUniformIndexing" );
        require( f12.scalarBlockLayout, "scalarBlockLayout" );
        require( f12.shaderFloat16, "shaderFloat16" );
        require( f12.drawIndirectCount, "drawIndirectCount" );
        require( f12.shaderOutputLayer, "shaderOutputLayer" );
        require( f12.separateDepthStencilLayouts, "separateDepthStencilLayouts" );

        const VkPhysicalDeviceVulkan13Features& f13 = info.Features13;
        require( f13.dynamicRendering, "dynamicRendering" );
        require( f13.synchronization2, "synchronization2" );
        require( f13.maintenance4, "maintenance4" );
        require( f13.shaderDemoteToHelperInvocation, "shaderDemoteToHelperInvocation" );

        // Wave intrinsics: the compute passes use WaveActiveBallot & co.
        require( ( info.Props11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT )
            && ( info.Props11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT ), "subgroup ballot in compute" );

        const VkPhysicalDeviceLimits& l = info.Props.properties.limits;
        const auto& l12 = info.Props12;
        const uint32_t heap = VulkanDevice::kBindlessHeapSize;
        require( l.maxBoundDescriptorSets >= 2, "maxBoundDescriptorSets >= 2" );
        require( info.PushProps.maxPushDescriptors >= kRequiredPushDescriptors, "maxPushDescriptors >= 18" );
        require( l12.maxDescriptorSetUpdateAfterBindSampledImages >= heap
            && l12.maxPerStageDescriptorUpdateAfterBindSampledImages >= heap, "65536 update-after-bind sampled images" );
        require( l12.maxDescriptorSetUpdateAfterBindStorageImages >= heap
            && l12.maxPerStageDescriptorUpdateAfterBindStorageImages >= heap, "65536 update-after-bind storage images" );
        require( l12.maxPerStageUpdateAfterBindResources >= heap, "maxPerStageUpdateAfterBindResources >= 65536" );
    }

    bool HeapUniformBuffersSupported( const DeviceInfo& info ) {
        const uint32_t heap = VulkanDevice::kBindlessHeapSize;
        return info.Features12.descriptorBindingUniformBufferUpdateAfterBind
            && info.Props12.maxDescriptorSetUpdateAfterBindUniformBuffers >= heap
            && info.Props12.maxPerStageDescriptorUpdateAfterBindUniformBuffers >= heap;
    }

    /** DXGI's high-performance adapter, so Vulkan runs on the GPU D3D11/D3D12 would pick. */
    bool GetPreferredAdapterLuid( LUID& out ) {
        typedef HRESULT( WINAPI* PFN_CREATE_DXGI_FACTORY1 )( REFIID riid, void** ppFactory );
        HMODULE dxgi = LoadLibraryA( "dxgi.dll" );
        auto create = dxgi ? reinterpret_cast<PFN_CREATE_DXGI_FACTORY1>( GetProcAddress( dxgi, "CreateDXGIFactory1" ) ) : nullptr;
        ComPtr<IDXGIFactory6> factory;
        if ( !create || FAILED( create( IID_PPV_ARGS( factory.GetAddressOf() ) ) ) ) return false;
        ComPtr<IDXGIAdapter1> adapter;
        for ( UINT i = 0; factory->EnumAdapterByGpuPreference( i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS( adapter.ReleaseAndGetAddressOf() ) ) != DXGI_ERROR_NOT_FOUND; ++i ) {
            DXGI_ADAPTER_DESC1 desc = {};
            if ( FAILED( adapter->GetDesc1( &desc ) ) || ( desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) ) continue;
            out = desc.AdapterLuid;
            return true;
        }
        return false;
    }

    bool LuidEquals( const DeviceInfo& info, const LUID& luid ) {
        return info.Props11.deviceLUIDValid && memcmp( info.Props11.deviceLUID, &luid, sizeof( LUID ) ) == 0;
    }

    void LogDevice( size_t index, const DeviceInfo& info ) {
        Logging::Inf( "Vulkan: GPU {}: \"{}\" ({}, API {}, driver {} {}, {} MiB device-local){}", index, info.Name(),
            DeviceTypeName( info.Props.properties.deviceType ), VkUtil::VersionToString( info.ApiVersion() ),
            info.Props12.driverName, info.Props12.driverInfo, info.DeviceLocalBytes / ( 1024ull * 1024ull ),
            info.Missing.empty() ? "" : " - rejected" );
        if ( !info.Missing.empty() ) {
            std::string list;
            for ( const auto& m : info.Missing ) list.append( list.empty() ? "" : ", " ).append( m );
            Logging::Inf( "Vulkan:   missing: {}", list );
        }
    }

    void LogCapabilities( const DeviceInfo& info ) {
        const VkPhysicalDeviceLimits& l = info.Props.properties.limits;
        auto yn = []( bool b ) { return b ? "yes" : "no"; };
        Logging::Inf( "Vulkan: queues: graphics family {}, transfer family {}{}; timestamps {}.", info.GraphicsFamily,
            info.TransferFamily, info.TransferFamily != info.GraphicsFamily ? " (dedicated)" : " (shared)", yn( info.TimestampQueries ) );
        Logging::Inf( "Vulkan: limits: maxPushDescriptors {}, maxPushConstantsSize {}, minUniformBufferOffsetAlignment {}, "
            "minStorageBufferOffsetAlignment {}, optimalBufferCopyRowPitchAlignment {}, nonCoherentAtomSize {}, maxImageArrayLayers {}.",
            info.PushProps.maxPushDescriptors, l.maxPushConstantsSize, l.minUniformBufferOffsetAlignment,
            l.minStorageBufferOffsetAlignment, l.optimalBufferCopyRowPitchAlignment, l.nonCoherentAtomSize, l.maxImageArrayLayers );
        Logging::Inf( "Vulkan: update-after-bind limits: sampled {}, storage images {}, uniform buffers {}, storage buffers {}, "
            "per-stage resources {}.", info.Props12.maxDescriptorSetUpdateAfterBindSampledImages,
            info.Props12.maxDescriptorSetUpdateAfterBindStorageImages, info.Props12.maxDescriptorSetUpdateAfterBindUniformBuffers,
            info.Props12.maxDescriptorSetUpdateAfterBindStorageBuffers, info.Props12.maxPerStageUpdateAfterBindResources );
        Logging::Inf( "Vulkan: optional: heap UBOs {}, nullDescriptor {}, storageBuffer16BitAccess {}, fragmentStoresAndAtomics {}, "
            "depthBiasClamp {}, fillModeNonSolid {}.", yn( HeapUniformBuffersSupported( info ) ),
            yn( info.Robustness2Extension && info.Robustness2.nullDescriptor ), yn( info.Features11.storageBuffer16BitAccess ),
            yn( info.Features.features.fragmentStoresAndAtomics ), yn( info.Features.features.depthBiasClamp ),
            yn( info.Features.features.fillModeNonSolid ) );

        static const char* const kOptionalExtensions[] = {
            VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, VK_EXT_DEVICE_FAULT_EXTENSION_NAME, VK_AMD_BUFFER_MARKER_EXTENSION_NAME,
            VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, VK_EXT_HDR_METADATA_EXTENSION_NAME,
            VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
            VK_KHR_PRESENT_ID_2_EXTENSION_NAME, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME,
            VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
            VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
            VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
            VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, VK_AMD_ANTI_LAG_EXTENSION_NAME, VK_NV_LOW_LATENCY_2_EXTENSION_NAME,
            VK_KHR_MAINTENANCE_6_EXTENSION_NAME,
        };
        std::string present, absent;
        for ( const char* ext : kOptionalExtensions ) {
            std::string& list = info.Has( ext ) ? present : absent;
            if ( !list.empty() ) list += ", ";
            list += ext;
        }
        Logging::Inf( "Vulkan: optional extensions present: {}", present.empty() ? "none" : present );
        Logging::Inf( "Vulkan: optional extensions absent: {}", absent.empty() ? "none" : absent );
    }

    /** Enumerates, evaluates and logs every GPU, then picks one. Returns nullptr (with reason) if none is usable. */
    std::unique_ptr<DeviceInfo> SelectDevice( VkInstance instance, std::string* reason ) {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices( instance, &count, nullptr );
        std::vector<VkPhysicalDevice> devices( count );
        vkEnumeratePhysicalDevices( instance, &count, devices.data() );
        if ( count == 0 ) {
            if ( reason ) *reason = "no Vulkan physical device was found";
            return nullptr;
        }

        std::vector<std::unique_ptr<DeviceInfo>> infos;
        for ( uint32_t i = 0; i < count; ++i ) {
            auto info = std::make_unique<DeviceInfo>();
            QueryDevice( devices[i], *info );
            CheckRequirements( *info );
            LogDevice( i, *info );
            infos.push_back( std::move( info ) );
        }

        LUID preferred = {};
        const bool havePreferred = GetPreferredAdapterLuid( preferred );
        auto score = [&]( const DeviceInfo& info ) -> uint64_t {
            uint64_t s = info.DeviceLocalBytes / ( 1024ull * 1024ull );
            if ( info.Props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ) s += 1ull << 40;
            else if ( info.Props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ) s += 1ull << 39;
            if ( havePreferred && LuidEquals( info, preferred ) ) s += 1ull << 50;
            return s;
        };

        std::unique_ptr<DeviceInfo>* best = nullptr;
        for ( auto& info : infos ) {
            if ( !info->Missing.empty() ) continue;
            if ( !best || score( *info ) > score( **best ) ) best = &info;
        }
        if ( !best ) {
            if ( reason ) {
                const DeviceInfo& first = *infos.front();
                *reason = "no GPU meets the Vulkan requirements (\"" + std::string( first.Name() ) + "\" lacks "
                    + first.Missing.front() + ( first.Missing.size() > 1 ? " and more, see Log.txt" : "" ) + ")";
            }
            return nullptr;
        }
        Logging::Inf( "Vulkan: selected \"{}\"{}.", ( *best )->Name(),
            havePreferred && LuidEquals( **best, preferred ) ? " (DXGI's high-performance adapter)" : "" );
        LogCapabilities( **best );
        return std::move( *best );
    }
}

bool VulkanDevice::IsAvailable( std::string* outDescription, std::string* outReason ) {
    if ( !LoadLoader( outReason ) ) return false;

    InstanceSetup setup;
    if ( !CreateInstance( false, setup, outReason ) ) return false;
    std::unique_ptr<DeviceInfo> info = SelectDevice( setup.Instance, outReason );
    if ( info && outDescription ) *outDescription = info->Name();
    vkDestroyInstance( setup.Instance, nullptr );
    if ( !info ) return false;

    // The backend compiles the D3D12 HLSL to SPIR-V at runtime; a DXC without SPIR-V codegen can't run it.
    return D3D12ShaderBackend::IsSpirvCodegenAvailable( outReason );
}

VulkanDevice::~VulkanDevice() {
    Shutdown();
}

void VulkanDevice::Shutdown() {
    if ( m_Device ) {
        vkDeviceWaitIdle( m_Device );
        vkDestroyDevice( m_Device, nullptr );
        m_Device = VK_NULL_HANDLE;
    }
    if ( m_Messenger ) {
        vkDestroyDebugUtilsMessengerEXT( m_Instance, m_Messenger, nullptr );
        m_Messenger = VK_NULL_HANDLE;
    }
    if ( m_Instance ) {
        vkDestroyInstance( m_Instance, nullptr );
        m_Instance = VK_NULL_HANDLE;
    }
}

void VulkanDevice::WaitIdle() {
    if ( m_Device ) vkDeviceWaitIdle( m_Device );
}

bool VulkanDevice::Init() {
    VkUtil::LogAddressSpace( "before Vulkan init" );

    std::string reason;
    if ( !LoadLoader( &reason ) ) {
        Logging::Wrn( "Vulkan: {}.", reason );
        return false;
    }
    if ( !D3D12ShaderBackend::IsSpirvCodegenAvailable( &reason ) ) {
        Logging::Wrn( "Vulkan: {}.", reason );
        return false;
    }

    InstanceSetup setup;
    if ( !CreateInstance( true, setup, &reason ) ) {
        Logging::Wrn( "Vulkan: {}.", reason );
        return false;
    }
    m_Instance = setup.Instance;
    m_Caps.DebugUtils = setup.DebugUtils;
    m_Caps.SwapchainColorSpace = setup.SwapchainColorSpace;
    m_Caps.Validation = setup.Validation;

    if ( setup.Validation && setup.DebugUtils ) {
        VkDebugUtilsMessengerCreateInfoEXT mci = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = &DebugMessengerCallback;
        VkUtil::Failed( vkCreateDebugUtilsMessengerEXT( m_Instance, &mci, nullptr, &m_Messenger ), "vkCreateDebugUtilsMessengerEXT" );
    }

    std::unique_ptr<DeviceInfo> info = SelectDevice( m_Instance, &reason );
    if ( !info ) {
        Logging::Wrn( "Vulkan: {}.", reason );
        return false;
    }
    m_PhysicalDevice = info->Device;
    m_GraphicsQueueFamily = info->GraphicsFamily;
    m_TransferQueueFamily = info->TransferFamily;

    // --- Extensions ---
    std::vector<const char*> extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, info->MutableExtension,
    };
    auto enableIf = [&]( bool condition, const char* ext ) {
        if ( condition ) extensions.push_back( ext );
        return condition;
    };
    m_Caps.NullDescriptor = enableIf( info->Robustness2Extension && info->Robustness2.nullDescriptor, info->Robustness2Extension );
    m_Caps.MemoryBudget = enableIf( info->Has( VK_EXT_MEMORY_BUDGET_EXTENSION_NAME ), VK_EXT_MEMORY_BUDGET_EXTENSION_NAME );
    m_Caps.DeviceFault = enableIf( info->Has( VK_EXT_DEVICE_FAULT_EXTENSION_NAME ) && info->Fault.deviceFault, VK_EXT_DEVICE_FAULT_EXTENSION_NAME );
    m_Caps.BufferMarkerAMD = enableIf( info->Has( VK_AMD_BUFFER_MARKER_EXTENSION_NAME ), VK_AMD_BUFFER_MARKER_EXTENSION_NAME );
    m_Caps.DiagnosticCheckpointsNV = enableIf( info->Has( VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME ),
        VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME );
    m_Caps.HdrMetadata = enableIf( m_Caps.SwapchainColorSpace && info->Has( VK_EXT_HDR_METADATA_EXTENSION_NAME ),
        VK_EXT_HDR_METADATA_EXTENSION_NAME );
    // Device-generated commands draw the D3D12 command signatures without CPU replay; optional.
    constexpr VkShaderStageFlags kDgcStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // -VKNODGC / GD3D11_VULKAN_NO_DGC=1 fall back to CPU replay, for A/B tests and driver trouble.
    const bool dgcDisabled = strstr( GetCommandLineA(), "-VKNODGC" ) || strstr( GetCommandLineA(), "-vknodgc" )
        || GetEnvironmentVariableA( "GD3D11_VULKAN_NO_DGC", nullptr, 0 ) > 0;
    m_Caps.DeviceGeneratedCommands = !dgcDisabled && info->Has( VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME ) && info->Dgc.deviceGeneratedCommands
        && info->Has( VK_KHR_MAINTENANCE_5_EXTENSION_NAME ) && info->Maintenance5.maintenance5 && info->Features12.bufferDeviceAddress
        && ( info->DgcProps.supportedIndirectCommandsShaderStages & kDgcStages ) == kDgcStages;
    if ( m_Caps.DeviceGeneratedCommands ) {
        extensions.push_back( VK_KHR_MAINTENANCE_5_EXTENSION_NAME );
        extensions.push_back( VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME );
        m_Caps.DgcMaxIndirectStride = info->DgcProps.maxIndirectCommandsIndirectStride;
        m_Caps.DgcMaxSequenceCount = info->DgcProps.maxIndirectSequenceCount;
        m_Caps.DgcShaderStages = kDgcStages;
    }

    // --- Features: every requirement from CheckRequirements, plus the supported optional ones ---
    const VkPhysicalDeviceFeatures& sup = info->Features.features;
    VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceFeatures& f = features.features;
    f.multiDrawIndirect = VK_TRUE;
    f.drawIndirectFirstInstance = VK_TRUE;
    f.samplerAnisotropy = VK_TRUE;
    f.textureCompressionBC = VK_TRUE;
    f.imageCubeArray = VK_TRUE;
    f.independentBlend = VK_TRUE;
    f.depthClamp = VK_TRUE;
    f.shaderClipDistance = VK_TRUE;
    f.shaderStorageImageReadWithoutFormat = VK_TRUE;
    f.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    f.fragmentStoresAndAtomics = sup.fragmentStoresAndAtomics;
    f.depthBiasClamp = sup.depthBiasClamp;
    f.fillModeNonSolid = sup.fillModeNonSolid;
    f.shaderInt16 = sup.shaderInt16;
    m_Caps.FragmentStoresAndAtomics = sup.fragmentStoresAndAtomics;
    m_Caps.DepthBiasClamp = sup.depthBiasClamp;
    m_Caps.FillModeNonSolid = sup.fillModeNonSolid;

    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    f11.shaderDrawParameters = VK_TRUE;
    f11.storageBuffer16BitAccess = info->Features11.storageBuffer16BitAccess;
    f11.uniformAndStorageBuffer16BitAccess = info->Features11.uniformAndStorageBuffer16BitAccess;

    const VkPhysicalDeviceVulkan12Features& sup12 = info->Features12;
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.timelineSemaphore = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.shaderFloat16 = VK_TRUE;
    f12.drawIndirectCount = VK_TRUE;
    f12.shaderOutputLayer = VK_TRUE;
    f12.separateDepthStencilLayouts = VK_TRUE;
    f12.descriptorBindingUniformBufferUpdateAfterBind = sup12.descriptorBindingUniformBufferUpdateAfterBind;
    f12.descriptorBindingStorageBufferUpdateAfterBind = sup12.descriptorBindingStorageBufferUpdateAfterBind;
    f12.shaderStorageImageArrayNonUniformIndexing = sup12.shaderStorageImageArrayNonUniformIndexing;
    f12.shaderUniformBufferArrayNonUniformIndexing = sup12.shaderUniformBufferArrayNonUniformIndexing;
    f12.shaderStorageBufferArrayNonUniformIndexing = sup12.shaderStorageBufferArrayNonUniformIndexing;
    f12.hostQueryReset = sup12.hostQueryReset;
    f12.samplerMirrorClampToEdge = sup12.samplerMirrorClampToEdge;
    f12.bufferDeviceAddress = m_Caps.DeviceGeneratedCommands;
    m_Caps.HeapUniformBuffers = HeapUniformBuffersSupported( *info );
    m_Caps.TimestampQueries = info->TimestampQueries;

    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.maintenance4 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE;

    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT };
    mutableFeatures.mutableDescriptorType = VK_TRUE;
    VkPhysicalDeviceRobustness2FeaturesKHR robustness2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR };
    robustness2.nullDescriptor = VK_TRUE;
    VkPhysicalDeviceFaultFeaturesEXT fault = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
    fault.deviceFault = VK_TRUE;
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR };
    maintenance5.maintenance5 = VK_TRUE;
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgc = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT };
    dgc.deviceGeneratedCommands = VK_TRUE;

    features.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &f13;
    f13.pNext = &mutableFeatures;
    void** tail = &mutableFeatures.pNext;
    if ( m_Caps.NullDescriptor ) { *tail = &robustness2; tail = &robustness2.pNext; }
    if ( m_Caps.DeviceFault ) { *tail = &fault; tail = &fault.pNext; }
    if ( m_Caps.DeviceGeneratedCommands ) {
        *tail = &maintenance5; tail = &maintenance5.pNext;
        *tail = &dgc; tail = &dgc.pNext;
    }

    // --- Queues ---
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queues[2] = {};
    uint32_t queueCount = 0;
    for ( uint32_t family : { m_GraphicsQueueFamily, m_TransferQueueFamily } ) {
        if ( queueCount == 1 && family == m_GraphicsQueueFamily ) break;
        queues[queueCount] = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queues[queueCount].queueFamilyIndex = family;
        queues[queueCount].queueCount = 1;
        queues[queueCount].pQueuePriorities = &priority;
        ++queueCount;
    }

    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &features;
    dci.queueCreateInfoCount = queueCount;
    dci.pQueueCreateInfos = queues;
    dci.enabledExtensionCount = static_cast<uint32_t>( extensions.size() );
    dci.ppEnabledExtensionNames = extensions.data();
    if ( VkUtil::Failed( vkCreateDevice( m_PhysicalDevice, &dci, nullptr, &m_Device ), "vkCreateDevice" ) ) {
        m_Device = VK_NULL_HANDLE;
        return false;
    }
    volkLoadDevice( m_Device );
    vkGetDeviceQueue( m_Device, m_GraphicsQueueFamily, 0, &m_GraphicsQueue );
    vkGetDeviceQueue( m_Device, m_TransferQueueFamily, 0, &m_TransferQueue );

    const VkPhysicalDeviceProperties& props = info->Props.properties;
    m_DeviceDescription = props.deviceName;
    m_Caps.ApiVersion = props.apiVersion;
    m_Caps.VendorId = props.vendorID;
    m_Caps.DeviceId = props.deviceID;
    m_Caps.DeviceType = props.deviceType;
    m_Caps.HasLuid = info->Props11.deviceLUIDValid;
    if ( m_Caps.HasLuid ) memcpy( &m_Caps.Luid, info->Props11.deviceLUID, sizeof( LUID ) );
    m_Caps.DeviceLocalBytes = info->DeviceLocalBytes;
    m_Caps.MaxPushDescriptors = info->PushProps.maxPushDescriptors;
    m_Caps.MaxPushConstantsSize = props.limits.maxPushConstantsSize;
    m_Caps.MinUniformBufferOffsetAlignment = props.limits.minUniformBufferOffsetAlignment;
    m_Caps.MinStorageBufferOffsetAlignment = props.limits.minStorageBufferOffsetAlignment;
    m_Caps.OptimalBufferCopyOffsetAlignment = props.limits.optimalBufferCopyOffsetAlignment;
    m_Caps.OptimalBufferCopyRowPitchAlignment = props.limits.optimalBufferCopyRowPitchAlignment;
    m_Caps.NonCoherentAtomSize = props.limits.nonCoherentAtomSize;
    m_Caps.TimestampPeriod = props.limits.timestampPeriod;

    SetObjectName( VK_OBJECT_TYPE_QUEUE, VkUtil::HandleToU64( m_GraphicsQueue ), "GraphicsQueue" );
    if ( m_TransferQueue != m_GraphicsQueue )
        SetObjectName( VK_OBJECT_TYPE_QUEUE, VkUtil::HandleToU64( m_TransferQueue ), "TransferQueue" );

    Logging::Inf( "Vulkan device created on: {}{}", m_DeviceDescription,
        m_Caps.DeviceGeneratedCommands ? " (device-generated commands on)" : "" );
    VkUtil::LogAddressSpace( "after Vulkan device creation" );
    return true;
}

void VulkanDevice::SetObjectName( VkObjectType type, uint64_t handle, const char* name ) const {
    if ( !m_Caps.DebugUtils || !m_Device || !handle || !name ) return;
    VkDebugUtilsObjectNameInfoEXT info = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT( m_Device, &info );
}
