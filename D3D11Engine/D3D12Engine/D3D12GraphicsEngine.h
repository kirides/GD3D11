#pragma once
#include <D3D12MemAlloc.h>
#include <deque>

#include "../BaseGraphicsEngine.h"
#include "../Frustum.h"
#include "D3D12Device.h"
#include <memory>
#include <unordered_map>
#include <vector>

#include "D3D12Texture.h"
#include "D3D12ShaderBackend.h"
#include "D3D12PipelineState.h"

struct RenderBucket;
class D3D12LineRenderer;
struct GothicBlendStateInfo;
struct FrameVobUpload;
class zCVobLight;

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
    XRESULT CreateTexture(std::unique_ptr<D3D12Texture>& outTexture);

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

    /** Asynchronously uploads CPU subresource data into a DEFAULT-heap resource using the dedicated
        copy queue, then defers the staging allocation lifetime until the copy-queue fence reaches the
        submitted value. The caller can then issue a direct-queue transition barrier once the copy has
        completed. */
    bool UploadTextureSubresources( ID3D12Resource* dst, const D3D12_SUBRESOURCE_DATA* subresources, UINT numSubresources );
    bool UploadBufferData( ID3D12Resource* dst, const void* srcData, UINT64 sizeInBytes );
    bool InitCopyQueue();
    UINT64 GetCopyFenceValue() const { return m_CopyFenceValue; }
    void WaitForCopyFence( UINT64 fenceValue );
    void TransitionTextureToSRVOnDirectQueue( ID3D12Resource* texture );

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

    void QueueSrvResourceForRelease( UINT slot, Microsoft::WRL::ComPtr<ID3D12Resource> resource );

    /*
     Defers release of a GPU resource until the GPU is provably done with the frames that may
        reference it (drained in MoveToNextFrame after that frame's fence). Unlike
        QueueSrvResourceForRelease it does NOT free an SRV slot — used when a texture recreates its
        backing resource (animated textures) but keeps its descriptor slot.
        */
    void QueueResourceForRelease( Microsoft::WRL::ComPtr<ID3D12Resource> resource );
    void QueueAllocationForRelease(Microsoft::WRL::ComPtr<D3D12MA::Allocation> value);

    void OnAddVob(VobInfo* vi) override;
    XRESULT OnVobRemovedFromWorld( zCVob* vob ) override;
    void OnLoadWorld() override;
    void DrawVobSingle( VobInfo* vob, zCCamera& camera ) override;  // inventory item preview (GInventory), drawn straight onto the backbuffer
    D3D12MA::Allocator* GetAllocator() const { return m_Allocator.Get(); }

    /** Savegame-thumbnail / screenshot readback (MyDirectDrawSurface7::Lock's DDLOCK_READONLY hack).
        Re-tonemaps the just-rendered HDR scene into a CPU-readable 32bpp BGRA8 buffer at either 256x256
        (thumbnail) or full resolution. Caller owns *data (new[]'d) and must delete[] it. */
    void GetBackbufferData( bool thumbnail, byte** data, INT2& buffersize, int& pixelsize ) override;

