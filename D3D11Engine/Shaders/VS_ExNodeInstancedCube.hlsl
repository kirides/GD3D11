//--------------------------------------------------------------------------------------
// Instanced Node Attachment Vertex Shader for Cube Shadow Maps
// For weapons, heads, and other attachments
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

// Per-instance data from StructuredBuffer
struct NodeAttachmentInstance
{
    matrix World;
    float4 Color;
    float Fatness;
    float Scaling;
    float2 Padding;
};

StructuredBuffer<NodeAttachmentInstance> InstanceBuffer : register(t10);

struct VS_INPUT
{
    float3 vPosition : POSITION;
    float3 vNormal : NORMAL;
    float2 vTex1 : TEXCOORD0;
    float2 vTex2 : TEXCOORD1;
    float4 vDiffuse : DIFFUSE;
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
    
    // Get instance data
    NodeAttachmentInstance inst = InstanceBuffer[instanceID];
    
    // Apply scaling and fatness
    float3 position = Input.vPosition * inst.Scaling;
    position += Input.vNormal * inst.Fatness;
    
    // Transform to world space
    float3 positionWorld = mul(float4(position, 1), inst.World).xyz;
    
    // Output
    Output.vPosition = mul(float4(positionWorld, 1), frame.M_ViewProj);
    Output.vTexcoord = Input.vTex1;
    Output.vTexcoord2 = Input.vTex2;
    Output.vDiffuse = inst.Color;
    Output.vNormalVS = mul(Input.vNormal, (float3x3)mul(inst.World, frame.M_View));
    Output.vViewPosition = mul(float4(positionWorld, 1), frame.M_View).xyz;
    Output.vWorldPosition = positionWorld; // Needed for cube shadow map distance calculation
    
    return Output;
}