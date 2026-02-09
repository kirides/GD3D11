
cbuffer AdvanceRainConstantBuffer : register( b0 )
{
	float3 AR_LightDirection;
	float AR_FPS;
	
	float3 AR_CameraPosition;
	float AR_Radius;
	
	float AR_Height;
	float3 AR_GlobalVelocity;
	
	uint AR_MoveRainParticles;
	float3 AR_Pad1;
};

RWByteAddressBuffer mutableBuf : register( u0 );

struct RainParticleStatic
{
	float3 seed;
	float randomBrightness;
	int drawMode;
};

StructuredBuffer<RainParticleStatic> staticBuf : register( t0 );

[numthreads(128, 1, 1)]
void CSMain( uint3 dispatchThreadID : SV_DispatchThreadID )
{
    if (dispatchThreadID.x >= AR_MoveRainParticles)
        return;
	
    uint dynamicOffset = dispatchThreadID.x * 24;
    float3 vertexPosition = asfloat(mutableBuf.Load3(dynamicOffset));
    float3 vertexVelocity = asfloat(mutableBuf.Load3(dynamicOffset + 12));
	
    vertexVelocity = vertexVelocity.xyz / max(AR_FPS, 1) + AR_GlobalVelocity.xyz / max(AR_FPS, 1);
    vertexPosition.xyz += vertexVelocity;
    if (vertexPosition.y <= AR_CameraPosition.y - AR_Height)
    {
        float3 seed = staticBuf[dispatchThreadID.x].seed;
        float x = seed.x + AR_CameraPosition.x;
        float z = seed.z + AR_CameraPosition.z;
        float y = seed.y + AR_CameraPosition.y;
        vertexPosition = float3(x, y, z);
    }
	
    mutableBuf.Store3(dynamicOffset, asuint(vertexPosition));
    mutableBuf.Store3(dynamicOffset + 12, asuint(vertexVelocity));
}
