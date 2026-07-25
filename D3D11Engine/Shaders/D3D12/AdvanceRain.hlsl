// D3D12 rain/snow particle advance — DXC/SM6 port of CS_AdvanceRain.hlsl. Ping-pongs the dynamic
// {position, velocity} buffer in place (root UAV): integrates velocity, and respawns particles that
// fall below CameraPosition.y - Height at a random seed position read from the immutable per-particle
// StructuredBuffer (root SRV). Root layout mirrors D3D12's light-cull compute pipeline (32-bit root
// consts + root SRV + root UAV, no descriptor-heap slots needed for either buffer).

cbuffer AdvanceRainCB : register( b0 )
{
    float3 AR_LightPosition;   // unused here (only the draw VS/PS need it); kept for CB-layout parity
    float  AR_FPS;

    float3 AR_CameraPosition;
    float  AR_Radius;          // unused here; kept for CB-layout parity with AdvanceRainConstantBuffer

    float  AR_Height;
    float3 AR_GlobalVelocity;

    int    AR_MoveRainParticles;   // particle count to advance (dispatch is padded to 128s; excess threads no-op)
    float3 AR_Pad1;
};

struct RainParticleDynamic
{
    float3 position;
    float3 velocity;
};

struct RainParticleStatic
{
    float3 seed;
    float  randomBrightness;
    int    drawMode;
};

StructuredBuffer<RainParticleStatic>     StaticData  : register( t0 );
RWStructuredBuffer<RainParticleDynamic>  DynamicData : register( u0 );

[numthreads(128, 1, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    if ( (int)DTid.x >= AR_MoveRainParticles )
        return;

    RainParticleDynamic p = DynamicData[DTid.x];

    float fps = max( AR_FPS, 1.0f );
    p.velocity = p.velocity / fps + AR_GlobalVelocity / fps;
    p.position += p.velocity;

    if ( p.position.y <= AR_CameraPosition.y - AR_Height )
    {
        float3 seed = StaticData[DTid.x].seed;
        p.position = float3( seed.x + AR_CameraPosition.x, seed.y + AR_CameraPosition.y, seed.z + AR_CameraPosition.z );
    }

    DynamicData[DTid.x] = p;
}
