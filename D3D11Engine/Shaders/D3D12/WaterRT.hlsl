// Ray-traced water reflections (D3D12 inline RayQuery). For every water pixel this rebuilds the geometry
// reflection ray ShadeWater would march in screen space (include/WaterWaves.hlsl), traces it against the frame's
// TLAS (world mesh, static VOBs, node attachments, posed NPCs) and writes the hit for Water.hlsl's
// WaterTraceSSR hook: rgb = linear color * coverage, a = coverage; the distance target holds distance * coverage.
// Misses leave coverage 0, so the water falls back to its sky sources exactly as an SSR miss does.

#include "include/ForwardPlusTypes.hlsl"   // NUM_CSM_CASCADES for ShadowCB
#define SHADOWCB_REGISTER b1
#include "include/ShadowCB.hlsl"

cbuffer RtCB : register( b0 )
{
    float4x4 InvViewProjRel;     // clip -> camera-relative world
    float4x4 ViewProjRel;        // camera-relative world -> clip
    float3   CamPos;        float Time;
    uint2    RtSize;        uint2 FullSize;
    float2   InvFullSize;   uint  Scale;          uint SampleMode;     // 0 one ray, 1 one per full-res pixel, 2 four jittered
    uint     SurfaceDepthIndex; uint SceneDepthIndex; uint SceneColorIndex; uint WaveDistortionIndex;
    uint     OutColorIndex; uint  OutDistanceIndex; uint Textured;     float MaxDistance;
    float    PixelAngle;    float LodBias;        float ProjA;         float ProjB;
    float3   FogColor;      float FogNear;        // FogColor is sRGB, like World.hlsl's FogCB
    float    FogFar;        uint  ShadowRays;     uint AlphaShadows;   uint ScreenReuse;
};

// One record per BLAS geometry of every non-world instance, at InstanceID() + GeometryIndex().
struct RtGeom
{
    uint BaseVertex;
    uint StartIndex;
    uint Material;   // diffuse SRV slot (low 24 bits) | kMatAlphaTest | kMatNoTexture
    uint Kind;       // kKind*
};
// Per TLAS instance, at InstanceIndex().
struct RtInstance
{
    uint Color;      // R8G8B8A8 instance light; .g is the baked vertex light
    uint Pad0, Pad1, Pad2;
};

static const uint kKindVob = 1u;
static const uint kKindAttach = 2u;
static const uint kKindSkinned = 3u;
static const uint kWorldInstanceId = 0xFFFFFFu;
static const uint kMatAlphaTest = 0x80000000u;
static const uint kMatNoTexture = 0x40000000u;
static const uint kMatSlotMask = 0x00FFFFFFu;

RaytracingAccelerationStructure Scene : register( t0 );
StructuredBuffer<RtGeom>     Geoms      : register( t1 );
StructuredBuffer<RtInstance> Instances  : register( t2 );
StructuredBuffer<uint2>      WorldGeoms : register( t3 );   // x = start index, y = world material index
StructuredBuffer<uint>       WorldMats  : register( t4 );   // per-frame Material words of the world materials
ByteAddressBuffer WorldVB    : register( t5 );   // 36-byte ExVertexStructGPU, R32 indices
ByteAddressBuffer WorldIB    : register( t6 );
ByteAddressBuffer VobVB      : register( t7 );   // 60-byte ExVertexStruct, R16 indices
ByteAddressBuffer VobIB      : register( t8 );
ByteAddressBuffer AttachVB   : register( t9 );   // same layout as the VOB arena
ByteAddressBuffer AttachIB   : register( t10 );
ByteAddressBuffer SkinPosUv  : register( t11 );  // 20-byte posed {world pos, uv}
ByteAddressBuffer SkelIB     : register( t12 );

SamplerState smpWrap  : register( s0 );
SamplerState smpClamp : register( s1 );

static const float PI = 3.14159265;

float3 SrgbToLinear( float3 c ) { return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) ); }

