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
	matrix M_PrevWorld; // Helper for rigid motion of skeletal mesh
	float4 PI_ModelColor;
	float PI_ModelFatness;
	float3 PI_Pad1;
};

cbuffer BoneTransforms : register( b2 )
{
	matrix BT_Transforms[NUM_MAX_BONES];
};

cbuffer PrevBoneTransforms : register( b3 )
{
	matrix BT_PrevTransforms[NUM_MAX_BONES];
};

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
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vPosition		: SV_POSITION;
};

void ApplySkinning(
    in float4 vPosition[4], 
    in float3 vNormal, 
    in uint4 boneIndices, 
    in float4 weights, 
    in matrix transforms[NUM_MAX_BONES], 
    out float3 skinnedPos, 
    out float3 skinnedNormal)
{
    skinnedPos = float3(0, 0, 0);
    skinnedNormal = float3(0, 0, 0);

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        uint boneIndex = boneIndices[i];
        float weight = weights[i];

        skinnedPos += weight * mul(float4(vPosition[i].xyz, 1.0f), transforms[boneIndex]).xyz;
        skinnedNormal += weight * mul(vNormal, (float3x3)transforms[boneIndex]);
    }
}

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	float3 position, prevPosition;
    float3 normal, prevNormal;

    ApplySkinning(Input.vPosition, Input.vNormal, Input.BoneIndices, Input.Weights, BT_Transforms, position, normal);

    ApplySkinning(Input.vPosition, Input.vNormal, Input.BoneIndices, Input.Weights, BT_PrevTransforms, prevPosition, prevNormal);

    // 3. Apply fatness and world transforms
    float3 positionWorld = mul(float4(position + PI_ModelFatness * normal, 1), M_World).xyz;
    float3 prevPositionWorld = mul(float4(prevPosition + PI_ModelFatness * prevNormal, 1), M_PrevWorld).xyz;
	
	//Output.vPosition = float4(Input.vPosition, 1);
	Output.vPosition = mul(float4(positionWorld,1), frame.M_ViewProj);
	Output.vTexcoord2 = Input.vTex1;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = PI_ModelColor;
	Output.vNormalVS = mul(Input.vBindPoseNormal, (float3x3)mul(M_World, frame.M_View));
	Output.vViewPosition = mul(float4(positionWorld,1), frame.M_View).xyz;

	// Motion Vectors - use UNJITTERED matrices for correct velocity
	Output.vCurrClipPos = mul(float4(positionWorld, 1.0), frame.M_UnjitteredViewProj);
	Output.vPrevClipPos = mul(float4(prevPositionWorld, 1.0), frame.M_PrevViewProj);
	
	//Output.vWorldPosition = positionWorld;
	
	return Output;
}
