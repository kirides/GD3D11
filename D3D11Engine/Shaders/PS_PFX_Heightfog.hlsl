//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

#include <AtmosphericScattering.h>
#include "DepthReconstruction.h"

cbuffer PFXBuffer : register( b0 )
{
	float4 HF_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
	matrix HF_InvView;
	float3 HF_CameraPosition;
	float HF_FogHeight;

	float HF_HeightFalloff;
	float HF_GlobalDensity;
	float HF_WeightZNear;
	float HF_WeightZFar;

	float3 HF_FogColorMod;
	float HF_pad2;

	float2 HF_ProjAB;
	float2 HF_Pad3;

	float HF_GlobalDistanceDensity;
	float HF_GlobalDistanceStart;
	float HF_GlobalDistanceRange;
	float HF_MaxOpacity;

	float HF_SecondaryFogHeight;
	float HF_SecondaryHeightFalloff;
	float HF_SecondaryGlobalDensity;
	float HF_SecondaryWeight;

	float3 HF_GlobalDistanceColorMod;
	float HF_SwampBlend;

	float3 HF_SecondaryFogColorMod;
	float HF_SkyDepthThreshold;

	float HF_SkyDepthFadeWidth;
	float3 HF_pad4;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_Depth : register( t1 );

float3 VSPositionFromDepth(float depth, float2 vTexCoord)
{
	return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, HF_ProjParams.xy );
}

float ComputeHeightLayerTransmittance(float3 cameraToWorldPos, float3 posOriginal, float fogHeight, float heightFalloff, float globalDensity)
{	
	float3 layerPos = cameraToWorldPos;
	layerPos.y -= fogHeight;
	float cVolFogHeightDensityAtViewer = exp( -heightFalloff );

	float lenOrig = length(posOriginal - HF_CameraPosition);
	float len = length(layerPos);
	float fogInt = len * cVolFogHeightDensityAtViewer;
	const float cSlopeThreshold = 0.01;
	
	float zRange = max(HF_WeightZFar - HF_WeightZNear, 0.0001);
	float w = saturate((lenOrig - HF_WeightZNear) / zRange);

	if (abs(layerPos.y) > cSlopeThreshold)
	{
		float t = heightFalloff * layerPos.y * w;
		fogInt *= (abs(t) > 0.0001 ? ((1.0 - exp(-t)) / t) : 1.0);
	}

	return exp(-max(0.0, globalDensity) * w * fogInt);
}

float ComputeDistanceTransmittance(float3 posOriginal)
{
	float distanceToCamera = length(posOriginal - HF_CameraPosition);
	float distanceRange = max(HF_GlobalDistanceRange, 1.0);
	float distanceWeight = saturate((distanceToCamera - HF_GlobalDistanceStart) / distanceRange);
	return exp(-max(0.0, HF_GlobalDistanceDensity) * distanceWeight * distanceToCamera);
}

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
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float expDepth = TX_Depth.Sample(SS_Linear, Input.vTexcoord).r;
	float skyFadeEnd = HF_SkyDepthThreshold + max(HF_SkyDepthFadeWidth, 0.000001f);
	float skyMask = smoothstep(HF_SkyDepthThreshold, skyFadeEnd, expDepth);
	if (skyMask <= 0.0001f)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	float3 worldPos = VSPositionFromDepth(expDepth, Input.vTexcoord);
	worldPos = mul(float4(worldPos, 1), HF_InvView).xyz;
	float3 cameraToWorldPos = worldPos - HF_CameraPosition;

	float baseTrans = ComputeHeightLayerTransmittance(cameraToWorldPos, worldPos, HF_FogHeight, HF_HeightFalloff, HF_GlobalDensity);
	float secondaryRawTrans = ComputeHeightLayerTransmittance(cameraToWorldPos, worldPos, HF_SecondaryFogHeight, HF_SecondaryHeightFalloff, HF_SecondaryGlobalDensity);
	float secondaryWeight = saturate(HF_SecondaryWeight);
	float secondaryTrans = lerp(1.0, secondaryRawTrans, secondaryWeight);
	float distanceTrans = ComputeDistanceTransmittance(worldPos);

	float totalTrans = saturate(baseTrans * secondaryTrans * distanceTrans);
	float fog = saturate(1.0 - totalTrans) * saturate(HF_MaxOpacity) * skyMask;

	float baseFog = 1.0 - baseTrans;
	float secondaryFog = (1.0 - secondaryRawTrans) * secondaryWeight;
	float distanceFog = 1.0 - distanceTrans;
	float weightSum = max(baseFog + secondaryFog + distanceFog, 0.0001);
	float3 fogTint = (
		HF_FogColorMod * baseFog +
		HF_SecondaryFogColorMod * secondaryFog +
		HF_GlobalDistanceColorMod * distanceFog) / weightSum;

	fogTint = lerp(fogTint, HF_SecondaryFogColorMod, saturate(HF_SwampBlend * 0.25));

	float3 color = ApplyAtmosphericScatteringGround(cameraToWorldPos, fogTint, true);

	// darken / lighten fog based on the day / night cycle
	float3 nightFogColor = float3(0.12f, 0.18f, 0.27f);
	float nightTimeBlend = saturate(-AC_LightPos.y * 4.0f);
	color = lerp(color, nightFogColor, nightTimeBlend);

	// Starts darker and brightens with daylight.
	float darknessFactor = 2.0f; 
	if (AC_LightPos.y > 0.0f) { 
		darknessFactor -= (AC_LightPos.y * 0.8f); 
	}

	return float4(saturate(color / max(darknessFactor, 0.2f)), fog);
}

