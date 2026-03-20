//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer DS_PointLightConstantBuffer : register( b1 )
{
	float4 PL_Color;
	
	float PL_Range;
	float3 PL_PositionWorld;
	
	float PL_Pad1;
	float3 PL_PositionView;

	float4 PL_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
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
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	float3 positionWorld = (Input.vPosition * PL_Range) + PL_PositionWorld;
	
	Output.vPosition = mul( float4(positionWorld,1), frame.M_ViewProj);	
	return Output;
}

