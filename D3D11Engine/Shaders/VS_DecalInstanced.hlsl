//--------------------------------------------------------------------------------------
// Instanced decal vertex shader
// Same math as VS_Decal.hlsl, but the per-decal world-view matrix is read from the
// per-instance vertex stream (slot 1) instead of a constant buffer. This lets us batch
// all decals that share a zCMaterial* into a single instanced draw call.
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
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

	// Per-instance world-view matrix from vertex stream slot 1
	float4x4 InstanceWorldView : INSTANCE_WORLD_MATRIX;
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

	float3 positionView = mul(float4(Input.vPosition,1), Input.InstanceWorldView).xyz;

	Output.vPosition = mul( float4(positionView,1), frame.M_Proj);
	Output.vTexcoord2 = Input.vTex2;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = Input.vDiffuse;
	Output.vNormalVS = mul(Input.vNormal, (float3x3)Input.InstanceWorldView);
	Output.vViewPosition = positionView;

	return Output;
}
