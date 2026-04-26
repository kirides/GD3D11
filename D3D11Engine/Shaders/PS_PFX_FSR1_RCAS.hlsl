//--------------------------------------------------------------------------------------
// AMD FidelityFX FSR1 - Robust Contrast Adaptive Sharpening (RCAS)
// This is applied after EASU for additional sharpening
//--------------------------------------------------------------------------------------

// Define for HLSL GPU usage
#define A_GPU
#define A_HLSL
#define A_SKIP_EXT  // Skip GLSL extension declarations

// Include the AMD FidelityFX A portability layer
#include "FidelityFX/fsr1/ffx_a.h"

// Enable RCAS pass (32-bit float version)
#define FSR_RCAS_F 1

// FSR1 RCAS Constant buffer
cbuffer FSR1RCASConstants : register(b0)
{
    uint4 RCASConst;  // FsrRcasCon output (only first element used)
};

// Input texture (EASU output or any texture to sharpen)
Texture2D InputTexture : register(t0);
SamplerState PointSampler : register(s0);

// FSR1 RCAS requires these callback functions to be defined
AF4 FsrRcasLoadF(ASU2 p)
{
    return InputTexture.Load(int3(p, 0));
}

void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b)
{
    // No input color conversion needed
}

// Include FSR1 implementation after callbacks are defined
#include "FidelityFX/fsr1/ffx_fsr1.h"

struct PS_INPUT
{
    float2 TexCoord : TEXCOORD0;
	float3 vEyeRay	: TEXCOORD1;
    float4 Position : SV_POSITION;
};

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    // Get output pixel coordinates
    AU2 pixelPos = AU2(input.Position.xy);
    
    // Output color components
    AF1 r, g, b;
    
    // Apply FSR1 RCAS sharpening
    FsrRcasF(r, g, b, pixelPos, RCASConst);
    
    return float4(r, g, b, 1.0f);
}
