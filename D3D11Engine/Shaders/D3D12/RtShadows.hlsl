// Ray-traced shadow mask (D3D12 inline RayQuery). For every pixel of the depth prepass this traces the sun and
// the point lights of the pixel's light cluster against the frame's TLAS and packs the visibilities for the lit
// passes (include/RtShadowMask.hlsl). Sun rays only reach SunDistance; the lit passes blend into the CSM there.

#include "include/ForwardPlusTypes.hlsl"
#define SHADOWCB_REGISTER b1
#include "include/ShadowCB.hlsl"
#include "include/RtShadowMask.hlsl"

cbuffer RtShadowCB : register( b0 )
{
    float4x4 InvViewProjRel;     // clip -> camera-relative world
    float3   CamPos;        uint  DepthIndex;
    uint2    Size;          uint  OutIndex;       uint NoiseFrame;
    float    SunDistance;   float SunFadeBand;    uint SunRays;        float SunConeTan;
    uint     PointRays;     float PointSourceRadius; uint NumTilesX;   float PixelAngle;
    float    ProjA;         float ProjB;          float NearZ;         float FarZ;   // as LightCB
};

#include "include/RtScene.hlsl"
StructuredBuffer<GPULight>  Lights       : register( t13 );
StructuredBuffer<LightGrid> LightGridBuf : register( t14 );
RWByteAddressBuffer         Stats        : register( u0 );   // pixels over the cap, most slots one walk wanted, pixels with a slot

static const float kTwoPi = 6.28318531;
static const float kSunRayLength = 60000.0;
static const float kLightNearClip = 15.0;   // stops short of the light so the lamp around it doesn't block it

// Same expression as PBRLighting.hlsl's ComputeZSlice; the lit pass must land in the same cluster
uint ComputeZSlice( float hwDepth )
{
    float d = max( hwDepth, 1e-6 );
    float viewZ = ProjB / ( d - ProjA );
    float t = log2( max( viewZ, NearZ ) / NearZ ) / log2( FarZ / NearZ );
    return (uint)clamp( floor( t * (float)NUM_Z_SLICES ), 0.0, (float)( NUM_Z_SLICES - 1 ) );
}

float RawDepth( int2 px )
{
    Texture2D<float> depth = ResourceDescriptorHeap[DepthIndex];
    return depth.Load( int3( clamp( px, int2( 0, 0 ), int2( Size ) - 1 ), 0 ) );
}

float3 PositionRel( int2 px, float raw )
{
    float2 uv = ( float2( px ) + 0.5 ) / float2( Size );
    float4 p = mul( float4( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, raw, 1.0 ), InvViewProjRel );
    return p.xyz / p.w;
}

// Geometric normal from depth, facing the camera; each axis takes the neighbour nearer in depth so silhouettes don't bend it
float3 ReconstructNormal( int2 px, float3 p )
{
    static const float kFar = 1e9;
    float rl = RawDepth( px - int2( 1, 0 ) ), rr = RawDepth( px + int2( 1, 0 ) );
    float ru = RawDepth( px - int2( 0, 1 ) ), rd = RawDepth( px + int2( 0, 1 ) );
    float3 l = rl > 0.0 ? PositionRel( px - int2( 1, 0 ), rl ) : kFar.xxx;
    float3 r = rr > 0.0 ? PositionRel( px + int2( 1, 0 ), rr ) : kFar.xxx;
    float3 u = ru > 0.0 ? PositionRel( px - int2( 0, 1 ), ru ) : kFar.xxx;
    float3 d = rd > 0.0 ? PositionRel( px + int2( 0, 1 ), rd ) : kFar.xxx;
    float3 dx = length( p - l ) < length( r - p ) ? p - l : r - p;
    float3 dy = length( p - u ) < length( d - p ) ? p - u : d - p;
    float3 n = cross( dy, dx );
    float len = length( n );
    float3 toCam = -normalize( p );
    if ( !( len > 1e-8 ) || len > 1e15 ) return toCam;   // also catches NaN
    n /= len;
    return dot( n, toCam ) < 0.0 ? -n : n;
}

// Interleaved gradient noise (Jimenez), rotated per frame when a temporal resolve averages it
float Noise( int2 px )
{
    float2 p = float2( px ) + 5.588238 * float( NoiseFrame );
    return frac( 52.9829189 * frac( dot( p, float2( 0.06711056, 0.00583715 ) ) ) );
}

// Vogel disk sample i of n, rotated by `rot`; unit radius
float2 DiskSample( uint i, uint n, float rot )
{
    float r = sqrt( ( float( i ) + 0.5 ) / float( n ) );
    float a = float( i ) * 2.39996323 + rot;
    return r * float2( cos( a ), sin( a ) );
}

void Basis( float3 n, out float3 t, out float3 b )
{
    float3 up = abs( n.y ) < 0.99 ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    t = normalize( cross( up, n ) );
    b = cross( n, t );
}

