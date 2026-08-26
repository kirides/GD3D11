#pragma once
#include "D3D11GraphicsEngineBase.h"
#include "D3D11DeferredRenderer.h"
#include "D3D11ForwardPlusRenderer.h"
#include "fpslimiter.h"
#include "GothicAPI.h"
#include "D3D11ShadowMap.h"
#include "D3D11ShaderManager.h"
#include <functional>
#include <array>
#include "D3D11TracyDebug.h"

struct RenderToDepthStencilBuffer;

class D3D11IndirectBuffer;
class D3D11VertexBuffer;
class D3D11ShaderManager;

class D3D11NVAPI;

/** The render-stage enum is now the backend-neutral ERenderStage (BaseGraphicsEngine.h).
    This alias keeps the historical D3D11-side spelling working unchanged. */
using D3D11ENGINE_RENDER_STAGE = ERenderStage;

enum ShadowCubeCasterMask : unsigned int {
    SHADOW_CASTER_WORLD = 1u << 0,
    SHADOW_CASTER_VOBS = 1u << 1,
    SHADOW_CASTER_MOBS = 1u << 2,
    SHADOW_CASTER_ANIMATED = 1u << 3,
    SHADOW_CASTER_ALL = SHADOW_CASTER_WORLD | SHADOW_CASTER_VOBS | SHADOW_CASTER_MOBS | SHADOW_CASTER_ANIMATED,
};

const unsigned int DRAWVERTEXARRAY_BUFFER_SIZE = 4096 * sizeof( ExVertexStruct );
const unsigned int POLYS_BUFFER_SIZE = 1024 * sizeof( ExVertexStruct );
const unsigned int PARTICLES_BUFFER_SIZE = 3072 * sizeof( ParticleInstanceInfo );
const unsigned int MORPHEDMESH_SMALL_BUFFER_SIZE = 3072 * sizeof( ExVertexStruct );
const unsigned int MORPHEDMESH_HIGH_BUFFER_SIZE = 20480 * sizeof( ExVertexStruct );
const int NUM_MAX_BONES = 96;
const int unsigned INSTANCING_BUFFER_SIZE = sizeof( VobInstanceInfo ) * 2048;

class D3D11PointLight;
class D3D11VShader;
class D3D11PShader;
class D3D11PfxRenderer;
class D3D11LineRenderer;
class zCVobLight;
class zCVob;
struct VobLightInfo;
class GMesh;
class D3D11HDShader;
class D3D11OcclusionQuerry;
struct MeshInfo;
struct RenderToTextureBuffer;
class D3D11Effect;

struct AlphaMeshData {
    MeshKey mk;
    MeshInfo* mi;
    MeshVisualInfo* vi;
    unsigned int StartInstanceNum = 0;
    std::vector<VobInstanceInfo> instances;
};

class D3D11GraphicsEngine : public D3D11GraphicsEngineBase {
public:
    D3D11GraphicsEngine();
    ~D3D11GraphicsEngine() override;

    /** Called after the fake-DDraw-Device got created */
    XRESULT Init() override;

    /** Selects the active scene renderer based on the RendererMode setting */
    void SelectActiveRenderer();

    /** Called when the game created its window */
    XRESULT SetWindow( HWND hWnd ) override;

    /** Reset BackBuffer */
    void OnResetBackBuffer() override;

    /** Get BackBuffer Format */
    DXGI_FORMAT GetBackBufferFormat();

    /** Get Window Mode */
    int GetWindowMode();

    XRESULT RecreateBuffers();

    /** (Re-)creates or releases the Forward+ MSAA color/depth buffers based on current settings */
    void RecreateMSAABuffers( INT2 resolution );

    /** Called on window resize/resolution change */
    XRESULT OnResize( INT2 newSize ) override;

    /** Called when the game wants to render a new frame */
    XRESULT OnBeginFrame() override;

    XRESULT TriggerResize( INT2 resolution ) override {
        m_NewResolution = resolution;
        return XR_SUCCESS;
    }

    /** Called when the game ended it's frame */
    XRESULT OnEndFrame() override;

    /** Called to set the current viewport */
    XRESULT SetViewport( const ViewportInfo& viewportInfo ) override;

    /** Called when the game wants to clear the bound rendertarget */
    XRESULT Clear( const float4& color ) override;

    /** Fetches a list of available display modes */
    XRESULT FetchDisplayModeList();
    XRESULT FetchDisplayModeListDXGI();
    XRESULT FetchDisplayModeListWindows();

