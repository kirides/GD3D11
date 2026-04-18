//--------------------------------------------------------------------------------------
// Instanced vertex shader for node attachments (non-MorphMesh)
// Based on VS_ExNode.hlsl but reads per-instance data from vertex stream
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

	// Per-instance data from vertex stream slot 1
	float4x4 InstanceWorld     : INSTANCE_WORLD_MATRIX;
	float4x4 InstancePrevWorld : INSTANCE_PREV_WORLD_MATRIX;
	float4   InstanceColor     : INSTANCE_COLOR;
};

struct VS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;  // Current clip position for velocity
	float4 vPrevClipPos     : TEXCOORD7;  // Previous clip position for velocity
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	// Non-MorphMesh: Fatness=0, Scaling=1
	float3 positionWorld = mul(float4(Input.vPosition, 1), Input.InstanceWorld).xyz;
	
	Output.vPosition = mul( float4(positionWorld,1), frame.M_ViewProj);
	Output.vTexcoord2 = Input.vTex2;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = Input.InstanceColor;
	Output.vNormalVS = mul(Input.vNormal, (float3x3)mul(Input.InstanceWorld, frame.M_View));
	Output.vViewPosition = mul(float4(positionWorld,1), frame.M_View);
	
	// Motion Vectors - use UNJITTERED matrices for correct velocity
	Output.vCurrClipPos = mul(float4(positionWorld, 1), frame.M_UnjitteredViewProj);
	float3 prevPositionWorld = mul(float4(Input.vPosition, 1), Input.InstancePrevWorld).xyz;
	Output.vPrevClipPos = mul(float4(prevPositionWorld, 1), frame.M_PrevViewProj);
	
	return Output;
}
