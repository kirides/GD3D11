//--------------------------------------------------------------------------------------
// AMD FidelityFX CAS (Contrast Adaptive Sharpening)
//--------------------------------------------------------------------------------------

// Define for HLSL usage
#define FFX_GPU
#define FFX_HLSL
#include "FidelityFX/ffx_core.h"

cbuffer CASConstants : register(b0)
{
    uint4 const0;
    uint4 const1;
};

Texture2D InputTexture : register(t0);
SamplerState LinearSampler : register(s0);

// CAS requires these functions to be defined
FfxFloat32x4 casLoad(uint2 p)
{
    return InputTexture.Load(int3(p, 0));
}

void casInput(inout FfxFloat32 r, inout FfxFloat32 g, inout FfxFloat32 b)
{
    // Optional: Apply any input transforms here (e.g., gamma to linear)
}

#include "FidelityFX/cas/ffx_cas.h"

struct PS_INPUT
{
	float2 TexCoord		: TEXCOORD0;
	float3 vEyeRay		: TEXCOORD1;
	float4 Position		: SV_POSITION;
};

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    // Get pixel coordinates
    uint2 pixelPos = uint2(input.Position.xy);
    
    // Apply CAS
    FfxFloat32x3 color;
    ffxCasFilter(color[0], color[1], color[2], pixelPos, const0, const1, true);
    
    return float4(color[0], color[1], color[2], 1.0f);
}