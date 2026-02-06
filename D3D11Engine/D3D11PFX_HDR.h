#pragma once
#include "d3d11pfx_effect.h"

struct RenderToTextureBuffer;
class D3D11PFX_HDR :
    public D3D11PFX_Effect {
public:
    D3D11PFX_HDR( D3D11PfxRenderer* rnd );
    ~D3D11PFX_HDR();

    /** Draws this effect to the given buffer */
    XRESULT Render( RenderToTextureBuffer* fxbuffer );

protected:
    /** Calcualtes the luminance */
    RenderToTextureBuffer* CalcLuminance();

    /** Blurs the backbuffer and puts the result into TempBufferDS4_1*/
    void CreateBloom( RenderToTextureBuffer* lum, RenderToTextureBuffer* bloomTempBuffer );

    RenderToTextureBuffer* LumBuffer1;
    RenderToTextureBuffer* LumBuffer2;
    RenderToTextureBuffer* LumBuffer3;
    int ActiveLumBuffer;
};

