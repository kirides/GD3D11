//--------------------------------------------------------------------------------------
// Depth-only vertex shader for the NVIDIA per-face cube-shadow fallback (no GS/layered indexing -
// see RequiresNvidiaTiledShadowFaceFallback / D3D11PointLight::RenderShadowCubeFacePasses).
// Used for NPC bodies instead of VS_ExSkeletal - see VS_ExLinDepth.hlsl for why. No previous-frame
// skinning: motion vectors aren't needed for a depth-only shadow pass.
//--------------------------------------------------------------------------------------

static const int NUM_MAX_BONES = 96;

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer Matrices_PerInstances : register( b1 )
{
	VS_ExConstantBuffer_PerInstanceSkeletal cbInstance;
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
	float3 normal = float3(0, 0, 0);

	[unroll]
	for ( int i = 0; i < 4; ++i ) {
		uint boneIndex = Input.BoneIndices[i];
		float weight = Input.Weights[i];

		float4x4 boneTransform = BT_CURR( boneIndex );
		position += weight * mul(float4(Input.vPosition[i].xyz, 1.0f), boneTransform).xyz;
		normal += weight * mul(Input.vNormal, (float3x3)boneTransform);
	}

	float3 positionWorld = mul(float4(position + cbInstance.PI_ModelFatness * normal, 1), cbInstance.M_World).xyz;

	Output.vPosition = mul(float4(positionWorld,1), frame.M_ViewProj);
	Output.vTexcoord2 = Input.vTex1;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = cbInstance.PI_ModelColor;
	Output.vDiffuse.w  = cbInstance.PI_Pad1.x;
	Output.vNormalVS = mul(Input.vBindPoseNormal, (float3x3)mul(cbInstance.M_World, frame.M_View));
	Output.vViewPosition = mul(float4(positionWorld,1), frame.M_View).xyz;

	return Output;
}
