#pragma once
#include "pch.h"

#include <FidelityFX/api/internal/ffx_interface.h>

// Forward declarations
class D3D11PfxRenderer;
struct FfxFsr2Context;
struct FfxFsr3UpscalerContext;

/** The FidelityFX temporal upscalers, FSR 2 and FSR 3. Both come from the same DLL and take the same
    inputs; only one context is alive at a time. */
class D3D11PFX_FSR3 {
public:
    enum class EVersion { Fsr2, Fsr3 };

    D3D11PFX_FSR3( D3D11PfxRenderer* renderer );
    ~D3D11PFX_FSR3();

    /** Initialize the backend interface and the context
     * @param maxInputSize The maximum resolution the game will render at before upscaling
     * @param maxOutputSize The maximum resolution the game will upscale to (display res)
     * @param version Which upscaler the context is for
     */
    bool Init( const INT2& maxInputSize, const INT2& maxOutputSize, EVersion version );

    /** Destroys the context. Call this on device lost or shutdown. */
    void Destroy();

    /** * Applies the temporal upscaling pass, (re)creating the context if size or version changed.
     * Note: The output resource MUST have been created with D3D11_BIND_UNORDERED_ACCESS.
     * * @param color Aliased color input (SRV)
     * @param depth Depth buffer input (SRV)
     * @param motionVectors Motion vectors input (SRV)
     * @param output High resolution output (RTV or UAV - underlying resource must support UAV)
     * @param inputSize Current render resolution
     * @param outputSize Current display resolution
     * @param deltaTimeMs Frame delta time in milliseconds
     * @param jitterOffset Sub-pixel jitter offset used this frame
     * @param motionVectorScale Scale to apply to motion vectors to convert them to pixel space
     * @param resetAccumulation Set to true if camera cuts or teleports occurred this frame
     * @param cameraFovAngleVertical Vertical FOV in radians
     * @param cameraNear Near clip plane distance
     * @param cameraFar Far clip plane distance
     * @param enableSharpening Whether to run the RCAS sharpening pass
     * @param sharpness Sharpness value (0.0 = max, 1.0 = min, opposite of FSR1's stops)
     */
    XRESULT Apply(
        EVersion version,
        ID3D11ShaderResourceView* color,
        ID3D11ShaderResourceView* depth,
        ID3D11ShaderResourceView* motionVectors,
        ID3D11ShaderResourceView* reactiveMask,
        ID3D11RenderTargetView* output, // We extract the ID3D11Resource from this
        const INT2& inputSize,
        const INT2& outputSize,
        float deltaTimeMs,
        const float2& jitterOffset,
        const float2& motionVectorScale,
        bool resetAccumulation,
        float cameraFovAngleVertical,
        float cameraNear = 0.1f,
        float cameraFar = 1000.0f,
        bool enableSharpening = true,
        float sharpness = 0.2f );

private:
    D3D11PfxRenderer* Renderer;

    FfxFsr2Context* Fsr2Context;            // exactly one of these two is set while Initialized
    FfxFsr3UpscalerContext* Context;
    void* ScratchMemory;

    INT2 MaxInputSize;
    INT2 MaxOutputSize;
    EVersion Version;
    bool Initialized;
};
