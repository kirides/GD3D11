#ifndef HEIGHTFOG_COLOR_H
#define HEIGHTFOG_COLOR_H

#include "Heightfog.h"
#include <AtmosphericScattering.h>

// The color the fullscreen fog pass fades the scene into. Shared so transparent surfaces that fog
// themselves land on exactly the same color.
float3 HeightfogColor( HeightfogParams p, float3 worldPos, float3 fogColorMod )
{
	float3 position = worldPos - p.CameraPosition;
	position.y -= p.FogHeight;

	float3 color = ApplyAtmosphericScatteringGround( position, fogColorMod, true );

	// (Increased the R, G, B values. Tweak these up/down if you want it brighter/darker!)
	float3 nightFogColor = float3(0.04f, 0.06f, 0.09f);
	        nightFogColor = float3(0.12f, 0.18f, 0.27f);
	float nightTimeBlend = saturate(-AC_LightPos.y * 4.0f);
	color = lerp(color, nightFogColor, nightTimeBlend);

	// Starts darker (2.5) and doesn't drop as much at noon.
	float darknessFactor = 2.5f;
	// AC_LightPos is a cbuffer scalar (frame-uniform), not per-pixel data.
	[branch]
	if (AC_LightPos.y > 0.0f) {
		darknessFactor -= (AC_LightPos.y * 0.8f);
	}

	return saturate(color / darknessFactor);
}

#endif
