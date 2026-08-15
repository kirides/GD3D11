// Lit quad marks (blood splats, ground marks). Unlike PS_World this isn't a G-buffer fill
// shader - it writes straight to the already-lit back buffer, so it applies the day/night
// factor itself instead of vertex color (always (0,0,0,1) for these procedural decal polys).
#include <FFFog.h>

// b0 is FFPipelineConstantBuffer, pulled in above for DoAlphaTest.
cbuffer QuadMarkLightCB : register( b1 )
{
	float3 QM_DayLight;
	float QM_Pad;
}

SamplerState SS_Linear : register( s0 );
Texture2D	TX_Texture0 : register( t0 );

struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vTangent			: TEXCOORD3;
	float4 vPosition		: SV_POSITION;
};

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);
	DoAlphaTest(color.a);
	color.rgb *= QM_DayLight;
	return color;
}