// Same de-lighting the lit passes apply to diffuse textures (PBRLighting.hlsl)
float3 DelightDiffuse( float3 linearAlbedo )
{
    float luminance = dot( linearAlbedo, float3( 0.2126, 0.7152, 0.0722 ) );
    float delightFactor = 1.0 / max( sqrt( luminance + 1e-4 ), 0.2 );
    return saturate( linearAlbedo * lerp( 1.0, delightFactor, 0.5 ) );
}

// --- Water waves: the same ray ShadeWater derives, with the distortion LOD the pixel shader would pick ---
static float g_DistortionLod;
float3 WaterDistortion( float2 uv )
{
    Texture2D tex = ResourceDescriptorHeap[WaveDistortionIndex];
    return tex.SampleLevel( smpWrap, uv, g_DistortionLod ).xyz;
}
#include "../include/WaterWaves.hlsl"

// --- Geometry fetch ---
uint LoadIndex16( ByteAddressBuffer ib, uint index )
{
    uint addr = index * 2u;
    uint w = ib.Load( addr & ~3u );
    return ( addr & 2u ) ? ( w >> 16 ) : ( w & 0xFFFFu );
}

struct Tri
{
    float3 p[3];       // object space (world space for the world mesh and posed NPCs)
    float2 uv[3];
    float  light[3];   // baked vertex light (world mesh only, else the instance light)
    uint   material;
};

Tri FetchTri( uint instanceId, uint instanceIndex, uint geometry, uint primitive )
{
    Tri t;
    if ( instanceId == kWorldInstanceId )
    {
        uint2 g = WorldGeoms[geometry];
        t.material = WorldMats[g.y];
        [unroll] for ( uint k = 0; k < 3; ++k )
        {
            uint v = WorldIB.Load( ( g.x + primitive * 3u + k ) * 4u );
            uint a = v * 36u;
            t.p[k] = asfloat( WorldVB.Load3( a ) );
            t.uv[k] = asfloat( WorldVB.Load2( a + 20u ) );
            t.light[k] = float( ( WorldVB.Load( a + 32u ) >> 8 ) & 0xFFu ) / 255.0;
        }
        return t;
    }

    RtGeom g = Geoms[instanceId + geometry];
    t.material = g.Material;
    float instLight = float( ( Instances[instanceIndex].Color >> 8 ) & 0xFFu ) / 255.0;
    [unroll] for ( uint k = 0; k < 3; ++k )
    {
        uint i = g.StartIndex + primitive * 3u + k;
        t.light[k] = instLight;
        if ( g.Kind == kKindSkinned )
        {
            uint a = ( g.BaseVertex + LoadIndex16( SkelIB, i ) ) * 20u;
            t.p[k] = asfloat( SkinPosUv.Load3( a ) );
            t.uv[k] = asfloat( SkinPosUv.Load2( a + 12u ) );
        }
        else if ( g.Kind == kKindAttach )
        {
            uint a = ( g.BaseVertex + LoadIndex16( AttachIB, i ) ) * 60u;
            t.p[k] = asfloat( AttachVB.Load3( a ) );
            t.uv[k] = asfloat( AttachVB.Load2( a + 24u ) );
        }
        else
        {
            uint a = ( g.BaseVertex + LoadIndex16( VobIB, i ) ) * 60u;
            t.p[k] = asfloat( VobVB.Load3( a ) );
            t.uv[k] = asfloat( VobVB.Load2( a + 24u ) );
        }
    }
    return t;
}

float2 InterpUV( Tri t, float2 bary ) { return t.uv[0] + bary.x * ( t.uv[1] - t.uv[0] ) + bary.y * ( t.uv[2] - t.uv[0] ); }

// Ray-cone mip selection (Akenine-Moeller et al.): texel-to-surface area ratio of the triangle plus the cone width.
float TextureLod( Tri t, Texture2D tex, float coneWidth, float cosine )
{
    float w, h;
    tex.GetDimensions( w, h );
    float2 duv1 = t.uv[1] - t.uv[0], duv2 = t.uv[2] - t.uv[0];
    float ta = abs( duv1.x * duv2.y - duv2.x * duv1.y ) * w * h;
    float pa = length( cross( t.p[1] - t.p[0], t.p[2] - t.p[0] ) );
    float base = 0.5 * log2( max( ta, 1e-12 ) / max( pa, 1e-12 ) );
    return base + log2( max( coneWidth, 1e-4 ) / max( cosine, 0.1 ) ) + LodBias;
}

