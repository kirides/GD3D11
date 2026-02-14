#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer cbObjectTessSettings : register( b1 ) 
{ 
	float VT_TesselationFactor;
	float VT_Roundness;
	float VT_DisplacementStrength;
	float VT_Pad1;
}


struct VS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalWS		: TEXCOORD3;
	float3 vNormalVS		: TEXCOORD4;
	float3 vWorldPosition	: TEXCOORD5;
};

struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalWS		: TEXCOORD3;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float3 vWorldPosition	: TEXCOORD6;
	float4 vPosition		: SV_POSITION;
};

struct ConstantOutputType
{
    float edges[3] : SV_TessFactor;
    float inside: SV_InsideTessFactor;
};

Texture2D Texture : register( t0 );  
SamplerState Sampler : register( s0 );

static const float mTessellationFactor = 170.0f;


struct ConstantOutputType
{
    float edges[3] : SV_TessFactor;
    float inside: SV_InsideTessFactor;
};


//--------------------------------------------------------------------------------------
// Patch Constant Function
//--------------------------------------------------------------------------------------
ConstantOutputType TessPatchConstantFunction(InputPatch<VS_OUTPUT, 3> inputPatch, uint patchId : SV_PrimitiveID)
{   
    ConstantOutputType output;
  
	//float factor = max(1, (1.0f-(inputPatch[0].vViewPosition.z / 4000.0f)) * mTessellationFactor);
	//factor = min(factor, 100);
	float factor = 40.0f;
  
	float3 viewPosition[3];
	viewPosition[0] = mul(float4(inputPatch[0].vWorldPosition, 1), frame.M_View);
	viewPosition[1] = mul(float4(inputPatch[1].vWorldPosition, 1), frame.M_View);
	viewPosition[2] = mul(float4(inputPatch[2].vWorldPosition, 1), frame.M_View);
	
	float far = 12000.0f;
	float tessPow = 1 / 6.0f;
	float dist[3];
	dist[0] = pow(saturate(length(viewPosition[0]) / far), tessPow);
	dist[1] = pow(saturate(length(viewPosition[1]) / far), tessPow);
	dist[2] = pow(saturate(length(viewPosition[2]) / far), tessPow);
	
	dist[0] = max(1, 1 + H_GlobalTessFactor * (1.0f - dist[0]));
	dist[1] = max(1, 1 + H_GlobalTessFactor * (1.0f - dist[1]));
	dist[2] = max(1, 1 + H_GlobalTessFactor * (1.0f - dist[2]));
	
	
	
	// Assign tessellation levels
    output.edges[0] = 0.5f * (dist[1] + dist[2]);
    output.edges[1] = 0.5f * (dist[2] + dist[0]);
    output.edges[2] = 0.5f * (dist[0] + dist[1]);
    output.inside   = (output.edges[0] + output.edges[1] + output.edges[2]) / 3.0f;
   
    return output;
}

//--------------------------------------------------------------------------------------
// Hull Shader
//--------------------------------------------------------------------------------------
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("TessPatchConstantFunction")]
//[maxtessfactor(64.0)]
VS_OUTPUT HSMain(InputPatch<VS_OUTPUT, 3> patch, uint pointId : SV_OutputControlPointID, uint patchId : SV_PrimitiveID)
{
    VS_OUTPUT output;

	output.vTexcoord	 	= patch[pointId].vTexcoord; 	
	output.vTexcoord2		= patch[pointId].vTexcoord2;		
	output.vDiffuse			= patch[pointId].vDiffuse;			
	output.vWorldPosition 	= patch[pointId].vWorldPosition; 	
	output.vNormalVS		= patch[pointId].vNormalVS;	
	output.vNormalWS		= patch[pointId].vNormalWS;		
	//output.vViewPosition	= patch[pointId].vViewPosition;	
	//output.vPosition		= patch[pointId].vPosition;		
	
    return output;
}


[domain("tri")]
PS_INPUT DSMain(ConstantOutputType input, float3 uvwCoord : SV_DomainLocation, const OutputPatch<VS_OUTPUT, 3> patch)
{
    float4 vertexPosition;
    float2 texCoord;
	float2 texCoord2;
    float3 normalVS;
	float3 normalWS;
	float4 diffuse;
    PS_INPUT output;
    float3 viewPosition;
	float3 worldPosition;
  
    diffuse = uvwCoord.x * patch[0].vDiffuse + uvwCoord.y * patch[1].vDiffuse + uvwCoord.z * patch[2].vDiffuse;
	//vertexPosition = uvwCoord.x * patch[0].vPosition + uvwCoord.y * patch[1].vPosition + uvwCoord.z * patch[2].vPosition;
	//viewPosition = uvwCoord.x * patch[0].vViewPosition + uvwCoord.y * patch[1].vViewPosition + uvwCoord.z * patch[2].vViewPosition;
	worldPosition = uvwCoord.x * patch[0].vWorldPosition + uvwCoord.y * patch[1].vWorldPosition + uvwCoord.z * patch[2].vWorldPosition;
    texCoord = uvwCoord.x * patch[0].vTexcoord + uvwCoord.y * patch[1].vTexcoord + uvwCoord.z * patch[2].vTexcoord;
	texCoord2 = uvwCoord.x * patch[0].vTexcoord2 + uvwCoord.y * patch[1].vTexcoord2 + uvwCoord.z * patch[2].vTexcoord2;
	normalVS = uvwCoord.x * patch[0].vNormalVS + uvwCoord.y * patch[1].vNormalVS + uvwCoord.z * patch[2].vNormalVS;
	normalWS = uvwCoord.x * patch[0].vNormalWS + uvwCoord.y * patch[1].vNormalWS + uvwCoord.z * patch[2].vNormalWS;

										
	float3 vHeight = 40.0f * tex * VT_DisplacementStrength;
	worldPosition += normalWS * vHeight;
	
   /* vertexPosition.xyz += normalVS * (vHeight);
	
    output.vPosition = vertexPosition;
	output.vTexcoord2 = texCoords;
	output.vTexcoord = texCoords;
	output.vDiffuse  = diffuse;
	
	output.vWorldPosition = worldPosition;
	output.vViewPosition = viewPosition;
	
	output.vNormalWS = normalWS;
	output.vNormalVS = normalVS;*/
		
	//Output.vPosition = float4(Input.vPosition, 1);
	output.vPosition = mul( float4(worldPosition,1), frame.M_ViewProj);
	output.vTexcoord2 = texCoord2;
	output.vTexcoord = texCoord;
	output.vDiffuse  = diffuse;
	output.vNormalWS = normalWS;
	output.vNormalVS = normalVS;
	output.vViewPosition = mul(float4(worldPosition,1), frame.M_View).xyz;
	output.vWorldPosition = worldPosition;
	
    return output;
}