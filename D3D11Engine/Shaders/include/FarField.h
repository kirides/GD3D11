// Static-light far field (StaticLightFarField.h), D3D11 side: decoding the bake's per-vertex and per-VOB data.
// The world/VOB vertex shaders hand it to PS_Diffuse in vFarField (rgb = irradiance, w = 1 when present).
#ifndef FARFIELD_D3D11_H
#define FARFIELD_D3D11_H

static const float kVobFarFieldMax = 4.0;   // StaticLightFarField.cpp kVobFarMax

float3 FF_DecodeRGB9E5( uint v )
{
	const float scale = exp2( (float)( v >> 27 ) - 24.0f );
	return float3( v & 511u, ( v >> 9 ) & 511u, ( v >> 18 ) & 511u ) * scale;
}

// Instance color's spare bytes (StaticLightFarField PackVob): .b = red, .r = blue, .a = 0b10 tag + 6-bit green
float4 FF_DecodeVob( float4 c )
{
	const uint a = (uint)round( c.a * 255.0f );
	if ( ( a & 0xC0u ) != 0x80u ) return 0.0f;
	const float3 s = float3( c.b, ( a & 63u ) / 63.0f, c.r );
	return float4( s * s * kVobFarFieldMax, 1.0f );
}

// 0 near the camera (real lights), 1 where only the bake lights the static point lights. invFade 0 = no bake.
float FF_Weight( float distanceToCamera, float nearRadius, float invFade, float valid )
{
	if ( invFade <= 0.0f || valid <= 0.0f ) return 0.0f;
	return saturate( ( distanceToCamera - nearRadius ) * invFade );
}

#endif
