//--------------------------------------------------------------------------------------
// Simple vertex shader
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
	float G_PrevTime; // G_Time as of the previous frame, for wind-sway motion vectors
	float G_Pad1;
	float3 G_PlayerPosWS;
	float G_Pad2;
};

// Same per-blade sway formula VSMain uses, factored out so VSMain and its previous-frame
// evaluation (for motion vectors) can never drift out of sync.
float2 GrassWindOffsetXZ(float3 localPos, float time, float windStrength)
{
	float wind = sin(localPos.z * 0.001f) * 0.5f + 0.5f;
	wind += sin(localPos.x * 0.001f) * 0.5f + 0.5f;
	wind += 0.2f;

	float2 offset = 0;
	offset += sin(time + wind) * 2.0f * localPos.y * windStrength;
	offset += sin(time * 3.0f + wind) * 1.55f * localPos.y * windStrength;
	offset += sin(time * 5.0f + wind) * 1.2f * localPos.y * windStrength;
	return offset;
}

// HERO AFFECTS CONST
static const float grassHeroAffectRange = 45.0f;
static const float grassHeroAffectStrength = 35.0f;

// Pushes grass away from the player, same falloff shape as the bush/tree hero-influence effect.
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
	float2 vTexcoord		: TEXCOORD0;
	float3 vNormalVS		: TEXCOORD1;
	float3 vWorldPosition	: TEXCOORD2;
	float4 vCurrClipPos     : TEXCOORD3;  // Current clip position for velocity
	float4 vPrevClipPos     : TEXCOORD4;  // Previous clip position for velocity
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
VS_OUTPUT VSMain( VS_INPUT Input )
{
	VS_OUTPUT Output;
	
	float3 basePos = mul(float4(Input.vPosition,1), Input.InstanceWorldMatrix).xyz;

	float3 wpos = basePos;
	wpos.xz += GrassWindOffsetXZ(Input.vPosition, G_Time, G_WindStrength);

	if (G_HeroAffectStrength > 0)
	{
		wpos.xz += GrassHeroAffectOffsetXZ(wpos, Input.vPosition.y);
	}

	Output.vPosition = mul( float4(wpos,1), frame.M_ViewProj);
	Output.vTexcoord = Input.vTex1;
	Output.vNormalVS = G_NormalVS;
	Output.vWorldPosition = wpos;

	// Motion Vectors - re-evaluate the sway at last frame's wind phase (G_PrevTime) instead of reusing
	// this frame's swayed `wpos`, which previously collapsed the entire per-blade sway out of the
	// velocity (grass instances don't move rigidly, so prevClipPos == currClipPos for the wind term)
	// and made TAA/motion blur treat visibly swaying grass as motion-static, blurring it.
	float3 prevWpos = basePos;
	prevWpos.xz += GrassWindOffsetXZ(Input.vPosition, G_PrevTime, G_WindStrength);

	// Hero-push must also be re-applied here (using the CURRENT G_PlayerPosWS — no previous-frame player
	// position is tracked). Time-independent, so this cancels correctly out of the velocity when the
	// player hasn't moved; omitting it entirely (as before) leaked the WHOLE push offset as bogus motion
	// every single frame the effect was active, regardless of whether anything actually moved.
	if (G_HeroAffectStrength > 0)
	{
		prevWpos.xz += GrassHeroAffectOffsetXZ(prevWpos, Input.vPosition.y);
	}

	Output.vCurrClipPos = mul(float4(wpos, 1.0), frame.M_UnjitteredViewProj);
	Output.vPrevClipPos = mul(float4(prevWpos, 1.0), frame.M_PrevViewProj);

	return Output;
}

