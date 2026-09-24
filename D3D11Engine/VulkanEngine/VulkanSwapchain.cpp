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

bool VulkanSwapchain::Create( VulkanDevice& device, HWND window, INT2 size, uint32_t minImageCount, bool vsync, bool hdr ) {
    m_Device = &device;
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
    for ( VkImageView view : m_Views ) vkDestroyImageView( device, view, nullptr );
    for ( VkSemaphore s : m_PresentSemaphores ) vkDestroySemaphore( device, s, nullptr );
    m_Views.clear();
    m_PresentSemaphores.clear();
    m_Images.clear();
}

void VulkanSwapchain::Destroy() {
    if ( !m_Device ) return;
    m_Device->WaitIdle();
    DestroyImageResources();
    if ( m_Swapchain ) vkDestroySwapchainKHR( m_Device->GetDevice(), m_Swapchain, nullptr );
    if ( m_Surface ) vkDestroySurfaceKHR( m_Device->GetInstance(), m_Surface, nullptr );
    m_Swapchain = VK_NULL_HANDLE;
    m_Surface = VK_NULL_HANDLE;
    m_Device = nullptr;
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
    // SDR: R10G10B10A2 like D3D12's backbuffer, UNORM (the shaders write gamma-encoded values themselves).
    if ( m_HdrRequested && m_Device->GetCaps().SwapchainColorSpace
        && has( VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT ) ) {
        m_Format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        m_ColorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    } else if ( has( VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ) ) {
        m_Format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
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
    m_PresentMode = VK_PRESENT_MODE_FIFO_KHR;
    if ( !m_VSync ) {
        if ( hasMode( VK_PRESENT_MODE_IMMEDIATE_KHR ) ) m_PresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if ( hasMode( VK_PRESENT_MODE_MAILBOX_KHR ) ) m_PresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    m_MinImageCount = std::max( caps.minImageCount, m_RequestedMinImages );
    if ( caps.maxImageCount > 0 ) m_MinImageCount = std::min( m_MinImageCount, caps.maxImageCount );

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

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR( device, &ci, nullptr, &swapchain );
    if ( m_Swapchain ) vkDestroySwapchainKHR( device, m_Swapchain, nullptr );   // retired either way
    m_Swapchain = VK_NULL_HANDLE;
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

    Logging::Inf( "Vulkan: swapchain {}x{}, {} images, format {}{}, present mode {}.", extent.width, extent.height, count,
        static_cast<int>( m_Format ), IsHdr() ? " (HDR10 ST.2084)" : "", PresentModeName( m_PresentMode ) );
    return true;
}

VkResult VulkanSwapchain::Acquire( VkSemaphore signal, uint32_t& outImageIndex ) {
    if ( !IsUsable() ) return VK_ERROR_OUT_OF_DATE_KHR;
    return vkAcquireNextImageKHR( m_Device->GetDevice(), m_Swapchain, UINT64_MAX, signal, VK_NULL_HANDLE, &outImageIndex );
}

VkResult VulkanSwapchain::Present( VkQueue queue, std::mutex& queueMutex, uint32_t imageIndex ) {
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &m_PresentSemaphores[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &m_Swapchain;
    pi.pImageIndices = &imageIndex;
    std::lock_guard<std::mutex> lock( queueMutex );
    return vkQueuePresentKHR( queue, &pi );
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
