#include "D3D11Upscaling.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "D3D11PFX_FSR1.h"
#include "D3D11PFX_FSR2.h"
#include "D3D11PFX_FSR3.h"
#include "D3D11PFX_TAA.h"
#include "oCGame.h"

namespace {

    void AddFSR1Pass( RenderGraph& graph,
        D3D11GraphicsEngine& engine,
        ID3D11RenderTargetView* outputRTV,
        RGResourceHandle backBufferHandle)
    {
        graph.AddPass( RG_PASS_NAME("FSR 1 Upscale"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [&engine, backBufferHandle, outputRTV]( const RenderGraph& graph ) {
                auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );
                auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

                // Now upscale it to backbuffer with sharpening
                auto sharpenFactor = settings.SharpenFactor;
                engine.GetPfxRenderer()->GetFSR1()->Apply(
                    backbufferTex->GetShaderResView(),
                    outputRTV,
                    engine.GetResolution(),
                    engine.GetBackbufferResolution(),
                    sharpenFactor >= 0.001f,
                    1.0f - sharpenFactor );
                };
            } );

    }

    void AddFSR2Pass( RenderGraph& graph,
        D3D11GraphicsEngine& engine,
        ID3D11RenderTargetView* outputRTV,
        RGResourceHandle backBufferHandle,
        ID3D11ShaderResourceView* depth,
        RGResourceHandle velocityBufferHandle,
        RGResourceHandle reactiveMaskResource )
    {
        graph.AddPass( RG_PASS_NAME("FSR 2"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( velocityBufferHandle );
            // builder.Read( reactiveMaskResource );
            builder.Read( backBufferHandle );

            builder.Write( backBufferHandle );

            pass.m_executeCallback = [&engine, backBufferHandle, outputRTV, velocityBufferHandle, reactiveMaskResource, depth]( const RenderGraph& graph ) {
                auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

                auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );

                auto sharpenFactor = settings.SharpenFactor;

                auto velocityBufferTex = graph.GetPhysicalTexture( velocityBufferHandle );
                auto reactiveMask = graph.GetPhysicalTexture( reactiveMaskResource );

                auto jitter = engine.GetPfxRenderer()->GetTAAEffect()->GetJitterOffsetUnscaled();
                const auto inputSize = engine.GetResolution();

                float fovY, fovX;
                auto cam = ((zCCamera*)oCGame::GetGame()->_zCSession_camera);
                cam->GetFOV( fovY, fovX );

                // With ENABLE_DEPTH_INVERTED | ENABLE_DEPTH_INFINITE flags,
                // FSR expects inverted metrics: cameraNear=FLT_MAX (infinity),
                // cameraFar=actual near plane (since depth is inverted: 1=near, 0=far)
                float nearZ = FLT_MAX;  // Infinity in view space
                float farZ = cam ? cam->GetNearPlane() : 0.01f;
                farZ = std::max( farZ, 0.075f );  // FSR2 validation requires cameraFar >= 0.075f

                ID3D11SamplerState* linearSampler = engine.GetLinearSamplerState();
                engine.GetContext()->CSSetSamplers( 0, 1, &linearSampler );
                engine.GetContext()->CSSetSamplers( 1, 1, &linearSampler );

                ID3D11Buffer* nullCBs[5]{};
                engine.GetContext()->CSSetConstantBuffers( 0, std::size( nullCBs ), nullCBs );

                engine.GetPfxRenderer()->GetFSR2()->Apply(
                    backbufferTex->GetShaderResView().Get(),
                    depth,
                    velocityBufferTex->GetShaderResView().Get(),
                    nullptr, // reactiveMask->GetShaderResView().Get(),
                    outputRTV,
                    inputSize,
                    engine.GetBackbufferResolution(),
                    Engine::GAPI->GetDeltaTime() * 1000.f,
                    jitter,
                    float2( static_cast<float>(inputSize.x), static_cast<float>(inputSize.y) ),
                    false,
                    fovY,
                    nearZ,
                    farZ,
                    sharpenFactor >= 0.001f,
                    sharpenFactor /* FSR2 has 0..1 (sharp)*/ );
                };
            } );
    }

    void AddFSR3Pass( RenderGraph& graph,
        D3D11GraphicsEngine& engine,
        ID3D11RenderTargetView* outputRTV,
        RGResourceHandle backBufferHandle,
        ID3D11ShaderResourceView* depth,
        RGResourceHandle velocityBufferHandle,
        RGResourceHandle reactiveMaskResource )
    {
        graph.AddPass( RG_PASS_NAME("FSR 3"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( velocityBufferHandle );
            // builder.Read( reactiveMaskResource );
            builder.Read( backBufferHandle );

            builder.Write( backBufferHandle );

            pass.m_executeCallback = [&engine, backBufferHandle, outputRTV, velocityBufferHandle, reactiveMaskResource, depth]( const RenderGraph& graph ) {
                auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

                auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );

                auto sharpenFactor = settings.SharpenFactor;

                auto velocityBufferTex = graph.GetPhysicalTexture( velocityBufferHandle );
                auto reactiveMask = graph.GetPhysicalTexture( reactiveMaskResource );

                auto jitter = engine.GetPfxRenderer()->GetTAAEffect()->GetJitterOffsetUnscaled();
                const auto inputSize = engine.GetResolution();

                float fovY, fovX;
                auto cam = ((zCCamera*)oCGame::GetGame()->_zCSession_camera);
                cam->GetFOV( fovY, fovX );

                // With ENABLE_DEPTH_INVERTED | ENABLE_DEPTH_INFINITE flags,
                // FSR expects inverted metrics: cameraNear=FLT_MAX (infinity),
                // cameraFar=actual near plane (since depth is inverted: 1=near, 0=far)
                float nearZ = FLT_MAX;  // Infinity in view space
                float farZ = cam ? cam->GetNearPlane() : 0.01f;
                farZ = std::max( farZ, 0.075f );  // FSR2 validation requires cameraFar >= 0.075f

                ID3D11SamplerState* linearSampler = engine.GetLinearSamplerState();
                engine.GetContext()->CSSetSamplers( 0, 1, &linearSampler );
                engine.GetContext()->CSSetSamplers( 1, 1, &linearSampler );

                ID3D11Buffer* nullCBs[5]{};
                engine.GetContext()->CSSetConstantBuffers( 0, std::size( nullCBs ), nullCBs );

                engine.GetPfxRenderer()->GetFSR3()->Apply(
                    backbufferTex->GetShaderResView().Get(),
                    depth,
                    velocityBufferTex->GetShaderResView().Get(),
                    nullptr, // reactiveMask->GetShaderResView().Get(),
                    outputRTV,
                    inputSize,
                    engine.GetBackbufferResolution(),
                    Engine::GAPI->GetDeltaTime() * 1000.f,
                    jitter,
                    float2( static_cast<float>(inputSize.x), static_cast<float>(inputSize.y) ),
                    false,
                    fovY,
                    nearZ,
                    farZ,
                    sharpenFactor >= 0.001f,
                    sharpenFactor /* FSR3 has 0..1 (sharp)*/ );
                };
        } );
    }
}

