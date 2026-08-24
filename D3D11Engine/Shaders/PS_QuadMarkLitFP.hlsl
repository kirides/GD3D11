// Real sun+CSM-shadow+clustered-point-light shading for lit quad marks (blood splats, ground
// marks), using the same tiled-deferred/Forward+ light data the main scene renderers consume.
// Falls back to PS_QuadMarkLit (flat day/night factor) when tiled lighting isn't active; see
// DrawQuadMarkRun.
#include <AtmosphericScattering.h>
#include <FFFog.h>

SamplerState SS_Linear : register( s0 );
Texture2D	TX_Texture0 : register( t0 );

// Must come after SS_Linear: FP_SS_Linear is a macro alias to it, used by headers pulled in below.
#include <include/ForwardPlusLighting.hlsl>

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

	float3 normal = normalize(Input.vNormalVS);
	float3 wsPosition = mul(float4(Input.vViewPosition, 1), SQ_InvView).xyz;
	float3 wsNormal = normalize(mul(float4(normal, 0), SQ_InvView).xyz);

	float shadow = 1.0f;
#if SHD_ENABLE
	// AC_LightPos is a cbuffer scalar (frame-uniform), not per-pixel data.
	[branch]
	if (AC_LightPos.y > 0)
	{
		float shadowNoL = saturate(dot(wsNormal, SQ_LightDirectionWS));
		float slopeScale = sqrt(saturate(1.0f - shadowNoL * shadowNoL));
		shadow = ComputeCascadedShadowValueSoft(wsPosition, wsNormal, slopeScale,
			Input.vViewPosition.z, 1.0f, 0.000003f, Input.vPosition.xy);
	}
	else
	{
		shadow = saturate(wsNormal.y); // night sky ambient, matches PS_Diffuse's Forward+ branch
	}
#endif

	float3 litPixel = FP_ComputeSunLighting(wsPosition, Input.vViewPosition, normal,
		color.rgb, 0.0f, 1.0f, shadow, 1.0f, 1.0f);
	litPixel = ApplyAtmosphericScatteringGround(wsPosition, litPixel);
	litPixel += FP_ComputePointLighting(wsPosition, Input.vViewPosition, normal,
		color.rgb, 0.0f, 1.0f, Input.vPosition.xy);

	return float4(litPixel, color.a);
}
