#pragma once
#include "D3D11PFX_Effect.h"
#include <wrl/client.h>

struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
struct ID3D11SamplerState;

/** Standalone multi-mip progressive bloom (Jimenez / Call of Duty style), computed
    entirely with compute shaders. Decoupled from the HDR/tonemap effect: it only adds
    glow to the scene and never changes exposure or tonemapping. FeatureLevel 11+ only. */
class D3D11PFX_Bloom :
    public D3D11PFX_Effect {
public:
    D3D11PFX_Bloom( D3D11PfxRenderer* rnd );
    ~D3D11PFX_Bloom() override = default;

    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override { return XR_FAILED; }

    /** Builds the bloom pyramid from the scene SRV and additively composites it onto rtv.
        resolution is the working (output) resolution; the pyramid and composite are sized to it. */
    XRESULT Render( ID3D11RenderTargetView* output, ID3D11ShaderResourceView* sceneSrv, INT2 resolution );

private:
    // Linear sampler with clamp addressing (avoids opposite-edge bleeding in the blur).
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_ClampSampler;
};
