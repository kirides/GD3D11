#include "pch.h"
#include "D3D11DeferredRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11ShaderManager.h"
#include "D3D11ShadowMap.h"
#include "D3D11PShader.h"
#include "RenderGraph.h"
#include "RenderToTextureBuffer.h"
#include "zCTexture.h"
#include "zCMaterial.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "GothicGraphicsState.h"

static ID3D11ShaderResourceView* s_nullSRVs[16] = { nullptr };

void D3D11DeferredRenderer::AddGeometryPasses( RenderGraph& graph,
    D3D11GraphicsEngine& engine,
    RGResourceHandle colorResource,
    RGResourceHandle velocityBufferHandle,
    RGResourceHandle backBufferHandle,
    RGResourceHandle& outNormalsResource,
    RGResourceHandle& outSpecularResource,
    RGResourceHandle& outReactiveMaskResource ) {

    RGResourceHandle normalsResource;
    RGResourceHandle specularResource;
    RGResourceHandle reactiveMaskResource;

    graph.AddPass( RG_PASS_NAME("G-Buffer Pass"), [&, colorResource, velocityBufferHandle, backBufferHandle]( RGBuilder& builder, RenderPass& pass ) {
        auto size = engine.GetResolution();
        normalsResource = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R16G16_FLOAT, L"GBufferNormals" } );
        specularResource = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R16G16_FLOAT, L"GBufferSpecular" } );
        reactiveMaskResource = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R8_UNORM, L"ReactiveMask" } );

        builder.Write( colorResource );
        builder.Write( normalsResource );
        builder.Write( specularResource );
        builder.Write( velocityBufferHandle );
        builder.Write( reactiveMaskResource );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [&engine, colorResource, normalsResource, specularResource, reactiveMaskResource, velocityBufferHandle]( const RenderGraph& graph ) -> void {
            TracyD3D11ZoneCGX( "D3D11DeferredRenderer::G-Buffer Pass" );
            const auto& context = engine.GetContext();
            context->VSSetShaderResources( 0, 8, s_nullSRVs );
            context->PSSetShaderResources( 0, 8, s_nullSRVs );
            context->DSSetShaderResources( 0, 8, s_nullSRVs );
            context->HSSetShaderResources( 0, 8, s_nullSRVs );
            context->CSSetShaderResources( 0, 8, s_nullSRVs );

            auto normals = graph.GetPhysicalTexture( normalsResource );
            auto specular = graph.GetPhysicalTexture( specularResource );
            auto reactiveMask = graph.GetPhysicalTexture( reactiveMaskResource );
            auto velocityBuffer = graph.GetPhysicalTexture( velocityBufferHandle );

            const auto aaMode = Engine::GAPI->GetRendererState().RendererSettings.AntiAliasingMode;
            if ( aaMode != GothicRendererSettings::AA_TAA
                && aaMode != GothicRendererSettings::AA_FSR
                && aaMode != GothicRendererSettings::AA_FSR3 ) {
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
            for ( size_t i = 1; i < std::size( rtvs ); i++ ) {
                if ( rtvs[i] )
                    context->ClearRenderTargetView( rtvs[i], black );
            }
            context->OMSetRenderTargets( 5, rtvs, engine.GetDepthBuffer()->GetDepthStencilView().Get() );

            Engine::GAPI->DrawWorldMeshNaive();

            engine.SetViewport( ViewportInfo( 0, 0, engine.GetResolution() ) );

            engine.StoreVobPreviousTransforms();
        };
    } );

    outNormalsResource = normalsResource;
    outSpecularResource = specularResource;
    outReactiveMaskResource = reactiveMaskResource;
}