void D3D11Upscaling::UpdateUpscaling( D3D11GraphicsEngine& engine )
{
    if ( engine.GetDevice()->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0 ) {
        return;
    }

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    const bool needsJitteredProj = settings.AntiAliasingMode == GothicRendererSettings::AA_FSR
        || settings.AntiAliasingMode == GothicRendererSettings::AA_TAA
        || settings.AntiAliasingMode == GothicRendererSettings::AA_FSR3;

    if ( needsJitteredProj ) {
        engine.SetFrameNeedsJitter();
    }
}

bool D3D11Upscaling::AddUpscalingPass( RenderGraph& graph,
    D3D11GraphicsEngine& engine, 
    ID3D11RenderTargetView* outputRTV,
    RGResourceHandle color, 
    ID3D11ShaderResourceView* depth,
    RGResourceHandle motionVectors,
    RGResourceHandle reactiveMask )
{

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    if ( settings.ResolutionScalePercent < 100
            && settings.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_1 ) {

        AddFSR1Pass( graph, engine, outputRTV, color );
        return true;
    } 
    
    if ( engine.GetDevice()->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0 ) {
        return false;
    }

    if ( settings.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_2
            && (settings.ResolutionScalePercent <= 100)
            && settings.AntiAliasingMode == GothicRendererSettings::AA_FSR ) {
        AddFSR2Pass( graph, engine, outputRTV, color, depth, motionVectors, reactiveMask );
        return true;
    } else if ( settings.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3
            && (settings.ResolutionScalePercent <= 100)
            && settings.AntiAliasingMode == GothicRendererSettings::AA_FSR ) {

        AddFSR3Pass( graph, engine, outputRTV, color, depth, motionVectors, reactiveMask );
        return true;
    }
    return false;
}