float SunVisibility( float3 origin, float rot, float cone )
{
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = 0.0;
    ray.TMax = kSunRayLength;
    if ( SunRays <= 1u )
    {
        ray.Direction = SunDirWS;
        return TraceVisibility( ray, 0xFF, true, cone );
    }
    float3 t, b;
    Basis( SunDirWS, t, b );
    float vis = 0.0;
    [loop] for ( uint i = 0; i < SunRays; ++i )
    {
        float2 d = DiskSample( i, SunRays, rot ) * SunConeTan;
        ray.Direction = normalize( SunDirWS + t * d.x + b * d.y );
        vis += TraceVisibility( ray, 0xFF, true, cone );
    }
    return vis / float( SunRays );
}

float PointVisibility( float3 lightPos, uint mask, float3 origin, float rot, float cone )
{
    float3 toLight = lightPos - origin;
    float3 t, b;
    Basis( normalize( toLight ), t, b );
    uint rays = max( PointRays, 1u );
    float vis = 0.0;
    [loop] for ( uint i = 0; i < rays; ++i )
    {
        float2 d = rays > 1u ? DiskSample( i, rays, rot ) * PointSourceRadius : float2( 0.0, 0.0 );
        float3 dir = toLight + t * d.x + b * d.y;
        float len = length( dir );
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = dir / len;
        ray.TMin = 0.0;
        ray.TMax = max( len - kLightNearClip, 0.0 );
        vis += ray.TMax > 0.0 ? TraceVisibility( ray, mask, true, cone ) : 1.0;
    }
    return vis / float( rays );
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID )
{
    if ( any( id.xy >= Size ) ) return;
    RWTexture2D<uint2> outMask = ResourceDescriptorHeap[OutIndex];
    int2 px = int2( id.xy );
    float raw = RawDepth( px );
    if ( raw <= 0.0 )
    {
        outMask[px] = uint2( 0, 0 );   // sky: nothing traced
        return;
    }

    float3 rel = PositionRel( px, raw );
    float viewDist = length( rel );
    float3 wpos = CamPos + rel;
    float3 N = ReconstructNormal( px, rel );
    float footprint = viewDist * PixelAngle;
    float3 origin = wpos + N * ( 0.5 + footprint * 2.0 );
    float rot = Noise( px ) * kTwoPi;
    float cone = max( footprint, 0.5 );
    uint slice = ComputeZSlice( raw );

    uint2 m = kRtMaskAllLit;
    m.x |= kRtMaskTraced | ( slice << 12 );

    [branch] if ( SunRays > 0u && viewDist < SunDistance )
    {
        float weight = saturate( ( SunDistance - viewDist ) / SunFadeBand );
        float vis = SunIntensity > 0.0 ? SunVisibility( origin, rot, cone ) : 1.0;
        m.x |= (uint)round( vis * 255.0 ) | ( (uint)round( weight * 15.0 ) << 8 );
    }

    uint slots = 0u;
    [branch] if ( PointRays > 0u )
    {
        uint2 tile = id.xy / TILE_SIZE;
        uint cluster = ( tile.y * NumTilesX + tile.x ) * NUM_Z_SLICES + slice;
        // Consecutive lights at one position with one mask (stacked fill lights) share a slot and its rays.
        // The walk runs past the cap only to count what it would have needed.
        int k = -1;
        float3 groupPos = float3( 1e30, 1e30, 1e30 );
        uint groupMask = 0u;
        bool groupTraced = false;
        uint wm = LightGridBuf[cluster].WordOccupancy;
        [loop] while ( wm != 0u )
        {
            uint w = firstbitlow( wm );
            wm &= wm - 1u;
            uint bits = LightGridBuf[cluster].Mask[w];
            [loop] while ( bits != 0u )
            {
                uint bit = firstbitlow( bits );
                bits &= bits - 1u;
                GPULight L = Lights[w * 32u + bit];
                if ( L.RtShadowMask == 0 ) continue;
                if ( any( L.PositionWorld != groupPos ) || (uint)L.RtShadowMask != groupMask )
                {
                    ++k;
                    groupPos = L.PositionWorld;
                    groupMask = (uint)L.RtShadowMask;
                    groupTraced = false;
                }
                if ( k >= (int)kRtMaxPointLights || groupTraced ) continue;
                // Any member reaching this pixel traces for the whole group
                float dist = length( L.PositionWorld - wpos );
                float fade = saturate( ( length( L.PositionView ) - L.Range * 6.0 ) / ( L.Range * 3.0 ) );
                [branch] if ( dist < L.Range && fade < 1.0 )
                {
                    m = RtMaskSetPoint( m, (uint)k, PointVisibility( L.PositionWorld, groupMask, origin, rot, cone ) );
                    groupTraced = true;
                }
            }
        }
        slots = (uint)( k + 1 );
    }
    outMask[px] = m;

    // Overflow statistics for the stats panel, one atomic per wave
    uint over = WaveActiveCountBits( slots > kRtMaxPointLights );
    uint used = WaveActiveCountBits( slots > 0u );
    uint most = WaveActiveMax( slots );
    if ( WaveIsFirstLane() && used != 0u )
    {
        uint previous;
        Stats.InterlockedAdd( 0, over, previous );
        Stats.InterlockedMax( 4, most, previous );
        Stats.InterlockedAdd( 8, used, previous );
    }
}
