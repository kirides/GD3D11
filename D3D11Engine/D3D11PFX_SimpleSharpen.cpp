#include "pch.h"
#include "D3D11PFX_SimpleSharpen.h"
#include "D3D11PfxRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11ShaderManager.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"

D3D11PFX_SimpleSharpen::D3D11PFX_SimpleSharpen( D3D11PfxRenderer* renderer )
    : Renderer( renderer ), Sharpness( 0.5f ) {
}

D3D11PFX_SimpleSharpen::~D3D11PFX_SimpleSharpen() {
}

XRESULT D3D11PFX_SimpleSharpen::Apply( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& inputTexture, INT2 inputSize,
        const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& outputTexture,
    INT2 outputSize,
        RenderToTextureBuffer& intermediateBuffer ) {
    D3D11GraphicsEngine* engine = (D3D11GraphicsEngine*)Engine::GraphicsEngine;
    auto context = engine->GetContext();

    engine->SetDefaultStates();
    engine->UpdateRenderStates();

    // Save old render targets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    // Get temp buffer to render into
    RenderToTextureBuffer& tempBuffer = intermediateBuffer;

    // update the temp buffer with the latest backbuffer data
    Renderer->CopyTextureToRTV( inputTexture, tempBuffer.GetRenderTargetView(), outputSize );

    auto sharpenPS = engine->GetShaderManager().GetPShader( "PS_PFX_Sharpen" );
    sharpenPS->Apply();

    PfxSharpenConstantBuffer gcb;
    gcb.G_TextureSize = inputSize;
    gcb.G_SharpenStrength = Engine::GAPI->GetRendererState().RendererSettings.SharpenFactor;

    sharpenPS->GetConstantBuffer()[0]->UpdateBuffer( &gcb );
    sharpenPS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    D3D11_VIEWPORT oldVP;
    UINT n = 1;
    context->RSGetViewports( &n, &oldVP );

    engine->SetViewport( ViewportInfo( 0, 0, outputSize.x, outputSize.y ) );

    // Set render target
    context->OMSetRenderTargets( 1, tempBuffer.GetRenderTargetView().GetAddressOf(), nullptr );

    // Bind input texture
    context->PSSetShaderResources( 0, 1, inputTexture.GetAddressOf() );

    // Draw fullscreen quad
    Renderer->DrawFullScreenQuad();

    // unbind resources
    static ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
    context->PSSetShaderResources( 0, 1, nullSRV );

    Renderer->CopyTextureToRTV( tempBuffer.GetShaderResView(), outputTexture, INT2( 0, 0 ), true );

    // restore old render targets
    context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

    context->RSSetViewports( 1, &oldVP );


    return XR_SUCCESS;
}