void D3D11DeferredRenderer::AddLightingPasses( RenderGraph& graph,
    D3D11GraphicsEngine& engine,
    RGResourceHandle colorResource,
    RGResourceHandle normalsResource,
    RGResourceHandle specularResource,
    RGResourceHandle backBufferHandle,
    std::vector<VobLightInfo*>& frameLights ) {

    graph.AddPass( RG_PASS_NAME("Draw Lighting"), [&, colorResource, normalsResource, specularResource, backBufferHandle]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( colorResource );
        builder.Read( normalsResource );
        builder.Read( specularResource );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [&engine, &frameLights, colorResource, normalsResource, specularResource]( const RenderGraph& graph ) -> void {
            TracyD3D11ZoneCGX( "D3D11DeferredRenderer::Draw Lighting" );
            auto colorTexture = graph.GetPhysicalTexture( colorResource );
            auto normalsTexture = graph.GetPhysicalTexture( normalsResource );
            auto specularTexture = graph.GetPhysicalTexture( specularResource );

            engine.CopyDepthStencil(); // always needed due to depth testing!

            engine.GetShadowMaps()->DrawLighting( frameLights,
                *colorTexture,
                *normalsTexture,
                *specularTexture,
                *engine.GetDepthBufferCopy() );

            if ( !Engine::GAPI->GetRendererState().RendererSettings.FixViewFrustum ) {
                frameLights.clear();
            }
        };
    } );
}

bool D3D11DeferredRenderer::BindShaderForTexture( D3D11ShaderManager& shaderManager,
    std::shared_ptr<D3D11PShader>& activePS,
    zCTexture* texture,
    bool forceAlphaTest,
    int zMatAlphaFunc,
    MaterialInfo::EMaterialType materialType,
    PShaderID resolvedDiffuseNormalmapped,
    PShaderID resolvedDiffuseNormalmappedFxMap,
    PShaderID resolvedDiffuseNormalmappedAlphatest,
    PShaderID resolvedDiffuseNormalmappedAlphatestFxMap ) {

    auto active = activePS;
    auto newShader = activePS;

    bool blendAdd = zMatAlphaFunc == zMAT_ALPHA_FUNC_ADD;
    bool blendBlend = zMatAlphaFunc == zMAT_ALPHA_FUNC_BLEND;
    bool linZ = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;

    if ( materialType == MaterialInfo::MT_Portal ) {
        newShader = shaderManager.GetPShader( PShaderID::PS_PortalDiffuse );
    } else if ( materialType == MaterialInfo::MT_WaterfallFoam ) {
        newShader = shaderManager.GetPShader( PShaderID::PS_WaterfallFoam );
    } else if ( linZ ) {
        newShader = shaderManager.GetPShader( PShaderID::PS_LinDepth );
    } else if ( blendAdd || blendBlend ) {
        newShader = shaderManager.GetPShader( PShaderID::PS_Simple_FF );
    } else if ( texture->HasAlphaChannel() || forceAlphaTest ) {
        if ( texture->GetSurface()->GetFxMap() ) {
            newShader = shaderManager.GetPShader( resolvedDiffuseNormalmappedAlphatestFxMap );
        } else if ( texture->GetSurface()->GetNormalmap() || Engine::GAPI->GetSceneWetness() > 1e-6 ) {
            newShader = shaderManager.GetPShader( resolvedDiffuseNormalmappedAlphatest ); 
        } else {
            newShader = shaderManager.GetPShader( PShaderID::PS_DiffuseAlphaTest );
        }
    } else {
        if ( texture->GetSurface()->GetFxMap() ) {
            newShader = shaderManager.GetPShader( resolvedDiffuseNormalmappedFxMap );
        } else if ( texture->GetSurface()->GetNormalmap() || Engine::GAPI->GetSceneWetness() > 1e-6 ) {
            newShader = shaderManager.GetPShader( resolvedDiffuseNormalmapped );
        } else {
            newShader = shaderManager.GetPShader( PShaderID::PS_Diffuse );
        }
    }

    if ( active != newShader ) {
        activePS = newShader;
        activePS->Apply();
        return true;
    }
    return false;
}
