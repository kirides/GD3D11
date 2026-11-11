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

#include "include/RtScene.hlsl"
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

// --- Water waves: the same ray ShadeWater derives, filtered over the pixel's anisotropic water-plane footprint ---
SamplerState smpAniso : register( s2 );

// Required by WaterWaves.hlsl; the RT path samples with gradients in WaterBigDistortionGrad instead
float3 WaterDistortion( float2 uv )
{
    Texture2D tex = GD_HEAP( WaveDistortionIndex );
    return tex.SampleLevel( smpWrap, uv, 0 ).xyz;
}
#include "../include/WaterWaves.hlsl"

// WaterBigDistortion (same two octaves) with the pixel's world.xz/1000 gradients, so grazing views filter along the footprint
float3 WaterBigDistortionGrad( float2 worldTexCoord, float time, float2 ddx_w, float2 ddy_w )
{
    Texture2D tex = GD_HEAP( WaveDistortionIndex );
    const float2 m2 = float2( -1, 0.7 );
    float3 d = tex.SampleGrad( smpAniso, worldTexCoord * DIST_BIG_SCALE + time * DIST_BIG_SPEED, ddx_w * DIST_BIG_SCALE, ddy_w * DIST_BIG_SCALE ).xyz * 2 - 1;
    d += tex.SampleGrad( smpAniso, worldTexCoord * m2 * DIST_BIG_SCALE + time * DIST_BIG_SPEED * 1.2, ddx_w * m2 * DIST_BIG_SCALE, ddy_w * m2 * DIST_BIG_SCALE ).xyz * 2 - 1;
    return d * 0.5f;
}

// World-plane (y = relPos.y) camera-relative hit of the view ray through a pixel; the slope is clamped near the horizon
float2 PlaneHitXZ( float2 uv, float raw, float planeY )
{
    float4 r = mul( float4( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, raw, 1.0 ), InvViewProjRel );
    float3 n = normalize( r.xyz / r.w );
    float ny = planeY < 0.0 ? min( n.y, -0.02 ) : max( n.y, 0.02 );
    return n.xz * ( planeY / ny );
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

// Per-ray alpha threshold in [1/255, 254/255]: PCG hash of pixel, sample and Time bits (Time changes every frame).
// Hashed alpha testing turns the coarse-mip hard cut on reflected foliage into noise the TAA/blur resolves.
float AlphaThreshold( int2 px, uint sampleIndex )
{
    uint v = uint( px.x ) * 1664525u + uint( px.y ) * 22695477u + sampleIndex * 747796405u + asuint( Time ) * 2891336453u;
    v = v * 747796405u + 2891336453u;
    v = ( ( v >> ( ( v >> 28u ) + 4u ) ) ^ v ) * 277803737u;
    v = ( v >> 22u ) ^ v;
    return clamp( float( v >> 8 ) * ( 1.0 / 16777216.0 ), 1.0 / 255.0, 254.0 / 255.0 );
}

Hit TraceReflection( float3 origin, float3 dir, float coneSpread, float coneBase, float alphaThreshold )
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
        if ( PassesAlphaThreshold( t, q.CandidateTriangleBarycentrics(), coneBase + coneSpread * q.CandidateTriangleRayT(), LodBias, alphaThreshold ) )
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
    return TraceVisibility( ray, 0xFF, AlphaShadows != 0u, 8.0 );
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
        TextureCube irr = GD_HEAP( SkyIrradianceIndex );
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
                Texture2D<float> depthTex = GD_HEAP( SceneDepthIndex );
                float raw = depthTex.Load( int3( px, 0 ) );
                if ( raw > 0.0 )
                {
                    float sceneZ = ProjB / ( raw - ProjA );
                    if ( abs( sceneZ - clip.w ) < max( 30.0, clip.w * 0.015 ) )
                    {
                        Texture2D sceneTex = GD_HEAP( SceneColorIndex );
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
        Texture2D tex = GD_HEAP_NONUNIFORM( t.material & kMatSlotMask );   // per-hit material: divergent across the wave
        float lod = max( TextureLod( t, tex, coneWidth, abs( dot( N, dir ) ) ) + LodBias, 0.0 );
        albedo = DelightDiffuse( SrgbToLinear( tex.SampleLevel( smpWrap, InterpUV( t, h.bary ), lod ).rgb ) );
    }

    float3 rgb = LightSurface( albedo, N, wpos, vertLighting, ShadowRays != 0u );
    return ApplyFog( rgb, distance );
}

// --- Per sample ---
float SurfaceRaw( int2 px ) { Texture2D<float> d = GD_HEAP( SurfaceDepthIndex ); return d.Load( int3( px, 0 ) ); }
float SceneRaw( int2 px )   { Texture2D<float> d = GD_HEAP( SceneDepthIndex );   return d.Load( int3( px, 0 ) ); }

// The water prepass is the only depth written between the two copies, so a nearer surface depth is water.
bool IsWater( int2 px ) { return SurfaceRaw( px ) > SceneRaw( px ) * 1.000001 + 1e-9; }

// Adds one ray's contribution; false when the position is not water
bool TraceSample( int2 px, uint sampleIndex, float2 subPixel, inout float4 color, inout float distance )
{
    float raw = SurfaceRaw( px );
    float2 uv = ( float2( px ) + subPixel ) * InvFullSize;
    float4 rel = mul( float4( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, raw, 1.0 ), InvViewProjRel );
    float3 relPos = rel.xyz / rel.w;
    float3 worldPos = CamPos + relPos;
    float viewDistance = length( relPos );
    float3 viewDir = relPos / max( viewDistance, 1e-4 );

    // Footprint of this full-res pixel on the water plane along both screen axes
    float2 hit0 = PlaneHitXZ( uv, raw, relPos.y );
    float2 gx = clamp( PlaneHitXZ( uv + float2( InvFullSize.x, 0.0 ), raw, relPos.y ) - hit0, -4000.0, 4000.0 );
    float2 gy = clamp( PlaneHitXZ( uv + float2( 0.0, InvFullSize.y ), raw, relPos.y ) - hit0, -4000.0, 4000.0 );

    float3 waves = WaterWaveNormal( WaterBigDistortionGrad( worldPos.xz / 1000.0f, Time, gx / 1000.0f, gy / 1000.0f ) );
    float3 dir = WaterGeometryReflectionDir( viewDir, waves, viewDistance );
    // Rays the waves push below half the flat reflection's elevation blend back toward it; grazing flat reflections are legit
    float3 flatDir = reflect( viewDir, WATER_UP );
    dir = lerp( flatDir, dir, smoothstep( 0.0, 0.5 * max( flatDir.y, 0.005 ), dir.y ) );
    dir.y = max( dir.y, 0.002 );
    dir = normalize( dir );

    float spread = PixelAngle * float( Scale );
    float3 origin = worldPos + float3( 0.0, 2.0, 0.0 );
    Hit h = TraceReflection( origin, dir, spread, spread * viewDistance, AlphaThreshold( px, sampleIndex ) );
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
            [loop] for ( uint s = 0; s < 4u; ++s ) TraceSample( base, s, kJitter[s], color, distance );
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
            TraceSample( px, s, float2( 0.5, 0.5 ), color, distance );
            samples += 1.0;
            if ( SampleMode == 0u ) break;
        }
    }

    float inv = samples > 0.0 ? 1.0 / samples : 0.0;
    RWTexture2D<float4> outColor = GD_HEAP( OutColorIndex );
    RWTexture2D<float>  outDistance = GD_HEAP( OutDistanceIndex );
    outColor[id.xy] = color * inv;
    outDistance[id.xy] = distance * inv;
}
