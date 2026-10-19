#include "../pch.h"
#include "VulkanSwapchain.h"
#include "VulkanDevice.h"

#include <algorithm>

namespace {
    const char* PresentModeName( VkPresentModeKHR mode ) {
        switch ( mode ) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default: return "other";
        }
    }
}

VulkanSwapchain::PresentWaitMode VulkanSwapchain::QueryPresentWaitMode() const {
    const VulkanDeviceCaps& caps = m_Device->GetCaps();
    if ( caps.PresentWait2 ) {
        // present_wait2 is per surface; present_wait (v1) assumes every surface supports it.
        VkSurfaceCapabilitiesPresentWait2KHR wait2 = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR };
        VkSurfaceCapabilitiesPresentId2KHR id2 = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR, &wait2 };
        VkSurfaceCapabilities2KHR caps2 = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, &id2 };
        VkPhysicalDeviceSurfaceInfo2KHR info = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR };
        info.surface = m_Surface;
        if ( vkGetPhysicalDeviceSurfaceCapabilities2KHR( m_Device->GetPhysicalDevice(), &info, &caps2 ) == VK_SUCCESS
            && id2.presentId2Supported && wait2.presentWait2Supported ) {
            return PresentWaitMode::PresentWait2;
        }
    }
    return caps.PresentWait ? PresentWaitMode::PresentWait : PresentWaitMode::None;
}

