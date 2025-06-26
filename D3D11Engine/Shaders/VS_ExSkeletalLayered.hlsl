//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

static const int NUM_MAX_BONES = 96;

cbuffer Matrices_PerFrame : register( b0 )
{
	matrix M_View;
	matrix M_Proj;
	matrix M_ViewProj;	
};

cbuffer Matrices_PerInstances : register( b1 )
{
	matrix M_World;
	float4 PI_ModelColor;
	float PI_ModelFatness;
	float3 PI_Pad1;
};

cbuffer BoneTransforms : register( b2 )
{
	matrix BT_Transforms[NUM_MAX_BONES];
};

cbuffer cbPerCubeRender : register( b3 )
{
    matrix PCR_View[6];
    matrix PCR_ViewProj[6];
};

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
    uint instanceID : SV_InstanceID;
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
    float4 vPosition : SV_POSITION;
    uint RTIndex : SV_RenderTargetArrayIndex;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	float3 position = float3(0, 0, 0);
	position += Input.Weights.x * mul(float4(Input.vPosition[0].xyz, 1), BT_Transforms[Input.BoneIndices.x]).xyz;
	position += Input.Weights.y * mul(float4(Input.vPosition[1].xyz, 1), BT_Transforms[Input.BoneIndices.y]).xyz;
	position += Input.Weights.z * mul(float4(Input.vPosition[2].xyz, 1), BT_Transforms[Input.BoneIndices.z]).xyz;
	position += Input.Weights.w * mul(float4(Input.vPosition[3].xyz, 1), BT_Transforms[Input.BoneIndices.w]).xyz;
	
	float3 normal = float3(0, 0, 0);
	normal += Input.Weights.x * mul(Input.vNormal, (float3x3)BT_Transforms[Input.BoneIndices.x]);
	normal += Input.Weights.y * mul(Input.vNormal, (float3x3)BT_Transforms[Input.BoneIndices.y]);
	normal += Input.Weights.z * mul(Input.vNormal, (float3x3)BT_Transforms[Input.BoneIndices.z]);
	normal += Input.Weights.w * mul(Input.vNormal, (float3x3)BT_Transforms[Input.BoneIndices.w]);
	
	float3 positionWorld = mul(float4(position + PI_ModelFatness * normal, 1), M_World).xyz;
	
    Output.RTIndex = Input.instanceID;
    Output.vPosition = mul(float4(positionWorld, 1), PCR_ViewProj[Input.instanceID]);
	Output.vTexcoord2 = Input.vTex1;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = PI_ModelColor;
    Output.vNormalVS = mul(Input.vBindPoseNormal, (float3x3)mul(M_World, PCR_View[Input.instanceID]));
    Output.vViewPosition = mul(float4(positionWorld, 1), PCR_View[Input.instanceID]).xyz;
	
	return Output;
}
