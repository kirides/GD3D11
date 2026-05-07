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
    XRESULT Render( ID3D11ShaderResourceView* backbuffer );

private:
    /** Compute shader path for FL11+ */
    XRESULT RenderCS( ID3D11ShaderResourceView* backbuffer );

    // Ping-pong 1x1 R32_FLOAT textures for temporal focus smoothing
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_FocusTexture[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_FocusSRV[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_FocusRTV[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_FocusUAV[2];
    int m_FocusIndex;
};
