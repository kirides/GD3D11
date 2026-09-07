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
	// NodeAttachmentInstanceData: World/PrevWorld are three rows each; ColorFlags packs RGB in bytes 0-2
	// and flag bits in byte 3 (bit 7 = focus highlight).
	float4 InstanceWorld0     : INSTANCE_WORLD_MATRIX0;
	float4 InstanceWorld1     : INSTANCE_WORLD_MATRIX1;
	float4 InstanceWorld2     : INSTANCE_WORLD_MATRIX2;
	uint4  InstanceColorFlags : INSTANCE_COLOR;
	float4 InstancePrevWorld0 : INSTANCE_PREV_WORLD_MATRIX0;
	float4 InstancePrevWorld1 : INSTANCE_PREV_WORLD_MATRIX1;
	float4 InstancePrevWorld2 : INSTANCE_PREV_WORLD_MATRIX2;
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
	float4 vTangent			: TEXCOORD3;  // no precomputed tangent -> zero (PS falls back to ddx/ddy)
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	// Non-MorphMesh: Fatness=0, Scaling=1
	float3x4 nodeWorld = float3x4(Input.InstanceWorld0, Input.InstanceWorld1, Input.InstanceWorld2);
	float3 positionWorld = mul(nodeWorld, float4(Input.vPosition, 1));
	
	Output.vPosition = mul( float4(positionWorld,1), frame.M_ViewProj);
	Output.vTexcoord2 = Input.vTex2;
	Output.vTexcoord = Input.vTex1;
	// .w carried the 2.0 focus sentinel before the pack; regenerate it from the flag bit.
	Output.vDiffuse  = float4(Input.InstanceColorFlags.rgb / 255.0,
		(Input.InstanceColorFlags.a & 0x80) ? 2.0 : 0.0);
	Output.vNormalVS = mul(mul((float3x3)nodeWorld, Input.vNormal), (float3x3)frame.M_View);
	Output.vViewPosition = mul(float4(positionWorld,1), frame.M_View);
	
	// Motion Vectors - use UNJITTERED matrices for correct velocity
	Output.vCurrClipPos = mul(float4(positionWorld, 1), frame.M_UnjitteredViewProj);
	float3 prevPositionWorld = mul(float3x4(Input.InstancePrevWorld0, Input.InstancePrevWorld1, Input.InstancePrevWorld2), float4(Input.vPosition, 1));
	Output.vPrevClipPos = mul(float4(prevPositionWorld, 1), frame.M_PrevViewProj);
	Output.vTangent = float4(0,0,0,0);

	return Output;
}
