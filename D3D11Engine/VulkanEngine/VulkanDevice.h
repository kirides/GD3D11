#pragma once
#include "VulkanCommon.h"
#include <mutex>
#include <string>

/** What the selected device can do beyond the hard requirements (a device missing any of those is
    rejected outright, see VulkanDevice.cpp). Filled by Init() and logged as the capability report. */
struct VulkanDeviceCaps {
    uint32_t ApiVersion = 0;
    uint32_t VendorId = 0;
    uint32_t DeviceId = 0;
    VkPhysicalDeviceType DeviceType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    bool     HasLuid = false;
    LUID     Luid = {};
    uint64_t DeviceLocalBytes = 0;

    // Optional instance/device extensions that were found and enabled.
    bool DebugUtils = false;
    bool Validation = false;
    bool SwapchainColorSpace = false;   // HDR10 swapchain colour spaces
    bool HdrMetadata = false;
    bool NullDescriptor = false;        // robustness2: unbound-but-used slots read zero instead of faulting
    bool MemoryBudget = false;
    bool MemoryPriority = false;       // allocations carry a residency priority
    bool PageableMemory = false;       // the OS may page device-local memory out under pressure, like D3D12
    bool DeviceFault = false;
    bool BufferMarkerAMD = false;
    bool DiagnosticCheckpointsNV = false;
    bool FragmentStoresAndAtomics = false;
    bool DepthBiasClamp = false;
    bool FillModeNonSolid = false;
    /** Heap-indexed ConstantBuffers can live in the mutable heap (feature + limits); else they need regular bindings. */
    bool HeapUniformBuffers = false;
    bool TimestampQueries = false;
    /** VK_EXT_device_generated_commands (+ maintenance5, buffer device address): ExecuteIndirect runs on the GPU. */
    bool DeviceGeneratedCommands = false;
    uint32_t DgcMaxIndirectStride = 0;
    uint32_t DgcMaxSequenceCount = 0;
    VkShaderStageFlags DgcShaderStages = 0;
    // VK_EXT_extended_dynamic_state3: these become dynamic, so PSOs differing only in them share a VkPipeline.
    bool DynamicBlend = false;             // colour blend enable + equation + write mask
    bool DynamicDepthClamp = false;
    bool DynamicPolygonMode = false;
    bool DynamicAlphaToCoverage = false;

    uint32_t MaxPushDescriptors = 0;
    uint32_t MaxPushConstantsSize = 0;
    VkDeviceSize MinUniformBufferOffsetAlignment = 0;
    VkDeviceSize MinStorageBufferOffsetAlignment = 0;
    VkDeviceSize OptimalBufferCopyOffsetAlignment = 0;
    VkDeviceSize OptimalBufferCopyRowPitchAlignment = 0;
    VkDeviceSize NonCoherentAtomSize = 0;
    float TimestampPeriod = 0.0f;
};

/** Owns the Vulkan instance, the selected physical device, the logical device and its queues.

    vulkan-1.dll is loaded through volk, so Vulkan is a soft dependency: no loader, no 1.3 device, a
    missing required feature or a dxcompiler.dll without SPIR-V codegen all make Init() fail and the
    engine fall back to D3D11. Required surface: VULKAN_IMPLEMENTATION_PLAN.md 5.2. */
class VulkanDevice {
public:
    /** Slots in the bindless heap (same indices as D3D12's shader-visible SRV heap). */
    static constexpr uint32_t kBindlessHeapSize = 65536;

    /** Throwaway probe: instance + device selection, then everything is destroyed again. Logs the
        capability report and fills outDescription / outReason. */
    static bool IsAvailable( std::string* outDescription = nullptr, std::string* outReason = nullptr );

    VulkanDevice() = default;
    ~VulkanDevice();
    VulkanDevice( const VulkanDevice& ) = delete;
    VulkanDevice& operator=( const VulkanDevice& ) = delete;

    /** Creates instance, device and queues. False (having logged why) on any failure. */
    bool Init();
    void Shutdown();
    void WaitIdle();

    VkInstance       GetInstance() const { return m_Instance; }
    VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
    VkDevice         GetDevice() const { return m_Device; }
    VkQueue          GetGraphicsQueue() const { return m_GraphicsQueue; }
    uint32_t         GetGraphicsQueueFamily() const { return m_GraphicsQueueFamily; }
    /** Dedicated DMA queue when the device has one, else the graphics queue. */
    VkQueue          GetTransferQueue() const { return m_TransferQueue; }
    uint32_t         GetTransferQueueFamily() const { return m_TransferQueueFamily; }
    const std::string& GetDeviceDescription() const { return m_DeviceDescription; }
    const VulkanDeviceCaps& GetCaps() const { return m_Caps; }

    /** vkQueueSubmit/vkQueuePresentKHR need external synchronization per queue; Gothic loads textures on
        worker threads, so every submit takes this. One lock covers both queues when they are the same. */
    std::mutex& GetGraphicsQueueMutex() { return m_GraphicsQueueMutex; }
    std::mutex& GetTransferQueueMutex() { return m_TransferQueue == m_GraphicsQueue ? m_GraphicsQueueMutex : m_TransferQueueMutex; }

    /** Debug-utils object name (no-op without VK_EXT_debug_utils). */
    void SetObjectName( VkObjectType type, uint64_t handle, const char* name ) const;

private:
    VkInstance               m_Instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_Messenger = VK_NULL_HANDLE;
    VkPhysicalDevice         m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice                 m_Device = VK_NULL_HANDLE;
    VkQueue                  m_GraphicsQueue = VK_NULL_HANDLE;
    VkQueue                  m_TransferQueue = VK_NULL_HANDLE;
    uint32_t                 m_GraphicsQueueFamily = 0;
    uint32_t                 m_TransferQueueFamily = 0;
    std::string              m_DeviceDescription;
    VulkanDeviceCaps         m_Caps;
    std::mutex               m_GraphicsQueueMutex;
    std::mutex               m_TransferQueueMutex;
};
