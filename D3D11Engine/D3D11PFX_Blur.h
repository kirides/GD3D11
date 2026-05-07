#pragma once
#include "D3D11PFX_Effect.h"
#include "ShaderIDs.h"

class D3D11PFX_Blur :
    public D3D11PFX_Effect {
public:
    D3D11PFX_Blur( D3D11PfxRenderer* rnd );
    ~D3D11PFX_Blur() override;

    /** Draws this effect to the given buffer */
    virtual XRESULT RenderBlur( RenderToTextureBuffer* fxbuffer, bool leaveResultInD4_2 = false, float threshold = 0.0f, float scale = 1.0f, const XMFLOAT4& colorMod = XMFLOAT4( 1, 1, 1, 1 ), PShaderID finalCopyShader = PShaderID::PS_PFX_Simple );

    /** Draws this effect to the given buffer */
    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override;
};

