#pragma once
#include "../BaseGraphicsEngine.h"
#include "D3D12Device.h"
#include <memory>

class D3D12LineRenderer;

/** Direct3D 12 backend — Phase 1 first-light.

    Implements BaseGraphicsEngine on top of D3D12Device. This first increment brings up the device
    + a flip-model swapchain and drives the per-frame boundary through OnBeginFrame()/OnEndFrame()
    — Gothic's BeginScene/EndScene hooks, which fire every frame in the menu AND in-game:
      OnBeginFrame() begins the frame (reset, backbuffer -> render target, clear to a sentinel color)
      OnEndFrame()   ends the frame (render target -> present, execute, Present, fence-advance)
    so a running game (even at the main menu) shows a solid "first-light" color. World / UI /
    resource paths are still stubbed (safe no-ops) and land in later Phase-1/2 increments
    (descriptors, upload ring, PSOs, DrawVertexArray, scene rendering).

    D3D11 remains the default backend and the fallback: if device or swapchain creation fails,
    Engine::CreateGraphicsEngine keeps D3D11. */
class D3D12GraphicsEngine : public BaseGraphicsEngine {
public:
    static constexpr UINT kBackBufferCount = 2;

    D3D12GraphicsEngine();
    ~D3D12GraphicsEngine() override;

    EGraphicsEngineBackend GetBackendAPI() const override { return EGraphicsEngineBackend::D3D12; }

    /** Creates the D3D12 device + command queues (swapchain waits for the window). */
    XRESULT Init() override;

    XRESULT SetWindow( HWND hWnd ) override;
    XRESULT OnResize( INT2 newSize ) override;
    XRESULT TriggerResize( INT2 resolution ) override;

    /** Begins the frame (BeginScene hook): reset command list, backbuffer -> RTV, clear. */
    XRESULT OnBeginFrame() override;
    /** Ends the frame (EndScene hook): RTV -> present, execute, Present, advance frame. */
    XRESULT OnEndFrame() override;
    XRESULT SetViewport( const ViewportInfo& viewportInfo ) override;
    XRESULT Clear( const float4& color ) override;

    XRESULT CreateVertexBuffer( std::unique_ptr<GfxVertexBuffer>& outBuffer ) override;
    XRESULT CreateTexture( GfxTexture** outTexture ) override;
    XRESULT CreateTexture( std::unique_ptr<GfxTexture>& outTexture ) override;

    XRESULT GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling = false ) override;
    /** Presents the current backbuffer. Invoked at the end of OnEndFrame. */
    XRESULT Present() override;

    /** In-game world render entry (zCBspNodeRender hook). Stubbed until the D3D12 scene path lands. */
    XRESULT OnStartWorldRendering() override { return XR_SUCCESS; }

    BaseLineRenderer* GetLineRenderer() override;
    const std::string& GetGraphicsDeviceName() override { return m_Device.GetDeviceDescription(); }

    /** Native device for the D3D12 resource classes (D3D12Texture / D3D12VertexBuffer). */
    ID3D12Device* GetD3DDevice() const { return m_Device.GetDevice(); }

    /** Synchronous upload helper: copies CPU subresource data into a DEFAULT-heap resource that was
        created in COPY_DEST state, then transitions it to PIXEL_SHADER_RESOURCE. Blocks until the
        copy completes. Used by D3D12Texture (load-time uploads; the async copy-queue path is later). */
    bool UploadTextureSubresources( ID3D12Resource* dst, const D3D12_SUBRESOURCE_DATA* subresources, UINT numSubresources );

    XRESULT DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex = 0, unsigned int stride = sizeof( ExVertexStruct ) ) override { return XR_SUCCESS; }

    INT2 GetResolution() override { return m_Resolution; }
    INT2 GetBackbufferResolution() override { return m_Resolution; }

private:
    bool CreateSwapChain( INT2 size );
    bool CreateFrameResources();      // RTV heap + allocators + command list + fence + event
    bool CreateUploadObjects();       // dedicated allocator + command list + fence for synchronous uploads
    bool AcquireBackBufferRTVs();     // (re)fetch swapchain buffers + build their RTVs
    bool ResizeSwapChain( INT2 size );
    void WaitForGpuIdle();            // full CPU/GPU flush (used on resize / teardown)
    void MoveToNextFrame();           // signal current frame's fence, advance, wait for next allocator

    D3D12Device m_Device;

    Microsoft::WRL::ComPtr<IDXGISwapChain3>        m_SwapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>   m_RtvHeap;
    UINT m_RtvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource>         m_BackBuffers[kBackBufferCount];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_CmdAllocators[kBackBufferCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CmdList;

    Microsoft::WRL::ComPtr<ID3D12Fence> m_Fence;
    UINT64 m_FenceValues[kBackBufferCount] = {};
    HANDLE m_FenceEvent = nullptr;
    UINT   m_FrameIndex = 0;

    // Synchronous upload path (textures / buffers) — blocks the caller until the copy is done.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_UploadAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_UploadCmdList;
    Microsoft::WRL::ComPtr<ID3D12Fence>               m_UploadFence;
    UINT64 m_UploadFenceValue = 0;
    HANDLE m_UploadEvent = nullptr;

    HWND  m_OutputWindow = nullptr;
    INT2  m_Resolution = {};
    bool  m_SwapChainReady = false;
    bool  m_FrameOpen = false;        // true between OnBeginFrame and OnEndFrame
    float m_ClearColor[4] = { 0.39f, 0.58f, 0.93f, 1.0f }; // cornflower blue — first-light sentinel

    std::unique_ptr<D3D12LineRenderer> m_LineRenderer;
};
