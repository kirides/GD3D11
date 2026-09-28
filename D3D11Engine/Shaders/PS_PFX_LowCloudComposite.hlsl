//--------------------------------------------------------------------------------------
// Low clouds, composite: upsamples the cloud layer onto the scene (ONE / INV_SRC_ALPHA blend)
//--------------------------------------------------------------------------------------
#include <AtmosphericScattering.h>

Texture2D TX_LowClouds : register( t0 );
Texture2D TX_LowCloudDepth : register( t1 );
Texture2D TX_SkyLowClouds : register( t2 );
Texture2D TX_Depth : register( t3 );   // full-resolution scene depth

float4 LowCloudLoadLayer( int2 p ) { return TX_LowClouds.Load( int3( p, 0 ) ); }
float LowCloudLoadDepth( int2 p ) { return TX_LowCloudDepth.Load( int3( p, 0 ) ).r; }
float4 LowCloudLoadSky( int2 p ) { return TX_SkyLowClouds.Load( int3( p, 0 ) ); }
int2 LowCloudLayerSize()
{
	uint width, height;
	TX_LowClouds.GetDimensions( width, height );
	return max( int2( width, height ), int2( 1, 1 ) );
}

#define LOW_CLOUDS_COMPOSITE
#include <include/LowClouds.hlsl>

struct PS_INPUT
{
	float2 vTexcoord : TEXCOORD0;
	float3 vEyeRay : TEXCOORD1;
	float4 vPosition : SV_POSITION;
};

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float depth = TX_Depth.Load( int3( int2( Input.vPosition.xy ), 0 ) ).r;
	return CompositeLowClouds( Input.vTexcoord, depth, Input.vPosition.xy );
}
