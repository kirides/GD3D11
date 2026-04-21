#include <AtmosphericScattering.h>

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
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain(PS_INPUT Input) : SV_TARGET
{
	float4 colour = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);

	//darken / lighten foam based on the day / night cycle
	float colourRGB = clamp(AC_LightPos.y+0.2f, 0.1f, 0.6f);
	if (AC_LightPos.y <= 0.08f) {
		colour *= float4(colourRGB, colourRGB, colourRGB+0.1f, 0.80f); //add blue tint at night
	}
	else if (AC_LightPos.y > 0.08f) {
		colour *= float4(colourRGB, colourRGB, colourRGB, 0.80f);
	}	

	return colour;
}