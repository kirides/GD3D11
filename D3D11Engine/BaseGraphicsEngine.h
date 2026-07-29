#pragma once
#include "Engine.h"
#include "WorldObjects.h"
#include "GraphicsEventRecord.h"
#include "ShaderCategory.h"
#include "ShaderIDs.h"
#include "GfxVertexBuffer.h"
#include "GfxTexture.h"

class BaseLineRenderer;
class BaseShadowedPointLight;
class zCTexture;
class zCMaterial;
class zCVob;
struct SkeletalMeshVisualInfo;
struct VobInfo;
struct VobLightInfo;
class zFont;

static void UpdateShouldBlockGameInput();

struct GraphicsEventName {
    const wchar_t* wide;
    const char* narrow;
};

#define GE_NAME(nameLiteral) GraphicsEventName{ L##nameLiteral, nameLiteral }

enum WindowModes {
    WINDOW_MODE_FULLSCREEN_EXCLUSIVE = 1,
    WINDOW_MODE_FULLSCREEN_BORDERLESS = 2,
    WINDOW_MODE_FULLSCREEN_LOWLATENCY = 3,
    WINDOW_MODE_WINDOWED = 4,
};

/** Identifies which concrete graphics backend Engine::GraphicsEngine points at.
    Used to gate the (few) external downcasts to a concrete backend type so a
    blind reinterpret_cast can be replaced by a checked static_cast. */
enum class EGraphicsEngineBackend {
    D3D11,
    D3D12,
};

/** Backend-neutral rendering stage. Scene code (GothicAPI) queries the active engine
    for this to branch on Z-prepass / main / shadow passes. Kept as an unscoped enum with
    the historical DES_* enumerators so existing call sites are unchanged; the D3D11 backend
    aliases its old D3D11ENGINE_RENDER_STAGE name to this. */
enum ERenderStage {
    DES_Z_PRE_PASS,
    DES_MAIN,
    DES_SHADOWMAP,
    DES_SHADOWMAP_CUBE,
    DES_GHOST
};

struct ViewportInfo {
    ViewportInfo()
    : ViewportInfo(0, 0, 0, 0)
    {}

    ViewportInfo( unsigned int topleftX,
        unsigned int topleftY,
        unsigned int width,
        unsigned int height,
        float minZ = 0.0f,
        float maxZ = 1.0f ) : 
        TopLeftX(topleftX),
        TopLeftY(topleftY),
        Width(width),
        Height(height),
        MinZ(minZ),
        MaxZ(maxZ)
    { }

    ViewportInfo( unsigned int topleftX,
        unsigned int topleftY,
        INT2 wh,
        float minZ = 0.0f,
        float maxZ = 1.0f )
    : ViewportInfo(topleftX, topleftY, wh.x, wh.y, minZ, maxZ)
    { }

    unsigned int TopLeftX;
    unsigned int TopLeftY;
    unsigned int Width;
    unsigned int Height;
    float MinZ;
    float MaxZ;
};

/** Base graphics engine */
class BaseGraphicsEngine {
public:
    enum EUIEvent {
        UI_OpenSettings,
        UI_OpenEditor,
        UI_ClosedSettings,
        UI_ToggleAdvancedSettings,
    };

    BaseGraphicsEngine() = default;
    virtual ~BaseGraphicsEngine() = default;
    
    /** Returns which concrete backend this engine is. Lets external code replace
        blind reinterpret_casts of Engine::GraphicsEngine with a checked downcast. */
    virtual EGraphicsEngineBackend GetBackendAPI() const PURE;

    /* Trigger resize on next frame */
    virtual XRESULT TriggerResize(INT2 resolution) PURE;

    /** Called after the fake-DDraw-Device got created */
    virtual XRESULT Init() PURE;

    /** Called when the game created its window */
    virtual XRESULT SetWindow( HWND hWnd ) PURE;

    /** Called on window resize/resolution change */
    virtual XRESULT OnResize( INT2 newSize ) PURE;

    /** Called when the game wants to render a new frame */
    virtual XRESULT OnBeginFrame() PURE;

    /** Called when the game ended it's frame */
    virtual XRESULT OnEndFrame() PURE;

    /** Called to set the current viewport */
    virtual XRESULT SetViewport( const ViewportInfo& viewportInfo ) PURE;

    /** Called when the game wants to clear the bound rendertarget */
    virtual XRESULT Clear( const float4& color ) PURE;

    /** Creates a vertexbuffer object (Not registered inside) */
    virtual XRESULT CreateVertexBuffer( std::unique_ptr<GfxVertexBuffer>& outBuffer ) PURE;

