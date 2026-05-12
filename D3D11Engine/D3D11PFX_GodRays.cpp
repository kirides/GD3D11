#include "pch.h"
#include "D3D11PFX_GodRays.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11CShader.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "GSky.h"
#include "TexturePool.h"

extern bool FeatureLevel10Compatibility;

D3D11PFX_GodRays::D3D11PFX_GodRays( D3D11PfxRenderer* rnd ) : D3D11PFX_Effect( rnd ) {}

/** Draws this effect to the given buffer */
XRESULT D3D11PFX_GodRays::Render( 
    ID3D11ShaderResourceView* backbuffer, 
    ID3D11ShaderResourceView* depth ) {
    if ( Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y <= 0 ) {
        return XR_SUCCESS; // Don't render the godrays in the night-time
    }

	D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

	engine->SetDefaultStates();

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    if ( !FeatureLevel10Compatibility ) {
        auto res = RenderCS( backbuffer, depth );
        engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return res;
    }

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

	auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
	auto maskPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GodRayMask );
	auto zoomPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GodRayZoom );

	maskPS->Apply();
	vs->Apply();

    auto tempBuffer = FxRenderer->GetTempBufferDS4();
    auto tempBuffer2 = FxRenderer->GetTempBufferDS4();

	// Draw downscaled mask
	engine->GetContext()->OMSetRenderTargets( 1, tempBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    ID3D11ShaderResourceView* srvs[2] {
        backbuffer,
        depth,
    };
    engine->GetContext()->PSSetShaderResources( 0, 2, srvs );

    engine->SetViewport({ 0,0, INT2(tempBuffer->GetSizeX(), tempBuffer->GetSizeY()) });

    FxRenderer->DrawFullScreenQuad();

    // Zoom
    zoomPS->Apply();

    zoomPS->GetBuffer( "GodRayZoomConstantBuffer" ).Update( &gcb ).Bind();

    auto clampSampler = engine->GetClampSamplerState();
    engine->GetContext()->PSSetSamplers( 0, 1, &clampSampler );

    FxRenderer->CopyTextureToRTV( tempBuffer->GetShaderResView(), tempBuffer2->GetRenderTargetView(), INT2( 0, 0 ), true );

    // Upscale and blend
    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    FxRenderer->CopyTextureToRTV( tempBuffer2->GetShaderResView(), oldRTV, engine->GetResolution() );

    engine->SetViewport({ 0,0, engine->GetResolution() });

    ID3D11ShaderResourceView* nullSRVs[2] {
        nullptr,
        nullptr,
    };
    engine->GetContext()->PSSetShaderResources( 0, 2, nullSRVs );
    
	engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

	return XR_SUCCESS;
}