private:
    void QueueCleanupJob(std::move_only_function<void()> callback); // cleanup job runs after the calling frames fence value is completed.
    bool CreateAllocators();
    void ResizeOutputWindow( INT2 size );  // size the OS window + inform Gothic (zCView) of the mode
    bool CreateSwapChain( INT2 size );
    bool CreateFrameResources();      // RTV heap + allocators + command list + fence + event
    bool CreateUploadObjects();       // dedicated allocator + command list + fence for synchronous uploads
    bool CreateSrvHeap();             // shader-visible CBV_SRV_UAV heap for texture SRVs
    // UI/Particle/Decal pipeline creation now lives in m_Pipelines (CreateUI/CreateParticle/CreateDecal); the
    // GetOrCreate* blend-key PSO caches moved there too. GPU ring buffers + the decal quad VB stay in the engine.
    bool CreateUIVertexBuffers();     // per-frame dynamic (upload-heap) vertex ring buffers
    bool CreateWhiteTexture();        // 1x1 white fallback (untextured colored 2D draws)
    bool CreateDepthBuffer( INT2 size ); // R32_TYPELESS depth target + DSV(D32) + SRV(R32) (reversed-Z world rendering)
    bool CreateSceneColorTarget( INT2 size ); // R16F HDR scene-color RT (+RTV +SRV) the 3D passes render into; recreated on resize
    void BindSceneColorTarget();      // transition HDR RT -> RENDER_TARGET (if needed) + bind it (+ depth) as the world-pass RTV
    void ResolveSceneToBackBuffer();  // tonemap the HDR scene into the swapchain backbuffer, then rebind the backbuffer for the 2D UI
    // World/DepthPrepass/Vob pipeline creation now lives in m_Pipelines (CreateWorld/CreateDepthPrepass/CreateVob).
    void DrawDepthPrepass();          // lay down opaque world-mesh depth before the lit passes (Forward+ prepass)
    bool CreateLightCullBuffers( INT2 size ); // per-resolution tile grid + index-list UAV buffers (rebuilt on resize)
    void DispatchLightCulling();      // dispatch the tiled light cull (writes the per-tile light grid; not yet consumed)
    // Vob pipeline creation now lives in m_Pipelines.CreateVob (buffers stay: CreateVobInstanceBuffers).
    bool CreateVobInstanceBuffers();  // per-frame dynamic (upload-heap) VOB instance ring buffers
    void UploadFrameVobInstances();   // snapshot visible VOB instances into the ring ONCE (prepass + color share it)
    bool UploadVobs(const std::vector<RenderBucket>& vobs, std::vector<FrameVobUpload>& uploads);
    void DrawVobDepthPrepass();       // lay down instanced VOB depth (alpha-clipped) into the Forward+ prepass
    XRESULT DrawVobsInstanced();      // collect visible VOBs + draw each visual instanced (textured)
    // Set a lit draw's per-material bindless normal/ORM indices (b6 root consts). matRootParam = 10 (world/VOB
    // root sig) or 12 (skeletal). Call once per material change, right after binding its diffuse SRV.
    void BindMaterialMaps( class zCTexture* tex, UINT matRootParam );
    bool CreateLightBuffer();         // per-frame point-light structured buffers (Forward+ MVP: brute-force)
    void BuildFrameLightBuffer();     // (re)fill this frame's light buffer from the collected visible lights
    void BindFrameLights( UINT srvParam = 3, UINT countParam = 4, UINT gridParam = 5, UINT indexParam = 6 );   // light SRV(t1)+count+grid(t2)+index(t3); (3,4,5,6)=world, (5,6,7,8)=skeletal
    void DrawWaterSurfaces() override; // draw water peeled out of the opaque world pass (scrolled UV, blended)
    // Skeletal (animated NPC/monster) pipeline creation now lives in m_Pipelines.CreateSkeletal (root sig + lit +
    // depth-prepass PSOs). The per-frame skeletal CB ring stays here:
    bool CreateSkeletalConstantBuffers(); // per-frame dynamic (upload-heap) skeletal CB ring (instance + bones)
    // once/frame anim update + upload bone/inst CBs + attachment instances (pre-cull). cullFrustum (if given)
    // rejects vobs whose bbox misses it — used by the shadow cascades to cull against the CASCADE frustum
    // instead of the player's view frustum (a caster invisible to the player can still cast a visible shadow).
    // sphereCenter/sphereRange (if given, sphereCenter != nullptr) instead cull by distance from that point —
    // used by point-light shadows to cull against the LIGHT's sphere instead of the player's camera radius;
    // cullFrustum and sphereCenter are mutually exclusive per call (cascades pass a frustum, point lights pass
    // a sphere). shadowCascade >= 0 routes the (filtered) records into that cascade's own list, -2 routes into
    // the point-shadow scratch list (g_PointShadowSkelDraws/g_PointShadowAttachDraws), else the main-view
    // g_FrameSkelDraws/g_FrameAttachDraws; the per-vob CB/attachment ring upload itself is still cached once per
    // frame (see g_SkelUploadCache) regardless of how many cull passes touch that vob.
    void PrepareFrameSkeletals( std::vector<SkeletalVobInfo*>& vobs, const Frustum* cullFrustum = nullptr, int shadowCascade = -1,
        const DirectX::XMFLOAT3* sphereCenter = nullptr, float sphereRange = 0.0f );
    void DrawSkeletalDepthPrepass();  // lay down skeletal base + node-attachment depth into the Forward+ prepass
    void DrawSkeletalColor();         // draw the collected skeletal base meshes + node attachments (post-cull, lit)
    bool CreateParticleInstanceBuffers(); // per-frame dynamic (upload-heap) particle instance ring
    XRESULT DrawParticleEffects();    // collect visible PFX (backend-neutral) + draw billboards, blended over the scene
    bool CreateDecalInstanceBuffers(); // per-frame dynamic (upload-heap) decal instance ring
    bool CreateDecalQuadVB();         // shared unit-quad VB (6 verts) consumed by the m_Pipelines.Decal PSOs
    // Draw the visible decals (blood, arrows, sprites) as instanced camera/surface-aligned quads. lighting=true
    // = the opaque/alpha-test pass (depth-write, drawn with the opaque geometry); lighting=false = the
    // transparent pass (per-material blend, depth-read-only, drawn over the finished scene). Mirrors D3D11's
    // two-pass DrawDecalList; preserves the received back-to-front order (painter's algorithm, no batching).
    void DrawDecalList( const std::vector<zCVob*>& decals, bool lighting );
    // Ghost/transparency VOBs (invisible-potion/fade items — GothicAPI::TransparencyVobs, populated every frame
    // by CollectVisibleVobs' GetVisualAlpha() branch). Mirrors D3D11's GothicAPI::DrawTransparencyVobs (unlit
    // diffuse sample, alpha *= per-vob GhostAlpha) but MUST run regardless of feature support: nothing else
    // drains this list, so skipping the call would leak one entry per ghost vob per frame forever.
    void DrawGhostVobs();
    bool AcquireBackBufferRTVs();     // (re)fetch swapchain buffers + build their RTVs
    bool ResizeSwapChain( INT2 size );
    void WaitForGpuIdle();            // full CPU/GPU flush (used on resize / teardown)
    void MoveToNextFrame();           // signal current frame's fence, advance, wait for next allocator

    // Mid-frame synchronous flush for GetBackbufferData: closes + executes the currently-recorded m_CmdList
    // and blocks until the GPU has consumed it, then Resets the same allocator/list so recording can
    // continue for the rest of the frame. Unlike Present()/MoveToNextFrame() this does NOT transition the
    // backbuffer to PRESENT and does NOT advance m_FrameIndex — callers must restore whatever RTV/viewport/
    // heap state subsequent draws expect (see RestoreFrameRenderTarget).
    void FlushCommandListSync();
    // Rebinds the swapchain backbuffer (+ depth) as the active render target with the full-resolution
    // viewport/scissor and shader-visible SRV heap — mirrors the tail of OnBeginFrame. Used after
    // FlushCommandListSync() leaves the command list freshly Reset with nothing bound.
    void RestoreFrameRenderTarget();

    D3D12Device m_Device;

    // D3D12 Memory Allocator (GPUOpen). Declared here — right after m_Device — deliberately: members are
    // destroyed in reverse declaration order, so keeping this near the top guarantees the allocator outlives
    // every D3D12MA::Allocation member below it (an Allocation frees back into the allocator on release, so
    // releasing one after the allocator is gone is a use-after-free). All persistent resources route their
    // creation through m_Allocator->CreateResource and hold a parallel ...Alloc member alongside the resource.
    Microsoft::WRL::ComPtr<D3D12MA::Allocator> m_Allocator;

    Microsoft::WRL::ComPtr<IDXGISwapChain3>        m_SwapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>   m_RtvHeap;   // kBackBufferCount swapchain RTVs + 1 HDR scene-color RTV
    UINT m_RtvDescriptorSize = 0;

    // HDR scene-color pipeline (Phase 3): the 3D world passes render into m_SceneColor (R16F, values >1 allowed —
    // sun + additive point lights no longer clip), then ResolveSceneToBackBuffer tonemaps it into the swapchain.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_SceneColor;           // R16G16B16A16_FLOAT, resolution-sized
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_SceneColorAlloc;       // backing D3D12MA allocation (recreated on resize)
    D3D12_CPU_DESCRIPTOR_HANDLE                  m_SceneColorRtv = {};    // RTV heap slot kBackBufferCount
    UINT m_SceneColorSrvSlot = UINT_MAX;                                  // SRV read by the tonemap resolve
    bool m_SceneColorInPixelState = false;                               // RENDER_TARGET (world) <-> PIXEL_SHADER_RESOURCE (resolve)
    // Which RTV format/target DrawVertexArray is currently rendering into — true while the HDR scene-color
    // target is bound (BindSceneColorTarget, i.e. during OnStartWorldRendering incl. DrawSky), false once
    // ResolveSceneToBackBuffer/OnBeginFrame bind the swapchain backbuffer for the later 2D UI/HUD draws.
    bool m_ColorTargetIsHDR = false;
    // Tonemap pipeline (RootSig/PSO/blobs) now lives in m_Pipelines.Tonemap; exposure is read live from
    // Engine::GAPI->GetRendererState().RendererSettings.Exposure in ResolveSceneToBackBuffer (player-tunable).

    Microsoft::WRL::ComPtr<ID3D12Resource>         m_BackBuffers[kBackBufferCount];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_CmdAllocators[kBackBufferCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CmdList;

    Microsoft::WRL::ComPtr<ID3D12Fence> m_Fence;
    UINT64 m_FenceValues[kBackBufferCount] = {};
    HANDLE m_FenceEvent = nullptr;
    UINT   m_FrameIndex = 0;

    // Synchronous upload path (direct queue) used for the transition barrier after async copy-queue uploads.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_UploadAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_UploadCmdList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_UploadFence;
    UINT64 m_UploadFenceValue = 0;
    HANDLE m_UploadEvent = nullptr;

    // Copy-queue upload path for textures (asynchronous to the main direct queue).
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_CopyQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_CopyAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CopyCmdList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_CopyFence;
    UINT64 m_CopyFenceValue = 0;
    HANDLE m_CopyFenceEvent = nullptr;
    bool m_TearingSupported;

    struct PendingCopyRelease {
        UINT64 FenceValue = 0;
        Microsoft::WRL::ComPtr<D3D12MA::Allocation> UploadAllocation;
        Microsoft::WRL::ComPtr<ID3D12Resource> UploadResource;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CopyAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> CopyCommandList;
    };
    std::deque<PendingCopyRelease> m_PendingCopyReleases;
    std::mutex m_CopyQueueMutex; // Protects copy queue operations and pending releases

    void ReleaseCompletedCopyResources( UINT64 fenceValue );

    HWND  m_OutputWindow = nullptr;
    INT2  m_Resolution = {};
    // Requested resolution (TriggerResize just stores it here, mirrors D3D11GraphicsEngine::NewResolution).
    // Applied at the very start of the NEXT OnBeginFrame — never mid-frame — so the resize always runs while
    // the command list is closed/idle (no open recording to disrupt), instead of the old per-frame-cleanup-ring
    // deferral which could stall for an unbounded number of frames waiting for m_FrameIndex to cycle back to
    // the slot the request landed in, and would leave stale null DSV/UAV bindings in the meantime.
    INT2  m_NewResolution = {};
    bool  m_SwapChainReady = false;
    bool  m_FrameOpen = false;        // true between OnBeginFrame and OnEndFrame
    float m_ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // black — the 2D UI draws over it

    // --- 2D / UI draw path (Gothic menus, fonts, HUD) ---
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SrvHeap;         // shader-visible CBV_SRV_UAV heap (texture SRVs)
    UINT m_SrvDescriptorSize = 0;
    UINT m_SrvHeapCapacity = 0;
    UINT m_SrvAllocated = 0;                                        // bump allocator (no free-list yet)
    std::vector<UINT> m_FreeSrvSlots; // Recycled descriptor indices

    // Loads + compiles the backend's HLSL from Shaders\D3D12\*.hlsl at runtime (DXC/SM6.6, zFILE_VDFS).
    D3D12ShaderBackend m_ShaderBackend;

    // Owns per-pass root signatures + PSOs + shader blobs (creation extracted from this monolith).
    D3D12PipelineState m_Pipelines;

    // The 2D/UI root sig + shaders + blend/depth PSO cache now live in m_Pipelines.UI. The vertex ring stays here.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_UIVertexBuffer[kBackBufferCount]; // persistently-mapped upload ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_UIVertexBufferAlloc[kBackBufferCount];
    uint8_t* m_UIVertexBufferPtr[kBackBufferCount] = {};
    UINT m_UIVertexBufferCapacity = 0;
    UINT m_UIVertexBufferOffset = 0;                               // reset each OnBeginFrame
    bool m_UIOverflowLogged = false;

    std::unique_ptr<D3D12Texture> m_WhiteTexture;         // 1x1 white fallback for untextured draws
    std::unique_ptr<D3D12Texture> m_BlackTexture;         // 1x1 black fallback for untextured draws
    std::unique_ptr<D3D12Texture> m_DefaultOrmTexture;    // 1x1 ORM default (AO 1, rough 0.5, metal 0)

    GfxTexture* m_CurrentTexture = nullptr;                        // diffuse bound for the next 2D draw
    D3D12_VIEWPORT m_CurrentViewport = {};                        // pixel-space viewport (drives transform + RSSetViewports)
    D3D12_RECT     m_CurrentScissor = {};

    // --- 3D world mesh path (Phase 2 first-light: flat-shaded, depth-tested, no G-buffer) ---
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_DsvHeap;         // single DSV
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_DepthBuffer;     // R32_TYPELESS (DSV D32_FLOAT / SRV R32_FLOAT), reversed-Z
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_DepthBufferAlloc; // backing D3D12MA allocation (recreated on resize)
    UINT m_DsvDescriptorSize = 0;
    UINT m_DepthSrvSlot = UINT_MAX;   // R32_FLOAT SRV of m_DepthBuffer, read by the light cull for per-tile far-Z tightening

    // World root sig + lit world-mesh PSO/blobs now live in m_Pipelines.World (RootSig/PSO/VsBlob/PsBlob).
    // That RootSig is the shared anchor bound by the VOB/skeletal/shadow-caster/point-shadow draws too.

    // ---- GPU-driven world mesh (P2.11): ExecuteIndirect + bindless diffuse. The per-material draws of BOTH the
    // depth prepass and the color pass are collapsed into ONE ExecuteIndirect each, over a per-frame command
    // buffer built once from the shared visible-section set (BuildWorldDrawCommands). One command = per-material
    // b6 { normal, orm, diffuse } bindless indices (root consts @ param 10) + DrawIndexedArguments. This removes
    // the thousands of per-draw SetGraphicsRootDescriptorTable + DrawIndexedInstanced calls (the ~70%-of-pass CPU
    // cost) and the duplicate BSP walk. The UPLOAD arg ring stays in GENERIC_READ (which includes INDIRECT_ARGUMENT).
    struct WorldDrawCommand {                       // 8 DWORDs = 32 bytes; MUST match the command signature layout
        uint32_t MatNormalIndex;
        uint32_t MatOrmIndex;
        uint32_t MatDiffuseIndex;
        D3D12_DRAW_INDEXED_ARGUMENTS Draw;          // IndexCountPerInstance, InstanceCount, Start*, BaseVertex, StartInstance
    };
    static constexpr UINT kMaxWorldDrawCommands = 16384;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_WorldIndirectCmdSig;   // b6(3 consts)@param10 + DrawIndexed
    Microsoft::WRL::ComPtr<ID3D12Resource> m_WorldDrawArgs[kBackBufferCount]; // persistently-mapped UPLOAD ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_WorldDrawArgsAlloc[kBackBufferCount];
    uint8_t* m_WorldDrawArgsPtr[kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_WorldDrawArgsGpu[kBackBufferCount] = {};
    UINT m_WorldDrawCount = 0;                       // commands built this frame (shared by both world passes)
    unsigned int m_WorldDrawnIndices = 0;            // total indices in this frame's command set (triangle counter)
    bool m_WorldDrawArgsOverflowLogged = false;
    bool CreateWorldIndirect();                      // command signature + per-frame arg ring (once, at init)
    void BuildWorldDrawCommands();                   // collect visible sections + fill arg ring (once/frame, pre-prepass)

    // Forward+ opaque depth-prepass PSOs/blobs (world + instanced VOB) live in m_Pipelines.World
    // (DepthPrepassPSO/VsBlob/PsBlob, DepthPrepassVobPSO/VobVsBlob/VobPsBlob). The skeletal depth-prepass PSO
    // + blobs live in m_Pipelines.Skeletal (DepthPrepassPSO/DepthPrepassVsBlob/DepthPrepassPsBlob). The shadow-
    // caster PSOs below still reuse those blobs via m_Pipelines.World.DepthPrepass* / m_Pipelines.Skeletal.DepthPrepass*.

    // CSM sun shadows (P2.9c). Directional shadow map = a Texture2DArray (one slice per cascade), R32_TYPELESS
    // so each slice serves a D32_FLOAT DSV and the whole array serves one R32_FLOAT SRV for later PCF sampling.
    // NORMAL-Z here (clear 1.0, LESS_EQUAL) — NOT reversed-Z like the main camera (mirrors the D3D11 caster).
    static constexpr UINT kShadowCascades = 3;
    UINT m_ShadowMapSize = 2048;   // per-cascade slice resolution; read from RendererSettings.ShadowMapSize at init (clamp 1024..4096)
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_ShadowMap;        // Texture2DArray(R32_TYPELESS), kShadowCascades slices
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_ShadowMapAlloc;   // backing D3D12MA allocation
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
    Frustum m_CascadeFrustum[kShadowCascades] = {};   // light-space view*proj per cascade (this frame)
    float m_CascadeTexelWorld[kShadowCascades] = {};   // world units / shadow texel per cascade (for the sampling normal bias)
    
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ShadowWorldDrawArgs[kShadowCascades][kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_ShadowWorldDrawArgsAlloc[kShadowCascades][kBackBufferCount];
    uint8_t*  m_ShadowWorldDrawArgsPtr[kShadowCascades][kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_ShadowWorldDrawArgsGpu[kShadowCascades][kBackBufferCount] = {};
    UINT      m_ShadowWorldDrawCount[kShadowCascades] = {};
    
    DirectX::XMFLOAT3 m_SunDirWS = { 0.0f, 1.0f, 0.0f };   // normalized world-space dir TOWARD the sun (this frame, smoothed)
    // Temporal light-direction smoothing (P2.9c-3c): the origin-anchored texel-snap grid amplifies tiny per-frame
    // sun-direction drift into a large lateral texel shift for players far from the origin (lever arm) → crawl.
    // Lerp the sun direction toward the live value so the grid orientation changes gradually, not per-frame.
    DirectX::XMFLOAT3 m_SmoothedSunDir = { 0.0f, 1.0f, 0.0f };
    bool m_SunDirInitialized = false;
    // Per-frame-in-flight shadow-sampling constant buffer (b3 in the lit passes): the cascade view-projs +
    // sun dir + strength + texel sizes, uploaded once per frame in RenderSunShadows and bound by DrawWorldMesh.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ShadowCB[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_ShadowCBAlloc[kBackBufferCount];
    uint8_t* m_ShadowCBMapped[kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_ShadowCBGpu[kBackBufferCount] = {};
    bool CreateShadowMap();            // shadow Texture2DArray + per-slice DSVs + array SRV + caster PSOs + CB ring (once, at init)
    void ComputeCascadeMatrices();     // fill m_CascadeViewProj/m_CascadeTexelWorld/m_SunDirWS from the sun + camera (simple ortho for now)
    void RenderSunShadows();           // render the opaque casters into each cascade slice from the sun's POV + upload the sampling CB

    // ---- Point-light shadow cubes (P2.10) — mirrors D3D11's shared TextureCubeArray Forward+ path. Up to
    // kMaxShadowCubes shadowed point lights, each a 6-face 128^2 cube slot in one array. NORMAL-Z (clear 1.0,
    // LESS_EQUAL) like the CSM. Faces rendered single-pass via an instanced layered VS (6 instances → the 6
    // faces via SV_RenderTargetArrayIndex, no geometry shader — needs VPAndRTArrayIndexFromAnyShaderFeeding
    // Rasterizer, present on the target AMD GPU). Sampled in the tiled point-light loop when ShadowCubeIndex>=0.
    static constexpr UINT kPointShadowCubeSize = 128;
    static constexpr UINT kMaxShadowCubes      = 32;    // tunable up to D3D11's 128; each = 6 slices @128^2 R16 (~6MB@32)
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_PointShadowCube;      // Texture2DArray(R16_TYPELESS), kMaxShadowCubes*6 slices
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_PointShadowCubeAlloc;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_PointShadowDsvHeap;   // one D16 Texture2DArray DSV (6 slices) per cube slot
    UINT m_PointShadowDsvSize = 0;
    UINT m_PointShadowSrvSlot = UINT_MAX;   // R16_UNORM TextureCubeArray SRV (all cubes), for the point-light sampler
    bool m_PointShadowInPixelState = false; // DEPTH_WRITE (caster) <-> PIXEL_SHADER_RESOURCE (lit) round-trip
    // Point-shadow caster PIPELINES (both root sigs, the four VS/PS blobs, and the three caster PSOs) now live in
    // m_Pipelines.PointShadow (RootSig/SkeletalRootSig, VsBlob/VobVsBlob/SkelVsBlob/PsBlob, CasterWorldPSO/
    // CasterVobPSO/CasterSkeletalPSO). The cube textures, DSV heaps, SRV, and per-frame rings below stay here.
    // Per-frame ring of the 6-face view-proj CB, one 512-aligned slot per shadowed light (bound as root CBV b0).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_PointShadowCB[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_PointShadowCBAlloc[kBackBufferCount];
    uint8_t* m_PointShadowCBMapped[kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_PointShadowCBGpu[kBackBufferCount] = {};
    // Per-frame TIGHT (64-byte world matrix) VOB-instance ring for the point-shadow VOB caster: only the instances
    // range-culled into a shadowed light's sphere get packed here, so cube draws stay proportional to near casters.
    static constexpr UINT kPointShadowMaxVobInstances = 8192;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_PointShadowVobInst[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_PointShadowVobInstAlloc[kBackBufferCount];
    uint8_t* m_PointShadowVobInstPtr[kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_PointShadowVobInstGpu[kBackBufferCount] = {};
    UINT m_PointShadowVobInstCapacity = 0;         // bytes
    UINT m_PointShadowVobInstOffset = 0;           // reset each frame at the top of RenderPointShadows
    bool m_PointShadowVobInstOverflowLogged = false;

    // ---- Static/dynamic split point-shadow caching (P2.10g) — the D3D11 static-aside model that lets hundreds
    // of shadowed lights update their MOVING casters every frame without re-rendering static geometry. Two
    // persistent cube arrays: m_PointShadowStaticCube holds ONLY static-caster depth (world mesh + instanced
    // VOBs), rendered per slot at most once (on slot assign / light move / range change). m_PointShadowCube is
    // the ACTIVE cube the lit pass samples: each frame it is CopyResource'd from the static cube, then the
    // DYNAMIC casters (skeletal NPCs) are overlaid (depth LESS_EQUAL, no clear). So the per-frame cost is one
    // whole-array depth copy + the handful of near dynamic draws — the expensive static cull/draw is amortized.
    // Slots are owned by light Vob identity and kept stable across frames (not reassigned by proximity).
    struct PointShadowSlot {
        zCVobLight*       owner = nullptr;   // light identity owning this slot (nullptr = free)
        DirectX::XMFLOAT3 pos = {};          // last static-rendered light position (move detection)
        float             range = 0.0f;      // last static-rendered range (range-change detection)
        bool              isStatic = false;  // Vob->IsStatic() (informational; moving lights re-render static each frame)
        bool              staticValid = false; // static-aside slot holds valid static-only depth (else must re-render static)
    };
    PointShadowSlot m_PointShadowSlots[kMaxShadowCubes];
    // Static-aside cube (P2.10g): second persistent cube array, static-caster depth only. No SRV (never sampled);
    // its content is CopyResource'd into the active cube each frame. m_PointShadowStaticState tracks its resource
    // state across frames (DEPTH_WRITE when rendering static, COPY_SOURCE while feeding the per-frame copy).
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_PointShadowStaticCube;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_PointShadowStaticCubeAlloc;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_PointShadowStaticDsvHeap; // one D16 6-slice DSV per slot (mirrors active)
    D3D12_RESOURCE_STATES m_PointShadowStaticState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    bool CreatePointShadowResources(); // both cube arrays + per-slot DSVs + array SRV + face-CB/VOB-instance rings (once, at init; PSOs in m_Pipelines.CreatePointShadow)
    void RenderPointShadows();         // static pass (dirty slots) -> copy static->active -> dynamic overlay -> PSR
    // Self-shadow exclusion for point lights attached to a carried item/NPC — without this, e.g. a torch light
    // held in an NPC's hand casts a huge shadow blob from that NPC's own body onto itself. Mirrors D3D11's
    // GetHasOriginVob + SetupVobsToExclude/CollectVobTreeToExclude (D3D11PointLight.cpp). Populates excludeOut
    // with the light vob's ancestor chain (+ any oCVisualFX origin) and returns true when non-empty; returns
    // false (excludeOut left empty) when self-shadowing is allowed, the light isn't attached to a carried item,
    // or it's a PFX-spawned light (those aren't excluded, matching D3D11's GetHasOriginVob gate).
    bool BuildPointShadowExcludeList( zCVobLight* lightVob, std::vector<const zCVob*>& excludeOut );

    // Forward+ tiled light culling (P2.9b-2): one global compute root sig + PSO; two resolution-sized
    // DEFAULT-heap UAV buffers holding the per-tile {Offset,Count} grid and the per-tile light-index slices
    // (fixed 32/tile, no global counter). All live permanently in UNORDERED_ACCESS. m_NumTilesX/Y = tile
    // grid dimensions for the current resolution.
    // Light-cull pipeline (RootSig/PSO/blob) now lives in m_Pipelines.LightCull
    Microsoft::WRL::ComPtr<ID3D12Resource> m_LightGridBuffer;    // RWStructuredBuffer<LightGrid> (numTiles * 8 B)
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_LightGridBufferAlloc;  // recreated on resize
    Microsoft::WRL::ComPtr<ID3D12Resource> m_LightIndexBuffer;   // RWStructuredBuffer<uint>  (numTiles * 32 * 4 B)
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_LightIndexBufferAlloc; // recreated on resize
    UINT m_NumTilesX = 0;
    UINT m_NumTilesY = 0;
    // Grid/index buffers round-trip UNORDERED_ACCESS (cull CS writes) -> PIXEL_SHADER_RESOURCE (lit PS reads)
    // each frame. Tracks whether they're currently in the PS-read state so DispatchLightCulling knows whether
    // it must transition them back to UAV before the next dispatch (false right after (re)creation in UAV).
    bool m_LightGridInPixelState = false;

    // Bloom pyramid (P2.11, mirrors D3D11PFX_Bloom): resolution-dependent chain of progressively half-sized
    // down[]/up[] textures, recreated on resize. Pipeline state (root sigs/PSOs/blobs) lives in m_Pipelines.Bloom;
    // these are just the GPU textures + their persistent heap slots. down[i] holds prefilter (i=0) / plain
    // downsample (i>0) results; up[i] holds the tent-upsampled composite of down[i] + the lower mip. Both chains
    // stay permanently in UNORDERED_ACCESS between dispatches (all work happens within one frame's command list,
    // no cross-frame RESOURCE-STATE to track — unlike m_SceneColor, which is also read by the next frame's UI).
    // The descriptor HEAP CONTENT for the upsample pass's variable "t0" slot is a different story (see below):
    // that one genuinely needs double-buffering, because CPU descriptor-heap writes are not synchronized to GPU
    // instruction execution — with kBackBufferCount=2 the CPU can be recording/submitting frame N's Bloom pass
    // (overwriting the shared heap slot) before the GPU has actually executed frame N-1's identical dispatch that
    // reads the SAME slot, corrupting frame N-1's sample with frame N's (wrong-resolution/wrong-resource) source.
    static constexpr int kBloomMaxMips = 6;
    static constexpr int kBloomMinMipSize = 8;
    int m_BloomMipCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_BloomDown[kBloomMaxMips];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_BloomDownAlloc[kBloomMaxMips];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_BloomUp[kBloomMaxMips];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_BloomUpAlloc[kBloomMaxMips];
    INT2 m_BloomMipSize[kBloomMaxMips] = {};
    UINT m_BloomDownSrvSlot[kBloomMaxMips] = { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX };   // down[i]'s canonical SRV (downsample-chain reads + upsample t1)
    UINT m_BloomDownUavSlot[kBloomMaxMips] = { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX };
    UINT m_BloomUpUavSlot[kBloomMaxMips] = { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX };     // up[i] UAV (i = 0..mipCount-2)
    UINT m_BloomUpSrvSlot[kBloomMaxMips] = { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX };     // up[i] canonical SRV, written after producing it; read by the next (i-1) upsample step and by the composite pass (i==0)
    // Per-level upsample SRV table: 2 CONTIGUOUS heap slots (t0=variable source, t1=down[i], fixed at creation).
    // t0 is rewritten every frame right before its dispatch (the source differs per level — down[last] for the
    // innermost level, up[i+1] otherwise); t1 is a static copy of down[i]'s SRV, written once at creation.
    // Indexed [m_FrameIndex][level] — double-buffered so frame N's t0 rewrite can never race frame N-1's
    // still-in-flight GPU read of the same slot (see the comment block above this section).
    UINT m_BloomUpSrvPairSlot[kBackBufferCount][kBloomMaxMips] = {
        { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX },
        { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX } };  // base slot of the pair; t1 = base+1
    bool CreateBloomResources( INT2 size );   // (re)builds the pyramid textures + persistent SRV/UAV slots
    void RenderBloom();                       // prefilter -> downsample chain -> upsample chain -> additive composite

    // Instanced static VOBs (reuses the shared world root sig; slot 0 = packed vertex, slot 1 = per-instance
    // data). Lit PSO/blobs now live in m_Pipelines.World (VobPSO/VobVsBlob/VobPsBlob); the buffers stay here.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_VobInstanceBuffer[kBackBufferCount]; // persistently-mapped upload ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobInstanceBufferAlloc[kBackBufferCount];
    uint8_t* m_VobInstanceBufferPtr[kBackBufferCount] = {};
    UINT m_VobInstanceBufferCapacity = 0;
    UINT m_VobInstanceBufferOffset = 0;                            // reset each OnBeginFrame
    bool m_VobInstanceOverflowLogged = false;

    // --- Forward+ dynamic point lights (P2.9a: per-frame brute-force light buffer, no tiling yet) ---
    // StructuredBuffer of the frame's visible point lights, rebuilt each frame from CollectVisibleVobs and
    // bound as a root SRV (t1) to the world/VOB pixel shaders, which loop it per pixel (N.L + attenuation)
    // on top of the baked vertex lighting. Root SRV => no descriptor-heap slot consumed.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_LightBuffer[kBackBufferCount]; // persistently-mapped UPLOAD, GPULight[]
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_LightBufferAlloc[kBackBufferCount];
    uint8_t* m_LightBufferPtr[kBackBufferCount] = {};
    UINT m_LightBufferCapacity = 0;                               // max lights per frame
    UINT m_FrameLightCount = 0;                                   // lights written this frame
    bool m_LightOverflowLogged = false;

    // Water (transparent world surfaces). Own root sig = the world layout + b2 = { time, alpha } (VS
    // scrolls the UV by time; PS uses alpha for the blend). Alpha-blended PSO: depth-test ON, write OFF.
    // Water geometry lives in the SAME wrapped world VB/IB (36-byte packed vertex, R32 indices); it is
    // peeled out of DrawWorldMesh's opaque loop and drawn here after all opaque geometry.
    // Water pipeline (RootSig/PSO/blobs) now lives in m_Pipelines.Water

    // Skeletal (animated NPC/monster) path — matrix-palette skinning. The root sig (b0 ViewProj root consts +
    // b1 per-instance CBV + b2 bone-palette CBV + t0 SRV + s0), lit + depth-prepass PSOs, and their blobs now
    // live in m_Pipelines.Skeletal. The per-frame CB ring below holds each vob's instance CB + bone matrices
    // (root CBVs into it, 256-byte aligned).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_SkeletalCBBuffer[kBackBufferCount]; // persistently-mapped upload ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_SkeletalCBBufferAlloc[kBackBufferCount];
    uint8_t* m_SkeletalCBBufferPtr[kBackBufferCount] = {};
    UINT m_SkeletalCBBufferCapacity = 0;
    UINT m_SkeletalCBBufferOffset = 0;                             // reset each OnBeginFrame
    bool m_SkeletalCBOverflowLogged = false;

    // Particle (PFX) path — instanced camera-facing billboards, one instance per live particle. The root sig,
    // shaders, and per-BlendKey PSO cache now live in m_Pipelines.Particle. Per-frame instance ring stays here.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ParticleInstanceBuffer[kBackBufferCount]; // persistently-mapped upload ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_ParticleInstanceBufferAlloc[kBackBufferCount];
    uint8_t* m_ParticleInstanceBufferPtr[kBackBufferCount] = {};
    UINT m_ParticleInstanceBufferCapacity = 0;
    UINT m_ParticleInstanceBufferOffset = 0;                       // reset each OnBeginFrame
    bool m_ParticleInstanceOverflowLogged = false;

    // Decal path — blood splats, arrows, sprites. Every decal is the same unit quad (m_DecalQuadVB),
    // instanced with a per-decal world matrix (world*offset*scale; ViewProj applies view+proj) + ghost
    // alpha. The root sig, shaders, fixed lit PSO, and per-BlendKey transparent PSO cache now live in
    // m_Pipelines.Decal. The shared unit-quad VB + per-frame instance ring stay here (GPU resources).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_DecalQuadVB;          // shared unit quad (6 verts), static
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_DecalQuadVBAlloc;
    D3D12_VERTEX_BUFFER_VIEW m_DecalQuadVBV = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_DecalInstanceBuffer[kBackBufferCount]; // persistently-mapped upload ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_DecalInstanceBufferAlloc[kBackBufferCount];
    uint8_t* m_DecalInstanceBufferPtr[kBackBufferCount] = {};
    UINT m_DecalInstanceBufferCapacity = 0;
    UINT m_DecalInstanceBufferOffset = 0;                          // reset each OnBeginFrame
    bool m_DecalInstanceOverflowLogged = false;

    std::unique_ptr<D3D12LineRenderer> m_LineRenderer;
    std::vector<std::move_only_function<void()>> m_PerFrameCleanupItems[kBackBufferCount] = {};
    bool m_PresentPending = false;
};
