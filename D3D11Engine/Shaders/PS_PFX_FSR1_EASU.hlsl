//--------------------------------------------------------------------------------------
// AMD FidelityFX FSR1 - Edge Adaptive Spatial Upsampling (EASU)
//--------------------------------------------------------------------------------------

// Define for HLSL GPU usage
#define A_GPU
#define A_HLSL
#define A_SKIP_EXT  // Skip GLSL extension declarations

// Include the AMD FidelityFX A portability layer and FSR1 header
#include "FidelityFX/fsr1/ffx_a.h"

// Enable EASU pass (32-bit float version)
#define FSR_EASU_F 1

// FSR1 Constant buffer
cbuffer FSR1Constants : register(b0)
{
    uint4 Const0;  // FsrEasuCon output
    uint4 Const1;  // FsrEasuCon output
    uint4 Const2;  // FsrEasuCon output
    uint4 Const3;  // FsrEasuCon output
};

// Input texture (low resolution)
Texture2D InputTexture : register(t0);
SamplerState PointSampler : register(s0);

// FSR1 EASU requires these callback functions to be defined
// These use Gather4 to fetch 4 texels at once for each color channel
AF4 FsrEasuRF(AF2 p)
{
    return InputTexture.GatherRed(PointSampler, p);
}

AF4 FsrEasuGF(AF2 p)
{
    return InputTexture.GatherGreen(PointSampler, p);
}

AF4 FsrEasuBF(AF2 p)
{
    return InputTexture.GatherBlue(PointSampler, p);
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
    
    // Output color
    AF3 color;
    
    // Apply FSR1 EASU upscaling
    FsrEasuF(color, pixelPos, Const0, Const1, Const2, Const3);
    
    return float4(color, 1.0f);
}
