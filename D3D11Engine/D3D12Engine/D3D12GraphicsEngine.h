#pragma once
#include <D3D12MemAlloc.h>
#include <atomic>
#include <deque>
#include <mutex>

#include "../BaseGraphicsEngine.h"
#include "../Frustum.h"
#include "../MorphGpu.h"         // MorphGpu::Job / ChannelRecord (the morph-fold queue members below)
#include "../TransparencyQueue.h" // the frame's sorted alpha-blended draw list
#include "../WorldConverter.h"   // SHADOW_LOD_FIRST_CASCADE
#include <span>
#include "D3D12Device.h"
#include <memory>
#include <unordered_map>
#include <vector>

#include "D3D12Texture.h"
#include "D3D12ShaderBackend.h"
#include "D3D12StateCache.h"
#include "D3D12PipelineState.h"
#include "D3D12ShadowMap.h"
#include "D3D12VobArena.h"
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
    // Same deal: the VOB mega-buffer arena needs the allocator + the fence-deferred cleanup to swap its
    // buffers out from under frames that may still be reading them.
    friend class D3D12VobArena;

public:
    /** Compile-time array-sizing bound for every per-frame resource ring (1 current + up to 2 queued).
        Arrays are always sized to this; kBackBufferCount below decides how many slots are actually used. */
    static constexpr UINT kBackBufferMax = 3;

    /** Actual configured frame-in-flight count (<= kBackBufferMax). Set once in the constructor from
        RendererSettings.LowLatency, before D3D12ShadowMap::Attach/D3D12PointShadows::Attach copy it -
        never changes afterwards (switching it requires a restart, like D3D11's swapchain waitable flag). */
    UINT kBackBufferCount = 3;

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

    HANDLE GetFrameLatencyWaitableObject() const override { return m_FrameLatencyWaitableObject; }

    /** Settings/ImGui-driven shader hot-reload. Only RECORDS the request (ORs into m_PendingShaderReload) —
        callable any number of times per frame (e.g. an ImGui button held/spammed, or several settings
        changes in one frame each requesting a reload) with no GPU work done here at all, so it can never
        itself stall or race. The actual recompile runs once, at the next OnBeginFrame's
        ApplyPendingShaderReload() checkpoint — see that method for why there and how failures are handled.
        D3D12 doesn't distinguish ShaderCategory subsets the way D3D11 does; any non-None request reloads
        every D3D12 pipeline (see D3D12PipelineState::ReloadAll). CSM/point-shadow shadow-CASTER PSOs
        (owned by D3D12ShadowMap/D3D12PointShadows, not D3D12PipelineState) are not yet part of this and
        keep whatever shader they were built with until the next full engine restart. */
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

    /** Fog-color fill + Gothic's atmosphere solve (RenderSky). Runs at the top of the world pass, before
        the shadow prepare and the depth prepass, because RenderSkyIBL and the wetness/fog constants read
        what it computes. Sets m_SkyGeometryPending for DrawSky. */
    XRESULT PrepareSky();

    /** The sky's actual geometry (atmosphere dome or Gothic's fixed-function RenderSkyPre), submitted AFTER
        the opaque depth prepass so the far-plane-pinned sky is depth-rejected behind geometry instead of
        shading 2.6 screens' worth of fragments that get painted over. See the body for the ordering rules. */
    XRESULT DrawSky() override;

    /** Set by PrepareSky: false indoors (zCSkyControler_Indoor draws nothing) or on a closed frame. */
    bool m_SkyGeometryPending = false;

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
    bool UploadBufferData( ID3D12Resource* dst, const void* srcData, UINT64 sizeInBytes ) {
        return UploadBufferData( dst, 0, srcData, sizeInBytes );
    }
    /** dstOffset variant, for filling one slice of a larger DEFAULT-heap buffer (the VOB arena). */
    bool UploadBufferData( ID3D12Resource* dst, UINT64 dstOffset, const void* srcData, UINT64 sizeInBytes );
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

    /** Render resolution; same split D3D11 has between m_scaledResolution and Resolution. */
    INT2 GetResolution() override { return m_Resolution; }
    /** Native swapchain/window size. */
    INT2 GetBackbufferResolution() override { return m_BackbufferResolution; }

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
    // Purges the VOB arena's cache of this MeshInfo* before it's freed - see BaseGraphicsEngine's doc comment.
    void OnMeshInfoDestroyed( MeshInfo* mesh ) override { m_VobArena.Forget( mesh ); }
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

    // --- Render-resolution scaling (RendererSettings.ResolutionScalePercent) ---------------------------------
    static INT2 ComputeRenderResolution( INT2 backbufferSize );        // backbuffer * ResolutionScalePercent
    static float ComputeMipLodBias( INT2 renderSize, INT2 displaySize );
    bool RebakeMipLodBias( float newBias );                            // false = old pipelines kept
    bool CreateRenderResolutionTargets( INT2 renderSize );             // fatal only for depth/scene color
    void CreateDisplayResolutionTargets( INT2 displaySize );           // post-tonemap targets, all non-fatal
    void ApplyPendingResolutionScale();

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
    bool CreateLightCullBuffers( INT2 size ); // per-resolution clustered light-grid UAV buffer (rebuilt on resize)
    void DispatchLightCulling();      // dispatch the clustered light cull (writes the per-cluster mask grid)
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
    void BindFrameLights( UINT srvParam = 3, UINT countParam = 4, UINT gridParam = 5 );   // light SRV(t1)+count+cluster-mask(t2); (3,4,5)=world, (4,5,6)=skeletal, (5,6,7)=grass
    void DrawWaterSurfaces() override; // draw water peeled out of the opaque world pass (scrolled UV, blended)
    // Skeletal (animated NPC/monster) pipeline creation now lives in m_Pipelines.CreateSkeletal (root sig + lit +
    // depth-prepass PSOs). The per-frame skeletal CB ring stays here:
    bool CreateSkeletalConstantBuffers(); // per-frame dynamic (upload-heap) skeletal CB ring (instance + bones)
    // Once/frame anim update + upload bone/inst CBs + attachment instances (pre-cull).
    // cullFrustum / sphereCenter+sphereRange are mutually exclusive: the cascades cull against the CASCADE
    // frustum (a caster invisible to the player can still cast a visible shadow), point lights against the
    // LIGHT's sphere. shadowCascade >= 0 routes the records into that cascade's list, -2 into the
    // point-shadow scratch lists, else the main-view ones. The per-vob upload itself stays cached once per
    // frame (g_SkelUploadCache) however many cull passes touch the vob.
    // Beyond this camera distance an .MMS node attachment stops morphing and renders as its undeformed rest
    // mesh. Same threshold D3D11 uses (GothicAPI.cpp's `dist < 1000`).
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
    // diffuse sample, alpha *= per-vob GhostAlpha). Drawn by DrawGhostRun; DrawTransparencyQueue clears the
    // list unconditionally, so it cannot leak.
    void DrawVegetation();    // GVegetationBox instanced grass cards (own PSO — see D3D12PipelineState::CreateGrass)
    // Grass in the Forward+ DEPTH PREPASS. Runs from the prepass block, after the skeletal draws. Its whole
    // purpose is the AO mask: RenderSSAO builds that from the prepass depth, so without this a grass pixel
    // would sample the occlusion of the terrain BEHIND the blade and the blades would cast none of their own.
    // It also gives grass true prepass velocity/normals instead of FillCameraVelocity's depth reprojection.
    //
    // HARD RANGE LIMIT, independent of OutdoorSmallVobDrawRadius: the prepass rasterizes every blade a second
    // time, and at distance the cards are sub-pixel alpha-test noise whose AO contribution is invisible while
    // the fill cost is not. Beyond it grass is simply absent from the mask and reads the terrain's AO, which is
    // the pre-existing behaviour and barely noticeable at that distance.
    static constexpr float kVegetationPrepassRange = 7500.0f;
    void DrawVegetationDepthPrepass();
    // The per-frame GrassCB (b1). Built ONCE per frame and pushed identically by the prepass, the CSM caster
    // and the lit pass — every grass VS derives its swayed world position from these, so a pass that saw a
    // different G_Time would place the blade somewhere else and z-fight against the prepass' own depth.
    struct GrassCBData { float Time; float WindStrength; float HeroAffectStrength; float PrevTime; XMFLOAT3 PlayerPosWS; float _pad1; };
    GrassCBData MakeGrassConstants() const;

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
    // Both blend classes go through DrawQuadMarkRun; MUL/MUL2 is no longer deferred to a second pass.
    // Mesh-shaped particle effects (zCParticleFX emitters with visShpType 5, e.g. swarms of solid debris).
    // Called back from GothicAPI::DrawParticlesSimple, i.e. from inside DrawParticleEffects.
    void DrawFrameParticleMeshes( std::unordered_map<zCVob*, std::unique_ptr<MeshVisualInfo>>& progMeshes ) override;
    // Weapon/spell trails + lightning flashes: DrawPolyStripRun, one dynamic-ring batch per texture.
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

    /** Dumps DRED auto-breadcrumbs (+ the CPU-side scope context recorded alongside them) to Log.txt,
        shows the player a message box with the removed-reason code, then terminates the process. Called
        from every path that observes the device is gone (Present() returning DXGI_ERROR_DEVICE_REMOVED,
        and WaitOnFrameFence giving up) so a lost device is never silently swallowed. */
    [[noreturn]] void HandleDeviceRemoved( HRESULT removedReason, const char* context );

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
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>   m_RtvHeap;   // kBackBufferMax swapchain RTVs + 1 HDR scene-color RTV
    UINT m_RtvDescriptorSize = 0;

    // HDR scene-color pipeline (Phase 3): the 3D world passes render into m_SceneColor (R16F, values >1 allowed —
    // sun + additive point lights no longer clip), then ResolveSceneToBackBuffer tonemaps it into the swapchain.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_SceneColor;           // R16G16B16A16_FLOAT, resolution-sized
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_SceneColorAlloc;       // backing D3D12MA allocation (recreated on resize)
    D3D12_CPU_DESCRIPTOR_HANDLE                  m_SceneColorRtv = {};    // RTV heap slot kBackBufferMax
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
    D3D12_CPU_DESCRIPTOR_HANDLE                 m_HdrDisplayRtv = {};   // RTV heap slot kBackBufferMax+3
    UINT m_HdrDisplaySrvSlot = UINT_MAX;                                // read bindlessly by HdrEncode.hlsl

    // --- Shader hot-reload --------------------------------------------------------------------------------
    // Coalesced request bitmask: ReloadShaders() only ORs into this (see its header comment) — never touches
    // the GPU or m_Pipelines directly, so calling it any number of times in a frame (a spammed ImGui button,
    // several settings toggles) is always safe and collapses to exactly one recompile pass.
    ShaderCategory m_PendingShaderReload = ShaderCategory::None;
    // Runs the coalesced request, if any: called once per frame from OnBeginFrame, at the same checkpoint
    // TriggerResize()'s pending resize is applied — after the previous frame's command list is
    // Closed+Executed+Presented (nothing is mid-recording) and before this frame's allocator Reset, so it is
    // safe to fully stall the GPU here. See the .cpp for the flush + rollback-on-fatal-failure sequence.
    void ApplyPendingShaderReload();

    Microsoft::WRL::ComPtr<ID3D12Resource>         m_BackBuffers[kBackBufferMax];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_CmdAllocators[kBackBufferMax];
    // The frame's direct command list, behind the engine-wide redundant-state filter (D3D12StateCache.h).
    // Reads exactly like the ComPtr it replaces (`m_CmdList->Foo()`, `.Get()`, `if (!m_CmdList)`), but every
    // PSO / root-signature / root-argument / IA / RS / OM bind now goes through the shadow first.
    D3D12CmdList m_CmdList;

    Microsoft::WRL::ComPtr<ID3D12Fence> m_Fence;
    UINT64 m_FenceValues[kBackBufferMax] = {};
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

    /** DXGI frame-latency waitable handle (DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),
        set up once in CreateSwapChain. Survives ResizeBuffers (same flag, same handle) - only
        closed in the destructor. See GetFrameLatencyWaitableObject(). */
    HANDLE m_FrameLatencyWaitableObject = nullptr;

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

    // RENDER (internal) resolution: everything the 3D scene draws into, up to the tonemap resolve.
    INT2  m_Resolution = {};
    // NATIVE swapchain/window size: swapchain, HDR display target, every post-tonemap pass, 2D UI, ImGui.
    INT2  m_BackbufferResolution = {};
    // Last ResolutionScalePercent the render targets were built for (D3D11's s_oldResolutionScalePercent).
    int   m_AppliedResolutionScalePercent = 100;
    int   m_PendingResolutionScalePercent = 0;   // value currently being waited out (0 = nothing pending)
    int   m_ResolutionScaleStableFrames = 0;
    static constexpr int kResolutionScaleDebounceFrames = 6;
    float m_AppliedMipLodBias = 0.0f;   // see D3D12RootLayout::SetAnisoMipLodBias
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
    Microsoft::WRL::ComPtr<ID3D12Resource> m_UIVertexBuffer[kBackBufferMax]; // persistently-mapped upload ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_UIVertexBufferAlloc[kBackBufferMax];
    uint8_t* m_UIVertexBufferPtr[kBackBufferMax] = {};
    UINT m_UIVertexBufferCapacity = 0;
    UINT m_UIVertexBufferOffset = 0;                               // reset each OnBeginFrame
    bool m_UIOverflowLogged = false;

    // Debug/editor line ring (D3D12LineRenderer). Same persistently-mapped per-frame upload ring pattern as
    // the UI vertex ring above; the line PSOs/root sig live in m_Pipelines.Lines.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_LineVertexBuffer[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_LineVertexBufferAlloc[kBackBufferMax];
    uint8_t* m_LineVertexBufferPtr[kBackBufferMax] = {};
    UINT m_LineVertexBufferCapacity = 0;
    UINT m_LineVertexBufferOffset = 0;                             // reset each OnBeginFrame
    bool m_LineOverflowLogged = false;

    // Poly-strip vertex ring (D3D12Fx.cpp). Same persistently-mapped per-frame upload ring pattern; the strip
    // geometry is rebuilt on the CPU every frame (GothicAPI::CalcPolyStripMeshes), so it can't live in a
    // static VB the way quad marks do. D3D11 instead grows one shared TempPolysVertexBuffer on demand.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_FxVertexBuffer[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_FxVertexBufferAlloc[kBackBufferMax];
    uint8_t* m_FxVertexBufferPtr[kBackBufferMax] = {};
    UINT m_FxVertexBufferCapacity = 0;
    UINT m_FxVertexBufferOffset = 0;                               // reset each OnBeginFrame
    bool m_FxOverflowLogged = false;

    std::unique_ptr<D3D12Texture> m_WhiteTexture;         // 1x1 white fallback for untextured draws
    std::unique_ptr<D3D12Texture> m_BlackTexture;         // 1x1 black fallback for untextured draws
    // One 1x1 ORM texture per selectable default roughness (AO 1, rough = DefaultRoughness::ForStep(i),
    // metal 0), all created up front by CreateWhiteTexture. Materials Gothic ships with no _FX/_ORM map —
    // most of them — are handed one of these bindlessly, and the shader can't be given a scalar instead
    // (MatOrmIndex is an SRV slot), hence a texture per step rather than a uniform. Never freed: FreeSrvSlot
    // skips all of them, same as the white/black fallbacks.
    std::unique_ptr<D3D12Texture> m_DefaultOrmTextures[DefaultRoughness::kNumSteps];
    // SRV slot of the currently selected one. Resolved once per frame in OnBeginFrame rather than per
    // material: the command builders read it thousands of times a frame, and the cascade builds read it
    // from worker threads, so it must not move mid-frame. UINT_MAX until the textures exist.
    UINT m_DefaultOrmSrvSlot = UINT_MAX;
    /** SRV heap slot of the default ORM texture for this frame's DefaultMaterialRoughness setting. */
    UINT GetDefaultOrmSrvSlot() const { return m_DefaultOrmSrvSlot; }
    /** Re-resolves m_DefaultOrmSrvSlot from RendererSettings.DefaultMaterialRoughness. Frame-start only. */
    void RefreshDefaultOrmSlot();
    std::unique_ptr<D3D12Texture> m_DistortionTexture;    // distortion2.dds — wet-ground normal fallback (see LoadDistortionTexture)
    bool LoadDistortionTexture();                         // one-time, non-fatal load (mirrors LoadSmaaTextures)

    GfxTexture* m_CurrentTexture = nullptr;                        // diffuse bound for the next 2D draw
    PShaderID m_ActivePixelShader = PShaderID::PS_FixedFunctionPipe; // see SetActivePixelShader
    GfxTexture* m_VideoTextures[3] = {};                           // Y/U/V planes bound for the next PS_Video draw
    D3D12_VIEWPORT m_CurrentViewport = {};                        // pixel-space viewport (drives transform + RSSetViewports)
    D3D12_RECT     m_CurrentScissor = {};

    // --- 3D world mesh path (Phase 2 first-light: flat-shaded, depth-tested, no G-buffer) ---
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_DsvHeap;         // slot 0 = scene depth, slot 1 = preview depth
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_DepthBuffer;     // R32_TYPELESS (DSV D32_FLOAT / SRV R32_FLOAT), reversed-Z
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_DepthBufferAlloc; // backing D3D12MA allocation (recreated on resize)
    UINT m_DsvDescriptorSize = 0;
    UINT m_DepthSrvSlot = UINT_MAX;   // R32_FLOAT SRV of m_DepthBuffer, read by the light cull for per-tile far-Z tightening

    // Native-resolution depth for the inventory-item preview (DrawVobSingle), which draws onto the display
    // target after the resolve — a bound DSV must match its RTV's size. D3D11's m_SwapchainDepthStencilBuffer.
    // Built lazily and only while the render scale is != 100%.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_PreviewDepthBuffer;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation>  m_PreviewDepthAlloc;
    INT2 m_PreviewDepthSize = {};
    bool m_PreviewDepthFailed = false;   // creation failed once — don't retry every preview draw
    D3D12_CPU_DESCRIPTOR_HANDLE GetPreviewDsv();   // {0} = none usable, caller skips the draw

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
    Microsoft::WRL::ComPtr<ID3D12Resource> m_WorldDrawArgs[kBackBufferMax]; // persistently-mapped UPLOAD ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_WorldDrawArgsAlloc[kBackBufferMax];
    uint8_t* m_WorldDrawArgsPtr[kBackBufferMax] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_WorldDrawArgsGpu[kBackBufferMax] = {};
    UINT m_WorldDrawCount = 0;                       // commands built this frame (shared by both world passes)
    // Alpha-test partition. BuildWorldDrawCommands orders the command set so [0, m_WorldOpaqueDrawCount)
    // needs NO alpha cutout and the rest does. Only the depth prepass cares: it submits the prefix through a
    // PS-less PSO (double-rate Z) and the suffix through the clipping one. The color pass draws the whole
    // range — opaque geometry is order-independent, so the reordering is invisible to it.
    UINT m_WorldOpaqueDrawCount = 0;
    // Coalesced mirror of the opaque prefix, appended PAST m_WorldDrawCount so the color pass' view of the
    // ring is untouched. The wrapped world index buffer is packed section-major, so a visible section's
    // opaque materials form one contiguous index run, and with no pixel shader bound their per-material b6
    // constants are dead — so those N commands become one DrawIndexed. Only the depth-only submits may use
    // it. 0 = not built this frame (no ring tail room), callers fall back to the per-material prefix.
    UINT m_WorldDepthMergedFirst = 0;
    UINT m_WorldDepthMergedCount = 0;
    unsigned int m_WorldDrawnIndices = 0;            // total indices in this frame's command set (triangle counter)
    bool m_WorldDrawArgsOverflowLogged = false;
    bool CreateWorldIndirect();                      // command signature + per-frame arg ring (once, at init)
    void BuildWorldDrawCommands();                   // collect visible sections + fill arg ring (once/frame, pre-prepass)
    // Merges the opaque world commands in `opaque` (sorted in place) into the fewest DrawIndexed commands
    // covering the EXACT same index ranges, written to `out`. Shared by BuildWorldDrawCommands and
    // D3D12ShadowMap::CullCascade. Returns the merged count, or 0 if `out` could not hold the result.
    static UINT CoalesceWorldDepthCommands( std::vector<WorldDrawCommand>& opaque,
        WorldDrawCommand* out, UINT outCapacity );

    // ---- Alpha-blended world-mesh surfaces (D3D12Transparency.cpp) — port of D3D11's
    // DrawMeshInfoListAlphablended. BuildWorldDrawCommands peels these out of the opaque command set into
    // g_FrameWorldTransparency (D3D12EngineCommon.h) and sorts them back-to-front; the pass draws them with
    // the material's own blend mode after the opaque scene + water, then re-lays their depth for the fog.
    // MT_Portal (G1 forest portals) and MT_WaterfallFoam are peeled by material TYPE into their own lists
    // and drawn as extra sub-passes with their own pixel shaders — same three-list split D3D11 has.
    static bool IsWorldMeshAlphaBlended( zCMaterial* mat );   // "does this material belong in that list?"
    void DrawWorldTransparencyRun( std::span<const TransparentItem> items,
        EWorldTransparencyVariant variant );                  // one run out of the transparency queue
    // Per run, since the queue interleaves kinds that bind their own root signature. False = cannot draw.
    bool BindWorldTransparencyFrameState();
    void DrawWorldTransparencyDepthOnly();                    // depth re-lay, once, after the whole replay

    // ---- The frame's sorted transparency queue (D3D12Transparency.cpp). Collect fills the backend-neutral
    // TransparencyQueue from this frame's per-kind lists and sorts it back-to-front; Draw replays it,
    // batching maximal same-kind runs. Payloads are indices into this backend's own arrays.
    void CollectTransparencyQueue();
    void DrawTransparencyQueue();
    void DrawGhostRun( std::span<const TransparentItem> items );
    void DrawDecalRun( std::span<const TransparentItem> items );
    void DrawQuadMarkRun( std::span<const TransparentItem> items );
    void DrawPolyStripRun( std::span<const TransparentItem> items );
    void DrawVobAlphaRun( std::span<const TransparentItem> items );

    // ---- Blended instanced VOBs (cobwebs, hanging cloth) — port of D3D11's DrawFrameAlphaMeshes.
    // BuildVobDrawCommands peels every VOB material with a BLEND/ADD alpha func out of the opaque
    // ExecuteIndirect set into g_FrameVobAlpha (D3D12EngineCommon.h); DrawVobAlphaRun above replays them unlit
    // and blended, depth-tested but never depth-writing, in transparency-queue order.

    // ---- GPU-driven instanced VOBs (P2.12): ExecuteIndirect + bindless diffuse + the VOB mega-buffer arena.
    //
    // Every static VOB sub-mesh lives in ONE DEFAULT-heap vertex buffer + ONE index buffer (D3D12VobArena),
    // so a command carries no buffer views at all: the mesh is BaseVertexLocation/StartIndexLocation inside
    // the shared pair and StartInstanceLocation covers the single instance buffer. Each pass therefore binds
    // the IA once — the presence of VBV/IBV arguments is what makes a command an IA state change, and 560 of
    // those per pass was the dominant VOB cost. See D3D12VobArena.h.
    //
    // Diffuse goes bindless (b6.MatDiffuseIndex root const). Wind min/max height (per-visual) rides as a
    // 2-const partial write into b4 @ offset 4; the frame-global wind fields are set once before the
    // ExecuteIndirect. Built ONCE per frame and consumed by both the depth prepass and the color pass; the
    // CSM cascades and rain shadowmap build their own sets over the same arena. Arg-member order MUST match
    // the command signature's pArgumentDescs order (b6 consts, b4 consts, draw).
    struct VobDrawCommand {                          // 48 bytes; all members 4-byte, no GPUVA alignment to keep
        uint32_t MatNormalIndex;                     // @0  b6.x  (0xFFFFFFFF = no normal map)
        uint32_t MatOrmIndex;                        // @4  b6.y  (default ORM slot when no _FX)
        uint32_t MatDiffuseIndex;                    // @8  b6.z  bindless diffuse (alpha clip + albedo)
        float    WindMinHeight;                      // @12 b4[4] per-visual bbox.min.y
        float    WindMaxHeight;                      // @16 b4[5] per-visual bbox.max.y
        D3D12_DRAW_INDEXED_ARGUMENTS Draw;           // @20 (20 bytes) — ends the indirect arguments at @40
        // --- Past the end of the command signature's arguments. ExecuteIndirect reads the args packed from
        // offset 0 and only requires ByteStride to COVER them, so trailing fields are free per-command payload
        // for our own compute passes. VisualIndex tells CSPatchArgs which per-visual surviving-instance count
        // to write into Draw.InstanceCount; 0xFFFFFFFF = "leave the CPU's count alone" (not GPU-culled).
        uint32_t VisualIndex;                        // @40
        // Which of the visual's two compacted instance runs this command draws; CSPatchArgs picks the
        // matching count + StartInstanceLocation from it.
        uint32_t LodBucket;                          // @44
    };

    // The pre-arena command shape, kept for the one consumer that cannot use the arena: node attachments
    // (weapons/heads/held items), whose geometry comes from the SharedVisualRegistry and arrives and leaves
    // with NPCs. They keep their own signature (m_VobBoundIndirectCmdSig) with the two VBVs + IBV inline.
    // VSMainAttach/VSDepthAttach never read the b4 wind min/max (the instance stream's wind fields carry
    // Fatness/Scaling here), so those two floats go out as 0.
    struct VobBoundDrawCommand {                     // 96 bytes; UINT64 members force 8-byte align, 96 % 8 == 0
        D3D12_VERTEX_BUFFER_VIEW MeshVBV;            // @0  packed ExVertexStruct stream (slot 0)
        D3D12_VERTEX_BUFFER_VIEW InstVBV;            // @16 per-instance VobInstanceInfo stream (slot 1)
        D3D12_INDEX_BUFFER_VIEW  IBV;                // @32 R16_UINT sub-mesh indices
        uint32_t MatNormalIndex;                     // @48 b6.x
        uint32_t MatOrmIndex;                        // @52 b6.y
        uint32_t MatDiffuseIndex;                    // @56 b6.z
        float    WindMinHeight;                      // @60 b4[4]
        float    WindMaxHeight;                      // @64 b4[5]
        D3D12_DRAW_INDEXED_ARGUMENTS Draw;           // @68 (20 bytes)
        uint32_t VisualIndex;                        // @88 always 0xFFFFFFFF here — attachments are never GPU-culled
        uint32_t LodBucket;                          // @92 pad / always kLodBucketNear
    };
    static constexpr uint32_t kLodBucketNear = 0;
    static constexpr uint32_t kLodBucketFar  = 1;
    // Separate caps: the main view now collects distance-only (360 degrees, GPU-culled) so it needs headroom,
    // while the CSM cascades still CPU-cull against their own frustum and keep the original budget — a shared
    // bump would multiply across 3 cascades x kBackBufferCount rings and cost real 32-bit address space.
    static constexpr UINT kMaxVobDrawCommands       = 16384;  // main view: visuals x materials x sub-meshes
    static constexpr UINT kMaxShadowVobDrawCommands = 8192;   // per shadow cascade; overflow logs + drops
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_VobIndirectCmdSig;        // b6(3) + b4[4..5](2) + DrawIndexed
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_VobBoundIndirectCmdSig;   // + VBVx2 + IBV (node attachments)
    // Every static VOB sub-mesh in one DEFAULT-heap VB/IB pair — see D3D12VobArena.h. Filled from OnAddVob
    // (which fires per vob during world load, so the world is resident before the first frame) and flushed
    // once per frame at the top of UploadFrameVobInstances.
    D3D12VobArena m_VobArena;
    // Re-uploads the arena ranges of animated static VOBs (.MMS morph meshes) from their own vertex buffers.
    // Runs right after DispatchMorphFold, which is what produces this frame's deformed vertices.
    void RefreshDynamicVobArena();
    /** The one IA bind every instanced-VOB ExecuteIndirect needs: the mega-buffers on slot 0 / the index
        stream, and `instances` (the per-instance VobInstanceInfo stream this pass draws from — the raw
        upload ring for the shadow/rain passes, the GPU-compacted buffer for a culled main view) on slot 1.
        Returns false when the arena holds nothing, in which case the caller must not submit: its commands
        would index a buffer that doesn't exist. */
    bool BindVobArenaIA( D3D12CmdList& cmdList, ID3D12Resource* instances, UINT instanceBytes );
    Microsoft::WRL::ComPtr<ID3D12Resource> m_VobDrawArgs[kBackBufferMax];   // main-view (prepass+color share it)
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobDrawArgsAlloc[kBackBufferMax];
    uint8_t* m_VobDrawArgsPtr[kBackBufferMax] = {};
    UINT m_VobDrawCount = 0;                          // commands built this frame (shared by both main-view VOB passes)
    UINT m_VobOpaqueDrawCount = 0;                    // alpha-test partition of the above — see m_WorldOpaqueDrawCount

