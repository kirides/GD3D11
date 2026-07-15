#include "D3D11GraphicsEngineBase.h"

#include "D3D11LineRenderer.h"
#include "D3D11PipelineStates.h"
#include "D3D11PointLight.h"
#include "D3D11PShader.h"
#include "D3D11ShaderManager.h"
#include "D3D11Texture.h"
#include "D3D11VShader.h"
#include "RenderToTextureBuffer.h"
#include "zCView.h"

// If defined, creates a debug-version of the d3d11-device
#if !PUBLIC_RELEASE
#define DEBUG_D3D11
#endif

D3D11GraphicsEngineBase::D3D11GraphicsEngineBase() {
    OutputWindow = HWND( 0 );
    PresentPending = false;

    // Match the resolution with the current desktop resolution
    Resolution = Engine::GAPI->GetRendererState().RendererSettings.LoadedResolution;
}

D3D11GraphicsEngineBase::~D3D11GraphicsEngineBase() {
    GothicDepthBufferStateInfo::DeleteCachedObjects();
    GothicBlendStateInfo::DeleteCachedObjects();
    GothicRasterizerStateInfo::DeleteCachedObjects();
}

/** Binds a surface's diffuse/normalmap textures to consecutive PS slots. The neutral
    seam for MyDirectDrawSurface7::BindToSlot: callers pass texture objects, the backend
    extracts the native SRVs so no ID3D11* view leaks through the base interface. */
void D3D11GraphicsEngineBase::BindSurfaceTextures( int slot, D3D11Texture* diffuse, D3D11Texture* normalmap, unsigned int numTextures ) {
    ID3D11ShaderResourceView* srvs[2] = { nullptr, nullptr };
    if ( diffuse ) {
        srvs[0] = diffuse->GetShaderResourceView().Get();
    }
    if ( normalmap ) {
        srvs[1] = normalmap->GetShaderResourceView().Get();
    }
    GetContext()->PSSetShaderResources( slot, numTextures, srvs );
}

/** Called when the game created its window */
XRESULT D3D11GraphicsEngineBase::SetWindow( HWND hWnd ) {
    LogInfo() << "Creating swapchain";
    OutputWindow = hWnd;

    OnResize( Resolution );

    return XR_SUCCESS;
}

/** Called to set the current viewport */
XRESULT D3D11GraphicsEngineBase::SetViewport( const ViewportInfo& viewportInfo ) {
    // Set the viewport
    D3D11_VIEWPORT viewport = {};

    viewport.TopLeftX = static_cast<float>(viewportInfo.TopLeftX);
    viewport.TopLeftY = static_cast<float>(viewportInfo.TopLeftY);
    viewport.Width = static_cast<float>(viewportInfo.Width);
    viewport.Height = static_cast<float>(viewportInfo.Height);
    viewport.MinDepth = viewportInfo.MinZ;
    viewport.MaxDepth = viewportInfo.MaxZ;

    GetContext()->RSSetViewports( 1, &viewport );

    return XR_SUCCESS;
}

/** Returns the shadermanager */
D3D11ShaderManager& D3D11GraphicsEngineBase::GetShaderManager() { return *ShaderManager; }

/** Creates a vertexbuffer object (Not registered inside) */
XRESULT D3D11GraphicsEngineBase::CreateVertexBuffer( std::unique_ptr<D3D11VertexBuffer>& outBuffer ) {
    outBuffer.reset(new D3D11VertexBuffer);
    return XR_SUCCESS;
}

/** Creates a texture object (Not registered inside) */
XRESULT D3D11GraphicsEngineBase::CreateTexture( D3D11Texture** outTexture ) {
    *outTexture = new D3D11Texture;
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngineBase::CreateTexture( std::unique_ptr<D3D11Texture>& outTexture ) {
    outTexture.reset(new D3D11Texture);
    return XR_SUCCESS;
}

/** Presents the current frame to the screen */
XRESULT D3D11GraphicsEngineBase::Present() {
    // Set default viewport
    SetViewport( ViewportInfo( 0, 0, Resolution.x, Resolution.y ) );

    // Reset State
    SetDefaultStates();

    // Draw debug-lines
    LineRenderer->Flush();

    // Draw ant tweak bar
    // Engine::AntTweakBar->Draw();

    bool vsync = Engine::GAPI->GetRendererState().RendererSettings.EnableVSync;
    if ( SwapChain->Present( vsync ? 1 : 0, 0 ) == DXGI_ERROR_DEVICE_REMOVED ) {
        switch ( GetDevice()->GetDeviceRemovedReason() ) {
        case DXGI_ERROR_DEVICE_HUNG:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_DEVICE_HUNG)";
            exit( 0 );
            break;

        case DXGI_ERROR_DEVICE_REMOVED:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_DEVICE_REMOVED)";
            exit( 0 );
            break;

        case DXGI_ERROR_DEVICE_RESET:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_DEVICE_RESET)";
            exit( 0 );
            break;

        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_DRIVER_INTERNAL_ERROR)";
            exit( 0 );
            break;

        case DXGI_ERROR_INVALID_CALL:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_INVALID_CALL)";
            exit( 0 );
            break;

        case S_OK:
            LogInfo() << "Device removed, but we're fine!";
            break;

        default:
            LogWarnBox() << "Device Removed! (Unknown reason)";
        }
    }

    // We did our present, set the next frame ready
    PresentPending = false;

    return XR_SUCCESS;
}

