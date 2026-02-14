//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer GrassCB : register( b1 )
{
	float3 G_NormalVS;
	float G_Time;
	float G_WindStrength;
	float3 G_Pad1;
};

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float2 vTex1		: TEXCOORD0;
	float4x4 InstanceWorldMatrix : INSTANCE_WORLD_MATRIX;
};

struct VS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float3 vNormalVS		: TEXCOORD1;
	float3 vWorldPosition	: TEXCOORD2;
	float4 vCurrClipPos     : TEXCOORD3;  // Current clip position for velocity
	float4 vPrevClipPos     : TEXCOORD4;  // Previous clip position for velocity
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	float3 wpos = mul(float4(Input.vPosition,1), Input.InstanceWorldMatrix).xyz;
	
	float wind = sin(Input.vPosition.z * 0.001f) * 0.5f + 0.5f;
	wind += sin(Input.vPosition.x * 0.001f) * 0.5f + 0.5f;
	wind += 0.2f;
	
	
	wpos.xz += sin(G_Time + wind) * 2.0f * Input.vPosition.y * G_WindStrength;
	wpos.xz += sin(G_Time * 3.0f + wind) * 1.55f * Input.vPosition.y * G_WindStrength;
	wpos.xz += sin(G_Time * 5.0f + wind) * 1.2f * Input.vPosition.y * G_WindStrength;
	
	Output.vPosition = mul( float4(wpos,1), frame.M_ViewProj);
	Output.vTexcoord = Input.vTex1;
	Output.vNormalVS = G_NormalVS;
	Output.vWorldPosition = wpos;

	// Motion Vectors - grass uses static world position for velocity
	// Wind animation is not tracked per-frame, so we output zero velocity
	Output.vCurrClipPos = mul(float4(wpos, 1.0), frame.M_UnjitteredViewProj);
	Output.vPrevClipPos = mul(float4(wpos, 1.0), frame.M_PrevViewProj);

	return Output;
}

