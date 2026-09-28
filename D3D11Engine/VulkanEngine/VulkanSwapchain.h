#pragma once
#include "VulkanCommon.h"
#include "../Types.h"
#include <mutex>
#include <vector>

class VulkanDevice;

/** Win32 surface + swapchain on Gothic's window.

    Presentation needs one "render finished" semaphore per swapchain IMAGE, not per frame in flight: a
    semaphore handed to vkQueuePresentKHR is only known to be free again once that image is re-acquired.
    Extent follows the window's client area (Win32 requires currentExtent), so a minimized window has a
    zero extent and IsUsable() stays false until it is restored. */
class VulkanSwapchain {
public:
    VulkanSwapchain() = default;
    ~VulkanSwapchain() { Destroy(); }
    VulkanSwapchain( const VulkanSwapchain& ) = delete;
    VulkanSwapchain& operator=( const VulkanSwapchain& ) = delete;

    /** hdr requests an HDR10 (A2B10G10R10 + ST.2084) swapchain; falls back to SDR when the surface can't. */
    bool Create( VulkanDevice& device, HWND window, INT2 size, uint32_t minImageCount, bool vsync, bool hdr );
    /** Waits for the device to go idle, then rebuilds at `size` (the window's client area wins on Win32). */
    bool Recreate( INT2 size, bool vsync );
    void Destroy();

    /** VK_SUCCESS / VK_SUBOPTIMAL_KHR hand out an image; VK_ERROR_OUT_OF_DATE_KHR means Recreate first. */
    VkResult Acquire( VkSemaphore signal, uint32_t& outImageIndex );
    /** Tags the present with the next present id when present-wait is available. */
    VkResult Present( VkQueue queue, std::mutex& queueMutex, uint32_t imageIndex );
    /** Blocks until present `presentId` (or a later one) is on screen; VK_TIMEOUT after timeoutNs. */
    VkResult WaitForPresent( uint64_t presentId, uint64_t timeoutNs );

    bool        IsUsable() const { return m_Swapchain != VK_NULL_HANDLE && m_Extent.width > 0 && m_Extent.height > 0; }
    VkImage     GetImage( uint32_t index ) const { return m_Images[index]; }
    VkImageView GetView( uint32_t index ) const { return m_Views[index]; }
    VkSemaphore GetPresentSemaphore( uint32_t index ) const { return m_PresentSemaphores[index]; }
    uint32_t    GetImageCount() const { return static_cast<uint32_t>( m_Images.size() ); }
    uint32_t    GetMinImageCount() const { return m_MinImageCount; }
    VkFormat    GetFormat() const { return m_Format; }
    VkExtent2D  GetExtent() const { return m_Extent; }
    bool        IsHdr() const { return m_ColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT; }
    bool        IsVSync() const { return m_VSync; }
    bool        HasPresentWait() const { return m_PresentWait != PresentWaitMode::None; }
    /** Id of the last present the swapchain accepted; ids restart at 1 with every rebuilt swapchain. */
    uint64_t    GetLastPresentId() const { return m_LastPresentId; }

    /** Whether the next Recreate asks for an HDR10 surface format. */
    void SetHdrRequested( bool hdr ) { m_HdrRequested = hdr; }

    /** HDR10 mastering metadata (Rec.709 primaries, like D3D12). No-op without VK_EXT_hdr_metadata or in SDR. */
    void SetHdrMetadata( float maxNits, float minNits, float maxFrameAverageNits );

private:
    enum class PresentWaitMode { None, PresentWait, PresentWait2 };

    PresentWaitMode QueryPresentWaitMode() const;
    bool BuildSwapchain( INT2 size );
    void DestroyImageResources();

    VulkanDevice*    m_Device = nullptr;
    HWND             m_Window = nullptr;
    VkSurfaceKHR     m_Surface = VK_NULL_HANDLE;
    VkSwapchainKHR   m_Swapchain = VK_NULL_HANDLE;
    std::vector<VkImage>     m_Images;
    std::vector<VkImageView> m_Views;
    std::vector<VkSemaphore> m_PresentSemaphores;
    VkFormat         m_Format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR  m_ColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR m_PresentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D       m_Extent = {};
    uint32_t         m_RequestedMinImages = 2;
    uint32_t         m_MinImageCount = 2;
    bool             m_VSync = true;
    bool             m_HdrRequested = false;
    PresentWaitMode  m_PresentWait = PresentWaitMode::None;
    uint64_t         m_PresentId = 0;       // last id handed to vkQueuePresentKHR
    uint64_t         m_LastPresentId = 0;   // last id whose present was accepted
};
