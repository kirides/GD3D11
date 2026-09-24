#include "../pch.h"
#include "VulkanGraphicsEngine.h"
#include "VulkanNullResources.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../ImGuiShim.h"
#include "../DXGIHelpers.h"

#include <algorithm>

namespace {
    constexpr uint64_t kTimelineWaitTimeoutNs = 2'000'000'000ull;

    void ImageBarrier( VkCommandBuffer cmd, VkImage image,
        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkImageLayout oldLayout,
        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, VkImageLayout newLayout ) {
        VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = srcStage;
        barrier.srcAccessMask = srcAccess;
        barrier.dstStageMask = dstStage;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2( cmd, &dep );
    }

    bool FormatSupports( VkPhysicalDevice gpu, VkFormat format, VkFormatFeatureFlags features ) {
        VkFormatProperties props = {};
        vkGetPhysicalDeviceFormatProperties( gpu, format, &props );
        return ( props.optimalTilingFeatures & features ) == features;
    }
}

VulkanGraphicsEngine::VulkanGraphicsEngine() {
    m_LineRenderer = std::make_unique<VulkanNullLineRenderer>();
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    m_BackbufferResolution = m_NewResolution = settings.LoadedResolution;
    // Same LowLatency split D3D12 uses for its frame count; switching it needs a restart.
    m_FramesInFlight = settings.LowLatency ? 2 : 3;
}

VulkanGraphicsEngine::~VulkanGraphicsEngine() {
    m_Device.WaitIdle();
    m_Swapchain.Destroy();
    DestroyFrameResources();
}

XRESULT VulkanGraphicsEngine::Init() {
    if ( !m_Device.Init() ) {
        Logging::Err( "VulkanGraphicsEngine::Init: device creation failed." );
        return XR_FAILED;
    }

    const VulkanDeviceCaps& caps = m_Device.GetCaps();
    VkPhysicalDevice gpu = m_Device.GetPhysicalDevice();
    m_DeviceCapabilities.DeviceDescription = m_Device.GetDeviceDescription();
    m_DeviceCapabilities.VendorId = caps.VendorId;
    m_DeviceCapabilities.DriverExtensions = false;
    m_DeviceCapabilities.MultiDrawIndirect = true;       // drawIndirectCount is a requirement
    m_DeviceCapabilities.UAVOverlap = true;              // barriers are explicit
    m_DeviceCapabilities.LayeredRendering = true;        // shaderOutputLayer is a requirement
    m_DeviceCapabilities.BindlessResources = true;
    m_DeviceCapabilities.Native16BitTextures =
        FormatSupports( gpu, VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT )
        && FormatSupports( gpu, VK_FORMAT_A1R5G5B5_UNORM_PACK16, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT )
        && FormatSupports( gpu, VK_FORMAT_A4R4G4B4_UNORM_PACK16, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT );
    m_DeviceCapabilities.TypedUAVLoadAdditionalFormats =
        FormatSupports( gpu, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT );
    Engine::GAPI->GetRendererState().RendererSettings.ApplyDeviceCapabilities( m_DeviceCapabilities );

    if ( !CreateFrameResources() ) {
        Logging::Err( "VulkanGraphicsEngine::Init: failed to create the per-frame command objects." );
        return XR_FAILED;
    }
    return XR_SUCCESS;
}

bool VulkanGraphicsEngine::CreateFrameResources() {
    VkDevice device = m_Device.GetDevice();

    VkSemaphoreTypeCreateInfo timelineInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo timelineCi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    timelineCi.pNext = &timelineInfo;
    if ( VkUtil::Failed( vkCreateSemaphore( device, &timelineCi, nullptr, &m_FrameTimeline ), "vkCreateSemaphore (timeline)" ) )
        return false;
    m_Device.SetObjectName( VK_OBJECT_TYPE_SEMAPHORE, VkUtil::HandleToU64( m_FrameTimeline ), "FrameTimeline" );

    for ( uint32_t i = 0; i < m_FramesInFlight; ++i ) {
        FrameContext& frame = m_Frames[i];
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = m_Device.GetGraphicsQueueFamily();
        if ( VkUtil::Failed( vkCreateCommandPool( device, &pci, nullptr, &frame.Pool ), "vkCreateCommandPool" ) ) return false;

        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = frame.Pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if ( VkUtil::Failed( vkAllocateCommandBuffers( device, &ai, &frame.Cmd ), "vkAllocateCommandBuffers" ) ) return false;

        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if ( VkUtil::Failed( vkCreateSemaphore( device, &sci, nullptr, &frame.AcquireSemaphore ), "vkCreateSemaphore (acquire)" ) )
            return false;
    }
    return true;
}

