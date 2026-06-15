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

    /** Applies simple unsharp-mask sharpening from a source texture into a destination
        buffer (source and destination must be different textures). Uses a compute shader
        that writes the destination's UAV on FeatureLevel 11+, and falls back to a
        pixel-shader pass that writes the destination's RTV on FeatureLevel 10. */
    XRESULT Apply( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& source, INT2 sourceSize,
        RenderToTextureBuffer* dest,
        INT2 destSize );

private:
    /** Pixel-shader fallback path (FeatureLevel 10): reads source SRV, writes dest RTV. */
    XRESULT ApplyPixelShader( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& source,
        RenderToTextureBuffer* dest,
        INT2 destSize );

    D3D11PfxRenderer* Renderer;
    float Sharpness;
};