public:
    /** Last frame's VOB submission shape, for the ImGui stats panel. The VOB passes are the frame's
        dominant GPU cost and the three plausible causes - too many COMMANDS, too many INSTANCES, too many
        TRIANGLES - are indistinguishable from a Tracy GPU zone alone, so publish all three plus whether the
        features that would reduce each are actually engaged. */
    struct VobFrameStats {
        UINT Commands;        // total ExecuteIndirect commands in the main-view set
        UINT OpaqueCommands;  // leading no-cutout run
        UINT CullVisuals;     // VobCullVisual records written (== visuals with instances this frame)
        UINT Instances;       // total instances uploaded across all visuals (pre-GPU-cull)
        UINT SplitNone;       // visuals per SplitMode - if these are all in SplitNone, the LOD slider
        UINT SplitLod;        // cannot be doing anything, whatever it is set to
        bool GpuCullActive;   // which of the two paths produced the split (compute vs the CPU upload)
    };
    const VobFrameStats& GetVobFrameStats() const { return m_VobStats; }
private:
    VobFrameStats m_VobStats = {};
    unsigned int m_VobDrawnTriangles = 0;            // triangles in this frame's main-view VOB command set (stats)
    bool m_VobDrawArgsOverflowLogged = false;
    // The shadow-caster variant of this pipeline (VSDepth + PSShadowClipBindless) and the per-cascade arg rings
    // it submits from live in D3D12ShadowMap; this signature is shared by both.
    bool CreateVobIndirect();                         // command signature + per-frame arg rings + shadow-caster PSO (once, at init)
    // Fill an arg buffer from the given VOB uploads; returns command count.
    //   resolveMaps  false leaves normal/orm at defaults (depth/shadow passes only alpha-clip on diffuse).
    //   culled       points each command's instance stream at the GPU-compacted buffer and stamps
    //                VisualIndex so CSPatchArgs can overwrite the instance count (main view only).
    //   cacheIn      false resolves diffuse with GetCacheState instead of CacheIn, making the build a pure
    //                read so it can run on a cascade's worker thread. A texture not yet pulled in alpha-clips
    //                against black for one frame, which on a caster silhouette never reaches the screen.
    //   shadowCascade  which of a sub-mesh's index buffers to draw, mirroring D3D11's
    //                GetShadowAwareIndexBuffer: main view = full render indices, cascade >= 0 = position-
    //                welded shadow indices, >= kFirstLodShadowCascade = the baked progressive-mesh LOD.
    //                Both reduced buffers merge wedges sharing a position but not a UV, so an alpha-clipped
    //                material always keeps its render indices.
    //
    // The LOD gate is shared with D3D11 (WorldConverter.h): an edge collapse MOVES the caster surface, so a
    // cascade biased tighter than that deviation self-shadows the full-detail surface black.
    // DebugSettings.ShadowCascades.FirstLodCascade overrides it at runtime; GetFirstLodShadowCascade() wins.
    static constexpr int kVobIndicesMainView = -1;
    static constexpr int kFirstLodShadowCascade = SHADOW_LOD_FIRST_CASCADE;
    static int GetFirstLodShadowCascade();
    // outOpaqueCount (optional): how many of the returned commands form the leading no-alpha-cutout run. The
    // build always partitions — opaque materials first, alpha-tested ones after — so the depth prepass can
    // submit the prefix through a PS-less PSO. Callers that don't split (the color pass, the shadow cascades)
    // just pass nullptr and draw the whole range. See m_WorldOpaqueDrawCount.
    UINT BuildVobDrawCommands( const std::vector<FrameVobUpload>& uploads, uint8_t* argPtr, bool resolveMaps,
        UINT maxCommands, bool culled = false, bool cacheIn = true,
        int shadowCascade = kVobIndicesMainView, UINT* outOpaqueCount = nullptr );

    // ---- GPU-driven skeletal meshes + node attachments (T9): ExecuteIndirect + bindless materials ----------
    // Possible because neither Skeletal.RootSig nor the attachment PSOs bind a t0 descriptor table any more,
    // and ExecuteIndirect can set root constants and root DESCRIPTORS but never a table. The per-mesh CPU
    // work happens ONCE per frame in BuildSkeletalDrawCommands, and the depth prepass and the lit pass each
    // become one submit over the same argument buffer.
    //
    // Base meshes need their own signature because each command also rebinds the per-vob root CBVs (b1
    // instance, b2 bone palette) the shared skinning VS reads. Node attachments do not — they already draw
    // through World.RootSig with exactly the shape m_VobIndirectCmdSig describes, so they reuse it and
    // VobDrawCommand verbatim.
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
    Microsoft::WRL::ComPtr<ID3D12Resource> m_SkeletalDrawArgs[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_SkeletalDrawArgsAlloc[kBackBufferMax];
    uint8_t* m_SkeletalDrawArgsPtr[kBackBufferMax] = {};
    UINT m_SkeletalDrawCount = 0;                    // commands built this frame (prepass + color share them)
    UINT m_SkeletalOpaqueDrawCount = 0;              // alpha-test partition — see m_WorldOpaqueDrawCount
    Microsoft::WRL::ComPtr<ID3D12Resource> m_AttachDrawArgs[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_AttachDrawArgsAlloc[kBackBufferMax];
    uint8_t* m_AttachDrawArgsPtr[kBackBufferMax] = {};
    UINT m_AttachDrawCount = 0;                      // VobDrawCommands built this frame (prepass + color share them)
    UINT m_AttachOpaqueDrawCount = 0;                // alpha-test partition — see m_WorldOpaqueDrawCount
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
        // Which distance CSCull buckets this visual's instances by - see kSplitMode* below. Written by
        // BuildVobDrawCommands, not here: only the command build knows whether the far bucket actually has
        // commands to draw it, and splitting without them strands those instances (see VobCull.hlsl).
        UINT              SplitMode;
    };
    // No split: every surviving instance lands in the near run.
    static constexpr UINT kSplitModeNone  = 0;
    // Split at m_VobLodDistance: the far run draws the simplified LOD index buffer. Requires EVERY drawn
    // sub-mesh of the visual to have emitted a far command.
    static constexpr UINT kSplitModeLod   = 1;
    static constexpr UINT kMaxCullVisuals = 16384;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_VobCullVisuals[kBackBufferMax];   // persistently-mapped UPLOAD
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobCullVisualsAlloc[kBackBufferMax];
    uint8_t* m_VobCullVisualsPtr[kBackBufferMax] = {};
    UINT m_VobCullVisualCount = 0;                  // records written this frame
    bool m_VobCullVisualOverflowLogged = false;
    // THIS frame's overflow (the ...Logged flag above is sticky for the session). CullVobsGPU seeds the
    // compacted instance buffer from the raw ring when it is set — see the note there.
    bool m_VobCullVisualOverflowed = false;
    // Resolved ONCE per frame: BuildVobDrawCommands and CSCull must see the same value or a bucket is
    // left with no command to draw it. 0 = off.
    float m_VobLodDistance = 0.0f;
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
    ID3D12Resource* GetVobInstanceBufferForDraws() const;

    // ---- GPU morph fold (D3D12MorphFold.cpp + MorphGpu.h + Shaders/D3D12/MorphFold.hlsl) ----
    // Morph attachments (NPC heads, bow/crossbow draw meshes) fold their blend shapes in a compute pass that
    // rewrites the Position of each vertex in the submesh's own DEFAULT-heap UAV vertex buffer, instead of
    // ZENGIN deforming on the CPU and re-uploading the stream every animation frame. The prototype tables are
    // per .MMS and immutable; the only per-frame upload is the channel records.
    static constexpr UINT kMaxMorphChannelRecords = 4096;   // 24 B each -> 96 KB per frame-in-flight
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_MorphChannelBuffer[kBackBufferMax];   // persistently-mapped UPLOAD
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_MorphChannelBufferAlloc[kBackBufferMax];
    uint8_t* m_MorphChannelBufferPtr[kBackBufferMax] = {};
    bool m_MorphChannelOverflowLogged = false;
    // One entry per MorphGpu::Prototype, uploaded on the first frame that folds it and kept for the session
    // (~1 MB in total). Keyed by the opaque Prototype pointer so this header need not include MorphGpu.h.
    struct MorphTableGpu {
        Microsoft::WRL::ComPtr<ID3D12Resource>      Positions;
        Microsoft::WRL::ComPtr<D3D12MA::Allocation> PositionsAlloc;
        Microsoft::WRL::ComPtr<ID3D12Resource>      Indices;
        Microsoft::WRL::ComPtr<D3D12MA::Allocation> IndicesAlloc;
    };
    std::unordered_map<const void*, MorphTableGpu> m_MorphTables;
    std::vector<D3D12ResourceTransition> m_MorphBarriers;   // scratch; keeps its capacity across frames
    // This frame's queue, moved out of MorphGpu by DispatchMorphFold (see MorphGpu::TakeJobs). Members rather
    // than locals so they keep their capacity across frames.
    std::vector<MorphGpu::Job> m_MorphJobs;
    std::vector<MorphGpu::ChannelRecord> m_MorphChannels;
    UINT m_MorphFoldSubmeshCount = 0;   // submeshes folded last frame (diagnostic)
    bool m_MorphFoldReady = false;
    bool CreateMorphFoldResources();   // channel ring (once, at init); enables MorphGpu if the PSO is there too
    void DispatchMorphFold();          // records this frame's folds; call before the first pass that draws them

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
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_ShadowCmdAllocators[kShadowRecordSlots][kBackBufferMax];
    // Each slot owns its OWN state cache: a D3D12CmdList shadow is per-list and unsynchronized, and these
    // lists are recorded concurrently on pool threads while the main thread records m_CmdList.
    D3D12CmdList m_ShadowCmdLists[kShadowRecordSlots][kBackBufferMax];
    bool m_ShadowCmdListsReady = false;
    bool CreateShadowRecordCommandLists();
    // Closes + submits whatever is currently recorded in m_CmdList and immediately reopens it on the SAME
    // frame allocator (no allocator Reset, no GPU wait) so a batch of independently-recorded lists can be
    // slotted into the queue at this exact point in the frame — or simply so the GPU can start on what is
    // already recorded instead of idling until Present.
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
    //   FinishShadowPasses   — join, execute the finished lists (they land ahead of the still-open part B2,
    //                          the lit passes), re-record any failed slot, then post-barriers + RT rebind.
    // NOTE the CSM cascades do NOT go through this driver: each cascade is one self-contained job (cull ->
    // build -> record -> close) launched by D3D12ShadowMap::Prepare, far earlier in the frame. Only the join
    // is shared, in FinishShadowPasses. See the phase table in D3D12ShadowMap.h.
    void PrepareShadowPasses();
    void BeginShadowRecording();
    void FinishShadowPasses();
    // Resets one (slot x frame-in-flight) allocator/list pair and hands back the open list, or nullptr on
    // failure. Public to the recording lambdas only in the sense that they are defined inside member functions.
    D3D12CmdList* BeginShadowList( UINT slot );
    bool m_ShadowListRecorded[kShadowRecordSlots] = {};   // per slot: recorded AND closed successfully this frame
    bool m_ShadowThreadedRecord = false;   // this frame's BeginShadowRecording took the pooled-recording path
    bool m_RainShadowPassReady = false;   // PrepareRainShadowmap produced a valid camera + caster set this frame
    bool m_ShadowRecordingPending = false;   // BeginShadowRecording fanned work out; FinishShadowPasses must join
    bool m_ShadowRecordFailureLogged = false;   // one warning per session, not per frame

    // Per-frame-in-flight shadow constant buffer, bound by every lit pass. Three disjoint byte ranges written by
    // three owners: D3D12ShadowMap::Prepare owns the head [0, kWetnessCbOffset) (cascade view-projs + sun dir +
    // strength + texel sizes), then UploadWetnessConstants and UploadSkyIblConstants each own a tail block —
    // with an unused 80-byte hole between them (kAoReprojCbOffset). The buffer lives here because it is shared.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ShadowCB[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_ShadowCBAlloc[kBackBufferMax];
    uint8_t* m_ShadowCBMapped[kBackBufferMax] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_ShadowCBGpu[kBackBufferMax] = {};
    bool CreateShadowConstantBuffer();   // the shared per-frame shadow CB ring (once, at init)
    // Bindless SRV-heap-slot resolvers for the (fully bindless) skeletal + node-attachment passes: the
    // material's cached-in engine texture's heap slot, or the 1x1 black fallback. ...Slot uses GetCacheState
    // (NOT CacheIn) so it stays a pure read — no Gothic texture load is triggered from a pass that only needs
    // an alpha cutout, which also keeps the shadow paths' "never CacheIn off the main thread" contract;
    // ...CacheIn is the main-view variant.
    UINT ResolveShadowDiffuseSlot( zCTexture* tex ) const;
    UINT ResolveDiffuseSlotCacheIn( zCTexture* tex );


    // Clustered Forward+ light culling (P2.14; tiled P2.9b-2 predecessor): one global compute root sig + PSO;
    // one resolution-sized DEFAULT-heap UAV buffer holding the per-cluster 64-bit light-membership mask
    // (NumTilesX * NumTilesY * kNumZSlices clusters). Lives permanently in UNORDERED_ACCESS. m_NumTilesX/Y =
    // screen-tile grid dimensions for the current resolution (Z slice count is the compile-time kNumZSlices).
    // Light-cull pipeline (RootSig/PSO/blob) now lives in m_Pipelines.LightCull
    Microsoft::WRL::ComPtr<ID3D12Resource> m_LightGridBuffer;    // RWStructuredBuffer<LightGrid> (numClusters * 8 B)
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_LightGridBufferAlloc;  // recreated on resize
    UINT m_NumTilesX = 0;
    UINT m_NumTilesY = 0;
    // The grid buffer round-trips UNORDERED_ACCESS (cull CS writes) -> PIXEL_SHADER_RESOURCE (lit PS reads)
    // each frame. Tracks whether it's currently in the PS-read state so DispatchLightCulling knows whether
    // it must transition it back to UAV before the next dispatch (false right after (re)creation in UAV).
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
    UINT m_BloomUpSrvPairSlot[kBackBufferMax][kBloomMaxMips] = {
        { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX },
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
    D3D12_CPU_DESCRIPTOR_HANDLE m_SmaaEdgesRtv = {};                 // RTV heap slot kBackBufferMax+1
    D3D12_CPU_DESCRIPTOR_HANDLE m_SmaaBlendRtv = {};                 // RTV heap slot kBackBufferMax+2
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

    // Underwater screen effect — port of D3D11GraphicsEngine::DrawUnderwaterEffects (D3D12Underwater.cpp).
    // Runs only while GothicAPI::IsUnderWater(), in the same frame slot D3D11 uses: on the finished LDR image,
    // after the sharpen pass and before Gothic's 2D UI/HUD composites on top (the HUD must not be blurred or
    // distorted). Three passes — quarter-res Gaussian H, quarter-res Gaussian V (both tinted by
    // kUnderwaterColorMod), then a full-res distorted composite back over the display target. The two blur
    // targets are compute-written, which is why they need no RTV heap slot; the composite has to be a graphics
    // pass because the display target has no UAV.
    //
    // Built LAZILY the first time the player goes under water (same reasoning as the DoF textures — this is a
    // rare state and the pair costs VA that 32-bit cannot spare for an effect most sessions never trigger).
    // Both rest in UNORDERED_ACCESS between frames.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_UnderwaterBlur[2];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_UnderwaterBlurAlloc[2];
    UINT m_UnderwaterBlurSrvSlot[2] = { UINT_MAX, UINT_MAX };
    UINT m_UnderwaterBlurUavSlot[2] = { UINT_MAX, UINT_MAX };
    INT2 m_UnderwaterBlurSize = { 0, 0 };       // quarter resolution the pair was built for
    bool m_UnderwaterResourcesReady = false;
    bool m_UnderwaterCreateAttempted = false;   // keeps a failed creation from retrying every frame; cleared on resize
    bool CreateUnderwaterResources( INT2 size );   // (re)builds the quarter-res blur pair + its SRV/UAV slots
    void DrawUnderwaterEffects();                  // no-op unless underwater; guards on the PSOs + m_LdrCopyReady

    // Simple screen-space AO (plan item #4, "SAO"): resolution-dependent R8_UNORM textures (m_AOMask holds the
    // final blurred result; m_AOBlurTemp is the horizontal-blur scratch target), recreated on resize like the
    // bloom pyramid. RenderSSAO dispatches main-estimate -> blurH -> blurV (Shaders/D3D12/SSAO.hlsl) and leaves
    // m_AOMask in PIXEL_SHADER_RESOURCE for the lit geometry passes (World/Vob/Skeletal PS) to sample bindlessly.
    // m_ActiveAOMaskSrvSlot is refreshed every frame by RenderSSAO: the AO mask's slot when SSAO ran, else the white texture's
    // slot (mask = no occlusion) — mirrors D3D11's white-clear "AO disabled" default.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_AOMask;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_AOMaskAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_AOBlurTemp;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_AOBlurTempAlloc;
    UINT m_AOMaskSrvSlot = UINT_MAX;    // bindless index the lit PS reads (final blurred result)
    UINT m_AOMaskUavSlot = UINT_MAX;    // main-pass output + blur-pass-2 output
    // Second UAV over the SAME m_AOMask texels, typed R8_UINT instead of R8_UNORM. That is why m_AOMask is
    // created R8_TYPELESS: XeGTAO's denoise writes the AO term as a raw `uint(value * 255 + 0.5)` (Intel's
    // packing, shared with its bent-normal variant), while every reader — the lit pixel shaders and the simple
    // SSAO blur — wants a float in [0,1]. An R8_UNORM SRV over the same bits decodes that byte back to exactly
    // the value written, so the two AO implementations can share one output texture and one consumer path
    // without either shader converting.
    UINT m_AOMaskUintUavSlot = UINT_MAX;
    UINT m_AOBlurTempUavSlot = UINT_MAX; // blur-pass-1 output
    // The blur passes' SRV descriptor TABLE needs its 2 entries (AO input, depth) heap-contiguous — m_DepthSrvSlot
    // lives elsewhere in the heap, so each direction gets its own 2-slot pair with a private copy of the depth
    // view. Written ONCE per (re)creation (CreateAOResources, after CreateDepthBuffer) — unlike the bloom
    // pyramid's per-frame-rewritten pair slots, nothing here changes frame-to-frame, so no double-buffering race.
    UINT m_AOBlurHPairSlot = UINT_MAX;  // [0]=AOMask SRV (raw estimate), [1]=Depth SRV
    UINT m_AOBlurVPairSlot = UINT_MAX;  // [0]=AOBlurTemp SRV, [1]=Depth SRV
    // The single b5/b7/b8 AOCB root constant every lit PS binds — this frame's AO mask heap slot. The matching
    // 1/screen-size lives in the shadow CB instead (see kAoReprojCbOffset): the World root signature sits at
    // 63 of its 64 DWORDs, so two more root constants would not fit there.
    UINT m_ActiveAOMaskSrvSlot = UINT_MAX;
    bool m_AOResourcesReady = false;
    // m_AOMask rests in UNORDERED_ACCESS between RenderSSAO runs, except right after a successful one, which
    // leaves it PIXEL_SHADER_RESOURCE for the lit passes. Toggling AO off skips RenderSSAO entirely, so the
    // flag is what tells the next run whether to flip it back.
    bool m_AOMaskInPixelState = false;
    // --- The AO depth source ---------------------------------------------------------------------------------
    // THIS frame's m_DepthBuffer, read after the Forward+ depth prepass and before any lit pass, so the mask is
    // in this frame's screen space and needs no reprojection. In exchange RenderSSAO round-trips the depth
    // buffer DEPTH_WRITE <-> NON_PIXEL_SHADER_RESOURCE and serialises against the prepass, as BuildHiZ does.
    //
    // 80 bytes of the shared shadow CB, between the wetness block and the sky-IBL tail. Only the leading float2
    // is live (1/screen-size, to turn SV_Position into a mask UV); the rest is the hole the old reprojection
    // constants left, kept because five HLSL cbuffer layouts bake in the offsets after it.
    static constexpr UINT kAoReprojCbOffset = 352;
    static constexpr UINT kAoReprojCbReservedBytes = 80;
    struct AoScreenCBData { float InvResX; float InvResY; float _pad[2]; };
    void UploadAoScreenConstants();           // publishes AoInvRes into the shadow CB (unconditional, every frame)
    bool CreateAOResources( INT2 size );      // (re)builds m_AOMask/m_AOBlurTemp + persistent SRV/UAV slots
    void RenderSSAO();                        // the AO entry point: publishes the AO constants, then runs XeGTAO or simple SSAO
    void RenderSimpleSSAO();                  // main estimate -> separable blur (Shaders/D3D12/SSAO.hlsl)
    void BeginAoDepthRead();                  // m_DepthBuffer DEPTH_WRITE -> NON_PIXEL_SHADER_RESOURCE
    void EndAoDepthRead();                    // ...and straight back (both AO implementations bracket with these)

    // ---- Intel XeGTAO (D3D12GTAO.cpp) ----------------------------------------------------------------------
    // Ground-truth ambient occlusion; this is what AOMode::AO_ASSAO selects on D3D12 (D3D11 keeps its own ASSAO
    // port). Reads the SAME depth-prepass depth the simple SSAO path does and writes the SAME m_AOMask, so the
    // whole consumer side — the bindless fetch in include/ScreenSpaceAO.hlsl — is untouched and the two
    // implementations are interchangeable.
    //
    // Four private intermediates, all recreated on resize alongside m_AOMask:
    //   m_GtaoWorkingDepth  R32_FLOAT with 5 MIPs — view-space depth pyramid built by the prefilter pass. FP32
    //                       rather than Intel's default R16_FLOAT because Gothic's view depths run past fp16's
    //                       65504 ceiling near the horizon (see the header of Shaders/D3D12/XeGTAO.hlsl).
    //   m_GtaoNormals       R32_UINT — view-space normals packed R11G11B10_UNORM, reconstructed from the depth.
    //                       FALLBACK ONLY: when the prepass normal G-buffer (m_NormalBuffer) is available
    //                       RenderGTAO skips this dispatch and feeds XeGTAO the real shading normals instead
    //                       (LoadNormal in XeGTAO.hlsl decodes + rotates them into view space).
    //   m_GtaoEdges         R8_UNORM — packed depth edges the denoiser weights by.
    //   m_GtaoAOTerm[2]     R8_UINT — the working AO term, ping-ponged across denoise passes. The final pass
    //                       writes m_AOMask through m_AOMaskUintUavSlot instead.
    // All four rest in UNORDERED_ACCESS between frames, like the bloom pyramid and the AO mask.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_GtaoWorkingDepth;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_GtaoWorkingDepthAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_GtaoNormals;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_GtaoNormalsAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_GtaoEdges;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_GtaoEdgesAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_GtaoAOTerm[2];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_GtaoAOTermAlloc[2];
    static constexpr UINT kGtaoDepthMipLevels = 5;   // must match XE_GTAO_DEPTH_MIP_LEVELS (hard-coded to 5)
    UINT m_GtaoWorkingDepthSrvSlot = UINT_MAX;                   // full MIP chain, read by the main pass
    UINT m_GtaoWorkingDepthUavSlot[kGtaoDepthMipLevels] = { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX };
    UINT m_GtaoNormalsSrvSlot = UINT_MAX;
    UINT m_GtaoNormalsUavSlot = UINT_MAX;
    UINT m_GtaoEdgesSrvSlot = UINT_MAX;
    UINT m_GtaoEdgesUavSlot = UINT_MAX;
    UINT m_GtaoAOTermSrvSlot[2] = { UINT_MAX, UINT_MAX };
    UINT m_GtaoAOTermUavSlot[2] = { UINT_MAX, UINT_MAX };
    bool m_GtaoResourcesReady = false;
    UINT m_GtaoFrameNumber = 0;                // drives the temporal noise rotation; only advances while TAA is on
    bool CreateGtaoResources( INT2 size );     // (re)builds the four intermediates + their persistent heap slots
    bool IsGtaoEnabled() const;                // AoMode == AO_ASSAO and every resource/PSO it needs exists
    void RenderGTAO();                         // prefilter -> (normals) -> GTAO integral -> N denoise passes

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
    //   m_NormalBuffer   — RG16F octahedral world-space shading normal, the XeGTAO input (see RenderGTAO /
    //                      LoadNormal). Untouched by the fill pass: a pixel with no prepass coverage has no
    //                      meaningful normal, so it keeps the kGBufferNormalSentinel clear and AO treats it as
    //                      "no geometry" rather than integrating against an invented orientation.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_VelocityBuffer;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VelocityAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_NormalBuffer;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_NormalAlloc;
    D3D12_CPU_DESCRIPTOR_HANDLE m_VelocityRtv = {};   // RTV heap slot kBackBufferMax+4
    D3D12_CPU_DESCRIPTOR_HANDLE m_NormalRtv = {};     // RTV heap slot kBackBufferMax+5
    UINT m_VelocitySrvSlot = UINT_MAX;    // SRV for the debug overlay + future TAA/FSR3 consumers
    UINT m_VelocityUavSlot = UINT_MAX;    // UAV the FillCameraVelocity compute pass writes through (bindless)
    UINT m_NormalSrvSlot = UINT_MAX;      // SRV for the debug overlay + future XeGTAO
    bool m_MotionResourcesReady = false;  // false disables the whole feature (prepass falls back to depth-only)
    bool m_VelocityInPixelState = false;  // mirrors m_SceneColorInPixelState: tracks RT vs shader-read rest state
    // The combined shader-read state m_VelocityBuffer rests in once FillCameraVelocity has run — the debug
    // overlay reads it from a pixel shader, TAA and FSR3 from compute. Named because FSR3 has to narrow it to
    // the plain NON_PIXEL state for the duration of its dispatch and put it back (see D3D12Fsr3.cpp).
    static constexpr D3D12_RESOURCE_STATES kVelocityReadState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // Camera history. m_PrevViewProjUnjittered is the matrix that rasterized the PREVIOUS frame, captured at the
    // end of each OnStartWorldRendering; m_MotionHistoryValid is false on the first frame of a world and right
    // after a resize, where there is no previous camera and every velocity must read as zero.
    XMFLOAT4X4 m_PrevViewProjUnjittered = {};
    XMFLOAT4X4 m_CurViewProjUnjittered = {};
    bool m_MotionHistoryValid = false;
    // Per-frame-in-flight 256-byte UPLOAD slabs holding MotionCBData, bound as a root CBV by the *GBuf prepass
    // PSOs (b5 world-family / b9 skeletal) and by the fill pass (b0). One allocation per frame in flight rather
    // than a ring: the contents are frame-global, written once in UploadMotionConstants.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_MotionCB[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_MotionCBAlloc[kBackBufferMax];
    uint8_t* m_MotionCBMapped[kBackBufferMax] = {};
    struct MotionCBData {
        XMFLOAT4X4 PrevViewProj;
        XMFLOAT4X4 UnjitteredViewProj;
        XMFLOAT4X4 InvUnjitteredViewProj;
        XMFLOAT4 CameraPosition;   // xyz = eye, w unused; only FillCameraVelocity's sky branch reads it
    };
    static_assert( sizeof( MotionCBData ) == 208, "MotionCBData must match Shaders/D3D12/include/MotionVectors.hlsl" );

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
    bool MotionGBufferNeeded() const;   // does any pass actually read velocity/normals this frame? see the impl

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
    // TAA needs the PREVIOUS frame's depth for its disocclusion test — nothing else in the frame keeps one, so
    // this is a private snapshot taken at the end of RenderTAA.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_TaaPrevDepth;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_TaaPrevDepthAlloc;
    // Rest state of that snapshot: the combined shader-read state, flipped to COPY_DEST and straight back once
    // per frame by the snapshot copy.
    static constexpr D3D12_RESOURCE_STATES kPrevDepthReadState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
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
    // Last frame's offset, also in pixels. The previous depth buffer the resolve's disocclusion test gathers
    // from was rasterized with THIS offset baked in, so the lookup has to put it back on after taking the
    // current one off — see GetDepthConfidenceFactor in Shaders/D3D12/TAAResolve.hlsl.
    XMFLOAT2 m_TaaPrevJitterPixels = { 0.0f, 0.0f };
    int  m_TaaJitterIndex = 0;
    UINT m_TaaFrameNumber = 0;
    bool m_TaaJitterActive = false;

    bool CreateTaaResources( INT2 size );  // (re)builds the history pair + the private previous-depth snapshot
    void AdvanceJitter();                  // per-frame jitter into TransformProj._13/_23 (no-op when TAA is off)
    void RenderTAA();                      // the resolve dispatch + copy back over the scene colour
    bool IsTaaEnabled() const;             // AntiAliasingMode == AA_TAA and everything it needs exists

    // ---- FSR 3 temporal upscaler (D3D12Fsr3.cpp) ------------------------------------------------------------
    // AMD FidelityFX Super Resolution 3, the AA_FSR alternative to the TAA resolve above. Mutually exclusive
    // with it (one AntiAliasingMode), and deliberately sharing its jitter sequence and its motion-vector
    // G-buffer. Unlike D3D11 (which upscales the finished LDR backbuffer) this runs on the LINEAR HDR scene
    // colour, immediately before the tonemap resolve — see the file header for why, and for the shipping
    // requirement on ffx_backend_dx12_x86.dll.
    //
    // m_Fsr3Output is display-resolution and kSceneColorFormat, so the tonemap resolve can sample it in place
    // of m_SceneColor with no second PSO; that swap (GetTonemapSourceSrvSlot) is what turns the resolve's
    // implicit bilinear upscale into a 1:1 blit whenever FSR3 actually ran.
    struct FfxFsr3UpscalerContext* m_Fsr3Context = nullptr;   // heap-allocated: the FFX header stays out of here
    void* m_Fsr3Scratch = nullptr;                            // backend scratch; must outlive the context
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_Fsr3Output;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_Fsr3OutputAlloc;
    UINT m_Fsr3OutputSrvSlot = UINT_MAX;
    bool m_Fsr3OutputReady = false;
    bool m_Fsr3OutputInUavState = false;   // UNORDERED_ACCESS (FSR writes) vs PIXEL_SHADER_RESOURCE (rest)
    // The three resources FfxFsr3UpscalerDispatchDescription makes the application own, in the order
    // { dilatedDepth, dilatedMotionVectors, reconstructedPrevNearestDepth }. Sized/formatted from
    // ffxFsr3UpscalerGetSharedResourceDescriptions. We never touch their contents; the rest state below is
    // both where they are created and what every dispatch declares (see CreateFsr3SharedResources).
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_Fsr3Shared[3];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_Fsr3SharedAlloc[3];
    static constexpr D3D12_RESOURCE_STATES kFsr3SharedRestState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bool m_Fsr3SharedReady = false;
    // True until a dispatch succeeds after a discontinuity (world load, resize, fresh context/output): tells
    // FSR to throw away its temporal history instead of smearing the previous world across the new one.
    bool m_Fsr3Reset = true;
    // Did THIS frame's dispatch succeed? Drives GetTonemapSourceSrvSlot and the sharpen pass's early-out, so
    // a failed dispatch degrades to the plain bilinear resolve instead of showing a stale/garbage frame.
    bool m_Fsr3RanThisFrame = false;
    bool m_Fsr3InitFailed = false;             // don't retry context creation every frame; cleared by ReleaseFsr3
    bool m_Fsr3DispatchFailureLogged = false;  // one log line per session, not one per frame

    void EnsureFsr3Ready();                                       // lazy build, from AdvanceJitter
    bool CreateFsr3Output( INT2 size );                           // display-res HDR UAV target + its SRV
    bool CreateFsr3Context( INT2 renderSize, INT2 upscaleSize );   // FFX interface + context + shared resources
    bool CreateFsr3SharedResources();                             // the three application-owned FFX resources
    void DestroyFsr3Context();                                    // requires an idle GPU (FFX frees immediately)
    void ReleaseFsr3();                                           // context + shared + output; from the resize paths
    void RenderFsr3Upscale();                                     // the dispatch (no-op unless IsFsr3Enabled)
    bool IsFsr3Enabled() const;                                   // AA_FSR + FSR3 upscaler + everything exists
    UINT GetTonemapSourceSrvSlot() const;                         // m_Fsr3Output when it ran, else m_SceneColor

    // ---- Depth of field (D3D12DoF.cpp) ----------------------------------------------------------------------
    // Compute port of D3D11PFX_DepthOfField::RenderCS (Shaders/D3D12/DoF.hlsl). Three passes on the LINEAR HDR
    // scene colour, in D3D11's slot: after the TAA resolve and BEFORE bloom — D3D11 runs it at the head of its
    // "Post-processing B" block, which is exactly there. Scene colour in, scene colour out, so nothing
    // downstream needs to know it ran.
    //
    //   m_DoFFocus[2]  1x1 R32_FLOAT ping-pong. The auto-focus distance, temporally smoothed against the
    //                  previous frame's value — pass 0 reads [m_DoFFocusIndex] and writes [1 - m_DoFFocusIndex].
    //   m_DoFHalf      half-res kSceneColorFormat: rgb = bokeh blur, a = centre CoC (pass 1).
    //   m_DoFComposite full-res kSceneColorFormat scratch. Compute cannot read and write the scene colour in one
    //                  dispatch, so pass 2 writes here and the result is CopyResource'd back — same shape as the
    //                  TAA resolve's history copy, and both are kSceneColorFormat so the copy is a straight one.
    //
    // Unlike the bloom/AO/TAA resources these are built LAZILY (see CreateDoFResources): DoF is off in a stock
    // config, and a full-res RGBA16F scratch plus a half-res one is ~20 MB of virtual address space at 1080p —
    // real money in a 32-bit process. The resize path only rebuilds them if they already exist.
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_DoFFocus[2];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_DoFFocusAlloc[2];
    UINT m_DoFFocusSrvSlot[2] = { UINT_MAX, UINT_MAX };
    UINT m_DoFFocusUavSlot[2] = { UINT_MAX, UINT_MAX };
    UINT m_DoFFocusIndex = 0;
    // The focus textures' initial contents are undefined, so the first resolve must SNAP to the measured depth
    // instead of blending against garbage (DoFCB::FocusValid). Cleared whenever the pair is (re)created.
    bool m_DoFFocusValid = false;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_DoFHalf;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_DoFHalfAlloc;
    UINT m_DoFHalfSrvSlot = UINT_MAX;
    UINT m_DoFHalfUavSlot = UINT_MAX;
    INT2 m_DoFHalfSize = { 0, 0 };
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_DoFComposite;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_DoFCompositeAlloc;
    UINT m_DoFCompositeUavSlot = UINT_MAX;
    bool m_DoFResourcesReady = false;
    // Set once creation has been attempted at the current resolution, so a failure (out of heap slots, out of
    // memory) is logged once and not retried every frame while DoF stays enabled. Cleared on resize.
    bool m_DoFCreateAttempted = false;

    bool CreateDoFResources( INT2 size );  // (re)builds the focus pair, the half-res blur target and the scratch
    void RenderDepthOfField();             // focus resolve -> half-res blur -> composite -> copy back

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
    // Sky-IBL tail of the shared shadow CB — the last byte range of m_ShadowCB, after the D3D12ShadowMap::Prepare
    // head [0,256), UploadWetnessConstants [256,352) and the unused AO-reprojection hole [352,432).
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
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_SkyCB[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_SkyCBAlloc[kBackBufferMax];
    uint8_t* m_SkyCBMapped[kBackBufferMax] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_SkyCBGpu[kBackBufferMax] = {};
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
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_FogCB[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_FogCBAlloc[kBackBufferMax];
    uint8_t* m_FogCBMapped[kBackBufferMax] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_FogCBGpu[kBackBufferMax] = {};
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
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_WaterCB[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_WaterCBAlloc[kBackBufferMax];
    uint8_t* m_WaterCBMapped[kBackBufferMax] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_WaterCBGpu[kBackBufferMax] = {};

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
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_RainVobDrawArgs[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_RainVobDrawArgsAlloc[kBackBufferMax];
    uint8_t* m_RainVobDrawArgsPtr[kBackBufferMax] = {};
    UINT     m_RainVobDrawCount = 0;   // built by PrepareRainShadowmap, consumed by RecordRainShadowmap
    UINT     m_RainVobOpaqueDrawCount = 0;   // alpha-test partition — see m_WorldOpaqueDrawCount
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
    void RecordRainShadowmap( D3D12CmdList& cmdList );

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
    Microsoft::WRL::ComPtr<ID3D12Resource> m_VobInstanceBuffer[kBackBufferMax]; // persistently-mapped upload ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_VobInstanceBufferAlloc[kBackBufferMax];
    uint8_t* m_VobInstanceBufferPtr[kBackBufferMax] = {};
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
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ShadowVobInstanceBuffer[kBackBufferMax];
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_ShadowVobInstanceBufferAlloc[kBackBufferMax];
    uint8_t* m_ShadowVobInstanceBufferPtr[kBackBufferMax] = {};
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
    Microsoft::WRL::ComPtr<ID3D12Resource> m_LightBuffer[kBackBufferMax]; // persistently-mapped UPLOAD, GPULight[]
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_LightBufferAlloc[kBackBufferMax];
    uint8_t* m_LightBufferPtr[kBackBufferMax] = {};
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
    Microsoft::WRL::ComPtr<ID3D12Resource> m_SkeletalCBBuffer[kBackBufferMax]; // persistently-mapped upload ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_SkeletalCBBufferAlloc[kBackBufferMax];
    uint8_t* m_SkeletalCBBufferPtr[kBackBufferMax] = {};
    UINT m_SkeletalCBBufferCapacity = 0;
    UINT m_SkeletalCBBufferOffset = 0;                             // reset each OnBeginFrame
    bool m_SkeletalCBOverflowLogged = false;

    // Particle (PFX) path — instanced camera-facing billboards, one instance per live particle. The root sig,
    // shaders, and per-BlendKey PSO cache now live in m_Pipelines.Particle. Per-frame instance ring stays here.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ParticleInstanceBuffer[kBackBufferMax]; // persistently-mapped upload ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_ParticleInstanceBufferAlloc[kBackBufferMax];
    uint8_t* m_ParticleInstanceBufferPtr[kBackBufferMax] = {};
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
    Microsoft::WRL::ComPtr<ID3D12Resource> m_DecalInstanceBuffer[kBackBufferMax]; // persistently-mapped upload ring
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> m_DecalInstanceBufferAlloc[kBackBufferMax];
    uint8_t* m_DecalInstanceBufferPtr[kBackBufferMax] = {};
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
