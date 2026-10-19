#pragma once
#include "VulkanCommon.h"
#include "../Types.h"
#include <mutex>
#include <vector>

class VulkanDevice;

/** Win32 surface + swapchain on Gothic's window.

    Presentation needs one "render finished" semaphore per swapchain IMAGE, not per frame in flight: a
    semaphore handed to vkQueuePresentKHR is only known to be free again once that image is re-acquired.
    A rebuilt swapchain's predecessor (and its semaphores) lives on until its presents are done: present
    fences with swapchain_maintenance1, else a fixed number of presents later.
    Extent follows the window's client area (Win32 requires currentExtent), so a minimized window has a
    zero extent and IsUsable() stays false until it is restored. */
class VulkanSwapchain {
public:
    VulkanSwapchain() = default;
    ~VulkanSwapchain() { Destroy(); }
    VulkanSwapchain( const VulkanSwapchain& ) = delete;
    VulkanSwapchain& operator=( const VulkanSwapchain& ) = delete;

    /** sdrFormat is the SDR image format; hdr requests HDR10 (A2B10G10R10 + ST.2084), falling back to SDR. */
    bool Create( VulkanDevice& device, HWND window, INT2 size, uint32_t minImageCount, bool vsync, VkFormat sdrFormat, bool hdr );
    /** Waits for the device to go idle, then rebuilds at `size` (the window's client area wins on Win32). */
    bool Recreate( INT2 size, bool vsync );
    void Destroy();

    /** VK_SUCCESS / VK_SUBOPTIMAL_KHR hand out an image; VK_ERROR_OUT_OF_DATE_KHR means Recreate first. */
    VkResult Acquire( VkSemaphore signal, uint32_t& outImageIndex );
    /** Tags the present with the next present id when present-wait is available. */
    VkResult Present( VkQueue queue, std::mutex& queueMutex, uint32_t imageIndex );
    /** Switches between FIFO and uncapped presents; false means this swapchain can't, so it needs a Recreate. */
    bool SetVSync( bool vsync );
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

    struct Retired {
        VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
        std::vector<VkSemaphore> Semaphores;
        uint64_t PresentCount = 0;   // m_PresentCount when it was retired
    };
    struct PresentFence { VkFence Fence = VK_NULL_HANDLE; VkSwapchainKHR Swapchain = VK_NULL_HANDLE; };
    /** Presents of the successor before a retired swapchain is destroyed when there are no present fences. */
    static constexpr uint64_t kRetirePresents = 8;

    PresentWaitMode QueryPresentWaitMode() const;
    /** Per-mode surface caps; `compatible` gets the modes a swapchain created with `mode` may switch to. */
    bool QueryPresentModeCaps( VkPresentModeKHR mode, VkSurfaceCapabilitiesKHR& caps, std::vector<VkPresentModeKHR>* compatible ) const;
    bool BuildSwapchain( INT2 size );
    void DestroyImageResources();
    /** Moves the current swapchain and its present semaphores to m_Retired. */
    void Retire();
    /** Recycles signalled present fences and destroys retired swapchains nothing presents from any more. */
    void CollectRetired( bool shutdown );
    VkFence TakePresentFence();

    VulkanDevice*    m_Device = nullptr;
    HWND             m_Window = nullptr;
    VkSurfaceKHR     m_Surface = VK_NULL_HANDLE;
    VkSwapchainKHR   m_Swapchain = VK_NULL_HANDLE;
    std::vector<VkImage>     m_Images;
    std::vector<VkImageView> m_Views;
    std::vector<VkSemaphore> m_PresentSemaphores;
    VkFormat         m_Format = VK_FORMAT_UNDEFINED;
    VkFormat         m_SdrFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR  m_ColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR m_PresentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR m_UncappedMode = VK_PRESENT_MODE_FIFO_KHR;   // IMMEDIATE, else MAILBOX, else FIFO
    bool             m_ModeSwitch = false;   // created with FIFO + m_UncappedMode: SetVSync switches per present
    std::vector<Retired>      m_Retired;
    std::vector<PresentFence> m_PresentFences;   // in flight
    std::vector<VkFence>      m_FreeFences;
    uint64_t         m_PresentCount = 0;     // every present, across rebuilds
    VkExtent2D       m_Extent = {};
    uint32_t         m_RequestedMinImages = 2;
    uint32_t         m_MinImageCount = 2;
    bool             m_VSync = true;
    bool             m_HdrRequested = false;
    PresentWaitMode  m_PresentWait = PresentWaitMode::None;
    uint64_t         m_PresentId = 0;       // last id handed to vkQueuePresentKHR
    uint64_t         m_LastPresentId = 0;   // last id whose present was accepted
};