/** Compute shader path for FL11+ */
XRESULT D3D11PFX_GodRays::RenderCS(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depthCopy ) {

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();

    engine->SetDefaultStates();

    XMVECTOR xmSunPosition = XMLoadFloat3( Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() );

    float outerRadius = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_OuterRadius;
    xmSunPosition *= outerRadius;
    xmSunPosition += Engine::GAPI->GetCameraPositionXM();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX proj = XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() );

    XMMATRIX viewProj = XMMatrixTranspose( XMMatrixMultiply( proj, view ) );
    view = XMMatrixTranspose( view );

    XMFLOAT3 sunViewPosition; XMStoreFloat3( &sunViewPosition, XMVector3TransformCoord( xmSunPosition, view ) );
    XMFLOAT3 sunPosition; XMStoreFloat3( &sunPosition, XMVector3TransformCoord( xmSunPosition, viewProj ) );

    if ( sunViewPosition.z < 0.0f )
        return XR_SUCCESS;

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

    // Save old render targets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    ID3D11RenderTargetView* nullRtv = nullptr;
    engine->GetContext()->OMSetRenderTargets( 1, &nullRtv, nullptr );

    auto res = engine->GetResolution();
    INT2 ds4Size = { res.x / 4, res.y / 4 };

    // Acquire DS4 UAV-capable textures from the pool
    auto maskBuffer = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ ds4Size.x, ds4Size.y, engine->GetBackBufferFormat(),
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );
    auto zoomBuffer = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ ds4Size.x, ds4Size.y, engine->GetBackBufferFormat(),
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );

    auto clampSampler = engine->GetClampSamplerState();

    // --- Pass 1: Compute Shader Mask ---
    auto maskCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayMask );
    maskCS->Apply();

    context->CSSetSamplers( 0, 1, &clampSampler );

    ID3D11ShaderResourceView* maskSRVs[2] = { backbuffer, depthCopy };
    context->CSSetShaderResources( 0, 2, maskSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, maskBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (ds4Size.x + 7) / 8, (ds4Size.y + 7) / 8, 1 );

    // Unbind UAV and SRVs
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    context->CSSetShaderResources( 0, 2, nullSRVs );

    // --- Pass 2: Compute Shader Zoom ---
    auto zoomCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayZoom );
    zoomCS->Apply();

    zoomCS->GetBuffer( "GodRayZoomConstantBuffer" ).Update( &gcb ).Bind();

    context->CSSetSamplers( 0, 1, &clampSampler );

    ID3D11ShaderResourceView* zoomSRV = maskBuffer->GetShaderResView().Get();
    context->CSSetShaderResources( 0, 1, &zoomSRV );
    context->CSSetUnorderedAccessViews( 0, 1, zoomBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (ds4Size.x + 7) / 8, (ds4Size.y + 7) / 8, 1 );

    // Unbind
    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    ID3D11ShaderResourceView* nullSRV1 = nullptr;
    context->CSSetShaderResources( 0, 1, &nullSRV1 );
    context->CSSetShader( nullptr, nullptr, 0 );

    // --- Pass 3: Upscale and additive blend via pixel shader (reuse existing path) ---
    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    FxRenderer->CopyTextureToRTV( zoomBuffer->GetShaderResView(), oldRTV, res );

    engine->SetViewport( { 0, 0, res } );

    engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
    auto defaultSampler = engine->GetDefaultSamplerState();
    engine->GetContext()->PSSetSamplers( 0, 1, &defaultSampler );

    return XR_SUCCESS;
}

/** Public entry point: renders godrays to a pool texture, skipping the final additive blit */
XRESULT D3D11PFX_GodRays::RenderToTexture(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView** outGodRaysSRV ) {

    *outGodRaysSRV = nullptr;

    if ( Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y <= 0 )
        return XR_SUCCESS;

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    engine->SetDefaultStates();

    if ( !FeatureLevel10Compatibility ) {
        return RenderToTextureCS( backbuffer, depthCopy, outGodRaysSRV );
    }

    // FL10 pixel shader path: mask → zoom → write to pool texture (no additive blit)
    XMVECTOR xmSunPosition = XMLoadFloat3( Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() );
    float outerRadius = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_OuterRadius;
    xmSunPosition *= outerRadius;
    xmSunPosition += Engine::GAPI->GetCameraPositionXM();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX proj = XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() );
    XMMATRIX viewProj = XMMatrixTranspose( XMMatrixMultiply( proj, view ) );
    view = XMMatrixTranspose( view );

    XMFLOAT3 sunViewPosition; XMStoreFloat3( &sunViewPosition, XMVector3TransformCoord( xmSunPosition, view ) );
    XMFLOAT3 sunPosition; XMStoreFloat3( &sunPosition, XMVector3TransformCoord( xmSunPosition, viewProj ) );

    if ( sunViewPosition.z < 0.0f )
        return XR_SUCCESS;

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

    auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    auto maskPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GodRayMask );
    auto zoomPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GodRayZoom );

    maskPS->Apply();
    vs->Apply();

    auto tempBuffer = FxRenderer->GetTempBufferDS4();
    auto tempBuffer2 = FxRenderer->GetTempBufferDS4();

    engine->GetContext()->OMSetRenderTargets( 1, tempBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    ID3D11ShaderResourceView* srvs[2] { backbuffer, depthCopy };
    engine->GetContext()->PSSetShaderResources( 0, 2, srvs );
    engine->SetViewport({ 0, 0, INT2(tempBuffer->GetSizeX(), tempBuffer->GetSizeY()) });
    FxRenderer->DrawFullScreenQuad();

    zoomPS->Apply();
    zoomPS->GetBuffer( "GodRayZoomConstantBuffer" ).Update( &gcb ).Bind();

    auto clampSampler = engine->GetClampSamplerState();
    engine->GetContext()->PSSetSamplers( 0, 1, &clampSampler );

    FxRenderer->CopyTextureToRTV( tempBuffer->GetShaderResView(), tempBuffer2->GetRenderTargetView(), INT2( 0, 0 ), true );

    // Keep the result texture alive until next frame by storing the handle as a member
    m_GodRaysResult = std::move( tempBuffer2 );
    *outGodRaysSRV = m_GodRaysResult->GetShaderResView().Get();

    ID3D11ShaderResourceView* nullSRVs[2] { nullptr, nullptr };
    engine->GetContext()->PSSetShaderResources( 0, 2, nullSRVs );

    auto defaultSampler2 = engine->GetDefaultSamplerState();
    engine->GetContext()->PSSetSamplers( 0, 1, &defaultSampler2 );

    return XR_SUCCESS;
}

