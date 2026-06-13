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
#include "D3D11NVHBAO.h"
#include "D3D11PFX_HDR.h"
#include "D3D11PFX_SMAA.h"
#include "D3D11PFX_GodRays.h"
#include "D3D11PFX_DepthOfField.h"
#include "D3D11PFX_TAA.h"
#include "D3D11PFX_SimpleSharpen.h"
#include "D3D11PFX_CAS.h"
#include "D3D11PFX_FSR1.h"
#include "D3D11PFX_FSR2.h"
#include "D3D11PFX_FSR3.h"
#include "D3D11PFX_SAO.h"
#include "D3D11PFX_ASSAO.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "GSky.h"

D3D11PfxRenderer::D3D11PfxRenderer() {

    auto engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    m_texturePool = std::make_unique<TexturePool>( engine->GetDevice().Get() );
    m_depthStencilPool = std::make_unique<DepthStencilPool>( engine->GetDevice().Get() );

    FX_Blur = std::make_unique<D3D11PFX_Blur>( this );
    FX_HeightFog = std::make_unique<D3D11PFX_HeightFog>( this );
    //FX_DistanceBlur = new D3D11PFX_DistanceBlur(this);
    FX_HDR = std::make_unique<D3D11PFX_HDR>( this );
    FX_GodRays = std::make_unique<D3D11PFX_GodRays>( this );
    FX_DepthOfField = std::make_unique<D3D11PFX_DepthOfField>( this );

    if ( !FeatureLevel10Compatibility ) {
        FX_SMAA = std::make_unique<D3D11PFX_SMAA>( this );
        FX_TAA = std::make_unique<D3D11PFX_TAA>( this );
        NvHBAO = std::make_unique<D3D11NVHBAO>();
        FX_SAO = std::make_unique<D3D11PFX_SAO>( this );
        PFX_FSR1 = std::make_unique<D3D11PFX_FSR1>( this );
        PFX_FSR2 = std::make_unique<D3D11PFX_FSR2>( this );
        PFX_FSR3 = std::make_unique<D3D11PFX_FSR3>( this );
        PFX_ASSAO = std::make_unique<D3D11PFX_ASSAO>(
            engine->GetDevice().Get(),
            engine->GetContext().Get() );

        PFX_ASSAO->Init();
    }

    PFX_CAS = std::make_unique<D3D11PFX_CAS>( this );
    PFX_SimpleSharpen = std::make_unique<D3D11PFX_SimpleSharpen>( this );
}

D3D11PfxRenderer::~D3D11PfxRenderer() {
    //delete FX_DistanceBlur;
}

/** Renders the distance blur effect */
XRESULT D3D11PfxRenderer::RenderDistanceBlur(ID3D11ShaderResourceView* diffuse ) {
    FX_DistanceBlur->Render( diffuse );
    return XR_SUCCESS;
}

/** Blurs the given texture */
XRESULT D3D11PfxRenderer::BlurTexture( RenderToTextureBuffer* texture, bool leaveResultInD4_2, float scale, const XMFLOAT4& colorMod, PShaderID finalCopyShader ) {
    FX_Blur->RenderBlur( texture, leaveResultInD4_2, 0.0f, scale, colorMod, finalCopyShader );
    return XR_SUCCESS;
}

/** Renders the heightfog */
XRESULT D3D11PfxRenderer::RenderHeightfog() {
    return FX_HeightFog->Render( nullptr );
}

/** Renders the godrays-Effect */
XRESULT D3D11PfxRenderer::RenderGodRays(ID3D11ShaderResourceView* backbuffer, ID3D11ShaderResourceView* depth) {
    return FX_GodRays->Render( backbuffer , depth );
}

/** Renders the depth-of-field effect */
XRESULT D3D11PfxRenderer::RenderDepthOfField(ID3D11ShaderResourceView* backbuffer) {
    return FX_DepthOfField->Render( backbuffer );
}

/** Renders the HDR-Effect */
XRESULT D3D11PfxRenderer::RenderHDR( ID3D11RenderTargetView* output, ID3D11ShaderResourceView* backbuffer ) {
    return FX_HDR->Render( output, backbuffer );
}

/** Renders the SMAA-Effect */
XRESULT D3D11PfxRenderer::RenderSMAA(ID3D11ShaderResourceView* backbuffer) {
    FX_SMAA->RenderPostFX( backbuffer );
    return XR_SUCCESS;
}

