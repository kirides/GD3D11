#include "D3D11GraphicsEngineBase.h"

#include "BaseAntTweakBar.h"
#include "D3D11LineRenderer.h"
#include "D3D11PipelineStates.h"
#include "D3D11PointLight.h"
#include "D3D11PShader.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "RenderToTextureBuffer.h"
#include "zCView.h"

const unsigned int DRAWVERTEXARRAY_BUFFER_SIZE = 4096 * sizeof( ExVertexStruct );
const int NUM_MAX_BONES = 96;
const unsigned int INSTANCING_BUFFER_SIZE = sizeof( VobInstanceInfo ) * 2048;

// If defined, creates a debug-version of the d3d11-device
//#define DEBUG_D3D11

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

/** Called when the game wants to clear the bound rendertarget */
XRESULT D3D11GraphicsEngineBase::Clear( const float4& color ) {
    GetContext()->ClearDepthStencilView( DepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );
    GetContext()->ClearRenderTargetView( HDRBackBuffer->GetRenderTargetView().Get(), reinterpret_cast<const float*>(&color) );
    GetContext()->ClearRenderTargetView( Backbuffer->GetRenderTargetView().Get(), reinterpret_cast<const float*>(&color) );

    return XR_SUCCESS;
}

/** Creates a vertexbuffer object (Not registered inside) */
XRESULT D3D11GraphicsEngineBase::CreateVertexBuffer( D3D11VertexBuffer** outBuffer ) {
    *outBuffer = new D3D11VertexBuffer;
    return XR_SUCCESS;
}

/** Creates a texture object (Not registered inside) */
XRESULT D3D11GraphicsEngineBase::CreateTexture( D3D11Texture** outTexture ) {
    *outTexture = new D3D11Texture;
    return XR_SUCCESS;
}

/** Creates a constantbuffer object (Not registered inside) */
XRESULT D3D11GraphicsEngineBase::CreateConstantBuffer( D3D11ConstantBuffer** outCB, void* data, int size ) {
    *outCB = new D3D11ConstantBuffer( size, data );
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
    Engine::AntTweakBar->Draw();

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

/** Called when we started to render the world */
XRESULT D3D11GraphicsEngineBase::OnStartWorldRendering() {
    if ( PresentPending )
        return XR_FAILED;

    PresentPending = true;

    // Update transforms
    UpdateTransformsCB();

    // Force farplane
    Engine::GAPI->SetFarPlane( Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE );

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
    auto vShader = ShaderManager->GetVShader( "VS_TransformedEx" );
    auto pShader = ShaderManager->GetPShader( "PS_FixedFunctionPipe" );

    // Bind the FF-Info to the first PS slot
    pShader->GetConstantBuffer()[0]->UpdateBuffer( &Engine::GAPI->GetRendererState().GraphicsState );
    pShader->GetConstantBuffer()[0]->BindToPixelShader( 0 );

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

    vShader->GetConstantBuffer()[0]->UpdateBuffer( temp2Float2 );
    vShader->GetConstantBuffer()[0]->BindToVertexShader( 0 );

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

/** Recreates the renderstates */
XRESULT D3D11GraphicsEngineBase::UpdateRenderStates() {
    if ( Engine::GAPI->GetRendererState().BlendState.StateDirty ) {
        D3D11BlendStateInfo* state = static_cast<D3D11BlendStateInfo*>(GothicStateCache::s_BlendStateMap[Engine::GAPI->GetRendererState().BlendState]);

        if ( !state ) {
            // Create new state
            state = new D3D11BlendStateInfo( Engine::GAPI->GetRendererState().BlendState );

            GothicStateCache::s_BlendStateMap[Engine::GAPI->GetRendererState().BlendState] = state;
        }

        FFBlendState = state->State.Get();

        Engine::GAPI->GetRendererState().BlendState.StateDirty = false;
        GetContext()->OMSetBlendState( FFBlendState.Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )), 0xFFFFFFFF );
    }

    if ( Engine::GAPI->GetRendererState().RasterizerState.StateDirty ) {
        D3D11RasterizerStateInfo* state = static_cast<D3D11RasterizerStateInfo*>(GothicStateCache::s_RasterizerStateMap[Engine::GAPI->GetRendererState().RasterizerState]);

        if ( !state ) {
            // Create new state
            state = new D3D11RasterizerStateInfo( Engine::GAPI->GetRendererState().RasterizerState );

            GothicStateCache::s_RasterizerStateMap[Engine::GAPI->GetRendererState().RasterizerState] = state;
        }

        FFRasterizerState = state->State.Get();

        Engine::GAPI->GetRendererState().RasterizerState.StateDirty = false;
        GetContext()->RSSetState( FFRasterizerState.Get() );
    }

    if ( Engine::GAPI->GetRendererState().DepthState.StateDirty ) {
        D3D11DepthBufferState* state = static_cast<D3D11DepthBufferState*>(GothicStateCache::s_DepthBufferMap[Engine::GAPI->GetRendererState().DepthState]);

        if ( !state ) {
            // Create new state
            state = new D3D11DepthBufferState( Engine::GAPI->GetRendererState().DepthState );

            GothicStateCache::s_DepthBufferMap[Engine::GAPI->GetRendererState().DepthState] = state;
        }

        FFDepthStencilState = state->State.Get();

        Engine::GAPI->GetRendererState().DepthState.StateDirty = false;
        GetContext()->OMSetDepthStencilState( FFDepthStencilState.Get(), 0 );
    }

    return XR_SUCCESS;
}