    /** Returns a list of available display modes */
    XRESULT GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling = false ) override;

    /** Presents the current frame to the screen */
    XRESULT Present() override;

    /** Saves a screenshot */
    void SaveScreenshot() override;

    void DrawString( std::string_view str, float x, float y, const zFont* font, zColor& fontColor ) override;

    //virtual int MeasureString(std::string str, zFont* zFont) override;

    /** Draws a vertexbuffer, non-indexed */
    XRESULT DrawVertexBuffer( GfxVertexBuffer* vb, unsigned int numVertices, unsigned int stride = sizeof( ExVertexStruct ) ) override;
    XRESULT DrawVertexBufferIndexed( GfxVertexBuffer* vb, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset = 0 ) override;
    XRESULT DrawVertexBufferIndexedUINT( GfxVertexBuffer* vb, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset ) override;

    /** Binds the wrapped world mesh's packed (36-byte / ExVertexStructGPU) vertex buffer + its
        32-bit index buffer to the IA. Used by the world-mesh color / alpha submissions that drive
        the packed buffer with VS_ExPacked. */
    void BindWrappedWorldMeshPacked( MeshInfo* wrappedWorldMesh );
    XRESULT DrawDynamicVertexBufferIndexed( std::vector<ExVertexStruct>& vertices, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset ) override;

    /** Draws a vertexbuffer, instanced */
    XRESULT DrawVertexBufferInstanced( GfxVertexBuffer* vb, unsigned int numVertices, unsigned int numInstances, unsigned int stride = sizeof( ExVertexStruct ) ) override;
    XRESULT DrawVertexBufferInstancedIndexed( GfxVertexBuffer* vb, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int numInstances, unsigned int indexOffset = 0 ) override;
    XRESULT DrawVertexBufferInstancedIndexedUINT( GfxVertexBuffer* vb, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int numInstances, unsigned int indexOffset );

    /** Draws a vertexbuffer, non-indexed, binding the FF-Pipe values */
    XRESULT DrawVertexBufferFF( GfxVertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Binds viewport information to the given constantbuffer slot */
    XRESULT BindViewportInformation( VShaderID shader, int slot ) override;

    /** Sets up a draw call for a VS_Ex-Mesh */
    void SetupVS_ExMeshDrawCall() override;
    void PreparePerFrameConstantBuffer(VS_ExConstantBuffer_PerFrame& cb);
    void SetupVS_ExConstantBuffer() override;
    void SetupVS_ExPerInstanceConstantBuffer() override;

    /** Colorspace for HDR-Monitors on Windows 10 */
    void UpdateColorSpace_SwapChain();

    /** Sets up texture with normalmap and fxmap for rendering */
    bool BindTextureNRFX( zCMaterial* mat, zCTexture* tex, bool bindShader, bool updateMaterialInfo = true ) override;
    bool BindTextureNRFX( zCMaterial* mat, bool bindShader, bool updateMaterialInfo = true ) override;

    /** Draws a skeletal mesh */
    XRESULT DrawSkeletalVertexNormals(SkeletalVobInfo* vi, const XMFLOAT4X4& world, const std::span<XMFLOAT4X4> transforms, float4 color, float fatness =
                                          1.0f) override;
    XRESULT DrawSkeletalMesh( SkeletalVobInfo* vi, const std::span<XMFLOAT4X4> transforms, float4 color, const XMFLOAT4X4& world, float fatness = 1.0f ) override;
    XRESULT DrawSkeletalMesh_Layered(SkeletalVobInfo* vi, const std::span<XMFLOAT4X4> transforms, float4 color, XMFLOAT4X4& world, float fatness = 1.0f) override;

    /** Draws a batch of skeletal mesh vobs */
    void DrawSkeletalMeshVobs( const std::vector<SkeletalVobInfo*>& vis, float distance, bool updateState, bool drawAttachments ) override;

    /** Draws a screen fade effects */
    XRESULT DrawScreenFade( void* camera ) override;

    /** Draws a vertexarray, non-indexed */
    XRESULT DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex = 0, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Draws a vertexarray, indexed */
    XRESULT DrawIndexedVertexArray( ExVertexStruct* vertices, unsigned int numVertices, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Draws a batch of instanced geometry */
    XRESULT DrawInstanced( GfxVertexBuffer* vb, GfxVertexBuffer* ib, unsigned int numIndices, GfxVertexBuffer* instanceData, unsigned int instanceDataStride, unsigned int numInstances, unsigned int vertexStride = sizeof( ExVertexStruct ), unsigned int startInstanceNum = 0, unsigned int indexOffset = 0, unsigned int instanceDataByteOffset = 0 ) override;

    /** Called when a vob was removed from the world */
    XRESULT OnVobRemovedFromWorld( zCVob* vob ) override;

    /** Called when a key got pressed */
    XRESULT OnKeyDown( unsigned int key ) override;

    /** Binds the active PixelShader */
    XRESULT BindActivePixelShader() override;
    XRESULT BindActiveVertexShader() override;

    /** ---------------- Sorted transparency -------------------- */

    /** Adds the late kinds (ghosts, decals, quad marks, poly strips) and sorts back to front. */
    void CollectTransparencyQueue();

    /** Replays the sorted queue, one call per maximal same-kind run. */
    void DrawTransparencyQueue();

    void DrawWorldTransparencyRun( std::span<const TransparentItem> items, EWorldTransparencyVariant variant );
    void DrawAlphaVobRun( std::span<const TransparentItem> items );
    void DrawGhostRun( std::span<const TransparentItem> items );
    void DrawDecalRun( std::span<const TransparentItem> items );
    void DrawQuadMarkRun( std::span<const TransparentItem> items );
    void DrawPolyStripRun( std::span<const TransparentItem> items );

    /** Depth re-lay for the world transparency meshes, once, after the replay - the fog/god-ray
        passes downstream sample it. */
    void DrawWorldTransparencyDepthOnly();

    /** Gets the depthbuffer */
    RenderToDepthStencilBuffer* GetDepthBuffer() const { return DepthStencilBuffer.get(); }
    RenderToTextureBuffer* GetDepthBufferCopy() const { return DepthStencilBufferCopy.get(); }

    /** Returns the HDRBackbuffer for regular geometry and effects */
    RenderToTextureBuffer& GetHDRBackBuffer() const { return *HDRBackBuffer; }

    /** MSAA resources for the Forward+ renderer's opaque geometry pass (null/1 sample when MSAA is off or Deferred is active) */
    RenderToTextureBuffer* GetMSAAColorBuffer() const { return MSAAColorBuffer.get(); }
    RenderToDepthStencilBuffer* GetMSAADepthBuffer() const { return MSAADepthStencilBuffer.get(); }
    UINT GetActiveMSAASampleCount() const { return MSAAColorBuffer ? MSAAColorBuffer->GetSampleCount() : 1; }

    /** Resolves MSAADepthStencilBuffer's sample 0 into the single-sample DepthStencilBuffer via a fullscreen pixel shader (no-op if MSAA is inactive) */
    void ResolveMSAADepth();

    /** Unbinds the texture at the given slot */
    XRESULT UnbindTexture( int slot ) override;

    /** Sets up the default rendering state */
    void SetDefaultStates( bool force = false );

    /** Returns the current resolution (Maybe supersampled)*/
    INT2 GetResolution() override { return m_scaledResolution; };

    /** Returns the actual resolution of the backbuffer (not supersampled) */
    INT2 GetBackbufferResolution() override { return Resolution; };
    
    INT2 GetScaledResolution() const { return m_scaledResolution; }

    /** Returns the data of the backbuffer */
    void GetBackbufferData( bool thumbnail, byte** data, INT2& buffersize, int& pixelsize ) override;

    /** Returns the line renderer object */
    BaseLineRenderer* GetLineRenderer() override;

    /** ---------------- Gothic rendering functions -------------------- */

    /** Draws the world mesh */
    XRESULT DrawWorldMesh( bool noTextures = false ) override;

    /** Draws the static VOBs */
    XRESULT DrawVOBs( bool noTextures = false ) override;

    /** Draws a VOB (used for inventory) */
    void DrawVobSingle( VobInfo* vob, zCCamera& camera ) override;

    /** Draws everything around the given position */
    void ShadowPass_DrawWorldMesh_Indirect( const std::vector<WorldMeshSectionInfo*>& visibleSections, const Frustum* cullingFrustum = nullptr );
    void ShadowPass_DrawWorldMesh( const std::vector<WorldMeshSectionInfo*>& visibleSections, const Frustum* cullingFrustum = nullptr );

    void XM_CALLCONV DrawWorldAroundForWorldShadow( FXMVECTOR position, float sectionRange, const RenderShadowmapsParams& params );
    void DrawVegetationGeometryPass(const std::list<GVegetationBox*>& vegetationBoxes);
    void XM_CALLCONV DrawWorldAround( FXMVECTOR position,
                                      float range,
                                      bool cullFront = true,
                                      bool indoor = false,
                                      bool noNPCs = false,
                                      std::list<VobInfo*>* renderedVobs = nullptr, std::list<SkeletalVobInfo*>* renderedMobs = nullptr, std::vector<MeshDrawRange>* worldMeshCache = nullptr,
                                      unsigned int casterMask = SHADOW_CASTER_ALL,
                                      const std::move_only_function<bool(const zCVob*) const>& ignoreVob = nullptr );
    void XM_CALLCONV DrawWorldAround_Layered( FXMVECTOR position,
        float range,
        bool cullFront = true,
        bool indoor = false,
        bool noNPCs = false,
        std::list<VobInfo*>* renderedVobs = nullptr, std::list<SkeletalVobInfo*>* renderedMobs = nullptr, std::vector<MeshDrawRange>* worldMeshCache = nullptr,
        unsigned int casterMask = SHADOW_CASTER_ALL,
        const std::move_only_function<bool(const zCVob*) const>& ignoreVob = nullptr );

    /** Update morph mesh visual */
    void UpdateMorphMeshVisual();

    /** Draws the static vobs instanced */
    XRESULT DrawVOBsInstanced();

    /** Per-visual wind metadata for this frame's alpha VOB batches; once, before the replay. */
    void PrepareAlphaMeshWindMetadata();

    /** Set wind props in const buffer */
    void ApplyWindProps( VS_ExConstantBuffer_Wind& windBuff );

    /** Called when we started to render the world */
    XRESULT OnStartWorldRendering() override;

    /** Draws the sky using the GSky-Object */
    XRESULT DrawSky() override;

    /** Rotation-only motion vectors for every pixel still at the reversed-Z far plane (the sky). */
    void RenderSkyVelocity( RenderToTextureBuffer* velocityBuffer );

    /** True when something downstream actually consumes the main velocity buffer this frame. */
    bool IsVelocityBufferInUse() const;

    /** Renders the shadowmaps for the sun */
    void XM_CALLCONV RenderShadowmaps( FXMVECTOR cameraPosition, RenderToDepthStencilBuffer* target = nullptr, bool cullFront = true, bool dontCull = false, Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsvOverwrite = nullptr, Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV = nullptr );

    /** Renders the shadowmaps for a pointlight */
    void XM_CALLCONV RenderShadowCube( FXMVECTOR position,
        float range,
        const RenderToDepthStencilBuffer& targetCube,
        const ComPtr<ID3D11DepthStencilView>& face,
        const ComPtr<ID3D11RenderTargetView>& debugRTV,
        bool cullFront = true,
        bool indoor = false,
        bool noNPCs = false,
        std::list<VobInfo*>* renderedVobs = nullptr, std::list<SkeletalVobInfo*>* renderedMobs = nullptr, std::vector<MeshDrawRange>* worldMeshCache = nullptr,
        bool clearDepth = true,
        unsigned int casterMask = SHADOW_CASTER_ALL,
        const std::move_only_function<bool( const zCVob* ) const>& ignoreVob = nullptr);

    /** Updates the occlusion for the bsp-tree */
    void UpdateOcclusion();

    /** Recreates the renderstates */
    XRESULT UpdateRenderStates() override;

    /** Draws a fullscreenquad, copying the given texture to the viewport */
    void DrawQuad( INT2 position, INT2 size ) override;

    /** Sets the current rendering stage */
    void SetRenderingStage( D3D11ENGINE_RENDER_STAGE stage );

    /** Returns the current rendering stage */
    D3D11ENGINE_RENDER_STAGE GetRenderingStage() override;

    /** True while D3D11PointLight::RenderShadowCubeFacePasses is issuing its per-face draws (the
        RequiresNvidiaTiledShadowFaceFallback workaround). GetRenderingStage() alone reads DES_SHADOWMAP_CUBE
        in both this path AND the normal GS_Cubemap/VS_ExLayered path, but this fallback never binds a
        geometry shader - it draws each face as an ordinary single-view pass into a single-slice DSV. Shader
        selection sites that pick a *Cube-suffixed vertex shader (VS_ExSkeletalCube, VS_ExNodeCube - they
        output world-space position and rely on GS_Cubemap to produce SV_Position per face) must check this
        too, or that GS-less draw call rasterizes nothing at all. See DrawSkeletalMesh / GothicAPI::
        DrawSkeletalMeshVob. */
    void SetCubeFaceFallbackActive( bool active ) { CubeFaceFallbackActive = active; }
    bool IsCubeFaceFallbackActive() const { return CubeFaceFallbackActive; }

    /** Reloads shaders */
    XRESULT ReloadShaders( ShaderCategory categories = ShaderCategory::All) override;

    /** Draws the given mesh infos as water */
    void DrawWaterSurfaces() override;

    /** Draws the given list of decals */
    void DrawDecalList( const std::vector<zCVob*>& decals, bool lighting );

    /** Draws underwater effects */
    void DrawUnderwaterEffects();

    /** Binds the right shader for the given texture */
    bool BindShaderForTexture( zCTexture* texture, bool forceAlphaTest = false, int zMatAlphaFunc = 0, MaterialInfo::EMaterialType materialInfo = MaterialInfo::MT_None );

    /** Copies the depth stencil buffer to DepthStencilBufferCopy */
    void CopyDepthStencil();

    /** Adds a pass that fills an R8_UNORM screen-space AO mask (HBAO+/ASSAO/SAO per AoMode),
        white-cleared so it reads as "no occlusion" when AO is disabled. The mask is later
        sampled in the lighting pass and applied to indirect light only.
        normalsResource may be RG_INVALID_HANDLE; depthOnlyNormals selects the depth-only
        producer path (Forward+ without the smooth-normals pre-pass). Returns the mask handle. */
    RGResourceHandle AddAOMaskPass( RenderGraph& graph, RGResourceHandle normalsResource, bool depthOnlyNormals );

    /** Adds an optional compute pass that reconstructs smooth view-space normals from the
        depth copy into a transient R16G16_FLOAT (octahedral) texture, for Forward+ AO.
        Returns the normals handle. */
    RGResourceHandle AddAONormalsFromDepthPass( RenderGraph& graph );

    /** Draws particle meshes */
    void DrawFrameParticleMeshes( std::unordered_map<zCVob*, std::unique_ptr<MeshVisualInfo>>& progMeshes ) override;

    /** Draws particle effects into the given engine-owned refraction targets. Backend-internal:
        the render graph creates the two targets and calls this directly (not via the base interface). */
    void DrawFrameParticles(std::map<zCTexture*, std::vector<ParticleInstanceInfo>>& particles, std::map<zCTexture*, ParticleRenderInfo>& info, RenderToTextureBuffer
                            * bufferParticleColor, RenderToTextureBuffer* bufferParticleDistortion);

    /** Returns a dummy cube-rendertarget used for pointlight shadowmaps */
    RenderToTextureBuffer* GetDummyCubeRT() const { return ShadowMaps ? ShadowMaps->GetDummyCubeRT() : nullptr; }

    void EnsureTempVertexBufferSize( std::unique_ptr<D3D11VertexBuffer>& buffer, UINT size );

    float UpdateCustomFontMultiplierFontRendering( float multiplier );

    // TODO: Remove from here, put into D3D11ShadowMaps
    D3D11PointLight* DebugPointlight;

    // Using a list here to determine which lights to update, since we don't want to update every light every frame.
    std::list<VobLightInfo*> FrameShadowUpdateLights;
    
    /** Effects wrapper */
    std::unique_ptr<D3D11Effect> Effects;

    D3D11PfxRenderer* GetPfxRenderer() const { return PfxRenderer.get(); }

    // --- Per-frame dynamic constant-buffer ring API ----------------------
    // For small per-draw/per-object/per-frame CBs (grass, view-distances, Forward+
    // sun/tile/atmosphere, TAA, point-light cubemap view matrices, ...). Each call
    // sub-allocates a transient slot from a per-frame ring (DYNAMIC buffer mapped
    // with DISCARD/NO_OVERWRITE) and binds it by offset, so callers don't need their
    // own ID3D11Buffer. The allocation is only valid for the current frame.
    ConstantBufferAllocation AllocateDynamicCB( const void* data, uint32_t size ) {
        return DynamicConstantBufferPool->Allocate( data, size );
    }
    
    template<typename T>
        requires (!std::is_pointer_v<T> && std::is_trivially_copyable_v<T>)
    ConstantBufferAllocation AllocateDynamicCB( const T* data ) {
        return DynamicConstantBufferPool->Allocate( data, sizeof(T) );
    }
    
    ConstantBufferPool* GetConstantBufferPool() override {
        return DynamicConstantBufferPool.get();
    };
    
    void BindDynamicCBToVertexShader( int slot, const ConstantBufferAllocation& a ) {
        if ( slot < 0 || !a.pBuffer ) return;
        UINT first = a.offsetInBytes / 16; UINT num = a.sizeInBytes / 16;
        GetContext()->VSSetConstantBuffers1( static_cast<UINT>( slot ), 1, &a.pBuffer, &first, &num );
    }
    void BindDynamicCBToPixelShader( int slot, const ConstantBufferAllocation& a ) {
        if ( slot < 0 || !a.pBuffer ) return;
        UINT first = a.offsetInBytes / 16; UINT num = a.sizeInBytes / 16;
        GetContext()->PSSetConstantBuffers1( static_cast<UINT>( slot ), 1, &a.pBuffer, &first, &num );
    }
    void BindDynamicCBToGeometryShader( int slot, const ConstantBufferAllocation& a ) {
        if ( slot < 0 || !a.pBuffer ) return;
        UINT first = a.offsetInBytes / 16; UINT num = a.sizeInBytes / 16;
        GetContext()->GSSetConstantBuffers1( static_cast<UINT>( slot ), 1, &a.pBuffer, &first, &num );
    }

    D3D11Texture* GetDistortionTexture() const { return DistortionTexture.get(); }
    D3D11Texture* GetBlueNoiseTexture() const { return BlueNoise512BGRA.get(); }
    D3D11Texture* GetWhiteTexture() const { return WhiteTexture.get(); }
    D3D11Texture* GetBlackTexture() const { return BlackTexture.get(); }

    RenderToTextureBuffer* GetVelocityBuffer() const { return VelocityBuffer.get(); }

    const XMFLOAT4X4& GetPrevViewProjMatrix() const { return m_PrevViewProjMatrix; }
    void StorePrevViewProjMatrix();

    auto GetClampSamplerState() -> auto { return ClampSamplerState.Get(); }
    auto GetCubeSamplerState() -> auto { return CubeSamplerState.Get(); }
    auto GetLinearSamplerState() -> auto { return LinearSamplerState.Get(); }

    D3D11ShadowMap* GetShadowMaps() const { return ShadowMaps.get(); }

    void SetFrameNeedsJitter() { m_FrameNeedsJitter = true; }

    void StoreVobPreviousTransforms();

    GraphicsEventRecord RecordGraphicsEvent( GraphicsEventName region ) override {
        return GraphicsEventRecord( m_UserDefinedAnnotation.Get(), region );
    }

private:
    // Ring of FrameCount slots, one used per frame-in-flight - mirrors ConstantBufferPool's
    // design. Each slot owns one growable buffer plus an ID3D11Query fence; BeginFrame() waits
    // on the fence of the slot it's about to reuse (guaranteeing the GPU is done reading whatever
    // that slot held FrameCount frames ago) so every sub-allocation can use NO_OVERWRITE instead
    // of relying on MAP_DISCARD to rename the backing allocation.
    static constexpr uint32_t kTransientPoolFrameCount = 3;

    struct FrameIndirectBufferPool {
        struct Slot {
            std::unique_ptr<D3D11IndirectBuffer> Buffer;
            Microsoft::WRL::ComPtr<ID3D11Query> Fence;
            bool FencePending = false;
            uint32_t Capacity = 0;
            uint32_t Offset = 0;
        };

        std::array<Slot, kTransientPoolFrameCount> Slots;
        uint32_t FrameIndex = 0;
    };

    struct FrameInstancingBufferPool {
        struct Slot {
            std::unique_ptr<D3D11VertexBuffer> Buffer;
            Microsoft::WRL::ComPtr<ID3D11Query> Fence;
            bool FencePending = false;
            uint32_t Capacity = 0;
            uint32_t Offset = 0;
        };

        std::array<Slot, kTransientPoolFrameCount> Slots;
        uint32_t FrameIndex = 0;
    };

    struct FrameIndirectAllocation {
        D3D11IndirectBuffer* Buffer = nullptr;
        uint32_t OffsetInBytes = 0;
    };

    struct FrameInstancingAllocation {
        D3D11VertexBuffer* Buffer = nullptr;
        uint32_t OffsetInBytes = 0;
    };

    void BeginFrameTransientBufferPools();
    void EndFrameTransientBufferPools();
    void WaitForTransientPoolFence( Microsoft::WRL::ComPtr<ID3D11Query>& fence, bool& fencePending );
    FrameIndirectAllocation AcquireFrameIndirectAllocation( FrameIndirectBufferPool& pool, const void* initData, unsigned int sizeInBytes, const char* debugName );
    FrameInstancingAllocation AcquireFrameInstancingAllocation( FrameInstancingBufferPool& pool, unsigned int sizeInBytes, const char* debugName );

protected:

    int m_LastFrameLimit;


    /** D3D11 Objects */
    Microsoft::WRL::ComPtr<ID3D11SamplerState> ClampSamplerState;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> CubeSamplerState;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> LinearSamplerState;

    /** Swapchain buffers */
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> BackbufferRTV;
    std::unique_ptr<RenderToTextureBuffer> DepthStencilBufferCopy;
    // DummyShadowCubemapTexture moved into ShadowMaps
    std::unique_ptr<D3D11ShadowMap> ShadowMaps;

    /** Deferred renderer (GBuffer pass, lighting pass, shader selection) */
    D3D11DeferredRenderer DeferredRenderer;

    /** Forward+ renderer (depth prepass, light culling, lit geometry pass) */
    D3D11ForwardPlusRenderer ForwardPlusRenderer{ DeferredRenderer };

    /** Active scene renderer, selected by RendererMode setting */
    ISceneRenderer* ActiveSceneRenderer = nullptr;

    /** Temp-Arrays for storing data to be put in constant buffers */
    float2 Temp2Float2[2];
    std::unique_ptr<D3D11VertexBuffer> DynamicInstancingBuffer;
    std::unique_ptr<D3D11VertexBuffer> DecalInstancingBuffer;
    std::unique_ptr<D3D11VertexBuffer> DynamicVertexBuffer;

    /** Post processing */
    std::unique_ptr<D3D11PfxRenderer> PfxRenderer;

    /** Sky */
    std::unique_ptr<D3D11Texture> DistortionTexture;
    std::unique_ptr<D3D11Texture> NoiseTexture;
    std::unique_ptr<D3D11Texture> WhiteTexture;
    std::unique_ptr<D3D11Texture> BlackTexture;
    std::unique_ptr<D3D11Texture> BlueNoise512BGRA;

    /** Shadowing */
    std::vector<VobInfo*> RenderedVobs;

    /** The current rendering stage */
    D3D11ENGINE_RENDER_STAGE RenderingStage;

    /** See SetCubeFaceFallbackActive(). */
    bool CubeFaceFallbackActive = false;

    /** List of water surfaces for this frame */
    std::unordered_map<zCTexture*, std::vector<MeshInfo*>> FrameWaterSurfaces;

    INT2 m_scaledResolution;

public:
    /** Lighting */
    GMesh* InverseUnitSphereMesh;
    /** Reflection */
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ReflectionCube;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ReflectionCube2;
private:
    bool PrepareAndBindWindMetadata( const std::vector<MeshVisualInfo*>& activeVisuals );
    void UnbindWindMetadata();

    std::vector<AlphaMeshData> m_AlphaMeshes;

    bool m_AlphaMeshWindMetadataValid = false;

    /** Instanced draws the alpha VOB emitter needed; vs. the instance count = surviving batching. */
    unsigned int m_AlphaVobDrawsThisFrame = 0;

    std::vector<VobLightInfo*> m_FrameLights;
    std::vector<VobWindMetadata> m_WindMetadataStaging;

    /** Optional per-visual wind metadata for FL11+ instanced VOB rendering. */
    std::unique_ptr<D3D11VertexBuffer> WindMetadataBuffer;
    
    /** World-Mesh indirect buffer */
    std::unique_ptr<D3D11IndirectBuffer> WorldMeshIndirectBuffer;
public:
    std::vector<VobLightInfo*>& GetFrameLights() { return m_FrameLights; }
private:

    /** Per-frame geometry cache: culling and draw-arg building is done once per frame.
     *  The Z-prepass populates this; the lit geometry pass reuses it. */
    struct FrameGeometryCache {
        struct CachedWorldMeshDraw {
            zCTexture* Texture = nullptr;
            MeshInfo* Mesh = nullptr;
            MaterialInfo* MeshMaterialInfo = nullptr;
            float DistanceSq = 0.0f;
            bool AlphaTest = false;
        };

        /// Snapshot of a static-mesh visual and its per-frame instance data.
        /// Avoids reliance on MeshVisualInfo::Instances across shadow-map passes.
        struct CachedVobVisual {
            MeshVisualInfo* Visual = nullptr;
            std::vector<VobInstanceInfo> Instances;
            unsigned int                 StartInstanceNum = 0;
        };

        struct SortKeyBuilder {
        public:
            uint64_t sortKey;
        public:
            uint8_t GetAlphaType() const {
                return (sortKey >> alpha_type_offset) & alpha_type_mask;
            }

            SortKeyBuilder& withAlphaType( uint8_t alphaType ) {
                sortKey = (sortKey & ~alpha_type_mask) | ((static_cast<uint64_t>(alphaType) & alpha_type_mask) << alpha_type_offset);
                return *this;
            }

            SortKeyBuilder& withTexture( uint64_t texture_id ) {
                uint64_t textureId = texture_id;
                sortKey = (sortKey & ~texture_id_mask) | ((textureId & texture_id_mask) << texture_id_offset);
                return *this;
            }

            SortKeyBuilder& withMesh( uint16_t mesh_id ) {
                sortKey = (sortKey & ~mesh_id_mask) | ((static_cast<uint64_t>(mesh_id) & mesh_id_mask) << mesh_id_offset);
                return *this;
            }

            operator uint64_t() { return sortKey; }

        private:
            static const uint64_t alpha_type_mask = 0b11ull;
            static const uint64_t alpha_type_offset = 62;

            static const uint64_t texture_id_mask = 0xFFFFFFFF;
            static const uint64_t texture_id_offset = 16;

            static const uint64_t mesh_id_mask = 0xFFFF;
            static const uint64_t mesh_id_offset = 0;
        };

        struct CachedInstancedMeshDraw {
        public:
            uint64_t sortKey;
            unsigned int VisualIndex = 0;
            MeshKey Mesh;
            MeshInfo* MeshEntry = nullptr;
        };

        /// Snapshot of one texture/mesh instanced-draw batch built while collecting
        /// node attachments, so a later stage in the same frame (e.g. the lit pass
        /// reusing the Z-prepass' work) can replay it without rebuilding.
        struct CachedNodeAttachmentBatch {
            MeshInfo* Mesh = nullptr;
            zCTexture* Texture = nullptr;
            zCMaterial* Material = nullptr;
            unsigned int StartInstance = 0;
            unsigned int InstanceCount = 0;
            bool NeedAlpha = false;
        };

        bool worldMeshBuilt    = false;  ///< CollectVisibleSections + MDI arg build + buffer upload done
        bool vobInstancesUploaded = false; ///< CollectVisibleVobs + DynamicInstancingBuffer upload done
        bool vobWindMetadataPrepared = false; ///< Wind metadata prepared for cached vob visuals
        bool skeletalBonesUploaded = false; ///< FL11 packed skeletal bone buffers uploaded for main/z-prepass reuse
        bool nodeAttachmentInstancesUploaded = false; ///< Node-attachment instance buffer uploaded for main/z-prepass reuse

        std::vector<WorldMeshSectionInfo*> visibleSections;
        std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> drawIndirectArgs;
        std::vector<CachedWorldMeshDraw> sortedDepthWorldMeshes;
        D3D11IndirectBuffer*           MainWorldIndirectArgsBuffer = nullptr;
        D3D11VertexBuffer*             MainVobInstancingBuffer = nullptr;
        uint32_t                       MainVobInstancingBufferOffset = 0; ///< byte offset of this frame's sub-allocation within MainVobInstancingBuffer
        std::vector<VobWindMetadata>   vobWindMetadata;
        std::vector<CachedVobVisual>    vobVisuals;
        std::vector<CachedInstancedMeshDraw> sortedInstancedMeshes;
        std::vector<SkeletalVobInfo*>   cachedMobs;
        std::vector<SkeletalVobInfo*> skeletalBoneVisOrder;
        std::vector<VS_ExConstantBuffer_SkeletalBoneRange> skeletalBoneRanges;
        std::vector<SkeletalVobInfo*> nodeAttachmentVisOrder;
        std::vector<CachedNodeAttachmentBatch> nodeAttachmentBatches;
        D3D11VertexBuffer*             NodeAttachmentInstancingBuffer = nullptr;
        uint32_t                       NodeAttachmentInstancingBufferOffset = 0; ///< byte offset of this frame's sub-allocation within NodeAttachmentInstancingBuffer

        void Reset() {
            worldMeshBuilt      = false;
            vobInstancesUploaded = false;
            vobWindMetadataPrepared = false;
            skeletalBonesUploaded = false;
            nodeAttachmentInstancesUploaded = false;
            visibleSections.clear();
            drawIndirectArgs.clear();
            sortedDepthWorldMeshes.clear();
            MainWorldIndirectArgsBuffer = nullptr;
            MainVobInstancingBuffer = nullptr;
            MainVobInstancingBufferOffset = 0;
            vobWindMetadata.clear();
            vobVisuals.clear();
            sortedInstancedMeshes.clear();
            cachedMobs.clear();
            skeletalBoneVisOrder.clear();
            skeletalBoneRanges.clear();
            nodeAttachmentVisOrder.clear();
            nodeAttachmentBatches.clear();
            NodeAttachmentInstancingBuffer = nullptr;
            NodeAttachmentInstancingBufferOffset = 0;
        }
    };
    FrameGeometryCache m_FrameGeometryCache;

    FrameIndirectBufferPool m_MainWorldIndirectPool;
    FrameIndirectBufferPool m_ShadowWorldIndirectPool;
    FrameInstancingBufferPool m_MainVobInstancingPool;
    FrameInstancingBufferPool m_ShadowVobInstancingPool;
    FrameInstancingBufferPool m_MainNodeAttachmentInstancingPool;
    FrameInstancingBufferPool m_ShadowNodeAttachmentInstancingPool;
    // DrawVertexArray's dynamic vertex data (2D UI/FF-pipe quads, glyph runs, Bink YUV quad, ...).
    // Was a single D3D11VertexBuffer remapped WRITE_DISCARD on every call (TempHUDVertexBuffer, now
    // removed); every DrawPrimitive-driven UI draw forced a fresh driver-side rename. This pool gets
    // the same fenced-ring/NO_OVERWRITE treatment as the instancing pools above instead.
    FrameInstancingBufferPool m_UIVertexPool;

    /** Water surface indirect buffer */
    std::unique_ptr<D3D11IndirectBuffer> WaterIndirectBuffer;

    /** FL11 packed structured buffers for skeletal skinning (main/z-prepass reusable path). */
    std::unique_ptr<D3D11VertexBuffer> SkeletalBoneTransformsBuffer;
    std::unique_ptr<D3D11VertexBuffer> SkeletalPrevBoneTransformsBuffer;

    /** FL11 packed structured buffers for non-reusable stages (shadow/cube/debug paths). */
    std::unique_ptr<D3D11VertexBuffer> SkeletalBoneTransformsBufferTransient;
    std::unique_ptr<D3D11VertexBuffer> SkeletalPrevBoneTransformsBufferTransient;

    /** Cached bone transforms for batched skeletal mesh drawing */
    std::vector<XMFLOAT4X4> BoneTransformCache;

    /** View-distance constant buffers. These are re-allocated from the per-frame
        dynamic ring each frame (bound at many draw sites, updated rarely), so the
        allocation handle is kept between the frame-start update and the binds. */
    ConstantBufferAllocation InfiniteRangeCB;
    ConstantBufferAllocation OutdoorSmallVobsCB;
    ConstantBufferAllocation OutdoorVobsCB;
    std::unique_ptr<ConstantBufferPool> PerObjectMaterialInfoPooledBuffer;

    /** Last MaterialInfo whose constant buffer is known to be bound. Reset to nullptr
        at the start of DrawVOBsInstanced, DrawSkeletalMeshVobs and DrawWorldMesh, since
        we can't otherwise be sure what any given draw path last bound. Checked via
        MaterialInfo::IsSame() to skip re-allocating/re-binding an identical buffer. */
    MaterialInfo* m_LastMaterialInfo = nullptr;

    /** Per-frame ring for small per-draw/per-object dynamic constant buffers
        (grass, and other buffers migrated off their own ID3D11Buffer). */
    std::unique_ptr<ConstantBufferPool> DynamicConstantBufferPool;

    /** Quads for decals/particles */
    std::unique_ptr<GfxVertexBuffer> QuadVertexBuffer;
    std::unique_ptr<GfxVertexBuffer> QuadIndexBuffer;

    /** Occlusion query manager */
    std::unique_ptr<D3D11OcclusionQuerry> Occlusion;

    /** Temporary vertex buffers */
    std::unique_ptr<D3D11VertexBuffer> TempPolysVertexBuffer;
    std::unique_ptr<D3D11VertexBuffer> TempParticlesVertexBuffer;
    std::unique_ptr<D3D11VertexBuffer> TempMorphedMeshSmallVertexBuffer;
    std::unique_ptr<D3D11VertexBuffer> TempMorphedMeshBigVertexBuffer;

    /** Cached refresh rate for the current exclusive-fullscreen mode (D3D11-only concept). */
    DXGI_RATIONAL CachedRefreshRate;

    /** Low latency object handle */
    HANDLE frameLatencyWaitableObject;

public:
    HANDLE GetFrameLatencyWaitableObject() const override { return frameLatencyWaitableObject; }

private:

    /** If true, we will save a screenshot after the next frame */
    bool SaveScreenshotNextFrame;

    bool m_flipWithTearing;
    bool m_swapchainflip;
    bool m_lowlatency;
    bool m_HDR;
    int m_previousFpsLimit;
    bool m_FrameNeedsJitter;
    float unionCurrentCustomFontMultiplier;

    std::unique_ptr<RenderToTextureBuffer> VelocityBuffer;
    XMFLOAT4X4 m_PrevViewProjMatrix;

    void CreateAndBindDefaultSampler();
};

/** Checked downcast of the global engine to the concrete D3D11 engine. Replaces the
    blind reinterpret_cast pattern: asserts the backend tag in debug builds, then
    performs a proper (inheritance-aware) static_cast. Returns nullptr for null. */
inline D3D11GraphicsEngine* AsD3D11Engine( BaseGraphicsEngine* engine ) {
    if ( !engine ) return nullptr;
    assert( engine->GetBackendAPI() == EGraphicsEngineBackend::D3D11
        && "AsD3D11Engine called on a non-D3D11 graphics engine" );
    return static_cast<D3D11GraphicsEngine*>( engine );
}

/** Same as AsD3D11Engine, but for code that can legitimately run under any backend: returns
    nullptr instead of asserting when the active engine isn't D3D11. Use this at every call site
    reachable from backend-neutral code (Gothic hooks, ImGui, exported script bindings):
    if ( auto d3d11Engine = TryAsD3D11Engine( Engine::GraphicsEngine ) ) { ... } */
inline D3D11GraphicsEngine* TryAsD3D11Engine( BaseGraphicsEngine* engine ) {
    return engine && engine->GetBackendAPI() == EGraphicsEngineBackend::D3D11
        ? dynamic_cast<D3D11GraphicsEngine*>( engine )
        : nullptr;
}
