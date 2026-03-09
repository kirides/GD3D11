#pragma once
#include "D3D11GraphicsEngineBase.h"
#include "fpslimiter.h"
#include "GothicAPI.h"
#include "D3D11ShadowMap.h"
#include "D3D11ShaderManager.h"
#include "D3D11TextureAtlasManager.h"
#include "D3D11StructuredBuffer.h"
#include "D3D11IndirectBuffer.h"
#include "D3D11StreamingResourcesManager.h"
#include "VobCulling.h"

struct RenderToDepthStencilBuffer;

class D3D11IndirectBuffer;
class D3D11ConstantBuffer;
class D3D11VertexBuffer;
class D3D11ShaderManager;

class D3D11NVAPI;

enum D3D11ENGINE_RENDER_STAGE {
    DES_Z_PRE_PASS,
    DES_MAIN,
    DES_SHADOWMAP,
    DES_SHADOWMAP_CUBE,
    DES_GHOST
};

const unsigned int DRAWVERTEXARRAY_BUFFER_SIZE = 4096 * sizeof( ExVertexStruct );
const unsigned int POLYS_BUFFER_SIZE = 1024 * sizeof( ExVertexStruct );
const unsigned int PARTICLES_BUFFER_SIZE = 3072 * sizeof( ParticleInstanceInfo );
const unsigned int MORPHEDMESH_SMALL_BUFFER_SIZE = 3072 * sizeof( ExVertexStruct );
const unsigned int MORPHEDMESH_HIGH_BUFFER_SIZE = 20480 * sizeof( ExVertexStruct );
const unsigned int HUD_BUFFER_SIZE = 6 * sizeof( ExVertexStruct );
const int NUM_MAX_BONES = 96;
const int unsigned INSTANCING_BUFFER_SIZE = sizeof( VobInstanceInfo ) * 2048;
constexpr size_t TEXTURE_ATLAS_MAX = DXGI_FORMAT_V408 + 1;


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
    std::vector<VobInstanceInfo> instances;
};

// Tracks one unique submesh in the global geometry buffer
struct StaticSubmeshEntry {
    UINT indexCount;
    UINT startIndexLocation;   // offset into global IB
    int  baseVertexLocation;   // offset into global VB
    TextureDescriptor atlasDesc;
    MeshVisualInfo* visual;    // which visual owns this submesh
};

// Groups all submeshes that share one atlas (same DXGI_FORMAT)
struct AtlasDrawGroup {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<StaticSubmeshEntry> submeshes;
    std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> indirectArgs;
    std::unique_ptr<D3D11IndirectBuffer> indirectBuffer;
    UINT mergedArgsOffset = 0;  // byte offset into merged indirect args buffer
    UINT mergedArgsCount = 0;   // number of args in this group
};

class D3D11GraphicsEngine : public D3D11GraphicsEngineBase {
public:
    D3D11GraphicsEngine();
    ~D3D11GraphicsEngine() override;

    /** Called after the fake-DDraw-Device got created */
    XRESULT Init() override;

    /** Called when the game created its window */
    XRESULT SetWindow( HWND hWnd ) override;

    /** Reset BackBuffer */
    void OnResetBackBuffer();

    /** Get BackBuffer Format */
    DXGI_FORMAT GetBackBufferFormat();

    /** Get Window Mode */
    int GetWindowMode();

    XRESULT RecreateBuffers();
    /** Called on window resize/resolution change */
    XRESULT OnResize( INT2 newSize ) override;

    /** Called when the game wants to render a new frame */
    XRESULT OnBeginFrame() override;

