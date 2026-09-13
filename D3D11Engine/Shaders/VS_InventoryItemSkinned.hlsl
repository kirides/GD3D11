//--------------------------------------------------------------------------------------
// Inventory item preview, skinned meshes (UIRenderer2D item batches)
//--------------------------------------------------------------------------------------

static const int NUM_MAX_BONES = 96;

#include "InventoryItem.h"

#if SKINNING_STRUCTURED
StructuredBuffer<float4x4> BoneTransforms : register( t0 );
StructuredBuffer<float4x4> PrevBoneTransforms : register( t1 );

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

cbuffer PrevBoneTransforms : register( b3 )
{
	matrix BT_PrevTransforms[NUM_MAX_BONES];
};

#define BT_CURR(idx) BT_Transforms[(idx)]
#endif

struct VS_INPUT
{
	float4 vPosition[4]		: POSITION;
	float3 vNormal			: NORMAL;
	float3 vBindPoseNormal	: TEXCOORD0;
	float2 vTex1			: TEXCOORD1;
	uint4 BoneIndices		: BONEIDS;
	float4 Weights			: WEIGHTS;
	float4 vClipRow0		: INSTANCE_CLIP0;
	float4 vClipRow1		: INSTANCE_CLIP1;
	float4 vClipRow2		: INSTANCE_CLIP2;
	float4 vClipRow3		: INSTANCE_CLIP3;
	float4 vRemap			: INSTANCE_REMAP;
};

// Matches PS_Preview.hlsl's input; the clip distances ride along at the end.
struct VS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vTangent			: TEXCOORD3;
	float4 vPosition		: SV_POSITION;
	float4 vClip			: SV_ClipDistance0;
};

VS_OUTPUT VSMain( VS_INPUT Input )
{
	float3 position = float3( 0, 0, 0 );
	[unroll]
	for ( int i = 0; i < 4; ++i )
	{
		position += Input.Weights[i] * mul( float4( Input.vPosition[i].xyz, 1.0f ), BT_CURR( Input.BoneIndices[i] ) ).xyz;
	}

	ItemProjection projection = ProjectItem( position, Input.vClipRow0, Input.vClipRow1, Input.vClipRow2, Input.vClipRow3, Input.vRemap );

	VS_OUTPUT Output;
	Output.vPosition = projection.Position;
	Output.vClip = projection.ClipDistance;
	Output.vTexcoord = Input.vTex1;
	Output.vTexcoord2 = Input.vTex1;
	Output.vDiffuse = float4( 1, 1, 1, 1 );
	Output.vNormalVS = float3( 0, 0, 0 );
	Output.vViewPosition = float3( 0, 0, 0 );
	Output.vCurrClipPos = projection.Position;
	Output.vPrevClipPos = projection.Position;
	Output.vTangent = float4( 0, 0, 0, 0 );
	return Output;
}
