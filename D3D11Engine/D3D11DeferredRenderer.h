#pragma once
#include "ISceneRenderer.h"
#include "GothicAPI.h"

/** Deferred shading renderer: GBuffer pass, deferred lighting pass,
    and GBuffer pixel shader selection. */
class D3D11DeferredRenderer final : public ISceneRenderer {
public:
    D3D11DeferredRenderer() = default;
    ~D3D11DeferredRenderer() override = default;

    D3D11DeferredRenderer( const D3D11DeferredRenderer& ) = delete;
    D3D11DeferredRenderer& operator=( const D3D11DeferredRenderer& ) = delete;

    // --- ISceneRenderer --------------------------------------------------

    void AddGeometryPasses(
        RenderGraph& graph,
        D3D11GraphicsEngine& engine,
        RGResourceHandle colorResource,
        RGResourceHandle velocityBufferHandle,
        RGResourceHandle backBufferHandle,
        RGResourceHandle& outNormalsResource,
        RGResourceHandle& outSpecularResource,
        RGResourceHandle& outReactiveMaskResource ) override;

    void AddLightingPasses(
        RenderGraph& graph,
        D3D11GraphicsEngine& engine,
        RGResourceHandle colorResource,
        RGResourceHandle normalsResource,
        RGResourceHandle specularResource,
        RGResourceHandle backBufferHandle,
        std::vector<VobLightInfo*>& frameLights ) override;

    bool BindShaderForTexture(
        D3D11ShaderManager& shaderManager,
        std::shared_ptr<D3D11PShader>& activePS,
        zCTexture* texture,
        bool forceAlphaTest,
        int zMatAlphaFunc,
        MaterialInfo::EMaterialType materialType,
        PShaderID resolvedDiffuseNormalmapped,
        PShaderID resolvedDiffuseNormalmappedFxMap,
        PShaderID resolvedDiffuseNormalmappedAlphatest,
        PShaderID resolvedDiffuseNormalmappedAlphatestFxMap ) override;
};
