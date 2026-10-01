// Average on-screen sky for PS_Water's sky fill; see include/WaterSkyAverage.hlsl.

cbuffer WaterSkyAverageCB : register( b0 )
{
	uint2 WSA_Size;      // viewport size in pixels
	float WSA_Blend;     // history blend factor for this frame
	uint WSA_Reset;
};

Texture2D TX_Scene : register( t0 );          // pre-water scene copy
Texture2D<float> TX_Depth : register( t1 );   // pre-water depth copy, reversed-Z
RWTexture2D<float> RW_History : register( u0 );

float3 WaterSkyAvgScene( int2 px ) { return TX_Scene.Load( int3( px, 0 ) ).rgb; }
float WaterSkyAvgRawDepth( int2 px ) { return TX_Depth.Load( int3( px, 0 ) ); }
float WaterSkyAvgLoad( uint i ) { return RW_History[uint2( i, 0 )]; }
void WaterSkyAvgStore( uint i, float v ) { RW_History[uint2( i, 0 )] = v; }

#include <include/WaterSkyAverage.hlsl>

[numthreads( WATER_SKY_AVG_THREADS, 1, 1 )]
void CSMain( uint3 gtid : SV_GroupThreadID )
{
	WaterSkyAverage( gtid.x, WSA_Size, WSA_Blend, WSA_Reset != 0 );
}
