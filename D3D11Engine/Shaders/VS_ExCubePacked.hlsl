//--------------------------------------------------------------------------------------
// Packed variant of VS_ExCube.hlsl for the 36-byte VOB vertex (ExVertexStructGPU).
// Same output signature as VS_ExCube (so it links with the same cube-shadow GS/PS);
// only the vertex input decode differs. See VertexPacking.h.
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"
#include "VertexPacking.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer Matrices_PerInstances : register( b1 )
{
	matrix M_World;
};


//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float2 vNormalOct	: NORMAL;    // octahedral-encoded (R16G16_SNORM)
	float4 vTangent		: TANGENT;   // R10G10B10A2 (unused here, present for layout match)
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
	float3 vWorldPosition	: TEXCOORD4;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;

	float3 vNormal = DecodeOctNormal( Input.vNormalOct );

	float3 positionWorld = mul(float4(Input.vPosition,1), M_World).xyz;

	Output.vTexcoord2 = Input.vTex2;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = Input.vDiffuse;
	Output.vNormalWS = mul(vNormal, (float3x3)M_World);
	Output.vWorldPosition = positionWorld;

	return Output;
}
