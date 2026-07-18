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

    /** Fills the sky/background with Gothic's atmosphere (fog) color. Forward-renderer MVP: a color
        clear at the start of the world pass; distance fog in the geometry shaders fades into the same
        color so the horizon dissolves seamlessly. No sky dome/scattering yet. */
    XRESULT DrawSky() override;

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

    /** Custom-font text (menu strings, subtitles). Builds glyph quads over the font atlas and routes
        them through the validated 2D/UI path (VS_TransformedEx + FF-stage PS + alpha blend). */
    void DrawString( std::string_view str, float x, float y, const zFont* font, zColor& fontColor ) override;

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

    /*
     Defers release of a GPU resource until the GPU is provably done with the frames that may
        reference it (drained in MoveToNextFrame after that frame's fence). Unlike
        QueueSrvResourceForRelease it does NOT free an SRV slot — used when a texture recreates its
        backing resource (animated textures) but keeps its descriptor slot.
        */
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
    ID3D12PipelineState* GetOrCreateUIPipeline( const GothicBlendStateInfo& blend, const GothicDepthBufferStateInfo& depth );
    bool CreateUIVertexBuffers();     // per-frame dynamic (upload-heap) vertex ring buffers
    bool CreateWhiteTexture();        // 1x1 white fallback (untextured colored 2D draws)
    bool CreateDepthBuffer( INT2 size ); // R32_TYPELESS depth target + DSV(D32) + SRV(R32) (reversed-Z world rendering)
    bool CreateWorldPipeline();       // root sig + inline shaders + PSO for the textured world-mesh pass
    bool CreateDepthPrepassPipeline(); // Forward+ opaque depth prepass PSO (depth-only world mesh; reuses m_WorldRootSig)
    void DrawDepthPrepass();          // lay down opaque world-mesh depth before the lit passes (Forward+ prepass)
    bool CreateLightCullPipeline();   // Forward+ tiled light-cull compute root sig + PSO (one global compute root sig)
    bool CreateLightCullBuffers( INT2 size ); // per-resolution tile grid + index-list UAV buffers (rebuilt on resize)
    void DispatchLightCulling();      // dispatch the tiled light cull (writes the per-tile light grid; not yet consumed)
    bool CreateVobPipeline();         // instanced VOB PSO (reuses the world root sig) + inline shaders
    bool CreateVobInstanceBuffers();  // per-frame dynamic (upload-heap) VOB instance ring buffers
    void UploadFrameVobInstances();   // snapshot visible VOB instances into the ring ONCE (prepass + color share it)
    void DrawVobDepthPrepass();       // lay down instanced VOB depth (alpha-clipped) into the Forward+ prepass
    XRESULT DrawVobsInstanced();      // collect visible VOBs + draw each visual instanced (textured)
    bool CreateLightBuffer();         // per-frame point-light structured buffers (Forward+ MVP: brute-force)
    void BuildFrameLightBuffer();     // (re)fill this frame's light buffer from the collected visible lights
    void BindFrameLights( UINT srvParam = 3, UINT countParam = 4, UINT gridParam = 5, UINT indexParam = 6 );   // light SRV(t1)+count+grid(t2)+index(t3); (3,4,5,6)=world, (5,6,7,8)=skeletal
    bool CreateWaterPipeline();       // alpha-blended water PSO + own root sig (adds b2 time) + inline shaders
    void DrawWaterSurfaces() override; // draw water peeled out of the opaque world pass (scrolled UV, blended)
    bool CreateSkeletalPipeline();    // skeletal (animated NPC/monster) root sig + inline shaders + PSO
    bool CreateSkeletalConstantBuffers(); // per-frame dynamic (upload-heap) skeletal CB ring (instance + bones)
    void PrepareFrameSkeletals( std::vector<SkeletalVobInfo*>& vobs );   // once/frame anim update + upload bone/inst CBs + attachment instances (pre-cull)
    void DrawSkeletalDepthPrepass();  // lay down skeletal base + node-attachment depth into the Forward+ prepass
    void DrawSkeletalColor();         // draw the collected skeletal base meshes + node attachments (post-cull, lit)
    bool CreateParticlePipeline();    // particle (PFX) root sig + inline billboard shaders (PSOs built per blend)
    bool CreateParticleInstanceBuffers(); // per-frame dynamic (upload-heap) particle instance ring
    // Returns a PSO for the particle shaders matching the given Gothic blend state (alpha/additive/modulate),
    // creating + caching it on first use. Keyed by BlendKey.
    ID3D12PipelineState* GetOrCreateParticlePipeline( const GothicBlendStateInfo& blend );
    XRESULT DrawParticleEffects();    // collect visible PFX (backend-neutral) + draw billboards, blended over the scene
    bool CreateDecalPipeline();       // decal root sig + shared unit-quad VB + inline shaders + lit/blend PSOs
    bool CreateDecalInstanceBuffers(); // per-frame dynamic (upload-heap) decal instance ring
    // Returns a transparent-decal PSO matching the given Gothic blend state (alpha/additive/modulate), created
    // + cached on first use. Keyed by BlendKey. The opaque/alpha-test path uses the fixed m_DecalLitPSO instead.
    ID3D12PipelineState* GetOrCreateDecalBlendPipeline( const GothicBlendStateInfo& blend );
    // Draw the visible decals (blood, arrows, sprites) as instanced camera/surface-aligned quads. lighting=true
    // = the opaque/alpha-test pass (depth-write, drawn with the opaque geometry); lighting=false = the
    // transparent pass (per-material blend, depth-read-only, drawn over the finished scene). Mirrors D3D11's
    // two-pass DrawDecalList; preserves the received back-to-front order (painter's algorithm, no batching).
    void DrawDecalList( const std::vector<zCVob*>& decals, bool lighting );
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
    // One PSO per distinct Gothic blend+depth state (opaque/alpha/additive/modulate/... x depth on/off),
    // built lazily. Key = BlendKey | (DepthKey << 32).
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_UIPipelines;

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
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_DepthBuffer;     // R32_TYPELESS (DSV D32_FLOAT / SRV R32_FLOAT), reversed-Z
    UINT m_DsvDescriptorSize = 0;
    UINT m_DepthSrvSlot = UINT_MAX;   // R32_FLOAT SRV of m_DepthBuffer, read by the light cull for per-tile far-Z tightening

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_WorldRootSig;     // b0 = ViewProj (16 root constants, VS); t0 SRV; s0
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_WorldPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_WorldVsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_WorldPsBlob;

    // Forward+ opaque depth prepass (P2.9b-1): depth-only world-mesh PSO (color write mask 0), reuses m_WorldRootSig.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_DepthPrepassWorldPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_DepthPrepassWorldVsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_DepthPrepassPsBlob;
    // Instanced-VOB depth prepass (P2.9b-4a): depth-only VOB PSO (color write mask 0), reuses m_WorldRootSig.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_DepthPrepassVobPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_DepthPrepassVobVsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_DepthPrepassVobPsBlob;
    // Skeletal depth prepass (P2.9b-4b): depth-only skinned PSO (color write mask 0), reuses m_SkeletalRootSig.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_DepthPrepassSkeletalPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_DepthPrepassSkeletalVsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_DepthPrepassSkeletalPsBlob;

    // CSM sun shadows (P2.9c). Directional shadow map = a Texture2DArray (one slice per cascade), R32_TYPELESS
    // so each slice serves a D32_FLOAT DSV and the whole array serves one R32_FLOAT SRV for later PCF sampling.
    // NORMAL-Z here (clear 1.0, LESS_EQUAL) — NOT reversed-Z like the main camera (mirrors the D3D11 caster).
    static constexpr UINT kShadowCascades = 3;
    static constexpr UINT kShadowMapSize  = 2048;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_ShadowMap;        // Texture2DArray(R32_TYPELESS), kShadowCascades slices
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_ShadowDsvHeap;    // one D32 DSV per cascade slice
    UINT m_ShadowDsvSize = 0;
    UINT m_ShadowSrvSlot = UINT_MAX;    // R32_FLOAT Texture2DArray SRV (all cascades), for the lit-pass sampler (later increment)
    bool m_ShadowInPixelState = false;  // DEPTH_WRITE (caster writes) <-> PIXEL_SHADER_RESOURCE (lit reads) round-trip
    // Reuses the depth-prepass world VS/PS (VSWorld + PSClip, b0 = a view-proj) with a normal-Z, front-cull,
    // depth-biased state. Fed the per-cascade light view-proj instead of the camera's.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ShadowCasterWorldPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_ShadowCasterPsBlob;   // PSShadowClip (void PS, alpha-clip only)
    // VOB + skeletal casters (P2.9c-2): reuse the VOB/skeletal depth-prepass VSDepth blobs + their void
    // PSShadowClip, with the same normal-Z/front-cull/bias caster state. Attachments ride the VOB caster PSO.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ShadowCasterVobPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_ShadowCasterVobPsBlob;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ShadowCasterSkeletalPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_ShadowCasterSkeletalPsBlob;
    DirectX::XMFLOAT4X4 m_CascadeViewProj[kShadowCascades] = {};   // light-space view*proj per cascade (this frame)
    float m_CascadeTexelWorld[kShadowCascades] = {};   // world units / shadow texel per cascade (for the sampling normal bias)
    DirectX::XMFLOAT3 m_SunDirWS = { 0.0f, 1.0f, 0.0f };   // normalized world-space dir TOWARD the sun (this frame)
    // Per-frame-in-flight shadow-sampling constant buffer (b3 in the lit passes): the cascade view-projs +
    // sun dir + strength + texel sizes, uploaded once per frame in RenderSunShadows and bound by DrawWorldMesh.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ShadowCB[kBackBufferCount];
    uint8_t* m_ShadowCBMapped[kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_ShadowCBGpu[kBackBufferCount] = {};
    bool CreateShadowMap();            // shadow Texture2DArray + per-slice DSVs + array SRV + caster PSOs + CB ring (once, at init)
    void ComputeCascadeMatrices();     // fill m_CascadeViewProj/m_CascadeTexelWorld/m_SunDirWS from the sun + camera (simple ortho for now)
    void RenderSunShadows();           // render the opaque casters into each cascade slice from the sun's POV + upload the sampling CB

    // Forward+ tiled light culling (P2.9b-2): one global compute root sig + PSO; two resolution-sized
    // DEFAULT-heap UAV buffers holding the per-tile {Offset,Count} grid and the per-tile light-index slices
    // (fixed 32/tile, no global counter). All live permanently in UNORDERED_ACCESS. m_NumTilesX/Y = tile
    // grid dimensions for the current resolution.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_LightCullRootSig;  // b0 consts; t0 lights SRV; u0 grid; u1 index list
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_LightCullPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_LightCullCsBlob;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_LightGridBuffer;    // RWStructuredBuffer<LightGrid> (numTiles * 8 B)
    Microsoft::WRL::ComPtr<ID3D12Resource> m_LightIndexBuffer;   // RWStructuredBuffer<uint>  (numTiles * 32 * 4 B)
    UINT m_NumTilesX = 0;
    UINT m_NumTilesY = 0;
    // Grid/index buffers round-trip UNORDERED_ACCESS (cull CS writes) -> PIXEL_SHADER_RESOURCE (lit PS reads)
    // each frame. Tracks whether they're currently in the PS-read state so DispatchLightCulling knows whether
    // it must transition them back to UAV before the next dispatch (false right after (re)creation in UAV).
    bool m_LightGridInPixelState = false;

    // Instanced static VOBs (reuses m_WorldRootSig; slot 0 = packed vertex, slot 1 = per-instance data).
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_VobPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_VobVsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_VobPsBlob;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_VobInstanceBuffer[kBackBufferCount]; // persistently-mapped upload ring
    uint8_t* m_VobInstanceBufferPtr[kBackBufferCount] = {};
    UINT m_VobInstanceBufferCapacity = 0;
    UINT m_VobInstanceBufferOffset = 0;                            // reset each OnBeginFrame
    bool m_VobInstanceOverflowLogged = false;

    // --- Forward+ dynamic point lights (P2.9a: per-frame brute-force light buffer, no tiling yet) ---
    // StructuredBuffer of the frame's visible point lights, rebuilt each frame from CollectVisibleVobs and
    // bound as a root SRV (t1) to the world/VOB pixel shaders, which loop it per pixel (N.L + attenuation)
    // on top of the baked vertex lighting. Root SRV => no descriptor-heap slot consumed.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_LightBuffer[kBackBufferCount]; // persistently-mapped UPLOAD, GPULight[]
    uint8_t* m_LightBufferPtr[kBackBufferCount] = {};
    UINT m_LightBufferCapacity = 0;                               // max lights per frame
    UINT m_FrameLightCount = 0;                                   // lights written this frame
    bool m_LightOverflowLogged = false;

    // Water (transparent world surfaces). Own root sig = the world layout + b2 = { time, alpha } (VS
    // scrolls the UV by time; PS uses alpha for the blend). Alpha-blended PSO: depth-test ON, write OFF.
    // Water geometry lives in the SAME wrapped world VB/IB (36-byte packed vertex, R32 indices); it is
    // peeled out of DrawWorldMesh's opaque loop and drawn here after all opaque geometry.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_WaterRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_WaterPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_WaterVsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_WaterPsBlob;

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

    // Particle (PFX) path — instanced camera-facing billboards, one instance per live particle. Own root
    // sig (b0 ViewProj root consts + b1 camera pos + t0 SRV + s0). PSOs are built per Gothic blend mode
    // (alpha/additive/modulate) and cached by BlendKey. Per-frame instance ring holds ParticleInstanceInfo.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_ParticleRootSig;
    Microsoft::WRL::ComPtr<ID3DBlob> m_ParticleVsBlob;             // compiled once; reused for every blend PSO
    Microsoft::WRL::ComPtr<ID3DBlob> m_ParticlePsBlob;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_ParticlePipelines; // key = BlendKey
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ParticleInstanceBuffer[kBackBufferCount]; // persistently-mapped upload ring
    uint8_t* m_ParticleInstanceBufferPtr[kBackBufferCount] = {};
    UINT m_ParticleInstanceBufferCapacity = 0;
    UINT m_ParticleInstanceBufferOffset = 0;                       // reset each OnBeginFrame
    bool m_ParticleInstanceOverflowLogged = false;

    // Decal path — blood splats, arrows, sprites. Every decal is the same unit quad (m_DecalQuadVB),
    // instanced with a per-decal world matrix (world*offset*scale; ViewProj applies view+proj) + ghost
    // alpha. Own root sig (b0 ViewProj + t0 SRV + s0 clamp). Two PS: opaque/alpha-test (m_DecalLitPSO,
    // depth-write) and transparent (m_DecalBlendPipelines, per Gothic blend mode, depth-read-only).
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_DecalRootSig;
    Microsoft::WRL::ComPtr<ID3DBlob> m_DecalVsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_DecalLitPsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_DecalBlendPsBlob;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_DecalLitPSO;      // opaque/alpha-test decals, depth-write on
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_DecalBlendPipelines; // key = BlendKey
    Microsoft::WRL::ComPtr<ID3D12Resource> m_DecalQuadVB;          // shared unit quad (6 verts), static
    D3D12_VERTEX_BUFFER_VIEW m_DecalQuadVBV = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_DecalInstanceBuffer[kBackBufferCount]; // persistently-mapped upload ring
    uint8_t* m_DecalInstanceBufferPtr[kBackBufferCount] = {};
    UINT m_DecalInstanceBufferCapacity = 0;
    UINT m_DecalInstanceBufferOffset = 0;                          // reset each OnBeginFrame
    bool m_DecalInstanceOverflowLogged = false;

    std::unique_ptr<D3D12LineRenderer> m_LineRenderer;
    std::vector<std::move_only_function<void()>> m_PerFrameCleanupItems[kBackBufferCount] = {};
};
