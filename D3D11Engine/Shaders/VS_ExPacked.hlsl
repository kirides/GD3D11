//--------------------------------------------------------------------------------------
// World-mesh vertex shader for the packed 36-byte vertex (ExVertexStructGPU).
// Mirrors VS_Ex.hlsl but decodes the octahedral normal and precomputed tangent from the
// packed stream. See VertexPacking.h (C++) / Shaders/VertexPacking.h (decoders).
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"
#include "VertexPacking.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer Matrices_PerInstances : register( b1 )
{
	VS_ExConstantBuffer_PerInstance cbInstance;
};


//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float2 vNormalOct	: NORMAL;    // octahedral-encoded (R16G16_SNORM)
	float4 vTangent		: TANGENT;   // R10G10B10A2: xyz + handedness (decoded below)
	float2 vTex1		: TEXCOORD0;
	float2 vTex2		: TEXCOORD1;
	float4 vDiffuse		: DIFFUSE;
};

// vTangent is appended LAST (before SV_POSITION) so it maps to the trailing interpolator register
// and does not shift the ones PS_Diffuse shares with VS_Ex. PS_Diffuse consumes it; other pixel
// shaders that read only the first registers link fine (VS output is a superset).
struct VS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vTangent			: TEXCOORD3;   // view-space tangent (xyz) + handedness (w)
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;

	float3 vNormal = DecodeOctNormal( Input.vNormalOct );
	float4 vTangent = DecodeTangent( Input.vTangent );   // xyz = tangent, w = handedness sign

	float3 positionWorld = mul(float4(Input.vPosition,1), cbInstance.M_World).xyz;

	Output.vPosition = mul( float4(positionWorld,1), frame.M_ViewProj);
	Output.vTexcoord2 = Input.vTex2;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = Input.vDiffuse;

	float3x3 worldView = (float3x3)mul(cbInstance.M_World, frame.M_View);
	Output.vNormalVS = mul(vNormal, worldView);
	Output.vTangent = float4( mul(vTangent.xyz, worldView), vTangent.w );
	Output.vViewPosition = mul(float4(positionWorld,1), frame.M_View);

	// Motion Vectors - use UNJITTERED matrices for correct velocity
	Output.vCurrClipPos = mul(float4(positionWorld, 1.0), frame.M_UnjitteredViewProj);
	// For static geometry, prev world pos == curr world pos
	Output.vPrevClipPos = mul(float4(positionWorld, 1.0), frame.M_PrevViewProj);

	return Output;
}
