#pragma once
#include "D3D11PFX_Effect.h"

class D3D11PFX_HeightFog :
    public D3D11PFX_Effect {
public:
    D3D11PFX_HeightFog( D3D11PfxRenderer* rnd )
        : D3D11PFX_Effect( rnd ) {
    }
    ~D3D11PFX_HeightFog() override = default;

    /** Draws this effect to the given buffer */
    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override;
};

