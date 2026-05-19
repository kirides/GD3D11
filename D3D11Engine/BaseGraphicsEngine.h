#pragma once
#include "WorldObjects.h"
#include "GraphicsEventRecord.h"
#include "ShaderCategory.h"
#include "ShaderIDs.h"

class BaseLineRenderer;
class BaseShadowedPointLight;
class D3D11ConstantBuffer;
class D3D11Texture;
class D3D11VertexBuffer;
struct RenderToTextureBuffer;
class zCTexture;
class zCVob;
struct SkeletalMeshVisualInfo;
struct VobInfo;
struct VobLightInfo;
class zFont;

struct GraphicsEventName {
    const wchar_t* wide;
    const char* narrow;
};

#define GE_NAME(nameLiteral) GraphicsEventName{ L##nameLiteral, nameLiteral }

struct DisplayModeInfo {
    DisplayModeInfo(): DisplayModeInfo(0,0) {}
    DisplayModeInfo( int w, int h ) : Width(static_cast<DWORD>(w)), Height(static_cast<DWORD>(h)) {}

    DWORD Height;
    DWORD Width;
};

enum WindowModes {
    WINDOW_MODE_FULLSCREEN_EXCLUSIVE = 1,
    WINDOW_MODE_FULLSCREEN_BORDERLESS = 2,
    WINDOW_MODE_FULLSCREEN_LOWLATENCY = 3,
    WINDOW_MODE_WINDOWED = 4,
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
    virtual XRESULT CreateVertexBuffer( D3D11VertexBuffer** outBuffer ) PURE;

    /** Creates a texture object (Not registered inside) */
    virtual XRESULT CreateTexture( D3D11Texture** outTexture ) PURE;

    /** Creates a constantbuffer object (Not registered inside) */
    virtual XRESULT CreateConstantBuffer( D3D11ConstantBuffer** outCB, void* data, int size ) PURE;

    /** Creates a bufferobject for a shadowed point light */
    virtual XRESULT CreateShadowedPointLight( BaseShadowedPointLight** outPL, VobLightInfo* lightInfo, bool dynamic = false ) { return XR_SUCCESS; }

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
    virtual XRESULT DrawScreenFade( void* camera ) { return XR_SUCCESS; };

    /** Draws a vertexarray, used for rendering gothics UI */
    virtual XRESULT DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex PURE, unsigned int stride = sizeof( ExVertexStruct ) ) PURE;

    /** Draws a vertexbuffer, non-indexed */
    virtual XRESULT DrawVertexBuffer( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int stride = sizeof( ExVertexStruct ) ) { return XR_SUCCESS; };

    /** Draws a vertexbuffer, non-indexed, binding the FF-Pipe values */
    virtual XRESULT DrawVertexBufferFF( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride = sizeof( ExVertexStruct ) ) { return XR_SUCCESS; };

    /** Draws a vertexbuffer, non-indexed */
    virtual XRESULT DrawVertexBufferIndexed( D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset = 0 ) { return XR_SUCCESS; };
    virtual XRESULT DrawVertexBufferIndexedUINT( D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset ) { return XR_SUCCESS; };
    
    virtual XRESULT DrawDynamicVertexBufferIndexed( std::vector<ExVertexStruct>& vertices, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset = 0 ) { return XR_SUCCESS; };

    /** Draws a skeletal mesh */
    virtual XRESULT DrawSkeletalMesh( SkeletalVobInfo* vi, const std::span<XMFLOAT4X4> transforms, float4 color, const XMFLOAT4X4& world, float fatness = 1.0f ) { return XR_SUCCESS; };

    /** Draws a vertexarray, non-indexed */
    virtual XRESULT DrawIndexedVertexArray( ExVertexStruct* vertices, unsigned int numVertices, D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int stride = sizeof( ExVertexStruct ) ) { return XR_SUCCESS; };

    /** Draws a batch of instanced geometry */
    virtual XRESULT DrawInstanced( D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices, D3D11VertexBuffer* instanceData, unsigned int instanceDataStride, unsigned int numInstances, unsigned int vertexStride = sizeof( ExVertexStruct ), unsigned int startInstanceNum = 0, unsigned int indexOffset = 0 ) { return XR_SUCCESS; };

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

    /** Message-Callback for the main window */
    virtual LRESULT OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) { return 0; }

    /** Called when a vob was removed from the world */
    virtual XRESULT OnVobRemovedFromWorld( zCVob* vob ) { return XR_SUCCESS; }

    /** Reloads shaders */
    virtual XRESULT ReloadShaders( ShaderCategory categories = ShaderCategory::All ) { return XR_SUCCESS; }

    /** Draws the water surfaces */
    virtual void DrawWaterSurfaces() {}

    /** Handles an UI-Event */
    virtual void OnUIEvent( EUIEvent uiEvent ) {}

    /** Draws particle meshes */
    virtual void DrawFrameParticleMeshes( std::unordered_map<zCVob*, MeshVisualInfo*>& progMeshes ) {}

    /** Draws particle effects */
    virtual void DrawFrameParticles(std::map<zCTexture*, std::vector<ParticleInstanceInfo>>& particles,
        std::map<zCTexture*, ParticleRenderInfo>& info,
        RenderToTextureBuffer* bufferParticleColor,
        RenderToTextureBuffer* bufferParticleDistortion) {}

    virtual void DrawString( const std::string& str, float x, float y, const zFont* font, zColor& fontColor ) {};
    
    virtual XRESULT UpdateRenderStates() { return XR_SUCCESS; };

    virtual std::unique_ptr<GraphicsEventRecord> RecordGraphicsEvent( GraphicsEventName region ) { return std::make_unique<GraphicsEventRecord>(); }
};
