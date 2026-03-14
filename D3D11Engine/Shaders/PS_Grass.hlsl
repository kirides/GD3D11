//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include "DS_Defines.h"

cbuffer MI_MaterialInfo : register( b0 )
{
	float MI_SpecularIntensity;
	float MI_SpecularPower;
	float MI_NormalmapStrength;
	float MI_ParallaxOcclusionStrength;
}


//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
Texture2D	TX_Texture0 : register( t1 );
Texture2D	TX_Ground : register( t0 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float3 vNormalVS		: TEXCOORD1;
	float3 vWorldPosition	: TEXCOORD2;
	float4 vCurrClipPos     : TEXCOORD3;  // Current clip position for velocity
	float4 vPrevClipPos     : TEXCOORD4;  // Previous clip position for velocity
	float4 vPosition		: SV_POSITION;
};

// Calculate screen-space velocity from clip positions
float2 CalculateVelocity(float4 currClipPos, float4 prevClipPos)
{
	if (currClipPos.w == 0.0 || prevClipPos.w == 0.0)
		return float2(0, 0);
	
	float2 currNDC = currClipPos.xy / currClipPos.w;
	float2 prevNDC = prevClipPos.xy / prevClipPos.w;
	
	float2 currUV = float2(currNDC.x * 0.5 + 0.5, 1.0 - (currNDC.y * 0.5 + 0.5));
	float2 prevUV = float2(prevNDC.x * 0.5 + 0.5, 1.0 - (prevNDC.y * 0.5 + 0.5));
	
	return prevUV - currUV;
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
DEFERRED_PS_OUTPUT PSMain( PS_INPUT Input ) : SV_TARGET
{
	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);
	
	color.rgb = lerp(saturate(dot(float3(0.333f, 0.333f, 0.333f), color.rgb) * 2.0f), color.rgb, 0.5f);
	
	clip(color.a * 0.7f - 0.5f);
	
	color.rgb *= TX_Ground.SampleLevel(SS_Linear, frac(Input.vWorldPosition.xz / 1000), 5) * 1.1f;
	
	//color.rgb *= 1 - pow((Input.vPosition.z / 1.3f), 2.0f);
	
	DEFERRED_PS_OUTPUT output;
	output.vDiffuse = float4(color.rgb, 1);
	
	output.vNrm = EncodeNormalGBuffer(normalize(Input.vNormalVS));
	
	output.vSI_SP.xy = 0;
	
	// Calculate velocity from clip positions
	output.vVelocity = CalculateVelocity(Input.vCurrClipPos, Input.vPrevClipPos);
	
	return output;
}
