#pragma once
#include "../BaseGraphicsEngine.h"
#include "VulkanDevice.h"
#include "VulkanSwapchain.h"
#include <memory>

/** Vulkan backend, Phase 1: platform smoke test (VULKAN_IMPLEMENTATION_PLAN.md).

    Brings up the device and a swapchain on Gothic's window, clears every frame and draws the ImGui
    overlay; every scene/UI draw is a no-op and textures/buffers are size-only stand-ins. It exists to
    prove the 32-bit ICD, VA headroom, window modes, resize/minimize and HDR swapchains before the
    renderer is moved onto the RHI. Frame slot (counter % frames in flight) and swapchain image index
    are tracked separately: acquire may return images out of order. */
class VulkanGraphicsEngine : public BaseGraphicsEngine {
public:
    static constexpr uint32_t kMaxFramesInFlight = 3;

    VulkanGraphicsEngine();
    ~VulkanGraphicsEngine() override;

    EGraphicsEngineBackend GetBackendAPI() const override { return EGraphicsEngineBackend::Vulkan; }

    XRESULT Init() override;
    XRESULT SetWindow( HWND hWnd ) override;
    XRESULT OnResize( INT2 newSize ) override;
    XRESULT TriggerResize( INT2 resolution ) override;
    XRESULT OnBeginFrame() override;
    XRESULT OnEndFrame() override;
    XRESULT Present() override;

    XRESULT SetViewport( const ViewportInfo& ) override { return XR_SUCCESS; }
    XRESULT Clear( const float4& ) override { return XR_SUCCESS; }
    XRESULT CreateVertexBuffer( std::unique_ptr<GfxVertexBuffer>& outBuffer ) override;
    XRESULT CreateTexture( GfxTexture** outTexture ) override;
    XRESULT CreateTexture( std::unique_ptr<GfxTexture>& outTexture ) override;
    XRESULT GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling = false ) override;
    XRESULT OnStartWorldRendering() override { return XR_SUCCESS; }
    BaseLineRenderer* GetLineRenderer() override { return m_LineRenderer.get(); }
    const std::string& GetGraphicsDeviceName() override { return m_Device.GetDeviceDescription(); }
    XRESULT DrawVertexArray( ExVertexStruct*, unsigned int, unsigned int = 0, unsigned int = sizeof( ExVertexStruct ) ) override { return XR_SUCCESS; }
    bool GetHdrOutputInfo( float& maxNits, float& minNits, float& maxFullFrameNits ) const override;

    INT2 GetResolution() override { return m_BackbufferResolution; }
    INT2 GetBackbufferResolution() override { return m_BackbufferResolution; }
    /** Savegame thumbnails get a black image until the scene renders on Vulkan. */
    void GetBackbufferData( bool thumbnail, byte** data, INT2& buffersize, int& pixelsize ) override;

    /** Debug-utils label scope on the frame's command buffer; empty outside an open frame. */
    GraphicsEventRecord RecordGraphicsEvent( GraphicsEventName region ) override;

private:
    struct FrameContext {
        VkCommandPool   Pool = VK_NULL_HANDLE;
        VkCommandBuffer Cmd = VK_NULL_HANDLE;
        VkSemaphore     AcquireSemaphore = VK_NULL_HANDLE;   // signalled by vkAcquireNextImageKHR
        uint64_t        RetireValue = 0;                     // timeline value this slot's last submit signals
    };

    bool CreateFrameResources();
    void DestroyFrameResources();
    bool CreateOrResizeSwapchain( INT2 size );
    /** Bounded, diagnosed CPU wait on the frame timeline (never an unlogged infinite hang). */
    void WaitForTimeline( uint64_t value, const char* site );
    [[noreturn]] void HandleDeviceLost( const char* context );
    void DetectHdrOutput();

    VulkanDevice    m_Device;           // destroyed last
    VulkanSwapchain m_Swapchain;
    std::unique_ptr<BaseLineRenderer> m_LineRenderer;

    FrameContext m_Frames[kMaxFramesInFlight];
    uint32_t     m_FramesInFlight = 3;
    uint32_t     m_FrameSlot = 0;
    uint32_t     m_ImageIndex = 0;
    VkSemaphore  m_FrameTimeline = VK_NULL_HANDLE;
    uint64_t     m_LastSignaledValue = 0;
    uint64_t     m_FrameCounter = 0;

    INT2  m_BackbufferResolution = {};
    bool  m_SwapchainReady = false;
    bool  m_SwapchainDirty = false;   // present reported OUT_OF_DATE/SUBOPTIMAL
    bool  m_FrameOpen = false;
    float m_ClearColor[4] = { 0.06f, 0.08f, 0.14f, 1.0f };   // distinct from D3D11/D3D12's black: "Vulkan is presenting"

    bool  m_HdrOutputActive = false;
    float m_HdrMaxNits = 0.0f;
    float m_HdrMinNits = 0.0f;
    float m_HdrMaxFullFrameNits = 0.0f;
};
