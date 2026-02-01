#include "pch.h"
#include "D3D11PfxRenderer.h"
#include "RenderToTextureBuffer.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11ShaderManager.h"
#include "D3D11PShader.h"
#include "D3D11VShader.h"
#include "D3D11PFX_Blur.h"
#include "D3D11PFX_HeightFog.h"
#include "D3D11PFX_DistanceBlur.h"
#include "D3D11PFX_HDR.h"
#include "D3D11NVHBAO.h"
#include "D3D11PFX_SMAA.h"
#include "D3D11PFX_GodRays.h"
#include "D3D11PFX_TAA.h"
#include "D3D11PFX_SimpleSharpen.h"
#include "D3D11PFX_FSR1.h"

D3D11PfxRenderer::D3D11PfxRenderer() {

    m_texturePool = std::make_unique<TexturePool>( reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine)->GetDevice().Get() );

    FX_Blur = std::make_unique<D3D11PFX_Blur>( this );
    FX_HeightFog = std::make_unique<D3D11PFX_HeightFog>( this );
    //FX_DistanceBlur = new D3D11PFX_DistanceBlur(this);
    FX_HDR = std::make_unique<D3D11PFX_HDR>( this );
    FX_GodRays = std::make_unique<D3D11PFX_GodRays>( this );

    if ( !FeatureLevel10Compatibility ) {
        FX_SMAA = std::make_unique<D3D11PFX_SMAA>( this );
        FX_TAA = std::make_unique<D3D11PFX_TAA>( this );

        FX_TAA->Init();

        NvHBAO = std::make_unique<D3D11NVHBAO>();
        NvHBAO->Init();
    }

    PFX_CAS = std::make_unique<D3D11PFX_CAS>( this );
    PFX_SimpleSharpen = std::make_unique<D3D11PFX_SimpleSharpen>( this );
    PFX_FSR1 = std::make_unique<D3D11PFX_FSR1>( this );
}

D3D11PfxRenderer::~D3D11PfxRenderer() {
    //delete FX_DistanceBlur;
}

/** Renders the distance blur effect */
XRESULT D3D11PfxRenderer::RenderDistanceBlur() {
    FX_DistanceBlur->Render( nullptr );
    return XR_SUCCESS;
}

/** Blurs the given texture */
XRESULT D3D11PfxRenderer::BlurTexture( RenderToTextureBuffer* texture, bool leaveResultInD4_2, float scale, const XMFLOAT4& colorMod, const std::string& finalCopyShader ) {
    FX_Blur->RenderBlur( texture, leaveResultInD4_2, 0.0f, scale, colorMod, finalCopyShader );
    return XR_SUCCESS;
}

/** Renders the heightfog */
XRESULT D3D11PfxRenderer::RenderHeightfog() {
    return FX_HeightFog->Render( nullptr );
}

/** Renders the godrays-Effect */
XRESULT D3D11PfxRenderer::RenderGodRays() {
    return FX_GodRays->Render( nullptr );
}

/** Renders the HDR-Effect */
XRESULT D3D11PfxRenderer::RenderHDR() {
    return FX_HDR->Render( nullptr );
}

/** Renders the SMAA-Effect */
XRESULT D3D11PfxRenderer::RenderSMAA() {
    FX_SMAA->RenderPostFX( reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine)->GetHDRBackBuffer().GetShaderResView() );
    return XR_SUCCESS;
}

/** Renders the TAA-Effect */
XRESULT D3D11PfxRenderer::RenderTAA() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    
    // First, generate the velocity buffer from depth
    FX_TAA->RenderVelocityBuffer(engine->GetDepthBuffer()->GetShaderResView());
    
    // Then render TAA using the velocity buffer
    FX_TAA->RenderPostFX(
        engine->GetHDRBackBuffer().GetShaderResView(),
        engine->GetDepthBuffer()->GetShaderResView(),
        FX_TAA->GetVelocityBufferSRV()
    );
    return XR_SUCCESS;
}

XRESULT D3D11PfxRenderer::RenderCAS( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input, INT2 inputSize, const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& output, INT2 outputSize, RenderToTextureBuffer& intermediateBuffer ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    PFX_CAS->SetSharpness( Engine::GAPI->GetRendererState().RendererSettings.SharpenFactor );
    PFX_CAS->Apply(
        input ? input : engine->GetHDRBackBuffer().GetShaderResView(),
        input ? inputSize : engine->GetResolution(),
        output ? output : engine->GetHDRBackBuffer().GetRenderTargetView(),
        output ? outputSize : engine->GetResolution(),
        intermediateBuffer );
    return XR_SUCCESS;
}

