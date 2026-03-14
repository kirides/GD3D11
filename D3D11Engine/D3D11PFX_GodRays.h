#pragma once
#include "d3d11pfx_effect.h"

class D3D11PFX_GodRays :
    public D3D11PFX_Effect {
public:
    D3D11PFX_GodRays( D3D11PfxRenderer* rnd );
    ~D3D11PFX_GodRays();

    /** Draws this effect to the given buffer */
    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override { return XR_FAILED; }
    XRESULT Render( ID3D11ShaderResourceView* backbuffer, ID3D11ShaderResourceView* depth );
};

