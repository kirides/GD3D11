#pragma once

#include "GothicAPI.h"
#include "ShaderIDs.h"
#include "RenderGraph.h"
#include <memory>
#include <vector>

class D3D11GraphicsEngine;
class D3D11ShaderManager;
class D3D11PShader;
class zCTexture;
struct VobLightInfo;
class RenderGraph;

/** Abstract interface for scene rendering strategies (Deferred, Forward+, etc.).
    Each implementation adds its specific render-graph passes and selects
    the appropriate pixel shaders for geometry rendering. */
class ISceneRenderer {
public:
    virtual ~ISceneRenderer() = default;

    /** Add the primary geometry rendering pass(es) to the render graph.
        Deferred: writes GBuffer.  Forward+: depth prepass + lit geometry.
        Outputs resource handles consumed by later passes (HBAO, post-FX). */
    virtual void AddGeometryPasses(
        RenderGraph& graph,
        D3D11GraphicsEngine& engine,
        RGResourceHandle colorResource,
        RGResourceHandle velocityBufferHandle,
        RGResourceHandle backBufferHandle,
        RGResourceHandle& outNormalsResource,
        RGResourceHandle& outSpecularResource,
        RGResourceHandle& outReactiveMaskResource ) = 0;

    /** Add the lighting resolution pass(es) to the render graph.
        Deferred: tiled / legacy deferred shading.  Forward+: no-op. */
    virtual void AddLightingPasses(
        RenderGraph& graph,
        D3D11GraphicsEngine& engine,
        RGResourceHandle colorResource,
        RGResourceHandle normalsResource,
        RGResourceHandle specularResource,
        RGResourceHandle backBufferHandle,
        std::vector<VobLightInfo*>& frameLights ) = 0;

    /** Select the appropriate pixel shader for a texture / material combination. */
    virtual bool BindShaderForTexture(
        D3D11ShaderManager& shaderManager,
        std::shared_ptr<D3D11PShader>& activePS,
        zCTexture* texture,
        bool forceAlphaTest,
        int zMatAlphaFunc,
        MaterialInfo::EMaterialType materialType,
        PShaderID resolvedDiffuseNormalmapped,
        PShaderID resolvedDiffuseNormalmappedFxMap,
        PShaderID resolvedDiffuseNormalmappedAlphatest,
        PShaderID resolvedDiffuseNormalmappedAlphatestFxMap ) = 0;
};
