// Rain wetness shared by the D3D11 deferred passes (PS_DS_AtmosphericScattering, CS_TiledShading,
// PS_DS_PointLight*), so the sun/ambient and point-light terms shade the same wet surface.
#ifndef RAIN_WETNESS_SAMPLE_H
#define RAIN_WETNESS_SAMPLE_H

// Inner 8-tap ring of ShadowSampling.h's g_PoissonDisk32 (that header also declares CSM-only resources).
static const float2 g_RainPoissonInner8[8] = {
    float2( -0.94201624, -0.39906216 ), float2(  0.94558609, -0.76890725 ),
    float2( -0.09418410, -0.92938870 ), float2(  0.34495938,  0.29387760 ),
    float2( -0.91588581,  0.45771432 ), float2( -0.81544232, -0.87912464 ),
    float2( -0.38277543,  0.27676845 ), float2(  0.97484398,  0.75648379 )
};

#ifndef RAIN_WET_BLUR_WORLD
#define RAIN_WET_BLUR_WORLD 100.0f   // ~1m filter radius => ~2m wide wet/dry transition, matches ShadowSampling.h
#endif

static const float RAIN_PI = 3.14159265f;

// ShadowSampling.h's ComputeRainWetness without the 16-tap outer ring.
float ComputeRainWetnessLite( float3 wsPosition, Texture2D rainMap, SamplerComparisonState samplerState, matrix viewProj )
{
    float4 sp = mul( float4( wsPosition, 1 ), viewProj );
    sp.xyz /= sp.www;   // orthographic rain camera, w == 1 -- kept for parity with ComputeRainWetness
    float2 uv = sp.xy * float2( 0.5f, -0.5f ) + float2( 0.5f, 0.5f );

    // Outside the rain camera there is no occluder information; assume open sky.
    if ( uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f )
        return 1.0f;

    float sx = length( float3( viewProj._11, viewProj._21, viewProj._31 ) );
    float sy = length( float3( viewProj._12, viewProj._22, viewProj._32 ) );
    float sz = length( float3( viewProj._13, viewProj._23, viewProj._33 ) );

    float2 radiusUV = float2( sx, sy ) * ( 0.5f * RAIN_WET_BLUR_WORLD );
    float bias = 0.0001f + RAIN_WET_BLUR_WORLD * sz;
    float zReceiver = sp.z - bias;

    const float wCenter = 1.0f;
    const float rInner  = 0.45f;
    const float wInner  = 0.66698f;   // exp(-0.45^2 * 2)

    float sum = wCenter * rainMap.SampleCmpLevelZero( samplerState, uv, zReceiver );
    float weight = wCenter;

    [unroll]
    for ( int i = 0; i < 8; ++i )
    {
        float2 offset = g_RainPoissonInner8[i] * ( rInner * radiusUV );
        sum += wInner * rainMap.SampleCmpLevelZero( samplerState, uv + offset, zReceiver );
        weight += wInner;
    }

    return saturate( sum / weight );
}

static const float WET_FILM_FLATTEN     = 0.7f;    // share of the normal-map relief the water film fills
static const float WET_FILM_ROUGHNESS   = 0.40f;   // broad sheen pool around each light's reflection
static const float WET_PUDDLE_ROUGHNESS = 0.22f;   // tighter, but still a pool rather than a thin beam

static const float PUDDLE_FLATNESS_MIN       = 0.92f;    // dot(geometric normal, up); excludes pitched roofs
static const float PUDDLE_NOISE_WORLD_SCALE  = 400.0f;   // ~4 m placement tile
static const float PUDDLE_DETAIL_WORLD_SCALE = 120.0f;   // ~1.2 m perimeter breakup
static const float PUDDLE_RIPPLE_CELL        = 70.0f;    // world units per drop cell
static const float PUDDLE_RIPPLE_PERIOD      = 0.9f;     // seconds from impact to fade-out
static const float PUDDLE_RIPPLE_STRENGTH    = 0.5f;
static const float PUDDLE_RIPPLE_FADE_DIST   = 2500.0f;  // rings alias beyond this camera distance