void VulkanGraphicsEngine::DestroyFrameResources() {
    VkDevice device = m_Device.GetDevice();
    if ( !device ) return;
    for ( FrameContext& frame : m_Frames ) {
        if ( frame.Pool ) vkDestroyCommandPool( device, frame.Pool, nullptr );
        if ( frame.AcquireSemaphore ) vkDestroySemaphore( device, frame.AcquireSemaphore, nullptr );
        frame = FrameContext{};
    }
    if ( m_FrameTimeline ) vkDestroySemaphore( device, m_FrameTimeline, nullptr );
    m_FrameTimeline = VK_NULL_HANDLE;
}

XRESULT VulkanGraphicsEngine::SetWindow( HWND hWnd ) {
    Logging::Inf( "Vulkan: creating the swapchain." );
    CommonSetWindow( hWnd );

    // The configured resolution, not the client rect: Gothic creates its window tiny (see D3D12's SetWindow).
    INT2 size = Engine::GAPI->GetRendererState().RendererSettings.LoadedResolution;
    if ( size.x <= 0 || size.y <= 0 ) {
        RECT rc = {};
        GetClientRect( hWnd, &rc );
        size = INT2( std::max<int>( 800, rc.right - rc.left ), std::max<int>( 600, rc.bottom - rc.top ) );
    }
    m_NewResolution = size;
    return OnResize( size );
}

void VulkanGraphicsEngine::DetectHdrOutput() {
    m_HdrOutputActive = false;
    if ( !Engine::GAPI->GetRendererState().RendererSettings.HDR_Monitor ) return;
    const VulkanDeviceCaps& caps = m_Device.GetCaps();
    if ( !caps.HasLuid || !caps.SwapchainColorSpace ) {
        Logging::Inf( "Vulkan: HDR output requested, but the driver can't report the adapter LUID or HDR colour spaces; using SDR." );
        return;
    }
    m_HdrOutputActive = DXGI_QueryHdrOutput( caps.Luid, m_OutputWindow, m_HdrMaxNits, m_HdrMinNits, m_HdrMaxFullFrameNits );
    if ( m_HdrOutputActive ) {
        Logging::Inf( "Vulkan: HDR output enabled. Monitor reports {} nits peak, {} nits full-frame, {} nits black.",
            m_HdrMaxNits, m_HdrMaxFullFrameNits, m_HdrMinNits );
    } else {
        Logging::Inf( "Vulkan: HDR output requested but Windows HDR is off on this display; using SDR." );
    }
}

bool VulkanGraphicsEngine::GetHdrOutputInfo( float& maxNits, float& minNits, float& maxFullFrameNits ) const {
    if ( !m_HdrOutputActive || !m_Swapchain.IsHdr() ) return false;
    maxNits = m_HdrMaxNits;
    minNits = m_HdrMinNits;
    maxFullFrameNits = m_HdrMaxFullFrameNits;
    return true;
}

bool VulkanGraphicsEngine::CreateOrResizeSwapchain( INT2 size ) {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const bool firstCreation = !m_SwapchainReady;
    if ( firstCreation ) {
        DetectHdrOutput();
        if ( !m_Swapchain.Create( m_Device, m_OutputWindow, size, m_FramesInFlight, settings.EnableVSync, m_HdrOutputActive ) ) {
            Logging::Err( "Vulkan: swapchain creation failed." );
            return false;
        }
        m_SwapchainReady = true;
    } else if ( !m_Swapchain.Recreate( size, settings.EnableVSync ) ) {
        Logging::Err( "Vulkan: swapchain re-creation failed ({}x{}).", size.x, size.y );
        return false;
    }
    m_SwapchainDirty = false;
    if ( !m_Swapchain.IsUsable() ) return true;   // minimized: retried every frame until restored

    const VkExtent2D extent = m_Swapchain.GetExtent();
    m_BackbufferResolution = INT2( static_cast<int>( extent.width ), static_cast<int>( extent.height ) );

    if ( m_Swapchain.IsHdr() ) {
        const float maxNits = settings.HDR_AutoMaxBrightness && m_HdrMaxNits > 0.0f ? m_HdrMaxNits : settings.HDR_MaxBrightness;
        m_Swapchain.SetHdrMetadata( maxNits, m_HdrMinNits, std::min( maxNits, settings.HDR_PaperWhite ) );
    }

    if ( Engine::ImGuiHandle && !Engine::ImGuiHandle->Initiated ) {
        Engine::ImGuiHandle->InitVulkan( m_OutputWindow, m_Device, static_cast<int>( m_Swapchain.GetFormat() ),
            m_Swapchain.GetMinImageCount(), std::max( m_Swapchain.GetImageCount(), m_Swapchain.GetMinImageCount() ) );
    } else if ( Engine::ImGuiHandle ) {
        Engine::ImGuiHandle->SetVulkanMinImageCount( m_Swapchain.GetMinImageCount() );
    }
    if ( firstCreation ) VkUtil::LogAddressSpace( "after swapchain + ImGui creation" );
    return true;
}

