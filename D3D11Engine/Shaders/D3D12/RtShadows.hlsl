// Ray-traced shadow mask (D3D12 inline RayQuery). For every pixel of the depth prepass this traces the sun and
// the point lights of the pixel's light cluster against the frame's TLAS and packs the visibilities for the lit
// passes (include/RtShadowMask.hlsl). Sun rays only reach SunDistance; the lit passes blend into the CSM there.
// Filtered lights trace one ray and store a penumbra size instead; CSFilter blurs them by it (contact hardening).
// Contact shadows march the depth buffer a short way toward each light for casters the TLAS lacks (grass, wind).

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
    uint     RawIndex;      uint  Filter;         float MaxPenumbraPx; uint  ContactShadows;
    float4x4 ViewProjRel;        // camera-relative world -> clip
    uint     ContactIndex;  uint3 Pad1;           // contact flags UAV (kFilterContact)
};

#include "include/RtScene.hlsl"
StructuredBuffer<GPULight>  Lights       : register( t13 );
StructuredBuffer<LightGrid> LightGridBuf : register( t14 );
RWByteAddressBuffer         Stats        : register( u0 );   // pixels over the cap, most slots one walk wanted, pixels with a slot
RWStructuredBuffer<uint4>   ClusterGroups : register( u1 );  // per cluster 2 x uint4: RtGroup + 1 of point slots 0-14, 16 bits each

static const float kTwoPi = 6.28318531;
static const float kSunRayLength = 60000.0;
static const float kLightNearClip = 15.0;   // stops short of the light so the lamp around it doesn't block it

// Filter bits. A filtered pass writes, instead of visibilities: sun 255 = lit, else a penumbra code 0-254;
// point slot 7 = lit, else a penumbra code 0-6. A code is the blur radius as a fraction of MaxPenumbraPx.
static const uint kFilterSun = 1u, kFilterPoints = 2u, kFilterContact = 4u;
static const float kSunCodes = 254.0, kPointCodes = 6.0;

// kFilterContact: contact hits go to their own R32_UINT for CSFilter to blur, instead of into the visibilities.
// Bits 0-14 point slot hit, 15 sun hit; 16-30 / 31 marched (not where the ray was already blocked).
static const uint kContactSunHit = 0x8000u, kContactSunMarched = 0x80000000u;

// Contact shadows: march length (world units) and steps, and how far from the camera; grass leaves the prepass at 7500
static const float kContactSunLength = 100.0, kContactPointLength = 60.0, kContactRange = 7000.0;
static const uint kContactSunSteps = 16u, kContactPointSteps = 10u;

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

// Vogel disk sample i of n, rotated by `rot` and pushed out by `jitter` (0-1); unit radius.
// The radial jitter matters: a fixed ring never samples the centre and loses small blockers under the light.
float2 DiskSample( uint i, uint n, float rot, float jitter )
{
    float r = sqrt( ( float( i ) + jitter ) / float( n ) );
    float a = float( i ) * 2.39996323 + rot;
    return r * float2( cos( a ), sin( a ) );
}

void Basis( float3 n, out float3 t, out float3 b )
{
    float3 up = abs( n.y ) < 0.99 ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    t = normalize( cross( up, n ) );
    b = cross( n, t );
}

uint ClusterOf( uint2 pixel, uint slice ) { return ( ( pixel.y / TILE_SIZE ) * NumTilesX + pixel.x / TILE_SIZE ) * NUM_Z_SLICES + slice; }

// Blur radius `worldRadius` as seen at a pixel `footprint` wide, as a code of `levels` steps
uint PenumbraCode( float worldRadius, float footprint, float levels )
{
    return (uint)round( saturate( worldRadius / ( footprint * MaxPenumbraPx ) ) * levels );
}