// Animated tri-planar rain ripple from the distortion texture. World space, N normalized.
float3 RainRippleNormal( Texture2D distTex, SamplerState smp, float3 N, float3 wsPosition, float time, float rainFx )
{
    const float scale = 1000.0f;
    float groundSpeed = 0.1f * rainFx;
    float downSpeed = 0.2f * rainFx;
    float2 uv0 = wsPosition.zy / scale + float2( 0.0f, time * downSpeed );
    float2 uv1 = wsPosition.xz / ( scale * 2.0f ) + time * groundSpeed;
    float2 uv2 = wsPosition.xz / ( scale * 2.0f ) * float2( 0.8f, 1.2f ) + float2( -time * groundSpeed * 0.7f, time * groundSpeed * 0.4f );
    float2 uv3 = wsPosition.xy / scale + float2( 0.0f, time * downSpeed );

    // Tightened per-axis blend so each planar projection dominates near its own axis.
    float3 weights = max( ( abs( N ) - 0.55f ) * 0.7f, 0.0f );
    weights /= weights.x + weights.y + weights.z;
    weights *= float3( 0.6f, 0.7f, 0.6f );
    weights *= weights;
    weights *= weights;

    float3 d0 = normalize( distTex.SampleLevel( smp, uv0, 0 ).zyx * 2.0f - 1.0f );
    float3 d1 = normalize( distTex.SampleLevel( smp, uv1, 0 ).xzy * 2.0f - 1.0f ) * 0.5f
              + normalize( distTex.SampleLevel( smp, uv2, 0 ).xzy * 2.0f - 1.0f ) * 0.5f;
    float3 d2 = normalize( distTex.SampleLevel( smp, uv3, 0 ).xyz * 2.0f - 1.0f );

    float3 n = lerp( N, d0, weights.x * 0.9f );
    n = lerp( n, d1, weights.y * 0.9f );
    n = lerp( n, d2, weights.z * 0.9f );
    return normalize( n );
}

// Mip for a world-planar lookup from an approximate pixel footprint; capped so distant puddles keep contrast.
float RainNoiseLod( Texture2D tex, float tileWorld, float viewDist )
{
    float w, h;
    tex.GetDimensions( w, h );
    return clamp( log2( max( viewDist, 1.0f ) * 0.0015f * w / tileWorld ), 0.0f, 2.0f );
}

// Pooled water [0,1] on flat ground, spreading as wetness rises. geomN must be the geometric world normal.
float ComputePuddleMask( Texture2D distTex, SamplerState smp, float3 geomN, float3 wsPosition, float wetness, float viewDist )
{
    float flatness = saturate( ( geomN.y - PUDDLE_FLATNESS_MIN ) / ( 1.0f - PUDDLE_FLATNESS_MIN ) );
    if ( flatness <= 0.0f ) return 0.0f;

    float placement = distTex.SampleLevel( smp, wsPosition.xz / PUDDLE_NOISE_WORLD_SCALE,
        RainNoiseLod( distTex, PUDDLE_NOISE_WORLD_SCALE, viewDist ) ).r;
    float detail = distTex.SampleLevel( smp, wsPosition.xz / PUDDLE_DETAIL_WORLD_SCALE + 17.31f,
        RainNoiseLod( distTex, PUDDLE_DETAIL_WORLD_SCALE, viewDist ) ).g;
    float noise = placement * 0.7f + detail * 0.3f;

    float threshold = lerp( 0.85f, 0.35f, saturate( wetness ) );
    return smoothstep( threshold - 0.08f, threshold + 0.08f, noise ) * flatness;
}