/** CS path: mask+zoom to pool texture, no final blit */
XRESULT D3D11PFX_GodRays::RenderToTextureCS(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* normals,
    ID3D11ShaderResourceView** outGodRaysSRV ) {

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();

    XMVECTOR xmSunPosition = XMLoadFloat3( Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() );
    float outerRadius = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_OuterRadius;
    xmSunPosition *= outerRadius;
    xmSunPosition += Engine::GAPI->GetCameraPositionXM();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX proj = XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() );
    XMMATRIX viewProj = XMMatrixTranspose( XMMatrixMultiply( proj, view ) );
    view = XMMatrixTranspose( view );

    XMFLOAT3 sunViewPosition; XMStoreFloat3( &sunViewPosition, XMVector3TransformCoord( xmSunPosition, view ) );
    XMFLOAT3 sunPosition; XMStoreFloat3( &sunPosition, XMVector3TransformCoord( xmSunPosition, viewProj ) );

    if ( sunViewPosition.z < 0.0f )
        return XR_SUCCESS;

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

    ID3D11RenderTargetView* nullRtv = nullptr;
    context->OMSetRenderTargets( 1, &nullRtv, nullptr );

    auto res = engine->GetResolution();
    INT2 ds4Size = { res.x / 4, res.y / 4 };

    auto maskBuffer = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ ds4Size.x, ds4Size.y, engine->GetBackBufferFormat(),
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );
    auto zoomBuffer = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ ds4Size.x, ds4Size.y, engine->GetBackBufferFormat(),
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );

    auto clampSampler = engine->GetClampSamplerState();

    // --- Pass 1: CS Mask ---
    auto maskCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayMask );
    maskCS->Apply();
    context->CSSetSamplers( 0, 1, &clampSampler );

    ID3D11ShaderResourceView* maskSRVs[2] = { backbuffer, normals };
    context->CSSetShaderResources( 0, 2, maskSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, maskBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );
    context->Dispatch( (ds4Size.x + 7) / 8, (ds4Size.y + 7) / 8, 1 );

    ID3D11UnorderedAccessView* nullUAV = nullptr;
    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    context->CSSetShaderResources( 0, 2, nullSRVs );

    // --- Pass 2: CS Zoom ---
    auto zoomCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayZoom );
    zoomCS->Apply();
    zoomCS->GetBuffer( "GodRayZoomConstantBuffer" ).Update( &gcb ).Bind();
    context->CSSetSamplers( 0, 1, &clampSampler );

    ID3D11ShaderResourceView* zoomSRV = maskBuffer->GetShaderResView().Get();
    context->CSSetShaderResources( 0, 1, &zoomSRV );
    context->CSSetUnorderedAccessViews( 0, 1, zoomBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );
    context->Dispatch( (ds4Size.x + 7) / 8, (ds4Size.y + 7) / 8, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    ID3D11ShaderResourceView* nullSRV1 = nullptr;
    context->CSSetShaderResources( 0, 1, &nullSRV1 );
    context->CSSetShader( nullptr, nullptr, 0 );

    // Keep the result texture alive until next frame by storing the handle as a member
    m_GodRaysResult = std::move( zoomBuffer );
    *outGodRaysSRV = m_GodRaysResult->GetShaderResView().Get();

    return XR_SUCCESS;
}
