#pragma once
#include "pch.h"
#include "RenderToTextureBuffer.h"

// Forward declarations
class D3D11PfxRenderer;

class D3D11PFX_CAS {
public:
    D3D11PFX_CAS( D3D11PfxRenderer* renderer );
    ~D3D11PFX_CAS();

    /** Applies CAS sharpening. input and target must be different resources — the draw goes straight into
        target, so there is no intermediate to copy through any more. */
    XRESULT Apply( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input, INT2 inputSize,
        const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>&  target, INT2 outputSize );
    /** Sets sharpening intensity (0.0 = no sharpening, 1.0 = max sharpening) */
    void SetSharpness( float sharpness );

private:
    D3D11PfxRenderer* Renderer;
    float Sharpness;
};
