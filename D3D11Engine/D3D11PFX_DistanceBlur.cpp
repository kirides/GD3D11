#include "pch.h"
#include "D3D11PFX_DistanceBlur.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"

D3D11PFX_DistanceBlur::D3D11PFX_DistanceBlur( D3D11PfxRenderer* rnd ) : D3D11PFX_Effect( rnd ) {}

D3D11PFX_DistanceBlur::~D3D11PFX_DistanceBlur() {}

/** Draws this effect to the given buffer */
XRESULT D3D11PFX_DistanceBlur::Render( RenderToTextureBuffer* fxbuffer ) {
	D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

	// Save old rendertargets
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
	engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

	engine->GetShaderManager().GetVShader( "VS_PFX" )->Apply();
	auto ps = engine->GetShaderManager().GetPShader( "PS_PFX_DistanceBlur" );

	Engine::GAPI->GetRendererState().BlendState.SetDefault();
	Engine::GAPI->GetRendererState().BlendState.SetDirty();

	// Copy scene
    auto tempBuffer = FxRenderer->GetTempBuffer();

	engine->GetContext()->ClearRenderTargetView( tempBuffer->GetRenderTargetView().Get(), reinterpret_cast<float*>(&float4( 0, 0, 0, 0 )) );
    FxRenderer->CopyTextureToRTV( engine->GetGBuffer0().GetShaderResView(), tempBuffer->GetRenderTargetView(), Engine::GraphicsEngine->GetResolution() );

	engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), nullptr );

	// Bind textures
    tempBuffer->BindToPixelShader( engine->GetContext().Get(), 0 );
	engine->GetDepthBuffer()->BindToPixelShader( engine->GetContext().Get(), 1 );

	// Blur/Copy
	ps->Apply();

	FxRenderer->DrawFullScreenQuad();

	// Restore rendertargets
	engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

	return XR_SUCCESS;
}
