//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer Matrices_PerInstances : register( b1 )
{
	matrix M_World;
};

SamplerState SS_Linear : register( s0 );
Texture2D TX_Texture0 : register( t0 );

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
	float3 vNormalWS		: TEXCOORD3;
	float3 vNormalVS		: TEXCOORD4;
	float3 vWorldPosition	: TEXCOORD5;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	//float height = TX_Texture0.SampleLevel(SS_Linear, Input.vTex1, 0).a * 2 - 1;
	//float noise = 1-TX_Texture0.SampleLevel(SS_Linear, Input.vTex1, 0).r;
	//Input.vPosition += Input.vNormal * height * 32.0f * Input.vTex2.x;
	
	float3 positionWorld = mul(float4(Input.vPosition,1), M_World).xyz;
	
	//Output.vPosition = float4(Input.vPosition, 1);
	Output.vTexcoord2 = Input.vTex2;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = Input.vDiffuse;
	Output.vNormalWS = mul(Input.vNormal, (float3x3)M_World);
	Output.vNormalVS = mul(Input.vNormal, (float3x3)mul(M_World, frame.M_View));
	Output.vWorldPosition = positionWorld;
	
	return Output;
}

