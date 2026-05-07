#pragma once
#include "BaseGraphicsEngine.h"
#include "D3DGraphicsEventRecord.h"
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
    using BaseGraphicsEngine::RecordGraphicsEvent;

    D3D11GraphicsEngineBase();
    ~D3D11GraphicsEngineBase() override;

    /** Called after the fake-DDraw-Device got created */
    XRESULT Init() override PURE;

    /** Called when the game created its window */
    XRESULT SetWindow( HWND hWnd ) override;

    /** Called on window resize/resolution change */
    XRESULT OnResize( INT2 newSize ) override PURE;
    XRESULT TriggerResize( INT2 resolution ) override PURE;

    /** Called when the game wants to render a new frame */
    XRESULT OnBeginFrame() override PURE;

    /** Called when the game ended it's frame */
    XRESULT OnEndFrame() override PURE;

    /** Called to set the current viewport */
    XRESULT SetViewport( const ViewportInfo& viewportInfo ) override;

    /** Called when the game wants to clear the bound rendertarget */
    XRESULT Clear( const float4& color ) override PURE;

    /** Creates a vertexbuffer object (Not registered inside) */
    XRESULT CreateVertexBuffer( D3D11VertexBuffer** outBuffer ) override;

    /** Creates a texture object (Not registered inside) */
    XRESULT CreateTexture( D3D11Texture** outTexture ) override;

    /** Creates a constantbuffer object (Not registered inside) */
    XRESULT CreateConstantBuffer( D3D11ConstantBuffer** outCB, void* data, int size ) override;

    /** Creates a bufferobject for a shadowed point light */
    XRESULT CreateShadowedPointLight( BaseShadowedPointLight** outPL, VobLightInfo* lightInfo, bool dynamic = false ) override;

    /** Returns a list of available display modes */
    XRESULT GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling = false ) override PURE;

    /** Presents the current frame to the screen */
    XRESULT Present() override;

    /** Called when we started to render the world */
    XRESULT OnStartWorldRendering() override;

    /** Returns the line renderer object */
    BaseLineRenderer* GetLineRenderer() override;

    /** Returns the graphics-device this is running on */
    const std::string& GetGraphicsDeviceName() override;

    /** Saves a screenshot */
    virtual void SaveScreenshot() {}

    /** Returns the shadermanager */
    D3D11ShaderManager& GetShaderManager();

    /** Draws a vertexarray, used for rendering gothics UI */
    XRESULT DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex = 0, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Draws a vertexbuffer, non-indexed, binding the FF-Pipe values */
    XRESULT DrawVertexBufferFF( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride = sizeof( ExVertexStruct ) ) override;

    /** Binds viewport information to the given constantbuffer slot */
    XRESULT BindViewportInformation( VShaderID shader, int slot ) override;

    /** Returns the Device/Context */
    auto GetDevice() -> const auto& { return Device; }
    auto GetContext() -> const auto& { return Context; }

    /** Pixel Shader functions */
    void UnbindActivePS() { ActivePS = nullptr; }
    auto GetActivePS() -> auto& { return ActivePS; }
    auto GetActiveVS() -> auto& { return ActiveVS; }
    auto GetActiveGS() -> auto& { return ActiveGS; }
    auto SetActivePS(std::shared_ptr<D3D11PShader>& ps) -> auto& { return ActivePS = ps; }

    /** Returns the current resolution */
    INT2 GetResolution() override { return Resolution; }

    /** Recreates the renderstates */
    XRESULT UpdateRenderStates() override PURE;

    /** Sets up the default rendering state */
    void SetDefaultStates();

    /** Sets up a draw call for a VS_Ex-Mesh */
    virtual void SetupVS_ExMeshDrawCall() PURE;
    virtual void SetupVS_ExConstantBuffer() PURE;
    virtual void SetupVS_ExPerInstanceConstantBuffer() PURE;

    /** Sets the active pixel shader object */
    XRESULT SetActivePixelShader( PShaderID shader ) override;
    XRESULT SetActiveVertexShader( VShaderID shader ) override;
    virtual XRESULT SetActiveHDShader( HDShaderID shader );
    virtual XRESULT SetActiveGShader( GShaderID shader );
    //virtual int MeasureString(std::string str, zFont* zFont);

    void ResetPresentPending() { PresentPending = false; }

    auto GetOutputWindow() -> auto { return OutputWindow; }
    ID3D11SamplerState* GetDefaultSamplerState() const { return DefaultSamplerState.Get(); }

    std::unique_ptr<GraphicsEventRecord> RecordGraphicsEvent( GraphicsEventName region ) override {
        return std::make_unique<GraphicsEventRecord>();
    }

protected:
    /** Updates the transformsCB with new values from the GAPI */
    void UpdateTransformsCB();

    /** Device-objects */
    Microsoft::WRL::ComPtr<IDXGIFactory2> DXGIFactory2;
    Microsoft::WRL::ComPtr<IDXGIAdapter2> DXGIAdapter2;
    std::string DeviceDescription;

    Microsoft::WRL::ComPtr<ID3D11Device1> Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> Context;
    Microsoft::WRL::ComPtr<ID3DUserDefinedAnnotation> m_UserDefinedAnnotation;

    /** Swapchain and resources */
    Microsoft::WRL::ComPtr<IDXGISwapChain1> SwapChain;
    std::unique_ptr<RenderToTextureBuffer> Backbuffer;
    std::unique_ptr<RenderToDepthStencilBuffer> m_SwapchainDepthStencilBuffer;

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

    /** Resolved shader IDs (may point to different actual shaders based on settings like AllowNormalmaps) */
    PShaderID Resolved_DiffuseNormalmapped;
    PShaderID Resolved_DiffuseNormalmappedFxMap;
    PShaderID Resolved_DiffuseNormalmappedAlphatest;
    PShaderID Resolved_DiffuseNormalmappedAlphatestFxMap;

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
