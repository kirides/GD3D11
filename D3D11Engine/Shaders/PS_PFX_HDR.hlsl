//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

#include <hdr.h>

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register(s0);
SamplerState SS_samMirror : register(s1);
Texture2D TX_Scene : register(t0);
Texture2D TX_Lum : register(t1);
Texture2D TX_Bloom : register(t2);



//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};


//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain(PS_INPUT Input) : SV_TARGET
{
    float4 sample = TX_Scene.Sample(SS_Linear, Input.vTexcoord);
    float3 HDRColor = sample.rgb;
	//HDRColor = float3(Input.vTexcoord.r, 0, 0);
#if USE_TONEMAP == 0
		float3 toneMapped = saturate(ToneMap_jafEq4(HDRColor, TX_Lum, SS_Linear));
#elif USE_TONEMAP == 1
		float3 toneMapped = saturate(Uncharted2Tonemap(HDRColor, TX_Lum, SS_Linear));
#elif USE_TONEMAP == 2
		float3 toneMapped = saturate(ACESFilmTonemap(HDRColor, TX_Lum, SS_Linear));
#elif USE_TONEMAP == 3
		float3 toneMapped = saturate(PerceptualQuantizerTonemap(HDRColor, TX_Lum, SS_Linear));
#elif USE_TONEMAP == 4
		float3 toneMapped = saturate(ToneMap_Simple(HDRColor, TX_Lum, SS_Linear));
#elif USE_TONEMAP == 5
		float3 toneMapped = saturate(ACESFittedTonemap(HDRColor, TX_Lum, SS_Linear));
#endif
	
    float3 bloom = TX_Bloom.Sample(SS_Linear, Input.vTexcoord).rgb * HDR_BloomStrength;

	//return float4(sample.rgb, 1);
	//return float4(TX_Lum.SampleLevel(SS_Linear, float2(0.5f, 0.5f), 10).rrr,1);
    return float4(pow(toneMapped * (1 - bloom) + bloom, 2.2f), 1);
}