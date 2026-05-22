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
	float2 PL_Pad2;
	
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
SamplerComparisonState SS_Comp : register( s2 );
Texture2D	TX_Diffuse : register( t0 );
Texture2D	TX_Nrm : register( t1 );
Texture2D	TX_Depth : register( t2 );
TextureCube	TX_ShadowCube : register( t3 );
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
	return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, PL_ProjParams.xy );
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
	
	//return float4(normalize(wsPosition - Pl_PositionWorld), 1.0f);
	
	// Compute flat normal
	//float3 flatNormal = normalize(cross(ddx(vsPosition),ddy(vsPosition)));
	
	//if(Input.vPosition.z > expDepth)
	//	discard;
	
	//return float4(Pl_PositionView, 1);
	
	// Get direction and distance from the light to that position
	float3 lightDir = Pl_PositionView - vsPosition;
	float distance = length(lightDir);
	lightDir /= distance; // Normalize the direction
	
	// Do some simple NdL-Lighting
	float ndl = max(0, dot(lightDir, normal));
	
	// Apply dynamic shadow
	float shadow = PLS_SampleShadowCube(TX_ShadowCube, SS_Comp, wsPosition, wsNormal, Pl_PositionWorld, PL_Range);
	//return float4(ndl.rrr,1);
	
	// Get rid of lighting on the backfaces of normalmapped surfaces
	//ndl *= saturate(dot(lightDir, flatNormal)  / 0.00001f);
	
	// Compute range falloff
	float falloff = PLS_ComputeRangeFalloff(distance, PL_Range);
	
	// Compute specular lighting
	float3 V = normalize(-vsPosition);
	float3 H = normalize(lightDir + V);
	float spec = PLS_CalcBlinnPhongLighting(normal, H);
	float specMod = PLS_ComputeSpecMod(diffuse.rgb);
	float3 lighting = PLS_ComputePointLightLighting(diffuse.rgb, PL_Color.rgb, ndl, falloff, spec, specIntensity, specPower, specMod);
	
	lighting *= shadow;
	
	//lighting = GetShadow(uv);
	
	// If this is an indoor-light, and this pixel already gets light from the sun, don't light it here.
	//float indoor = 1.0f - PL_Outdoor;
	//float indoorPixel = diffuse.a < 0.5f ? 1.0f : 0.0f;
	//lighting *= saturate(PL_Outdoor + indoor * indoorPixel);
	//lighting = indoorPixel;
	
	//return float4(0.2f,0.2f,0.2f,1);
	//return float4(ndl.rrr,1);
	return float4(saturate(lighting),1);
}

