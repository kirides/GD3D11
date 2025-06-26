
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

RWByteAddressBuffer buf : register( u0 );

[numthreads(128, 1, 1)]
void CSMain( uint3 dispatchThreadID : SV_DispatchThreadID )
{
    if (dispatchThreadID.x >= AR_MoveRainParticles)
        return;
	
    uint vertexOffset = dispatchThreadID.x * 52;
    float3 vertexPosition = asfloat(buf.Load3(vertexOffset));
    float3 vertexVelocity = asfloat(buf.Load3(vertexOffset + 40));
	
    vertexVelocity = vertexVelocity.xyz / max(AR_FPS, 1) + AR_GlobalVelocity.xyz / max(AR_FPS, 1);
    vertexPosition.xyz += vertexVelocity;
    if (vertexPosition.y <= AR_CameraPosition.y - AR_Height)
    {
        float3 seed = asfloat(buf.Load3(vertexOffset + 12));
        float x = seed.x + AR_CameraPosition.x;
        float z = seed.z + AR_CameraPosition.z;
        float y = seed.y + AR_CameraPosition.y;
        vertexPosition = float3(x, y, z);
    }
	
    buf.Store3(vertexOffset, asuint(vertexPosition));
    buf.Store3(vertexOffset + 40, asuint(vertexVelocity));
}