bool VulkanSwapchain::Create( VulkanDevice& device, HWND window, INT2 size, uint32_t minImageCount, bool vsync, VkFormat sdrFormat, bool hdr ) {
    m_Device = &device;
    m_SdrFormat = sdrFormat;
    m_Window = window;
    m_RequestedMinImages = std::max( 2u, minImageCount );
    m_VSync = vsync;
    m_HdrRequested = hdr;

    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = GetModuleHandleA( nullptr );
    sci.hwnd = window;
    if ( VkUtil::Failed( vkCreateWin32SurfaceKHR( device.GetInstance(), &sci, nullptr, &m_Surface ), "vkCreateWin32SurfaceKHR" ) ) {
        m_Surface = VK_NULL_HANDLE;
        return false;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR( device.GetPhysicalDevice(), device.GetGraphicsQueueFamily(), m_Surface, &supported );
    if ( !supported ) {
        Logging::Wrn( "Vulkan: the graphics queue cannot present to Gothic's window." );
        return false;
    }
    m_PresentWait = QueryPresentWaitMode();
    return BuildSwapchain( size );
}

bool VulkanSwapchain::Recreate( INT2 size, bool vsync ) {
    if ( !m_Device || !m_Surface ) return false;
    m_Device->WaitIdle();
    m_VSync = vsync;
    return BuildSwapchain( size );
}

void VulkanSwapchain::DestroyImageResources() {
    VkDevice device = m_Device ? m_Device->GetDevice() : VK_NULL_HANDLE;
    if ( !device ) return;
    // Views are only used by our command buffers (idle by now); present semaphores go with Retire().
    for ( VkImageView view : m_Views ) vkDestroyImageView( device, view, nullptr );
    m_Views.clear();
    m_Images.clear();
}

void VulkanSwapchain::Retire() {
    if ( !m_Swapchain && m_PresentSemaphores.empty() ) return;
    m_Retired.push_back( { m_Swapchain, std::move( m_PresentSemaphores ), m_PresentCount } );
    m_PresentSemaphores.clear();
    m_Swapchain = VK_NULL_HANDLE;
}

void VulkanSwapchain::CollectRetired( bool shutdown ) {
    VkDevice device = m_Device->GetDevice();
    if ( shutdown && !m_PresentFences.empty() ) {
        std::vector<VkFence> fences;
        for ( const PresentFence& p : m_PresentFences ) fences.push_back( p.Fence );
        // Bounded: a present on a lost device must not hang shutdown.
        vkWaitForFences( device, static_cast<uint32_t>( fences.size() ), fences.data(), VK_TRUE, 1000000000ull );
    }
    for ( size_t i = 0; i < m_PresentFences.size(); ) {
        if ( vkGetFenceStatus( device, m_PresentFences[i].Fence ) == VK_SUCCESS ) {
            vkResetFences( device, 1, &m_PresentFences[i].Fence );
            m_FreeFences.push_back( m_PresentFences[i].Fence );
            m_PresentFences[i] = m_PresentFences.back();
            m_PresentFences.pop_back();
        } else {
            ++i;
        }
    }
    const bool fenced = m_Device->GetCaps().SwapchainMaintenance1;
    for ( size_t i = 0; i < m_Retired.size(); ) {
        const Retired& r = m_Retired[i];
        const bool presenting = fenced
            ? std::any_of( m_PresentFences.begin(), m_PresentFences.end(), [&]( const PresentFence& p ) { return p.Swapchain == r.Swapchain; } )
            : m_PresentCount < r.PresentCount + kRetirePresents;
        if ( presenting && !shutdown ) {
            ++i;
            continue;
        }
        for ( VkSemaphore s : r.Semaphores ) vkDestroySemaphore( device, s, nullptr );
        if ( r.Swapchain ) vkDestroySwapchainKHR( device, r.Swapchain, nullptr );
        m_Retired.erase( m_Retired.begin() + i );
    }
}

VkFence VulkanSwapchain::TakePresentFence() {
    if ( !m_FreeFences.empty() ) {
        const VkFence fence = m_FreeFences.back();
        m_FreeFences.pop_back();
        return fence;
    }
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    if ( VkUtil::Failed( vkCreateFence( m_Device->GetDevice(), &fci, nullptr, &fence ), "vkCreateFence (present)" ) ) return VK_NULL_HANDLE;
    return fence;
}

void VulkanSwapchain::Destroy() {
    if ( !m_Device ) return;
    m_Device->WaitIdle();
    DestroyImageResources();
    Retire();
    CollectRetired( true );
    // Still unsignalled after the bounded wait only if the device is lost.
    for ( const PresentFence& p : m_PresentFences ) vkDestroyFence( m_Device->GetDevice(), p.Fence, nullptr );
    for ( VkFence f : m_FreeFences ) vkDestroyFence( m_Device->GetDevice(), f, nullptr );
    m_PresentFences.clear();
    m_FreeFences.clear();
    if ( m_Surface ) vkDestroySurfaceKHR( m_Device->GetInstance(), m_Surface, nullptr );
    m_Surface = VK_NULL_HANDLE;
    m_Device = nullptr;
}

bool VulkanSwapchain::QueryPresentModeCaps( VkPresentModeKHR mode, VkSurfaceCapabilitiesKHR& caps,
    std::vector<VkPresentModeKHR>* compatible ) const {
    VkSurfacePresentModeKHR modeInfo = { VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR, nullptr, mode };
    VkPhysicalDeviceSurfaceInfo2KHR info = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, &modeInfo, m_Surface };
    VkSurfacePresentModeCompatibilityKHR compat = { VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR };
    VkSurfaceCapabilities2KHR caps2 = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, compatible ? &compat : nullptr };
    VkPhysicalDevice gpu = m_Device->GetPhysicalDevice();
    if ( vkGetPhysicalDeviceSurfaceCapabilities2KHR( gpu, &info, &caps2 ) != VK_SUCCESS ) return false;
    if ( compatible ) {
        compatible->resize( compat.presentModeCount );
        compat.pPresentModes = compatible->data();
        if ( vkGetPhysicalDeviceSurfaceCapabilities2KHR( gpu, &info, &caps2 ) != VK_SUCCESS ) return false;
        compatible->resize( compat.presentModeCount );
    }
    caps = caps2.surfaceCapabilities;
    return true;
}

