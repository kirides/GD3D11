//--------------------------------------------------------------------------------------
// World mesh vertex shader for atlas indirect draw path
// Reads per-submesh atlas descriptors from a StructuredBuffer.
// The submesh index comes from the instance ID buffer + StartInstanceLocation.
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

struct WorldMeshSubmeshGPUData
{
	int   diffuseSlice;
	float dUStart, dVStart, dUEnd, dVEnd;
	int   normalSlice;
	float nUStart, nVStart, nUEnd, nVEnd;
	int   fxSlice;
	float fUStart, fVStart, fUEnd, fVEnd;
	uint  flags;
};

StructuredBuffer<WorldMeshSubmeshGPUData> submeshData : register( t1 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float3 vPosition : POSITION;
	float3 vNormal   : NORMAL;
	float2 vTex1     : TEXCOORD0;
	float2 vTex2     : TEXCOORD1;
	float4 vDiffuse  : DIFFUSE;

	// StartInstanceLocation in the MDI args offsets this so it equals the submesh index
	uint submeshIdx  : INSTANCE_REMAP_INDEX;
};

struct VS_OUTPUT
{
	float3 vTexcoord3D       : TEXCOORD0;  // (rawU, rawV, diffuseSlice)
	float2 vTexcoord2        : TEXCOORD1;
	float4 vDiffuse          : TEXCOORD2;
	float4 vAtlasRect        : TEXCOORD3;  // diffuse (uStart, vStart, uEnd, vEnd)
	float3 vNormalVS         : TEXCOORD4;
	float3 vViewPosition     : TEXCOORD5;
	float4 vCurrClipPos      : TEXCOORD6;
	float4 vPrevClipPos      : TEXCOORD7;
	float3 vNormalAtlas3D    : TEXCOORD8;  // (rawU, rawV, normalSlice)
	float4 vNormalAtlasRect  : TEXCOORD9;  // normal (uStart, vStart, uEnd, vEnd)
	float3 vFxAtlas3D        : TEXCOORD10; // (rawU, rawV, fxSlice)
	nointerpolation uint vFlags : TEXCOORD11; // material flags
	float4 vFxAtlasRect      : TEXCOORD12; // fx (uStart, vStart, uEnd, vEnd)
	float4 vPosition         : SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;

	WorldMeshSubmeshGPUData sm = submeshData[Input.submeshIdx];

	// World mesh vertices are already in world space (M_World = Identity)
	float3 positionWorld = Input.vPosition;

	Output.vPosition = mul( float4(positionWorld, 1), frame.M_ViewProj );

	// Pass raw UVs + slice — PS does frac() and atlas remap per-pixel
	Output.vTexcoord3D = float3( Input.vTex1, (float)sm.diffuseSlice );
	Output.vAtlasRect  = float4( sm.dUStart, sm.dVStart, sm.dUEnd, sm.dVEnd );

	Output.vTexcoord2 = Input.vTex2;
	Output.vDiffuse   = Input.vDiffuse;
	Output.vNormalVS  = mul( Input.vNormal, (float3x3)frame.M_View );
	Output.vViewPosition = mul( float4(positionWorld, 1), frame.M_View ).xyz;

	// Normal map atlas coords
	Output.vNormalAtlas3D   = float3( Input.vTex1, (float)sm.normalSlice );
	Output.vNormalAtlasRect = float4( sm.nUStart, sm.nVStart, sm.nUEnd, sm.nVEnd );

	// FX map atlas coords
	Output.vFxAtlas3D    = float3( Input.vTex1, (float)sm.fxSlice );
	Output.vFxAtlasRect  = float4( sm.fUStart, sm.fVStart, sm.fUEnd, sm.fVEnd );

	Output.vFlags = sm.flags;

	// Motion vectors — static world mesh, so prev == current
	Output.vCurrClipPos = mul( float4(positionWorld, 1.0), frame.M_UnjitteredViewProj );
	Output.vPrevClipPos = mul( float4(positionWorld, 1.0), frame.M_PrevViewProj );

	return Output;
}