    /** Creates a texture object (Not registered inside) */
    virtual XRESULT CreateTexture( GfxTexture** outTexture ) PURE;
    virtual XRESULT CreateTexture( std::unique_ptr<GfxTexture>& outTexture ) PURE;
    
    /** Creates a bufferobject for a shadowed point light */
    virtual XRESULT CreateShadowedPointLight( BaseShadowedPointLight** outPL, VobLightInfo* lightInfo, bool dynamic = false ) { 
        *outPL = nullptr;
        return XR_SUCCESS;
    }

    /** Returns a list of available display modes */
    virtual XRESULT GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling = false ) PURE;

    /** Presents the current frame to the screen */
    virtual XRESULT Present() PURE;

    /** Called when we started to render the world */
    virtual XRESULT OnStartWorldRendering() PURE;

    /** Returns the line renderer object */
    virtual BaseLineRenderer* GetLineRenderer() PURE;

    /** Returns the graphics-device this is running on */
    virtual const std::string& GetGraphicsDeviceName() PURE;

    /** Draws a screen fade effects */
    virtual XRESULT DrawScreenFade( void* camera ) { return XR_SUCCESS; }
    virtual void OnAddVob(VobInfo* vi) { }

    /** Draws a vertexarray, used for rendering gothics UI */
    virtual XRESULT DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex PURE, unsigned int stride = sizeof( ExVertexStruct ) ) PURE;

    /** Draws a vertexbuffer, non-indexed */
    virtual XRESULT DrawVertexBuffer( GfxVertexBuffer* vb, unsigned int numVertices, unsigned int stride = sizeof( ExVertexStruct ) ) { return XR_SUCCESS; }
    virtual void OnLoadWorld() {}

    /** Draws a vertexbuffer, non-indexed, binding the FF-Pipe values */
    virtual XRESULT DrawVertexBufferFF( GfxVertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride = sizeof( ExVertexStruct ) ) { return XR_SUCCESS; };

    /** Draws a vertexbuffer, non-indexed */
    virtual XRESULT DrawVertexBufferIndexed( GfxVertexBuffer* vb, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset = 0 ) { return XR_SUCCESS; };
    virtual XRESULT DrawVertexBufferIndexedUINT( GfxVertexBuffer* vb, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset ) { return XR_SUCCESS; };

    virtual XRESULT DrawDynamicVertexBufferIndexed( std::vector<ExVertexStruct>& vertices, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset = 0 ) { return XR_SUCCESS; };

    /** Draws a skeletal mesh */
    virtual XRESULT DrawSkeletalMesh( SkeletalVobInfo* vi, const std::span<XMFLOAT4X4> transforms, float4 color, const XMFLOAT4X4& world, float fatness = 1.0f ) { return XR_SUCCESS; };

    /** Draws a vertexarray, non-indexed */
    virtual XRESULT DrawIndexedVertexArray( ExVertexStruct* vertices, unsigned int numVertices, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int stride = sizeof( ExVertexStruct ) ) { return XR_SUCCESS; };

    /** Draws a batch of instanced geometry */
    virtual XRESULT DrawInstanced( GfxVertexBuffer* vb, GfxVertexBuffer* ib, unsigned int numIndices, GfxVertexBuffer* instanceData, unsigned int instanceDataStride, unsigned int numInstances, unsigned int vertexStride = sizeof( ExVertexStruct ), unsigned int startInstanceNum = 0, unsigned int indexOffset = 0, unsigned int instanceDataByteOffset = 0 ) { return XR_SUCCESS; };

    /** Packed-stride VOB draws (36-byte ExVertexStructGPU; paired with VS_ExPacked). */
    virtual XRESULT DrawVertexBufferIndexedPacked( GfxVertexBuffer* vb, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset = 0 ) { return XR_SUCCESS; };
    virtual XRESULT DrawVertexBufferInstanced( GfxVertexBuffer* vb, unsigned int numVertices, unsigned int numInstances, unsigned int stride = sizeof( ExVertexStruct ) ) { return XR_SUCCESS; };
    virtual XRESULT DrawVertexBufferInstancedIndexed( GfxVertexBuffer* vb, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int numInstances, unsigned int indexOffset = 0 ) { return XR_SUCCESS; };
    virtual XRESULT DrawVertexBufferInstancedIndexedPacked( GfxVertexBuffer* vb, GfxVertexBuffer* ib, unsigned int numIndices, unsigned int numInstances, unsigned int indexOffset = 0 ) { return XR_SUCCESS; };

    /** Sets up a draw call for a VS_Ex-Mesh (FF-pipe emulation state). Default no-op. */
    virtual void SetupVS_ExMeshDrawCall() {}
    virtual void SetupVS_ExConstantBuffer() {}
    virtual void SetupVS_ExPerInstanceConstantBuffer() {}

    /** Returns the current rendering stage (Z-prepass / main / shadow / ghost). */
    virtual ERenderStage GetRenderingStage() { return DES_MAIN; }

    /** Sets up a texture with normalmap and fxmap for rendering. Returns true if bound. */
    virtual bool BindTextureNRFX( zCMaterial* mat, zCTexture* tex, bool bindShader, bool updateMaterialInfo = true ) { return true; }
    virtual bool BindTextureNRFX( zCMaterial* mat, bool bindShader, bool updateMaterialInfo = true ) { return true; }

    /** Skeletal-mesh draws. */
    virtual XRESULT DrawSkeletalVertexNormals( SkeletalVobInfo* vi, const XMFLOAT4X4& world, const std::span<XMFLOAT4X4> transforms, float4 color, float fatness = 1.0f ) { return XR_SUCCESS; };
    virtual XRESULT DrawSkeletalMesh_Layered( SkeletalVobInfo* vi, const std::span<XMFLOAT4X4> transforms, float4 color, XMFLOAT4X4& world, float fatness = 1.0f ) { return XR_SUCCESS; };
    virtual void DrawSkeletalMeshVobs( const std::vector<SkeletalVobInfo*>& vis, float distance, bool updateState, bool drawAttachments ) {};

    /** Unbinds the active pixel shader object. */
    virtual void UnbindActivePS() {}

    /** Called when the backbuffer is (re)created after a resize/reset. */
    virtual void OnResetBackBuffer() {}

    /** Clears the "waiting for a present" guard, so the next OnStartWorldRendering actually draws
        the world instead of early-outing. Needed by the backbuffer-readback path (savegame
        thumbnails/screenshots), which renders a frame on demand outside the normal present cycle. */
    virtual void ResetPresentPending() {}

    /** Sets the active pixel shader object */
    virtual XRESULT SetActivePixelShader( PShaderID shader ) { return XR_SUCCESS; };
    virtual XRESULT SetActiveVertexShader( VShaderID shader ) { return XR_SUCCESS; };

    /** Binds the active PixelShader */
    virtual XRESULT BindActivePixelShader() { return XR_SUCCESS; };
    virtual XRESULT BindActiveVertexShader() { return XR_SUCCESS; };

    /** Binds viewport information to the given constantbuffer slot */
    virtual XRESULT BindViewportInformation( VShaderID vshader, int slot ) { return XR_SUCCESS; };

    /** Unbinds the texture at the given slot */
    virtual XRESULT UnbindTexture( int slot ) { return XR_SUCCESS; };

    /** Binds a surface's diffuse (at 'slot') and normalmap (at 'slot'+1) textures to the pixel
        shader. numTextures selects how many consecutive slots are written: 2 for a ready surface,
        1 to clear just the diffuse slot for a not-yet-loaded surface. Either texture may be null.
        This is the hot texture-bind seam used by the DDraw surface wrapper; the backend extracts
        the native views internally so the interface stays API-neutral. */
    virtual void BindSurfaceTextures( int slot, GfxTexture* diffuse, GfxTexture* normalmap, unsigned int numTextures = 2 ) {}

    /** Draws the world mesh */
    virtual XRESULT DrawWorldMesh( bool noTextures = false ) { return XR_SUCCESS; };

    /** Draws the static VOBs */
    virtual XRESULT DrawVOBs( bool noTextures = false ) { return XR_SUCCESS; }

    /** Draws PolyStrips (weapon and particle trails) */
    virtual XRESULT DrawPolyStrips( bool noTextures = false ) { return XR_SUCCESS; };

    /** Draws the sky using the GSky-Object */
    virtual XRESULT DrawSky() { return XR_SUCCESS; }

    /** Called when a key got pressed */
    virtual XRESULT OnKeyDown( unsigned int key ) { return XR_SUCCESS; }

    /** Returns the current resolution */
    virtual INT2 GetResolution() { return INT2( 0, 0 ); }
    virtual INT2 GetBackbufferResolution() { return GetResolution(); }

    /** Returns the data of the backbuffer */
    virtual void GetBackbufferData( bool thumbnail, byte** data, INT2& buffersize, int& pixelsize ) {}

    /** Draws a fullscreenquad, copying the given texture to the viewport */
    virtual void DrawQuad( INT2 position, INT2 size ) {}

    /** Draws a VOB (used for inventory) */
    virtual void DrawVobSingle( VobInfo* vob, zCCamera& camera ) {};

    /** Called when a vob was removed from the world */
    virtual XRESULT OnVobRemovedFromWorld( zCVob* vob ) { return XR_SUCCESS; }

    /** Reloads shaders */
    virtual XRESULT ReloadShaders( ShaderCategory categories = ShaderCategory::All ) { return XR_SUCCESS; }

    /** Draws the water surfaces */
    virtual void DrawWaterSurfaces() {}

    /** Handles an UI-Event */
    virtual void OnUIEvent( EUIEvent uiEvent );

    /** Draws particle meshes */
    virtual void DrawFrameParticleMeshes( std::unordered_map<zCVob*, std::unique_ptr<MeshVisualInfo>>& progMeshes ) {}

    virtual void DrawString( std::string_view str, float x, float y, const zFont* font, zColor& fontColor ) {};

    virtual XRESULT UpdateRenderStates() { return XR_SUCCESS; };

    virtual std::unique_ptr<GraphicsEventRecord> RecordGraphicsEvent( GraphicsEventName region ) { return std::make_unique<GraphicsEventRecord>(); }

    // --- Window management shared by every backend (owns the OS window handle + common OS-level
    //     policy: startup activation, style/mode switching, focus/cursor-clip, display-mode caching). ---

    /** The game's main window, once known (null until SetWindow() has been called). */
    HWND GetOutputWindow() const { return m_OutputWindow; }

    /** Whether the game window currently has OS focus. */
    bool IsWindowActive() const { return m_IsWindowActive; }

    /** Whether the mod's own ImGui settings/editor window is currently open — used to avoid
        clipping the cursor to the game window while the player is meant to be interacting
        with the settings UI on another part of the screen. */
    bool HasSettingsWindow() const;

    /** Common SetWindow() bring-up, called once per process by each backend's own SetWindow()
        override (before that override sizes and calls OnResize() itself — sizing sources differ
        too much between backends to fold in here). Stores hWnd, force-activates the window
        (foreground/focus dance + closes a stray "Union Splash" window if present), and — outside
        BUILD_SPACER_NET — clips the cursor to the window and force-hides it. No-op if the output
        window is already known or hWnd is null. */
    void CommonSetWindow( HWND hWnd );

    /** Applies WS_ / WS_EX_ style bits for windowed vs. borderless/fullscreen-ish modes against
        the output window, then SetWindowPos to windowRect. Binary on
        windowMode == WINDOW_MODE_WINDOWED vs. anything else (matches both backends' existing
        style logic); callers compute windowRect themselves since the two backends size it
        slightly differently today. */
    void ApplyWindowStyle( WindowModes windowMode, RECT windowRect, UINT swpFlags = SWP_SHOWWINDOW | SWP_FRAMECHANGED );

    /** Focus tracking + cursor-clip. UpdateFocus is idempotent — it re-checks GetForegroundWindow()
        itself and only acts on a genuine transition — and calls UpdateClipCursor() when the state
        actually flips. UpdateClipCursor claims ClipCursor() to the window's client rect while the
        window is active and no settings window is open, and releases it otherwise (only releasing
        a clip this engine itself claimed, tracked via a remembered rect). */
    void UpdateFocus( bool focusState );
    void UpdateClipCursor();

    /** Default window-message handling: routes WM_NCACTIVATE/WM_ACTIVATE/WM_SETFOCUS/
        WM_KILLFOCUS/WM_ENTERIDLE to UpdateFocus() and WM_WINDOWPOSCHANGED to UpdateClipCursor().
        Backends needing extra messages should call BaseGraphicsEngine::OnWindowMessage(...) from
        their override rather than reimplementing focus/clip handling. */
    virtual LRESULT OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

    /** Copies the cached display-mode list into *modeList, optionally appending supersample
        resolutions (integer multiples of the highest cached mode, up to just under 8192) when
        includeSuperSampling is set. Backends populate m_CachedDisplayModes with their own
        enumeration (the concrete DXGI/Windows APIs used differ enough to stay backend-specific),
        then call this from their GetDisplayModeList() override. */
    XRESULT AppendCachedDisplayModes( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling ) const;

protected:
    HWND m_OutputWindow = nullptr;
    bool m_IsWindowActive = false;
    INT2 m_NewResolution = {};
    std::vector<DisplayModeInfo> m_CachedDisplayModes;
};