bool VulkanSwapchain::BuildSwapchain( INT2 size ) {
    VkPhysicalDevice gpu = m_Device->GetPhysicalDevice();
    VkDevice device = m_Device->GetDevice();

    VkSurfaceCapabilitiesKHR caps = {};
    if ( VkUtil::Failed( vkGetPhysicalDeviceSurfaceCapabilitiesKHR( gpu, m_Surface, &caps ), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR" ) )
        return false;

    VkExtent2D extent = caps.currentExtent;
    if ( extent.width == UINT32_MAX ) {
        extent.width = std::clamp<uint32_t>( static_cast<uint32_t>( std::max( 1, size.x ) ), caps.minImageExtent.width, caps.maxImageExtent.width );
        extent.height = std::clamp<uint32_t>( static_cast<uint32_t>( std::max( 1, size.y ) ), caps.minImageExtent.height, caps.maxImageExtent.height );
    }
    DestroyImageResources();
    if ( extent.width == 0 || extent.height == 0 ) {
        // Minimized: keep the old swapchain handle as oldSwapchain for the next attempt, draw nothing.
        m_Extent = extent;
        return true;
    }

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR( gpu, m_Surface, &count, nullptr );
    std::vector<VkSurfaceFormatKHR> formats( count );
    vkGetPhysicalDeviceSurfaceFormatsKHR( gpu, m_Surface, &count, formats.data() );
    auto has = [&]( VkFormat f, VkColorSpaceKHR cs ) {
        return std::any_of( formats.begin(), formats.end(),
            [&]( const VkSurfaceFormatKHR& s ) { return s.format == f && s.colorSpace == cs; } );
    };
    // SDR: the caller's UNORM format (the shaders write gamma-encoded values themselves), else BGRA8.
    if ( m_HdrRequested && m_Device->GetCaps().SwapchainColorSpace
        && has( VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT ) ) {
        m_Format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        m_ColorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    } else if ( has( m_SdrFormat, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ) ) {
        m_Format = m_SdrFormat;
        m_ColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    } else if ( has( VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ) ) {
        m_Format = VK_FORMAT_B8G8R8A8_UNORM;
        m_ColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    } else if ( !formats.empty() ) {
        m_Format = formats[0].format;
        m_ColorSpace = formats[0].colorSpace;
    } else {
        Logging::Wrn( "Vulkan: the surface reports no formats." );
        return false;
    }
    if ( m_HdrRequested && !IsHdr() )
        Logging::Wrn( "Vulkan: HDR output requested but the surface offers no HDR10 format; using SDR." );

    vkGetPhysicalDeviceSurfacePresentModesKHR( gpu, m_Surface, &count, nullptr );
    std::vector<VkPresentModeKHR> modes( count );
    vkGetPhysicalDeviceSurfacePresentModesKHR( gpu, m_Surface, &count, modes.data() );
    auto hasMode = [&]( VkPresentModeKHR m ) { return std::find( modes.begin(), modes.end(), m ) != modes.end(); };
    // Uncapped matches D3D12's ALLOW_TEARING first; MAILBOX is the tear-free fallback.
    m_UncappedMode = hasMode( VK_PRESENT_MODE_IMMEDIATE_KHR ) ? VK_PRESENT_MODE_IMMEDIATE_KHR
        : hasMode( VK_PRESENT_MODE_MAILBOX_KHR ) ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
    m_PresentMode = m_VSync ? VK_PRESENT_MODE_FIFO_KHR : m_UncappedMode;

    // swapchain_maintenance1: FIFO and uncapped on one swapchain, so a vsync toggle needs no rebuild.
    uint32_t minImages = caps.minImageCount, maxImages = caps.maxImageCount;
    const VkPresentModeKHR switchModes[2] = { VK_PRESENT_MODE_FIFO_KHR, m_UncappedMode };
    m_ModeSwitch = false;
    if ( m_Device->GetCaps().SwapchainMaintenance1 && m_UncappedMode != VK_PRESENT_MODE_FIFO_KHR ) {
        VkSurfaceCapabilitiesKHR fifoCaps = {}, uncappedCaps = {};
        std::vector<VkPresentModeKHR> compatible;
        if ( QueryPresentModeCaps( VK_PRESENT_MODE_FIFO_KHR, fifoCaps, &compatible )
            && std::find( compatible.begin(), compatible.end(), m_UncappedMode ) != compatible.end()
            && QueryPresentModeCaps( m_UncappedMode, uncappedCaps, nullptr ) ) {
            m_ModeSwitch = true;
            minImages = std::max( fifoCaps.minImageCount, uncappedCaps.minImageCount );
            maxImages = !fifoCaps.maxImageCount ? uncappedCaps.maxImageCount : !uncappedCaps.maxImageCount ? fifoCaps.maxImageCount
                : std::min( fifoCaps.maxImageCount, uncappedCaps.maxImageCount );
        }
    }

    m_MinImageCount = std::max( minImages, m_RequestedMinImages );
    if ( maxImages > 0 ) m_MinImageCount = std::min( m_MinImageCount, maxImages );

    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = m_Surface;
    ci.minImageCount = m_MinImageCount;
    ci.imageFormat = m_Format;
    ci.imageColorSpace = m_ColorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = ( caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR )
        ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for ( VkCompositeAlphaFlagBitsKHR a : { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR } ) {
        if ( caps.supportedCompositeAlpha & a ) { ci.compositeAlpha = a; break; }
    }
    ci.presentMode = m_PresentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = m_Swapchain;
    if ( m_PresentWait == PresentWaitMode::PresentWait2 )
        ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
    const VkSwapchainPresentModesCreateInfoKHR modesCi = { VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR, nullptr, 2, switchModes };
    if ( m_ModeSwitch ) ci.pNext = &modesCi;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR( device, &ci, nullptr, &swapchain );
    Retire();   // oldSwapchain is retired either way; destroyed once its presents are done
    CollectRetired( false );
    m_PresentId = 0;
    m_LastPresentId = 0;
    if ( VkUtil::Failed( r, "vkCreateSwapchainKHR" ) ) return false;
    m_Swapchain = swapchain;
    m_Extent = extent;

    vkGetSwapchainImagesKHR( device, m_Swapchain, &count, nullptr );
    m_Images.resize( count );
    vkGetSwapchainImagesKHR( device, m_Swapchain, &count, m_Images.data() );
    for ( uint32_t i = 0; i < count; ++i ) {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = m_Images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = m_Format;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        if ( VkUtil::Failed( vkCreateImageView( device, &vci, nullptr, &view ), "vkCreateImageView (swapchain)" ) ) return false;
        m_Views.push_back( view );

        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if ( VkUtil::Failed( vkCreateSemaphore( device, &sci, nullptr, &semaphore ), "vkCreateSemaphore (present)" ) ) return false;
        m_PresentSemaphores.push_back( semaphore );

        const std::string name = "BackBuffer" + std::to_string( i );
        m_Device->SetObjectName( VK_OBJECT_TYPE_IMAGE, VkUtil::HandleToU64( m_Images[i] ), name.c_str() );
    }

    Logging::Inf( "Vulkan: swapchain {}x{}, {} images, format {}{}, present mode {}{}, frame pacing on {}, {} retired pending.",
        extent.width, extent.height, count, static_cast<int>( m_Format ), IsHdr() ? " (HDR10 ST.2084)" : "",
        PresentModeName( m_PresentMode ), m_ModeSwitch ? " (switchable)" : "",
        !m_VSync || m_PresentWait == PresentWaitMode::None ? "GPU completion"
            : m_PresentWait == PresentWaitMode::PresentWait2 ? "present_wait2" : "present_wait",
        m_Retired.size() );
    return true;
}

VkResult VulkanSwapchain::Acquire( VkSemaphore signal, uint32_t& outImageIndex ) {
    if ( !IsUsable() ) return VK_ERROR_OUT_OF_DATE_KHR;
    return vkAcquireNextImageKHR( m_Device->GetDevice(), m_Swapchain, UINT64_MAX, signal, VK_NULL_HANDLE, &outImageIndex );
}

VkResult VulkanSwapchain::Present( VkQueue queue, std::mutex& queueMutex, uint32_t imageIndex ) {
    CollectRetired( false );
    const void* chain = nullptr;
    const VkSwapchainPresentModeInfoKHR modeInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR, nullptr, 1, &m_PresentMode };
    if ( m_ModeSwitch ) chain = &modeInfo;
    const VkFence fence = m_Device->GetCaps().SwapchainMaintenance1 ? TakePresentFence() : VK_NULL_HANDLE;
    const VkSwapchainPresentFenceInfoKHR fenceInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR, chain, 1, &fence };
    if ( fence ) chain = &fenceInfo;

    const uint64_t presentId = m_PresentId + 1;
    VkPresentIdKHR id = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR, chain, 1, &presentId };
    VkPresentId2KHR id2 = { VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, chain, 1, &presentId };
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, chain };
    if ( m_PresentWait == PresentWaitMode::PresentWait ) pi.pNext = &id;
    else if ( m_PresentWait == PresentWaitMode::PresentWait2 ) pi.pNext = &id2;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &m_PresentSemaphores[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &m_Swapchain;
    pi.pImageIndices = &imageIndex;
    VkResult r;
    {
        std::lock_guard<std::mutex> lock( queueMutex );
        r = vkQueuePresentKHR( queue, &pi );
    }
    // Tracked even on failure: OUT_OF_DATE / SURFACE_LOST presents still signal; Destroy bounds the rest.
    if ( fence ) m_PresentFences.push_back( { fence, m_Swapchain } );
    ++m_PresentCount;
    // Ids must keep increasing even past a failed present; only accepted ones can be waited on.
    m_PresentId = presentId;
    if ( r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR ) m_LastPresentId = presentId;
    return r;
}

