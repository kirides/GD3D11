//--------------------------------------------------------------------------------------
// Depth-only VS for the NVIDIA per-face cube-shadow fallback (PointShadowCasters RenderFacePasses), trimmed
// to PS_CubeShadow's input layout. The face's matrices come from cbPerCubeRender, indexed by PCR_Face.
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerInstances : register( b1 )
{
	VS_ExConstantBuffer_PerInstance cbInstance;
};

cbuffer cbPerCubeRender : register( b3 )
{
	matrix PCR_View[6];
	matrix PCR_ViewProj[6];
	uint PCR_SliceBase;
	uint PCR_Face;
	uint2 PCR_Pad;
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
};

struct VS_OUTPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;

	matrix view = PCR_View[PCR_Face];
	float3 positionWorld = mul(float4(Input.vPosition,1), cbInstance.M_World).xyz;

	Output.vPosition = mul( float4(positionWorld,1), PCR_ViewProj[PCR_Face]);
	Output.vTexcoord2 = Input.vTex2;
	Output.vTexcoord = Input.vTex1;
	Output.vDiffuse  = Input.vDiffuse;
	Output.vNormalVS = mul(Input.vNormal, (float3x3)mul(cbInstance.M_World, view));
	Output.vViewPosition = mul(float4(positionWorld,1), view).xyz;

	return Output;
}
