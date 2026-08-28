//--------------------------------------------------------------------------------------
// Depth-only vertex shader for the NVIDIA per-face cube-shadow fallback (no GS/layered indexing -
// see RequiresNvidiaTiledShadowFaceFallback / D3D11PointLight::RenderShadowCubeFacePasses). Same
// transform as VS_Ex.hlsl, but trimmed to match PS_LinDepth's expected input register layout.
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer Matrices_PerInstances : register( b1 )
{
	VS_ExConstantBuffer_PerInstance cbInstance;
};

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float3 vNormal		: NORMAL;
	float2 vTex1		: TEXCOORD0;
	float2 vTex2		: TEXCOORD1;
	float4 vDiffuse		: DIFFUSE;
};

struct VS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;

	float3 positionWorld = mul(float4(Input.vPosition,1), cbInstance.M_World).xyz;

	Output.vPosition = mul( float4(positionWorld,1), frame.M_ViewProj);
	Output.vTexcoord2 = Input.vTex2;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = Input.vDiffuse;
	Output.vNormalVS = mul(Input.vNormal, (float3x3)mul(cbInstance.M_World, frame.M_View));
	Output.vViewPosition = mul(float4(positionWorld,1), frame.M_View).xyz;

	return Output;
}