bool VulkanSwapchain::SetVSync( bool vsync ) {
    if ( vsync == m_VSync ) return true;
    if ( !m_ModeSwitch ) return false;
    m_VSync = vsync;
    m_PresentMode = vsync ? VK_PRESENT_MODE_FIFO_KHR : m_UncappedMode;
    Logging::Inf( "Vulkan: present mode {} (switched without a rebuild).", PresentModeName( m_PresentMode ) );
    return true;
}

VkResult VulkanSwapchain::WaitForPresent( uint64_t presentId, uint64_t timeoutNs ) {
    if ( !m_Swapchain || presentId == 0 || presentId > m_LastPresentId ) return VK_SUCCESS;
    if ( m_PresentWait == PresentWaitMode::PresentWait2 ) {
        const VkPresentWait2InfoKHR wi = { VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR, nullptr, presentId, timeoutNs };
        return vkWaitForPresent2KHR( m_Device->GetDevice(), m_Swapchain, &wi );
    }
    if ( m_PresentWait == PresentWaitMode::PresentWait )
        return vkWaitForPresentKHR( m_Device->GetDevice(), m_Swapchain, presentId, timeoutNs );
    return VK_SUCCESS;
}

void VulkanSwapchain::SetHdrMetadata( float maxNits, float minNits, float maxFrameAverageNits ) {
    if ( !IsHdr() || !m_Device->GetCaps().HdrMetadata ) return;
    VkHdrMetadataEXT meta = { VK_STRUCTURE_TYPE_HDR_METADATA_EXT };
    meta.displayPrimaryRed = { 0.640f, 0.330f };
    meta.displayPrimaryGreen = { 0.300f, 0.600f };
    meta.displayPrimaryBlue = { 0.150f, 0.060f };
    meta.whitePoint = { 0.3127f, 0.3290f };
    meta.maxLuminance = maxNits;
    meta.minLuminance = std::max( 0.0f, minNits );
    meta.maxContentLightLevel = maxNits;
    meta.maxFrameAverageLightLevel = maxFrameAverageNits;
    vkSetHdrMetadataEXT( m_Device->GetDevice(), 1, &m_Swapchain, &meta );
}
