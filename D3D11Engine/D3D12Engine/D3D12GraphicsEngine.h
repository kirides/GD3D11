#pragma once
#include <D3D12MemAlloc.h>
#include <atomic>
#include <deque>
#include <mutex>

#include "../BaseGraphicsEngine.h"
#include "../Frustum.h"
#include "D3D12Device.h"
#include <memory>
#include <unordered_map>
#include <vector>

#include "D3D12Texture.h"
#include "D3D12ShaderBackend.h"
#include "D3D12PipelineState.h"
#include "D3D12ShadowMap.h"
#include "D3D12PointShadows.h"

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
    // The two shadow subsystems are self-contained passes that still need the engine's frame plumbing (device,
    // allocator, SRV heap, the frame index + the shared upload rings, the indirect command signatures and the
    // shared mesh/skeletal collectors). They are extracted modules, not external clients — hence friendship
    // rather than widening the engine's public surface for them.
    friend class D3D12ShadowMap;
    friend class D3D12PointShadows;

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

    /** Settings-driven shader reload. D3D12 bakes its configuration macros (see
        D3D12ShaderBackend::AppendGlobalMacros) into every PSO at Init(), and PSOs are referenced by the
        engine-owned shadow-caster pipelines too, so live rebuilding isn't wired yet — this logs that a
        restart is needed instead of silently doing nothing like the BaseGraphicsEngine default would. */
    XRESULT ReloadShaders( ShaderCategory categories = ShaderCategory::All ) override;

    XRESULT CreateVertexBuffer( std::unique_ptr<GfxVertexBuffer>& outBuffer ) override;
    XRESULT CreateTexture( GfxTexture** outTexture ) override;
    XRESULT CreateTexture( std::unique_ptr<GfxTexture>& outTexture ) override;
    XRESULT CreateTexture(std::unique_ptr<D3D12Texture>& outTexture);

    XRESULT GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling = false ) override;
    /** Presents the current backbuffer. Invoked at the end of OnEndFrame. */
    XRESULT Present() override;

    /** In-game world render entry (zCBspNodeRender hook). Draws the static world mesh (Phase 2). */
    XRESULT OnStartWorldRendering() override;

    /** Clears the present-pending guard so the backbuffer-readback path (savegame thumbnails) can
        force a world render mid-frame. Mirrors D3D11GraphicsEngineBase::ResetPresentPending. */
    void ResetPresentPending() override { m_PresentPending = false; }

    /** Draws the wrapped static world mesh. Phase-2 first-light: flat-shaded (screen-space derivative
        normal), depth-tested, no textures / no G-buffer — just the backbuffer + a depth target.
        `noTextures` is accepted for interface parity but currently always effectively true. */
    XRESULT DrawWorldMesh( bool noTextures = false ) override;

    /** Fills the sky/background with Gothic's atmosphere (fog) color. Forward-renderer MVP: a color
        clear at the start of the world pass; distance fog in the geometry shaders fades into the same
        color so the horizon dissolves seamlessly. No sky dome/scattering yet. */
    XRESULT DrawSky() override;

    BaseLineRenderer* GetLineRenderer() override;

    /** Draws one of D3D12LineRenderer's cached line lists (see D3D12LineRenderer.cpp). Public only because
        the line renderer reaches the engine through Engine::GraphicsEngine, not an owning back-pointer.
        screenSpace=false -> world-space, ViewProj-transformed, depth-tested against the finished scene;
        screenSpace=true  -> pre-transformed xyzrhw 2D overlay, no depth. */
    void DrawLines( const std::vector<struct LineVertex>& lines, bool screenSpace );
    const std::string& GetGraphicsDeviceName() override { return m_Device.GetDeviceDescription(); }

    bool GetHdrOutputInfo( float& maxNits, float& minNits, float& maxFullFrameNits ) const override {
        if ( !m_HdrOutputActive ) return false;
        maxNits = m_HdrMonitorMaxNits;
        minNits = m_HdrMonitorMinNits;
        maxFullFrameNits = m_HdrMonitorMaxFullFrameNits;
        return true;
    }

    /** Native device for the D3D12 resource classes (D3D12Texture / D3D12VertexBuffer). */
    ID3D12Device* GetD3DDevice() const { return m_Device.GetDevice(); }

    /** Current frame-in-flight index (0..kBackBufferCount-1), stable for the whole frame. Used by
        D3D12VertexBuffer to pick which of its per-frame copies to write/bind for a dynamic (GPU-bound,
        CPU-updated-every-frame) buffer, so a frame N update can never race frame N-1's still-in-flight
        GPU reads of the same conceptual buffer (see D3D12VertexBuffer's dynamic-buffer ring). */
    UINT GetFrameIndex() const { return m_FrameIndex; }

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
        Binds the game's Gothic_XYZRHW_DIF_T1_Vertex buffer straight off the IA using the FF_VB_LAYOUT
        UI pipeline variant (no readback/conversion), falling back to a ring snapshot only when the
        current frame's copy of the ring-buffered D3D7 buffer is stale. */
    XRESULT DrawVertexBufferFF( GfxVertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Records the currently-bound diffuse texture for the next 2D draw (SetTexture -> BindToSlot). */
    void BindSurfaceTextures( int slot, GfxTexture* diffuse, GfxTexture* normalmap, unsigned int numTextures = 2 ) override;

    /** Tracks the active pixel shader ID set by the D3D7 FF layer / zBinkPlayer. D3D12 has no generic
        per-shader dispatch (unlike D3D11) — this is only consulted by DrawVertexArray to special-case
        PS_Video (Bink YUV playback); every other value keeps the normal FF/UI draw path. */
    XRESULT SetActivePixelShader( PShaderID shader ) override { m_ActivePixelShader = shader; return XR_SUCCESS; }

    /** Records a Y/U/V plane texture for the next PS_Video draw (zBinkPlayer's GfxTexture::BindToPixelShader(0/1/2)). */
    void SetVideoTextureSlot( int slot, GfxTexture* texture ) { if ( slot >= 0 && slot < 3 ) m_VideoTextures[slot] = texture; }

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
    D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandleLocked( UINT slot ) const; // caller holds m_SrvHeapMutex
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandleLocked( UINT slot ) const; // caller holds m_SrvHeapMutex
    bool CreateAllocators();
    void ResizeOutputWindow( INT2 size );  // size the OS window + inform Gothic (zCView) of the mode
    bool CreateSwapChain( INT2 size );
    bool CreateFrameResources();      // RTV heap + allocators + command list + fence + event
    bool CreateUploadObjects();       // dedicated allocator + command list + fence for synchronous uploads
    bool CreateSrvHeap();             // shader-visible CBV_SRV_UAV heap for texture SRVs
    // UI/Particle/Decal pipeline creation now lives in m_Pipelines (CreateUI/CreateParticle/CreateDecal); the
    // GetOrCreate* blend-key PSO caches moved there too. GPU ring buffers + the decal quad VB stay in the engine.
    bool CreateUIVertexBuffers();     // per-frame dynamic (upload-heap) vertex ring buffers
    bool CreateLineVertexBuffers();   // per-frame dynamic (upload-heap) debug-line vertex ring buffers
    bool CreateWhiteTexture();        // 1x1 white fallback (untextured colored 2D draws)
    bool CreateDepthBuffer( INT2 size ); // R32_TYPELESS depth target + DSV(D32) + SRV(R32) (reversed-Z world rendering)
    bool CreateSceneColorTarget( INT2 size ); // R16F HDR scene-color RT (+RTV +SRV) the 3D passes render into; recreated on resize
    void BindSceneColorTarget();      // transition HDR RT -> RENDER_TARGET (if needed) + bind it (+ depth) as the world-pass RTV
    void ResolveSceneToBackBuffer();  // tonemap the HDR scene onto the display target, then rebind it for the 2D UI

    // --- Real HDR display output (ST.2084 scanout) ---------------------------------------------------------
    // Decided once at Init from RendererSettings.HDR_Monitor + what DXGI reports about the adapter's outputs;
    // fixed for the process lifetime, because the display-buffer format is baked into every display-space PSO.
    // Tonemap.hlsl's b0 (8 root constants). HdrOutput switches the PS off the SDR operators and onto the
    // display-referred highlight roll-off; DisplayHeadroom is the panel's peak in paper-white units.
    struct TonemapRootConstants {
        float Exposure; float LumWhite; UINT ToneMapMode; UINT HdrOutput;
        float DisplayHeadroom;
        // Dynamic-exposure controls (RendererSettings.AutoExposure*), see MakeTonemapConstants.
        float MiddleGray; float AutoExposureStrength; float AutoExposureMin;
        float AutoExposureMax; float _pad[3];
    };
    static constexpr UINT kTonemapRootConstantCount = sizeof( TonemapRootConstants ) / sizeof( UINT );
    TonemapRootConstants MakeTonemapConstants( bool hdrOutput ) const;

    void   DetectHdrOutputCapability();     // fills m_HdrOutputActive + the luminance metadata (best effort)
    void   ApplySwapChainColorSpace();      // SetColorSpace1(G2084/P2020) + SetHDRMetaData; clears m_HdrEncodePQ on refusal
    bool   CreateHdrDisplayTarget( INT2 size );  // FP16 extended-sRGB composite target (+RTV +SRV); recreated on resize
    void   EncodeHdrDisplayToBackBuffer();  // final fullscreen PQ encode into the swapchain (called from Present)
    float  GetHdrMaxBrightnessNits() const; // the roll-off ceiling: monitor metadata or the user's override
    float  GetHdrPaperWhiteNits() const;    // nit level 1.0 in the display buffer maps to (clamped to the ceiling)
    // The target every "display space" pass renders into: the swapchain backbuffer normally, or m_HdrDisplay
    // when real HDR output is up. Both rest in RENDER_TARGET for the whole frame, so callers that copy out of
    // and back into it (SMAA, sharpen) need no special-casing.
    ID3D12Resource*             GetDisplayTarget() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDisplayRtv() const;
    // World/DepthPrepass/Vob pipeline creation now lives in m_Pipelines (CreateWorld/CreateDepthPrepass/CreateVob).
    void DrawDepthPrepass();          // lay down opaque world-mesh depth before the lit passes (Forward+ prepass)
    bool CreateLightCullBuffers( INT2 size ); // per-resolution tile grid + index-list UAV buffers (rebuilt on resize)
    void DispatchLightCulling();      // dispatch the tiled light cull (writes the per-tile light grid; not yet consumed)
    // Vob pipeline creation now lives in m_Pipelines.CreateVob (buffers stay: CreateVobInstanceBuffers).
    bool CreateVobInstanceBuffers();  // per-frame dynamic (upload-heap) VOB instance ring buffers
    void UploadFrameVobInstances();   // snapshot visible VOB instances into the ring ONCE (prepass + color share it)
    // Shadow-caster instance upload. ringSlot selects this pass's private slice of the shadow instance ring
    // (cascade index, or kRainInstanceRingSlot) — see m_ShadowVobInstanceBuffer. Because the slice and its
    // cursor are private to the slot, this is safe to call from a cascade's worker thread; it touches no
    // shared engine state beyond the read-only g_vobInfoVisualIndexToVisualInfo lookup.
    bool UploadVobs(const std::vector<RenderBucket>& vobs, std::vector<FrameVobUpload>& uploads, UINT ringSlot);
    void DrawVobDepthPrepass();       // lay down instanced VOB depth (alpha-clipped) into the Forward+ prepass
    XRESULT DrawVobsInstanced();      // collect visible VOBs + draw each visual instanced (textured)
    // Set a lit draw's per-material bindless normal/ORM indices (b6 root consts). matRootParam = 10 (world/VOB
    // root sig) or 12 (skeletal). Call once per material change, right after binding its diffuse SRV.
    void BindMaterialMaps( class zCTexture* tex, UINT matRootParam );
    // Bindless variant: same b6 block plus the diffuse SRV heap slot as the third constant. Used by the
    // skeletal passes, whose root signature carries no diffuse descriptor table.
    void BindMaterialMaps( class zCTexture* tex, UINT matRootParam, UINT diffuseSlot );
    void ResolveMaterialMapSlots( class zCTexture* tex, UINT* outNormalOrm ) const;
    bool CreateLightBuffer();         // per-frame point-light structured buffers (Forward+ MVP: brute-force)
    void BuildFrameLightBuffer();     // (re)fill this frame's light buffer from the collected visible lights
    void BindFrameLights( UINT srvParam = 3, UINT countParam = 4, UINT gridParam = 5, UINT indexParam = 6 );   // light SRV(t1)+count+grid(t2)+index(t3); (3,4,5,6)=world, (4,5,6,7)=skeletal, (5,6,7,8)=grass
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
    // Beyond this camera distance an .MMS node attachment (facial morph head, bow/crossbow draw mesh) stops
    // morphing and renders as its undeformed rest mesh — the literal D3D11 threshold (GothicAPI.cpp's
    // `dist < 1000` split into drawAsMorphMesh/drawRegular, plus DrawSkeletalMeshVobs' `distance < 1000`
    // morph branch). Morphing costs a CPU vertex re-upload per animation frame, so this bounds crowd cost.
    static constexpr float kMorphMeshMaxDistance = 1000.0f;
    // cascadeCount > 1 (only valid with shadowCascade >= 0) switches to the MULTI-cascade mode: cullFrustum is
    // read as an ARRAY of cascadeCount frusta (i.e. D3D12ShadowMap::CascadeFrusta()) and each prepared vob is appended to
    // every cascade list whose frustum it intersects. One pass over the registered skeletal-vob list instead of
    // kShadowCascades passes — and, more importantly, it keeps every Gothic-touching part of the skeletal
    // preparation (animation update, morph meshes, texani, bone palette, ring uploads) on the main thread while
    // the cascades themselves are culled/recorded concurrently.
    // collectGhosts: reroute ghost vobs (GetVisualAlpha()) into GothicAPI::TransparencyVobs instead of dropping
    // them, so DrawGhostVobs draws them later. ONLY the main-view animated-skeletal call passes true — it is the
    // D3D12 stand-in for the reroute D3D11 does in GothicAPI::DrawWorldMeshNaive (:1394), which this backend
    // never calls. Shadow callers must leave it false: ghosts cast no shadows on either backend.
    void PrepareFrameSkeletals( std::vector<SkeletalVobInfo*>& vobs, const Frustum* cullFrustum = nullptr, int shadowCascade = -1,
        const DirectX::XMFLOAT3* sphereCenter = nullptr, float sphereRange = 0.0f, UINT cascadeCount = 1,
        bool collectGhosts = false );
    void DrawSkeletalDepthPrepass();  // lay down skeletal base + node-attachment depth into the Forward+ prepass
    void DrawSkeletalColor();         // draw the collected skeletal base meshes + node attachments (post-cull, lit)
    // Turn this frame's g_FrameSkelDraws/g_FrameAttachDraws into the two main-view ExecuteIndirect argument
    // buffers both skeletal passes submit (T9). See CreateSkeletalIndirect / SkeletalDrawCommand below.
    void BuildSkeletalDrawCommands();
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
    void DrawVegetation();    // GVegetationBox instanced grass cards (own PSO — see D3D12PipelineState::CreateGrass)

    // Binds every frame-constant root argument of World.RootSig (b0 ViewProj, b1 fog, the Forward+ light
    // SRV/count, the CSM CB + array, the point-shadow cubes, the SSAO mask index). Shared by the lit world
    // mesh and the lit quad marks — anything drawing through World.RootSig after another root signature was
    // bound must call this first.
    void BindWorldFrameRootState( const DirectX::XMFLOAT4X4& viewProj );

    // Gothic FX geometry — quad marks and poly strips (D3D12Fx.cpp). Quad marks are LIT (they reuse
    // World.hlsl's PSMain through World.RootSig); the MUL/MUL2 marks and the poly strips are unlit through
    // the small Fx.hlsl pipeline, matching which shader each D3D11 pass binds.
    bool CreateFxVertexBuffers();     // per-frame dynamic (upload-heap) vertex ring for the poly-strip geometry
    // zCQuadMark decals (blood splatter, spell ground marks). Split in two exactly like D3D11: the opaque /
    // additive / alpha-blended marks draw with the opaque decals (depth-write on), while MUL/MUL2 marks are
    // deferred to DrawMQuadMarks and drawn with the transparent decals (depth-write off).
    void DrawQuadMarks();
    void DrawMQuadMarks();
    // Mesh-shaped particle effects (zCParticleFX emitters with visShpType 5, e.g. swarms of solid debris).
    // Called back from GothicAPI::DrawParticlesSimple, i.e. from inside DrawParticleEffects.
    void DrawFrameParticleMeshes( std::unordered_map<zCVob*, std::unique_ptr<MeshVisualInfo>>& progMeshes ) override;
    // Weapon/spell trails + lightning flashes (zCPolyStrip). Recomputes the strip/flash meshes first, exactly
    // like D3D11's "Draw PolyStrips" pass, then draws one dynamic-ring batch per texture.
    XRESULT DrawPolyStrips( bool noTextures = false ) override;
    // Bink cutscene YUV quad (zBinkPlayer.cpp) — DrawVertexArray's PS_Video special case. Same pre-transformed
    // vertex ring as the FF/UI path, but binds m_VideoTextures[0..2] through Video.RootSig/PSO instead.
    XRESULT DrawVideoVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex, unsigned int stride );
    // Copies vertex bytes into the current frame's 2D/UI upload ring; false (logged once) when it overflows.
    bool AllocateUIVertices( const void* vertices, unsigned int bytes, D3D12_GPU_VIRTUAL_ADDRESS& outGpuVA );
    // Shared tail of DrawVertexArray/DrawVertexBufferFF: UI PSO for Gothic's tracked FF state + binds + draw.
    // ffVbLayout picks the native Gothic_XYZRHW_DIF_T1_Vertex layout/VS over the ExVertexStruct one.
    XRESULT SubmitUIDraw( const D3D12_VERTEX_BUFFER_VIEW& vbv, unsigned int numVertices, unsigned int startVertex, bool ffVbLayout );
    bool AcquireBackBufferRTVs();     // (re)fetch swapchain buffers + build their RTVs
    bool ResizeSwapChain( INT2 size );
    void WaitForGpuIdle();            // full CPU/GPU flush (used on resize / teardown)
    void MoveToNextFrame();           // signal current frame's fence, advance, wait for next allocator

    /** CPU-blocks on m_Fence reaching `value`, but bounded + diagnosed instead of WaitForSingleObject(INFINITE).
        A direct-queue Signal that never *executes* (the queue is stuck on the copy-fence cross-queue Wait, or
        the frame's command list faulted) is not recoverable by TDR — the game just freezes with no log at all.
        Every kFenceWaitTimeoutMs of no progress this logs the site, the target/completed fence values, the copy
        fence's state (the only blocking primitive on our direct queue) and the device-removed reason, then keeps
        waiting. Returns false if it gave up because the device is gone. */
    bool WaitOnFrameFence( UINT64 value, const char* site );
    static constexpr DWORD kFenceWaitTimeoutMs = 2000;

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

    // --- Real HDR display output ---------------------------------------------------------------------------
    // When active, the whole post-scene chain (tonemap resolve -> Gothic's 2D UI/HUD -> SMAA -> sharpen ->
    // ImGui) composites into m_HdrDisplay instead of the swapchain. That buffer holds EXTENDED-sRGB values in
    // paper-white units (see kHdrDisplayFormat), which is why none of those passes needed an HDR variant: they
    // keep their SDR shaders and blend in the same perceptual space as before. EncodeHdrDisplayToBackBuffer
    // does the one ST.2084/Rec.2020 conversion at the very end of the frame.
    bool m_HdrOutputActive = false;   // HDR_Monitor && an HDR10-capable output was found; fixed after Init
    bool m_HdrEncodePQ = false;       // the swapchain accepted the G2084 colour space (else: SDR passthrough)
    float m_HdrMonitorMaxNits = 0.0f;          // DXGI_OUTPUT_DESC1::MaxLuminance (0 = nothing reported)
    float m_HdrMonitorMinNits = 0.0f;          // DXGI_OUTPUT_DESC1::MinLuminance
    float m_HdrMonitorMaxFullFrameNits = 0.0f; // DXGI_OUTPUT_DESC1::MaxFullFrameLuminance
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_HdrDisplay;        // kHdrDisplayFormat, resolution-sized
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_HdrDisplayAlloc;
    D3D12_CPU_DESCRIPTOR_HANDLE                 m_HdrDisplayRtv = {};   // RTV heap slot kBackBufferCount+3
    UINT m_HdrDisplaySrvSlot = UINT_MAX;                                // read bindlessly by HdrEncode.hlsl

    Microsoft::WRL::ComPtr<ID3D12Resource>         m_BackBuffers[kBackBufferCount];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_CmdAllocators[kBackBufferCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CmdList;

    Microsoft::WRL::ComPtr<ID3D12Fence> m_Fence;
    UINT64 m_FenceValues[kBackBufferCount] = {};
    HANDLE m_FenceEvent = nullptr;
    UINT   m_FrameIndex = 0;   // render-thread-only; every use above indexes per-frame GPU rings

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

    // --- Batched copy-queue uploader ---
    // A CacheIn burst can load dozens-hundreds of textures in a single frame. Doing a
    // CreateCommandList + ExecuteCommandLists + Signal + cross-queue Wait *per texture* (the old
    // path) was the cause of the massive CacheIn stutter / black frames: hundreds of the most
    // expensive D3D12 API calls on the game thread, plus a render-queue stall gated on the whole
    // copy burst draining. D3D11 shows none of this because its driver memcpies into managed staging
    // and folds the GPU copy into the normal command stream. To mirror that, all uploads in a burst
    // now record CopyTextureRegion/CopyBufferRegion into ONE open copy command list and submit ONCE
    // — at frame end (Present), on GPU-idle, or when the accumulated upload bytes cross a threshold
    // (bounds VA use during no-Present world-load bursts). Command allocator+list objects are
    // recycled by fence, so steady-state streaming creates ~0 new command lists.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_CopyBatchAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CopyBatchList;
    bool   m_CopyBatchOpen = false;   // m_CopyBatchList is Reset+recording with >=1 queued copy
    UINT64 m_CopyBatchBytes = 0;      // upload bytes accumulated in the currently open batch
    std::vector<Microsoft::WRL::ComPtr<D3D12MA::Allocation>> m_CopyBatchUploadAllocs;     // kept alive until flush
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>      m_CopyBatchUploadResources;  // "
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>      m_CopyBatchDestResources;    // copy targets, see PendingCopyRelease::DestResources

    // --- Pooled staging memory for buffer uploads ---
    // Creating a throwaway UPLOAD resource per upload cost one D3D12MA::CreateResource each (~0.1ms
    // measured), which *doubled* the allocator cost of every static vertex/index buffer: a mesh costs
    // VB + IB + shadow-IB, so a weapon/head attachment popping in fired ~10 CreateResource calls where
    // 5 were pure staging overhead. Small copies now bump-allocate out of persistently-mapped chunks
    // that are recycled by fence, so steady-state streaming creates zero upload resources. Uploads
    // larger than a chunk still fall back to a dedicated resource.
    struct StagingChunk {
        Microsoft::WRL::ComPtr<D3D12MA::Allocation> Allocation;
        Microsoft::WRL::ComPtr<ID3D12Resource>      Resource;
        uint8_t* MappedPtr = nullptr;   // persistently mapped; never unmapped until teardown
        UINT64   Capacity = 0;
        UINT64   Offset = 0;            // bump pointer; reset when the chunk returns to the free list
    };
    static constexpr UINT64 kStagingChunkSize = 4ull * 1024 * 1024;
    static constexpr size_t kMaxStagingChunks = 8;   // hard cap on 32-bit VA held by the pool (32 MB)

    std::vector<StagingChunk> m_FreeStagingChunks;      // idle, GPU is done reading them
    std::vector<StagingChunk> m_CopyBatchStagingChunks; // in use by the currently open batch
    size_t m_LiveStagingChunks = 0;                     // free + in-batch + pending; only ever grows to the cap

    struct PendingCopyRelease {
        UINT64 FenceValue = 0;
        std::vector<Microsoft::WRL::ComPtr<D3D12MA::Allocation>> UploadAllocations;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>      UploadResources;
        // The copy DESTINATIONS (texture/VB/IB being filled). The batch's command list references them,
        // and the batch lives on the copy queue — completely outside the frame-fence deferral that
        // guards D3D12Texture/D3D12VertexBuffer destruction. Gothic can create a visual and evict it
        // again before the batch is even flushed, so without this reference the destination is
        // final-released while a copy that reads/writes it is still queued.
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>      DestResources;
        std::vector<StagingChunk> StagingChunks;   // returned to m_FreeStagingChunks, not freed
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CopyAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> CopyCommandList;
    };
    std::deque<PendingCopyRelease> m_PendingCopyReleases;

    // Free-list of (allocator,list) pairs whose copies have completed on the GPU — recycled by
    // BeginCopyBatch instead of re-creating command objects each burst.
    struct CopyCmdObjects {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    Allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> List;
    };
    std::vector<CopyCmdObjects> m_FreeCopyCmdObjects;

    std::mutex m_CopyQueueMutex; // Protects the open batch, the copy queue submit, and the pending/free lists

    void ReleaseCompletedCopyResources( UINT64 fenceValue );
    bool BeginCopyBatch();            // ensures m_CopyBatchList is open (recycles or creates cmd objects). Caller holds m_CopyQueueMutex.
    void FlushTextureUploadsLocked(); // submits the open batch (1 Execute+Signal+Wait). Caller holds m_CopyQueueMutex.

    /** Bump-allocates 'size' bytes of persistently-mapped UPLOAD space charged to the currently open
        copy batch. Returns false when the request is bigger than a chunk or the pool is at its cap —
        the caller then falls back to a dedicated upload resource. Caller holds m_CopyQueueMutex and has
        already opened the batch. ('alignment' exists so the texture path can ask for the 512-byte
        D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT its placed footprints require.) */
    bool AcquireStagingSpaceLocked( UINT64 size, UINT64 alignment,
        ID3D12Resource** outResource, UINT64* outOffset, uint8_t** outCpuPtr );

public:
    /** Submits any pending batched texture/buffer uploads to the copy queue and inserts the single
        cross-queue wait on the direct queue. Called before the frame's graphics execute (Present),
        before a mid-frame sync (thumbnail readback), and before GPU-idle. Cheap no-op when nothing
        is queued. */
    void FlushTextureUploads();
private:

    INT2  m_Resolution = {};
    // Requested resolution (TriggerResize just stores it here — m_NewResolution itself lives on
    // BaseGraphicsEngine, shared with D3D11's identical deferral). Applied at the very start of the
    // NEXT OnBeginFrame — never mid-frame — so the resize always runs while the command list is
    // closed/idle (no open recording to disrupt), instead of the old per-frame-cleanup-ring deferral
    // which could stall for an unbounded number of frames waiting for m_FrameIndex to cycle back to
    // the slot the request landed in, and would leave stale null DSV/UAV bindings in the meantime.
    bool  m_SwapChainReady = false;
    bool  m_FrameOpen = false;        // true between OnBeginFrame and OnEndFrame
    float m_ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // black — the 2D UI draws over it

    // --- 2D / UI draw path (Gothic menus, fonts, HUD) ---
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SrvHeap;         // shader-visible CBV_SRV_UAV heap (texture SRVs)
    UINT m_SrvDescriptorSize = 0;
    UINT m_SrvHeapCapacity = 0;
    UINT m_SrvAllocated = 0;                                        // bump allocator (no free-list yet)
    std::vector<UINT> m_FreeSrvSlots; // Recycled descriptor indices

    // Guards m_SrvAllocated + m_FreeSrvSlots. AllocateSrvSlot() is called from D3D12Texture::CreateSRV,
    // which (once MT texture loading is re-enabled) can run on a Gothic resource-manager worker thread
    // concurrently with the render thread reading slots every draw via GetSrvCpuHandle/GetSrvGpuHandle,
    // and with FreeSrvSlot() running from a per-frame cleanup callback. Held only across the vector/
    // counter bookkeeping (+ the single CreateShaderResourceView "clear" write in FreeSrvSlot, which
    // targets a private CPU-visible descriptor slot the render thread's own draws don't write to) — an
    // uncontended lock here is a few tens of ns, negligible next to a draw call.
    mutable std::mutex m_SrvHeapMutex;

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

    // Debug/editor line ring (D3D12LineRenderer). Same persistently-mapped per-frame upload ring pattern as
    // the UI vertex ring above; the line PSOs/root sig live in m_Pipelines.Lines.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_LineVertexBuffer[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_LineVertexBufferAlloc[kBackBufferCount];
    uint8_t* m_LineVertexBufferPtr[kBackBufferCount] = {};
    UINT m_LineVertexBufferCapacity = 0;
    UINT m_LineVertexBufferOffset = 0;                             // reset each OnBeginFrame
    bool m_LineOverflowLogged = false;

    // Poly-strip vertex ring (D3D12Fx.cpp). Same persistently-mapped per-frame upload ring pattern; the strip
    // geometry is rebuilt on the CPU every frame (GothicAPI::CalcPolyStripMeshes), so it can't live in a
    // static VB the way quad marks do. D3D11 instead grows one shared TempPolysVertexBuffer on demand.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_FxVertexBuffer[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_FxVertexBufferAlloc[kBackBufferCount];
    uint8_t* m_FxVertexBufferPtr[kBackBufferCount] = {};
    UINT m_FxVertexBufferCapacity = 0;
    UINT m_FxVertexBufferOffset = 0;                               // reset each OnBeginFrame
    bool m_FxOverflowLogged = false;

    std::unique_ptr<D3D12Texture> m_WhiteTexture;         // 1x1 white fallback for untextured draws
    std::unique_ptr<D3D12Texture> m_BlackTexture;         // 1x1 black fallback for untextured draws
    std::unique_ptr<D3D12Texture> m_DefaultOrmTexture;    // 1x1 ORM default (AO 1, rough 0.5, metal 0)
    std::unique_ptr<D3D12Texture> m_DistortionTexture;    // distortion2.dds — wet-ground normal fallback (see LoadDistortionTexture)
    bool LoadDistortionTexture();                         // one-time, non-fatal load (mirrors LoadSmaaTextures)

    GfxTexture* m_CurrentTexture = nullptr;                        // diffuse bound for the next 2D draw
    PShaderID m_ActivePixelShader = PShaderID::PS_FixedFunctionPipe; // see SetActivePixelShader
    GfxTexture* m_VideoTextures[3] = {};                           // Y/U/V planes bound for the next PS_Video draw
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
    struct WorldDrawCommand {                       // 9 DWORDs = 36 bytes; MUST match the command signature layout
        uint32_t MatNormalIndex;
        uint32_t MatOrmIndex;
        uint32_t MatDiffuseIndex;
        float    MatNormalStrength;                 // 1.0 for a real normalmap; DEFAULT_NOISE_NORMALMAP_STRENGTH (0.10)
                                                      // when MatNormalIndex is the rain-distortion fallback (see BuildWorldDrawCommands)
        D3D12_DRAW_INDEXED_ARGUMENTS Draw;          // IndexCountPerInstance, InstanceCount, Start*, BaseVertex, StartInstance
    };
    static constexpr UINT kMaxWorldDrawCommands = 16384;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_WorldIndirectCmdSig;   // b6(4 consts)@param10 + DrawIndexed
    Microsoft::WRL::ComPtr<ID3D12Resource> m_WorldDrawArgs[kBackBufferCount]; // persistently-mapped UPLOAD ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_WorldDrawArgsAlloc[kBackBufferCount];
    uint8_t* m_WorldDrawArgsPtr[kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_WorldDrawArgsGpu[kBackBufferCount] = {};
    UINT m_WorldDrawCount = 0;                       // commands built this frame (shared by both world passes)
    unsigned int m_WorldDrawnIndices = 0;            // total indices in this frame's command set (triangle counter)
    bool m_WorldDrawArgsOverflowLogged = false;
    bool CreateWorldIndirect();                      // command signature + per-frame arg ring (once, at init)
    void BuildWorldDrawCommands();                   // collect visible sections + fill arg ring (once/frame, pre-prepass)

    // ---- Alpha-blended world-mesh surfaces (D3D12Transparency.cpp) — port of D3D11's
    // DrawMeshInfoListAlphablended. BuildWorldDrawCommands peels these out of the opaque command set into
    // g_FrameWorldTransparency (D3D12EngineCommon.h) and sorts them back-to-front; the pass draws them with
    // the material's own blend mode after the opaque scene + water, then re-lays their depth for the fog.
    // MT_Portal (G1 forest portals) and MT_WaterfallFoam are peeled by material TYPE into their own lists
    // and drawn as extra sub-passes with their own pixel shaders — same three-list split D3D11 has.
    static bool IsWorldMeshAlphaBlended( zCMaterial* mat );   // "does this material belong in that list?"
    void SortWorldTransparencyMeshes();                       // painter's order (far -> near), once per frame
    void DrawWorldTransparencyMeshes();                       // the pass itself (drains all three lists)
    void DrawWorldTransparencyList( std::vector<struct WorldTransparencyMesh>& list,
        D3D12PipelineState::WorldTransparencyPipeline::EKind kind, bool depthFill );   // one sub-pass

    // ---- Blended instanced VOBs (cobwebs, hanging cloth) — port of D3D11's DrawFrameAlphaMeshes.
    // BuildVobDrawCommands peels every VOB material with a BLEND/ADD alpha func out of the opaque
    // ExecuteIndirect set into g_FrameVobAlpha (D3D12EngineCommon.h); this pass replays them unlit and blended,
    // depth-tested but never depth-writing. Also in D3D12Transparency.cpp.
    void DrawVobAlphaMeshes();

    // ---- GPU-driven instanced VOBs (P2.12): ExecuteIndirect + bindless diffuse. Unlike the world mesh (one
    // shared VB/IB, so a command only carries material indices + DrawIndexed), every VOB visual/mesh has its OWN
    // vertex/index buffer and its own per-instance stream — so each command ALSO sets the two vertex-buffer views
    // (packed mesh @slot0, per-instance @slot1) + the index-buffer view before drawing. Diffuse goes bindless
    // (b6.MatDiffuseIndex root const) so no per-draw descriptor table is needed. Wind min/max height (per-visual)
    // rides as a 2-const partial write into b4 @ offset 4; the frame-global wind fields (dir/time/playerPos) are
    // set once before the ExecuteIndirect. Built ONCE per frame (BuildVobDrawCommands) and consumed by BOTH the
    // depth prepass and the color pass; the CSM cascades build their own per-cascade command set. Arg-member order
    // MUST match the command signature's pArgumentDescs order below (VBV0, VBV1, IBV, b6 consts, b4 consts, draw).
    struct VobDrawCommand {                          // 96 bytes; UINT64 members force 8-byte align, 96 % 8 == 0
        D3D12_VERTEX_BUFFER_VIEW MeshVBV;            // @0  packed ExVertexStruct stream (slot 0)
        D3D12_VERTEX_BUFFER_VIEW InstVBV;            // @16 per-instance VobInstanceInfo stream (slot 1)
        D3D12_INDEX_BUFFER_VIEW  IBV;                // @32 R16_UINT sub-mesh indices
        uint32_t MatNormalIndex;                     // @48 b6.x  (0xFFFFFFFF = no normal map)
        uint32_t MatOrmIndex;                        // @52 b6.y  (default ORM slot when no _FX)
        uint32_t MatDiffuseIndex;                    // @56 b6.z  bindless diffuse (alpha clip + albedo)
        float    WindMinHeight;                      // @60 b4[4] per-visual bbox.min.y
        float    WindMaxHeight;                      // @64 b4[5] per-visual bbox.max.y
        D3D12_DRAW_INDEXED_ARGUMENTS Draw;           // @68 (20 bytes)
        // --- Past the end of the command signature's arguments. ExecuteIndirect reads the args packed from
        // offset 0 and only requires ByteStride to COVER them, so trailing fields are free per-command payload
        // for our own compute passes. VisualIndex tells CSPatchArgs which per-visual surviving-instance count
        // to write into Draw.InstanceCount; 0xFFFFFFFF = "leave the CPU's count alone" (not GPU-culled).
        uint32_t VisualIndex;                        // @88
        uint32_t _cmdPad;                            // @92 keeps the stride 8-aligned for the next command's VBVs
    };
    // Separate caps: the main view now collects distance-only (360 degrees, GPU-culled) so it needs headroom,
    // while the CSM cascades still CPU-cull against their own frustum and keep the original budget — a shared
    // bump would multiply across 3 cascades x kBackBufferCount rings and cost real 32-bit address space.
    static constexpr UINT kMaxVobDrawCommands       = 16384;  // main view: visuals x materials x sub-meshes
    static constexpr UINT kMaxShadowVobDrawCommands = 8192;   // per shadow cascade; overflow logs + drops
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_VobIndirectCmdSig;   // VBVx2 + IBV + b6(3) + b4[4..5](2) + DrawIndexed
    Microsoft::WRL::ComPtr<ID3D12Resource> m_VobDrawArgs[kBackBufferCount];   // main-view (prepass+color share it)
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobDrawArgsAlloc[kBackBufferCount];
    uint8_t* m_VobDrawArgsPtr[kBackBufferCount] = {};
    UINT m_VobDrawCount = 0;                          // commands built this frame (shared by both main-view VOB passes)
    unsigned int m_VobDrawnTriangles = 0;            // triangles in this frame's main-view VOB command set (stats)
    bool m_VobDrawArgsOverflowLogged = false;
    // The shadow-caster variant of this pipeline (VSDepth + PSShadowClipBindless) and the per-cascade arg rings
    // it submits from live in D3D12ShadowMap; this signature is shared by both.
    bool CreateVobIndirect();                         // command signature + per-frame arg rings + shadow-caster PSO (once, at init)
    // Fill an arg buffer from the given VOB uploads; returns command count. resolveMaps=false leaves normal/orm at
    // defaults (depth/shadow passes only alpha-clip on diffuse), true resolves the full PBR material set (color pass).
    // culled=true points each command's instance stream at the GPU-compacted output buffer and stamps
    // VisualIndex so CSPatchArgs can overwrite the instance count (main view only — the cascades CPU-cull).
    // cacheIn=false resolves each material's diffuse with zCTexture::GetCacheState instead of CacheIn, making
    // the build a pure read so it can run on a cascade's worker thread (CacheIn mutates Gothic's resource
    // manager). A texture that has not been pulled in yet simply alpha-clips against black for that frame —
    // a caster silhouette, not a visible surface, so the inaccuracy never reaches the screen as a wrong pixel.
    UINT BuildVobDrawCommands( const std::vector<FrameVobUpload>& uploads, uint8_t* argPtr, bool resolveMaps,
        UINT maxCommands, bool culled = false, bool cacheIn = true );

    // ---- GPU-driven skeletal meshes + node attachments (T9): ExecuteIndirect + bindless materials ----------
    // The prerequisite was the bindless-skeletal / bindless-attachment work: neither Skeletal.RootSig nor the
    // attachment PSOs bind a t0 descriptor table any more, and ExecuteIndirect can set root constants and root
    // DESCRIPTORS but never a descriptor table. With that gone, both skeletal passes collapse the same way the
    // world/VOB ones did (P2.11/P2.12): the per-mesh CPU work (UpdateMeshLibTexAniState, CacheIn, the b6
    // material push, IASetVertexBuffers/IASetIndexBuffer, DrawIndexedInstanced) happens ONCE per frame in
    // BuildSkeletalDrawCommands, and the depth prepass + the lit color pass each become one submit over the
    // very same argument buffer (the prepass' clip-only PS simply ignores the normal/ORM constants).
    //
    // Base meshes need their own signature because each command also has to rebind the per-vob root CBVs (b1
    // instance, b2 bone palette — root descriptors, so legal indirect arguments) that the shared skinning VS
    // reads. Node attachments do NOT: they already draw through World.RootSig with exactly the VBVx2 + IBV +
    // b6 shape m_VobIndirectCmdSig describes, so they reuse that signature and VobDrawCommand verbatim (their
    // VSMainAttach/VSDepthAttach never read the b4 wind min/max the signature also writes — the instance
    // stream's wind fields are reinterpreted as Fatness/Scaling instead — so those two floats go out as 0).
    struct SkeletalDrawCommand {                     // 80 bytes; UINT64 members force 8-byte align, 80 % 8 == 0
        D3D12_GPU_VIRTUAL_ADDRESS InstCB;            // @0  root param 1 -> b1 InstanceCB (world/color/fatness)
        D3D12_GPU_VIRTUAL_ADDRESS BoneCB;            // @8  root param 2 -> b2 BonesCB (bone palette)
        D3D12_VERTEX_BUFFER_VIEW  MeshVBV;           // @16 ExSkelVertexStruct stream (slot 0)
        D3D12_INDEX_BUFFER_VIEW   IBV;               // @32 R16_UINT sub-mesh indices
        uint32_t MatNormalIndex;                     // @48 b6.x  (0xFFFFFFFF = no normal map)
        uint32_t MatOrmIndex;                        // @52 b6.y  (default ORM slot when no _FX)
        uint32_t MatDiffuseIndex;                    // @56 b6.z  bindless diffuse (alpha clip + albedo)
        D3D12_DRAW_INDEXED_ARGUMENTS Draw;           // @60 (20 bytes)
    };
    // Both are per-frame-in-flight UPLOAD rings, so keep the caps tight — the 32-bit address space is the
    // binding constraint, not the command count. A crowd of NPCs is ~100 vobs x a handful of materials x
    // sub-meshes; overflow logs once and drops the tail (same contract as the VOB/world rings).
    static constexpr UINT kMaxSkeletalDrawCommands = 4096;   // base skinned meshes (visual x material x sub-mesh)
    static constexpr UINT kMaxAttachDrawCommands   = 4096;   // node attachments (weapons/heads/held items)
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_SkeletalIndirectCmdSig;  // CBV(b1) + CBV(b2) + VBV + IBV + b6(3) + DrawIndexed
    Microsoft::WRL::ComPtr<ID3D12Resource> m_SkeletalDrawArgs[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_SkeletalDrawArgsAlloc[kBackBufferCount];
    uint8_t* m_SkeletalDrawArgsPtr[kBackBufferCount] = {};
    UINT m_SkeletalDrawCount = 0;                    // commands built this frame (prepass + color share them)
    Microsoft::WRL::ComPtr<ID3D12Resource> m_AttachDrawArgs[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_AttachDrawArgsAlloc[kBackBufferCount];
    uint8_t* m_AttachDrawArgsPtr[kBackBufferCount] = {};
    UINT m_AttachDrawCount = 0;                      // VobDrawCommands built this frame (prepass + color share them)
    unsigned int m_SkeletalDrawnTriangles = 0;       // triangles in this frame's skeletal+attachment command set (stats)
    bool m_SkeletalDrawArgsOverflowLogged = false;
    bool CreateSkeletalIndirect();                   // command signature + both per-frame arg rings (once, at init)

    // ---- GPU-driven VOB culling (Hi-Z occlusion + frustum, replaces the CPU per-VOB frustum test) ----
    // Pipeline, per frame:
    //   1. CollectVisibleVobs runs DISTANCE-ONLY (RndCullContext::drawFlags.SkipVobFrustumCull), so every
    //      in-range static VOB lands in the instance ring — including everything behind the camera.
    //   2. UploadFrameVobInstances additionally emits one VobCullVisual record per visual (local bbox +
    //      where its instances live in the ring).
    //   3. BuildVobDrawCommands fills the UPLOAD staging arg ring as before, but with InstVBV pointing at
    //      m_VobCulledInstances and VisualIndex stamped per command.
    //   4. DrawDepthPrepass lays down the WORLD MESH depth; BuildHiZ() min-reduces it into m_HiZ.
    //   5. CullVobsGPU() copies the staged args into a DEFAULT buffer, runs CSCull (frustum + Hi-Z, compacting
    //      survivors into m_VobCulledInstances) then CSPatchArgs (rewrites Draw.InstanceCount), and leaves the
    //      arg buffer in INDIRECT_ARGUMENT for both VOB passes.
    // Nothing here is required: EvaluateGpuVobCulling() turns the whole thing off (and the CPU frustum cull
    // back on) if a PSO/resource is missing or RendererSettings.GpuVobCulling is unticked.
    static constexpr UINT kMaxHiZMips = 14;         // 2^14 covers any mip-0 up to 16384 px
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_HiZ;        // R32_FLOAT mip chain, min-reduced reversed-Z depth
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_HiZAlloc;
    UINT m_HiZWidth = 0, m_HiZHeight = 0;           // mip-0 dims = HALF the render resolution
    UINT m_HiZMipCount = 0;
    UINT m_HiZSrvSlot = UINT_MAX;                   // full-mip-chain SRV, Load()ed per level by the cull CS
    UINT m_HiZMipUavSlot[kMaxHiZMips] = {};         // one UAV per level for the build passes
    bool m_HiZSlotsAllocated = false;               // slots are taken once and re-pointed on every resize
    bool m_HiZReady = false;
    bool m_HiZInSrvState = false;                   // built (UAV) <-> read by the cull CS (NON_PIXEL_SHADER_RESOURCE)
    bool CreateHiZResources( INT2 size );         // (re)builds m_HiZ + its SRV/per-mip UAV slots (resize-driven)
    void BuildHiZ();                                // depth -> mip 0 -> full chain; leaves m_HiZ readable

    // One record per visible VISUAL, consumed by VobCull.hlsl's CSCull (must match its VobCullVisual).
    struct VobCullVisual {
        DirectX::XMFLOAT3 BBoxMin;   // LOCAL-space visual bbox (MeshVisualInfo::BBox), shared by all instances
        UINT              InstanceBase;
        DirectX::XMFLOAT3 BBoxMax;
        UINT              InstanceCount;
    };
    static constexpr UINT kMaxCullVisuals = 16384;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_VobCullVisuals[kBackBufferCount];   // persistently-mapped UPLOAD
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobCullVisualsAlloc[kBackBufferCount];
    uint8_t* m_VobCullVisualsPtr[kBackBufferCount] = {};
    UINT m_VobCullVisualCount = 0;                  // records written this frame
    bool m_VobCullVisualOverflowLogged = false;
    // Single-instance GPU-side buffers: the direct queue is in-order, so frame N's draws are consumed before
    // frame N+1's cull writes — no per-frame-in-flight copies needed (and none of the 32-bit VA cost).
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_VobCulledInstances;   // DEFAULT UAV, mirrors the instance ring layout
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobCulledInstancesAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_VobVisibleCounts;     // DEFAULT UAV, uint[kMaxCullVisuals]
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobVisibleCountsAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_VobDrawArgsGpu;       // DEFAULT, the patched ExecuteIndirect args
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobDrawArgsGpuAlloc;
    bool m_VobCullReady = false;
    // Rest-state trackers (same pattern as m_LightGridInPixelState/m_AOMaskInPixelState): the draw passes leave
    // these in their read states, so CullVobsGPU flips them back at its top rather than after the draws.
    bool m_VobCulledInstancesInVertexState = false;
    bool m_VobVisibleCountsInSrvState = false;
    bool m_VobDrawArgsGpuInIndirectState = false;
    // Decided ONCE at the top of OnStartWorldRendering, before anything collects or builds: every stage below
    // (the distance-only collect, the cull-record emit, which instance buffer the commands point at, which arg
    // buffer the two VOB passes execute) has to agree, and some of them run long before the cull dispatch.
    bool m_GpuVobCullActive = false;
    bool CreateVobCullResources();                  // cull-record rings + the three DEFAULT GPU buffers (once, at init)
    bool EvaluateGpuVobCulling() const;             // settings + PSO/resource availability
    void CullVobsGPU();                             // CSCull + CSPatchArgs; call after DrawDepthPrepass/BuildHiZ
    // The buffer both VOB passes ExecuteIndirect from: the GPU-patched DEFAULT copy when culling, else the
    // per-frame CPU-written UPLOAD ring.
    ID3D12Resource* GetVobDrawArgsBuffer() const;

    // Forward+ opaque depth-prepass PSOs/blobs (world + instanced VOB) live in m_Pipelines.World
    // (DepthPrepassPSO/VsBlob/PsBlob, DepthPrepassVobPSO/VobVsBlob/VobPsBlob). The skeletal depth-prepass PSO
    // + blobs live in m_Pipelines.Skeletal (DepthPrepassPSO/DepthPrepassVsBlob/DepthPrepassPsBlob). D3D12ShadowMap's
    // caster PSOs reuse those blobs via m_Pipelines.World.DepthPrepass* / m_Pipelines.Skeletal.DepthPrepass*.

    // ---- CSM sun shadows + point-light shadow cubes: both are self-contained modules now (D3D12ShadowMap.h /
    // D3D12PointShadows.h). The engine only owns the frame ORCHESTRATION below (the deferred-recording driver,
    // the per-slot command lists, and the shared shadow CB the lit passes bind).
    D3D12ShadowMap     m_ShadowMap;
    D3D12PointShadows  m_PointShadows;


    // ---- Multi-threaded shadow culling + DEFERRED shadow command recording (plan item #7 + the overlap pass) ----
    // One command allocator AND one command list per (recording slot x frame-in-flight). An allocator may never
    // back two concurrently-recording lists, nor be reset while the GPU still consumes a list recorded from it —
    // so the pair is indexed by both the slot (= the recording thread) and the frame index. Slots
    // 0..kShadowCascades-1 are the CSM cascades; kPointShadowListIndex / kRainShadowListIndex are the point-cube
    // and rain-shadowmap passes, which now record concurrently with them instead of serially on the main thread.
    // Created once in Init(); if creation fails m_ShadowCmdListsReady stays false and every pass silently records
    // inline on m_CmdList (degrade, don't lose shadows).
    static constexpr UINT kPointShadowListIndex = kShadowCascades;       // (from D3D12ShadowMap.h)
    static constexpr UINT kRainShadowListIndex  = kShadowCascades + 1;
    static constexpr UINT kShadowRecordSlots    = kShadowCascades + 2;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_ShadowCmdAllocators[kShadowRecordSlots][kBackBufferCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_ShadowCmdLists[kShadowRecordSlots][kBackBufferCount];
    bool m_ShadowCmdListsReady = false;
    bool CreateShadowRecordCommandLists();
    // Closes + submits whatever is currently recorded in m_CmdList and immediately reopens it on the SAME
    // frame allocator (no allocator Reset, no GPU wait) so a batch of independently-recorded lists can be
    // slotted into the queue at this exact point in the frame. Used by BeginShadowRecording.
    void SubmitRecordedCommandsAndReopen();

    // ---- The deferred shadow driver, called from OnStartWorldRendering (see D3D12Scene.cpp for the rationale).
    // The three shadow passes write resources nothing else touches until the LIT passes, so their command
    // recording has no business sitting on the main thread's critical path. Split three ways:
    //   PrepareShadowPasses  — main thread ONLY: every Gothic read/mutation the passes need (BSP walks,
    //                          zCTexture::CacheIn, zCModel animation/texani, shared upload rings), flattened
    //                          into per-pass draw records that reference nothing but D3D12 handles.
    //   BeginShadowRecording — submit m_CmdList "part A", fan the pure-D3D12 recording out to the pool and
    //                          RETURN IMMEDIATELY. The main thread then records the depth prepass, the GPU VOB
    //                          cull, the light cull and SSAO into the reopened m_CmdList while the pool works.
    //   FinishShadowPasses   — join, execute the finished lists (they land in the queue ahead of the still-open
    //                          part B), re-record any slot that failed, then the post-barriers + RT rebind.
    // NOTE the CSM cascades do NOT go through this driver: each cascade is one self-contained job (cull ->
    // build -> record -> close) launched by D3D12ShadowMap::Prepare, far earlier in the frame. Only the join
    // is shared, in FinishShadowPasses. See the phase table in D3D12ShadowMap.h.
    void PrepareShadowPasses();
    void BeginShadowRecording();
    void FinishShadowPasses();
    // Resets one (slot x frame-in-flight) allocator/list pair and hands back the open list, or nullptr on
    // failure. Public to the recording lambdas only in the sense that they are defined inside member functions.
    ID3D12GraphicsCommandList* BeginShadowList( UINT slot );
    bool m_ShadowListRecorded[kShadowRecordSlots] = {};   // per slot: recorded AND closed successfully this frame
    bool m_ShadowThreadedRecord = false;   // this frame's BeginShadowRecording took the pooled-recording path
    bool m_RainShadowPassReady = false;   // PrepareRainShadowmap produced a valid camera + caster set this frame
    bool m_ShadowRecordingPending = false;   // BeginShadowRecording fanned work out; FinishShadowPasses must join
    bool m_ShadowRecordFailureLogged = false;   // one warning per session, not per frame

    // Per-frame-in-flight shadow constant buffer, bound by every lit pass. FOUR disjoint byte ranges written by
    // four owners: D3D12ShadowMap::Prepare owns the head [0, kWetnessCbOffset) (cascade view-projs + sun dir +
    // strength + texel sizes), then UploadWetnessConstants, UploadAoReprojConstants and UploadSkyIblConstants
    // each own a tail block. The buffer lives here because it is shared; see kWetnessCbOffset below.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ShadowCB[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_ShadowCBAlloc[kBackBufferCount];
    uint8_t* m_ShadowCBMapped[kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_ShadowCBGpu[kBackBufferCount] = {};
    bool CreateShadowConstantBuffer();   // the shared per-frame shadow CB ring (once, at init)
    // Bindless SRV-heap-slot resolvers for the (fully bindless) skeletal + node-attachment passes: the
    // material's cached-in engine texture's heap slot, or the 1x1 black fallback. ...Slot uses GetCacheState
    // (NOT CacheIn) so it stays a pure read — no Gothic texture load is triggered from a pass that only needs
    // an alpha cutout, which also keeps the shadow paths' "never CacheIn off the main thread" contract;
    // ...CacheIn is the main-view variant.
    UINT ResolveShadowDiffuseSlot( zCTexture* tex ) const;
    UINT ResolveDiffuseSlotCacheIn( zCTexture* tex );


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

    // SMAA anti-aliasing (runtime toggle: RendererSettings.AntiAliasingMode == AA_SMAA). Mirrors the D3D11
    // 3-pass post-FX (D3D11SMAA::Render): run on the tonemapped LDR swapchain image right after
    // ResolveSceneToBackBuffer, before Gothic's 2D UI/HUD composites on top (so the HUD stays crisp).
    //   1. Edge detection      color        -> m_SmaaEdges
    //   2. Blend-weight calc    edges+LUTs   -> m_SmaaBlend
    //   3. Neighborhood blend   color+blend  -> swapchain
    // m_LdrCopy (below) holds the copy of the tonemapped LDR image the color pass reads (the swapchain can't be
    // both the SMAA color SRV and the pass-3 RTV at once), so the effect costs one extra full-res copy per frame
    // while enabled. The area/search LUTs are precomputed static textures loaded once; edges/blend are
    // resolution-dependent (recreated on resize like the bloom pyramid). All are bound bindlessly (SRV heap
    // index -> b0 root const).
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_SmaaEdges;         // pass-1 output (R8G8B8A8)
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_SmaaEdgesAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_SmaaBlend;         // pass-2 output (R8G8B8A8)
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_SmaaBlendAlloc;
    UINT m_SmaaEdgesSrvSlot = UINT_MAX;
    UINT m_SmaaBlendSrvSlot = UINT_MAX;
    D3D12_CPU_DESCRIPTOR_HANDLE m_SmaaEdgesRtv = {};                 // RTV heap slot kBackBufferCount+1
    D3D12_CPU_DESCRIPTOR_HANDLE m_SmaaBlendRtv = {};                 // RTV heap slot kBackBufferCount+2
    std::unique_ptr<D3D12Texture> m_SmaaAreaTex;                     // precomputed area LUT (160x560 R8G8), loaded once
    std::unique_ptr<D3D12Texture> m_SmaaSearchTex;                   // precomputed search LUT (66x33 R8), loaded once
    bool m_SmaaTexturesLoaded = false;
    bool m_SmaaResourcesReady = false;                              // edges/blend created for the current resolution
    bool LoadSmaaTextures();                  // one-time: load the area + search LUTs from system\GD3D11\Textures
    bool CreateSmaaResources( INT2 size );    // (re)builds the resolution-dependent edges/blend textures + views
    void RenderSMAA();                        // 3-pass SMAA on the swapchain LDR image (guards on the toggle + resources)

    // Shared scratch copy of the tonemapped LDR swapchain image, used by every post-tonemap pass that has to
    // read the frame while writing it back (SMAA's color input, the sharpen source). Only one such pass reads
    // it at a time — they run in sequence at the end of OnStartWorldRendering — so one full-res kBackBufferFormat
    // texture serves both, which matters more than usual on 32-bit (see CLAUDE.md). Resolution-dependent
    // (recreated on resize); rests in COPY_DEST, and every user must leave it that way.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_LdrCopy;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_LdrCopyAlloc;
    UINT m_LdrCopySrvSlot = UINT_MAX;
    bool m_LdrCopyReady = false;
    bool CreateLdrCopyResource( INT2 size );  // (re)builds the LDR scratch copy + its SRV

    // Post-tonemap sharpening (RendererSettings.SharpeningMode / SharpenFactor — SHARPEN_CAS by default, same
    // as D3D11). Runs on the tonemapped LDR swapchain right after RenderSMAA, before Gothic's 2D UI/HUD
    // composites on top, mirroring D3D11's "Sharpen" render-graph pass placement.
    void RenderSharpen();                     // guards on the mode/strength, m_LdrCopyReady and the mode's PSO

    // Simple screen-space AO (plan item #4, "SAO"): resolution-dependent R8_UNORM textures (m_AOMask holds the
    // final blurred result; m_AOBlurTemp is the horizontal-blur scratch target), recreated on resize like the
    // bloom pyramid. RenderSSAO dispatches main-estimate -> blurH -> blurV (Shaders/D3D12/SSAO.hlsl) and leaves
    // m_AOMask in PIXEL_SHADER_RESOURCE for the lit geometry passes (World/Vob/Skeletal PS) to sample bindlessly.
    // m_ActiveAOMaskSrvSlot is refreshed every frame by RenderSSAO: the AO mask's slot when SSAO ran, else the
    // white texture's slot (mask = no occlusion) — mirrors D3D11's white-clear "AO disabled" default.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_AOMask;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_AOMaskAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_AOBlurTemp;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_AOBlurTempAlloc;
    UINT m_AOMaskSrvSlot = UINT_MAX;    // bindless index the lit PS reads (final blurred result)
    UINT m_AOMaskUavSlot = UINT_MAX;    // main-pass output + blur-pass-2 output
    UINT m_AOBlurTempUavSlot = UINT_MAX; // blur-pass-1 output
    // The blur passes' SRV descriptor TABLE needs its 2 entries (AO input, depth) heap-contiguous — m_DepthSrvSlot
    // lives elsewhere in the heap, so each direction gets its own 2-slot pair with a private copy of the depth
    // view. Written ONCE per (re)creation (CreateAOResources, after CreateDepthBuffer) — unlike the bloom
    // pyramid's per-frame-rewritten pair slots, nothing here changes frame-to-frame, so no double-buffering race.
    UINT m_AOBlurHPairSlot = UINT_MAX;  // [0]=AOMask SRV (raw estimate), [1]=Depth SRV
    UINT m_AOBlurVPairSlot = UINT_MAX;  // [0]=AOBlurTemp SRV, [1]=Depth SRV
    UINT m_ActiveAOMaskSrvSlot = UINT_MAX;   // this frame's bindless AO index for World/Vob/Skeletal PS (b7/b8 AOCB)
    bool m_AOResourcesReady = false;
    // m_AOMask rests in UNORDERED_ACCESS between RenderSSAO runs (its creation state) except right after a
    // successful run, which leaves it PIXEL_SHADER_RESOURCE for the lit passes' bindless read — tracks that so
    // the NEXT run knows whether it must flip it back to UNORDERED_ACCESS before the main pass writes it (mirrors
    // m_SceneColorInPixelState/m_LightGridInPixelState). Toggling AO off skips RenderSSAO entirely, so without
    // this the resource would still show PIXEL_SHADER_RESOURCE if AO is re-enabled a frame later.
    bool m_AOMaskInPixelState = false;
    // --- Previous-frame depth (the AO source) --------------------------------------------------------------
    // SSAO no longer runs off THIS frame's depth prepass: the prepass only covers world + instanced VOBs +
    // skeletal, so everything drawn later in the color pass (grass/foliage, decals, water) was invisible to
    // the AO and, worse, its screen pixels sampled the mask of whatever sat BEHIND it. Instead we snapshot
    // the COMPLETE depth buffer at the end of world rendering (CopyDepthForAO, after every depth-writing pass)
    // and run the whole AO chain off that copy on the next frame. The mask is therefore one frame stale and
    // lives in the PREVIOUS frame's screen space, so the lit pixel shaders reproject into it per-pixel:
    // world position -> m_PrevDepthViewProj -> UV (see Shaders/D3D12/include/ScreenSpaceAO.hlsl). That is exact
    // for static geometry (not a camera-motion approximation) and only lags on moving objects.
    // Side benefits: the AO chain never touches m_DepthBuffer, so RenderSSAO needs no depth barriers at all and
    // no longer serialises against the prepass.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_PrevDepth;        // full copy of last frame's D32 depth (R32_TYPELESS)
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_PrevDepthAlloc;
    UINT m_PrevDepthSrvSlot = UINT_MAX;   // R32_FLOAT SRV: SSAO's t0 AND the lit PS's bindless disocclusion test
    // Rest state is the combined shader-read state: the AO compute dispatches read it non-pixel, the lit pixel
    // shaders read it pixel-side, and CopyDepthForAO flips it to COPY_DEST and straight back once per frame.
    static constexpr D3D12_RESOURCE_STATES kPrevDepthReadState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bool m_PrevDepthValid = false;        // false until the first CopyDepthForAO of a world (reset on resize/world load)
    XMFLOAT4X4 m_PrevDepthViewProj = {};  // the camera that produced m_PrevDepth — the reprojection matrix
    XMFLOAT4X4 m_PrevDepthProj = {};      // ...and its projection, for the AO pass's depth -> view-position math
    // Reprojection tail of the shared shadow CB, written every frame by UploadAoReprojConstants (the third
    // disjoint byte range of m_ShadowCB, after the D3D12ShadowMap::Prepare head and the UploadWetnessConstants block).
    static constexpr UINT kAoReprojCbOffset = 352;
    struct AoReprojCBData {
        XMFLOAT4X4 PrevViewProj;
        UINT  PrevDepthIndex;   float PrevProjZX;   float PrevProjZY;   float ReprojValid;
    };
    void UploadAoReprojConstants();           // publishes m_PrevDepth* into the shadow CB for the lit PS
    void CopyDepthForAO();                    // end-of-world-render snapshot of the complete depth buffer
    bool CreateAOResources( INT2 size );      // (re)builds m_AOMask/m_AOBlurTemp/m_PrevDepth + persistent SRV/UAV slots
    void RenderSSAO();                        // main estimate -> separable blur; no-ops (mask stays white) if disabled/unavailable

    // ---- Motion-vector + normal G-buffer (D3D12Motion.cpp) -------------------------------------------------
    // Two extra full-resolution targets the Forward+ DEPTH PREPASS writes alongside depth, via the *GBuf PSO
    // variants (World/Skeletal). Producers only for now — TAA, FSR3 and XeGTAO are the consumers, and none of
    // them exist yet, so nothing in the frame currently READS either target. That is deliberate: it makes this
    // increment independently GPU-verifiable through the debug overlay without changing a single output pixel.
    //
    //   m_VelocityBuffer — RG16F screen-space motion, prevUV - currUV (D3D11's CalculateVelocity convention).
    //                      Cleared to kVelocitySentinel; the prepass overwrites opaque geometry with TRUE
    //                      per-object velocity (static world via camera reprojection, VOBs via the instance
    //                      stream's prevWorld, skeletals via a second skinning pass through the previous bone
    //                      pose); FillCameraVelocity then replaces whatever sentinel is left.
    //   m_NormalBuffer   — RG16F octahedral world-space normal, the XeGTAO input. Untouched by the fill pass:
    //                      a pixel with no prepass coverage has no meaningful normal, and AO must treat it as
    //                      "no geometry" rather than invent one.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_VelocityBuffer;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VelocityAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_NormalBuffer;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_NormalAlloc;
    D3D12_CPU_DESCRIPTOR_HANDLE m_VelocityRtv = {};   // RTV heap slot kBackBufferCount+4
    D3D12_CPU_DESCRIPTOR_HANDLE m_NormalRtv = {};     // RTV heap slot kBackBufferCount+5
    UINT m_VelocitySrvSlot = UINT_MAX;    // SRV for the debug overlay + future TAA/FSR3 consumers
    UINT m_VelocityUavSlot = UINT_MAX;    // UAV the FillCameraVelocity compute pass writes through (bindless)
    UINT m_NormalSrvSlot = UINT_MAX;      // SRV for the debug overlay + future XeGTAO
    bool m_MotionResourcesReady = false;  // false disables the whole feature (prepass falls back to depth-only)
    bool m_VelocityInPixelState = false;  // mirrors m_SceneColorInPixelState: tracks RT vs shader-read rest state

    // Camera history. m_PrevViewProjUnjittered is the matrix that rasterized the PREVIOUS frame, captured at the
    // end of each OnStartWorldRendering; m_MotionHistoryValid is false on the first frame of a world and right
    // after a resize, where there is no previous camera and every velocity must read as zero.
    XMFLOAT4X4 m_PrevViewProjUnjittered = {};
    XMFLOAT4X4 m_CurViewProjUnjittered = {};
    bool m_MotionHistoryValid = false;
    // Per-frame-in-flight 256-byte UPLOAD slabs holding MotionCBData, bound as a root CBV by the *GBuf prepass
    // PSOs (b5 world-family / b9 skeletal) and by the fill pass (b0). One allocation per frame in flight rather
    // than a ring: the contents are frame-global, written once in UploadMotionConstants.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_MotionCB[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_MotionCBAlloc[kBackBufferCount];
    uint8_t* m_MotionCBMapped[kBackBufferCount] = {};
    struct MotionCBData {
        XMFLOAT4X4 PrevViewProj;
        XMFLOAT4X4 UnjitteredViewProj;
        XMFLOAT4X4 InvUnjitteredViewProj;
    };
    static_assert( sizeof( MotionCBData ) == 192, "MotionCBData must match Shaders/D3D12/include/MotionVectors.hlsl" );

    bool CreateMotionResources( INT2 size );  // (re)builds the velocity/normal targets + their RTV/SRV/UAV views
    bool CreateMotionConstantBuffers();       // one-time: the kBackBufferCount persistently-mapped MotionCB slabs
    void UploadMotionConstants();             // captures this frame's camera into MotionCBData (start of world render)
    void BeginMotionGBuffer();                // clears both targets + binds them as the prepass's two RTVs
    void EndMotionGBuffer();                  // flips them back to shader-read and restores the scene-color RT
    void FillCameraVelocity();                // camera-only velocity for every pixel the prepass never covered
    void StoreVobPreviousTransforms();        // end-of-frame snapshot of vob/skeletal transforms for next frame
    void RenderMotionDebugOverlay();          // DisplayVelocity/DisplayNormals debug view over the finished image
    D3D12_GPU_VIRTUAL_ADDRESS GetMotionCbAddress() const;   // 0 when the feature is unavailable
    // ALL-OR-NOTHING gate for the G-buffer prepass. Every prepass PSO must agree with whatever render targets
    // BeginMotionGBuffer bound: if the two G-buffer RTVs are bound, a pass that fell back to its 1-RT
    // depth-only PSO would be a render-target format mismatch (device removal, not a graceful degrade). The
    // four *GBuf PSOs are created by two independent, individually non-fatal paths (CreateDepthPrepassGBuf and
    // the tail of CreateSkeletal), so a partial success is genuinely reachable — hence one gate, tested by
    // BeginMotionGBuffer and by each of the three prepass draws.
    bool MotionGBufferActive() const;

    // ---- Temporal anti-aliasing (D3D12Taa.cpp) --------------------------------------------------------------
    // Port of Intel's Graphics Optimized TAA (MIT; Shaders/D3D12/TAAResolve.hlsl carries the attribution). The
    // first real consumer of the motion-vector G-buffer above.
    //
    // Runs on the LINEAR HDR scene colour, after the fog/god-ray composition and before bloom — the standard
    // slot, and the reason the resolve keeps its history in linear HDR (see note 4 in the shader): TAA is then
    // transparent to everything downstream, scene colour in and scene colour out.
    //
    // Two history buffers ping-pong: the resolve reads m_TaaHistory[1 - m_TaaHistoryIndex] and writes
    // m_TaaHistory[m_TaaHistoryIndex], whose .a carries the accumulated confidence weight (Intel's [0.5, 1)
    // range). The result is then copied back over the scene colour for the rest of the chain.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_TaaHistory[2];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_TaaHistoryAlloc[2];
    UINT m_TaaHistorySrvSlot[2] = { UINT_MAX, UINT_MAX };
    UINT m_TaaHistoryUavSlot[2] = { UINT_MAX, UINT_MAX };
    UINT m_TaaHistoryIndex = 0;
    // TAA needs the PREVIOUS frame's depth for its disocclusion test, and cannot borrow m_PrevDepth: that one is
    // refilled by CopyDepthForAO earlier in the SAME frame, so by the time TAA runs it already holds THIS
    // frame's depth. Hence a private snapshot, taken at the end of RenderTAA.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_TaaPrevDepth;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_TaaPrevDepthAlloc;
    UINT m_TaaPrevDepthSrvSlot = UINT_MAX;
    bool m_TaaPrevDepthValid = false;
    bool m_TaaResourcesReady = false;
    // Cleared on world load / resize / whenever TAA is switched off, so a stale history can never be blended in.
    bool m_TaaHistoryValid = false;

    // Sub-pixel jitter. Uses the SAME FSR3 phase sequence D3D11PFX_TAA::AdvanceJitter does — it is already
    // linked, and sharing it makes the planned FSR3 upscaler a drop-in rather than a second jitter regime.
    // m_TaaJitterPixels is this frame's offset in PIXELS (what the resolve's depth lookup needs); the projection
    // gets it in clip units. Zero whenever TAA is off, so an un-resolved jittered image can never reach the
    // screen.
    XMFLOAT2 m_TaaJitterPixels = { 0.0f, 0.0f };
    int  m_TaaJitterIndex = 0;
    UINT m_TaaFrameNumber = 0;
    bool m_TaaJitterActive = false;

    bool CreateTaaResources( INT2 size );  // (re)builds the history pair + the private previous-depth snapshot
    void AdvanceJitter();                  // per-frame jitter into TransformProj._13/_23 (no-op when TAA is off)
    void RenderTAA();                      // the resolve dispatch + copy back over the scene colour
    bool IsTaaEnabled() const;             // AntiAliasingMode == AA_TAA and everything it needs exists

    // ---- Sky image-based lighting (indirect light for the Forward+ PBR shaders) -----------------------------
    // Replaces the flat greyscale ambient floor in PBRLighting.hlsl's ComputeSunLightingPBR
    // (`albedo * AmbientStrength * sunLum`), which gave metals NO indirect specular at all — at metallic=1 the
    // Cook-Torrance kD is 0, so armour/weapons/ore went black outside direct light — and made roughness
    // irrelevant to ambient. Two cubes, built by three compute passes (Shaders/D3D12/SkyIbl.hlsl):
    //   m_SkyEnvCube    128^2 RGBA16F, kSkyEnvMips mips. Mip 0 = the analytic sky radiance; mips 1..N are its
    //                   GGX prefilter, mip m == roughness m/(N-1) (the split-sum specular chain).
    //   m_SkyIrradCube  16^2 RGBA16F, 1 mip. Cosine-convolved irradiance (the diffuse term).
    // Both are tiny by design — ~1.05 MB and ~12 KB of VA, which is what makes this affordable in a 32-bit
    // process (a placed grid of localized probes would not be, and is deliberately left to a later stage).
    // The source is ANALYTIC, not a scene capture: D3D12 draws Gothic's fixed-function skydome (DrawSky — the
    // atmospheric-scattering path is D3D11-only), so a gradient built from Gothic's own zCSkyState master
    // colours is both cheaper and a closer match to what is actually on screen than a scattering model.
    static constexpr UINT kSkyEnvSize = 128;   // mip-0 face resolution of the specular cube
    static constexpr UINT kSkyEnvMips = 6;     // 128,64,32,16,8,4 -> roughness 0.0 .. 1.0
    static constexpr UINT kSkyIrradSize = 16;  // irradiance is very low frequency; 16^2 is plenty
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_SkyEnvCube;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_SkyEnvCubeAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_SkyIrradCube;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_SkyIrradCubeAlloc;
    UINT m_SkyEnvSrvSlot = UINT_MAX;               // full-chain TextureCube SRV — the bindless index the lit PS samples
    UINT m_SkyEnvMip0SrvSlot = UINT_MAX;           // mip-0-ONLY TextureCube SRV — the prefilter/irradiance source.
                                                   // Must be mip-0-only: the prefilter writes mips 1..N as a UAV in the
                                                   // same dispatch, so the full-chain view would put one resource in two
                                                   // states at once. RenderSkyIBL splits the barrier per subresource.
    UINT m_SkyEnvUavSlot[kSkyEnvMips] = {};        // one Texture2DArray(6-slice) UAV per mip; filled with UINT_MAX in Create
    UINT m_SkyIrradSrvSlot = UINT_MAX;             // bindless index the lit PS samples for the diffuse term
    UINT m_SkyIrradUavSlot = UINT_MAX;
    bool m_SkyIblResourcesReady = false;
    bool m_SkyIblValid = false;                    // false until the first successful build of a world (reset on world load)
    // The cubes are rebuilt only when the sky state actually moved (time of day / weather / sun), not every
    // frame — the whole chain is ~100k texels of analytic evaluation plus a 2k-texel hemisphere march, which is
    // cheap but pointless to repeat while the player stands still. m_SkyLastParams is the last-built state.
    struct SkyIblParams {
        XMFLOAT3 Zenith = {}; XMFLOAT3 Horizon = {}; XMFLOAT3 Ground = {};
        XMFLOAT3 SunDir = {}; XMFLOAT3 SunColor = {}; float SunIntensity = 0.0f;
        bool Indoor = false;
    };
    SkyIblParams m_SkyLastParams;
    bool m_SkyEnvInReadState = false;              // tracks the rest state of m_SkyEnvCube (see RenderSkyIBL's barriers)
    // Sky-IBL tail of the shared shadow CB — the FOURTH disjoint byte range of m_ShadowCB, after the
    // D3D12ShadowMap::Prepare head [0,256), UploadWetnessConstants [256,352) and UploadAoReprojConstants [352,432).
    // Riding the CB the lit shaders already bind is what keeps this feature free of root-signature churn:
    // World/Vob/Skeletal/Vegetation all declare ShadowCB at their own register and just gained four fields.
    static constexpr UINT kSkyIblCbOffset = 432;
    struct SkyIblCBData {
        UINT  IrradianceIndex;   // 0xFFFFFFFF -> the lit shaders use the old flat ambient instead
        UINT  SpecularIndex;
        float SpecularMips;      // == kSkyEnvMips, so the PS can map roughness -> mip
        // The COMPLETE ambient scale for the IBL path, premultiplied on the CPU:
        //   SkyIblIntensity (user knob) * kSkyIblNormalize (radiance unit conversion) * ShadowStrength.
        // ShadowStrength enters UNHALVED — unlike AmbientStrength, which D3D12ShadowMap::Prepare halves at night.
        // The IBL branch in ComputeSunLightingPBR therefore multiplies by this and nothing else; only the flat
        // fallback branch still uses AmbientStrength. See UploadSkyIblConstants for the reasoning.
        float Intensity;
    };
    void UploadSkyIblConstants();             // publishes the cube indices into the shadow CB for the lit PS
    bool CreateSkyIblResources();             // one-time: builds both cubes + their persistent SRV/UAV slots
    void RenderSkyIBL();                      // rebuilds the cubes when the sky state moved; no-ops otherwise

    // ---- Procedural atmospheric-scattering sky dome (D3D12Sky.cpp) ------------------------------------------
    // Replaces Gothic's fixed-function sky: ONE indexed draw over GSky's static unit sphere, coloured by the
    // AC_* scattering constants GSky::RenderSky() already computes every frame, instead of the ~5-15 separate
    // zCRenderer draws (two dome layers, colour dome, background tile, screen blend) that ZenGin's
    // zCSkyController_Outdoor::RenderSkyPre() issues and that come back through the D3D7 shim one at a time.
    //
    // Per-frame-in-flight 256-byte UPLOAD CB holding just the AtmosphereConstantBuffer the pixel shader reads
    // at b1. Small and separate on purpose: the sky is drawn very early in OnStartWorldRendering, long before
    // RenderFogAndGodRays / DrawWaterSurfaces fill their own atmosphere blocks, so it cannot share either.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_SkyCB[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_SkyCBAlloc[kBackBufferCount];
    uint8_t* m_SkyCBMapped[kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_SkyCBGpu[kBackBufferCount] = {};
    bool CreateSkyConstantBuffers();          // one-time: the per-frame-in-flight atmosphere CB ring
    // Draws the dome. Returns false WITHOUT having drawn anything if the pipeline/CB/dome mesh isn't available,
    // which is DrawSky's signal to fall back to the fixed-function sky rather than show an empty sky.
    bool DrawAtmosphereSkyDome();

    // ---- Height fog + god rays (plan item #5) — D3D12 port of D3D11's PostFX composition pass. -------------
    // Two quarter-resolution HDR textures carry the god-ray chain (mask -> radial blur), mirroring D3D11's
    // GetTempBufferDS4() pool textures; both rest in UNORDERED_ACCESS between frames like the bloom mips.
    // The composition itself needs no scene-color copy: it blends premultiplied straight onto m_SceneColor
    // (see Shaders/D3D12/HeightFog.hlsl's header for why that's equivalent to D3D11's copy-and-lerp).
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_GodRayMask;        // quarter-res, sky-only pixels of the scene
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_GodRayMaskAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_GodRayZoom;        // quarter-res, radially blurred rays
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_GodRayZoomAlloc;
    UINT m_GodRayMaskSrvSlot = UINT_MAX;
    UINT m_GodRayMaskUavSlot = UINT_MAX;
    UINT m_GodRayZoomSrvSlot = UINT_MAX;
    UINT m_GodRayZoomUavSlot = UINT_MAX;
    INT2 m_GodRaySize = { 0, 0 };             // quarter of the scene resolution (rounded up, min 1)
    bool m_FogResourcesReady = false;
    // Per-frame-in-flight constant buffer for the composition pass: 512 B split into two 256-B-aligned
    // blocks — [0,256) the HeightfogConstantBuffer (b0), [256,512) the AtmosphereConstantBuffer (b1). Both
    // are filled once per frame in RenderFogAndGodRays from the exact same GAPI/GSky values D3D11 uses.
    static constexpr UINT kFogAtmosphereCbOffset = 256;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_FogCB[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_FogCBAlloc[kBackBufferCount];
    uint8_t* m_FogCBMapped[kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_FogCBGpu[kBackBufferCount] = {};
    // True for frames where the height-fog composition actually runs. The lit geometry shaders' cheap linear
    // distance fog (MakeFogConstants) is D3D12's stand-in for this pass and MUST be suppressed while it runs,
    // or the scene is fogged twice — D3D11's world/VOB/skeletal shaders apply no distance fog at all (their
    // PS_World.hlsl includes FFFog.h but never calls ComputeFog; only the fixed-function 2D/UI emulation does).
    // Evaluated once per frame at the top of OnStartWorldRendering, before any geometry pass reads it.
    bool m_HeightFogActive = false;
    bool EvaluateHeightFogActive() const;     // DrawFog && outdoor && resources/PSOs present
    bool CreateFogResources( INT2 size );     // (re)builds the quarter-res god-ray textures + their SRV/UAV slots
    bool CreateFogConstantBuffers();          // one-time: the per-frame-in-flight composition CB ring
    void RenderFogAndGodRays();               // god-ray mask+zoom compute, then the fullscreen composition blend

    // ---- Water refraction / reflection (D3D12Water.cpp) — port of D3D11's DrawWaterSurfaces + PS_Water ----
    // Water is drawn OPAQUE and does its own see-through compositing from copies of the finished opaque
    // scene, exactly like D3D11 (which CopyTextureToRTVs the HDR backbuffer into PfxRenderer's temp buffer
    // and CopyDepthStencil()s the depth before the water Z-prepass). Both copies must be taken BEFORE the
    // water Z-prepass writes the surface's own depth, or the refraction would read water-vs-water.
    //
    // Allocated lazily on the first frame a world actually renders water (~24 MB of 32-bit VA at 1080p,
    // which menus, indoor worlds and water-free maps should not pay) and released on resize, where the GPU
    // is already idle; EnsureWaterCopyResources() then rebuilds them at the new size on the next water frame.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_WaterSceneCopy;      // pre-water HDR scene (kSceneColorFormat)
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_WaterSceneCopyAlloc;
    UINT m_WaterSceneCopySrvSlot = UINT_MAX;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_WaterDepthCopy;      // pre-water depth (R32_TYPELESS -> R32_FLOAT SRV)
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_WaterDepthCopyAlloc;
    UINT m_WaterDepthCopySrvSlot = UINT_MAX;
    INT2 m_WaterCopySize = { 0, 0 };          // resolution the two copies were built for (0,0 = not allocated)

    // reflect_cube.dds as a real TextureCube — the static sky/environment reflection D3D11 binds at t3, and
    // the fallback whenever an SSR ray misses or leaves the screen. D3D12Texture is Texture2D-only, so this
    // is loaded by a small dedicated 6-face DDS parser in D3D12Water.cpp rather than through it.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_ReflectionCube;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_ReflectionCubeAlloc;
    UINT m_ReflectionCubeSrvSlot = UINT_MAX;   // UINT_MAX => shader skips the cube (SSR/refraction only)

    // Per-frame-in-flight water CB: 512 B split into two 256-B-aligned blocks — [0,256) the WaterCB the
    // shader declares at b2, [256,512) the AtmosphereConstantBuffer at b1. Water runs long BEFORE
    // RenderFogAndGodRays, so it cannot share m_FogCB's atmosphere block (that one is only filled when the
    // height-fog composition actually runs, later in the frame).
    static constexpr UINT kWaterAtmosphereCbOffset = 256;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_WaterCB[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_WaterCBAlloc[kBackBufferCount];
    uint8_t* m_WaterCBMapped[kBackBufferCount] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_WaterCBGpu[kBackBufferCount] = {};

    bool LoadReflectionCube();                // one-time, non-fatal (mirrors LoadDistortionTexture)
    bool CreateWaterConstantBuffers();        // one-time: the per-frame-in-flight water/atmosphere CB ring
    bool EnsureWaterCopyResources();          // lazy (re)build of the scene+depth copies at m_Resolution
    void ReleaseWaterCopyResources();         // called on resize (GPU idle) so the next water frame rebuilds

    // Rain/snow particles (D3D12 rain parity, step 1: buffers + CS advance only — no draw yet). Mirrors
    // D3D11Effect's RainBufferStatic/RainBufferDrawFrom, but as plain StructuredBuffers bound via ROOT
    // SRV/UAV (see D3D12PipelineState::AdvanceRain) instead of D3D11's VBV+SRV dual-bind — the CS only
    // ever reads/writes them as StructuredBuffers, so neither needs a descriptor-heap slot at this stage.
    // Rebuilt whenever RainRadiusRange/RainHeightRange/RainNumParticles change, mirroring D3D11Effect::
    // DrawRain_CS's dirty-check (D3D11Effect.cpp:314-316).
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_RainBufferStatic;   // StructuredBuffer<RainParticleStatic>, UPLOAD heap, written once
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_RainBufferStaticAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_RainBufferDynamic;  // RWStructuredBuffer<RainParticleDynamic>, DEFAULT heap
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_RainBufferDynamicAlloc;
    UINT  m_RainParticleCount = 0;       // particle count backing the CURRENT buffers (128-aligned, like D3D11's alignedCount)
    float m_RainLastRadius = -1.0f;
    float m_RainLastHeight = -1.0f;
    UINT  m_RainLastNumParticles = 0;
    bool  m_RainBuffersReady = false;
    // m_RainBufferDynamic is created in COMMON (so its one-time initial-data CopyBufferRegion via
    // UploadBufferData can target it — buffers implicitly promote COMMON->COPY_DEST for a copy) and must
    // flip to UNORDERED_ACCESS exactly once before its first CS write; tracks that pending transition.
    bool  m_RainDynamicNeedsInitialBarrier = false;
    // m_RainBufferDynamic round-trips UNORDERED_ACCESS (AdvanceRain's CS writes it) -> NON_PIXEL_SHADER_
    // RESOURCE (DrawRainParticles' VS reads it as a root SRV) each frame, mirroring m_LightGridInPixelState.
    // true right after DrawRainParticles leaves it in the read state; AdvanceRain flips it back before
    // its next dispatch.
    bool  m_RainDynamicInReadState = false;
    bool CreateRainBuffers( UINT numParticles );   // (re)builds the static/dynamic particle buffers
    void AdvanceRain();                             // dispatches the CS advance pass; no-op if disabled/unavailable
    void DrawRainParticles();                       // billboard draw; no-op if disabled/unavailable

    // Rain/snow texture arrays (D3D12 rain parity, step 3): 370-slice raindrop / 256-slice snowflake
    // R8 Texture2DArrays, loaded once and sampled bindlessly (ResourceDescriptorHeap) by Shaders/D3D12/
    // Rain.hlsl's PS. Parsed via the shared DDSArrayLoader.h ParseTextureArrayDDS (device-agnostic CPU
    // parse — D3D11's D3D11Effect::LoadRainResources uses the same parser for its own D3D11 arrays).
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_RainTextureArray;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_RainTextureArrayAlloc;
    UINT m_RainTextureArraySrvSlot = UINT_MAX;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_SnowTextureArray;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_SnowTextureArrayAlloc;
    UINT m_SnowTextureArraySrvSlot = UINT_MAX;
    bool m_RainTexturesLoaded = false;
    // One-time (or on-demand retry) load of both arrays; non-fatal on failure — DrawRainParticles guards
    // on m_RainTexturesLoaded and stays on the step-2 flat-white placeholder if this never succeeds.
    bool LoadRainTextures();
    bool LoadRainTextureArray( const char* prefix, int count, Microsoft::WRL::ComPtr<ID3D12Resource>& outTex,
        Microsoft::WRL::ComPtr<D3D12MA::Allocation>& outAlloc, UINT& outSrvSlot );

    // Rain shadowmap (D3D12 rain parity, step 4): a single-slice normal-Z depth map rendered from an
    // orthographic camera looking along the (inverted) rain-velocity direction. Casters are the world mesh
    // (per-draw root consts through D3D12ShadowMap::GetWorldCasterPSO()/m_Pipelines.World.RootSig — see the
    // comment in PrepareRainShadowmap for why non-indirect is fine there) plus the instanced VOBs, which go
    // through the shared VOB command signature as ONE ExecuteIndirect, exactly like a CSM cascade. That is
    // what makes alpha-tested tree canopies, wagons, roofs and market stalls shelter the ground and the
    // raindrops under them. Grass is deliberately NOT a caster: vegetation cards are thin, dense and
    // ground-level, so they would only self-shadow the terrain they stand on.
    static constexpr UINT kRainShadowMapSize = 2048;
    // World-space edge length of the orthographic rain camera, i.e. the map covers ±kRainShadowWorldSpan/2
    // around the player: 49152 units = ~±245 m, at 24 world units (~24 cm) per texel. Sized by COVERAGE, not
    // by the CSM's texel-density rule — outside the map the wetness lookup has to assume open sky, so a
    // border inside view distance shows up as a hard dry line on distant ground. 24 cm/texel still resolves a
    // wagon or a tree crown to a few dozen texels, which is all a rain occluder needs.
    static constexpr float kRainShadowWorldSpan = 49152.0f;
    // Asymmetric depth slab, D3D11Effect::DrawRainShadowmap's numbers: eye pulled kRainShadowUpRange ABOVE
    // the camera (so roofs overhead are captured), far plane at kRainShadowDepth (so ~14000 units of valley
    // BELOW the camera stay inside the map when the player stands on a hill).
    static constexpr float kRainShadowUpRange = 6000.0f;
    static constexpr float kRainShadowDepth = 20000.0f;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_RainShadowMap;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_RainShadowMapAlloc;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_RainShadowDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_RainShadowDsv = {};
    UINT m_RainShadowSrvSlot = UINT_MAX;
    bool m_RainShadowResourcesReady = false;
    // Round-trips DEPTH_WRITE (PrepareRainShadowmap writes it) -> ALL_SHADER_RESOURCE each frame, mirroring
    // the CSM map's own DEPTH_WRITE<->read round-trip. ALL_ (not NON_PIXEL_) because two stages read it the same frame: DrawRainParticles'
    // VS (via ResourceDescriptorHeap) and the lit World/Vob/Skeletal PS wetness lookup.
    bool m_RainShadowInReadState = false;
    XMFLOAT4X4 m_RainShadowViewProj = {};   // this frame's combined rain-shadow view*proj (see CB layout note)
    // False until PrepareRainShadowmap has actually produced a camera. Sampling the wetness with the
    // zero-initialized matrix above would divide by w == 0 and feed NaN UVs into the PCF loop.
    bool m_RainShadowViewProjValid = false;
    Frustum m_RainShadowFrustum;
    // Instanced-VOB rain casters. Same shape as a CSM cascade's VOB casters (D3D12ShadowMap's
    // m_VobDrawArgs/m_VobDrawCount): CollectVisibleVobs against the rain frustum fills m_RainShadowVobs,
    // UploadVobs + BuildVobDrawCommands turn it into one ExecuteIndirect through m_VobIndirectCmdSig, and
    // the bindless alpha-clip caster PSO (PSShadowClipBindless) is what makes tree canopies shelter what is
    // under them instead of casting a solid quad. Own (smaller-capped) ring rather than sharing the
    // main-view one: this pass is built at a different point in the frame and must not disturb it.
    static constexpr UINT kMaxRainVobDrawCommands = 8192;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_RainVobDrawArgs[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_RainVobDrawArgsAlloc[kBackBufferCount];
    uint8_t* m_RainVobDrawArgsPtr[kBackBufferCount] = {};
    UINT     m_RainVobDrawCount = 0;   // built by PrepareRainShadowmap, consumed by RecordRainShadowmap
    // Bucket-per-visual collection target; grown by OnAddVob and reset by OnLoadWorld alongside the
    // main-view / per-cascade views, since the bucket index IS the visual index.
    RenderView m_RainShadowVobs;
    bool CreateRainShadowResources();   // one-time (or on-demand retry) DSV/SRV + resource creation
    bool CreateRainVobArgRings( UINT commandStride );   // called from CreateVobIndirect, next to the cascade rings
    // Split for deferred recording like the other two shadow passes: PrepareRainShadowmap computes the rain-light
    // camera and resolves the visible world-mesh + VOB casters (BSP walks, bindless material indices, instance
    // ring upload) on the main thread; RecordRainShadowmap issues the resulting depth-only draws into whichever
    // list it is handed.
    void PrepareRainShadowmap();
    void CollectRainShadowVobs();   // the VOB half of PrepareRainShadowmap (cull -> instance upload -> arg build)
    void RecordRainShadowmap( ID3D12GraphicsCommandList* cmdList );

    // Scene wetness ("wet ground"): the port of D3D11's deferred ApplySceneWettness into the Forward+ lit
    // pixel shaders (Shaders/D3D12/include/Wetness.hlsl). All of its inputs ride in the TAIL of the shared
    // per-frame shadow CB (m_ShadowCB), starting at kWetnessCbOffset — D3D12ShadowMap::Prepare owns bytes
    // [0, kWetnessCbOffset) and this owns [kWetnessCbOffset, ...). It must run AFTER PrepareRainShadowmap
    // (which computes m_RainShadowViewProj for this frame) but before any lit draw binds the CB.
    static constexpr UINT kWetnessCbOffset = 256;
    struct WetnessCBData {
        XMFLOAT4X4 RainViewProj;
        float SceneWetness; float RainFxWeight; float RainTime; UINT RainShadowIndex;
        UINT  DistortionIndex; float RainShadowMapSize; float _pad0; float _pad1;
    };
    void UploadWetnessConstants();

    // Dynamic exposure / auto-exposure: a two-pass GPU luminance reduction of the finished HDR scene color,
    // temporally adapted (Pattanaik's technique) toward last frame's value, feeding Tonemap's exposure divisor
    // (mirrors D3D11's D3D11PFX_HDR ping-pong PS_PFX_LumConvert/LumAdapt, but reduces on compute instead of a
    // mip chain). All buffers are root SRV/UAV (StructuredBuffers, no descriptor-heap slots needed).
    // m_LumPartialBuffer is resolution-dependent (recreated on resize like the bloom pyramid): CS_LumReduce
    // writes one {sum,count} per 16x16 thread group, CS_LumAdapt reads + fully consumes it every frame, so it
    // needs no persistent cross-frame state (always UNORDERED_ACCESS at the top of RenderLuminanceAdapt).
    // m_LumAdaptedBuffer is a FIXED single-float buffer created ONCE at Init and never resized — the sole
    // cross-frame GPU state, holding last frame's adapted luminance for CS_LumAdapt to blend against; Tonemap's
    // PSO reads it unconditionally every frame, so its creation is a fatal Init failure like the tonemap PSO
    // itself, not an opt-in effect like bloom.
    UINT m_LumGroupsX = 0, m_LumGroupsY = 0;      // reduce-pass dispatch dims for the current resolution
    UINT m_LumPartialCapacity = 0;                // groups the current m_LumPartialBuffer can hold
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_LumPartialBuffer;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_LumPartialBufferAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_LumAdaptedBuffer;       // RWStructuredBuffer<float>[1], persistent
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_LumAdaptedBufferAlloc;
    D3D12_RESOURCE_STATES m_LumAdaptedBufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool m_LumAdaptInitialized = false;   // false until the first successful adapt dispatch (snap instead of blend)
    bool CreateLumPartialBuffer( INT2 size );   // (re)builds the resolution-dependent per-group partial-sum buffer
    bool CreateLumAdaptedBuffer();               // one-time: the persistent single-float adapted-luminance buffer
    void RenderLuminanceAdapt();                 // dispatch reduce -> adapt; called before ResolveSceneToBackBuffer

    // Instanced static VOBs (reuses the shared world root sig; slot 0 = packed vertex, slot 1 = per-instance
    // data). Lit PSO/blobs now live in m_Pipelines.World (VobPSO/VobVsBlob/VobPsBlob); the buffers stay here.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_VobInstanceBuffer[kBackBufferCount]; // persistently-mapped upload ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobInstanceBufferAlloc[kBackBufferCount];
    uint8_t* m_VobInstanceBufferPtr[kBackBufferCount] = {};
    UINT m_VobInstanceBufferCapacity = 0;
    UINT m_VobInstanceBufferOffset = 0;                            // reset each OnBeginFrame
    bool m_VobInstanceOverflowLogged = false;

    // ---- Shadow-caster VOB instance ring ----------------------------------------------------------------
    // A SEPARATE ring from the main-view one above, statically partitioned into kShadowInstanceRingSlots equal
    // slices: one per CSM cascade, plus one for the rain shadowmap. Each shadow pass writes only its own slice
    // with a LOCAL cursor, which is what lets a cascade's instance upload run on its own worker thread — the
    // main ring's m_VobInstanceBufferOffset is a shared non-atomic cursor and could never be bumped
    // concurrently. Also keeps the shadow passes out of m_VobInstanceBuffer entirely, which the GPU VOB cull
    // binds as an SRV (D3D12Cull.cpp) and sizes its own buffers against.
    // Slice index: cascade c uses slot c; the rain shadowmap uses kRainInstanceRingSlot.
    static constexpr UINT kShadowInstanceRingSlots = kShadowCascades + 1;
    static constexpr UINT kRainInstanceRingSlot    = kShadowCascades;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ShadowVobInstanceBuffer[kBackBufferCount];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_ShadowVobInstanceBufferAlloc[kBackBufferCount];
    uint8_t* m_ShadowVobInstanceBufferPtr[kBackBufferCount] = {};
    UINT m_ShadowInstanceSliceCapacity = 0;   // bytes per slot (NOT the whole buffer)
    // Per-slot, so one busy cascade overflowing doesn't silence the warning for the others. Reset each
    // OnBeginFrame like the other ring warn-once flags — a drop is never silent.
    bool m_ShadowInstanceOverflowLogged[kShadowInstanceRingSlots] = {};

    // Wind sway (flags/foliage) global state — windDir/globalTime advanced once per frame in OnBeginFrame via
    // the shared UpdateWindAnimation() (WindAnimation.h, also used by D3D11GraphicsEngine::ApplyWindProps);
    // minHeight/maxHeight/playerPos are refreshed per-visual/per-frame right before each bind (no WindMetaData
    // structured buffer yet — flat bounding-box fallback only, matching D3D11's non-WIND_META_SRV path).
    VS_ExConstantBuffer_Wind m_WindBuffer = {};

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
    // Deferred resource releases, tagged with the ordinal of the frame they were queued in.
    //
    // This deliberately does NOT key off m_FrameIndex: that index aliases every kBackBufferCount frames,
    // so a worker thread that read it just before MoveToNextFrame advanced (or in the window between the
    // frame's Signal and that advance) would file its job under a slot that gets drained after the WRONG
    // frame's fence — one frame too early — and final-release a resource the GPU is still reading
    // (OBJECT_DELETED_WHILE_STILL_IN_USE / d3d12SDKLayers "referenced by GPU operations in-flight").
    // A monotonic ordinal can't alias, and reading it under the same mutex as the push makes the
    // read+file atomic, so the worst a racing worker can do is tag its job with the NEXT frame — which
    // only ever defers the release longer.
    struct PendingCleanupJob {
        uint64_t FrameOrdinal = 0;
        std::move_only_function<void()> Job;
    };
    std::deque<PendingCleanupJob> m_PendingCleanupJobs;
    // Ordinal of the frame currently being recorded. Bumped (under m_CleanupMutex) at the top of
    // MoveToNextFrame, before the frame's fence Signal. Starts at 1 so ordinal 0 is never in flight.
    uint64_t m_CleanupFrameOrdinal = 1;
    // Guards m_PendingCleanupJobs + m_CleanupFrameOrdinal: QueueCleanupJob can run on a Gothic
    // resource-manager worker thread (D3D12Texture/D3D12VertexBuffer create/destroy queuing an
    // SRV-slot/allocation release), concurrently with the render thread draining in MoveToNextFrame or
    // the destructor's forced cleanup. Callbacks are moved out and run AFTER releasing the lock (see
    // MoveToNextFrame/destructor) so an arbitrary callback body never executes while this mutex is held.
    std::mutex m_CleanupMutex;
    bool m_PresentPending = false;
};
