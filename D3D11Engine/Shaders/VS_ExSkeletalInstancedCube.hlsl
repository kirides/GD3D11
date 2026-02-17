//--------------------------------------------------------------------------------------
// Instanced Skeletal Vertex Shader for Cube Shadow Maps
// Uses an indirection buffer to map SV_InstanceID to actual instance index
//--------------------------------------------------------------------------------------

static const int NUM_MAX_BONES = 96;

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

struct SkeletalInstanceData
{
    matrix World;
    matrix PrevWorld;
    float4 Color;
    float Fatness;
    float Scale;
    uint BoneOffset;
    uint Padding;
};

StructuredBuffer<SkeletalInstanceData> InstanceBuffer : register(t10);
StructuredBuffer<matrix> BoneBuffer : register(t11);
StructuredBuffer<uint> InstanceIndexBuffer : register(t12); // Indirection: SV_InstanceID -> actual instance index

struct VS_INPUT
{
    float4 vPosition[4] : POSITION;
    float3 vNormal : NORMAL;
    float3 vBindPoseNormal : TEXCOORD0;
    float2 vTex1 : TEXCOORD1;
    uint4 BoneIndices : BONEIDS;
    float4 Weights : WEIGHTS;
};

struct VS_OUTPUT
{
    float2 vTexcoord : TEXCOORD0;
    float2 vTexcoord2 : TEXCOORD1;
    float4 vDiffuse : TEXCOORD2;
    float3 vNormalVS : TEXCOORD4;
    float3 vViewPosition : TEXCOORD5;
    float3 vWorldPosition : TEXCOORD6;
    float4 vPosition : SV_POSITION;
};

VS_OUTPUT VSMain(VS_INPUT Input, uint instanceID : SV_InstanceID)
{
    VS_OUTPUT Output;
    
    // Use indirection buffer to get actual instance index
    uint actualInstanceIndex = InstanceIndexBuffer[instanceID];
    
    // Get instance data using the actual index
    SkeletalInstanceData inst = InstanceBuffer[actualInstanceIndex];
    
    uint bone0 = inst.BoneOffset + Input.BoneIndices.x;
    uint bone1 = inst.BoneOffset + Input.BoneIndices.y;
    uint bone2 = inst.BoneOffset + Input.BoneIndices.z;
    uint bone3 = inst.BoneOffset + Input.BoneIndices.w;
    
    float3 position = float3(0, 0, 0);
    position += Input.Weights.x * mul(float4(Input.vPosition[0].xyz, 1), BoneBuffer[bone0]).xyz;
    position += Input.Weights.y * mul(float4(Input.vPosition[1].xyz, 1), BoneBuffer[bone1]).xyz;
    position += Input.Weights.z * mul(float4(Input.vPosition[2].xyz, 1), BoneBuffer[bone2]).xyz;
    position += Input.Weights.w * mul(float4(Input.vPosition[3].xyz, 1), BoneBuffer[bone3]).xyz;
    
    float3 normal = float3(0, 0, 0);
    normal += Input.Weights.x * mul(Input.vNormal, (float3x3)BoneBuffer[bone0]);
    normal += Input.Weights.y * mul(Input.vNormal, (float3x3)BoneBuffer[bone1]);
    normal += Input.Weights.z * mul(Input.vNormal, (float3x3)BoneBuffer[bone2]);
    normal += Input.Weights.w * mul(Input.vNormal, (float3x3)BoneBuffer[bone3]);
    
    position += inst.Fatness * normalize(normal);
    
    float3 positionWorld = mul(float4(position, 1), inst.World).xyz;
    
    Output.vPosition = mul(float4(positionWorld, 1), frame.M_ViewProj);
    Output.vTexcoord = Input.vTex1;
    Output.vTexcoord2 = Input.vTex1;
    Output.vDiffuse = inst.Color;
    Output.vNormalVS = mul(Input.vBindPoseNormal, (float3x3)mul(inst.World, frame.M_View));
    Output.vViewPosition = mul(float4(positionWorld, 1), frame.M_View).xyz;
    Output.vWorldPosition = positionWorld;
    
    return Output;
}