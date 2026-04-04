#include "pch.h"
#include "D3D11PFX_CAS.h"

#include "D3D11PfxRenderer.h"
#include "D3D11GraphicsEngine.h"

// Include AMD FidelityFX CAS header (GPU version)

#define FFX_CPU
#define FFX_HLSL
#include "Shaders/FidelityFX/ffx_core.h"
#include "Shaders/FidelityFX/cas/ffx_cas.h"

D3D11PFX_CAS::D3D11PFX_CAS( D3D11PfxRenderer* renderer )
    : Renderer( renderer ), Sharpness( 0.1f ) {
}

D3D11PFX_CAS::~D3D11PFX_CAS() {
}

void D3D11PFX_CAS::SetSharpness( float sharpness ) {
    Sharpness = std::clamp( sharpness, 0.0f, 1.0f );
}

XRESULT D3D11PFX_CAS::Apply( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& inputTexture, INT2 inputSize,
        const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& outputTexture, INT2 outputSize, RenderToTextureBuffer& intermediateBuffer ) {
    D3D11GraphicsEngine* engine = (D3D11GraphicsEngine*)Engine::GraphicsEngine;
    auto context = engine->GetContext();

    engine->SetDefaultStates();
    engine->UpdateRenderStates();

    // Save old render targets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    ID3D11RenderTargetView* nullRTVs[6] { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    context->OMSetRenderTargets( 6, nullRTVs, nullptr );

    RenderToTextureBuffer& tempBuffer = intermediateBuffer;

    // update the temp buffer with the latest backbuffer data
    Renderer->CopyTextureToRTV( inputTexture, tempBuffer.GetRenderTargetView(), outputSize );

    // Get shader
    auto casPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_CAS );
    if ( !casPS ) {
        return XR_FAILED;
    }
    // Apply shader
    casPS->Apply();

    // Setup CAS constants
    CASConstantBuffer cb;

    // CasSetup: inputSize == outputSize for sharpening-only mode
    ffxCasSetup(
        cb.const0,
        cb.const1,
        static_cast<FfxFloat32>(Sharpness),
        static_cast<FfxFloat32>(inputSize.x),
        static_cast<FfxFloat32>(inputSize.y),
        static_cast<FfxFloat32>(inputSize.x),
        static_cast<FfxFloat32>(inputSize.y)
    );

    // Update constant buffer
    casPS->GetBuffer( "CASConstants" ).Update( &cb ).Bind();

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

    // Copy result to output render target
    Renderer->CopyTextureToRTV( tempBuffer.GetShaderResView(), outputTexture );

    // unbind resources
    static ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
    context->PSSetShaderResources( 0, 1, nullSRV );

    // restore old render targets
    context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

    context->RSSetViewports( 1, &oldVP );

    return XR_SUCCESS;
}
