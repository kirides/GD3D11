#include "pch.h"
#include "D3D11PFX_GodRays.h"
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
#include "GSky.h"

D3D11PFX_GodRays::D3D11PFX_GodRays( D3D11PfxRenderer* rnd ) : D3D11PFX_Effect( rnd ) {}

D3D11PFX_GodRays::~D3D11PFX_GodRays() {}

/** Draws this effect to the given buffer */
XRESULT D3D11PFX_GodRays::Render( RenderToTextureBuffer* fxbuffer ) {
    if ( Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y <= 0 )
        return XR_SUCCESS; // Don't render the godrays in the night-time

	D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

	engine->SetDefaultStates();

	XMVECTOR xmSunPosition = XMLoadFloat3( Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() );

	float outerRadius = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_OuterRadius;
	xmSunPosition *= outerRadius;
	xmSunPosition += Engine::GAPI->GetCameraPositionXM(); // Maybe use cameraposition from sky?

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	XMMATRIX proj = XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() );

	XMMATRIX viewProj = XMMatrixTranspose( XMMatrixMultiply( proj, view ) );
	view = XMMatrixTranspose( view );

	XMFLOAT3 sunViewPosition; XMStoreFloat3( &sunViewPosition, XMVector3TransformCoord( xmSunPosition, view ) ); // This is for checking if the light is behind the camera
	XMFLOAT3 sunPosition; XMStoreFloat3( &sunPosition, XMVector3TransformCoord( xmSunPosition, viewProj ) );

	if ( sunViewPosition.z < 0.0f )
		return XR_SUCCESS; // Don't render the godrays when the sun is behind the camera

	GodRayZoomConstantBuffer gcb = {};
	gcb.GR_Weight = 1.0f;
	gcb.GR_Decay = Engine::GAPI->GetRendererState().RendererSettings.GodRayDecay;
	gcb.GR_Weight = Engine::GAPI->GetRendererState().RendererSettings.GodRayWeight;
	gcb.GR_Density = Engine::GAPI->GetRendererState().RendererSettings.GodRayDensity;

	gcb.GR_Center.x = sunPosition.x / 2.0f + 0.5f;
	gcb.GR_Center.y = sunPosition.y / -2.0f + 0.5f;

	gcb.GR_ColorMod = Engine::GAPI->GetRendererState().RendererSettings.GodRayColorMod;

	if ( abs( gcb.GR_Center.x - 0.5f ) > 0.5f )
		gcb.GR_Weight *= std::max( 0.0f, 1.0f - (abs( gcb.GR_Center.x - 0.5f ) - 0.5f) / 0.5f );

	if ( abs( gcb.GR_Center.y - 0.5f ) > 0.5f )
		gcb.GR_Weight *= std::max( 0.0f, 1.0f - (abs( gcb.GR_Center.y - 0.5f ) - 0.5f) / 0.5f );

	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;

	engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

	auto vs = engine->GetShaderManager().GetVShader( "VS_PFX" );
	auto maskPS = engine->GetShaderManager().GetPShader( "PS_PFX_GodRayMask" );
	auto zoomPS = engine->GetShaderManager().GetPShader( "PS_PFX_GodRayZoom" );

	maskPS->Apply();
	vs->Apply();

    auto tempBuffer = FxRenderer->GetTempBufferDS4();
    auto tempBuffer2 = FxRenderer->GetTempBufferDS4();

	// Draw downscaled mask
	engine->GetContext()->OMSetRenderTargets( 1, tempBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    engine->GetHDRBackBuffer().BindToPixelShader( engine->GetContext().Get(), 0 );
    engine->GetGBuffer1().BindToPixelShader( engine->GetContext().Get(), 1 );

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.Width = static_cast<float>(tempBuffer->GetSizeX());
    vp.Height = static_cast<float>(tempBuffer->GetSizeY());

    engine->GetContext()->RSSetViewports( 1, &vp );

    FxRenderer->DrawFullScreenQuad();

    // Zoom
    zoomPS->Apply();

    zoomPS->GetConstantBuffer()[0]->UpdateBuffer( &gcb );
    zoomPS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    FxRenderer->CopyTextureToRTV( tempBuffer->GetShaderResView(), tempBuffer2->GetRenderTargetView(), INT2( 0, 0 ), true );

    // Upscale and blend
    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    FxRenderer->CopyTextureToRTV( tempBuffer2->GetShaderResView(), oldRTV, engine->GetResolution() );

	vp.Width = static_cast<float>(engine->GetResolution().x);
	vp.Height = static_cast<float>(engine->GetResolution().y);

	engine->GetContext()->RSSetViewports( 1, &vp );

	engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

	return XR_SUCCESS;
}