// Candidate alpha test; true = the hit stands
bool PassesAlpha( Tri t, float2 bary, float coneWidth )
{
    if ( ( t.material & kMatAlphaTest ) == 0u || ( t.material & kMatNoTexture ) != 0u ) return true;
    Texture2D tex = ResourceDescriptorHeap[t.material & kMatSlotMask];
    float lod = max( TextureLod( t, tex, coneWidth, 1.0 ), 0.0 );
    return tex.SampleLevel( smpWrap, InterpUV( t, bary ), lod ).a >= 0.5;
}

// --- Tracing ---
struct Hit
{
    bool   valid;
    float  t;
    uint   instanceId, instanceIndex, geometry, primitive;
    float2 bary;
    float3x4 objectToWorld;
};

Hit TraceReflection( float3 origin, float3 dir, float coneSpread, float coneBase )
{
    Hit h = (Hit)0;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1.0;
    ray.TMax = MaxDistance;

    [branch] if ( Textured == 0u )
    {
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline( Scene, RAY_FLAG_NONE, 0xFF, ray );
        q.Proceed();
        if ( q.CommittedStatus() != COMMITTED_TRIANGLE_HIT ) return h;
        h.valid = true;
        h.t = q.CommittedRayT();
        h.instanceId = q.CommittedInstanceID();
        h.instanceIndex = q.CommittedInstanceIndex();
        h.geometry = q.CommittedGeometryIndex();
        h.primitive = q.CommittedPrimitiveIndex();
        h.bary = q.CommittedTriangleBarycentrics();
        h.objectToWorld = q.CommittedObjectToWorld3x4();
        return h;
    }

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline( Scene, RAY_FLAG_NONE, 0xFF, ray );
    while ( q.Proceed() )
    {
        if ( q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE ) continue;
        Tri t = FetchTri( q.CandidateInstanceID(), q.CandidateInstanceIndex(), q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex() );
        if ( PassesAlpha( t, q.CandidateTriangleBarycentrics(), coneBase + coneSpread * q.CandidateTriangleRayT() ) )
            q.CommitNonOpaqueTriangleHit();
    }
    if ( q.CommittedStatus() != COMMITTED_TRIANGLE_HIT ) return h;
    h.valid = true;
    h.t = q.CommittedRayT();
    h.instanceId = q.CommittedInstanceID();
    h.instanceIndex = q.CommittedInstanceIndex();
    h.geometry = q.CommittedGeometryIndex();
    h.primitive = q.CommittedPrimitiveIndex();
    h.bary = q.CommittedTriangleBarycentrics();
    h.objectToWorld = q.CommittedObjectToWorld3x4();
    return h;
}