XRESULT D3D11PfxRenderer::RenderSimpleSharpen( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input, INT2 inputSize, const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& output, INT2 outputSize, RenderToTextureBuffer& intermediateBuffer ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    PFX_SimpleSharpen->Apply(
        input ? input : engine->GetHDRBackBuffer().GetShaderResView(),
        input ? inputSize : engine->GetResolution(),
        output ? output : engine->GetHDRBackBuffer().GetRenderTargetView(),
        output ? outputSize : engine->GetResolution(),
        intermediateBuffer);
    return XR_SUCCESS;
}

/** Draws a fullscreenquad */
XRESULT D3D11PfxRenderer::DrawFullScreenQuad() {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    engine->UpdateRenderStates();

    engine->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    //Draw the mesh
    engine->GetContext()->Draw( 3, 0 );

    return XR_SUCCESS;
}

/** Unbinds texturesamplers from the pixel-shader */
XRESULT D3D11PfxRenderer::UnbindPSResources( int num ) {
    ID3D11ShaderResourceView** srv = new ID3D11ShaderResourceView*[num];
    ZeroMemory( srv, sizeof( ID3D11ShaderResourceView* ) * num );
    reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine)->GetContext()->PSSetShaderResources( 0, num, srv );
    delete[] srv;

    return XR_SUCCESS;
}

/** Copies the given texture to the given RTV */
XRESULT D3D11PfxRenderer::CopyTextureToRTV( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& texture, const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv, INT2 targetResolution, bool useCustomPS, INT2 offset ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    D3D11_VIEWPORT oldVP;
    if ( targetResolution.x != 0 && targetResolution.y != 0 ) {
        UINT n = 1;
        engine->GetContext()->RSGetViewports( &n, &oldVP );

        D3D11_VIEWPORT vp;
        vp.TopLeftX = static_cast<float>(offset.x);
        vp.TopLeftY = static_cast<float>(offset.y);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        vp.Width = static_cast<float>(targetResolution.x);
        vp.Height = static_cast<float>(targetResolution.y);

        engine->GetContext()->RSSetViewports( 1, &vp );
    }

    // Save old rendertargets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    // Bind shaders
    if ( !useCustomPS ) {
        auto simplePS = engine->GetShaderManager().GetPShader( "PS_PFX_Simple" );
        simplePS->Apply();
    }

    engine->GetShaderManager().GetVShader( "VS_PFX" )->Apply();

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    engine->GetContext()->PSSetShaderResources( 0, 1, srv.GetAddressOf() );

    engine->GetContext()->OMSetRenderTargets( 1, rtv.GetAddressOf(), nullptr );

    if ( texture.Get() )
        engine->GetContext()->PSSetShaderResources( 0, 1, texture.GetAddressOf() );

    DrawFullScreenQuad();

    engine->GetContext()->PSSetShaderResources( 0, 1, srv.GetAddressOf() );
    engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

    if ( targetResolution.x != 0 && targetResolution.y != 0 ) {
        engine->GetContext()->RSSetViewports( 1, &oldVP );
    }

    return XR_SUCCESS;
}

/** Called on resize */
XRESULT D3D11PfxRenderer::OnResize( const INT2& newResolution ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    m_texturePool->Clear(); // textures will be created on demand
    if ( !FeatureLevel10Compatibility ) {
        FX_SMAA->OnResize( newResolution );
        FX_TAA->OnResize( newResolution );
    }

    return XR_SUCCESS;
}

/** Draws the HBAO-Effect to the given buffer */
XRESULT D3D11PfxRenderer::DrawHBAO( const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv ) {
    return NvHBAO->Render( rtv.Get() );
}

TextureHandle D3D11PfxRenderer::GetTempBuffer()
{
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    DXGI_FORMAT bbufferFormat = engine->GetBackBufferFormat(); // actually intermediate backbuffer format -> HDRBackbuffer
    auto res = engine->GetResolution();

    return m_texturePool->Acquire( TexturePool::Description{res.x, res.y, bbufferFormat });
}

TextureHandle D3D11PfxRenderer::GetBackbufferTempBuffer()
{
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto res = engine->GetBackbufferResolution();

    return m_texturePool->Acquire( TexturePool::Description{ res.x, res.y, DXGI_FORMAT_B8G8R8A8_UNORM } );
}

TextureHandle D3D11PfxRenderer::GetTempBufferDS4()
{
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    DXGI_FORMAT bbufferFormat = engine->GetBackBufferFormat(); // actually intermediate backbuffer format -> HDRBackbuffer
    auto res = engine->GetResolution();

    return m_texturePool->Acquire( TexturePool::Description{ res.x / 4, res.y / 4, bbufferFormat } );
}