/** Renders the TAA-Effect */
XRESULT D3D11PfxRenderer::RenderTAA(const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& velocityBuffer) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();

    // WICHTIG: Depth-Buffer als DSV entbinden, damit er als SRV gelesen werden kann!
    // Speichere aktuellen RTV, setze DSV auf nullptr
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> currentRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> currentDSV;
    context->OMGetRenderTargets( 1, currentRTV.GetAddressOf(), currentDSV.GetAddressOf() );
    context->OMSetRenderTargets( 1, currentRTV.GetAddressOf(), nullptr );  // DSV = nullptr!

    // First, generate the velocity buffer from depth
    if (!velocityBuffer.Get()) {
        FX_TAA->RenderVelocityBuffer(engine->GetDepthBuffer()->GetShaderResView());
    }
    
    // Then render TAA using the velocity buffer
    FX_TAA->RenderPostFX(
        engine->GetHDRBackBuffer().GetShaderResView(),
        engine->GetDepthBuffer()->GetShaderResView(),
        velocityBuffer.Get() ? velocityBuffer : FX_TAA->GetVelocityBufferSRV()
    );

    // Stelle den DSV wieder her falls nötig
    context->OMSetRenderTargets(1, currentRTV.GetAddressOf(), currentDSV.Get());
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

XRESULT D3D11PfxRenderer::RenderSimpleSharpen( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& source, INT2 sourceSize, RenderToTextureBuffer* dest, INT2 destSize ) {
    PFX_SimpleSharpen->Apply( source, sourceSize, dest, destSize );
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
        auto simplePS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Simple );
        simplePS->Apply();
    }

    engine->GetShaderManager().GetVShader( VShaderID::VS_PFX )->Apply();

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

    m_texturePool->Clear(); // textures will be created on demand
    if ( !FeatureLevel10Compatibility ) {
        FX_SMAA->OnResize( newResolution );
        FX_TAA->OnResize( newResolution );
    }

    return XR_SUCCESS;
}

/** Draws the HBAO-Effect to the given buffer */
XRESULT D3D11PfxRenderer::DrawHBAO(
    const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv,
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& pFullResDepthTexSRV,
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& pFullResNormalTexSRV) {
    return NvHBAO->Render( rtv.Get(), pFullResDepthTexSRV, pFullResNormalTexSRV);
}

/** Renders the SAO effect */
XRESULT D3D11PfxRenderer::RenderSAO(
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalsSRV,
    ID3D11RenderTargetView* outputRTV ) {
    if ( !FX_SAO ) return XR_FAILED;
    return FX_SAO->Render( depthSRV, normalsSRV, outputRTV );
}

XRESULT D3D11PfxRenderer::RenderSAOCompute(
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalsSRV ) {
    if ( !FX_SAO ) return XR_FAILED;
    return FX_SAO->RenderAO( depthSRV, normalsSRV );
}

ID3D11ShaderResourceView* D3D11PfxRenderer::GetSAOResultSRV() const {
    return FX_SAO ? FX_SAO->GetAOResultSRV() : nullptr;
}

XRESULT D3D11PfxRenderer::RenderGodRaysToTexture(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView** outGodRaysSRV ) {
    return FX_GodRays->RenderToTexture( backbuffer, depthCopy, outGodRaysSRV );
}