// 1 = sun visible
float TraceSunShadow( float3 origin, float3 dir )
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 2.0;
    ray.TMax = 60000.0;

    [branch] if ( AlphaShadows == 0u )
    {
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline( Scene, RAY_FLAG_NONE, 0xFF, ray );
        q.Proceed();
        return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
    }

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline( Scene, RAY_FLAG_NONE, 0xFF, ray );
    while ( q.Proceed() )
    {
        if ( q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE ) continue;
        Tri t = FetchTri( q.CandidateInstanceID(), q.CandidateInstanceIndex(), q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex() );
        if ( PassesAlpha( t, q.CandidateTriangleBarycentrics(), 8.0 ) )
            q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

// --- Shading ---
float3 ApplyFog( float3 rgb, float distance )
{
    float f = saturate( ( distance - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return lerp( rgb, SrgbToLinear( FogColor ), f );
}

// Sun + sky + night fill, a reduced ComputeSunLightingPBR (Lambert only, no specular)
float3 LightSurface( float3 albedo, float3 N, float3 wpos, float vertLighting, bool castShadowRay )
{
    float3 sunCol = SrgbToLinear( SunColor );
    float  sunLum = dot( sunCol, float3( 0.3333, 0.3333, 0.3333 ) );
    float  worldAO = lerp( 1.0, vertLighting, WorldAOStrength );
    float  shadowAO = lerp( 1.0, vertLighting, ShadowAOStrength );

    float3 ambient = albedo * AmbientStrength * sunLum * shadowAO;
    [branch] if ( SkyIrradianceIndex != 0xFFFFFFFFu )
    {
        TextureCube irr = ResourceDescriptorHeap[SkyIrradianceIndex];
        float skyVis = lerp( 1.0, vertLighting, SkyOccStrength );
        float3 ibl = irr.SampleLevel( smpClamp, N, 0 ).rgb * albedo * shadowAO * SkyIblIntensity;
        ambient = lerp( ambient, ibl, skyVis );
    }

    float NdotL = saturate( dot( N, SunDirWS ) );
    float shadow = vertLighting;
    [branch] if ( NdotL > 0.0 && SunIntensity > 0.0 )
    {
        if ( castShadowRay ) shadow = TraceSunShadow( wpos + N * 4.0, SunDirWS );
    }
    float3 direct = albedo * ( 1.0 / PI ) * sunCol * SunIntensity * NdotL * shadow * worldAO;
    float3 night = albedo * SrgbToLinear( NightFill ) * worldAO;
    return ambient + direct + night;
}

// Linear color of what the reflection ray hit, before coverage weighting
float3 ShadeHit( Hit h, float3 origin, float3 dir, float coneWidth )
{
    float3 wpos = origin + dir * h.t;
    float distance = length( wpos - CamPos );

    // Hits the camera can see reuse the fully lit scene color, point lights included
    [branch] if ( ScreenReuse != 0u )
    {
        float4 clip = mul( float4( wpos - CamPos, 1.0 ), ViewProjRel );
        if ( clip.w > 1.0 )
        {
            float2 uv = clip.xy / clip.w * float2( 0.5, -0.5 ) + 0.5;
            if ( all( uv > 0.002 ) && all( uv < 0.998 ) )
            {
                int2 px = int2( uv * float2( FullSize ) );
                Texture2D<float> depthTex = ResourceDescriptorHeap[SceneDepthIndex];
                float raw = depthTex.Load( int3( px, 0 ) );
                if ( raw > 0.0 )
                {
                    float sceneZ = ProjB / ( raw - ProjA );
                    if ( abs( sceneZ - clip.w ) < max( 30.0, clip.w * 0.015 ) )
                    {
                        Texture2D sceneTex = ResourceDescriptorHeap[SceneColorIndex];
                        return sceneTex.Load( int3( px, 0 ) ).rgb;
                    }
                }
            }
        }
    }

    Tri t = FetchTri( h.instanceId, h.instanceIndex, h.geometry, h.primitive );
    float3 nObj = cross( t.p[1] - t.p[0], t.p[2] - t.p[0] );
    float3 N = normalize( mul( (float3x3)h.objectToWorld, nObj ) );
    if ( dot( N, dir ) > 0.0 ) N = -N;   // both faces are traced; light the side the ray arrived on
    float vertLighting = t.light[0] + h.bary.x * ( t.light[1] - t.light[0] ) + h.bary.y * ( t.light[2] - t.light[0] );

    float3 albedo;
    [branch] if ( Textured == 0u || ( t.material & kMatNoTexture ) != 0u )
    {
        albedo = float3( 0.10, 0.075, 0.05 );   // dark brown silhouette
    }
    else
    {
        Texture2D tex = ResourceDescriptorHeap[t.material & kMatSlotMask];
        float lod = max( TextureLod( t, tex, coneWidth, abs( dot( N, dir ) ) ), 0.0 );
        albedo = DelightDiffuse( SrgbToLinear( tex.SampleLevel( smpWrap, InterpUV( t, h.bary ), lod ).rgb ) );
    }

    float3 rgb = LightSurface( albedo, N, wpos, vertLighting, ShadowRays != 0u );
    return ApplyFog( rgb, distance );
}

// --- Per sample ---
float SurfaceRaw( int2 px ) { Texture2D<float> d = ResourceDescriptorHeap[SurfaceDepthIndex]; return d.Load( int3( px, 0 ) ); }
float SceneRaw( int2 px )   { Texture2D<float> d = ResourceDescriptorHeap[SceneDepthIndex];   return d.Load( int3( px, 0 ) ); }

// The water prepass is the only depth written between the two copies, so a nearer surface depth is water.
bool IsWater( int2 px ) { return SurfaceRaw( px ) > SceneRaw( px ) * 1.000001 + 1e-9; }

// Adds one ray's contribution; false when the position is not water
bool TraceSample( int2 px, float2 subPixel, inout float4 color, inout float distance )
{
    float raw = SurfaceRaw( px );
    float2 uv = ( float2( px ) + subPixel ) * InvFullSize;
    float4 rel = mul( float4( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, raw, 1.0 ), InvViewProjRel );
    float3 relPos = rel.xyz / rel.w;
    float3 worldPos = CamPos + relPos;
    float viewDistance = length( relPos );
    float3 viewDir = relPos / max( viewDistance, 1e-4 );

    // LOD the pixel shader's trilinear lookup would take: the footprint's long axis on the water plane
    float texW, texH;
    Texture2D distTex = ResourceDescriptorHeap[WaveDistortionIndex];
    distTex.GetDimensions( texW, texH );
    float footprint = viewDistance * PixelAngle / max( abs( viewDir.y ), 0.02 );
    g_DistortionLod = log2( max( footprint * ( DIST_BIG_SCALE / 1000.0 ) * texW, 1e-4 ) );

    float3 waves = WaterWaveNormal( WaterBigDistortion( worldPos.xz / 1000.0f, Time ) );
    float3 dir = WaterGeometryReflectionDir( viewDir, waves, viewDistance );
    dir.y = max( dir.y, 0.02 );   // steep waves must not send the ray into the lake bed
    dir = normalize( dir );

    float spread = PixelAngle * float( Scale );
    float3 origin = worldPos + float3( 0.0, 2.0, 0.0 );
    Hit h = TraceReflection( origin, dir, spread, spread * viewDistance );
    if ( !h.valid ) return true;

    float coverage = saturate( ( MaxDistance - h.t ) / ( MaxDistance * 0.15 ) );
    float3 rgb = ShadeHit( h, origin, dir, spread * ( viewDistance + h.t ) );
    color += float4( rgb * coverage, coverage );
    distance += h.t * coverage;
    return true;
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID )
{
    if ( any( id.xy >= RtSize ) ) return;
    g_DistortionLod = 0.0;

    float4 color = 0.0;
    float distance = 0.0;
    float samples = 0.0;
    int2 base = int2( id.xy * Scale );
    int2 maxPx = int2( FullSize ) - 1;

    [branch] if ( SampleMode == 2u )
    {
        // Full res: four rotated-grid sub-pixel rays
        static const float2 kJitter[4] = { float2( 0.375, 0.125 ), float2( 0.875, 0.375 ), float2( 0.625, 0.875 ), float2( 0.125, 0.625 ) };
        if ( IsWater( base ) )
        {
            [loop] for ( uint s = 0; s < 4u; ++s ) TraceSample( base, kJitter[s], color, distance );
            samples = 4.0;
        }
    }
    else
    {
        // Half res: the 2x2 block's water pixels; one of them, or every one of them
        static const int2 kOrder[4] = { int2( 0, 0 ), int2( 1, 1 ), int2( 1, 0 ), int2( 0, 1 ) };
        [loop] for ( uint s = 0; s < 4u; ++s )
        {
            int2 px = min( base + kOrder[s], maxPx );
            if ( !IsWater( px ) ) continue;
            TraceSample( px, float2( 0.5, 0.5 ), color, distance );
            samples += 1.0;
            if ( SampleMode == 0u ) break;
        }
    }

    float inv = samples > 0.0 ? 1.0 / samples : 0.0;
    RWTexture2D<float4> outColor = ResourceDescriptorHeap[OutColorIndex];
    RWTexture2D<float>  outDistance = ResourceDescriptorHeap[OutDistanceIndex];
    outColor[id.xy] = color * inv;
    outDistance[id.xy] = distance * inv;
}
