//--------------------------------------------------------------------------------------
// Position-only vertex shader for opaque depth / shadow passes.
//
// Consumes a 12-byte position-only vertex stream (VERTEX_INPUT_LAYOUT_POS_ONLY) instead of the
// full 44-byte ExVertexStruct, cutting IA vertex-fetch bandwidth on the world geometry that is
// re-rendered depth-only multiple times per frame (Z-prepass + each sun shadow cascade).
//
// Uses the exact same constant buffers / registers as VS_Ex.hlsl so the CB bindings set up by
// SetupVS_ExConstantBuffer() / SetupVS_ExMeshDrawCall() remain valid after swapping to this shader.
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer Matrices_PerInstances : register( b1 )
{
	VS_ExConstantBuffer_PerInstance cbInstance;
};

struct VS_INPUT
{
	float3 vPosition	: POSITION;
};

struct VS_OUTPUT
{
	float4 vPosition	: SV_POSITION;
};

VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;

	float3 positionWorld = mul( float4(Input.vPosition, 1), cbInstance.M_World ).xyz;
	Output.vPosition = mul( float4(positionWorld, 1), frame.M_ViewProj );

	return Output;
}