XRESULT VulkanGraphicsEngine::OnResize( INT2 newSize ) {
    if ( newSize.x <= 0 || newSize.y <= 0 ) return XR_SUCCESS;

    // Never exceed the desktop, whatever asked for the size (ini, -zRes, ImGui) - same clamp as D3D12.
    const int maxWidth = GetSystemMetrics( SM_CXSCREEN );
    const int maxHeight = GetSystemMetrics( SM_CYSCREEN );
    if ( maxWidth > 0 && maxHeight > 0 && ( newSize.x > maxWidth || newSize.y > maxHeight ) ) {
        Logging::Wrn( "VulkanGraphicsEngine::OnResize: requested {}x{} exceeds the desktop resolution ({}x{}) — clamping.",
            newSize.x, newSize.y, maxWidth, maxHeight );
        newSize.x = std::min( newSize.x, maxWidth );
        newSize.y = std::min( newSize.y, maxHeight );
        m_NewResolution = newSize;
    }
    if ( m_SwapchainReady && newSize.x == m_BackbufferResolution.x && newSize.y == m_BackbufferResolution.y )
        return XR_SUCCESS;

    ResizeOutputWindow( newSize );
    if ( !CreateOrResizeSwapchain( newSize ) ) {
        m_NewResolution = m_BackbufferResolution;
        return XR_FAILED;
    }
    m_NewResolution = m_BackbufferResolution;   // the window's client area may have overruled the request

    if ( Engine::ImGuiHandle && Engine::ImGuiHandle->Initiated ) {
        Engine::ImGuiHandle->OnResize( m_BackbufferResolution );
    }
    return XR_SUCCESS;
}

XRESULT VulkanGraphicsEngine::TriggerResize( INT2 resolution ) {
    // Applied at the top of the next OnBeginFrame, never mid-frame (same contract as D3D12).
    m_NewResolution = resolution;
    return XR_SUCCESS;
}

void VulkanGraphicsEngine::WaitForTimeline( uint64_t value, const char* site ) {
    if ( value == 0 || !m_FrameTimeline ) return;
    VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &m_FrameTimeline;
    wi.pValues = &value;
    for ( uint32_t round = 1; ; ++round ) {
        const VkResult r = vkWaitSemaphores( m_Device.GetDevice(), &wi, kTimelineWaitTimeoutNs );
        if ( r == VK_SUCCESS ) return;
        if ( r != VK_TIMEOUT ) HandleDeviceLost( site );
        uint64_t completed = 0;
        vkGetSemaphoreCounterValue( m_Device.GetDevice(), m_FrameTimeline, &completed );
        Logging::Wrn( "Vulkan: {} has been waiting {}s for frame value {} (completed {}).", site, round * 2, value, completed );
    }
}

