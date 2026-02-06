#define TRI_SAMPLE_LEVEL 0
#include <Triplanar.h>

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer OceanSettings : register( b1 )
{
	float3 OS_CameraPosition;
	float OS_SpecularPower;
	
	// Water-reflected sky color
	float3			OS_SkyColor;
	float			unused0;
	// The color of bottomless water body
	float3			OS_WaterbodyColor;

	// The strength, direction and color of sun streak
	float			OS_Shineness;
	float3			OS_SunDir;
	float			unused1;
	float3			OS_SunColor;
	float			unused2;
	
	// The parameter is used for fixing an artifact
	float3			OS_BendParam;

	// Perlin noise for distant wave crest
	float			OS_PerlinSize;
	float3			OS_PerlinAmplitude;
	float			unused3;
	float3			OS_PerlinOctave;
	float			unused4;
	float3			OS_PerlinGradient;

	// Constants for calculating texcoord from position
	float			OS_TexelLength_x2;
	float			OS_UVScale;
	float			OS_UVOffset;
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
	float3 vWorldPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};

struct ConstantOutputType
{
    float edges[3] : SV_TessFactor;
    float inside: SV_InsideTessFactor;
};

Texture2D TX_Texture0 : register( t0 );  
SamplerState SS_Linear : register( s0 );

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
	
	float3 localPos = (worldPosition - OPP_PatchPosition) / 2048;
	float distance = length(OPP_LocalEye - localPos) * 0.0001f;
	float dispMod = saturate((distance - 2000) * 0.0001f);
	
	//float scale = 1/(2000.0f);
	texCoord = (worldPosition.xz / 2048) + OS_UVOffset;
	float3 displacementFFT = TX_Texture0.SampleLevel(SS_Linear, texCoord, 0).xzy;
	float3 displacementRandom = TX_Texture0.SampleLevel(SS_Linear, 3 * texCoord + displacementFFT + (worldPosition.xz / 2048 * 0.666f), 0).xzy;

	float3 displacement = lerp(displacementFFT, displacementRandom, dispMod);

						
	float3 vHeight = 1.0f * displacement;
	worldPosition += vHeight;
	
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
	output.vDiffuse  = float4(worldPosition,1);
	output.vNormalWS = normalWS;
	output.vNormalVS = normalVS;
	//output.vViewPosition = mul(float4(worldPosition,1), frame.M_View).xyz;
	output.vWorldPosition = worldPosition;
	
    return output;
}