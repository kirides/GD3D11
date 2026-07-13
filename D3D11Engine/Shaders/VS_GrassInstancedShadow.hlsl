//--------------------------------------------------------------------------------------
// Grass shadow-caster vertex shader
//--------------------------------------------------------------------------------------

#include "Globals_VS_ExConstants.h"

cbuffer Matrices_PerFrame : register( b0 )
{
	VS_ExConstantBuffer_PerFrame frame;
};

cbuffer GrassCB : register( b1 )
{
	float3 G_NormalVS;
	float G_Time;
	float G_WindStrength;
	float G_HeroAffectStrength;
	float2 G_Pad1;
	float3 G_PlayerPosWS;
	float G_Pad2;
};

// HERO AFFECTS CONST
static const float grassHeroAffectRange = 45.0f;
static const float grassHeroAffectStrength = 35.0f;

// Match the hero-influence push of VS_GrassInstanced so the shadow silhouette
// doesn't lag behind the blades it's supposed to be casting from.
float2 GrassHeroAffectOffsetXZ(float3 wpos, float vertexY)
{
	float3 toGrass = wpos - G_PlayerPosWS;
	float distSqXZ = dot(toGrass.xz, toGrass.xz);
	float distanceFactor = exp(-distSqXZ / (2.0f * grassHeroAffectRange * grassHeroAffectRange));
	float2 pushDirXZ = distSqXZ > 0.0001f ? toGrass.xz * rsqrt(distSqXZ) : float2(1, 0);

	return pushDirXZ * distanceFactor * vertexY * grassHeroAffectStrength * G_HeroAffectStrength;
}

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct VS_INPUT
{
	float3 vPosition	: POSITION;
	float2 vTex1		: TEXCOORD0;
	float4x4 InstanceWorldMatrix : INSTANCE_WORLD_MATRIX;
};

struct VS_OUTPUT
{
	float2 vTexcoord	: TEXCOORD0;
	float4 vPosition	: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;

	float3 wpos = mul(float4(Input.vPosition,1), Input.InstanceWorldMatrix).xyz;

	// Match the wind animation of VS_GrassInstanced so the shadow silhouette
	// doesn't lag behind the swaying blades it's supposed to be casting from.
	float wind = sin(Input.vPosition.z * 0.001f) * 0.5f + 0.5f;
	wind += sin(Input.vPosition.x * 0.001f) * 0.5f + 0.5f;
	wind += 0.2f;

	wpos.xz += sin(G_Time + wind) * 2.0f * Input.vPosition.y * G_WindStrength;
	wpos.xz += sin(G_Time * 3.0f + wind) * 1.55f * Input.vPosition.y * G_WindStrength;
	wpos.xz += sin(G_Time * 5.0f + wind) * 1.2f * Input.vPosition.y * G_WindStrength;

	if (G_HeroAffectStrength > 0)
	{
		wpos.xz += GrassHeroAffectOffsetXZ(wpos, Input.vPosition.y);
	}

	Output.vPosition = mul( float4(wpos,1), frame.M_ViewProj);
	Output.vTexcoord = Input.vTex1;

	return Output;
}