    XRESULT TriggerResize( INT2 resolution ) override {
        NewResolution = resolution;
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

    void DrawString( const std::string& str, float x, float y, const zFont* font, zColor& fontColor ) override;

    //virtual int MeasureString(std::string str, zFont* zFont) override;

    /** Draws a vertexbuffer, non-indexed */
    XRESULT DrawVertexBuffer( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int stride = sizeof( ExVertexStruct ) ) override;
    XRESULT DrawVertexBufferIndexed( D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset = 0 ) override;
    XRESULT DrawVertexBufferIndexedUINT( D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset ) override;
    XRESULT DrawDynamicVertexBufferIndexed( std::vector<ExVertexStruct>& vertices, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset ) override;

    /** Draws a vertexbuffer, instanced */
    XRESULT DrawVertexBufferInstanced( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int numInstances, unsigned int stride = sizeof( ExVertexStruct ) );
    XRESULT DrawVertexBufferInstancedIndexed( D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int numInstances, unsigned int indexOffset = 0 );
    XRESULT DrawVertexBufferInstancedIndexedUINT( D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int numInstances, unsigned int indexOffset );

    /** Draws a vertexbuffer, non-indexed, binding the FF-Pipe values */
    XRESULT DrawVertexBufferFF( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Binds viewport information to the given constantbuffer slot */
    XRESULT BindViewportInformation( VShaderID shader, int slot ) override;

    /** Sets up a draw call for a VS_Ex-Mesh */
    void SetupVS_ExMeshDrawCall() override;
    void SetupVS_ExConstantBuffer() override;
    void SetupVS_ExPerInstanceConstantBuffer() override;

    /** Colorspace for HDR-Monitors on Windows 10 */
    void UpdateColorSpace_SwapChain();

    /** Sets up texture with normalmap and fxmap for rendering */
    bool BindTextureNRFX( zCTexture* tex, bool bindShader );

    /** Draws a skeletal mesh */
    XRESULT DrawSkeletalVertexNormals(SkeletalVobInfo* vi, const XMFLOAT4X4& world, const std::span<XMFLOAT4X4> transforms, float4 color, float fatness =
                                          1.0f);
    XRESULT DrawSkeletalMesh( SkeletalVobInfo* vi, const std::span<XMFLOAT4X4> transforms, float4 color, const XMFLOAT4X4& world, float fatness = 1.0f ) override;
    XRESULT DrawSkeletalMesh_Layered(SkeletalVobInfo* vi, const std::span<XMFLOAT4X4> transforms, float4 color, XMFLOAT4X4& world, float fatness = 1.0f);

    /** Draws a batch of skeletal mesh vobs */
    void DrawSkeletalMeshVobs( const std::vector<SkeletalVobInfo*>& vis, float distance, bool updateState, bool drawAttachments );

    /** Draws a screen fade effects */
    XRESULT DrawScreenFade( void* camera ) override;

    /** Draws a vertexarray, non-indexed */
    XRESULT DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex = 0, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Draws a vertexarray, indexed */
    XRESULT DrawIndexedVertexArray( ExVertexStruct* vertices, unsigned int numVertices, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Draws a batch of instanced geometry */
    XRESULT DrawInstanced( D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices, D3D11VertexBuffer* instanceData, unsigned int instanceDataStride, unsigned int numInstances, unsigned int vertexStride = sizeof( ExVertexStruct ), unsigned int startInstanceNum = 0, unsigned int indexOffset = 0 ) override;

    /** Called when a vob was removed from the world */
    XRESULT OnVobRemovedFromWorld( zCVob* vob ) override;

    /** Called when a key got pressed */
    XRESULT OnKeyDown( unsigned int key ) override;

    /** Binds the active PixelShader */
    XRESULT BindActivePixelShader() override;
    XRESULT BindActiveVertexShader() override;

    /** Draws quadmarks in a simple way */
    void DrawQuadMarks();
    void DrawMQuadMarks();

    /** Gets the depthbuffer */
    RenderToDepthStencilBuffer* GetDepthBuffer() const { return DepthStencilBuffer.get(); }
    RenderToTextureBuffer* GetDepthBufferCopy() const { return DepthStencilBufferCopy.get(); }

    /** Returns the HDRBackbuffer */
    RenderToTextureBuffer& GetHDRBackBuffer() const { return *HDRBackBuffer; }

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
    XRESULT DrawWorldMesh_Indirect( bool noTextures = false ) override;
    XRESULT DrawWorldMesh( bool noTextures = false ) override;

    /** Draws a list of mesh infos */
    XRESULT DrawMeshInfoListAlphablended( const std::vector<std::pair<MeshKey, MeshInfo*>>& list );

    /** Draws the static VOBs */
    XRESULT DrawVOBs( bool noTextures = false ) override;

    /** Draws PolyStrips (weapon and particle trails) */
    XRESULT DrawPolyStrips( bool noTextures = false ) override;

    /** Draws a VOB (used for inventory) */
    void DrawVobSingle( VobInfo* vob, zCCamera& camera ) override;

    /** Draws everything around the given position */
    void ShadowPass_DrawWorldMesh_Indirect(const std::vector<WorldMeshSectionInfo*>& visibleSections);
    void ShadowPass_DrawWorldMesh(const std::vector<WorldMeshSectionInfo*>& visibleSections);

    void XM_CALLCONV DrawWorldAroundForWorldShadow( FXMVECTOR position, float sectionRange, const RenderShadowmapsParams& params );
    void XM_CALLCONV DrawWorldAround( FXMVECTOR position,
        float range,
        bool cullFront = true,
        bool indoor = false,
        bool noNPCs = false,
        std::list<VobInfo*>* renderedVobs = nullptr, std::list<SkeletalVobInfo*>* renderedMobs = nullptr, std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>* worldMeshCache = nullptr );
    void XM_CALLCONV DrawWorldAround_Layered( FXMVECTOR position,
        float range,
        bool cullFront = true,
        bool indoor = false,
        bool noNPCs = false,
        std::list<VobInfo*>* renderedVobs = nullptr, std::list<SkeletalVobInfo*>* renderedMobs = nullptr, std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>* worldMeshCache = nullptr );

    /** Update morph mesh visual */
    void UpdateMorphMeshVisual();

    /** Draws the static vobs instanced */
    XRESULT DrawVOBsInstanced();
    XRESULT DrawFrameAlphaMeshes();

    /** Draws static vobs using atlas indirect path */
    XRESULT DrawVOBsIndirect( const Frustum& frustum, bool bindPS = true );

    /** Builds global VB/IB and indirect args from atlas data (called from OnWorldLoaded) */
    void BuildStaticGeometryBuffers();

    /** Builds GPU data for compute shader culling (called after BuildStaticGeometryBuffers) */
    void BuildGPUCullingBuffers();

    /** Set wind props in const buffer */
    void ApplyWindProps( VS_ExConstantBuffer_Wind& windBuff );

    /** Called when we started to render the world */
    XRESULT OnStartWorldRendering() override;

    /** Draws the sky using the GSky-Object */
    XRESULT DrawSky() override;

    /** Renders the shadowmaps for the sun */
    void XM_CALLCONV RenderShadowmaps( FXMVECTOR cameraPosition, RenderToDepthStencilBuffer* target = nullptr, bool cullFront = true, bool dontCull = false, Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsvOverwrite = nullptr, Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV = nullptr );

    /** Renders the shadowmaps for a pointlight */
    void XM_CALLCONV RenderShadowCube( FXMVECTOR position,
        float range,
        const RenderToDepthStencilBuffer& targetCube,
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> face,
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV,
        bool cullFront = true,
        bool indoor = false,
        bool noNPCs = false,
        std::list<VobInfo*>* renderedVobs = nullptr, std::list<SkeletalVobInfo*>* renderedMobs = nullptr, std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>* worldMeshCache = nullptr );

    /** Updates the occlusion for the bsp-tree */
    void UpdateOcclusion();

    /** Recreates the renderstates */
    XRESULT UpdateRenderStates() override;

    /** Draws a fullscreenquad, copying the given texture to the viewport */
    void DrawQuad( INT2 position, INT2 size ) override;

    /** Sets the current rendering stage */
    void SetRenderingStage( D3D11ENGINE_RENDER_STAGE stage );

    /** Returns the current rendering stage */
    D3D11ENGINE_RENDER_STAGE GetRenderingStage();

    /** Update focus window state */
    void UpdateFocus( HWND hWnd, bool focus_state );

    /** Update clipping cursor onto window */
    void UpdateClipCursor( HWND hWnd );

    /** Message-Callback for the main window */
    LRESULT OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) override;

    static void UpdateShouldBlockGameInput();

    /** Reloads shaders */
    XRESULT ReloadShaders( ShaderCategory categories = ShaderCategory::All) override;

    /** Draws the given mesh infos as water */
    void DrawWaterSurfaces() override;

    /** Handles an UI-Event */
    void OnUIEvent( EUIEvent uiEvent ) override;

    /** Draws the given list of decals */
    void DrawDecalList( const std::vector<zCVob*>& decals, bool lighting );

    /** Draws underwater effects */
    void DrawUnderwaterEffects();

    /** Binds the right shader for the given texture */
    bool BindShaderForTexture( zCTexture* texture, bool forceAlphaTest = false, int zMatAlphaFunc = 0, MaterialInfo::EMaterialType materialInfo = MaterialInfo::MT_None );

    /** Copies the depth stencil buffer to DepthStencilBufferCopy */
    void CopyDepthStencil();

    /** Draws particle meshes */
    void DrawFrameParticleMeshes( std::unordered_map<zCVob*, MeshVisualInfo*>& progMeshes ) override;

    /** Draws particle effects */
    void DrawFrameParticles(std::map<zCTexture*, std::vector<ParticleInstanceInfo>>& particles, std::map<zCTexture*, ParticleRenderInfo>& info, RenderToTextureBuffer
                            * bufferParticleColor, RenderToTextureBuffer* bufferParticleDistortion) override;

    /** Returns the settings window availability */
    bool HasSettingsWindow();

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
    D3D11Texture* GetDistortionTexture() const { return DistortionTexture.get(); }

    RenderToTextureBuffer* GetVelocityBuffer() const { return VelocityBuffer.get(); }

    const XMFLOAT4X4& GetPrevViewProjMatrix() const { return m_PrevViewProjMatrix; }

    auto GetClampSamplerState() -> auto { return ClampSamplerState.Get(); }
    auto GetCubeSamplerState() -> auto { return CubeSamplerState.Get(); }
    auto GetLinearSamplerState() -> auto { return LinearSamplerState.Get(); }

    D3D11ShadowMap* GetShadowMaps() const { return ShadowMaps.get(); }
    void OnWorldLoaded() override;
protected:

    void StoreVobPreviousTransforms();

    void StorePrevViewProjMatrix();

    void BuildSceneTextureAtlasses();

    /** World mesh atlas: collect textures, build atlases, merge geometry */
    void BuildWorldMeshTextureAtlasses();
    void BuildStaticWorldMeshBuffers();

    /** Draw world mesh using atlas indirect path */
    XRESULT DrawWorldMesh_Atlas();

    void CacheWorldStaticVobs();

    std::unique_ptr<FpsLimiter> m_FrameLimiter;
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

    /** Temp-Arrays for storing data to be put in constant buffers */
    XMFLOAT4X4 Temp2D3DXMatrix[2];
    float2 Temp2Float2[2];
    std::unique_ptr<D3D11VertexBuffer> DynamicInstancingBuffer;
    std::unique_ptr<D3D11VertexBuffer> DynamicVertexBuffer;

    /** Post processing */
    std::unique_ptr<D3D11PfxRenderer> PfxRenderer;

    /** Sky */
    std::unique_ptr<D3D11Texture> DistortionTexture;
    std::unique_ptr<D3D11Texture> NoiseTexture;
    std::unique_ptr<D3D11Texture> WhiteTexture;

    /** Shadowing */
    std::vector<VobInfo*> RenderedVobs;

    /** Modulate Quad Marks */
    std::vector<std::pair<zCQuadMark*, const QuadMarkInfo*>> MulQuadMarks;

    /** The current rendering stage */
    D3D11ENGINE_RENDER_STAGE RenderingStage;

    /** List of water surfaces for this frame */
    std::unordered_map<zCTexture*, std::vector<WorldMeshInfo*>> FrameWaterSurfaces;

    /** List of worldmeshes we have to render using alphablending */
    std::vector<std::pair<MeshKey, MeshInfo*>> FrameTransparencyMeshes;

    /** List of portal worldmeshes we have to render using alphablending */
    std::vector<std::pair<MeshKey, MeshInfo*>> FrameTransparencyMeshesPortal;

    /** List of waterfall worldmeshes we have to render using alphablending */
    std::vector<std::pair<MeshKey, MeshInfo*>> FrameTransparencyMeshesWaterfall;

    INT2 m_scaledResolution;

public:
    /** Lighting */
    GMesh* InverseUnitSphereMesh;
    /** Reflection */
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ReflectionCube;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ReflectionCube2;
private:
    std::vector<AlphaMeshData> m_AlphaMeshes;
    std::vector<VobLightInfo*> m_FrameLights;
    
    /** World-Mesh indirect buffer */
    std::unique_ptr<D3D11IndirectBuffer> WorldMeshIndirectBuffer;

    /** Buffer for previous bone transforms */
    std::unique_ptr<D3D11ConstantBuffer> PrevBoneTransformsBuffer;

    /** Cached bone transforms for batched skeletal mesh drawing */
    std::vector<XMFLOAT4X4> BoneTransformCache;

    /** Constantbuffers for view-distances */
    std::unique_ptr<D3D11ConstantBuffer> InfiniteRangeConstantBuffer;
    std::unique_ptr<D3D11ConstantBuffer> OutdoorSmallVobsConstantBuffer;
    std::unique_ptr<D3D11ConstantBuffer> OutdoorVobsConstantBuffer;

    /** Quads for decals/particles */
    D3D11VertexBuffer* QuadVertexBuffer;
    D3D11VertexBuffer* QuadIndexBuffer;

    /** Occlusion query manager */
    std::unique_ptr<D3D11OcclusionQuerry> Occlusion;

    /** Temporary vertex buffers */
    std::unique_ptr<D3D11VertexBuffer> TempPolysVertexBuffer;
    std::unique_ptr<D3D11VertexBuffer> TempParticlesVertexBuffer;
    std::unique_ptr<D3D11VertexBuffer> TempMorphedMeshSmallVertexBuffer;
    std::unique_ptr<D3D11VertexBuffer> TempMorphedMeshBigVertexBuffer;
    std::unique_ptr<D3D11VertexBuffer> TempHUDVertexBuffer;

    /** Cached display modes */
    std::vector<DisplayModeInfo> CachedDisplayModes;
    DXGI_RATIONAL CachedRefreshRate;

    /** Low latency object handle */
    HANDLE frameLatencyWaitableObject;

    /** If true, we will save a screenshot after the next frame */
    bool SaveScreenshotNextFrame;

    bool m_flipWithTearing;
    bool m_swapchainflip;
    bool m_lowlatency;
    bool m_HDR;
    int m_previousFpsLimit;
    bool m_isWindowActive;
    float unionCurrentCustomFontMultiplier;

    std::unique_ptr<RenderToTextureBuffer> VelocityBuffer;
    XMFLOAT4X4 m_PrevViewProjMatrix;
    
    INT2 NewResolution;
    
    void CreateAndBindDefaultSampler();

    std::vector<VobInfo*> m_StaticVobs{};
    std::vector<AABB_SoA_Batch8> m_StaticVobsAABBs{};
    std::array<TextureManager::AtlasResult, TEXTURE_ATLAS_MAX> m_TextureAtlasses{};

    /** Atlas indirect draw path */
    std::unordered_map<zCTexture*, TextureAtlasLookup> m_TextureAtlasLookup;
    std::unique_ptr<D3D11VertexBuffer> m_StaticGlobalVertexBuffer;
    std::unique_ptr<D3D11VertexBuffer> m_StaticGlobalIndexBuffer;
    std::unique_ptr<D3D11VertexBuffer> m_GlobalInstanceIdBuffer;
    std::vector<AtlasDrawGroup> m_AtlasDrawGroups;
    std::unique_ptr<D3D11StructuredBuffer<VobInstanceInfoAtlas>> m_StaticVobInstanceBuffer;

    /** GPU culling buffers (created once at world load) */
    std::unique_ptr<D3D11StructuredBuffer<VobGPUData>> m_VobGPUBuffer;
    std::unique_ptr<D3D11StructuredBuffer<SubmeshGPUData>> m_SubmeshGPUBuffer;
    std::unique_ptr<D3D11StructuredBuffer<VobInstanceInfoAtlas>> m_InstanceBufferGPU;
    std::unique_ptr<D3D11IndirectBuffer> m_MergedIndirectArgs;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_IndirectArgsTemplate;
    std::unique_ptr<D3D11ConstantBuffer> m_CullConstantBuffer;
    std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> m_MergedArgsReset; // CPU-side template for reset
    UINT m_TotalMaxInstances = 0;

    /** World mesh atlas indirect draw path */
    std::unordered_map<zCTexture*, TextureAtlasLookup> m_WorldMeshDiffuseAtlasLookup;
    std::unordered_map<D3D11Texture*, TextureAtlasLookup> m_WorldMeshNormalAtlasLookup;
    std::unordered_map<D3D11Texture*, TextureAtlasLookup> m_WorldMeshFxAtlasLookup;
    std::array<TextureManager::AtlasResult, TEXTURE_ATLAS_MAX> m_WorldMeshDiffuseAtlasses{};
    std::array<TextureManager::AtlasResult, TEXTURE_ATLAS_MAX> m_WorldMeshNormalAtlasses{};
    std::array<TextureManager::AtlasResult, TEXTURE_ATLAS_MAX> m_WorldMeshFxAtlasses{};
    std::unique_ptr<D3D11VertexBuffer> m_WorldMeshGlobalVertexBuffer;
    std::unique_ptr<D3D11VertexBuffer> m_WorldMeshGlobalIndexBuffer;
    std::unique_ptr<D3D11VertexBuffer> m_WorldMeshGlobalInstanceIdBuffer;
    std::unique_ptr<D3D11StructuredBuffer<WorldMeshSubmeshGPUData>> m_WorldMeshSubmeshBuffer;
    std::vector<AtlasDrawGroup> m_WorldMeshAtlasDrawGroups;
    std::unordered_set<MeshInfo*> m_WorldMeshAtlasedSubmeshes;  // submeshes in the atlas (for legacy filter)

    /** Hi-Z occlusion culling resources */
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_HiZTexture;       // Full mip-chain, SRV-only
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_HiZSRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_HiZScratch;       // Single-mip scratch for CS UAV writes
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_HiZScratchUAV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_HiZScratchSRV;
    UINT m_HiZMipCount = 0;

    /** Create Hi-Z pyramid resources (called after depth buffer creation) */
    void CreateHiZResources();
    /** Build the Hi-Z mip chain from the current depth buffer */
    void BuildHiZPyramid();
};