XRESULT D3D11PfxRenderer::RenderPostFXComposition(
    ID3D11RenderTargetView* outputRTV,
    ID3D11ShaderResourceView* backbufferSRV,
    ID3D11ShaderResourceView* saoSRV,
    ID3D11ShaderResourceView* godraysSRV,
    ID3D11ShaderResourceView* depthSRV ) {

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    auto res = engine->GetResolution();

    // Set up shaders
    engine->GetShaderManager().GetVShader( VShaderID::VS_PFX )->Apply();
    auto compositionPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Composition );
    compositionPS->Apply();

    // Update constant buffers for inline heightfog if active
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( settings.DrawFog ) {
        HeightfogConstantBuffer cb;
        {
            auto& proj = Engine::GAPI->GetProjectionMatrix();
            cb.HF_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
        }
        XMStoreFloat4x4( &cb.InvView, XMMatrixInverse( nullptr, Engine::GAPI->GetViewMatrixXM() ) );
        cb.CameraPosition = Engine::GAPI->GetCameraPosition();
        cb.HF_GlobalDensity = settings.FogGlobalDensity;
        cb.HF_HeightFalloff = settings.FogHeightFalloff;

        float height = settings.FogHeight;
        XMVECTOR color = XMLoadFloat3( settings.FogColorMod.toXMFLOAT3() );

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
        float fogDensityFactorRain = (1.0f - Engine::GAPI->GetFogOverride());
#else
        float fogDensityFactor = pow( 15000.0f / Engine::GAPI->GetFarZ(), 4.0f );
        float fogDensityFactorRain = 1.0f;
#endif

        if ( Engine::GAPI->GetFogOverride() > 0.0f ) {
            height = Toolbox::lerp( height, Engine::GAPI->GetCameraPosition().y + 10000, Engine::GAPI->GetFogOverride() );
            color = Engine::GAPI->GetFogColor();
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
            cb.HF_HeightFalloff = Toolbox::lerp( cb.HF_HeightFalloff, 0.000001f, Engine::GAPI->GetFogOverride() );
#endif
            cb.HF_GlobalDensity = Toolbox::lerp( cb.HF_GlobalDensity, cb.HF_GlobalDensity * fogDensityFactor, Engine::GAPI->GetFogOverride() );
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
            cb.HF_WeightZNear = Toolbox::lerp( cb.HF_WeightZNear, WORLD_SECTION_SIZE * 0.09f, Engine::GAPI->GetFogOverride() );
            cb.HF_WeightZFar = Toolbox::lerp( cb.HF_WeightZFar, WORLD_SECTION_SIZE * 0.8, Engine::GAPI->GetFogOverride() );
#endif
        }

        cb.HF_FogHeight = height;
        cb.HF_ProjAB = float2( Engine::GAPI->GetProjectionMatrix()._33, Engine::GAPI->GetProjectionMatrix()._34 );

        float rain = Engine::GAPI->GetRainFXWeight();
        XMFLOAT3 FogColorMod;
        XMStoreFloat3( &FogColorMod, XMVectorLerpV( color, XMLoadFloat3( &settings.RainFogColor ), XMVectorSet( std::min( 1.0f, rain * 2.0f ), std::min( 1.0f, rain * 2.0f ), std::min( 1.0f, rain * 2.0f ), 0 ) ) );
        cb.HF_FogColorMod = FogColorMod;
        cb.HF_GlobalDensity = Toolbox::lerp( cb.HF_GlobalDensity, settings.RainFogDensity, rain * fogDensityFactorRain );

        compositionPS->GetBuffer( "PFXBuffer" ).Update( &cb ).Bind();

        GSky* sky = Engine::GAPI->GetSky();
        compositionPS->GetBuffer( "Atmosphere" ).Update( &sky->GetAtmosphereCB() ).Bind();
    }

    // Set viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(res.x);
    vp.Height = static_cast<float>(res.y);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports( 1, &vp );

    // Bind output RTV (no depth)
    context->OMSetRenderTargets( 1, &outputRTV, nullptr );

    // Bind SRVs: t0=backbuffer, t1=SAO, t2=GodRays, t3=Depth
    ID3D11ShaderResourceView* srvs[4] = { backbufferSRV, saoSRV, godraysSRV, depthSRV };
    context->PSSetShaderResources( 0, 4, srvs );

    // No blending — direct overwrite
    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    DrawFullScreenQuad();

    // Unbind SRVs
    ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
    context->PSSetShaderResources( 0, 4, nullSRVs );

    // Restore default states
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::DEFAULT_DEPTH_COMP_STATE;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = true;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    return XR_SUCCESS;
}

XRESULT D3D11PfxRenderer::RenderASSAO( ID3D11RenderTargetView* outputRTV, ID3D11ShaderResourceView* depthCopy, ID3D11ShaderResourceView* normals )
{
    if ( !PFX_ASSAO ) {
        return XR_FAILED;
    }

    PFX_ASSAO->Render( depthCopy, normals, outputRTV );
    return XR_SUCCESS;
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

    return m_texturePool->Acquire( TexturePool::Description{ res.x, res.y, DXGI_FORMAT_ENGINE_SWAPCHAIN  } );
}

TextureHandle D3D11PfxRenderer::GetTempBufferDS4()
{
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    DXGI_FORMAT bbufferFormat = engine->GetBackBufferFormat(); // actually intermediate backbuffer format -> HDRBackbuffer
    auto res = engine->GetResolution();

    return m_texturePool->Acquire( TexturePool::Description{ res.x / 4, res.y / 4, bbufferFormat } );
}

void D3D11PfxRenderer::FreeResources()
{
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( this->FX_SMAA 
        && settings.AntiAliasingMode != GothicRendererSettings::AA_SMAA ) {
        this->FX_SMAA->ReleaseResources();
    }
    if ( this->FX_TAA 
        && settings.AntiAliasingMode != GothicRendererSettings::AA_TAA ) {
        this->FX_TAA->ReleaseResources();
    }

    if ( this->PFX_FSR2
        && settings.AntiAliasingMode != GothicRendererSettings::AA_FSR
        && !(settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_2) && settings.ResolutionScalePercent < 100) {
        this->PFX_FSR2->ReleaseResources();
    }

    if ( this->PFX_FSR1
        && !(settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_1) && settings.ResolutionScalePercent < 100 ) {
        this->PFX_FSR1->ReleaseResources();
    }

    if ( this->NvHBAO 
        && settings.AoMode != AOMode::AO_HBAO ) {
        this->NvHBAO->ReleaseResources();
    }
}
