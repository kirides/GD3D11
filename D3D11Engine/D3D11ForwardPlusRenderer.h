#pragma once
#include "ISceneRenderer.h"
#include <memory>

class D3D11DeferredRenderer;
class D3D11ConstantBuffer;

class D3D11ForwardPlusRenderer final : public ISceneRenderer {
public:
    explicit D3D11ForwardPlusRenderer( D3D11DeferredRenderer& deferredFallback );
    ~D3D11ForwardPlusRenderer() override = default;

    D3D11ForwardPlusRenderer( const D3D11ForwardPlusRenderer& ) = delete;
    D3D11ForwardPlusRenderer& operator=( const D3D11ForwardPlusRenderer& ) = delete;

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

private:
    D3D11DeferredRenderer& m_DeferredFallback;
    std::unique_ptr<D3D11ConstantBuffer> m_SunCSMConstantBuffer;
    std::unique_ptr<D3D11ConstantBuffer> m_TileConstantBuffer;
};
