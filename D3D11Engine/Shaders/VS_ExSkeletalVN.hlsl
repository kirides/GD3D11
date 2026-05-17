//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

static const int NUM_MAX_BONES = 96;

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer Matrices_PerInstances : register( b1 )
{
	matrix M_World;
	float4 PI_ModelColor;
	float PI_ModelFatness;
	float3 PI_Pad1;
};

#if SKINNING_STRUCTURED
StructuredBuffer<float4x4> BoneTransforms : register( t0 );

cbuffer BoneTransformRange : register( b2 )
{
	uint BT_BoneOffset;
	uint BT_PrevBoneOffset;
	uint BT_BoneCount;
	uint BT_UseStructuredBones;
};

#define BT_CURR(idx) BoneTransforms[BT_BoneOffset + (idx)]
#else
cbuffer BoneTransforms : register( b2 )
{
	matrix BT_Transforms[NUM_MAX_BONES];
};

#define BT_CURR(idx) BT_Transforms[(idx)]
#endif

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float4 vPosition[4]	: POSITION;
	float3 vNormal		: NORMAL;
	float3 vBindPoseNormal		: TEXCOORD0;
	float2 vTex1		: TEXCOORD1;
	uint4 BoneIndices : BONEIDS;
	float4 Weights 	: WEIGHTS;
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
	
	float3 position = float3(0, 0, 0);
	position += Input.Weights.x * mul(float4(Input.vPosition[0].xyz, 1), BT_CURR(Input.BoneIndices.x)).xyz;
	position += Input.Weights.y * mul(float4(Input.vPosition[1].xyz, 1), BT_CURR(Input.BoneIndices.y)).xyz;
	position += Input.Weights.z * mul(float4(Input.vPosition[2].xyz, 1), BT_CURR(Input.BoneIndices.z)).xyz;
	position += Input.Weights.w * mul(float4(Input.vPosition[3].xyz, 1), BT_CURR(Input.BoneIndices.w)).xyz;
	
	float3 normal = float3(0, 0, 0);
	normal += Input.Weights.x * mul(Input.vNormal, (float3x3)BT_CURR(Input.BoneIndices.x));
	normal += Input.Weights.y * mul(Input.vNormal, (float3x3)BT_CURR(Input.BoneIndices.y));
	normal += Input.Weights.z * mul(Input.vNormal, (float3x3)BT_CURR(Input.BoneIndices.z));
	normal += Input.Weights.w * mul(Input.vNormal, (float3x3)BT_CURR(Input.BoneIndices.w));
	
	Output.vPosition = float4(position, 1);
	Output.vTexcoord2 = Input.vTex1;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = PI_ModelColor;
	Output.vNormalVS = normal;
	Output.vViewPosition = Input.vBindPoseNormal;
	
	return Output;
}
