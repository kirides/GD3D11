#pragma once
#include "basegraphicsengine.h"
#include <dxgi1_5.h>

class D3D11DepthBufferState;
class D3D11BlendStateInfo;
class D3D11RasterizerStateInfo;
class D3D11PShader;
class D3D11VShader;
class D3D11HDShader;
class D3D11Texture;
class D3D11GShader;

struct RenderToTextureBuffer;
struct RenderToDepthStencilBuffer;
class D3D11ShaderManager;

class D3D11VertexBuffer;
class D3D11LineRenderer;
class D3D11ConstantBuffer;

class D3D11GraphicsEngineBase : public BaseGraphicsEngine {
public:
    D3D11GraphicsEngineBase();
    ~D3D11GraphicsEngineBase();

    /** Called after the fake-DDraw-Device got created */
    virtual XRESULT Init() PURE;

    /** Called when the game created its window */
    virtual XRESULT SetWindow( HWND hWnd );

    /** Called on window resize/resolution change */
    virtual XRESULT OnResize( INT2 newSize ) PURE;

    /** Called when the game wants to render a new frame */
    virtual XRESULT OnBeginFrame() PURE;

    /** Called when the game ended it's frame */
    virtual XRESULT OnEndFrame() PURE;

    /** Called to set the current viewport */
    virtual XRESULT SetViewport( const ViewportInfo& viewportInfo );

    /** Called when the game wants to clear the bound rendertarget */
    virtual XRESULT Clear( const float4& color );

    /** Creates a vertexbuffer object (Not registered inside) */
    virtual XRESULT CreateVertexBuffer( D3D11VertexBuffer** outBuffer );

    /** Creates a texture object (Not registered inside) */
    virtual XRESULT CreateTexture( D3D11Texture** outTexture );

    /** Creates a constantbuffer object (Not registered inside) */
    virtual XRESULT CreateConstantBuffer( D3D11ConstantBuffer** outCB, void* data, int size );

    /** Creates a bufferobject for a shadowed point light */
    virtual XRESULT CreateShadowedPointLight( BaseShadowedPointLight** outPL, VobLightInfo* lightInfo, bool dynamic = false );

    /** Returns a list of available display modes */
    virtual XRESULT GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling = false ) PURE;

    /** Presents the current frame to the screen */
    virtual XRESULT Present();

    /** Called when we started to render the world */
    virtual XRESULT OnStartWorldRendering();

    /** Returns the line renderer object */
    virtual BaseLineRenderer* GetLineRenderer();

    /** Returns the graphics-device this is running on */
    virtual std::string GetGraphicsDeviceName();

    /** Saves a screenshot */
    virtual void SaveScreenshot() {}

    /** Returns the shadermanager */
    D3D11ShaderManager& GetShaderManager();

    /** Draws a vertexarray, used for rendering gothics UI */
    virtual XRESULT DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex = 0, unsigned int stride = sizeof( ExVertexStruct ) );
    virtual XRESULT DrawVertexArrayMM( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex = 0, unsigned int stride = sizeof( ExVertexStruct ) )
    {
        return DrawVertexArray( vertices, numVertices, startVertex, stride );
    }

    /** Draws a vertexbuffer, non-indexed, binding the FF-Pipe values */
    virtual XRESULT DrawVertexBufferFF( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride = sizeof( ExVertexStruct ) );

    /** Binds viewport information to the given constantbuffer slot */
    XRESULT D3D11GraphicsEngineBase::BindViewportInformation( const std::string& shader, int slot );

    /** Returns the Device/Context */
    const Microsoft::WRL::ComPtr<ID3D11Device1>& GetDevice() { return Device; }
    const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& GetContext() { return Context; }

    /** Pixel Shader functions */
    void UnbindActivePS() { ActivePS = nullptr; }
    std::shared_ptr<D3D11PShader>& GetActivePS() { return ActivePS; }
    std::shared_ptr<D3D11VShader>& GetActiveVS() { return ActiveVS; }

    /** Returns the current resolution */
    virtual INT2 GetResolution() { return Resolution; }

    /** Recreates the renderstates */
    XRESULT UpdateRenderStates();

    /** Constructs the makro list for shader compilation */
    static void ConstructShaderMakroList( std::vector<D3D_SHADER_MACRO>& list );

    /** Sets up the default rendering state */
    void SetDefaultStates();

    /** Sets up a draw call for a VS_Ex-Mesh */
    void SetupVS_ExMeshDrawCall();
    void SetupVS_ExConstantBuffer();
    void SetupVS_ExPerInstanceConstantBuffer();

    /** Puts the current world matrix into a CB and binds it to the given slot */
    void SetupPerInstanceConstantBuffer( int slot = 1 );

    /** Sets the active pixel shader object */
    virtual XRESULT SetActivePixelShader( const std::string& shader );
    virtual XRESULT SetActiveVertexShader( const std::string& shader );
    virtual XRESULT SetActiveHDShader( const std::string& shader );
    virtual XRESULT SetActiveGShader( const std::string& shader );
    //virtual int MeasureString(std::string str, zFont* zFont);

    void ResetPresentPending() { PresentPending = false; }

    HWND GetOutputWindow() { return OutputWindow; }