/** Returns the line renderer object */
BaseLineRenderer* D3D11GraphicsEngineBase::GetLineRenderer() {
    return LineRenderer.get();
}

/** Sets up the default rendering state */
void D3D11GraphicsEngineBase::SetDefaultStates() {
    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().DepthState.SetDefault();
    Engine::GAPI->GetRendererState().SamplerState.SetDefault();

    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.SetDirty();
    Engine::GAPI->GetRendererState().SamplerState.SetDirty();

    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );

    UpdateRenderStates();
}

/** Draws a vertexarray, used for rendering gothics UI */
XRESULT D3D11GraphicsEngineBase::DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
    UpdateRenderStates();
    auto vShader = ShaderManager->GetVShader( VShaderID::VS_TransformedEx );
    auto pShader = ShaderManager->GetPShader( PShaderID::PS_FixedFunctionPipe );

    // Bind the FF-Info to the first PS slot
    pShader->UpdateBuffer("FFPipelineConstantBuffer", &Engine::GAPI->GetRendererState().GraphicsState, sizeof(Engine::GAPI->GetRendererState().GraphicsState));

    vShader->Apply();
    pShader->Apply();

    // Set vertex type
    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // Bind the viewport information to the shader
    D3D11_VIEWPORT vp;
    UINT num = 1;
    GetContext()->RSGetViewports( &num, &vp );

    // Update viewport information
    const float scale = Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale;
    float2 temp2Float2[2];
    temp2Float2[0].x = vp.TopLeftX / scale;
    temp2Float2[0].y = vp.TopLeftY / scale;
    temp2Float2[1].x = vp.Width / scale;
    temp2Float2[1].y = vp.Height / scale;

    vShader->UpdateBuffer( "Viewport" , &temp2Float2, sizeof(float2) * 2 );

    D3D11_BUFFER_DESC desc;
    TempVertexBuffer->GetVertexBuffer().Get()->GetDesc( &desc );

    // Check if we need a bigger vertexbuffer
    if ( desc.ByteWidth < stride * numVertices ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
            LogInfo() << "TempVertexBuffer too small (" << desc.ByteWidth << "), need " << stride * numVertices << " bytes. Recreating buffer.";

        // Buffer too small, recreate it
        TempVertexBuffer = std::make_unique<D3D11VertexBuffer>();

        TempVertexBuffer->Init( nullptr, stride * numVertices, D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    }

    // Send vertexdata to the GPU
    TempVertexBuffer->UpdateBuffer( vertices, stride * numVertices );

    UINT offset = 0;
    UINT uStride = stride;
    GetContext()->IASetVertexBuffers( 0, 1, TempVertexBuffer->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    //Draw the mesh
    GetContext()->Draw( numVertices, startVertex );

    return XR_SUCCESS;
}

/** Sets the active pixel shader object */
XRESULT D3D11GraphicsEngineBase::SetActivePixelShader( PShaderID shader ) {
    ActivePS = ShaderManager->GetPShader( shader );
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngineBase::SetActiveVertexShader( VShaderID shader ) {
    ActiveVS = ShaderManager->GetVShader( shader );
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngineBase::SetActiveHDShader( HDShaderID shader ) {
    ActiveHDS = ShaderManager->GetHDShader( shader );
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngineBase::SetActiveGShader( GShaderID shader ) {
    ActiveGS = ShaderManager->GetGShader( shader );
    return XR_SUCCESS;
}

//int D3D11GraphicsEngineBase::MeasureString(std::string str, zFont* zFont)
//{
//	return 0;
//}

/** Creates a bufferobject for a shadowed point light */
XRESULT D3D11GraphicsEngineBase::CreateShadowedPointLight( BaseShadowedPointLight** outPL, VobLightInfo* lightInfo, bool dynamic ) {
    if ( Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows > 0 )
        *outPL = new D3D11PointLight( lightInfo, dynamic );
    else
        *outPL = nullptr;

    return XR_SUCCESS;
}

/** Draws a vertexbuffer, non-indexed, binding the FF-Pipe values */
XRESULT D3D11GraphicsEngineBase::DrawVertexBufferFF( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
    SetupVS_ExMeshDrawCall();

    // Bind the FF-Info to the first PS slot
    ActivePS->UpdateBuffer("FFPipelineConstantBuffer", &Engine::GAPI->GetRendererState().GraphicsState, sizeof(Engine::GAPI->GetRendererState().GraphicsState));

    UINT offset = 0;
    UINT uStride = stride;
    GetContext()->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    //Draw the mesh
    GetContext()->Draw( numVertices, startVertex );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += numVertices;

    return XR_SUCCESS;
}

/** Binds viewport information to the given constantbuffer slot */
XRESULT D3D11GraphicsEngineBase::BindViewportInformation( VShaderID shader, int slot ) {
    D3D11_VIEWPORT vp;
    UINT num = 1;
    GetContext()->RSGetViewports( &num, &vp );

    // Update viewport information
    float scale = Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale;
    float2 f2[2];
    f2[0].x = vp.TopLeftX / scale;
    f2[0].y = vp.TopLeftY / scale;
    f2[1].x = vp.Width / scale;
    f2[1].y = vp.Height / scale;

    auto vs = ShaderManager->GetVShader( shader );

    if ( vs ) {
        vs->UpdateBuffer( "Viewport" , &f2, sizeof(float2) * 2 );
    }

    return XR_SUCCESS;
}

/** Returns the graphics-device this is running on */
const std::string& D3D11GraphicsEngineBase::GetGraphicsDeviceName() {
    return DeviceDescription;
}
