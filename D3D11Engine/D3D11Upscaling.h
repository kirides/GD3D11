#pragma once

#include "RenderGraph.h"
#include "RenderPass.h"

class D3D11GraphicsEngine;

class D3D11Upscaling
{
public:
    void UpdateUpscaling( D3D11GraphicsEngine& engine );

    bool AddUpscalingPass(
        RenderGraph& graph,
        D3D11GraphicsEngine& engine,
        ID3D11RenderTargetView* outputRTV,
        RGResourceHandle color,
        ID3D11ShaderResourceView* depth,
        RGResourceHandle motionVectors,
        RGResourceHandle reactiveMask );
private:
};

