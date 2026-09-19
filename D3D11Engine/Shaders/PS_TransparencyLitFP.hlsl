// Ghost/fading static VOBs lit with sun, CSM shadows and clustered point lights, then faded by GA_Alpha.
// Falls back to unlit PS_Transparency when tiled lighting isn't active; see GothicAPI::DrawTransparencyVob.
#include <AtmosphericScattering.h>
#include <FFFog.h>
#include <TransparencyFog.h>

cbuffer GhostAlphaInfo : register( b2 )
{
	float2 GA_ViewportSize;
	float GA_Alpha;
	float GA_AlphaRef;
	float GA_VertLighting;
	float3 GA_Pad;
};

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

	// Alpha-tested materials keep their cutout; the surviving texels fade as a whole.
	float alpha = color.a * GA_Alpha;
	[branch]
	if (GA_AlphaRef > 0.0f)
	{
		clip(color.a - GA_AlphaRef);
		alpha = GA_Alpha;
	}

	float3 normal = normalize(Input.vNormalVS);
	float3 wsPosition = mul(float4(Input.vViewPosition, 1), SQ_InvView).xyz;
	float3 wsNormal = normalize(mul(float4(normal, 0), SQ_InvView).xyz);
	float vertLighting = GA_VertLighting;

	float shadow = vertLighting;
#if SHD_ENABLE
	[branch]
	if (AC_LightPos.y > 0)
	{
		float shadowNoL = saturate(dot(wsNormal, SQ_LightDirectionWS));
		float slopeScale = sqrt(saturate(1.0f - shadowNoL * shadowNoL));
		shadow = ComputeCascadedShadowValueSoft(wsPosition, wsNormal, slopeScale,
			Input.vViewPosition.z, vertLighting, 0.000003f, Input.vPosition.xy);
	}
	else
	{
		shadow = saturate(wsNormal.y) * vertLighting; // night sky ambient, matches PS_Diffuse's Forward+ branch
	}
#endif

	float3 litPixel = FP_ComputeSunLighting(wsPosition, Input.vViewPosition, normal,
		color.rgb, 0.0f, 1.0f, shadow, vertLighting, 1.0f);
	litPixel = ApplyAtmosphericScatteringGround(wsPosition, litPixel);
	litPixel += FP_ComputePointLighting(wsPosition, Input.vViewPosition, normal,
		color.rgb, 0.0f, 1.0f, Input.vPosition.xy);

	return float4(ApplyTransparencyFogWS(litPixel, wsPosition), alpha);
}
