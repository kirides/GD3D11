#ifndef TRANSPARENCY_FOG_H
#define TRANSPARENCY_FOG_H

#include "HeightfogColor.h"

// Transparent surfaces draw AFTER the fullscreen fog pass, which fogs by the depth of the opaque
// surface behind them - for anything against the sky that is the far plane. They fog themselves from
// their own depth here instead. TF_Mode picks what the fog fades toward, matching the blend mode:
// blending fades to the fog color, additive light is only attenuated, a modulate pass fades to white.
#define TF_MODE_OFF       0
#define TF_MODE_BLEND     1
#define TF_MODE_ADD       2
#define TF_MODE_MODULATE  3

cbuffer TransparencyFogCB : register( b7 )
{
	matrix TF_InvView;

	float3 TF_CameraPosition;
	float TF_FogHeight;

	float TF_HeightFalloff;
	float TF_GlobalDensity;
	float TF_WeightZNear;
	float TF_WeightZFar;

	float3 TF_FogColorMod;
	int TF_Mode;
};

float3 ApplyTransparencyFogWS( float3 color, float3 worldPosition )
{
	HeightfogParams p;
	p.CameraPosition = TF_CameraPosition;
	p.FogHeight = TF_FogHeight;
	p.HeightFalloff = TF_HeightFalloff;
	p.GlobalDensity = TF_GlobalDensity;
	p.WeightZNear = TF_WeightZNear;
	p.WeightZFar = TF_WeightZFar;

	float3 result = color;

	// TF_Mode is a cbuffer scalar, so these are uniform branches.
	[branch]
	if ( TF_Mode != TF_MODE_OFF )
	{
		float3 target;
		[branch]
		if ( TF_Mode == TF_MODE_ADD ) target = 0.0f;
		else if ( TF_Mode == TF_MODE_MODULATE ) target = 1.0f;
		else target = HeightfogColor( p, worldPosition, TF_FogColorMod );

		result = lerp( color, target, HeightfogCoverage( p, worldPosition ) );
	}

	return result;
}

float3 ApplyTransparencyFog( float3 color, float3 viewPosition )
{
	float3 result = color;

	[branch]
	if ( TF_Mode != TF_MODE_OFF )
	{
		result = ApplyTransparencyFogWS( color, mul( float4( viewPosition, 1.0f ), TF_InvView ).xyz );
	}

	return result;
}

#endif
