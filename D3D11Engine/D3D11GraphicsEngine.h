#pragma once
#include "D3D11GraphicsEngineBase.h"
#include "fpslimiter.h"
#include "GothicAPI.h"

struct RenderToDepthStencilBuffer;

class D3D11ConstantBuffer;
class D3D11VertexBuffer;
class D3D11ShaderManager;

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

const int POINTLIGHT_SHADOWMAP_SIZE = 64;

class D3D11PointLight;
class D3D11VShader;
class D3D11PShader;
class D3D11PfxRenderer;
class D3D11LineRenderer;
class zCVobLight;
class zCVob;
class D2DView;
struct VobLightInfo;
class GMesh;
class GOcean;
class D3D11HDShader;
class D3D11OcclusionQuerry;
struct MeshInfo;
struct RenderToTextureBuffer;
class D3D11Effect;

class D3D11GraphicsEngine : public D3D11GraphicsEngineBase {
public:
    D3D11GraphicsEngine();
    ~D3D11GraphicsEngine();

    /** Called after the fake-DDraw-Device got created */
    virtual XRESULT Init() override;

    /** Called when the game created its window */
    virtual XRESULT SetWindow( HWND hWnd ) override;

    /** Reset BackBuffer */
    void OnResetBackBuffer();

    /** Get BackBuffer Format */
    DXGI_FORMAT GetBackBufferFormat();

    /** Get Window Mode */
    int GetWindowMode();

    /** Called on window resize/resolution change */
    virtual XRESULT OnResize( INT2 newSize ) override;

    /** Called when the game wants to render a new frame */
    virtual XRESULT OnBeginFrame() override;

    /** Called when the game ended it's frame */
    virtual XRESULT OnEndFrame() override;

    /** Called to set the current viewport */
    virtual XRESULT SetViewport( const ViewportInfo& viewportInfo ) override;

    /** Called when the game wants to clear the bound rendertarget */
    virtual XRESULT Clear( const float4& color );

    /** Creates a vertexbuffer object (Not registered inside) */
    virtual XRESULT CreateVertexBuffer( D3D11VertexBuffer** outBuffer );

    /** Creates a texture object (Not registered inside) */
    virtual XRESULT CreateTexture( D3D11Texture** outTexture );

    /** Creates a constantbuffer object (Not registered inside) */
    virtual XRESULT CreateConstantBuffer( D3D11ConstantBuffer** outCB, void* data, int size );

    /** Fetches a list of available display modes */
    XRESULT FetchDisplayModeList();
    XRESULT FetchDisplayModeListDXGI();
    XRESULT FetchDisplayModeListWindows();

