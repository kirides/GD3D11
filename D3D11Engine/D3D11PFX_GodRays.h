#pragma once
#include "D3D11PFX_Effect.h"
#include "TexturePool.h"

class D3D11PFX_GodRays :
    public D3D11PFX_Effect {
public:
    D3D11PFX_GodRays( D3D11PfxRenderer* rnd );
    ~D3D11PFX_GodRays() override = default;

    /** Draws this effect to the given buffer */
    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override { return XR_FAILED; }
    XRESULT Render( ID3D11ShaderResourceView* backbuffer, ID3D11ShaderResourceView* depthCopy );

    /** Renders godrays mask+zoom to a ¼-res pool texture, skipping the final additive blit.
        Returns the pool texture SRV via outGodRaysSRV. Returns XR_SUCCESS if godrays were produced. */
    XRESULT RenderToTexture( ID3D11ShaderResourceView* backbuffer,
                             ID3D11ShaderResourceView* depthCopy,
                             ID3D11ShaderResourceView** outGodRaysSRV );

private:
    /** Compute shader path for FL11+ */
    XRESULT RenderCS( ID3D11ShaderResourceView* backbuffer, ID3D11ShaderResourceView* depthCopy );

    /** Compute shader path that writes to pool texture without final blit */
    XRESULT RenderToTextureCS( ID3D11ShaderResourceView* backbuffer,
                               ID3D11ShaderResourceView* depthCopy,
                               ID3D11ShaderResourceView** outGodRaysSRV );

    /** Keeps the godrays result texture alive until the next frame replaces it */
    TextureHandle m_GodRaysResult;
};