uint PuddleHash( int2 c )
{
    uint h = ( uint( c.x ) * 0x8DA6B343u ) ^ ( uint( c.y ) * 0xD8163841u );
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

// World-XZ slope of the rain-drop rings: one expanding ring per cell on two offset grids, re-seeded every cycle.
float2 PuddleRippleSlope( float2 p, float time, float rainFx )
{
    float2 slope = 0.0f;
    [unroll] for ( int layer = 0; layer < 2; ++layer )
    {
        float2 q = p / PUDDLE_RIPPLE_CELL + float2( 0.5f, 0.37f ) * layer;
        int2 cell = int2( floor( q ) );
        float phase = float( PuddleHash( cell + int2( 0, 7919 * layer ) ) & 0xFFFFu ) / 65535.0f;
        float cycle = time / PUDDLE_RIPPLE_PERIOD + phase;
        uint h = PuddleHash( cell + int2( int( floor( cycle ) ) * 131, 7919 * layer + 17 ) );
        if ( float( h >> 24 ) / 255.0f <= rainFx )   // lighter rain, fewer drops
        {
            float t = frac( cycle );
            float2 center = 0.3f + 0.4f * float2( h & 0xFFu, ( h >> 8 ) & 0xFFu ) / 255.0f;
            float2 d = frac( q ) - center;
            float r = length( d );
            // 1.5 wavelengths either side of the expanding front.
            float x = clamp( ( r - t * 0.28f ) * 160.0f, -3.0f * RAIN_PI, 3.0f * RAIN_PI );
            float wave = sin( x ) * ( 1.0f - abs( x ) / ( 3.0f * RAIN_PI ) );
            slope += d / max( r, 1e-4f ) * ( wave * ( 1.0f - t ) * ( 1.0f - t ) );
        }
    }
    return slope * PUDDLE_RIPPLE_STRENGTH;
}

struct WetSurface
{
    float wetness;    // 0 = dry; the other fields are then just the inputs
    float puddle;
    float roughness;  // water-film lobe roughness
    float3 rippleN;   // world normal for diffuse lighting
    float3 coatN;     // world normal of the water film: relief filled, flat in puddles, drop rings
};

// reach = rain-map exposure * scene wetness. N = G-buffer world normal, geomN = geometric world normal.
WetSurface EvaluateWetSurface( float reach, float3 N, float3 geomN, float3 wsPosition, float viewDist,
    Texture2D distTex, SamplerState smp, float time, float rainFx )
{
    WetSurface s = (WetSurface)0;
    s.roughness = WET_FILM_ROUGHNESS;
    s.rippleN = N;
    s.coatN = N;
    if ( reach < 0.001f ) return s;

    float3 ripple = RainRippleNormal( distTex, smp, N, wsPosition, time, rainFx );

    // Rain settles on upward-facing, unsheltered surfaces.
    float wDot = saturate( -ripple.y );
    float wDot2 = wDot * wDot;
    float exposure = saturate( ripple.y );
    float wetness = reach * ( 1.0f - wDot2 * wDot2 ) * exposure * exposure;
    if ( wetness <= 0.0f ) return s;

    float puddle = ComputePuddleMask( distTex, smp, geomN, wsPosition, wetness, viewDist );
    // Ripples only while it rains; pooled water stays calmer.
    float rippleAmount = rainFx * wetness * 0.5f * ( 1.0f - puddle * 0.8f );

    s.wetness = wetness;
    s.puddle = puddle;
    s.roughness = lerp( WET_FILM_ROUGHNESS, WET_PUDDLE_ROUGHNESS, puddle );
    s.rippleN = normalize( lerp( N, ripple, rippleAmount ) );

    float3 filmN = normalize( lerp( N, geomN, wetness * lerp( WET_FILM_FLATTEN, 1.0f, puddle ) ) );
    filmN = normalize( lerp( filmN, float3( 0.0f, 1.0f, 0.0f ), puddle ) );
    filmN = normalize( lerp( filmN, ripple, rippleAmount ) );

    // Drop rings go into the diffuse normal too, so puddles read even with every reflection off.
    float rippleFade = saturate( 1.0f - viewDist / PUDDLE_RIPPLE_FADE_DIST );
    [branch]
    if ( puddle > 0.0f && rainFx > 0.0f && rippleFade > 0.0f )
    {
        float2 slope = PuddleRippleSlope( wsPosition.xz, time, rainFx ) * ( puddle * rippleFade );
        float3 ring = float3( -slope.x, 0.0f, -slope.y );
        filmN = normalize( filmN + ring );
        s.rippleN = normalize( s.rippleN + ring * 0.5f );   // diffuse sees rings faintly; the reflection carries them
    }
    s.coatN = filmN;
    return s;
}

// Darkens and desaturates wet albedo; pooled water reads darker still.
void ApplyWetAlbedo( inout float3 diffuse, WetSurface s )
{
    float lum = dot( diffuse, float3( 0.3333f, 0.3333f, 0.3333f ) );
    float k = lerp( 0.75f, 0.55f, s.puddle );
    diffuse = lerp( diffuse, lerp( lum.xxx, diffuse, k ) * k, s.wetness );
}

// Point-light passes: evaluates the wet surface once per pixel and damps albedo/Blinn-Phong spec to match the sun pass.
WetSurface ApplyPointLightWetness( float3 wsPosition, float3 wsNormal, float3 wsGeomNormal, float viewDist,
    Texture2D rainMap, SamplerComparisonState cmp, matrix rainViewProj, float sceneWetness,
    Texture2D distTex, SamplerState smp, float time, float rainFx,
    inout float3 diffuse, inout float specIntensity, inout float specPower )
{
    float reach = 0.0f;
    [branch]
    if ( sceneWetness > 0.0f )
        reach = ComputeRainWetnessLite( wsPosition, rainMap, cmp, rainViewProj ) * sceneWetness;

    WetSurface s = EvaluateWetSurface( reach, wsNormal, wsGeomNormal, wsPosition, viewDist, distTex, smp, time, rainFx );
    if ( s.wetness > 0.0f )
    {
        specIntensity = lerp( specIntensity, 0.0f, s.wetness );
        specPower = lerp( specPower, 150.0f, s.wetness );
        ApplyWetAlbedo( diffuse, s );
    }
    return s;
}

// Geometric view-space normal from screen derivatives (pixel shaders only); falls back to N at silhouettes.
float3 GeomNormalFromDerivativesVS( float3 vsPosition, float3 N )
{
    float3 n = cross( ddx( vsPosition ), ddy( vsPosition ) );
    n = normalize( n * sign( dot( n, N ) ) );
    return dot( n, N ) >= 0.5f ? n : N;   // also catches NaN from sky neighbours
}

static const float WET_COAT_STREAK         = 1.0f;    // elongation toward the viewer at grazing view; higher = thinner beam
static const float WET_LIGHT_SOURCE_RADIUS = 15.0f;   // world units (~15 cm flame)
static const float WET_COAT_GAIN           = 3.0f;    // light colours are LDR-scaled; a physical 2% water reflection is invisible
static const float WET_COAT_MAX            = 1.5f;    // asymptote of the per-light HDR shoulder below

// Soft HDR shoulder instead of a hard clamp, so a hot highlight doesn't flatten into a blown-out plateau.
float WetCoatRolloff( float x ) { return x / ( 1.0f + x / WET_COAT_MAX ); }
float3 WetCoatRolloff( float3 x ) { return x / ( 1.0f + x / WET_COAT_MAX ); }

// Water-film specular: anisotropic GGX stretched toward the viewer (wet-street streaks), widened by an assumed
// source size. N/V/L must share one space. Brighter than the D3D12 twin in PBRLighting.hlsl (WET_COAT_GAIN).
float WetCoatSpecular( float3 N, float3 V, float3 L, float lightDist, float roughness )
{
    float NdotL = saturate( dot( N, L ) );
    if ( NdotL <= 0.0f ) return 0.0f;
    float NdotV = saturate( dot( N, V ) );
    float3 H = normalize( V + L );

    // Karis sphere-light widening.
    float a = saturate( roughness * roughness + WET_LIGHT_SOURCE_RADIUS / ( 2.0f * max( lightDist, 1.0f ) ) );

    // Tangent = view projected onto the surface; the stretch fades out when looking straight down.
    float3 vt = V - N * dot( N, V );
    float vtLen = length( vt );
    float3 T = vtLen > 1e-3f ? vt / vtLen : normalize( cross( N, abs( N.x ) < 0.9f ? float3( 1, 0, 0 ) : float3( 0, 0, 1 ) ) );
    float3 B = cross( N, T );
    float at = min( a * lerp( 1.0f, WET_COAT_STREAK, vtLen ), 1.0f );
    float ab = a;

    float ht = dot( H, T ) / at;
    float hb = dot( H, B ) / ab;
    float hn = dot( N, H );
    float s = ht * ht + hb * hb + hn * hn;
    float D = 1.0f / ( RAIN_PI * at * ab * s * s );

    // Height-correlated anisotropic Smith visibility.
    float lv = NdotL * length( float3( at * dot( T, V ), ab * dot( B, V ), NdotV ) );
    float ll = NdotV * length( float3( at * dot( T, L ), ab * dot( B, L ), NdotL ) );
    float vis = 0.5f / max( lv + ll, 1e-4f );

    float f = saturate( 1.0f - dot( V, H ) );
    float f2 = f * f;
    float F = 0.02f + 0.98f * f2 * f2 * f;   // water F0
    return D * vis * F * NdotL * WET_COAT_GAIN;
}

#endif // RAIN_WETNESS_SAMPLE_H
