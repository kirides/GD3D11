//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <DS_Defines.h>
#include "DepthReconstruction.h"
#include "include/PointLightShadows.h"
#include "include/RainWetnessSample.h"

cbuffer DS_PointLightConstantBuffer : register( b0 )
{
	float4 PL_Color;

	float PL_Range;
	float3 Pl_PositionWorld;

	float PL_Outdoor;
	float3 Pl_PositionView;

	float2 PL_ViewportSize;
	float2 PL_JitterOffset;

	float4 PL_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
	matrix PL_InvView;

	float3 PL_LightScreenPos;
	float PL_ShadowRange;

	// Rain wetness, frame-constant (see ApplyPointLightWetness / RainWetnessSample.h).
	matrix PL_RainViewProj;
	float PL_SceneWettness;
	float PL_WetLightReflections;
	float PL_RainTime;
	float PL_RainFxWeight;

	float PL_WetCoatScale;   // wet-ground reflection gate; PL_Color.w only gates material highlights
	float3 PL_Pad5;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
SamplerComparisonState SS_Comp : register( s2 );
Texture2D	TX_Diffuse : register( t0 );
Texture2D	TX_Nrm : register( t1 );
Texture2D	TX_Depth : register( t2 );
Texture2D	TX_SI_SP : register( t7 );
Texture2D	TX_RainShadowmap : register( t4 );
Texture2D	TX_Distortion : register( t6 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float4 vPosition		: SV_POSITION;
};

float3 VSPositionFromDepth(float depth, float2 vTexCoord)
{
	return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord - PL_JitterOffset, PL_ProjParams.xy );
}

//--------------------------------------------------------------------------------------
// Blinn-Phong Lighting Reflection Model
//--------------------------------------------------------------------------------------
float CalcBlinnPhongLighting(float3 N, float3 H)
{
    return saturate(dot(N, H));
}

float GetShadow(float2 uv)
{
	// Get light direction
	float2 lightDir = PL_LightScreenPos.xy - uv;
	float distance = length(lightDir);
	lightDir /= distance; // Normalize the direction

	// Calculate ray steps size
	const int numSteps = 100;
	float stepSize = distance / numSteps;

	//float depthLight = TX_Depth.Sample(SS_Linear, PL_LightScreenPos).r;
	float depthTarget = TX_Depth.Sample(SS_Linear, uv).r;

	float dx = ddx(uv.xy);
	float dy = ddy(uv.xy);

	float2 ray = PL_LightScreenPos.xy;
	for(int i=0;i<numSteps;i++)
	{
		ray += lightDir * stepSize;

		float depthRay = TX_Depth.SampleGrad(SS_Linear,ray, dx, dy);

		if(depthRay < PL_LightScreenPos.z)
			return 0;
	}

	return 1;
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	// Get screen UV
	float2 uv = Input.vPosition.xy / PL_ViewportSize;

	// Look up the diffuse color
	float4 diffuse = TX_Diffuse.Sample(SS_Linear, uv);

	// Get the second GBuffer
	float2 gb2 = TX_Nrm.Sample(SS_Linear, uv).xy;

	// Decode the view-space normal from octahedral R16G16_SNORM
	float3 normal = DecodeNormalGBuffer(gb2);

	// Get specular parameters
	float4 gb3 = TX_SI_SP.Sample(SS_Linear, uv);
	float specIntensity = gb3.x;
	float specPower = gb3.y;

	// Reconstruct VS World Position from depth
	float expDepth = TX_Depth.Sample(SS_Linear, uv).r;
	float3 vsPosition = VSPositionFromDepth(expDepth, uv);
	float3 wsPosition = mul(float4(vsPosition, 1), PL_InvView).xyz;
	float3 wsNormal = normalize(mul(float4(normal, 0), PL_InvView).xyz);
	float3 wsGeomNormal = normalize(mul(float4(GeomNormalFromDerivativesVS(vsPosition, normal), 0), PL_InvView).xyz);

	// Rain wetness: the same wet surface (albedo, puddles, water film) the sun pass shaded.
	WetSurface wet = ApplyPointLightWetness(wsPosition, wsNormal, wsGeomNormal, length(vsPosition),
		TX_RainShadowmap, SS_Comp, PL_RainViewProj, PL_SceneWettness, TX_Distortion, SS_Linear, PL_RainTime, PL_RainFxWeight,
		diffuse.rgb, specIntensity, specPower);
	float3 litN = normalize(mul((float3x3)PL_InvView, wet.rippleN));   // G-buffer normal plus rain ripples/drop rings

	// Get direction and distance from the light to that position
	float3 lightDir = Pl_PositionView - vsPosition;
	float distance = length(lightDir);
	lightDir /= distance; // Normalize the direction

	// Do some simple NdL-Lighting
	float ndl = max(0, dot(lightDir, litN));

	// Compute range falloff
	float falloff = PLS_ComputeRangeFalloff(distance, PL_Range);
	//float falloff = saturate(1.0f / (pow(distance / PL_Range * 2, 2)));

	// Compute specular lighting
	float3 V = normalize(-vsPosition);
	float3 H = normalize(lightDir + V);
	float spec = PLS_CalcBlinnPhongLighting(litN, H) * PL_Color.w;
	float specMod = PLS_ComputeSpecMod(diffuse.rgb);

	// Blend this with the light color, world diffuse and specular term.
	float3 lighting = saturate(PLS_ComputePointLightLighting(diffuse.rgb, PL_Color.rgb, ndl, falloff, spec, specIntensity, specPower, specMod));

	// Wet ground: water-film reflection, kept out of the clamp above so it can go HDR.
	[branch]
	if (wet.wetness > 0.0f && PL_WetLightReflections > 0.0f)
	{
		float3 coatN = normalize(mul((float3x3)PL_InvView, wet.coatN));
		lighting += PL_Color.rgb * WetCoatRolloff(WetCoatSpecular(coatN, V, lightDir, distance, wet.roughness)
			* falloff * wet.wetness * PL_WetCoatScale * PL_WetLightReflections);
	}

	return float4(lighting, 1);
}

