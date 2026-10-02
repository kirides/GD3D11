//--------------------------------------------------------------------------------------
// Simple vertex shader
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"
#include "VertexPacking.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer Matrices_PerInstances : register( b1 )
{
	float M_TotalTime;
};

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
// Water surfaces are part of the wrapped world mesh, which is stored packed (ExVertexStructGPU).
struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float2 vNormalOct	: NORMAL;    // octahedral-encoded (R16G16_SNORM)
	float2 vTex1		: TEXCOORD0;
	float2 vTex2		: TEXCOORD1;
	float4 vDiffuse		: DIFFUSE;
};

struct VS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalWS		: TEXCOORD4;
	float3 vWorldPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
#if SHD_WATERANI
#include <include/WaterVertexWaves.hlsl>
#endif

VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	//Input.vPosition = float3(-Input.vPosition.x, Input.vPosition.y, -Input.vPosition.z);
	
	float3 positionWorld = Input.vPosition;
	float2 texAniMap = Input.vTex2 * M_TotalTime;
	texAniMap -= floor( texAniMap );
    
#if SHD_WATERANI
    positionWorld += WaterWaveOffset( positionWorld, Input.vDiffuse, M_TotalTime, SHD_WATERANI );
#endif
	//Output.vPosition = float4(Input.vPosition, 1);
	Output.vPosition = mul( float4(positionWorld,1), frame.M_ViewProj);
	Output.vTexcoord = Input.vTex1 + texAniMap;
	Output.vDiffuse  = Input.vDiffuse;
    Output.vNormalWS = DecodeOctNormal( Input.vNormalOct );
	Output.vWorldPosition = positionWorld;
	Output.vTexcoord2.x = mul(float4(positionWorld,1), frame.M_View).z;
	Output.vTexcoord2.y = length(mul(float4(positionWorld,1), frame.M_View));
	//Output.vWorldPosition = positionWorld;
	
	return Output;
}

