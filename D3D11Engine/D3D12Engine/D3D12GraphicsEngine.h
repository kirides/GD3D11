#pragma once
#include "../BaseGraphicsEngine.h"
#include "D3D12Device.h"
#include <memory>
#include <unordered_map>

class D3D12LineRenderer;
struct GothicBlendStateInfo;

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

    /** In-game world render entry (zCBspNodeRender hook). Draws the static world mesh (Phase 2). */
    XRESULT OnStartWorldRendering() override;

    /** Draws the wrapped static world mesh. Phase-2 first-light: flat-shaded (screen-space derivative
        normal), depth-tested, no textures / no G-buffer — just the backbuffer + a depth target.
        `noTextures` is accepted for interface parity but currently always effectively true. */
    XRESULT DrawWorldMesh( bool noTextures = false ) override;

    BaseLineRenderer* GetLineRenderer() override;
    const std::string& GetGraphicsDeviceName() override { return m_Device.GetDeviceDescription(); }

    /** Native device for the D3D12 resource classes (D3D12Texture / D3D12VertexBuffer). */
    ID3D12Device* GetD3DDevice() const { return m_Device.GetDevice(); }

    /** Synchronous upload helper: copies CPU subresource data into a DEFAULT-heap resource that was
        created in COPY_DEST state, then transitions it to PIXEL_SHADER_RESOURCE. Blocks until the
        copy completes. Used by D3D12Texture (load-time uploads; the async copy-queue path is later). */
    bool UploadTextureSubresources( ID3D12Resource* dst, const D3D12_SUBRESOURCE_DATA* subresources, UINT numSubresources );

    /** Gothic's 2D/UI draw entry (menus, fonts, HUD). Uploads the transformed ExVertexStruct verts to
        a per-frame ring, binds the UI PSO + current texture + viewport constants, and draws. */
    XRESULT DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex = 0, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Gothic's D3D7 fixed-function vertex-buffer draw (DrawPrimitiveVB — sky dome, some HUD strips).
        Snapshots the cached Gothic_XYZRHW_DIF_T1_Vertex data and reuses the 2D/UI DrawVertexArray path. */
    XRESULT DrawVertexBufferFF( GfxVertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Records the currently-bound diffuse texture for the next 2D draw (SetTexture -> BindToSlot). */
    void BindSurfaceTextures( int slot, GfxTexture* diffuse, GfxTexture* normalmap, unsigned int numTextures = 2 ) override;

    INT2 GetResolution() override { return m_Resolution; }
    INT2 GetBackbufferResolution() override { return m_Resolution; }

    /** Allocates a persistent slot in the shader-visible SRV heap. Returns UINT_MAX if exhausted.
        Used by D3D12Texture to create its SRV once at load time. */
    UINT AllocateSrvSlot();
    void FreeSrvSlot( UINT slot );
    D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandle( UINT slot ) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle( UINT slot ) const;

    void UglySyncrhonizationWorkaroundWaitForGpuIdle() {
        WaitForGpuIdle();
    }

    void QueueSrvResourceForRelease( UINT slot, Microsoft::WRL::ComPtr<ID3D12Resource> resource );

    /** Defers release of a GPU resource until the GPU is provably done with the frames that may
        reference it (drained in MoveToNextFrame after that frame's fence). Unlike
        QueueSrvResourceForRelease it does NOT free an SRV slot — used when a texture recreates its
        backing resource (animated textures) but keeps its descriptor slot. */
    void QueueResourceForRelease( Microsoft::WRL::ComPtr<ID3D12Resource> resource );

private:
    void ResizeOutputWindow( INT2 size );  // size the OS window + inform Gothic (zCView) of the mode
    bool CreateSwapChain( INT2 size );
    bool CreateFrameResources();      // RTV heap + allocators + command list + fence + event
    bool CreateUploadObjects();       // dedicated allocator + command list + fence for synchronous uploads
    bool CreateSrvHeap();             // shader-visible CBV_SRV_UAV heap for texture SRVs
    bool CreateUIPipeline();          // root signature + inline shaders (compiled once); PSOs built per blend state
    // Returns a PSO for the 2D UI shaders matching the given Gothic blend state, creating + caching it on
    // first use. Emulates Gothic's per-draw fixed-function blend modes (opaque/alpha/additive/modulate/...).
    ID3D12PipelineState* GetOrCreateUIPipeline( const GothicBlendStateInfo& blend );
    bool CreateUIVertexBuffers();     // per-frame dynamic (upload-heap) vertex ring buffers
    bool CreateWhiteTexture();        // 1x1 white fallback (untextured colored 2D draws)
    bool CreateDepthBuffer( INT2 size ); // D32_FLOAT depth target + DSV (reversed-Z world rendering)
    bool CreateWorldPipeline();       // root sig + inline shaders + PSO for the textured world-mesh pass
    bool CreateVobPipeline();         // instanced VOB PSO (reuses the world root sig) + inline shaders
    bool CreateVobInstanceBuffers();  // per-frame dynamic (upload-heap) VOB instance ring buffers
    XRESULT DrawVobsInstanced();      // collect visible VOBs + draw each visual instanced (textured)
    bool CreateSkeletalPipeline();    // skeletal (animated NPC/monster) root sig + inline shaders + PSO
    bool CreateSkeletalConstantBuffers(); // per-frame dynamic (upload-heap) skeletal CB ring (instance + bones)
    XRESULT DrawSkeletalMeshes( std::vector<SkeletalVobInfo*>& vobs, bool asMorphMeshes = false );     // draw animated skeletal vobs (matrix-palette skinning)
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
    float m_ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // black — the 2D UI draws over it

    // --- 2D / UI draw path (Gothic menus, fonts, HUD) ---
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SrvHeap;         // shader-visible CBV_SRV_UAV heap (texture SRVs)
    UINT m_SrvDescriptorSize = 0;
    UINT m_SrvHeapCapacity = 0;
    UINT m_SrvAllocated = 0;                                        // bump allocator (no free-list yet)
    std::vector<UINT> m_FreeSrvSlots; // Recycled descriptor indices

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_UIRootSig;
    Microsoft::WRL::ComPtr<ID3DBlob> m_UIVsBlob;                    // compiled once; reused for every blend PSO
    Microsoft::WRL::ComPtr<ID3DBlob> m_UIPsBlob;
    // One PSO per distinct Gothic blend state (opaque/alpha/additive/modulate/...), built lazily.
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_UIPipelines;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_UIVertexBuffer[kBackBufferCount]; // persistently-mapped upload ring
    uint8_t* m_UIVertexBufferPtr[kBackBufferCount] = {};
    UINT m_UIVertexBufferCapacity = 0;
    UINT m_UIVertexBufferOffset = 0;                               // reset each OnBeginFrame
    bool m_UIOverflowLogged = false;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_WhiteTexture;         // 1x1 white fallback for untextured draws
    UINT m_WhiteSrvSlot = UINT_MAX;

    GfxTexture* m_CurrentTexture = nullptr;                        // diffuse bound for the next 2D draw
    D3D12_VIEWPORT m_CurrentViewport = {};                        // pixel-space viewport (drives transform + RSSetViewports)
    D3D12_RECT     m_CurrentScissor = {};

    // --- 3D world mesh path (Phase 2 first-light: flat-shaded, depth-tested, no G-buffer) ---
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_DsvHeap;         // single DSV
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_DepthBuffer;     // D32_FLOAT, reversed-Z
    UINT m_DsvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_WorldRootSig;     // b0 = ViewProj (16 root constants, VS); t0 SRV; s0
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_WorldPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_WorldVsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_WorldPsBlob;

    // Instanced static VOBs (reuses m_WorldRootSig; slot 0 = packed vertex, slot 1 = per-instance data).
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_VobPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_VobVsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_VobPsBlob;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_VobInstanceBuffer[kBackBufferCount]; // persistently-mapped upload ring
    uint8_t* m_VobInstanceBufferPtr[kBackBufferCount] = {};
    UINT m_VobInstanceBufferCapacity = 0;
    UINT m_VobInstanceBufferOffset = 0;                            // reset each OnBeginFrame
    bool m_VobInstanceOverflowLogged = false;

    // Skeletal (animated NPC/monster) path — matrix-palette skinning. Own root sig (b0 ViewProj root
    // consts + b1 per-instance CBV + b2 bone-palette CBV + t0 SRV + s0). Per-frame CB ring holds each
    // vob's instance CB + bone matrices (root CBVs into it, 256-byte aligned).
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_SkeletalRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_SkeletalPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_SkeletalVsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_SkeletalPsBlob;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_SkeletalCBBuffer[kBackBufferCount]; // persistently-mapped upload ring
    uint8_t* m_SkeletalCBBufferPtr[kBackBufferCount] = {};
    UINT m_SkeletalCBBufferCapacity = 0;
    UINT m_SkeletalCBBufferOffset = 0;                             // reset each OnBeginFrame
    bool m_SkeletalCBOverflowLogged = false;

    std::unique_ptr<D3D12LineRenderer> m_LineRenderer;
    std::vector<std::move_only_function<void()>> m_PerFrameCleanupItems[kBackBufferCount] = {};
};
