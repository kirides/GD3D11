#include "pch.h"
#include "D3D11PFX_HeightFog.h"
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

D3D11PFX_HeightFog::D3D11PFX_HeightFog( D3D11PfxRenderer* rnd ) : D3D11PFX_Effect( rnd ) {}

D3D11PFX_HeightFog::~D3D11PFX_HeightFog() {}

/** Draws this effect to the given buffer */
XRESULT D3D11PFX_HeightFog::Render( RenderToTextureBuffer* fxbuffer ) {
	D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

	// Save old rendertargets
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
	engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

	auto vs = engine->GetShaderManager().GetVShader( "VS_PFX" );
	auto hfPS = engine->GetShaderManager().GetPShader( "PS_PFX_Heightfog" );

	hfPS->Apply();
	vs->Apply();

	HeightfogConstantBuffer cb;
	XMStoreFloat4x4( &cb.InvProj, XMMatrixInverse( nullptr, XMLoadFloat4x4(&Engine::GAPI->GetProjectionMatrix()) ) );

	XMStoreFloat4x4( &cb.InvView, XMMatrixInverse( nullptr, Engine::GAPI->GetViewMatrixXM() ) );

	cb.CameraPosition = Engine::GAPI->GetCameraPosition();
	float NearPlane = Engine::GAPI->GetRendererState().RendererInfo.NearPlane;
	float FarPlane = Engine::GAPI->GetRendererState().RendererInfo.FarPlane;

	cb.HF_GlobalDensity = Engine::GAPI->GetRendererState().RendererSettings.FogGlobalDensity;
	cb.HF_HeightFalloff = Engine::GAPI->GetRendererState().RendererSettings.FogHeightFalloff;

	float height = Engine::GAPI->GetRendererState().RendererSettings.FogHeight;
	XMVECTOR color = XMLoadFloat3( Engine::GAPI->GetRendererState().RendererSettings.FogColorMod.toXMFLOAT3() );

	float fnear = 15000.0f;
	float ffar = 60000.0f;
    float secScale = std::min<float>( Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius, Engine::GAPI->GetRendererState().RendererSettings.FogRange + 3 );

	cb.HF_WeightZNear = std::max( 0.0f, WORLD_SECTION_SIZE * ((secScale - 0.5f) * 0.7f) - (ffar - fnear) ); // Keep distance from original fog but scale the near-fog up to section draw distance
	cb.HF_WeightZFar = WORLD_SECTION_SIZE * ((secScale - 0.5f) * 0.8f);

	float atmoMax = 83200.0f; // TODO: Calculate!
	float atmoMin = 27799.9922f;

	cb.HF_WeightZFar = std::min( cb.HF_WeightZFar, atmoMax );
	cb.HF_WeightZNear = std::min( cb.HF_WeightZNear, atmoMin );

#if !defined(BUILD_GOTHIC_1_08k) || !defined(BUILD_1_12F)
    float fogDensityFactor = 2;
    float fogDensityFactorRain = (1.0f - Engine::GAPI->GetFogOverride());
#else
    float fogDensityFactor = pow( 15000.0f / Engine::GAPI->GetFarZ(), 4.0f );
    float fogDensityFactorRain = 1.0f;
#endif

	if ( Engine::GAPI->GetFogOverride() > 0.0f ) {
		// Make sure the camera is inside the fog when in fog zone
		height = Toolbox::lerp( height, Engine::GAPI->GetCameraPosition().y + 10000, Engine::GAPI->GetFogOverride() ); // TODO: Get this from the actual fog-distance in the fogzone!

		// Override fog color when in fog zone
		color = Engine::GAPI->GetFogColor();

#if !defined(BUILD_GOTHIC_1_08k) || !defined(BUILD_1_12F)
		// Make it z-Fog
		cb.HF_HeightFalloff = Toolbox::lerp( cb.HF_HeightFalloff, 0.000001f, Engine::GAPI->GetFogOverride() );
#endif

		// Turn up density
		cb.HF_GlobalDensity = Toolbox::lerp( cb.HF_GlobalDensity, cb.HF_GlobalDensity * fogDensityFactor, Engine::GAPI->GetFogOverride() );

#if !defined(BUILD_GOTHIC_1_08k) || !defined(BUILD_1_12F)
		// Use other fog-values for fog-zones
		float distNear = WORLD_SECTION_SIZE * ((ffar - fnear) / ffar);
		cb.HF_WeightZNear = Toolbox::lerp( cb.HF_WeightZNear, WORLD_SECTION_SIZE * 0.09f, Engine::GAPI->GetFogOverride() );
		cb.HF_WeightZFar = Toolbox::lerp( cb.HF_WeightZFar, WORLD_SECTION_SIZE * 0.8, Engine::GAPI->GetFogOverride() );
#endif
	}

	//Engine::GAPI->GetRendererState().RendererSettings.FogColorMod;

	cb.HF_FogHeight = height;

	//cb.HF_FogColorMod = Engine::GAPI->GetRendererState().GraphicsState.FF_FogColor;
	cb.HF_ProjAB = float2( Engine::GAPI->GetProjectionMatrix()._33, Engine::GAPI->GetProjectionMatrix()._34 );


	// Modify fog when raining
	float rain = Engine::GAPI->GetRainFXWeight();

	// Color
	XMFLOAT3 FogColorMod;
	XMStoreFloat3( &FogColorMod, XMVectorLerpV( color, XMLoadFloat3( &Engine::GAPI->GetRendererState().RendererSettings.RainFogColor ), XMVectorSet( std::min( 1.0f, rain * 2.0f ), std::min( 1.0f, rain * 2.0f ), std::min( 1.0f, rain * 2.0f ), 0 ) ) ); // Scale color faster here, so it looks better on light rain
	cb.HF_FogColorMod = FogColorMod;
	// Raining Density, only when not in fogzone
	cb.HF_GlobalDensity = Toolbox::lerp( cb.HF_GlobalDensity, Engine::GAPI->GetRendererState().RendererSettings.RainFogDensity, rain * fogDensityFactorRain );


	hfPS->GetConstantBuffer()[0]->UpdateBuffer( &cb );
	hfPS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

	GSky* sky = Engine::GAPI->GetSky();
	hfPS->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
	hfPS->GetConstantBuffer()[1]->BindToPixelShader( 1 );

	engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), nullptr );

	// Bind depthbuffer
	engine->GetDepthBuffer()->BindToPixelShader( engine->GetContext().Get(), 1 );

    engine->SetDefaultStates();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
	Engine::GAPI->GetRendererState().BlendState.SetDefault();
	//Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
	Engine::GAPI->GetRendererState().BlendState.BlendEnabled = true;
	Engine::GAPI->GetRendererState().BlendState.SetDirty();

	// Copy
	FxRenderer->DrawFullScreenQuad();

	// Restore rendertargets
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
	engine->GetContext()->PSSetShaderResources( 1, 1, srv.GetAddressOf() );

	engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

	return XR_SUCCESS;
}