// Distance along `dir` to the first depth sample the march passes just behind, -1 when clear. `start` is
// camera-relative, lifted off a receiver facing N. A sample blocks only within a thin shell behind its surface;
// a thicker one extrudes every caster along the view and casts an offset second shadow.
float ContactBlocker( float3 start, float3 N, float3 dir, float length, uint steps, float jitter )
{
    float stepLen = length / float( steps );
    [loop] for ( uint i = 0u; i < steps; ++i )
    {
        float t = ( float( i ) + 0.5 + 0.5 * jitter ) * stepLen;
        float4 clip = mul( float4( start + dir * t, 1.0 ), ViewProjRel );
        if ( clip.w <= 0.0 ) break;
        float2 ndc = clip.xy / clip.w;
        if ( any( abs( ndc ) >= 1.0 ) ) break;
        int2 q = int2( ( ndc * float2( 0.5, -0.5 ) + 0.5 ) * float2( Size ) );
        float d = RawDepth( q );
        if ( d <= 0.0 ) continue;   // sky
        // The receiver itself, or anything level with or below it, can't stand between it and the light
        float sampleFootprint = clip.w * PixelAngle;
        if ( dot( N, PositionRel( q, d ) - start ) < sampleFootprint * 2.0 + 1.0 ) continue;
        float behind = clip.w - ProjB / ( d - ProjA );
        if ( behind > 0.5 && behind < 2.0 + stepLen * 0.75 + sampleFootprint ) return t;
    }
    return -1.0;
}

// Nearer of a ray's blocker distance and the contact march's; -1 when both are clear
float NearestBlocker( float rayT, float contactT )
{
    if ( rayT < 0.0 ) return contactT;
    return contactT < 0.0 ? rayT : min( rayT, contactT );
}

float SunVisibility( float3 origin, float rot, float jitter, float cone )
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
        float2 d = DiskSample( i, SunRays, rot, jitter ) * SunConeTan;
        ray.Direction = normalize( SunDirWS + t * d.x + b * d.y );
        vis += TraceVisibility( ray, 0xFF, true, cone );
    }
    return vis / float( SunRays );
}

// The sun's 8 mask bits: a visibility, or a penumbra code when filtered. `contact` = also march from originRel.
uint SunBits( float3 origin, float3 originRel, float3 N, bool contact, float rot, float jitter, float cone, float footprint )
{
    if ( SunIntensity <= 0.0 ) return 255u;
    [branch] if ( Filter & kFilterSun )
    {
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = SunDirWS;
        ray.TMin = 0.0;
        ray.TMax = kSunRayLength;
        float t = TraceBlocker( ray, 0xFF, cone );
        if ( contact ) t = NearestBlocker( t, ContactBlocker( originRel, N, SunDirWS, kContactSunLength, kContactSunSteps, jitter ) );
        return t < 0.0 ? 255u : min( PenumbraCode( SunConeTan * t, footprint, kSunCodes ), 254u );
    }
    float vis = SunVisibility( origin, rot, jitter, cone );
    [branch] if ( contact && vis > 0.0 )
    {
        if ( ContactBlocker( originRel, N, SunDirWS, kContactSunLength, kContactSunSteps, jitter ) >= 0.0 ) vis = 0.0;
    }
    return (uint)round( vis * 255.0 );
}

// The contact march toward a point light; -1 when clear or the light is too close to march to
float PointContact( float3 lightPos, float3 origin, float3 originRel, float3 N, float jitter )
{
    float3 toLight = lightPos - origin;
    float len = length( toLight );
    float contactLength = min( kContactPointLength, len - kLightNearClip );
    return contactLength > 0.0 ? ContactBlocker( originRel, N, toLight / len, contactLength, kContactPointSteps, jitter ) : -1.0;
}

