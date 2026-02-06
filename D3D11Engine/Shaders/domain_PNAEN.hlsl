#define TRI_SAMPLE_LEVEL 0
#include <Triplanar.h>

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer MI_MaterialInfo : register( b2 )
{
	float MI_SpecularIntensity;
	float MI_SpecularPower;
	float MI_NormalmapStrength;
	float MI_DisplacementFactor;
	
	float MI_TextureScale;
	float3 MI_Pad;
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
	float4 vCurrClipPos     : TEXCOORD7;
	float4 vPrevClipPos     : TEXCOORD8;
	float4 vPosition		: SV_POSITION;
};

struct ConstantOutputType
{
    float edges[3] : SV_TessFactor;
    float inside: SV_InsideTessFactor;
};

Texture2D Texture : register( t0 );  
SamplerState Sampler : register( s0 );

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
	
	float scale = (1/1500.0f) * MI_TextureScale;
	float tex = GetTriPlanarTexture(Texture, Texture, Texture, Sampler,
										scale * 1.0f, 
										worldPosition, 
										normalWS).a * 2 - 1;
										
	float3 vHeight = 40.0f * tex * MI_DisplacementFactor;
	worldPosition += normalWS * vHeight * (1 - pow(1 - texCoord2.x, 8.0f));
	
		
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