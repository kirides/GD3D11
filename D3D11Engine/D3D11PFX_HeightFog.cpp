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

HeightfogConstantBuffer BuildHeightfogConstantBuffer() {
	HeightfogConstantBuffer cb = {};
	const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
	const float fogOverride = Engine::GAPI->GetFogOverride();

	{
		auto& proj = Engine::GAPI->GetProjectionMatrix();
		cb.HF_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
	}

	XMStoreFloat4x4( &cb.InvView, XMMatrixInverse( nullptr, Engine::GAPI->GetViewMatrixXM() ) );
	cb.CameraPosition = Engine::GAPI->GetCameraPosition();

	cb.HF_GlobalDensity = settings.FogGlobalDensity;
	cb.HF_HeightFalloff = settings.FogHeightFalloff;

	float height = settings.FogHeight;
	float3 fogColorBase = settings.FogColorMod;
	if ( settings.AutoFogColor ) {
		WorldInfo* worldInfo = Engine::GAPI->GetLoadedWorldInfo();
		if ( worldInfo && worldInfo->HasWorldFogColorAtLoad ) {
			fogColorBase = worldInfo->WorldFogColorAtLoad;
		}
	}
	XMVECTOR color = XMLoadFloat3( fogColorBase.toXMFLOAT3() );

	float fnear = 15000.0f;
	float ffar = 60000.0f;
	float secScale = std::min<float>( settings.SectionDrawRadius, settings.FogRange );

	cb.HF_WeightZNear = std::max( 0.0f, WORLD_SECTION_SIZE * ((secScale - 0.5f) * 0.7f) - (ffar - fnear) );
	cb.HF_WeightZFar = WORLD_SECTION_SIZE * ((secScale - 0.5f) * 0.8f);

	float atmoMax = 83200.0f;
	float atmoMin = 27799.9922f;

	cb.HF_WeightZFar = std::min( cb.HF_WeightZFar, atmoMax );
	cb.HF_WeightZNear = std::min( cb.HF_WeightZNear, atmoMin );

#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
	float fogDensityFactor = 2;
	float fogDensityFactorRain = (1.0f - fogOverride);
#else
	float fogDensityFactor = pow( 15000.0f / Engine::GAPI->GetFarZ(), 4.0f );
	float fogDensityFactorRain = 1.0f;
#endif

	if ( fogOverride > 0.0f ) {
		height = Toolbox::lerp( height, Engine::GAPI->GetCameraPosition().y + 10000, fogOverride );
		color = Engine::GAPI->GetFogColor();

#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
		cb.HF_HeightFalloff = Toolbox::lerp( cb.HF_HeightFalloff, 0.000001f, fogOverride );
#endif

		cb.HF_GlobalDensity = Toolbox::lerp( cb.HF_GlobalDensity, cb.HF_GlobalDensity * fogDensityFactor, fogOverride );

#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
		cb.HF_WeightZNear = Toolbox::lerp( cb.HF_WeightZNear, WORLD_SECTION_SIZE * 0.09f, fogOverride );
		cb.HF_WeightZFar = Toolbox::lerp( cb.HF_WeightZFar, WORLD_SECTION_SIZE * 0.8, fogOverride );
#endif
	}

	cb.HF_FogHeight = height;
	cb.HF_ProjAB = float2( Engine::GAPI->GetProjectionMatrix()._33, Engine::GAPI->GetProjectionMatrix()._34 );

	float rain = Engine::GAPI->GetRainFXWeight();

	XMFLOAT3 fogColorMod;
	XMStoreFloat3( &fogColorMod, XMVectorLerpV( color, XMLoadFloat3( &settings.RainFogColor ), XMVectorSet( std::min( 1.0f, rain * 2.0f ), std::min( 1.0f, rain * 2.0f ), std::min( 1.0f, rain * 2.0f ), 0 ) ) );
	cb.HF_FogColorMod = fogColorMod;
	cb.HF_GlobalDensity = Toolbox::lerp( cb.HF_GlobalDensity, settings.RainFogDensity, rain * fogDensityFactorRain );

	cb.HF_GlobalDistanceDensity = std::max( 0.0f, settings.FogGlobalDistanceDensity );
	cb.HF_GlobalDistanceStart = std::max( 0.0f, settings.FogGlobalDistanceStart );
	cb.HF_GlobalDistanceRange = std::max( 1.0f, settings.FogGlobalDistanceRange );
	cb.HF_MaxOpacity = std::clamp( settings.FogMaxOpacity, 0.0f, 1.0f );
	cb.HF_SecondaryFogHeight = settings.FogLayer2Height;
	cb.HF_SecondaryHeightFalloff = std::max( 0.0f, settings.FogLayer2HeightFalloff );
	cb.HF_SecondaryGlobalDensity = std::max( 0.0f, settings.FogLayer2GlobalDensity );
	cb.HF_SecondaryWeight = std::clamp( settings.FogLayer2Weight, 0.0f, 1.0f );
	cb.HF_GlobalDistanceColorMod = settings.FogGlobalDistanceColorMod;
	cb.HF_SecondaryFogColorMod = settings.FogLayer2ColorMod;

	const float swampBlend = std::clamp( fogOverride * std::max( 0.0f, settings.FogSwampBlendStrength ), 0.0f, 1.0f );
	cb.HF_SwampBlend = swampBlend;
	cb.HF_SecondaryFogHeight += settings.FogSwampHeightOffset * swampBlend;
	cb.HF_SecondaryGlobalDensity *= Toolbox::lerp( 1.0f, std::max( 1.0f, settings.FogSwampLayer2DensityMul ), swampBlend );
	cb.HF_GlobalDistanceDensity *= Toolbox::lerp( 1.0f, std::max( 1.0f, settings.FogSwampDistanceDensityMul ), swampBlend );
	cb.HF_SecondaryWeight = std::clamp( cb.HF_SecondaryWeight + settings.FogSwampLayer2WeightBoost * swampBlend, 0.0f, 1.0f );

	return cb;
}

/** Draws this effect to the given buffer */
XRESULT D3D11PFX_HeightFog::Render( RenderToTextureBuffer* fxbuffer ) {
	D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

	// Save old rendertargets
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
	engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

	auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
	auto hfPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Heightfog );

	hfPS->Apply();
	vs->Apply();

	HeightfogConstantBuffer cb = BuildHeightfogConstantBuffer();


	hfPS->GetBuffer( "PFXBuffer" ).Update( &cb ).Bind();

	GSky* sky = Engine::GAPI->GetSky();
	hfPS->GetBuffer( "Atmosphere" ).Update( &sky->GetAtmosphereCB() ).Bind();

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