float PointVisibility( float3 lightPos, uint mask, float3 origin, float rot, float jitter, float cone )
{
    float3 toLight = lightPos - origin;
    float3 t, b;
    Basis( normalize( toLight ), t, b );
    uint rays = max( PointRays, 1u );
    float vis = 0.0;
    [loop] for ( uint i = 0; i < rays; ++i )
    {
        float2 d = rays > 1u ? DiskSample( i, rays, rot, jitter ) * PointSourceRadius : float2( 0.0, 0.0 );
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

// A point slot's 3 mask bits: a visibility, or a penumbra code when filtered. `contact` = also march from originRel.
uint PointBits( float3 lightPos, uint mask, float3 origin, float3 originRel, float3 N, bool contact, float rot, float jitter, float cone, float footprint )
{
    float3 toLight = lightPos - origin;
    float len = length( toLight );
    float3 dir = toLight / len;
    float contactLength = min( kContactPointLength, len - kLightNearClip );
    contact = contact && contactLength > 0.0;
    [branch] if ( Filter & kFilterPoints )
    {
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = dir;
        ray.TMin = 0.0;
        ray.TMax = max( len - kLightNearClip, 0.0 );
        float t = ray.TMax > 0.0 ? TraceBlocker( ray, mask, cone ) : -1.0;
        if ( contact ) t = NearestBlocker( t, ContactBlocker( originRel, N, dir, contactLength, kContactPointSteps, jitter ) );
        // Similar triangles: a source of PointSourceRadius, a blocker t from the receiver, the light len away
        return t < 0.0 ? 7u : min( PenumbraCode( PointSourceRadius * t / max( len - t, 1.0 ), footprint, kPointCodes ), 6u );
    }
    float vis = saturate( PointVisibility( lightPos, mask, origin, rot, jitter, cone ) );
    [branch] if ( contact && vis > 0.0 )
    {
        if ( ContactBlocker( originRel, N, dir, contactLength, kContactPointSteps, jitter ) >= 0.0 ) vis = 0.0;
    }
    return (uint)round( vis * 7.0 );
}

// Slot table entry k (RtGroup + 1, 0 = none) of a cluster's two uint4
uint GroupEntry( uint4 a, uint4 b, uint k )
{
    uint4 v = k < 8u ? a : b;
    uint i = ( k >> 1 ) & 3u;
    uint w = i == 0u ? v.x : ( i == 1u ? v.y : ( i == 2u ? v.z : v.w ) );
    return ( w >> ( ( k & 1u ) * 16u ) ) & 0xFFFFu;
}

void PutGroupEntry( inout uint4 a, inout uint4 b, uint k, uint e )
{
    uint v = e << ( ( k & 1u ) * 16u );
    uint i = ( k >> 1 ) & 3u;
    uint4 add = uint4( i == 0u ? v : 0u, i == 1u ? v : 0u, i == 2u ? v : 0u, i == 3u ? v : 0u );
    if ( k < 8u ) a |= add;
    else b |= add;
}

uint FirstLane( uint4 lanes )
{
    return lanes.x != 0u ? firstbitlow( lanes.x ) : lanes.y != 0u ? 32u + firstbitlow( lanes.y )
         : lanes.z != 0u ? 64u + firstbitlow( lanes.z ) : 96u + firstbitlow( lanes.w );
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID )
{
    if ( any( id.xy >= Size ) ) return;
    RWTexture2D<uint2> outMask = ResourceDescriptorHeap[Filter != 0u ? RawIndex : OutIndex];
    int2 px = int2( id.xy );
    float raw = RawDepth( px );
    const bool separate = ( Filter & kFilterContact ) != 0u;
    if ( raw <= 0.0 )
    {
        outMask[px] = uint2( 0, 0 );   // sky: nothing traced
        if ( separate )
        {
            RWTexture2D<uint> contactOut = ResourceDescriptorHeap[ContactIndex];
            contactOut[px] = 0u;
        }
        return;
    }

    float3 rel = PositionRel( px, raw );
    float viewDist = length( rel );
    float3 wpos = CamPos + rel;
    float3 N = ReconstructNormal( px, rel );
    float footprint = viewDist * PixelAngle;
    float3 originRel = rel + N * ( 0.5 + footprint * 2.0 );
    float3 origin = CamPos + originRel;
    bool contact = ContactShadows != 0u && viewDist < kContactRange;
    bool merge = contact && !separate;   // into the visibilities; else into contactBits for CSFilter
    uint contactBits = 0u;
    float rot = Noise( px ) * kTwoPi;
    float jitter = Noise( px + int2( 47, 17 ) );
    float cone = max( footprint, 0.5 );
    uint slice = ComputeZSlice( raw );

    uint2 m = kRtMaskAllLit;
    m.x |= kRtMaskTraced | ( slice << 12 );

    [branch] if ( SunRays > 0u && viewDist < SunDistance )
    {
        float weight = saturate( ( SunDistance - viewDist ) / SunFadeBand );
        uint sun = SunBits( origin, originRel, N, merge, rot, jitter, cone, footprint );
        m.x |= sun | ( (uint)round( weight * 15.0 ) << 8 );
        bool blocked = ( Filter & kFilterSun ) ? sun != 255u : sun == 0u;
        [branch] if ( contact && separate && SunIntensity > 0.0 && !blocked )
        {
            bool hit = ContactBlocker( originRel, N, SunDirWS, kContactSunLength, kContactSunSteps, jitter ) >= 0.0;
            contactBits |= kContactSunMarched | ( hit ? kContactSunHit : 0u );
        }
    }

    uint slots = 0u;
    [branch] if ( PointRays > 0u )
    {
        uint2 tile = id.xy / TILE_SIZE;
        uint cluster = ( tile.y * NumTilesX + tile.x ) * NUM_Z_SLICES + slice;
        // Consecutive lights at one position with one mask (stacked fill lights) share a slot and its rays.
        // The walk runs past the cap only to count what it would have needed.
        int k = -1;
        int groupId = -1;
        bool groupTraced = false;
        uint4 groupsA = 0u, groupsB = 0u;
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
                if ( L.RtGroup != groupId )
                {
                    ++k;
                    groupId = L.RtGroup;
                    groupTraced = false;
                    if ( k < (int)kRtMaxPointLights ) PutGroupEntry( groupsA, groupsB, (uint)k, (uint)groupId + 1u );
                }
                if ( k >= (int)kRtMaxPointLights || groupTraced ) continue;
                // Any member reaching this pixel traces for the whole group
                float dist = length( L.PositionWorld - wpos );
                float fade = saturate( ( length( L.PositionView ) - L.Range * 6.0 ) / ( L.Range * 3.0 ) );
                [branch] if ( dist < L.Range && fade < 1.0 )
                {
                    uint b = PointBits( L.PositionWorld, (uint)L.RtShadowMask, origin, originRel, N, merge, rot, jitter, cone, footprint );
                    m = RtMaskSetPointBits( m, (uint)k, b );
                    groupTraced = true;
                    bool blocked = ( Filter & kFilterPoints ) ? b != 7u : b == 0u;
                    [branch] if ( contact && separate && !blocked )
                    {
                        bool hit = PointContact( L.PositionWorld, origin, originRel, N, jitter ) >= 0.0;
                        contactBits |= ( 0x10000u | ( hit ? 1u : 0u ) ) << (uint)k;
                    }
                }
            }
        }
        slots = (uint)( k + 1 );
        // CSFilter matches slots across clusters by this table; one lane per cluster writes it
        [branch] if ( Filter & ( kFilterPoints | kFilterContact ) )
        {
            if ( WaveGetLaneIndex() == FirstLane( WaveMatch( cluster ) ) )
            {
                ClusterGroups[cluster * 2u] = groupsA;
                ClusterGroups[cluster * 2u + 1u] = groupsB;
            }
        }
    }
    outMask[px] = m;
    if ( separate )
    {
        RWTexture2D<uint> contactOut = ResourceDescriptorHeap[ContactIndex];
        contactOut[px] = contactBits;
    }

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

// ---- CSFilter: the filtered lights' blur, and the contact shadows' -----------------------------------------------
// Taps on three rings out to MaxPenumbraPx. Shadowed taps give the blur radius (their mean penumbra code, so the
// edge hardens where the blocker touches); the visibility is a tent over the rings. Contact hits get a fixed
// blur over the inner two rings and scale the ray-traced visibility. Taps on another surface are skipped, and
// taps from another cluster have their point slots renumbered through ClusterGroups.

static const uint kFilterRings = 3u, kFilterTapsPerRing = 8u;
static const float kFilterRingRadius[3] = { 1.0 / 3.0, 2.0 / 3.0, 1.0 };
static const uint kContactRings = 2u;
static const float kContactRingWeight[2] = { 1.0, 0.5 };

// A tap's point slots renumbered to the centre's list, with its contact flags `cq` likewise (into `cOut`);
// `valid` flags the slots the tap's cluster also has. Both lists ascend (RtGroup is a light index and the walk
// ascends), so one merge pass does it.
uint2 RemapPoints( uint2 m, uint cq, uint4 pa, uint4 pb, uint4 qa, uint4 qb, uint slots, out uint valid, out uint cOut )
{
    uint2 r = kRtMaskAllLit;
    valid = 0u;
    cOut = cq & ( kContactSunHit | kContactSunMarched );
    uint j = 0u;
    uint ej = GroupEntry( qa, qb, 0u );
    [loop] for ( uint k = 0u; k < slots; ++k )
    {
        uint ek = GroupEntry( pa, pb, k );
        [loop] while ( ej != 0u && ej < ek )
        {
            ++j;
            ej = j < 16u ? GroupEntry( qa, qb, j ) : 0u;
        }
        if ( ej == ek )
        {
            valid |= 1u << k;
            r = RtMaskSetPointBits( r, k, RtMaskPointBits( m, j ) );
            cOut |= ( ( ( cq >> j ) & 1u ) << k ) | ( ( ( cq >> ( 16u + j ) ) & 1u ) << ( 16u + k ) );
        }
    }
    return r;
}

// Tent over the rings for a blur `radius` pixels wide; ring counts are 4 bits each (lit at 0, taps at 12)
float RingVisibility( bool centreLit, uint counts, float radius )
{
    float lit = centreLit ? 1.0 : 0.0, total = 1.0;
    [unroll] for ( uint ring = 0u; ring < kFilterRings; ++ring )
    {
        float w = saturate( 1.5 - kFilterRingRadius[ring] * MaxPenumbraPx / max( radius, 1e-3 ) );
        lit += w * float( ( counts >> ( ring * 4u ) ) & 0xFu );
        total += w * float( ( counts >> ( 12u + ring * 4u ) ) & 0xFu );
    }
    return lit / total;
}

// Contact counts: 4 bits each, hits of rings 0-1 at 0, marched taps at 8
uint ContactCount( uint ring, bool hit ) { return ( hit ? 1u << ( ring * 4u ) : 0u ) + ( 1u << ( 8u + ring * 4u ) ); }

float ContactVisibility( bool centreMarched, bool centreHit, uint counts )
{
    float hit = centreMarched && centreHit ? 1.0 : 0.0, total = centreMarched ? 1.0 : 0.0;
    [unroll] for ( uint ring = 0u; ring < kContactRings; ++ring )
    {
        hit += kContactRingWeight[ring] * float( ( counts >> ( ring * 4u ) ) & 0xFu );
        total += kContactRingWeight[ring] * float( ( counts >> ( 8u + ring * 4u ) ) & 0xFu );
    }
    return total > 0.0 ? 1.0 - hit / total : 1.0;
}

[numthreads( 8, 8, 1 )]
void CSFilter( uint3 id : SV_DispatchThreadID )
{
    if ( any( id.xy >= Size ) ) return;
    RWTexture2D<uint2> rawMask = ResourceDescriptorHeap[RawIndex];
    RWTexture2D<uint2> outMask = ResourceDescriptorHeap[OutIndex];
    int2 px = int2( id.xy );
    uint2 c = rawMask[px];
    if ( ( c.x & kRtMaskTraced ) == 0u )
    {
        outMask[px] = c;
        return;
    }

    const bool contactOn = ( Filter & kFilterContact ) != 0u;
    float3 rel = PositionRel( px, RawDepth( px ) );
    float3 N = ReconstructNormal( px, rel );
    float footprint = length( rel ) * PixelAngle;
    uint slice = RtMaskSlice( c );
    uint cluster = ClusterOf( id.xy, slice );
    bool sunTraced = ( ( c.x >> 8 ) & 0xFu ) != 0u;
    bool sunRt = ( Filter & kFilterSun ) != 0u && sunTraced;
    bool sunContact = contactOn && sunTraced;
    bool pointsRt = ( Filter & kFilterPoints ) != 0u;

    uint4 pa = 0u, pb = 0u;
    uint slots = 0u;
    if ( ( pointsRt || contactOn ) && PointRays > 0u )
    {
        pa = ClusterGroups[cluster * 2u];
        pb = ClusterGroups[cluster * 2u + 1u];
        while ( slots < kRtMaxPointLights && GroupEntry( pa, pb, slots ) != 0u ) ++slots;
    }
    if ( !sunRt && !sunContact && slots == 0u )
    {
        outMask[px] = c;
        return;
    }
    RWTexture2D<uint> contactMask = ResourceDescriptorHeap[ContactIndex];
    uint cc = contactOn ? contactMask[px] : 0u;

    // Per slot: lit and tap counts per ring, shadowed taps at bit 24; the shadowed taps' code sum; contact counts
    uint counts[kRtMaxPointLights];
    uint codes[kRtMaxPointLights];
    uint contacts[kRtMaxPointLights];
    [unroll] for ( uint k0 = 0u; k0 < kRtMaxPointLights; ++k0 ) { counts[k0] = 0u; codes[k0] = 0u; contacts[k0] = 0u; }
    uint sunCounts = 0u, sunShadowed = 0u, sunCodes = 0u, sunContacts = 0u;

    // Contact-only blurs skip the outer ring
    uint rings = ( sunRt || ( pointsRt && slots != 0u ) ) ? kFilterRings : kContactRings;
    float rot = NoiseFrame != 0u ? Noise( px ) * ( kTwoPi / float( kFilterTapsPerRing ) ) : 0.0;
    [loop] for ( uint i = 0u; i < rings * kFilterTapsPerRing; ++i )
    {
        uint ring = i / kFilterTapsPerRing;
        float a = ( float( i % kFilterTapsPerRing ) + 0.5 * float( ring & 1u ) ) * ( kTwoPi / float( kFilterTapsPerRing ) ) + rot;
        float r = kFilterRingRadius[ring] * MaxPenumbraPx;
        int2 q = clamp( px + int2( round( r * float2( cos( a ), sin( a ) ) ) ), int2( 0, 0 ), int2( Size ) - 1 );
        uint2 m = rawMask[q];
        if ( ( m.x & kRtMaskTraced ) == 0u ) continue;
        float3 qrel = PositionRel( q, RawDepth( q ) );
        if ( abs( dot( N, qrel - rel ) ) > footprint * ( 2.0 + r ) ) continue;   // another surface
        uint cq = contactOn ? contactMask[q] : 0u;
        bool contactRing = ring < kContactRings;

        if ( ( ( m.x >> 8 ) & 0xFu ) != 0u )
        {
            if ( sunRt )
            {
                uint b = m.x & 0xFFu;
                bool lit = b == 255u;
                sunCounts += ( lit ? 1u << ( ring * 4u ) : 0u ) + ( 1u << ( 12u + ring * 4u ) );
                sunShadowed += lit ? 0u : 1u;
                sunCodes += lit ? 0u : b;
            }
            if ( sunContact && contactRing && ( cq & kContactSunMarched ) != 0u )
                sunContacts += ContactCount( ring, ( cq & kContactSunHit ) != 0u );
        }
        if ( slots != 0u )
        {
            uint valid = 0x7FFFu;
            uint qcluster = ClusterOf( uint2( q ), RtMaskSlice( m ) );
            if ( qcluster != cluster )
            {
                uint4 qa = ClusterGroups[qcluster * 2u], qb = ClusterGroups[qcluster * 2u + 1u];
                if ( any( qa != pa ) || any( qb != pb ) )
                {
                    uint remapped;
                    m = RemapPoints( m, cq, pa, pb, qa, qb, slots, valid, remapped );
                    cq = remapped;
                }
            }
            [unroll] for ( uint k = 0u; k < kRtMaxPointLights; ++k )
            {
                if ( k < slots && ( ( valid >> k ) & 1u ) != 0u )
                {
                    if ( pointsRt )
                    {
                        uint b = RtMaskPointBits( m, k );
                        bool lit = b == 7u;
                        counts[k] += ( lit ? 1u << ( ring * 4u ) : 1u << 24 ) + ( 1u << ( 12u + ring * 4u ) );
                        codes[k] += lit ? 0u : b;
                    }
                    if ( contactOn && contactRing && ( ( cq >> ( 16u + k ) ) & 1u ) != 0u )
                        contacts[k] += ContactCount( ring, ( ( cq >> k ) & 1u ) != 0u );
                }
            }
        }
    }

    uint2 o = c;
    if ( sunRt || sunContact )
    {
        float vis = RtMaskSunVisibility( c );
        if ( sunRt )
        {
            uint b = c.x & 0xFFu;
            bool lit = b == 255u;
            uint shadowed = sunShadowed + ( lit ? 0u : 1u );
            vis = 1.0;
            if ( shadowed != 0u )
            {
                float radius = float( sunCodes + ( lit ? 0u : b ) ) / ( float( shadowed ) * kSunCodes ) * MaxPenumbraPx;
                vis = RingVisibility( lit, sunCounts, radius );
            }
        }
        if ( sunContact ) vis *= ContactVisibility( ( cc & kContactSunMarched ) != 0u, ( cc & kContactSunHit ) != 0u, sunContacts );
        o.x = ( o.x & ~0xFFu ) | (uint)round( saturate( vis ) * 255.0 );
    }
    [unroll] for ( uint k = 0u; k < kRtMaxPointLights; ++k )
    {
        if ( k < slots )
        {
            float vis = RtMaskPointVisibility( c, k );
            if ( pointsRt )
            {
                uint b = RtMaskPointBits( c, k );
                bool lit = b == 7u;
                uint shadowed = ( counts[k] >> 24 ) + ( lit ? 0u : 1u );
                vis = 1.0;
                if ( shadowed != 0u )
                {
                    float radius = float( codes[k] + ( lit ? 0u : b ) ) / ( float( shadowed ) * kPointCodes ) * MaxPenumbraPx;
                    vis = RingVisibility( lit, counts[k], radius );
                }
            }
            if ( contactOn ) vis *= ContactVisibility( ( ( cc >> ( 16u + k ) ) & 1u ) != 0u, ( ( cc >> k ) & 1u ) != 0u, contacts[k] );
            o = RtMaskSetPoint( o, k, vis );
        }
    }
    outMask[px] = o;
}
