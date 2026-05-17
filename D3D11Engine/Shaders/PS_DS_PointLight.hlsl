//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <DS_Defines.h>
#include "DepthReconstruction.h"
#include "include/PointLightShadows.h"

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
	matrix PL_InvView; // Optimize out!
	
	float3 PL_LightScreenPos;
	float PL_Pad3;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Diffuse : register( t0 );
Texture2D	TX_Nrm : register( t1 );
Texture2D	TX_Depth : register( t2 );
Texture2D	TX_SI_SP : register( t7 );

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
	float roughness = gb3.x;
	float metallic = gb3.y;
	
	// Reconstruct VS World Position from depth
	float expDepth = TX_Depth.Sample(SS_Linear, uv).r;
	float3 vsPosition = VSPositionFromDepth(expDepth, uv);
	
	// Get direction and distance from the light to that position
	float3 lightDir = Pl_PositionView - vsPosition;
	float distance = length(lightDir);
	lightDir /= distance; // Normalize the direction
	
	// Compute range falloff
	float falloff = PLS_ComputeRangeFalloff(distance, PL_Range);
	//float falloff = saturate(1.0f / (pow(distance / PL_Range * 2, 2)));

	// Compute physically-based direct lighting
	float3 V = normalize(-Pl_PositionView);
	float3 lighting = PLS_ComputePointLightLightingPBR(diffuse.rgb, PL_Color.rgb, normal, V, lightDir, falloff, roughness, metallic);
	
	return float4(saturate(lighting),1);
}

