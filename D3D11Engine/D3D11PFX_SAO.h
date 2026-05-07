#pragma once
#include "D3D11PFX_Effect.h"
#include <wrl/client.h>

struct RenderToTextureBuffer;

class D3D11PFX_SAO : public D3D11PFX_Effect {
public:
    D3D11PFX_SAO( D3D11PfxRenderer* rnd );
    ~D3D11PFX_SAO() override = default;

    /** Not used */
    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override { return XR_FAILED; }

    /** Renders SAO into the given render target */
    XRESULT Render( ID3D11ShaderResourceView* depthSRV,
                    ID3D11ShaderResourceView* normalsSRV,
                    ID3D11RenderTargetView* outputRTV );

    /** Computes AO into m_AOBuffer but skips the final modulate blit */
    XRESULT RenderAO( ID3D11ShaderResourceView* depthSRV,
                      ID3D11ShaderResourceView* normalsSRV );

    /** Returns the SRV of the computed AO buffer (R8_UNORM) for composition */
    ID3D11ShaderResourceView* GetAOResultSRV() const;

private:
    std::unique_ptr<RenderToTextureBuffer> m_AOBuffer;      // Full-res R8_UNORM with UAV
    std::unique_ptr<RenderToTextureBuffer> m_BlurTempBuffer; // Full-res R8_UNORM with UAV for blur ping-pong
};
