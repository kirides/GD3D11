#include <TransparencyFog.h>

//--------------------------------------------------------------------------------------
// Ghost NPC Buffer
//--------------------------------------------------------------------------------------
cbuffer GhostAlphaInfo : register( b0 )
{
	float2 GA_ViewportSize;
	float GA_Alpha;
	float GA_AlphaRef;
	float GA_VertLighting;
	float3 GA_Pad;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_Scene : register( t5 );

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
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	//float2 screenUV = Input.vPosition.xy / GA_ViewportSize;
	//float3 screenColor = TX_Scene.Sample(SS_Linear, screenUV).rgb;
	//float screenLuma = 0.2126 * screenColor.r + 0.7125 * screenColor.g + 0.0722 * screenColor.b;

	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);
	//color *= float4(screenLuma, screenLuma, screenLuma, GA_Alpha);

	// Alpha-tested materials keep their cutout; the surviving texels fade as a whole.
	float alpha = color.a * GA_Alpha;
	[branch]
	if (GA_AlphaRef > 0.0f)
	{
		clip(color.a - GA_AlphaRef);
		alpha = GA_Alpha;
	}
	return float4(ApplyTransparencyFog(color.rgb, Input.vViewPosition), alpha);
}
