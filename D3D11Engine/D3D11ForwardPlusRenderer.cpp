#include "pch.h"
#include "D3D11ForwardPlusRenderer.h"
#include "D3D11DeferredRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11ShadowMap.h"
#include "D3D11TiledDeferredShading.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11Effect.h"
#include "D3D11PfxRenderer.h"
#include "RenderGraph.h"
#include "RenderToTextureBuffer.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "GothicGraphicsState.h"
#include "ConstantBufferStructs.h"
#include "GSky.h"
#include "zCTexture.h"
#include "zCMaterial.h"

static ID3D11ShaderResourceView* s_nullSRVs[16] = { nullptr };

D3D11ForwardPlusRenderer::D3D11ForwardPlusRenderer( D3D11DeferredRenderer& deferredFallback )
    : m_DeferredFallback( deferredFallback ) {
}

void D3D11ForwardPlusRenderer::AddGeometryPasses(
    RenderGraph& graph,
    D3D11GraphicsEngine& engine,
    RGResourceHandle colorResource,
    RGResourceHandle velocityBufferHandle,
    RGResourceHandle backBufferHandle,
    RGResourceHandle& outNormalsResource,
    RGResourceHandle& outSpecularResource,
    RGResourceHandle& outReactiveMaskResource ) {

    RGResourceHandle normalsResource = {};
    RGResourceHandle specularResource = {};
    RGResourceHandle reactiveMaskResource = {};
    RGResourceHandle shadowMaskResource = RG_INVALID_HANDLE;
    const bool useScreenSpaceShadowMask = Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseScreenSpaceShadowMask;

    // --- Depth prepass ---
    graph.AddPass( RG_PASS_NAME("FP Depth Prepass"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [&engine]( const RenderGraph& ) -> void {
            TracyD3D11ZoneCGX( "D3D11ForwardPlusRenderer::Depth Prepass" );
            auto& context = engine.GetContext();

            // Clear all SRVs to avoid resource hazards
            context->VSSetShaderResources( 0, 8, s_nullSRVs );
            context->PSSetShaderResources( 0, 8, s_nullSRVs );
            context->DSSetShaderResources( 0, 8, s_nullSRVs );
            context->HSSetShaderResources( 0, 8, s_nullSRVs );
            context->CSSetShaderResources( 0, 8, s_nullSRVs );

            // Bind only DSV (no color targets) — pure depth fill
            ID3D11RenderTargetView* nullRTV = nullptr;
            context->OMSetRenderTargets( 1, &nullRTV,
                engine.GetDepthBuffer()->GetDepthStencilView().Get() );

            // Disable pixel shader for depth-only rendering
            context->PSSetShader( nullptr, nullptr, 0 );

            engine.SetRenderingStage( D3D11ENGINE_RENDER_STAGE::DES_Z_PRE_PASS );

            // Draw all world geometry (depth only)
            Engine::GAPI->DrawWorldMeshNaive();

            // Restore viewport (DrawWorldMeshNaive may change it)
            engine.SetViewport( ViewportInfo( 0, 0, engine.GetResolution() ) );
            engine.SetRenderingStage( D3D11ENGINE_RENDER_STAGE::DES_MAIN );
        };
    } );

    // --- Light culling ---
    graph.AddPass( RG_PASS_NAME("FP Light Culling"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [&engine]( const RenderGraph& ) -> void {
            TracyD3D11ZoneCGX( "D3D11ForwardPlusRenderer::Light Culling" );
            // CopyDepthStencil so depth can be read as SRV
            engine.CopyDepthStencil();

            auto* tiledDeferred = engine.GetShadowMaps()->GetTiledDeferred();
            if ( tiledDeferred ) {
                tiledDeferred->CullLights(
                    engine.GetFrameLights(), *engine.GetDepthBufferCopy() );
            }
        };
    } );

    // --- Shadow map rendering ---
    graph.AddPass( RG_PASS_NAME("FP Shadow Maps"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [&engine]( const RenderGraph& ) -> void {
            TracyD3D11ZoneCGX( "D3D11ForwardPlusRenderer::Shadow Maps" );
            auto* shadowMaps = engine.GetShadowMaps();
            engine.SetDefaultStates();
            const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

            shadowMaps->DrawPointlightShadows( engine.GetFrameLights() );
            if ( settings.EnableShadows ) {
                shadowMaps->DrawWorldShadow();
            }
            engine.SetDefaultStates();
            shadowMaps->DrawRainShadowmap();

            Engine::GAPI->SetFarPlane( static_cast<float>( settings.SectionDrawRadius ) * WORLD_SECTION_SIZE );
        };
    } );

    // --- Screen-space shadow mask ---
    // Runs a fullscreen pass reading the Z-prepass depth to compute the CSM shadow
    // value per pixel.  PS_Diffuse.hlsl (Forward+ branch) samples this at t12 instead
    // of running ComputeCascadedShadowValueSoft inline, saving one CSM evaluation per
    // visible fragment.
    if ( useScreenSpaceShadowMask ) {
        graph.AddPass( RG_PASS_NAME("FP Shadow Mask"), [&]( RGBuilder& builder, RenderPass& pass ) {
            auto size = engine.GetResolution();
            shadowMaskResource = builder.CreateTexture( {
                static_cast<uint32_t>( size.x ), static_cast<uint32_t>( size.y ),
                DXGI_FORMAT_R8_UNORM, L"ShadowMask" } );
            builder.Write( shadowMaskResource );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, &engine, shadowMaskResource]( const RenderGraph& graph ) -> void {
                TracyD3D11ZoneCGX( "D3D11ForwardPlusRenderer::ShadowMask" );
                auto& context = engine.GetContext();
                auto* shadowMaps = engine.GetShadowMaps();

                ID3D11RenderTargetView* nullRTV = nullptr;
                context->OMSetRenderTargets( 1, &nullRTV, nullptr ); 

                // Fill and bind the sun/CSM constant buffer at b0
                DS_ScreenQuadConstantBuffer scb = shadowMaps->FillSunCSMConstantBuffer();
                if ( !m_SunCSMConstantBuffer ) {
                    m_SunCSMConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
                        sizeof( DS_ScreenQuadConstantBuffer ), &scb );
                } else {
                    m_SunCSMConstantBuffer->UpdateBuffer( &scb );
                }
                m_SunCSMConstantBuffer->BindToPixelShader( 0 );

                // Bind depth copy as SRV at t2 (filled by the "FP Light Culling" pass)
                auto* depthCopy = engine.GetDepthBufferCopy();
                ID3D11ShaderResourceView* depthSRV = depthCopy ? depthCopy->GetShaderResView().Get() : nullptr;
                context->PSSetShaderResources( 2, 1, &depthSRV );

                // Bind CSM shadow map at t3 and comparison sampler at s2
                shadowMaps->BindToPixelShader( context.Get(), 3 );
                shadowMaps->BindSampler( context.Get(), 2 );

                engine.GetBlueNoiseTexture()->BindToPixelShader( 8 );

                // Bind shadow mask RTV and clear to default (fully lit, sky depth)
                auto* shadowMaskTex = graph.GetPhysicalTexture( shadowMaskResource );
                ID3D11RenderTargetView* maskRTV = shadowMaskTex ? shadowMaskTex->GetRenderTargetView().Get() : nullptr;
                constexpr float defaultMask[] { 1.f, 0.f, 0.f, 0.f };
                if ( maskRTV ) context->ClearRenderTargetView( maskRTV, defaultMask );
                context->OMSetRenderTargets( 1, &maskRTV, nullptr );

                // Draw fullscreen triangle with the shadow mask shader
                engine.SetActiveVertexShader( VShaderID::VS_PFX );
                engine.BindActiveVertexShader();

                engine.SetActivePixelShader( PShaderID::PS_FP_ShadowMask );
                engine.BindActivePixelShader();

                engine.UpdateRenderStates();
                engine.GetPfxRenderer()->DrawFullScreenQuad();

                // Unbind RTVs and SRVs
                context->OMSetRenderTargets( 1, &nullRTV, nullptr );
                context->PSSetShaderResources( 2, 1, s_nullSRVs );
                context->PSSetShaderResources( 3, 1, s_nullSRVs );
                context->PSSetShaderResources( 8, 1, s_nullSRVs );
            };
        } );
    }

    // --- Forward+ lit geometry pass ---
    graph.AddPass( RG_PASS_NAME("FP Lit Geometry"), [&]( RGBuilder& builder, RenderPass& pass ) {
        auto size = engine.GetResolution();
        normalsResource = builder.CreateTexture( { static_cast<uint32_t>( size.x ), static_cast<uint32_t>( size.y ), DXGI_FORMAT_R16G16_FLOAT, L"GBufferNormals" } );
        specularResource = builder.CreateTexture( { static_cast<uint32_t>( size.x ), static_cast<uint32_t>( size.y ), DXGI_FORMAT_R16G16_FLOAT, L"GBufferSpecular" } );
        reactiveMaskResource = builder.CreateTexture( { static_cast<uint32_t>( size.x ), static_cast<uint32_t>( size.y ), DXGI_FORMAT_R8_UNORM, L"ReactiveMask" } );
        builder.Write( reactiveMaskResource );
        builder.Write( velocityBufferHandle );
        builder.Write( colorResource );
        builder.Write( normalsResource );
        builder.Write( specularResource );
        builder.Write( backBufferHandle );
        if ( useScreenSpaceShadowMask && shadowMaskResource != RG_INVALID_HANDLE ) {
            builder.Read( shadowMaskResource );
        }

        pass.m_executeCallback = [this, &engine, colorResource, normalsResource, specularResource, reactiveMaskResource, velocityBufferHandle, shadowMaskResource, useScreenSpaceShadowMask]( const RenderGraph& graph ) -> void {
            TracyD3D11ZoneCGX( "D3D11ForwardPlusRenderer::Lit Geometry" );
            auto& context = engine.GetContext();
            auto* shadowMaps = engine.GetShadowMaps();

            auto normals = graph.GetPhysicalTexture( normalsResource );
            auto specular = graph.GetPhysicalTexture( specularResource );
            auto reactiveMask = graph.GetPhysicalTexture( reactiveMaskResource );
            auto velocityBuffer = graph.GetPhysicalTexture( velocityBufferHandle );
            auto* shadowMask = ( useScreenSpaceShadowMask && shadowMaskResource != RG_INVALID_HANDLE )
                ? graph.GetPhysicalTexture( shadowMaskResource )
                : nullptr;
            const auto aaMode = Engine::GAPI->GetRendererState().RendererSettings.AntiAliasingMode;
            if (aaMode != GothicRendererSettings::AA_TAA
                && aaMode != GothicRendererSettings::AA_FSR
                && aaMode != GothicRendererSettings::AA_FSR3) {
                velocityBuffer = nullptr; // don't write velocity if not needed.
                // NOTE: we should automate this, by putting the velocity 
                // buffer creation INTO the rendergraph instead of passing it in via external handle
            }
            ID3D11RenderTargetView* rtvs[] = {
                graph.GetPhysicalTexture( colorResource )->GetRenderTargetView().Get(),
                normals ? normals->GetRenderTargetView().Get() : nullptr,
                specular ? specular->GetRenderTargetView().Get() : nullptr,
                velocityBuffer ? velocityBuffer->GetRenderTargetView().Get() : nullptr,
                reactiveMask ? reactiveMask->GetRenderTargetView().Get() : nullptr,
            };

            constexpr float black[] { 0.f, 0.f, 0.f, 0.f };
            // skip color target, clear all others.
            for ( size_t i = 1; i < std::size(rtvs); i++ ) {
                if ( rtvs[i] )
                    context->ClearRenderTargetView( rtvs[i], black );
            }

            context->OMSetRenderTargets( 5, rtvs, engine.GetDepthBuffer()->GetDepthStencilView().Get() );

            // Use LESS_EQUAL depth test to leverage the depth prepass
            auto& depthState = Engine::GAPI->GetRendererState().DepthState;
            depthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_GREATER_EQUAL;
            depthState.DepthWriteEnabled = false;
            depthState.SetDirty();

            // --- Bind sun/CSM constant buffer at b4 ---
            DS_ScreenQuadConstantBuffer scb = shadowMaps->FillSunCSMConstantBuffer();
            if ( !m_SunCSMConstantBuffer ) {
                m_SunCSMConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
                    sizeof( DS_ScreenQuadConstantBuffer ), &scb );
            } else {
                m_SunCSMConstantBuffer->UpdateBuffer( &scb );
            }
            m_SunCSMConstantBuffer->BindToPixelShader( 4 );

            // --- Bind tile constant buffer at b5 ---
            auto res = engine.GetResolution();
            ForwardPlusTileConstantBuffer tileCB = {};
            tileCB.ViewportSize = float2( static_cast<float>( res.x ), static_cast<float>( res.y ) );
            tileCB.NumTilesX = ( static_cast<uint32_t>( res.x ) + 15 ) / 16;
            tileCB.LimitLightIntensity = Engine::GAPI->GetRendererState().RendererSettings.LimitLightIntesity ? 1u : 0u;
            if ( !m_TileConstantBuffer ) {
                m_TileConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
                    sizeof( ForwardPlusTileConstantBuffer ), &tileCB );
            } else {
                m_TileConstantBuffer->UpdateBuffer( &tileCB );
            }
            m_TileConstantBuffer->BindToPixelShader( 5 );
             
            // --- Bind CSM shadow map at t3 ---
            shadowMaps->BindToPixelShader( context.Get(), 3 );
            shadowMaps->BindSampler( context.Get(), 2 );

            engine.GetBlueNoiseTexture()->BindToPixelShader( 6 );

            // --- Bind atmosphere cbuffer at b1 ---
            GSky* sky = Engine::GAPI->GetSky();
            auto atmoCB = sky->GetAtmosphereCB();
            static std::unique_ptr<D3D11ConstantBuffer> s_atmoCB;
            if ( !s_atmoCB ) {
                s_atmoCB = std::make_unique<D3D11ConstantBuffer>( sizeof( atmoCB ), &atmoCB );
            } else {
                s_atmoCB->UpdateBuffer( &atmoCB );
            }
            s_atmoCB->BindToPixelShader( 1 );

            // --- Bind light SRVs (t8-t11) from tiled deferred ---
            auto* tiledDeferred = shadowMaps->GetTiledDeferred();
            if ( tiledDeferred ) {
                ID3D11ShaderResourceView* lightSRVs[4] = {
                    tiledDeferred->GetLightBufferSRV(),
                    tiledDeferred->GetLightGridSRV(),
                    tiledDeferred->GetLightIndexListSRV(),
                    tiledDeferred->IsShadowArrayCreated() ? tiledDeferred->GetShadowCubeArraySRV() : nullptr,
                };
                context->PSSetShaderResources( 8, 4, lightSRVs );
            }

            // --- Bind shadow mask at t12 ---
            ID3D11ShaderResourceView* shadowMaskSRV = ( useScreenSpaceShadowMask && shadowMask )
                ? shadowMask->GetShaderResView().Get()
                : nullptr;
            context->PSSetShaderResources( 12, 1, &shadowMaskSRV );

            // Draw all world geometry with Forward+ shaders
            Engine::GAPI->DrawWorldMeshNaive();
            engine.SetViewport( ViewportInfo( 0, 0, engine.GetResolution() ) );

            engine.StoreVobPreviousTransforms();

            // --- Unbind light SRVs and shadow mask ---
            context->PSSetShaderResources( 3, 1, s_nullSRVs );
            context->PSSetShaderResources( 6, 1, s_nullSRVs );
            context->PSSetShaderResources( 8, 4, s_nullSRVs );
            context->PSSetShaderResources( 12, 1, s_nullSRVs );

            // Restore default depth comparison
            depthState.SetDefault();
            depthState.SetDirty();
        };
    } );

    outNormalsResource = normalsResource;
    outSpecularResource = specularResource;
    outReactiveMaskResource = reactiveMaskResource;
}

