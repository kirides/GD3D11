#pragma once
#include "pch.h"

// Forward declarations
class D3D11PfxRenderer;
struct RenderToTextureBuffer;

class D3D11PFX_FSR1 {
public:
    D3D11PFX_FSR1( D3D11PfxRenderer* renderer );
    ~D3D11PFX_FSR1();

    /** Initialize FSR1 resources */
    bool Init();

    /** Called on resize */
    void OnResize( const INT2& inputSize, const INT2& outputSize );

    /** Applies FSR1 EASU upscaling from input to output 
     *  @param input Low resolution input texture SRV
     *  @param output High resolution output RTV
     *  @param inputSize Size of the input texture
     *  @param outputSize Size of the output texture
     */
    XRESULT ApplyEASU( 
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input,
        ID3D11RenderTargetView* output,
        const INT2& inputSize,
        const INT2& outputSize );

    /** Applies FSR1 RCAS sharpening (optional, after EASU)
     *  @param input Input texture SRV (EASU output)
     *  @param output Output RTV
     *  @param sharpness Sharpness value (0.0 = maximum sharpness, higher = less sharp)
     */
    XRESULT ApplyRCAS(
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input,
        ID3D11RenderTargetView* output,
        float sharpness = 0.2f );

    /** Applies full FSR1 pipeline (EASU + optional RCAS)
     *  @param input Low resolution input texture SRV
     *  @param output High resolution output RTV
     *  @param inputSize Size of the input texture
     *  @param outputSize Size of the output texture
     *  @param enableRCAS Whether to apply RCAS sharpening after EASU
     *  @param sharpness RCAS sharpness (only used if enableRCAS is true)
     */
    XRESULT Apply(
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input,
        ID3D11RenderTargetView* output,
        const INT2& inputSize,
        const INT2& outputSize,
        bool enableRCAS = true,
        float sharpness = 0.2f );

    /** Sets the RCAS sharpness value (0.0 = maximum sharpness, higher values = less sharp) */
    void SetSharpness( float sharpness );

    void ReleaseResources();

private:
    D3D11PfxRenderer* Renderer;
    
    // Sampler state for FSR1 (point sampling required for Gather operations)
    Microsoft::WRL::ComPtr<ID3D11SamplerState> PointSampler;
    
    float Sharpness;
    INT2 CurrentInputSize;
    INT2 CurrentOutputSize;
    bool Initialized;
};
