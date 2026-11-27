cbuffer FrameCB    : register(b0) { float4x4 ViewProj; };
cbuffer ParticleCB : register(b1) { float3 CameraPosition; float _ppad; };
// Heap CBV of TransparencyFrameData (0xFFFFFFFF = unavailable) + the bucket's TF_MODE_*.
cbuffer ParticlePSCB : register(b2) { uint TransparencyFrameIndex; uint FogMode; };
#include "include/TransparencyFog.hlsl"

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

float3 SrgbToLinear( float3 c )   // accurate sRGB EOTF — linearize gamma-encoded albedo so lighting is done in linear space
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

struct VS_IN {
    uint   vertexID : SV_VertexID;
    float3 pos      : POSITION;
    float4 dif      : DIFFUSE;
    float3 size     : SIZE;
    uint   type     : TYPE;
    float3 vel      : VELOCITY;
};
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 dif : TEXCOORD1; float3 wpos : TEXCOORD2; };

// VERTEX_PULL: ParticleInstanceInfo (56 B: pos f3 @0, color f4 @12, size f3 @28, type u32 @40, velocity f3 @44) from the frame's
// instance ring (root SRV at offset 0); each draw's first element comes in b3, since SV_InstanceID excludes StartInstanceLocation.
#ifdef VERTEX_PULL
ByteAddressBuffer ParticleInstances : register(t1);
cbuffer ParticlePullCB : register(b3) { uint ParticleInstanceBase; };
struct VS_ENTRY_IN { uint vid : SV_VertexID; uint iid : SV_InstanceID; };
VS_IN FetchParticle( VS_ENTRY_IN e )
{
    const uint a = ( ParticleInstanceBase + e.iid ) * 56;
    VS_IN i;
    i.vertexID = e.vid;
    i.pos  = asfloat( ParticleInstances.Load3( a ) );
    i.dif  = asfloat( ParticleInstances.Load4( a + 12 ) );
    i.size = asfloat( ParticleInstances.Load3( a + 28 ) );
    i.type = ParticleInstances.Load( a + 40 );
    i.vel  = asfloat( ParticleInstances.Load3( a + 44 ) );
    return i;
}
#else
typedef VS_IN VS_ENTRY_IN;
VS_IN FetchParticle( VS_ENTRY_IN e ) { return e; }
#endif

static const float tu[4] = { 0.0, 1.0, 0.0, 1.0 };
static const float tv[4] = { 1.0, 1.0, 0.0, 0.0 };
static const float vr[4] = { -1.0,  1.0, -1.0, 1.0 };
static const float vu[4] = { -1.0, -1.0,  1.0, 1.0 };

VS_OUT VSMain( VS_ENTRY_IN e )
{
    const VS_IN i = FetchParticle( e );
    float3 planeNormal = normalize( -( i.pos - CameraPosition ) );
    float3 position = i.pos;
    float3 upVector;
    float3 rightVector;

    int visIsQuadPoly = int( step( 10.0, float( i.type ) ) );
    int visOrientation = int( i.type ) - ( 10 * visIsQuadPoly );
    float sizeScale = mad( 0.5, float( visIsQuadPoly ), 0.5 );

    if ( visOrientation == 2 ) {
        rightVector = i.size;
        upVector = i.vel;
    } else if ( visOrientation == 3 ) {
        float3 velY = normalize( i.vel );
        float3 velX = normalize( cross( planeNormal, velY ) );
        rightVector = velX * i.size.x * sizeScale;
        upVector = velY * i.size.y * sizeScale;
    } else if ( visOrientation == 1 ) {
        float3 velY = normalize( i.vel );
        float3 velX = normalize( cross( planeNormal, velY ) );
        velY = normalize( cross( planeNormal, velX ) );
        rightVector = velX * i.size.x * sizeScale;
        upVector = velY * i.size.y * sizeScale;
    } else {
        upVector = float3( 0.0, 1.0, 0.0 );
        rightVector = normalize( cross( planeNormal, upVector ) );
        upVector = normalize( cross( planeNormal, rightVector ) );
        rightVector = rightVector * i.size.x * sizeScale;
        upVector = upVector * i.size.y * sizeScale;
        position += float3( i.size.x * 0.5, -i.size.y * 0.5, 0.0 ) * float( 1 - visIsQuadPoly );
    }

    position += rightVector * vr[i.vertexID];
    position += upVector * vu[i.vertexID];

    VS_OUT o;
    o.clip = mul( float4( position, 1.0 ), ViewProj );
    o.wpos = position;
    o.uv   = float2( tu[i.vertexID], tv[i.vertexID] );
    o.dif  = float4( i.dif.rgb, pow( i.dif.a, 2.2 ) );   // gamma the alpha, like VS_ParticlePoint
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 c = tx.Sample( smp, i.uv ) * i.dif;   // color = texture * particle diffuse (blend picks add/alpha/mul)
    float3 s = saturate( c.rgb );
    // linearize rgb for the linear HDR buffer (emissive)
    return float4( FinishTransparentColor( TransparencyFrameIndex, FogMode, i.clip.xy, i.wpos,
        s, SrgbToLinear( s ), c.a ), c.a );
}
