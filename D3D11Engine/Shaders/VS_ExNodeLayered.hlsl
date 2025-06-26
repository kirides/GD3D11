//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

cbuffer Matrices_PerFrame : register( b0 )
{
	matrix M_View;
	matrix M_Proj;
	matrix M_ViewProj;	
};

cbuffer Matrices_PerInstances : register( b1 )
{
	matrix M_World;
	float4 M_Color;
	float M_Fatness;
	float M_Scaling;
	float2 M_Pad1;
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
    float4 vPosition : SV_POSITION;
    uint RTIndex : SV_RenderTargetArrayIndex;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	float3 positionWorld = mul(float4((Input.vPosition + M_Fatness * Input.vNormal) * M_Scaling, 1), M_World).xyz;
	
    Output.RTIndex = Input.instanceID;
    Output.vPosition = mul(float4(positionWorld, 1), PCR_ViewProj[Input.instanceID]);
	Output.vTexcoord2 = Input.vTex2;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = M_Color;
    Output.vNormalVS = mul(Input.vNormal, (float3x3)mul(M_World, PCR_View[Input.instanceID]));
    Output.vViewPosition = mul(float4(positionWorld, 1), PCR_View[Input.instanceID]);
	
	return Output;
}