void VulkanGraphicsEngine::HandleDeviceLost( const char* context ) {
    Logging::Err( "Vulkan: device lost in {}.", context );
    VkDevice device = m_Device.GetDevice();
    if ( device && m_Device.GetCaps().DeviceFault ) {
        VkDeviceFaultCountsEXT counts = { VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
        if ( vkGetDeviceFaultInfoEXT( device, &counts, nullptr ) >= 0 ) {
            std::vector<VkDeviceFaultAddressInfoEXT> addresses( counts.addressInfoCount );
            std::vector<VkDeviceFaultVendorInfoEXT> vendor( counts.vendorInfoCount );
            counts.vendorBinarySize = 0;
            VkDeviceFaultInfoEXT info = { VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
            info.pAddressInfos = addresses.data();
            info.pVendorInfos = vendor.data();
            if ( vkGetDeviceFaultInfoEXT( device, &counts, &info ) >= 0 ) {
                Logging::Err( "Vulkan device fault: {}", info.description );
                for ( const auto& a : addresses )
                    Logging::Err( "  address type {} at 0x{:016X} (precision 0x{:X})", static_cast<int>( a.addressType ),
                        a.reportedAddress, a.addressPrecision );
                for ( const auto& v : vendor )
                    Logging::Err( "  vendor fault 0x{:X} (data 0x{:X}): {}", v.vendorFaultCode, v.vendorFaultData, v.description );
            }
        }
    }
    const std::string msg = std::format( "The Vulkan device was lost ({}). See Log.txt for details.", context );
    MessageBoxA( NULL, msg.c_str(), "GD3D11 (Vulkan): Device Lost", MB_OK );
    exit( static_cast<int>( VK_ERROR_DEVICE_LOST ) );
}

XRESULT VulkanGraphicsEngine::OnBeginFrame() {
    if ( !m_SwapchainReady ) return XR_SUCCESS;

    if ( !g_MainLoopFramePacingInstalled ) {
        FrameMark;
        FrameLimiterBeginFrame();
    } else if ( Engine::GAPI->IsIngameMenuPaused() ) {
        FrameMark;
    }
    PausedFrameLimiterBeginFrame();

    // Pending resize, vsync toggle, out-of-date or minimized swapchain: the previous frame is submitted, so
    // this is the one place a device-idle rebuild can't disturb an open recording.
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( m_NewResolution.x > 0 && m_NewResolution.y > 0
        && ( m_NewResolution.x != m_BackbufferResolution.x || m_NewResolution.y != m_BackbufferResolution.y ) ) {
        OnResize( m_NewResolution );
    }
    if ( m_SwapchainDirty || !m_Swapchain.IsUsable() || settings.EnableVSync != m_Swapchain.IsVSync() ) {
        CreateOrResizeSwapchain( m_BackbufferResolution );
    }

    // Textures a Gothic loader thread finished since last frame become ready (see D3D12's OnBeginFrame).
    Engine::GAPI->EnterResourceCriticalSection();
    Engine::GAPI->SetFrameProcessedTexturesReady();
    Engine::GAPI->LeaveResourceCriticalSection();

    if ( !m_Swapchain.IsUsable() ) return XR_SUCCESS;   // minimized: nothing to draw into

    FrameContext& frame = m_Frames[m_FrameSlot];
    WaitForTimeline( frame.RetireValue, "OnBeginFrame" );

    VkResult r = m_Swapchain.Acquire( frame.AcquireSemaphore, m_ImageIndex );
    if ( r == VK_ERROR_OUT_OF_DATE_KHR ) {
        if ( !CreateOrResizeSwapchain( m_BackbufferResolution ) || !m_Swapchain.IsUsable() ) return XR_SUCCESS;
        r = m_Swapchain.Acquire( frame.AcquireSemaphore, m_ImageIndex );
    }
    if ( r == VK_ERROR_DEVICE_LOST ) HandleDeviceLost( "vkAcquireNextImageKHR" );
    if ( VkUtil::Failed( r, "vkAcquireNextImageKHR" ) ) {
        m_SwapchainDirty = true;
        return XR_SUCCESS;
    }
    if ( r == VK_SUBOPTIMAL_KHR ) m_SwapchainDirty = true;   // still presentable; rebuilt next frame

    vkResetCommandPool( m_Device.GetDevice(), frame.Pool, 0 );
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer( frame.Cmd, &bi );

    ImageBarrier( frame.Cmd, m_Swapchain.GetImage( m_ImageIndex ),
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );

    VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView = m_Swapchain.GetView( m_ImageIndex );
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    memcpy( color.clearValue.color.float32, m_ClearColor, sizeof( m_ClearColor ) );
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, m_Swapchain.GetExtent() };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering( frame.Cmd, &ri );

    ApplyZViewModeIfChanged( m_BackbufferResolution );
    m_FrameOpen = true;
    return XR_SUCCESS;
}

GraphicsEventRecord VulkanGraphicsEngine::RecordGraphicsEvent( GraphicsEventName region ) {
    if ( !m_FrameOpen || !m_Device.GetCaps().DebugUtils ) return GraphicsEventRecord{};
    VkDebugUtilsLabelEXT label = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
    label.pLabelName = region.narrow;
    vkCmdBeginDebugUtilsLabelEXT( m_Frames[m_FrameSlot].Cmd, &label );
    return GraphicsEventRecord( this, []( void* context ) {
        auto* engine = static_cast<VulkanGraphicsEngine*>( context );
        if ( engine->m_FrameOpen ) vkCmdEndDebugUtilsLabelEXT( engine->m_Frames[engine->m_FrameSlot].Cmd );
    } );
}

XRESULT VulkanGraphicsEngine::Present() {
    if ( !m_FrameOpen ) return XR_SUCCESS;
    FrameContext& frame = m_Frames[m_FrameSlot];

    if ( Engine::ImGuiHandle && Engine::ImGuiHandle->Initiated ) {
        Engine::ImGuiHandle->RenderLoopVulkan( frame.Cmd );
    }
    vkCmdEndRendering( frame.Cmd );
    ImageBarrier( frame.Cmd, m_Swapchain.GetImage( m_ImageIndex ),
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR );
    vkEndCommandBuffer( frame.Cmd );

    const uint64_t signalValue = ++m_LastSignaledValue;
    VkSemaphoreSubmitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    wait.semaphore = frame.AcquireSemaphore;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signals[2] = { { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO }, { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO } };
    signals[0].semaphore = m_Swapchain.GetPresentSemaphore( m_ImageIndex );
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].semaphore = m_FrameTimeline;
    signals[1].value = signalValue;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cmdInfo.commandBuffer = frame.Cmd;
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cmdInfo;
    si.signalSemaphoreInfoCount = 2;
    si.pSignalSemaphoreInfos = signals;
    VkResult r;
    {
        std::lock_guard<std::mutex> lock( m_Device.GetGraphicsQueueMutex() );
        r = vkQueueSubmit2( m_Device.GetGraphicsQueue(), 1, &si, VK_NULL_HANDLE );
    }
    if ( r == VK_ERROR_DEVICE_LOST ) HandleDeviceLost( "vkQueueSubmit2" );
    if ( VkUtil::Failed( r, "vkQueueSubmit2" ) ) return XR_FAILED;
    frame.RetireValue = signalValue;

    r = m_Swapchain.Present( m_Device.GetGraphicsQueue(), m_Device.GetGraphicsQueueMutex(), m_ImageIndex );
    if ( r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR ) {
        m_SwapchainDirty = true;
    } else if ( r == VK_ERROR_DEVICE_LOST ) {
        HandleDeviceLost( "vkQueuePresentKHR" );
    } else {
        VkUtil::Failed( r, "vkQueuePresentKHR" );
    }

    m_FrameSlot = ( m_FrameSlot + 1 ) % m_FramesInFlight;
    if ( ++m_FrameCounter == 600 ) VkUtil::LogAddressSpace( "after 600 frames" );
    return XR_SUCCESS;
}