protected:
    /** Updates the transformsCB with new values from the GAPI */
    void UpdateTransformsCB();

    /** Device-objects */
    Microsoft::WRL::ComPtr<IDXGIFactory2> DXGIFactory2;
    Microsoft::WRL::ComPtr<IDXGIAdapter2> DXGIAdapter2;
    std::string DeviceDescription;

    Microsoft::WRL::ComPtr<ID3D11Device1> Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> Context;

    /** Swapchain and resources */
    Microsoft::WRL::ComPtr<IDXGISwapChain1> SwapChain;
    std::unique_ptr<RenderToTextureBuffer> Backbuffer;
    std::unique_ptr<RenderToDepthStencilBuffer> DepthStencilBuffer;
    std::unique_ptr<RenderToTextureBuffer> HDRBackBuffer;

    /** States */
    Microsoft::WRL::ComPtr<ID3D11SamplerState> DefaultSamplerState;

    /** Output-window (Gothics main window)*/
    HWND OutputWindow;

    /** Total resolution we are rendering at */
    INT2 Resolution;

    /** Shader manager */
    std::unique_ptr<D3D11ShaderManager> ShaderManager;

    /** Dynamic buffer for vertex array rendering */
    std::unique_ptr<D3D11VertexBuffer> TempVertexBuffer;

    /** Constantbuffers */
    std::unique_ptr<D3D11ConstantBuffer> TransformsCB; // Holds View/Proj-Transforms

    /** Shaders */
    std::shared_ptr<D3D11PShader> PS_DiffuseNormalmapped;
    std::shared_ptr<D3D11PShader> PS_DiffuseNormalmappedFxMap;
    std::shared_ptr<D3D11PShader> PS_Diffuse;
    std::shared_ptr<D3D11PShader> PS_DiffuseNormalmappedAlphatest;
    std::shared_ptr<D3D11PShader> PS_DiffuseNormalmappedAlphatestFxMap;
    std::shared_ptr<D3D11PShader> PS_DiffuseAlphatest;
    std::shared_ptr<D3D11PShader> PS_Simple;
    std::shared_ptr<D3D11PShader> PS_LinDepth;
    std::shared_ptr<D3D11VShader> VS_Ex;
    std::shared_ptr<D3D11VShader> VS_ExInstancedObj;
    std::shared_ptr<D3D11VShader> VS_ExSkeletal;
    
    std::shared_ptr<D3D11PShader> PS_PortalDiffuse;
    std::shared_ptr<D3D11PShader> PS_WaterfallFoam;

    std::shared_ptr<D3D11VShader> ActiveVS;
    std::shared_ptr<D3D11PShader> ActivePS;
    std::shared_ptr<D3D11HDShader> ActiveHDS;
    std::shared_ptr<D3D11GShader> ActiveGS;

    /** FixedFunction-State render states */
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> FFRasterizerState;
    size_t FFRasterizerStateHash;
    Microsoft::WRL::ComPtr<ID3D11BlendState> FFBlendState;
    size_t FFBlendStateHash;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> FFDepthStencilState;
    size_t FFDepthStencilStateHash;

    /** Debug line-renderer */
    std::unique_ptr<D3D11LineRenderer> LineRenderer;

    /** If true, we are still waiting for a present to happen. Don't draw everything twice! */
    bool PresentPending;
};
