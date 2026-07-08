#pragma once
#include "D3D11PFX_Effect.h"
#include <wrl/client.h>

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
struct ID3D11UnorderedAccessView;

class D3D11PFX_DepthOfField :
    public D3D11PFX_Effect {
public:
    D3D11PFX_DepthOfField( D3D11PfxRenderer* rnd );
    ~D3D11PFX_DepthOfField() override = default;

    /** Draws this effect to the given buffer */
    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override { return XR_FAILED; }

    /** Applies depth-of-field. backbuffer is the scene SRV, depthSrv the (possibly lower-res) depth
        sampled with normalized UVs, output the RTV that receives the result, resolution the working
        (output) resolution. */
    XRESULT Render( ID3D11RenderTargetView* output, ID3D11ShaderResourceView* backbuffer, ID3D11ShaderResourceView* depthSrv, INT2 resolution );

private:
    /** Compute shader path for FL11+ */
    XRESULT RenderCS( ID3D11RenderTargetView* output, ID3D11ShaderResourceView* backbuffer, ID3D11ShaderResourceView* depthSrv, INT2 resolution );

    // Ping-pong 1x1 R32_FLOAT textures for temporal focus smoothing
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_FocusTexture[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_FocusSRV[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_FocusRTV[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_FocusUAV[2];
    int m_FocusIndex;
};
