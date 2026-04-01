#pragma once

#include "pch.h"
#include <vector>
#include "WorldObjects.h"

struct RenderToTextureBuffer;

class D3D11LegacyDeferredShading {
public:
    XRESULT DrawPointlightLights(
        std::vector<VobLightInfo*>& lights,
        RenderToTextureBuffer& color,
        RenderToTextureBuffer& normals,
        RenderToTextureBuffer& specular,
        RenderToTextureBuffer& depthCopy );
};
