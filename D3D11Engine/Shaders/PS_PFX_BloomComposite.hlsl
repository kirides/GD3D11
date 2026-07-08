//--------------------------------------------------------------------------------------
// Pixel Shader - Bloom Composite
// Samples the final (mip-0) bloom result and scales it by the bloom strength. Drawn as a
// fullscreen triangle with additive blending, so the output is added on top of the scene
// (dst + bloom * strength). The bloom buffer is half-resolution; linear sampling upscales
// it smoothly to full resolution.
//--------------------------------------------------------------------------------------

SamplerState SS_LinearClamp : register( s0 );
Texture2D TX_Bloom : register( t0 );

cbuffer BloomConstantBuffer : register( b0 )
{
    float2 B_TexelSize;
    float  B_Threshold;
    float  B_Knee;
    float  B_Intensity;    // bloom strength
    float  B_FilterRadius;
    float2 B_Pad;
};

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float3 bloom = TX_Bloom.Sample( SS_LinearClamp, Input.vTexcoord ).rgb;
    return float4( bloom * B_Intensity, 1.0f );
}
