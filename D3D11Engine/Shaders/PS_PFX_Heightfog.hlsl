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

float ComputeVolumetricFog(float3 cameraToWorldPos, float3 posOriginal)
{	
	float cVolFogHeightDensityAtViewer = exp( -HF_HeightFalloff );
	
	float lenOrig = length(posOriginal - HF_CameraPosition);
	float len = length(cameraToWorldPos);
	float fogInt = len * cVolFogHeightDensityAtViewer;
	const float	cSlopeThreshold = 0.01;
	
	float w = saturate((lenOrig-HF_WeightZNear)/(HF_WeightZFar-HF_WeightZNear));

	if(abs( cameraToWorldPos.y ) > cSlopeThreshold )
	{
		float t = HF_HeightFalloff * cameraToWorldPos.y * w;
		fogInt *= (abs( t ) > 0.0001 ? ( ( 1.0 - exp( -t ) ) / t ) : 1.0);
	}
	
	
	
	return exp( -HF_GlobalDensity * w * fogInt );
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
	
	float3 position = VSPositionFromDepth(expDepth, Input.vTexcoord);
	
	
	position = mul(float4(position, 1), HF_InvView).xyz;
	float3 posOriginal = position;
	
	position -= HF_CameraPosition;
	
	position.y -= HF_FogHeight;
	
	float fog = 1.0f - ComputeVolumetricFog(position, posOriginal);
		
	float3 color = ApplyAtmosphericScatteringGround(position, HF_FogColorMod, true);

	//darken / lighten fog based on the day / night cycle
	// (Increased the R, G, B values. Tweak these up/down if you want it brighter/darker!)
	float3 nightFogColor = float3(0.04f, 0.06f, 0.09f); 
	        nightFogColor = float3(0.12f, 0.18f, 0.27f); 
	float nightTimeBlend = saturate(-AC_LightPos.y * 4.0f);
	color = lerp(color, nightFogColor, nightTimeBlend);

	// Starts darker (2.5) and doesn't drop as much at noon.
	float darknessFactor = 2.0f; 
	if (AC_LightPos.y > 0.0f) { 
		darknessFactor -= (AC_LightPos.y * 0.8f); 
	}
	
	// Never let the fog become a 100% solid wall of color.
	float maxFogOpacity = 0.85f;

	return float4(saturate(color / darknessFactor), saturate(fog) * maxFogOpacity);
}

