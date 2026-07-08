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

    /** Tonemaps the HDR scene SRV into the (LDR) output RTV. resolution is the working (output)
        resolution; sceneSrv is the pre-tonemap HDR scene (no longer read from global engine state). */
    XRESULT Render( ID3D11RenderTargetView* output, ID3D11ShaderResourceView* backbuffer, INT2 resolution );

protected:
    /** Calcualtes the luminance from the given HDR scene SRV */
    RenderToTextureBuffer* CalcLuminance( ID3D11ShaderResourceView* sceneSrv );

    /** Blurs the HDR scene SRV and puts the result into bloomTempBuffer */
    void CreateBloom( RenderToTextureBuffer* lum, RenderToTextureBuffer* bloomTempBuffer, ID3D11ShaderResourceView* sceneSrv, INT2 resolution );

    RenderToTextureBuffer* LumBuffer1;
    RenderToTextureBuffer* LumBuffer2;
    RenderToTextureBuffer* LumBuffer3;
    int ActiveLumBuffer;
};