void D3D11ForwardPlusRenderer::AddLightingPasses(
    RenderGraph& graph,
    D3D11GraphicsEngine& engine,
    RGResourceHandle colorResource,
    RGResourceHandle normalsResource,
    RGResourceHandle specularResource,
    RGResourceHandle backBufferHandle,
    std::vector<VobLightInfo*>& frameLights ) {
    // Forward+ performs lighting in the geometry pass — no separate lighting pass needed.
    // Point light shadows are rendered in the "FP Shadow Maps" pass.
    // Deferred point light accumulation is not used.
}

bool D3D11ForwardPlusRenderer::BindShaderForTexture(
    D3D11ShaderManager& shaderManager,
    std::shared_ptr<D3D11PShader>& activePS,
    zCTexture* texture,
    bool forceAlphaTest,
    int zMatAlphaFunc,
    MaterialInfo::EMaterialType materialType,
    PShaderID resolvedDiffuseNormalmapped,
    PShaderID resolvedDiffuseNormalmappedFxMap,
    PShaderID resolvedDiffuseNormalmappedAlphatest,
    PShaderID resolvedDiffuseNormalmappedAlphatestFxMap ) {

    // Special material types fall through to deferred (non-lit) shaders
    bool blendAdd = zMatAlphaFunc == zMAT_ALPHA_FUNC_ADD; 
    bool blendBlend = zMatAlphaFunc == zMAT_ALPHA_FUNC_BLEND;
    bool linZ = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;

    if ( materialType == MaterialInfo::MT_Portal ||
         materialType == MaterialInfo::MT_WaterfallFoam ||
         linZ || blendAdd || blendBlend ) {
        // These don't participate in Forward+ lighting — use deferred fallback shaders
        return m_DeferredFallback.BindShaderForTexture( shaderManager, activePS,
            texture, forceAlphaTest, zMatAlphaFunc, materialType,
            resolvedDiffuseNormalmapped, resolvedDiffuseNormalmappedFxMap,
            resolvedDiffuseNormalmappedAlphatest, resolvedDiffuseNormalmappedAlphatestFxMap );
    }

    auto active = activePS;
    auto newShader = activePS;

    if ( texture->HasAlphaChannel() || forceAlphaTest ) {
        if ( texture->GetSurface()->GetFxMap() ) {
            newShader = shaderManager.GetPShader( PShaderID::PS_FP_DiffuseNormalmappedAlphaTestFxMap );
        } else if ( texture->GetSurface()->GetNormalmap() || Engine::GAPI->GetSceneWetness() > 1e-6 ) {
            newShader = shaderManager.GetPShader( PShaderID::PS_FP_DiffuseNormalmappedAlphaTest );
        } else {
            newShader = shaderManager.GetPShader( PShaderID::PS_FP_DiffuseAlphaTest );
        }
    } else {
        if ( texture->GetSurface()->GetFxMap() ) {
            newShader = shaderManager.GetPShader( PShaderID::PS_FP_DiffuseNormalmappedFxMap );
        } else if ( texture->GetSurface()->GetNormalmap() || Engine::GAPI->GetSceneWetness() > 1e-6 ) {
            newShader = shaderManager.GetPShader( PShaderID::PS_FP_DiffuseNormalmapped );
        } else {
            newShader = shaderManager.GetPShader( PShaderID::PS_FP_Diffuse );
        }
    }

    // When normalmaps are disabled, fall back to non-normalmap variants
    if ( !Engine::GAPI->GetRendererState().RendererSettings.AllowNormalmaps ) {
        if ( texture->HasAlphaChannel() || forceAlphaTest ) {
            newShader = shaderManager.GetPShader( PShaderID::PS_FP_DiffuseAlphaTest );
        } else {
            newShader = shaderManager.GetPShader( PShaderID::PS_FP_Diffuse );
        }
    }

    if ( active != newShader ) {
        activePS = newShader;
        activePS->Apply();
        return true;
    }
    return false;
}