    /** Returns a list of available display modes */
    virtual XRESULT GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling = false );

    /** Presents the current frame to the screen */
    virtual XRESULT Present() override;

    /** Saves a screenshot */
    virtual void SaveScreenshot() override;

    virtual void DrawString( const std::string& str, float x, float y, const zFont* font, zColor& fontColor ) override;

    //virtual int MeasureString(std::string str, zFont* zFont) override;

    /** Draws a vertexbuffer, non-indexed */
    virtual XRESULT DrawVertexBuffer( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Draws a vertexbuffer, non-indexed */
    virtual XRESULT DrawVertexBufferIndexed( D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset = 0 ) override;
    virtual XRESULT DrawVertexBufferIndexedUINT( D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset ) override;

    /** Draws a vertexbuffer, non-indexed, binding the FF-Pipe values */
    virtual XRESULT DrawVertexBufferFF( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Binds viewport information to the given constantbuffer slot */
    virtual XRESULT BindViewportInformation( const std::string& shader, int slot ) override;

    /** Sets up a draw call for a VS_Ex-Mesh */
    void SetupVS_ExMeshDrawCall();
    void SetupVS_ExConstantBuffer();
    void SetupVS_ExPerInstanceConstantBuffer();

    /** Puts the current world matrix into a CB and binds it to the given slot */
    void SetupPerInstanceConstantBuffer( int slot = 1 );

    /**Colorspace for HDR-Monitors on Windows 10 */
    /** HDR Support */
    //DXGI_COLOR_SPACE_TYPE   m_colorSpace; //only used when access from other function required
    //DXGI_COLOR_SPACE_TYPE   GetColorSpace() const noexcept { return m_colorSpace; } //only used when access from other function required

    void UpdateColorSpace_SwapChain3();
    void UpdateColorSpace_SwapChain4();

#if ENABLE_TESSELATION > 0
    enum EPNAENRenderMode {
        PNAEN_Default,
        PNAEN_Instanced,
        PNAEN_Skeletal,
    };

    /** Sets up everything for a PNAEN-Mesh */
    void Setup_PNAEN( EPNAENRenderMode mode = PNAEN_Default );
#endif

    /** Sets up texture with normalmap and fxmap for rendering */
    bool BindTextureNRFX( zCTexture* tex, bool bindShader );

    /** Draws a skeletal mesh */
    XRESULT DrawSkeletalVertexNormals( SkeletalVobInfo* vi, const std::vector<XMFLOAT4X4>& transforms, float4 color, float fatness = 1.0f );
    virtual XRESULT DrawSkeletalMesh( SkeletalVobInfo* vi, const std::vector<XMFLOAT4X4>& transforms, float4 color, float fatness = 1.0f ) override;

    /** Draws a screen fade effects */
    virtual XRESULT DrawScreenFade( void* camera ) override;

    /** Draws a vertexarray, non-indexed */
    virtual XRESULT DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex = 0, unsigned int stride = sizeof( ExVertexStruct ) ) override;
    virtual XRESULT DrawVertexArrayMM( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex = 0, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Draws a vertexarray, indexed */
    virtual XRESULT DrawIndexedVertexArray( ExVertexStruct* vertices, unsigned int numVertices, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Draws a batch of instanced geometry */
    virtual XRESULT DrawInstanced( D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices, void* instanceData, unsigned int instanceDataStride, unsigned int numInstances, unsigned int vertexStride = sizeof( ExVertexStruct ) );
    virtual XRESULT DrawInstanced( D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices, D3D11VertexBuffer* instanceData, unsigned int instanceDataStride, unsigned int numInstances, unsigned int vertexStride = sizeof( ExVertexStruct ), unsigned int startInstanceNum = 0, unsigned int indexOffset = 0 );

    /** Called when a vob was removed from the world */
    virtual XRESULT OnVobRemovedFromWorld( zCVob* vob );

    /** Called when a key got pressed */
    virtual XRESULT OnKeyDown( unsigned int key ) override;

    /** Sets the active pixel shader object */
    virtual XRESULT SetActivePixelShader( const std::string& shader );
    virtual XRESULT SetActiveVertexShader( const std::string& shader );
    XRESULT SetActiveHDShader( const std::string& shader );

    /** Binds the active PixelShader */
    virtual XRESULT BindActivePixelShader();
    virtual XRESULT BindActiveVertexShader();

    /** Draws quadmarks in a simple way */
    void DrawQuadMarks();
    void DrawMQuadMarks();

    /** Draws the ocean */
    XRESULT DrawOcean( GOcean* ocean );

    /** Gets the depthbuffer */
    RenderToDepthStencilBuffer* GetDepthBuffer() { return DepthStencilBuffer.get(); }

    /** Returns the first GBuffer */
    RenderToTextureBuffer& GetGBuffer0() { return *GBuffer0_Diffuse; }

    /** Returns the second GBuffer */
    RenderToTextureBuffer& GetGBuffer1() { return *GBuffer1_Normals; }

    /** Returns the HDRBackbuffer */
    RenderToTextureBuffer& GetHDRBackBuffer() { return *HDRBackBuffer; }

    /** Unbinds the texture at the given slot */
    virtual XRESULT UnbindTexture( int slot );

    /** Sets up the default rendering state */
    void SetDefaultStates( bool force = false );

    /** Returns the current resolution (Maybe supersampled)*/
    INT2 GetResolution();

    /** Returns the actual resolution of the backbuffer (not supersampled) */
    INT2 GetBackbufferResolution();

    /** Returns the data of the backbuffer */
    void GetBackbufferData( byte** data, int& pixelsize );

    /** Returns the line renderer object */
    BaseLineRenderer* GetLineRenderer();

    /** ---------------- Gothic rendering functions -------------------- */

    /** Draws the world mesh */
    virtual XRESULT DrawWorldMesh( bool noTextures = false );

    /** Draws a list of mesh infos */
    XRESULT DrawMeshInfoListAlphablended( const std::vector<std::pair<MeshKey, MeshInfo*>>& list );

    XRESULT DrawWorldMeshW( bool noTextures = false );

    /** Draws the static VOBs */
    virtual XRESULT DrawVOBs( bool noTextures = false );

    /** Draws PolyStrips (weapon and particle trails) */
    XRESULT DrawPolyStrips( bool noTextures = false );

    /** Draws a VOB (used for inventory) */
    virtual void DrawVobSingle( VobInfo* vob, zCCamera& camera ) override;

    /** Draws everything around the given position */
    void XM_CALLCONV DrawWorldAround( FXMVECTOR position, int sectionRange, float vobXZRange, bool cullFront = true, bool dontCull = false );
    void XM_CALLCONV DrawWorldAround( FXMVECTOR position,
        float range,
        bool cullFront = true,
        bool indoor = false,
        bool noNPCs = false,
        std::list<VobInfo*>* renderedVobs = nullptr, std::list<SkeletalVobInfo*>* renderedMobs = nullptr, std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>* worldMeshCache = nullptr );

    /** Update morph mesh visual */
    void UpdateMorphMeshVisual();

    /** Draws the static vobs instanced */
    XRESULT DrawVOBsInstanced();

    /** Applys the lighting to the scene */
    XRESULT DrawLighting( std::vector<VobLightInfo*>& lights );

    /** Called when we started to render the world */
    virtual XRESULT OnStartWorldRendering();

    /** Draws the sky using the GSky-Object */
    virtual XRESULT DrawSky();

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
    void DrawQuad( INT2 position, INT2 size );

    /** Sets the current rendering stage */
    void SetRenderingStage( D3D11ENGINE_RENDER_STAGE stage );

    /** Returns the current rendering stage */
    D3D11ENGINE_RENDER_STAGE GetRenderingStage();

    /** Update clipping cursor onto window */
    void UpdateClipCursor( HWND hWnd );

    /** Message-Callback for the main window */
    virtual LRESULT OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

    /** Reloads shaders */
    virtual XRESULT ReloadShaders();

    /** Draws the given mesh infos as water */
    void DrawWaterSurfaces();

    /** Handles an UI-Event */
    void OnUIEvent( EUIEvent uiEvent );

    /** Draws the given list of decals */
    void DrawDecalList( const std::vector<zCVob*>& decals, bool lighting );

    /** Draws underwater effects */
    void DrawUnderwaterEffects();

    /** Binds the right shader for the given texture */
    void BindShaderForTexture( zCTexture* texture, bool forceAlphaTest = false, int zMatAlphaFunc = 0, MaterialInfo::EMaterialType materialInfo = MaterialInfo::MT_None );

    /** Copies the depth stencil buffer to DepthStencilBufferCopy */
    void CopyDepthStencil();

    /** Draws particle meshes */
    void DrawFrameParticleMeshes( std::unordered_map<zCVob*, MeshVisualInfo*>& progMeshes );

    /** Draws particle effects */
    void DrawFrameParticles( std::map<zCTexture*, std::vector<ParticleInstanceInfo>>& particles, std::map<zCTexture*, ParticleRenderInfo>& info );

    /** Returns the UI-View */
    D2DView* GetUIView() { return UIView.get(); }

    /** Returns the settings window availability */
    bool HasSettingsWindow();

    /** Creates the main UI-View */
    void CreateMainUIView();

    /** Returns a dummy cube-rendertarget used for pointlight shadowmaps */
    RenderToTextureBuffer* GetDummyCubeRT() { return DummyShadowCubemapTexture.get(); }

    void EnsureTempVertexBufferSize( std::unique_ptr<D3D11VertexBuffer>& buffer, UINT size );
protected:
    std::unique_ptr<FpsLimiter> m_FrameLimiter;
    int m_LastFrameLimit;
    /** Test draw world */
    void TestDrawWorldMesh();

    D3D11PointLight* DebugPointlight;

    // Using a list here to determine which lights to update, since we don't want to update every light every frame.
    std::list<VobLightInfo*> FrameShadowUpdateLights;

    /** D3D11 Objects */
    Microsoft::WRL::ComPtr<ID3D11SamplerState> ClampSamplerState;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> CubeSamplerState;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> ShadowmapSamplerState;

    /** Effects wrapper */
    std::unique_ptr<D3D11Effect> Effects;

    /** Swapchain buffers */
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> BackbufferRTV;
    std::unique_ptr<RenderToTextureBuffer> GBuffer0_Diffuse;
    std::unique_ptr<RenderToTextureBuffer> GBuffer1_Normals; // Normals
    std::unique_ptr<RenderToTextureBuffer> GBuffer2_SpecIntens_SpecPower; // SpecIntensity / SpecPower
    std::unique_ptr<RenderToTextureBuffer> DepthStencilBufferCopy;
    std::unique_ptr<RenderToTextureBuffer> DummyShadowCubemapTexture; // PS-Stage needs to have a rendertarget bound to execute SV_Depth-Writes, as it seems.

    /** Temp-Arrays for storing data to be put in constant buffers */
    XMFLOAT4X4 Temp2D3DXMatrix[2];
    float2 Temp2Float2[2];
    std::unique_ptr<D3D11VertexBuffer> DynamicInstancingBuffer;

    /** Post processing */
    std::unique_ptr<D3D11PfxRenderer> PfxRenderer;

    /** Sky */
    std::unique_ptr<D3D11Texture> DistortionTexture;
    std::unique_ptr<D3D11Texture> NoiseTexture;
    std::unique_ptr<D3D11Texture> WhiteTexture;

    /** Lighting */
    GMesh* InverseUnitSphereMesh;

    /** Shadowing */
    std::unique_ptr<RenderToDepthStencilBuffer> WorldShadowmap1;
    std::vector<VobInfo*> RenderedVobs;

    /** Modulate Quad Marks */
    std::vector<std::pair<zCQuadMark*, const QuadMarkInfo*>> MulQuadMarks;

    /** The current rendering stage */
    D3D11ENGINE_RENDER_STAGE RenderingStage;

    /** The editorcontrols */
    std::unique_ptr<D2DView> UIView;

    /** List of water surfaces for this frame */
    std::unordered_map<zCTexture*, std::vector<WorldMeshInfo*>> FrameWaterSurfaces;

    /** List of worldmeshes we have to render using alphablending */
    std::vector<std::pair<MeshKey, MeshInfo*>> FrameTransparencyMeshes;

    /** List of portal worldmeshes we have to render using alphablending */
    std::vector<std::pair<MeshKey, MeshInfo*>> FrameTransparencyMeshesPortal;

    /** List of waterfall worldmeshes we have to render using alphablending */
    std::vector<std::pair<MeshKey, MeshInfo*>> FrameTransparencyMeshesWaterfall;

    /** Reflection */
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ReflectionCube;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ReflectionCube2;

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
};
