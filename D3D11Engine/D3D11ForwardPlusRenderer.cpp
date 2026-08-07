#include "pch.h"
#include "D3D11ForwardPlusRenderer.h"
#include "D3D11DeferredRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11ShadowMap.h"
#include "D3D11TiledDeferredShading.h"
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
    RGResourceHandle specularResource = RG_INVALID_HANDLE;
    RGResourceHandle reactiveMaskResource = {};
    RGResourceHandle shadowMaskResource = RG_INVALID_HANDLE;
    const bool useScreenSpaceShadowMask = Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseScreenSpaceShadowMask;

    // MSAA (Forward+ only): opaque Z-prepass + lit geometry render into multisampled targets,
    // then get resolved back into the regular single-sample buffers before any other pass runs.
    const bool msaaActive = engine.GetMSAADepthBuffer() != nullptr;

    // --- Depth prepass ---
    graph.AddPass( RG_PASS_NAME("FP Depth Prepass"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [&engine, msaaActive]( const RenderGraph& ) -> void {
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
            auto* dsv = msaaActive ? engine.GetMSAADepthBuffer()->GetDepthStencilView().Get()
                                   : engine.GetDepthBuffer()->GetDepthStencilView().Get();
            context->OMSetRenderTargets( 1, &nullRTV, dsv );

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

    // --- Resolve MSAA depth ---
    // Resolves MSAADepthStencilBuffer into the regular single-sample DepthStencilBuffer so every
    // existing depth consumer below (light culling, shadow mask, AO, godrays, height fog) keeps
    // working unmodified.
    if ( msaaActive ) {
        graph.AddPass( RG_PASS_NAME("FP Resolve MSAA Depth"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [&engine]( const RenderGraph& ) -> void {
                TracyD3D11ZoneCGX( "D3D11ForwardPlusRenderer::Resolve MSAA Depth" );
                engine.ResolveMSAADepth();
            };
        } );
    }

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
                engine.BindDynamicCBToPixelShader( 0, engine.AllocateDynamicCB( &scb, sizeof( scb ) ) );

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

    // --- Screen-space AO producer ---
    // Forward+ has no GBuffer normals before lighting, so optionally reconstruct smooth
    // normals from depth (feeds SAO/ASSAO); otherwise the producers fall back to depth-only.
    // The resulting R8 mask is sampled by the lit-geometry pass and applied to indirect light.
    const auto& aoRs = Engine::GAPI->GetRendererState().RendererSettings;
    const AOMode aoMode = aoRs.AoMode;
    const bool aoUsesNormals = ( aoMode == AOMode::AO_SAO || aoMode == AOMode::AO_ASSAO );
    RGResourceHandle aoNormalsResource = RG_INVALID_HANDLE;
    // MSAA: the lit-geometry pass can't also output a GBuffer normals RTV (bound RTVs/DSV must
    // share sample count), so always fall back to reconstructing normals from the resolved depth.
    if ( msaaActive || (aoRs.DebugSettings.FeatureSet.GenerateAONormalsFromDepth && aoUsesNormals) ) {
        aoNormalsResource = engine.AddAONormalsFromDepthPass( graph );
    }
    RGResourceHandle aoMaskResource = engine.AddAOMaskPass( graph, aoNormalsResource,
        /*depthOnlyNormals*/ ( aoNormalsResource == RG_INVALID_HANDLE ) );

    // --- Forward+ lit geometry pass ---
    graph.AddPass( RG_PASS_NAME("FP Lit Geometry"), [&]( RGBuilder& builder, RenderPass& pass ) {
        auto size = engine.GetResolution();
        if ( !msaaActive ) {
            normalsResource = builder.CreateTexture( { static_cast<uint32_t>( size.x ), static_cast<uint32_t>( size.y ), DXGI_FORMAT_R16G16_FLOAT, L"GBufferNormals" } );
            builder.Write( normalsResource );
        }
        reactiveMaskResource = builder.CreateTexture( { static_cast<uint32_t>( size.x ), static_cast<uint32_t>( size.y ), DXGI_FORMAT_R8_UNORM, L"ReactiveMask" } );
        builder.Write( velocityBufferHandle );
        builder.Write( colorResource );
        builder.Write( backBufferHandle );
        if ( useScreenSpaceShadowMask && shadowMaskResource != RG_INVALID_HANDLE ) {
            builder.Read( shadowMaskResource );
        }
        if ( aoMaskResource != RG_INVALID_HANDLE ) {
            builder.Read( aoMaskResource );
        }

        pass.m_executeCallback = [this, &engine, colorResource, normalsResource, velocityBufferHandle, shadowMaskResource, useScreenSpaceShadowMask, aoMaskResource, msaaActive]( const RenderGraph& graph ) -> void {
            TracyD3D11ZoneCGX( "D3D11ForwardPlusRenderer::Lit Geometry" );
            auto& context = engine.GetContext();
            auto* shadowMaps = engine.GetShadowMaps();

            auto normals = msaaActive ? nullptr : graph.GetPhysicalTexture( normalsResource );
            auto velocityBuffer = msaaActive ? nullptr : graph.GetPhysicalTexture( velocityBufferHandle );
            auto* shadowMask = ( useScreenSpaceShadowMask && shadowMaskResource != RG_INVALID_HANDLE )
                ? graph.GetPhysicalTexture( shadowMaskResource )
                : nullptr;
            const auto aaMode = Engine::GAPI->GetRendererState().RendererSettings.AntiAliasingMode;
            if (!msaaActive && aaMode != GothicRendererSettings::AA_TAA
                && aaMode != GothicRendererSettings::AA_FSR) {
                velocityBuffer = nullptr; // don't write velocity if not needed.
                // NOTE: we should automate this, by putting the velocity
                // buffer creation INTO the rendergraph instead of passing it in via external handle
            }
            ID3D11RenderTargetView* colorRTV = msaaActive
                ? engine.GetMSAAColorBuffer()->GetRenderTargetView().Get()
                : graph.GetPhysicalTexture( colorResource )->GetRenderTargetView().Get();
            ID3D11RenderTargetView* rtvs[] = {
                colorRTV,
                normals ? normals->GetRenderTargetView().Get() : nullptr,
                velocityBuffer ? velocityBuffer->GetRenderTargetView().Get() : nullptr,
            };

            constexpr float black[] { 0.f, 0.f, 0.f, 0.f };
            // skip color target, clear all others.
            for ( size_t i = 1; i < std::size(rtvs); i++ ) {
                if ( rtvs[i] )
                    context->ClearRenderTargetView( rtvs[i], black );
            }

            auto* dsv = msaaActive ? engine.GetMSAADepthBuffer()->GetDepthStencilView().Get()
                                   : engine.GetDepthBuffer()->GetDepthStencilView().Get();
            context->OMSetRenderTargets( std::size( rtvs ), rtvs, dsv );

            // Use LESS_EQUAL depth test to leverage the depth prepass
            auto& depthState = Engine::GAPI->GetRendererState().DepthState;
            depthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_GREATER_EQUAL;
            depthState.DepthWriteEnabled = false;
            depthState.SetDirty();

            // Alpha-tested world mesh/vobs output a sharpened per-pixel coverage value into the
            // color target's alpha channel (see DoAlphaTestCoverage in FFConstantBuffer.h) instead
            // of a hard binary clip when MSAA is active; enabling alpha-to-coverage here lets the
            // hardware dither that coverage across subsamples for an anti-aliased cutout edge.
            // Non-alpha-tested materials always output alpha=1, so this is a no-op for them.
            auto& blendState = Engine::GAPI->GetRendererState().BlendState;
            blendState.AlphaToCoverage = msaaActive;
            blendState.SetDirty();

            // --- Bind sun/CSM constant buffer at b4 ---
            DS_ScreenQuadConstantBuffer scb = shadowMaps->FillSunCSMConstantBuffer();
            engine.BindDynamicCBToPixelShader( 4, engine.AllocateDynamicCB( &scb, sizeof( scb ) ) );

            // --- Bind tile constant buffer at b5 ---
            auto res = engine.GetResolution();
            ForwardPlusTileConstantBuffer tileCB = {};
            tileCB.ViewportSize = float2( static_cast<float>( res.x ), static_cast<float>( res.y ) );
            tileCB.NumTilesX = ( static_cast<uint32_t>( res.x ) + 15 ) / 16;
            tileCB.LimitLightIntensity = Engine::GAPI->GetRendererState().RendererSettings.LimitLightIntesity ? 1u : 0u;
            tileCB.ClusterNearZ = Engine::GAPI->GetNearPlane();
            tileCB.ClusterFarZ = std::max( CLUSTER_MIN_FAR_Z,
                Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius );
            engine.BindDynamicCBToPixelShader( 5, engine.AllocateDynamicCB( &tileCB, sizeof( tileCB ) ) );
             
            // --- Bind CSM shadow map at t3 ---
            shadowMaps->BindToPixelShader( context.Get(), 3 );
            shadowMaps->BindSampler( context.Get(), 2 );

            engine.GetBlueNoiseTexture()->BindToPixelShader( 6 );

            // --- Bind atmosphere cbuffer at b1 ---
            GSky* sky = Engine::GAPI->GetSky();
            auto atmoCB = sky->GetAtmosphereCB();
            engine.BindDynamicCBToPixelShader( 1, engine.AllocateDynamicCB( &atmoCB, sizeof( atmoCB ) ) );

            // --- Bind light SRVs (t8-t11) from tiled deferred ---
            auto* tiledDeferred = shadowMaps->GetTiledDeferred();
            if ( tiledDeferred ) {
                // t10 was the flat light-index list; the clustered grid carries a bitmask instead, so the
                // slot is bound null to drop any stale SRV.
                ID3D11ShaderResourceView* lightSRVs[4] = {
                    tiledDeferred->GetLightBufferSRV(),
                    tiledDeferred->GetLightGridSRV(),
                    nullptr,
                    tiledDeferred->IsShadowArrayCreated() ? tiledDeferred->GetShadowCubeArraySRV() : nullptr,
                };
                context->PSSetShaderResources( 8, 4, lightSRVs );

                // --- Dynamic point-shadow overlay array at t14 (t12/t13 are the shadow and AO masks) ---
                // May be null: no light can carry SHADOW_CUBE_HAS_DYNAMIC before the array exists.
                ID3D11ShaderResourceView* dynCubeSRV = tiledDeferred->IsDynShadowArrayCreated()
                    ? tiledDeferred->GetShadowDynCubeArraySRV() : nullptr;
                context->PSSetShaderResources( 14, 1, &dynCubeSRV );
            }

            // --- Bind shadow mask at t12 ---
            ID3D11ShaderResourceView* shadowMaskSRV = ( useScreenSpaceShadowMask && shadowMask )
                ? shadowMask->GetShaderResView().Get()
                : nullptr;
            context->PSSetShaderResources( 12, 1, &shadowMaskSRV );

            // --- Bind screen-space AO mask at t13 (indirect light only; white = no occlusion) ---
            auto* aoMaskTex = ( aoMaskResource != RG_INVALID_HANDLE )
                ? graph.GetPhysicalTexture( aoMaskResource ) : nullptr;
            ID3D11ShaderResourceView* aoMaskSRV = aoMaskTex ? aoMaskTex->GetShaderResView().Get()
                : engine.GetWhiteTexture()->GetShaderResourceView().Get();
            context->PSSetShaderResources( 13, 1, &aoMaskSRV );

            // Draw all world geometry with Forward+ shaders
            Engine::GAPI->DrawWorldMeshNaive();
            engine.SetViewport( ViewportInfo( 0, 0, engine.GetResolution() ) );

            engine.StoreVobPreviousTransforms();

            // --- Unbind light SRVs, shadow mask and AO mask ---
            context->PSSetShaderResources( 3, 1, s_nullSRVs );
            context->PSSetShaderResources( 6, 1, s_nullSRVs );
            context->PSSetShaderResources( 8, 4, s_nullSRVs );
            context->PSSetShaderResources( 12, 3, s_nullSRVs ); // shadow mask, AO mask, dynamic shadow cubes

            // Restore default depth comparison
            depthState.SetDefault();
            depthState.SetDirty();

            blendState.AlphaToCoverage = false;
            blendState.SetDirty();
        };
    } );

    // --- Resolve MSAA color ---
    // Standard box-filter resolve of the multisampled HDR color into the regular single-sample
    // HDRBackBuffer. Everything downstream (transparent/alpha meshes, particles, sky, post-FX)
    // keeps operating on the single-sample buffer exactly as in the non-MSAA path.
    if ( msaaActive ) {
        graph.AddPass( RG_PASS_NAME("FP Resolve MSAA Color"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [&engine, backBufferHandle]( const RenderGraph& graph ) -> void {
                TracyD3D11ZoneCGX( "D3D11ForwardPlusRenderer::Resolve MSAA Color" );
                auto* backBufferTex = graph.GetPhysicalTexture( backBufferHandle );
                engine.GetContext()->ResolveSubresource(
                    backBufferTex->GetTexture().Get(), 0,
                    engine.GetMSAAColorBuffer()->GetTexture().Get(), 0,
                    engine.GetBackBufferFormat() );
            };
        } );
    }

    outNormalsResource = msaaActive ? aoNormalsResource : normalsResource;
    outSpecularResource = specularResource;
    outReactiveMaskResource = reactiveMaskResource;
}

RGResourceHandle D3D11ForwardPlusRenderer::AddAmbientOcclusionPass(
    RenderGraph& graph,
    D3D11GraphicsEngine& engine,
    RGResourceHandle normalsResource ) {
    // Forward+ produces the AO mask inside AddGeometryPasses (before "FP Lit Geometry"),
    // because no GBuffer normals exist beforehand. Nothing to do at the top level.
    return RG_INVALID_HANDLE;
}

void D3D11ForwardPlusRenderer::AddLightingPasses(
    RenderGraph& graph,
    D3D11GraphicsEngine& engine,
    RGResourceHandle colorResource,
    RGResourceHandle normalsResource,
    RGResourceHandle specularResource,
    RGResourceHandle backBufferHandle,
    RGResourceHandle aoMaskResource,
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
    const bool blendAdd = zMatAlphaFunc == zMAT_ALPHA_FUNC_ADD; 
    const bool blendBlend = zMatAlphaFunc == zMAT_ALPHA_FUNC_BLEND;
    const bool linZ = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;

    if ( materialType == MaterialInfo::MT_Portal ||
         materialType == MaterialInfo::MT_WaterfallFoam ||
         linZ || blendAdd || blendBlend ) {
        // These don't participate in Forward+ lighting — use deferred fallback shaders
        return m_DeferredFallback.BindShaderForTexture( shaderManager, activePS,
            texture, forceAlphaTest, zMatAlphaFunc, materialType,
            resolvedDiffuseNormalmapped, resolvedDiffuseNormalmappedFxMap,
            resolvedDiffuseNormalmappedAlphatest, resolvedDiffuseNormalmappedAlphatestFxMap );
    }

    const auto& active = activePS;
    auto newShader = activePS;
    
    if (texture->GetCacheState() != zRES_CACHED_IN) {
        newShader = shaderManager.GetPShader( PShaderID::PS_Diffuse );
        if ( active != newShader ) {
            activePS = newShader;
            activePS->Apply();
            return true;
        }
        return false;
    }
    
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
