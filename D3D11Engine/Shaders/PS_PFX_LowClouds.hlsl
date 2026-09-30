//--------------------------------------------------------------------------------------
// Low clouds, generate: ray-marches the half-resolution cloud layer (include/LowClouds.hlsl)
//--------------------------------------------------------------------------------------
#include <AtmosphericScattering.h>
#include <include/LowClouds.hlsl>

Texture2D TX_Depth : register( t0 );   // full-resolution scene depth

struct PS_INPUT
{
	float2 vTexcoord : TEXCOORD0;
	float3 vEyeRay : TEXCOORD1;
	float4 vPosition : SV_POSITION;
};

struct PS_OUTPUT
{
	float4 Clouds : SV_TARGET0;
	float Depth : SV_TARGET1;
	float4 SkyClouds : SV_TARGET2;
};

PS_OUTPUT PSMain( PS_INPUT Input )
{
	uint width, height;
	TX_Depth.GetDimensions( width, height );
	int2 maxPixel = int2( width, height ) - 1;
	int2 base = int2( floor( Input.vTexcoord * float2( width, height ) + 0.5f ) ) - 1;
	float4 footprint = float4(
		TX_Depth.Load( int3( clamp( base, 0, maxPixel ), 0 ) ).r,
		TX_Depth.Load( int3( clamp( base + int2( 1, 0 ), 0, maxPixel ), 0 ) ).r,
		TX_Depth.Load( int3( clamp( base + int2( 0, 1 ), 0, maxPixel ), 0 ) ).r,
		TX_Depth.Load( int3( clamp( base + int2( 1, 1 ), 0, maxPixel ), 0 ) ).r );

	LowCloudLayerTexel texel = GenerateLowCloudTexel( Input.vTexcoord, footprint, Input.vPosition.xy );
	PS_OUTPUT o;
	o.Clouds = texel.clouds;
	o.Depth = texel.depth;
	o.SkyClouds = texel.skyClouds;
	return o;
}
