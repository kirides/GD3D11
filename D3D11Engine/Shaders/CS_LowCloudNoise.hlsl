//--------------------------------------------------------------------------------------
// Low clouds: fills the 64^3 tileable noise texture once (include/LowCloudNoise.hlsl)
//--------------------------------------------------------------------------------------
#include <include/LowCloudNoise.hlsl>

RWTexture3D<float4> NoiseOut : register( u0 );

[numthreads( 4, 4, 4 )]
void CSMain( uint3 id : SV_DispatchThreadID )
{
	NoiseOut[id] = LowCloudNoiseTexel( id );
}
