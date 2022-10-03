//--------------------------------------------------------------------------------------
// Alpha Blend Buffer
//--------------------------------------------------------------------------------------
cbuffer AlphaBlendInfo : register(b0)
{
	float AB_Alpha;
	float3 AB_Color;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register(s0);
SamplerState SS_samMirror : register(s1);
Texture2D	TX_Texture0 : register(t0);

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float3 vEyeRay			: TEXCOORD1;
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain(PS_INPUT Input) : SV_TARGET
{
	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);
	clip(color.a - 0.5f);
	color *= float4(1.0f, 1.0f, 1.0f, 0.1f);
	return color;
}