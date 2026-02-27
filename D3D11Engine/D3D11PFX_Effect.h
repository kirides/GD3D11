#pragma once
#include "pch.h"

struct RenderToTextureBuffer;
class D3D11PfxRenderer;
class D3D11PFX_Effect {
public:
    D3D11PFX_Effect( D3D11PfxRenderer* rnd ) :
        FxRenderer( rnd ) {
    }
    virtual ~D3D11PFX_Effect() = default;

    /** Draws this effect to the given buffer */
    virtual XRESULT Render( RenderToTextureBuffer* fxbuffer ) = 0;
protected:
    /** FX-Object */
    D3D11PfxRenderer* FxRenderer;
};

