#pragma once
#include "d3d11pfx_effect.h"

class D3D11PFX_DistanceBlur :
    public D3D11PFX_Effect {
public:
    D3D11PFX_DistanceBlur( D3D11PfxRenderer* rnd );
    ~D3D11PFX_DistanceBlur();

    /** Draws this effect to the given buffer */
    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override { return XR_FAILED; }
    XRESULT Render( ID3D11ShaderResourceView* diffuse );
};