/** Constructs the makro list for shader compilation */
void D3D11GraphicsEngineBase::ConstructShaderMakroList( std::vector<D3D_SHADER_MACRO>& list ) {
    const GothicRendererSettings& s = Engine::GAPI->GetRendererState().RendererSettings;
    D3D_SHADER_MACRO m;

    m.Name = "SHD_ENABLE";
    m.Definition = s.EnableShadows ? "1" : "0";
    list.push_back( m );

    m.Name = "SHD_FILTER_16TAP_PCF";
    m.Definition = s.EnableSoftShadows ? "1" : "0";
    list.push_back( m );

    m.Name = "SHD_WIND";
#ifdef BUILD_GOTHIC_2_6_fix
    m.Definition = s.WindQuality == GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED ? "1" : "0";
#else
    m.Definition = "0";
#endif
    list.push_back( m );

    m.Name = "SHD_WATERANI";
#ifdef BUILD_GOTHIC_2_6_fix
    m.Definition = s.EnableWaterAnimation ? "1" : "0";
#else
    m.Definition = "0";
#endif
    list.push_back( m );

    m.Name = nullptr;
    m.Definition = nullptr;
    list.push_back( m );
}

void D3D11GraphicsEngineBase::SetupVS_ExMeshDrawCall() {
    UpdateRenderStates();

    if ( ActiveVS )ActiveVS->Apply();
    if ( ActivePS )ActivePS->Apply();

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
}

void D3D11GraphicsEngineBase::SetupVS_ExConstantBuffer() {
    const XMFLOAT4X4& world = Engine::GAPI->GetRendererState().TransformState.TransformWorld;
    const XMFLOAT4X4& view = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& proj = Engine::GAPI->GetProjectionMatrix();

    VS_ExConstantBuffer_PerFrame cb = {};
    cb.View = view;
    cb.Projection = proj;
    XMStoreFloat4x4( &cb.ViewProj, XMMatrixMultiply( XMLoadFloat4x4( &proj ), XMLoadFloat4x4( &view ) ) );
    ActiveVS->GetConstantBuffer()[0]->UpdateBuffer( &cb );
    ActiveVS->GetConstantBuffer()[0]->BindToVertexShader( 0 );
    ActiveVS->GetConstantBuffer()[0]->BindToDomainShader( 0 );
    ActiveVS->GetConstantBuffer()[0]->BindToHullShader( 0 );
}

void D3D11GraphicsEngineBase::SetupVS_ExPerInstanceConstantBuffer() {
    XMFLOAT4X4 world = Engine::GAPI->GetRendererState().TransformState.TransformWorld;

    VS_ExConstantBuffer_PerInstance cb = {};
    cb.World = world;

    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &cb );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );
}

/** Sets the active pixel shader object */
XRESULT D3D11GraphicsEngineBase::SetActivePixelShader( const std::string& shader ) {
    ActivePS = ShaderManager->GetPShader( shader );
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngineBase::SetActiveVertexShader( const std::string& shader ) {
    ActiveVS = ShaderManager->GetVShader( shader );
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngineBase::SetActiveHDShader( const std::string& shader ) {
    ActiveHDS = ShaderManager->GetHDShader( shader );
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngineBase::SetActiveGShader( const std::string& shader ) {
    ActiveGS = ShaderManager->GetGShader( shader );
    return XR_SUCCESS;
}

//int D3D11GraphicsEngineBase::MeasureString(std::string str, zFont* zFont)
//{
//	return 0;
//}

/** Puts the current world matrix into a CB and binds it to the given slot */
void D3D11GraphicsEngineBase::SetupPerInstanceConstantBuffer( int slot ) {
    const XMFLOAT4X4 world = Engine::GAPI->GetRendererState().TransformState.TransformWorld;

    VS_ExConstantBuffer_PerInstance cb = {};
    cb.World = world;

    ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &cb );
    ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( slot );
}

/** Updates the transformsCB with new values from the GAPI */
void D3D11GraphicsEngineBase::UpdateTransformsCB() {
    const XMFLOAT4X4& world = Engine::GAPI->GetRendererState().TransformState.TransformWorld;
    const XMFLOAT4X4& view = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& proj = Engine::GAPI->GetProjectionMatrix();

    VS_ExConstantBuffer_PerFrame cb = {};
    cb.View = view;
    cb.Projection = proj;
    XMStoreFloat4x4( &cb.ViewProj, XMMatrixMultiply( XMLoadFloat4x4( &proj ), XMLoadFloat4x4( &view ) ) );

    TransformsCB->UpdateBuffer( &cb );
}

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
    ActivePS->GetConstantBuffer()[0]->UpdateBuffer( &Engine::GAPI->GetRendererState().GraphicsState );
    ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    UINT offset = 0;
    UINT uStride = stride;
    GetContext()->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    //Draw the mesh
    GetContext()->Draw( numVertices, startVertex );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += numVertices;

    return XR_SUCCESS;
}

/** Binds viewport information to the given constantbuffer slot */
XRESULT D3D11GraphicsEngineBase::BindViewportInformation( const std::string& shader, int slot ) {
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

    auto ps = ShaderManager->GetPShader( shader );
    auto vs = ShaderManager->GetVShader( shader );

    if ( vs ) {
        vs->GetConstantBuffer()[slot]->UpdateBuffer( f2 );
        vs->GetConstantBuffer()[slot]->BindToVertexShader( slot );
    }

    if ( ps ) {
        ps->GetConstantBuffer()[slot]->UpdateBuffer( f2 );
        ps->GetConstantBuffer()[slot]->BindToVertexShader( slot );
    }

    return XR_SUCCESS;
}

/** Returns the graphics-device this is running on */
std::string D3D11GraphicsEngineBase::GetGraphicsDeviceName() {
    return DeviceDescription;
}
