#pragma once
#include "pch.h"
#include "RenderToTextureBuffer.h"
// Forward declarations
class D3D11PfxRenderer;

class D3D11PFX_SimpleSharpen {
public:
    D3D11PFX_SimpleSharpen( D3D11PfxRenderer* renderer )
    : Renderer( renderer ),
    Sharpness( 0.5f )
    { }

    ~D3D11PFX_SimpleSharpen() = default;

    /** Applies CAS sharpening */
    XRESULT Apply( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input, INT2 inputSize,
        const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& target,
        INT2 outputSize,
        RenderToTextureBuffer& intermediateBuffer );

private:
    D3D11PfxRenderer* Renderer;
    float Sharpness;
};
