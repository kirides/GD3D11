#pragma once
#include "D3D11PFX_Effect.h"

struct RenderToTextureBuffer;
class D3D11PFX_HDR :
    public D3D11PFX_Effect {
public:
    D3D11PFX_HDR( D3D11PfxRenderer* rnd );
    ~D3D11PFX_HDR() override;

    /** Draws this effect to the given buffer */
    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override { return XR_FAILED; };
    XRESULT Render( ID3D11RenderTargetView* output, ID3D11ShaderResourceView* backbuffer );

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