XRESULT VulkanGraphicsEngine::OnEndFrame() {
    if ( !m_SwapchainReady ) return XR_SUCCESS;
    FlushUI2D();
    if ( m_FrameOpen ) Present();
    m_FrameOpen = false;

    // Paced even for skipped (minimized) frames, or a minimized game would spin a core.
    if ( !g_MainLoopFramePacingInstalled ) {
        FrameLimiterEndFrame();
    }
    PausedFrameLimiterEndFrame();
    Engine::GAPI->OnEndFrame();
    return XR_SUCCESS;
}

XRESULT VulkanGraphicsEngine::CreateVertexBuffer( std::unique_ptr<GfxVertexBuffer>& outBuffer ) {
    outBuffer = std::make_unique<VulkanNullVertexBuffer>();
    return XR_SUCCESS;
}

XRESULT VulkanGraphicsEngine::CreateTexture( GfxTexture** outTexture ) {
    if ( outTexture ) *outTexture = new VulkanNullTexture();
    return XR_SUCCESS;
}

XRESULT VulkanGraphicsEngine::CreateTexture( std::unique_ptr<GfxTexture>& outTexture ) {
    outTexture = std::make_unique<VulkanNullTexture>();
    return XR_SUCCESS;
}

XRESULT VulkanGraphicsEngine::GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling ) {
    if ( !modeList ) return XR_SUCCESS;
    modeList->clear();
    const VulkanDeviceCaps& caps = m_Device.GetCaps();
    if ( !caps.HasLuid || XR_SUCCESS != DXGI_GetDisplayModeList( caps.Luid, m_OutputWindow, &m_CachedDisplayModes ) ) {
        m_CachedDisplayModes.clear();
        m_CachedDisplayModes.push_back( DisplayModeInfo( std::max<int>( 1, m_BackbufferResolution.x ),
            std::max<int>( 1, m_BackbufferResolution.y ), 60, 1 ) );
    }
    return AppendCachedDisplayModes( modeList, includeSuperSampling );
}

void VulkanGraphicsEngine::GetBackbufferData( bool thumbnail, byte** data, INT2& buffersize, int& pixelsize ) {
    buffersize = thumbnail ? INT2( 256, 256 ) : m_BackbufferResolution;
    pixelsize = 4;
    const size_t bytes = static_cast<size_t>( std::max( 1, buffersize.x ) ) * std::max( 1, buffersize.y ) * pixelsize;
    *data = new byte[bytes];
    memset( *data, 0, bytes );
}
