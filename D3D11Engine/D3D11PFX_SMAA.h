#pragma once
#include <d3d11_1.h>
#include <d3d11shader.h>

#include "pch.h"
#include "D3D11PFX_Effect.h"
#include "SMAA/D3D11SMAA.h"


struct RenderToTextureBuffer;
class D3D11PFX_SMAA :
    public D3D11PFX_Effect {
public:
    D3D11PFX_SMAA( D3D11PfxRenderer* rnd );
    ~D3D11PFX_SMAA() override = default;

    /** Creates needed resources */
    bool Init();

    /** Called on resize */
    void OnResize( const INT2& size );

    /** Renders the PostFX */
    void RenderPostFX( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& renderTargetSRV );

    /** Draws this effect to the given buffer */
    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override { return XR_SUCCESS; };

    void ReleaseResources() { m_Native->ReleaseResources(); }

private:
    std::unique_ptr<D3D11SMAA> m_Native;
};

