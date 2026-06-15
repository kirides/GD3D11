#include "pch.h"
#include "D3D11PFX_SimpleSharpen.h"
#include "D3D11PfxRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11ShaderManager.h"
#include "D3D11CShader.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"

extern bool FeatureLevel10Compatibility;

XRESULT D3D11PFX_SimpleSharpen::Apply( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& source, INT2 sourceSize,
                                       RenderToTextureBuffer* dest,
                                       INT2 destSize ) {

    return ApplyPixelShader( source, dest, destSize );
}

XRESULT D3D11PFX_SimpleSharpen::ApplyPixelShader( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& source,
                                                  RenderToTextureBuffer* dest,
                                                  INT2 destSize ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto context = engine->GetContext();

    engine->SetDefaultStates();
    engine->UpdateRenderStates();

    // Save old render targets / viewport
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    D3D11_VIEWPORT oldVP;
    UINT n = 1;
    context->RSGetViewports( &n, &oldVP );

    auto sharpenPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Sharpen );
    sharpenPS->Apply();

    PfxSharpenConstantBuffer gcb;
    gcb.G_TextureSize = destSize;
    gcb.G_SharpenStrength = Engine::GAPI->GetRendererState().RendererSettings.SharpenFactor;
    sharpenPS->GetBuffer( "PfxSharpenConstantBuffer" ).Update( &gcb ).Bind();

    engine->SetViewport( ViewportInfo( 0, 0, destSize.x, destSize.y ) );

    // Read source, write destination directly (source != destination, so no copy needed).
    context->OMSetRenderTargets( 1, dest->GetRenderTargetView().GetAddressOf(), nullptr );
    context->PSSetShaderResources( 0, 1, source.GetAddressOf() );

    Renderer->DrawFullScreenQuad();

    // Unbind and restore
    static ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
    context->PSSetShaderResources( 0, 1, nullSRV );
    context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
    context->RSSetViewports( 1, &oldVP );

    return XR_SUCCESS;
}
