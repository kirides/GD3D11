#ifndef WATER_SHADING_HLSL
#define WATER_SHADING_HLSL
// Water surface shading shared by PS_Water (D3D11) and D3D12/Water.hlsl; model and constants from
// MarcoMarwin's GD3D11 fork. Everything here works in D3D11's gamma space; D3D12 converts at its hooks.
// The includer defines: WaterSceneRawDepth, WaterSceneRawDepthTexel, WaterSurfaceRawDepth, WaterLinearDepth, WaterWorldToView,
// WaterViewToUV, WaterSceneColor, WaterDistortion, WaterDiffuse, WaterCube, WaterSSREnabled,
// WaterTraceSSR, WaterScatterGround, WaterLowClouds, WaterSkyAverage and WaterShoreField, plus the Atmosphere constants
// (AC_LightPos, AC_RainFXWeight).

#include "WaterWaves.hlsl"

static const float3 WATER_LUMA = float3( 0.2126f, 0.7152f, 0.0722f );

float WaterLuma( float3 c ) { return dot( c, WATER_LUMA ); }

//--------------------------------------------------------------------------------------
// Water body: Beer-Lambert absorption + in-scatter over the refracted water column
//--------------------------------------------------------------------------------------
static const float3 WATER_VOLUME_ABSORPTION = float3( 0.0020f, 0.00115f, 0.00165f ); // per world unit
static const float3 WATER_VOLUME_SCATTER    = float3( 0.050f, 0.080f, 0.060f );

// View-Z gap between surface and scene, stretched to the length along the view ray (1/cos, capped).
float WaterColumnLength( float sceneViewZ, float surfaceViewZ, float surfaceViewDistance )
{
    float viewRayScale = clamp( surfaceViewDistance / max( abs( surfaceViewZ ), 1.0f ), 1.0f, 8.0f );
    return clamp( max( sceneViewZ - surfaceViewZ, 0.0f ) * viewRayScale, 0.0f, 6000.0f );
}

// Fades in over 18..198 units so shorelines stay clear; optical depth soft-saturates at 1400 units.
float3 ApplyWaterVolume( float3 scene, float columnLength, float3 scatter )
{
    float influence = WaterSmootherStep01( ( columnLength - 18.0f ) / 180.0f );
    float opticalDepth = 1400.0f * ( 1.0f - exp( -columnLength / 1400.0f ) );
    float3 transmittance = exp( -WATER_VOLUME_ABSORPTION * opticalDepth );
    float3 volume = scene * transmittance + scatter * ( 1.0f - transmittance );
    return lerp( scene, volume, influence );
}

// Ocean (NW_WATER_LAKE* textures). climate: 0 = dense coastal water, 1 = clear turquoise (ADDONWORLD)
void GetOceanOptics( float climate, float rain, float night, float cameraBelow, float diffuseLuma,
                     out float3 absorption, out float3 scatter )
{
    float3 absDay = lerp( float3( 0.0031f, 0.00165f, 0.00108f ), float3( 0.0024f, 0.00115f, 0.00062f ), climate );
    float3 absRain = float3( 0.0030f, 0.00155f, 0.00088f );
    absorption = lerp( absDay, absRain, rain ) * lerp( 1.0f, 0.58f, cameraBelow );

    float3 clearDay = lerp( float3( 0.043f, 0.082f, 0.091f ), float3( 0.026f, 0.132f, 0.174f ), climate );
    float3 rainDay = float3( 0.070f, 0.066f, 0.064f );
    float3 clearNight = float3( 0.008f, 0.017f, 0.034f );
    float3 rainNight = lerp( float3( 0.010f, 0.014f, 0.017f ), float3( 0.020f, 0.032f, 0.044f ), 0.72f );
    scatter = lerp( lerp( clearDay, rainDay, rain ), lerp( clearNight, rainNight, rain ), night );
    scatter *= lerp( 0.94f, 1.06f, saturate( diffuseLuma * 1.4f ) );
}

//--------------------------------------------------------------------------------------
// Shoreline. x = reflection/glint visibility, y = water-body color; both 0 at the waterline.
//--------------------------------------------------------------------------------------
static const float WATER_DEEP_WATER_DEPTH  = 100.0f;      // world units below the surface
static const float WATER_SHORE_RING_RADIUS = 75.0f;
static const float WATER_NO_FLOOR_DEPTH    = 1000000.0f;

float2 OceanShore( float column, float columnDeriv )
{
    float fadeEnd = clamp( max( 65.0f, columnDeriv * 1.25f ), 65.0f, 160.0f );
    float visibility = WaterSmootherStep01( ( column - 1.0f ) / max( max( 240.0f, fadeEnd * 2.60f ) - 1.0f, 1.0f ) );
    float color = WaterSmootherStep01( ( column - 1.0f ) / max( max( 110.0f, fadeEnd * 1.35f ) - 1.0f, 1.0f ) );
    return float2( visibility, color );
}

// colorColumn is the smaller of the refracted and undistorted columns, widened on sloped banks.
float2 LegacyShore( float column, float columnDeriv, float colorColumn, float colorColumnDeriv )
{
    float fadeEnd = clamp( max( 65.0f, max( columnDeriv, 1.0f ) * 1.25f ), 65.0f, 160.0f );
    float visibility = WaterSmootherStep01( ( column - 1.0f ) / max( fadeEnd - 1.0f, 1.0f ) );
    float colorStart = max( 22.0f, colorColumnDeriv * 0.18f );
    float colorEnd = clamp( max( 260.0f, colorColumnDeriv * 2.85f ), 260.0f, 520.0f );
    float color = WaterSmootherStep01( ( colorColumn - colorStart ) / max( colorEnd - colorStart, 1.0f ) );
    return float2( visibility, color );
}

// Vertical water depth below a surface point; the scene texel lies on the same camera ray.
float WaterDepthBelowSurface( float3 surfaceWS, float surfaceViewZ, float sceneRawDepth, float3 cameraWS )
{
    if ( sceneRawDepth <= 0.000001f ) return WATER_NO_FLOOR_DEPTH;
    float sceneViewZ = WaterLinearDepth( sceneRawDepth );
    float3 sceneWS = cameraWS + ( surfaceWS - cameraWS ) * ( sceneViewZ / max( surfaceViewZ, 0.001f ) );
    return max( surfaceWS.y - sceneWS.y, 0.0f );
}

// 1 when all 8 ring probes are covered, deep water: the shallow spot is a submerged rock, not a shore.
float WaterShoreProbeException( float3 surfaceWS, float3 cameraWS )
{
    const float2 ring[8] = {
        float2( 1.0f, 0.0f ), float2( -1.0f, 0.0f ), float2( 0.0f, 1.0f ), float2( 0.0f, -1.0f ),
        float2( 0.70710678f, 0.70710678f ), float2( -0.70710678f, 0.70710678f ),
        float2( 0.70710678f, -0.70710678f ), float2( -0.70710678f, -0.70710678f )
    };

    float waterProbes = 0.0f;
    float deepProbes = 0.0f;
    [unroll]
    for ( int i = 0; i < 8; ++i )
    {
        float3 probeWS = surfaceWS + float3( ring[i].x, 0.0f, ring[i].y ) * WATER_SHORE_RING_RADIUS;
        float3 probeVS = WaterWorldToView( probeWS );
        float2 uv;
        if ( probeVS.z <= 1.0f || !WaterViewToUV( probeVS, uv ) || any( uv < 0.0f ) || any( uv > 1.0f ) )
            continue;

        // Covered: the water prepass left a surface at least one unit in front of the scene here.
        float sceneRaw = WaterSceneRawDepth( uv );
        float surfaceRaw = WaterSurfaceRawDepth( uv );
        float sceneZ = sceneRaw > 0.000001f ? WaterLinearDepth( sceneRaw ) : WATER_NO_FLOOR_DEPTH;
        float surfaceZ = surfaceRaw > 0.000001f ? WaterLinearDepth( surfaceRaw ) : WATER_NO_FLOOR_DEPTH;
        float covered = step( 1.0f, sceneZ - surfaceZ );

        float deep = step( WATER_DEEP_WATER_DEPTH, WaterDepthBelowSurface( probeWS, probeVS.z, sceneRaw, cameraWS ) );
        waterProbes += covered;
        deepProbes += covered * deep;
    }
    return step( 7.5f, waterProbes ) * step( 7.5f, deepProbes );
}

//--------------------------------------------------------------------------------------
// Steep water and scene hue
//--------------------------------------------------------------------------------------
// 1 on flat water, 0 on waterfalls; reflections fade between 50 and 39 degrees of tilt.
float WaterFlatness( float3 geometricNormalWS )
{
    return smoothstep( 0.64278761f, 0.77714596f, abs( normalize( geometricNormalWS ).y ) );
}

// The scene's hue at the water color's luma, held within 35% per channel of the water color.
float3 WaterSceneHue( float3 color, float3 sceneClean )
{
    float3 sceneChroma = sceneClean / max( WaterLuma( sceneClean ), 0.001f );
    float3 hued = sceneChroma * max( WaterLuma( color ), 0.001f );
    float3 limit = max( abs( color ) * 0.35f, 0.005f );
    return clamp( hued, color - limit, color + limit );
}

// Brightest-channel-preserving cap: scales `c` down to at most `lumaLimit`.
float3 WaterLimitLuma( float3 c, float lumaLimit )
{
    return c * min( 1.0f, lumaLimit / max( WaterLuma( c ), 0.0001f ) );
}

//--------------------------------------------------------------------------------------
// Reflection sources: screen-space sky, the weather-aware fallback, processed geometry hits
//--------------------------------------------------------------------------------------
// Marches the reflection ray outward and keeps the last on-screen texel that shows open sky.
float WaterSkyMarch( float3 worldPos, float3 skyDir, out float2 skyUV )
{
    skyUV = float2( 0.0f, 0.0f );
    float valid = 0.0f;
    float3 pos = worldPos;
    float stepSize = 180.0f;
    [loop]
    for ( int k = 0; k < 14; ++k )
    {
        pos += skyDir * stepSize;
        stepSize *= 1.42f;
        float3 posVS = WaterWorldToView( pos );
        float2 uv;
        if ( posVS.z <= 0.001f || !WaterViewToUV( posVS, uv ) || any( uv < 0.0f ) || any( uv > 1.0f ) )
            break;
        if ( WaterSceneRawDepth( uv ) <= 0.000001f )
        {
            skyUV = uv;
            valid = 1.0f;
        }
    }
    return valid;
}

void AccumulateWaterSkySample( float2 uv, inout float3 sum, inout float weight )
{
    if ( any( uv < 0.0f ) || any( uv > 1.0f ) ) return;
    if ( WaterSceneRawDepth( uv ) > 0.000001f ) return;
    sum += WaterSceneColor( uv );
    weight += 1.0f;
}

// The reflected sky with the sun and moon discs replaced by the surrounding sky; the glints draw those.
float3 WaterSkyWithoutCelestialBodies( float2 uv, float3 ray, float3 fallback, float2 viewportSize,
                                       float sunVisibility, float3 moonDir, float moonVisibility )
{
    float3 current = WaterSceneColor( uv );
    float3 r = normalize( ray );
    float sunMask = smoothstep( 0.9962f, 0.9988f, dot( r, normalize( AC_LightPos ) ) ) * sunVisibility;
    float moonMask = smoothstep( 0.9925f, 0.9982f, dot( r, moonDir ) ) * moonVisibility;
    float mask = saturate( max( sunMask, moonMask ) );
    if ( mask <= 0.0001f ) return current;

    float2 texel = 1.0f / viewportSize;
    float2 near = texel * 64.0f;
    float2 far = texel * 128.0f;
    float3 sum = 0.0f;
    float weight = 0.0f;
    AccumulateWaterSkySample( uv + float2( near.x, 0.0f ), sum, weight );
    AccumulateWaterSkySample( uv - float2( near.x, 0.0f ), sum, weight );
    AccumulateWaterSkySample( uv + float2( 0.0f, near.y ), sum, weight );
    AccumulateWaterSkySample( uv - float2( 0.0f, near.y ), sum, weight );
    AccumulateWaterSkySample( uv + far, sum, weight );
    AccumulateWaterSkySample( uv - far, sum, weight );
    AccumulateWaterSkySample( uv + float2( far.x, -far.y ), sum, weight );
    AccumulateWaterSkySample( uv + float2( -far.x, far.y ), sum, weight );
    return lerp( current, weight > 0.5f ? sum / weight : fallback, mask );
}

// Stand-in for missing reflections: the static cube by clear day, overcast grey in rain, dark blue at night.
float3 WaterReflectionFallback( float3 cube, float rain, float night, bool isOcean )
{
    float3 dayRain = lerp( WaterLuma( cube ).xxx * 0.46f, float3( 0.18f, 0.20f, 0.21f ), 0.55f );
    float3 clearNight = lerp( cube * 0.025f, float3( 0.004f, 0.009f, 0.023f ), 0.72f );
    float3 rainNight = isOcean ? lerp( float3( 0.006f, 0.008f, 0.012f ), float3( 0.018f, 0.027f, 0.040f ), 0.70f )
                               : float3( 0.012f, 0.018f, 0.030f );
    return lerp( lerp( cube, dayRain, rain ), lerp( clearNight, rainNight, rain ), night );
}

// The low cloud layer (premultiplied) as seen in the reflected sky: rain hides it, rain and night veil it.
float4 ResolveWaterLowClouds( float4 rawClouds, float3 skyColor )
{
    float rain = saturate( AC_RainFXWeight );
    float nightBlend = smoothstep( 0.0f, 1.0f, saturate( -AC_LightPos.y * 4.0f ) );
    float visibility = 1.0f - smoothstep( 0.18f, 0.88f, rain );
    float veil = saturate( rain * lerp( 0.050f, 0.22f, nightBlend ) + ( 1.0f - rain ) * nightBlend * 0.12f );
    float alpha = saturate( rawClouds.a ) * visibility;
    float3 premultiplied = max( rawClouds.rgb, 0.0f ) * visibility;
    if ( alpha > 0.001f && veil > 0.0001f )
    {
        float3 cloudColor = lerp( premultiplied / max( alpha, 0.001f ), skyColor, veil * lerp( 0.65f, 1.0f, nightBlend ) );
        alpha *= 1.0f - veil * lerp( 0.08f, 0.22f, nightBlend );
        premultiplied = cloudColor * alpha;
    }
    return float4( premultiplied, alpha );
}

// Tames screen-space hits: soft HDR roll-off, a little contrast, compressed daylight highlights.
float3 ProcessWaterHitReflection( float3 c, float night )
{
    c = max( c, 0.0f );
    c *= rcp( 1.0f + max( 0.0f, WaterLuma( c ) - 6.0f ) * 0.12f );
    float luma = WaterLuma( c );
    c = max( lerp( luma.xxx, c, lerp( 1.10f, 1.05f, night ) ), 0.0f );
    luma = WaterLuma( c );

    const float highlightStart = 0.62f;
    float excess = max( luma - highlightStart, 0.0f );
    float compressed = highlightStart + excess / ( 1.0f + excess * 1.65f );
    c *= lerp( 1.0f, min( 1.0f, compressed / max( luma, 0.0001f ) ), 1.0f - night );
    return c;
}

//--------------------------------------------------------------------------------------
// Moon on the water: sparkles on the small waves plus a broad sheen that forms the moon path
//--------------------------------------------------------------------------------------
static const float3 WATER_MOON_GLINT_COLOR = float3( 0.60f, 0.70f, 1.00f );

float3 WaterMoonGlint( float3 viewDirection, float3 smallWaveNormal, float3 bigWaveNormal, float3 moonDir )
{
    float sparkle = pow( saturate( dot( reflect( -viewDirection, smallWaveNormal ), -moonDir ) ), 420.0f ) * 0.15f;
    float path = pow( saturate( dot( reflect( -viewDirection, bigWaveNormal ), -moonDir ) ), 48.0f ) * 0.035f;
    return WATER_MOON_GLINT_COLOR * ( sparkle + path );
}

//--------------------------------------------------------------------------------------
// Shore foam: a line at the waterline, swash bands running up the shore, a drifting bubble lace
//--------------------------------------------------------------------------------------
static const float  WATER_FOAM_FADE_DISTANCE = 20000.0f;   // view distance where foam is gone
static const float3 WATER_FOAM_ALBEDO = float3( 0.76f, 0.79f, 0.80f );
static const float  WATER_FOAM_BUMP = 3.0f;                 // world units of foam relief

// 1 = inside the underwater scene, `gap` view units behind it; 0 = in front of it; -1 = off screen or hidden
// behind something nearer or above the water (a swimmer's head must not read as a shallow floor)
float WaterProbeSolid( float3 posWS, float3 cameraWS, float waterY, float thickness, out float gap )
{
    gap = 0.0f;
    float3 posVS = WaterWorldToView( posWS );
    float2 uv;
    if ( posVS.z <= 1.0f || !WaterViewToUV( posVS, uv ) || any( uv < 0.0f ) || any( uv > 1.0f ) )
        return -1.0f;
    // Unfiltered: filtered depth at a silhouette is a phantom surface the probe would "hit"
    float raw = WaterSceneRawDepthTexel( uv );
    if ( raw <= 0.000001f ) return 0.0f;
    float sceneZ = WaterLinearDepth( raw );
    gap = posVS.z - sceneZ;
    if ( gap < 0.0f ) return 0.0f;
    float sceneY = cameraWS.y + ( posWS.y - cameraWS.y ) * ( sceneZ / posVS.z );
    return ( gap < thickness && sceneY < waterY + 4.0f ) ? 1.0f : -1.0f;
}

// Floor depth straight below a surface point, by walking the plumb line down in screen space. Unlike the depth
// at the view ray's floor hit it does not change with the viewing angle. > maxDepth = deeper, -1 = unknown.
float WaterPlumbDepth( float3 surfaceWS, float3 cameraWS, float maxDepth, float viewDirY, float footprint )
{
    const int steps = 6;
    float stepLen = maxDepth / steps;
    // A view ray grazing a flat floor runs far through it per unit of depth; one texel spans `footprint`
    float rayStretch = 1.0f / max( abs( viewDirY ), 0.04f );
    float thickness = clamp( stepLen * 1.5f * rayStretch + footprint, 20.0f, 400.0f );
    float lo = 0.0f;
    float hi = maxDepth * 2.0f;
    float gap = 0.0f;
    [loop]
    for ( int i = 1; i <= steps; ++i )
    {
        float t = stepLen * i;
        float state = WaterProbeSolid( surfaceWS - float3( 0.0f, t, 0.0f ), cameraWS, surfaceWS.y, thickness, gap );
        if ( state < 0.0f ) return -1.0f;
        if ( state > 0.0f ) { hi = t; break; }
        lo = t;
    }
    if ( hi > maxDepth ) return hi;

    float hiGap = gap;
    [unroll]
    for ( int j = 0; j < 4; ++j )
    {
        float mid = ( lo + hi ) * 0.5f;
        float midGap;
        if ( WaterProbeSolid( surfaceWS - float3( 0.0f, mid, 0.0f ), cameraWS, surfaceWS.y, thickness, midGap ) > 0.0f )
        {
            hi = mid;
            hiGap = midGap;
        }
        else lo = mid;
    }
    // A floor closes in on the probe as the bisection narrows; a post in front of the plumb line stays put
    return hiGap < max( 4.0f, ( hi - lo ) * 2.5f * rayStretch + footprint ) ? ( lo + hi ) * 0.5f : -1.0f;
}

// PCG2D (Jarzynski & Olano) in [0, 1); integer math, so every GPU and both compilers agree
float2 WaterFoamHash( int2 cell )
{
    uint2 v = asuint( cell ) * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return float2( v >> 8u ) * ( 1.0f / 16777216.0f );
}

float WaterFoamNoise( float2 p )
{
    float2 c = floor( p );
    float2 f = p - c;
    f = f * f * ( 3.0f - 2.0f * f );
    int2 i = int2( c );
    float a = WaterFoamHash( i ).x;
    float b = WaterFoamHash( i + int2( 1, 0 ) ).x;
    float d = WaterFoamHash( i + int2( 0, 1 ) ).x;
    float e = WaterFoamHash( i + int2( 1, 1 ) ).x;
    return lerp( lerp( a, b, f.x ), lerp( d, e, f.x ), f.y );
}

// Bubble lace: 1 along the borders of drifting cells, 0 in their middles; grad = its gradient per cell. Per-cell
// weights bend the borders and vary the cell sizes; width = strand thickness in cells.
float WaterFoamLace( float2 p, float time, float width, out float2 grad )
{
    float2 c = floor( p );
    float2 f = p - c;
    int2 i = int2( c );
    float d1 = 8.0f;   // weighted squared distance to the nearest and second-nearest point
    float d2 = 8.0f;
    float2 g1 = float2( 0.0f, 0.0f );   // weight * (point - p); the distance's gradient is -g / distance
    float2 g2 = float2( 0.0f, 0.0f );
    [unroll]
    for ( int y = -1; y <= 1; ++y )
    {
        [unroll]
        for ( int x = -1; x <= 1; ++x )
        {
            float2 h = WaterFoamHash( i + int2( x, y ) );
            float2 pt = float2( x, y ) + 0.5f + 0.36f * sin( time * ( 0.7f + h.yx * 0.6f ) + h * 6.2831853f ) - f;
            float w = lerp( 0.65f, 1.35f, h.x );
            float dist = dot( pt, pt ) * w;
            bool nearest = dist < d1;
            bool second = dist < d2;
            d2 = nearest ? d1 : ( second ? dist : d2 );
            g2 = nearest ? g1 : ( second ? pt * w : g2 );
            d1 = nearest ? dist : d1;
            g1 = nearest ? pt * w : g1;
        }
    }
    float f1 = max( sqrt( d1 ), 0.0001f );
    float f2 = max( sqrt( d2 ), 0.0001f );
    float u = saturate( ( f2 - f1 ) / width );
    grad = -( 6.0f * u * ( 1.0f - u ) / width ) * ( g1 / f1 - g2 / f2 );
    return 1.0f - u * u * ( 3.0f - 2.0f * u );
}

struct WaterFoamSample
{
    float  opacity;
    float  thickness;   // 0 on thin strands, 1 in dense foam
    float2 slope;       // gradient of the foam height per world unit, for the bump normal
    float  rim;         // a soft ring just outside the foam and in its holes, where it shades the water
};

// Foam texture over `amount` (0..1): three lace layers dissolved by the amount. p = advected world xz,
// footprint = world units per pixel.
WaterFoamSample WaterFoamCover( float2 p, float amount, float time, float footprint, bool isOcean )
{
    WaterFoamSample foam = (WaterFoamSample)0;
    if ( amount <= 0.001f ) return foam;

    // Each layer fades to its mean once its cells shrink to a few pixels, so distant foam doesn't shimmer
    const float warpCell = 140.0f;
    const float coarseCell = 90.0f;
    const float mediumCell = 30.0f;
    const float fineCell = 11.0f;
    float coarseDetail = 1.0f - smoothstep( 0.10f, 0.25f, footprint / coarseCell );
    float mediumDetail = 1.0f - smoothstep( 0.10f, 0.25f, footprint / mediumCell );
    float fineDetail = 1.0f - smoothstep( 0.10f, 0.25f, footprint / fineCell );

    float coarse = 0.4f;
    float medium = 0.4f;
    float fine = 0.4f;
    float2 coarseGrad = float2( 0.0f, 0.0f );
    float2 mediumGrad = float2( 0.0f, 0.0f );
    float2 fineGrad = float2( 0.0f, 0.0f );
    [branch] if ( coarseDetail > 0.001f )
    {
        float2 warp = float2( WaterFoamNoise( p / warpCell + 11.3f ), WaterFoamNoise( p / warpCell + 31.7f ) ) * 0.8f;
        float width = lerp( 0.22f, 0.52f, WaterFoamNoise( p / 300.0f - 7.1f ) );
        coarse = lerp( 0.4f, WaterFoamLace( p / coarseCell + warp, time * 0.35f, width, coarseGrad ), coarseDetail );
        coarseGrad *= coarseDetail / coarseCell;
    }
    [branch] if ( mediumDetail > 0.001f )
    {
        medium = lerp( 0.4f, WaterFoamLace( p / mediumCell + 17.3f, time * 0.7f, 0.40f, mediumGrad ), mediumDetail );
        mediumGrad *= mediumDetail / mediumCell;
    }
    [branch] if ( fineDetail > 0.001f )
    {
        fine = lerp( 0.4f, WaterFoamLace( p / fineCell - 41.9f, time * 1.3f, 0.45f, fineGrad ), fineDetail );
        fineGrad *= fineDetail / fineCell;
    }
    // Three layers average toward the mean; the contrast keeps the dissolve's upper thresholds reachable
    const float contrast = 1.15f;
    float pattern = saturate( ( coarse * 0.5f + medium * 0.32f + fine * 0.18f - 0.4f ) * contrast + 0.4f ) * 0.85f + 0.15f;
    float2 patternGrad = ( coarseGrad * 0.5f + mediumGrad * 0.32f + fineGrad * 0.18f ) * ( contrast * 0.85f );
    float detail = coarseDetail * 0.5f + mediumDetail * 0.32f + fineDetail * 0.18f;

    // Dissolve: dense foam closes the holes, thin foam keeps only the lace ridges. The edge widens as detail is
    // lost, so far foam becomes its average cover instead of a hard-edged solid band.
    float threshold = 1.0f - amount * 0.95f;
    float softness = lerp( 0.45f, 0.07f, detail );
    float cover = smoothstep( threshold - softness, threshold + softness, pattern );
    foam.opacity = cover * lerp( 0.72f, 0.92f, amount ) * lerp( 0.60f, 1.0f, detail ) * ( isOcean ? 1.0f : 0.75f );
    foam.thickness = saturate( ( pattern - threshold ) * 3.0f );

    // Height: the cover with rounded shoulders so blobs bulge, plus the lace ridges as bumps on top
    float shoulder = max( softness * 2.5f, 0.16f );
    float hu = saturate( ( pattern - threshold + shoulder ) / ( 2.0f * shoulder ) );
    foam.slope = patternGrad * ( 6.0f * hu * ( 1.0f - hu ) / ( 2.0f * shoulder ) + 0.8f * cover );
    foam.rim = smoothstep( threshold - softness - 0.25f, threshold - softness, pattern ) * ( 1.0f - cover ) * detail * amount;
    return foam;
}

// Foam amount without a shoreline field: bands that run up the shore, from the depth under the pixel alone.
// isolated = a shallow spot in deep water, which keeps only its waterline.
float WaterBandFoamAmount( float2 posXZ, float depth, float time, bool isOcean, float isolated )
{
    float lineDepth    = isOcean ? 9.0f : 5.0f;
    float residueDepth = isOcean ? 32.0f : 15.0f;
    float bandDepth    = isOcean ? 66.0f : 26.0f;
    float bandSpacing  = isOcean ? 85.0f : 32.0f;    // depth between two swash bands
    float bandRate     = isOcean ? 0.14f : 0.22f;    // bands per second

    // Where along the shore foam gathers; also staggers the bands so they don't run as contour rings
    float patches = WaterFoamNoise( posXZ / 520.0f + time * 0.012f );
    float stagger = WaterFoamNoise( posXZ / 1700.0f - time * 0.004f ) * 1.6f;
    float clumps = WaterFoamNoise( posXZ / 140.0f + float2( time * 0.020f, -time * 0.015f ) );

    float waterline = ( 1.0f - smoothstep( 0.0f, lineDepth, depth ) ) * 0.92f;
    float residue = ( 1.0f - smoothstep( 0.0f, residueDepth, depth ) ) * lerp( 0.25f, 0.58f, patches );
    // Phase grows with depth, so a band moves shoreward: sharp front on the shore side, trail behind it
    float phase = frac( depth / bandSpacing + time * bandRate + stagger );
    float band = smoothstep( 0.0f, 0.05f, phase ) * ( 1.0f - smoothstep( 0.05f, 0.42f, phase ) );
    band *= ( 1.0f - smoothstep( bandDepth * 0.45f, bandDepth, depth ) ) * lerp( 0.40f, 0.80f, patches );
    float swash = ( residue + band ) * lerp( 0.65f, 1.25f, clumps ) * ( 1.0f - isolated * 0.75f );
    return saturate( max( waterline, swash ) );
}

//--------------------------------------------------------------------------------------
// Surf from the baked shoreline field (ShoreField.cpp): waves run in along the shore normal and break in the shallows
//--------------------------------------------------------------------------------------
static const float WATER_SHORE_FAR = 30000.0f;     // what WaterShoreField returns away from any shore
static const float WATER_SHORE_NO_FLOOR = 5000.0f;

struct WaterShore
{
    float  distance;    // to the nearest shoreline in world units, negative under the beach
    float  depth;       // baked water depth
    float2 seaward;     // unit direction away from the shore
    float  coherence;   // 0 where two shores meet and the direction is ambiguous
};

WaterShore WaterReadShore( float2 xz )
{
    float4 field = WaterShoreField( xz );
    WaterShore shore;
    shore.distance = field.x;
    shore.depth = field.y;
    float len = length( field.zw );
    shore.seaward = len > 0.0001f ? field.zw / len : float2( 0.0f, 0.0f );
    shore.coherence = saturate( len * 1.25f );
    return shore;
}

struct WaterSurf
{
    float  cycle;        // 0 as a crest passes, rising to 1 just before the next one
    float  age;          // seconds since the crest passed
    float  strength;     // this wave's size
    float  breaking;     // 1 in the surf zone, shoreward of where this wave breaks
    float  nearBreak;    // 1 just seaward of the break point, where the crest steepens
    float  carried;      // how far the bore has pushed the water shoreward, world units
    float  stretch;      // backwash stretch of the foam along the shore normal
    float2 swellSlope;   // gradient of the swell height, for the surface normal
};

WaterSurf WaterSurfAt( WaterShore shore, float2 xz, float time, bool isOcean )
{
    float wavelength = isOcean ? 900.0f : 260.0f;
    float period = isOcean ? 7.0f : 3.2f;
    // Phase falls toward the shore as time goes on, so crests run in; the stagger keeps them from arriving
    // along the whole coast at once
    float stagger = WaterFoamNoise( xz / 2600.0f + 3.1f ) * 0.9f + WaterFoamNoise( xz / 700.0f - 5.7f ) * 0.18f;
    float phase = shore.distance / wavelength + time / period + stagger;
    float waveIndex = floor( phase );

    WaterSurf surf;
    surf.cycle = phase - waveIndex;
    surf.age = surf.cycle * period;
    float waveSet = 0.5f + 0.5f * sin( waveIndex * 1.3f );   // sets of bigger and smaller waves
    surf.strength = lerp( 0.5f, 1.0f, saturate( WaterFoamHash( int2( (int)waveIndex, 17 ) ).x * 0.6f + waveSet * 0.4f ) );

    float breakDepth = ( isOcean ? 95.0f : 22.0f ) * lerp( 0.75f, 1.15f, surf.strength );
    surf.breaking = 1.0f - smoothstep( breakDepth * 0.8f, breakDepth, shore.depth );
    surf.nearBreak = smoothstep( breakDepth * 1.7f, breakDepth, shore.depth ) * ( 1.0f - surf.breaking );

    // The bore pushes the water up the shore quickly; the backwash drains it slowly and streaks the foam
    float push = smoothstep( 0.0f, 0.28f, surf.cycle ) - smoothstep( 0.28f, 1.0f, surf.cycle );
    surf.carried = ( isOcean ? 110.0f : 25.0f ) * surf.strength * surf.breaking * push * shore.coherence;
    float backwash = smoothstep( 0.30f, 0.70f, surf.cycle ) * ( 1.0f - smoothstep( 0.85f, 1.0f, surf.cycle ) );
    surf.stretch = 1.0f + 1.6f * backwash * surf.breaking * shore.coherence * ( 1.0f - smoothstep( 300.0f, 700.0f, shore.distance ) );

    // Swell: crest at cycle 0, a gentle back and a steep shoreward face. It grows in the shallows and flattens once
    // broken; its set modulation is smooth in the phase, so the slope has no seam where the wave index changes.
    float swellSet = 0.72f + 0.28f * sin( phase * 1.18f + 1.0f );
    float back = saturate( 1.0f - surf.cycle / 0.55f );
    float face = saturate( ( surf.cycle - 0.82f ) / 0.18f );
    float heightPerCycle = -2.0f * back / 0.55f + 2.0f * face / 0.18f;
    float amplitude = isOcean ? 22.0f * swellSet * smoothstep( 700.0f, 160.0f, shore.depth ) * ( 1.0f - 0.65f * surf.breaking ) : 0.0f;
    surf.swellSlope = shore.seaward * ( amplitude * heightPerCycle / wavelength * shore.coherence );
    return surf;
}

// Foam laid down by the surf: the breaking bore and its trail, a cap on crests about to break, the swash line,
// leftovers in the shallows and streaks drifting outside the surf zone. aeration = how milky the water turns.
float WaterSurfFoamAmount( WaterShore shore, WaterSurf surf, float localDepth, float2 xz, float time, bool isOcean,
                           float isolated, out float aeration )
{
    float front = smoothstep( 0.0f, 0.015f, surf.cycle );   // the bore's sharp shoreward edge
    float bore = front * exp( -surf.age / 0.9f ) * surf.breaking * surf.strength;
    float trail = front * exp( -surf.age / ( isOcean ? 3.2f : 1.4f ) ) * surf.breaking * surf.strength * 0.62f;
    float cap = smoothstep( 0.94f, 1.0f, surf.cycle ) * surf.nearBreak * surf.strength * 0.55f;

    float patches = WaterFoamNoise( xz / 520.0f + time * 0.012f );
    float clumps = WaterFoamNoise( xz / 140.0f + float2( time * 0.020f, -time * 0.015f ) );
    // The swash line swells as each bore runs up the beach
    float swash = exp( -surf.age / 1.6f ) * surf.strength * surf.breaking;
    float lineDepth = ( isOcean ? 9.0f : 5.0f ) * ( 1.0f + 1.3f * swash );
    float waterline = ( 1.0f - smoothstep( 0.0f, lineDepth, localDepth ) ) * 0.92f;
    float residue = ( 1.0f - smoothstep( 0.0f, isOcean ? 32.0f : 15.0f, localDepth ) ) * lerp( 0.2f, 0.5f, patches )
                  * ( 1.0f - isolated * 0.75f );
    // Old foam drifting outside the surf zone, in loose lines along the shore
    float drift = 0.0f;
    [branch] if ( isOcean && shore.distance < 2600.0f )
    {
        float2 lines = xz + shore.seaward * ( shore.distance * 2.0f );   // squeezed across the shore
        drift = saturate( ( WaterFoamNoise( lines / 380.0f + float2( time * 0.01f, 0.0f ) ) - 0.58f ) * 3.0f )
              * 0.30f * smoothstep( 2600.0f, 600.0f, shore.distance ) * ( 1.0f - surf.breaking ) * shore.coherence;
    }

    aeration = saturate( bore * 1.2f + trail * 0.6f );
    float loose = ( trail + residue + cap + drift ) * lerp( 0.65f, 1.25f, clumps );
    return saturate( max( waterline, bore ) + loose );
}

// Debug paint of the field: depth blue to cyan, a white line every 5 m of shore distance, red where the field
// says land, yellow surf zone, the seaward direction as red/green in the shallows
float3 WaterShoreFieldDebug( WaterShore shore, WaterSurf surf )
{
    float3 c = lerp( float3( 0.02f, 0.05f, 0.25f ), float3( 0.10f, 0.80f, 1.00f ), saturate( 1.0f - shore.depth / 400.0f ) );
    c = lerp( c, float3( 0.5f + 0.5f * shore.seaward.x, 0.5f + 0.5f * shore.seaward.y, 0.2f ),
              0.6f * shore.coherence * ( 1.0f - smoothstep( 600.0f, 900.0f, shore.distance ) ) );
    c = lerp( c, float3( 1.0f, 0.9f, 0.1f ), surf.breaking * 0.35f );
    c = lerp( c, float3( 1.0f, 0.15f, 0.1f ), step( shore.distance, 0.0f ) );
    c += smoothstep( 0.93f, 1.0f, frac( shore.distance / 500.0f ) ) * 0.6f;
    return c;
}

// Bumped foam under a light, relative to flat foam: the bumps show without changing its overall brightness
float WaterFoamRelief( float3 n, float3 lightDir )
{
    float bumped = saturate( ( dot( n, lightDir ) + 0.6f ) / 1.6f );
    float flat = max( ( lightDir.y + 0.6f ) / 1.6f, 0.05f );
    return clamp( bumped / flat, 0.5f, 1.5f );
}

// Foam is lit like the water texture by day; at night moonlight, or the shore's own light (torches, fires)
float3 WaterFoamColor( float3 worldPos, float3 sceneClean, float night, float sunVisibility, float rain, float moonGlint )
{
    float3 sunTint = lerp( float3( 1.0f, 0.80f, 0.62f ), float3( 1.0f, 1.0f, 1.0f ), saturate( AC_LightPos.y * 3.0f ) );
    float3 day = WaterScatterGround( worldPos, WATER_FOAM_ALBEDO * sunTint * lerp( 0.58f, 1.0f, sunVisibility ) );
    day = lerp( day, WaterLuma( day ).xxx * 0.85f, rain * 0.5f );
    float3 moon = WATER_FOAM_ALBEDO * ( float3( 0.035f, 0.045f, 0.07f ) + WATER_MOON_GLINT_COLOR * 0.08f * moonGlint );
    return lerp( day, max( moon, sceneClean * 1.35f ), night );
}

//--------------------------------------------------------------------------------------
// The water pixel
//--------------------------------------------------------------------------------------
struct WaterPixel
{
    float2 screenUV;
    float2 texcoord;          // material UV, already scrolled
    float  surfaceViewZ;
    float  surfaceViewDistance;
    float3 worldPos;
    float3 geometricNormal;   // world space
};

struct WaterFrame
{
    float3 cameraPos;
    float  time;              // seconds
    float2 viewportSize;
    float  cameraBelow;       // 1 while the camera is under the surface
    float  isOcean;           // per texture batch
    float  oceanClimate;
    float3 oceanTint;
    float  oceanTintStrength;
    float  oceanTexture;      // 0 = pure water body, 1 = legacy-strength texture blend
    float3 moonDir;           // world space, toward the moon
    float  moonGlint;         // moon reflection strength (night, fog, rain)
    float  moonDisc;          // how visible the moon disc is in the sky
    float  skyReflection;     // 1 = screen-space sky march, 0 = geometry hits + cube only
    float  shoreFoam;         // 0 = off, 1 = ocean only, 2 = all water
    float  shoreFieldState;   // 0 = no shoreline field, 1 = field, 2 = field painted for debugging
};

float3 ShadeWater( WaterPixel px, WaterFrame fr )
{
    bool isOcean = fr.isOcean > 0.5f;
    float cameraBelow = fr.cameraBelow;
    float topSide = 1.0f - cameraBelow;
    float ssrOn = WaterSSREnabled() ? 1.0f : 0.0f;
    float rain = saturate( AC_RainFXWeight );
    float night = saturate( ( -AC_LightPos.y + 0.12f ) * 2.2f );
    float sunVisibility = smoothstep( -0.04f, 0.08f, AC_LightPos.y ) * ( 1.0f - rain );

    // Depth of whatever the opaque scene put behind this water pixel
    float rawCenterDepth = WaterSceneRawDepth( px.screenUV );
    float depth = WaterLinearDepth( rawCenterDepth );
    float shallowDepth = saturate( ( depth - px.surfaceViewZ ) * 0.01f );
    float3 viewDirection = normalize( px.worldPos - fr.cameraPos );
    float footprint = max( fwidth( px.worldPos.x ), fwidth( px.worldPos.z ) );   // world units per pixel, for the foam

    // Two-octave distortion
    float2 worldTexCoord = px.worldPos.xz / 1000.0f;
    float3 distortionSmall = WaterDistortion( worldTexCoord * DIST_SMALL_SCALE + fr.time * DIST_SMALL_SPEED ) * 2 - 1;
    distortionSmall += WaterDistortion( worldTexCoord * float2( -1, 0.7 ) * DIST_SMALL_SCALE + fr.time * DIST_SMALL_SPEED * 2 ) * 2 - 1;
    distortionSmall *= 0.5f;
    float3 distortionBig = WaterBigDistortion( worldTexCoord, fr.time );

    float2 distUV = px.screenUV + distortionSmall.xy * DIST_SMALL_AMOUNT + distortionBig.xy * DIST_SMALL_AMOUNT;
    float3 diffuse = WaterDiffuse( px.texcoord + distortionSmall.xy * DIST_SMALL_AMOUNT * 0.5f );

    // Refraction, pulled back to the undistorted UV where the distorted texel lies in front of the water
    float depthRefracted = WaterLinearDepth( WaterSceneRawDepth( distUV ) );
    distUV = saturate( lerp( distUV, px.screenUV, saturate( px.surfaceViewZ - depthRefracted ) ) );
    float rawDepthRefracted = WaterSceneRawDepth( distUV );
    depthRefracted = WaterLinearDepth( rawDepthRefracted );
    float refractedValid = step( 0.000001f, rawDepthRefracted );

    float3 wavesFres = WaterWaveNormal( distortionBig );
    float3 wavesSmall = normalize( distortionSmall.xzy * float3( 1, 10, 1 ) );

    // Shore surf from the baked field; its swell tilts the surface before anything reflects off it
    bool foamOn = fr.shoreFoam > 1.5f || ( fr.shoreFoam > 0.5f && isOcean );
    bool fieldOn = fr.shoreFieldState > 0.5f;
    WaterShore coast = (WaterShore)0;
    coast.distance = WATER_SHORE_FAR;
    coast.depth = WATER_SHORE_NO_FLOOR;
    WaterSurf surf = (WaterSurf)0;
    surf.stretch = 1.0f;
    [branch] if ( fieldOn && ( foamOn || fr.shoreFieldState > 1.5f ) && topSide > 0.5f )
    {
        coast = WaterReadShore( px.worldPos.xz );
        surf = WaterSurfAt( coast, px.worldPos.xz, fr.time, isOcean );
        float3 swell = float3( -surf.swellSlope.x, 0.0f, -surf.swellSlope.y ) * ( foamOn ? WaterFlatness( px.geometricNormal ) : 0.0f );
        wavesFres = normalize( wavesFres / max( wavesFres.y, 0.2f ) + swell );
        wavesSmall = normalize( wavesSmall / max( wavesSmall.y, 0.2f ) + swell );
    }

    float3 scene = WaterSceneColor( distUV );
    float3 sceneClean = WaterSceneColor( lerp( distUV, px.screenUV, pow( 1 - shallowDepth, 20.0f ) ) );

    float NdotV = saturate( dot( -viewDirection, wavesFres ) );
    float legacyFresnel = min( 0.5f, saturate( pow( 1.0f - NdotV, 10.0f ) ) );
    float schlickFresnel = 0.02f + 0.98f * pow( 1.0f - NdotV, 5.0f );
    float reflectFresnel = kPow3( 1.0f - NdotV );
    // Reflections fade only once the (wave-smoothed) reflected ray dips below the horizon, so distant water keeps them
    float3 skyDir = reflect( viewDirection, normalize( lerp( wavesFres, WATER_UP, 0.46f ) ) );
    float hemi = smoothstep( -0.04f, 0.02f, skyDir.y ) * topSide;

    // Waterfalls: near-vertical sheets get neither reflections nor a shoreline
    float flatness = WaterFlatness( px.geometricNormal );
    float waterfallMask = 1.0f - flatness;
    float reflectionSuppress = lerp( 0.12f, 1.0f, flatness );
    float ssrStrength = ssrOn * lerp( 0.45f, 1.0f, flatness );
    float cubeStrength = lerp( 0.34f, 0.82f, ssrOn ) * topSide * lerp( 0.70f, 1.0f, flatness );

    // Reflection sources
    float3 cube = WaterCube( reflect( -viewDirection, wavesFres ) );
    float3 fallback = WaterReflectionFallback( cube, rain, night, isOcean );

    float hitConfidence = 0.0f;
    float hitDistance = 0.0f;
    float3 hitColor = float3( 0.0f, 0.0f, 0.0f );
    float3 hitDir = WaterGeometryReflectionDir( viewDirection, wavesFres, px.surfaceViewDistance );
    [branch] if ( ssrOn > 0.5f )
        hitColor = WaterTraceSSR( px.worldPos, hitDir, hitConfidence, hitDistance );
    hitConfidence = saturate( hitConfidence );

    float2 skyUV = px.screenUV;
    float skyValid = 0.0f;
    [branch] if ( ssrOn > 0.5f && fr.skyReflection > 0.5f && topSide > 0.5f && skyDir.y > 0.0001f )
        skyValid = WaterSkyMarch( px.worldPos, skyDir, skyUV );
    // Where the march misses (looking down, rays leaving the screen): the averaged on-screen sky, else the cube
    float4 skyAverage = WaterSkyAverage();
    float3 skyFill = lerp( fallback, max( skyAverage.rgb, 0.0f ), saturate( skyAverage.a ) * step( 0.5f, fr.skyReflection ) );
    float3 skyReflection = skyFill;
    [branch] if ( skyValid > 0.5f )
    {
        skyReflection = WaterSkyWithoutCelestialBodies( skyUV, skyDir, skyFill, fr.viewportSize, sunVisibility, fr.moonDir, fr.moonDisc );
        float4 clouds = ResolveWaterLowClouds( WaterLowClouds( skyUV ), skyReflection );
        skyReflection = max( skyReflection + ( skyReflection * ( 1.0f - clouds.a ) + clouds.rgb - skyReflection )
                                           * lerp( 1.12f, 1.30f, saturate( clouds.a ) ), 0.0f );
    }
    float2 skyEdge = saturate( abs( skyUV - 0.5f ) * 2.0f );
    float skyMarchWeight = skyValid * ( 1.0f - smoothstep( 0.78f, 1.0f, max( skyEdge.x, skyEdge.y ) ) );
    skyReflection = lerp( skyFill, skyReflection, skyMarchWeight );
    float skyWeight = ssrOn * hemi;
    float skyConfidence = saturate( skyWeight * lerp( 0.90f, 0.80f, rain ) );

    float3 processedReflection = float3( 0.0f, 0.0f, 0.0f );
    float rainVisibility = 1.0f;
    if ( hitConfidence > 0.0f )
    {
        float3 hitWorld = px.worldPos + hitDir * hitDistance;
        processedReflection = lerp( hitColor, WaterScatterGround( hitWorld, max( hitColor, 0.0f ) ), rain );
        float rainFog = rain * smoothstep( 5000.0f, 22000.0f, length( hitWorld - fr.cameraPos ) );
        rainVisibility = ( 1.0f - rainFog ) * ( 1.0f - rainFog );
        processedReflection = ProcessWaterHitReflection( processedReflection, night );
    }

    // At low sun the static cube shows the wrong sky at grazing angles: dim the unconfirmed sources
    float lowLight = saturate( max( night, saturate( ( 0.28f - AC_LightPos.y ) * 2.8f ) ) );
    float grazingFallback = saturate( ssrOn * lowLight * smoothstep( 0.12f, 0.78f, reflectFresnel ) );
    float nightCubeDim = lerp( 1.0f, 0.14f, grazingFallback );
    float geometryAvailable = saturate( hitConfidence * reflectionSuppress * ssrOn );
    float glintBlock = saturate( geometryAvailable * 1.45f );
    float cubeOnlyAmount = saturate( lerp( 0.35f, 1.0f, reflectFresnel ) * 0.5f * reflectFresnel ) * reflectionSuppress;
    float3 cubeOnlyColor = cube * lerp( 1.0f, diffuse, 0.6f );

    // Shoreline: water thickness along the view ray, and the vertical depth below this pixel
    float column = WaterColumnLength( depthRefracted, px.surfaceViewZ, px.surfaceViewDistance );
    float straightColumn = WaterColumnLength( depth, px.surfaceViewZ, px.surfaceViewDistance );
    float colorColumn = min( column, straightColumn );
    float columnDeriv = fwidth( column );
    float colorColumnDeriv = fwidth( colorColumn );
    float rayFloorDepth = WaterDepthBelowSurface( px.worldPos, px.surfaceViewZ, rawCenterDepth, fr.cameraPos );
    float shoreException = step( WATER_DEEP_WATER_DEPTH, rayFloorDepth );
    [branch] if ( shoreException < 0.5f && cameraBelow < 0.5f )
        shoreException = WaterShoreProbeException( px.worldPos, fr.cameraPos );
    float isolatedShallow = shoreException * ( 1.0f - step( WATER_DEEP_WATER_DEPTH, rayFloorDepth ) );   // reef, swimmer
    float2 shore = isOcean ? OceanShore( column, columnDeriv ) : LegacyShore( column, columnDeriv, colorColumn, colorColumnDeriv );
    shore = lerp( shore, 1.0f, max( max( shoreException, waterfallMask ), cameraBelow ) );

    float3 smallReflect = reflect( -viewDirection, wavesSmall );
    float sunSpot = pow( saturate( dot( smallReflect, -AC_LightPos.xyz ) ), 500.0f ) * 0.5f
        * sunVisibility * ( 1.0f - glintBlock ) * shore.x;
    float3 sunGlint = lerp( float3( 1.2f, 0.6f, 0.2f ), float3( 5.0f, 5.0f, 5.0f ), saturate( AC_LightPos.y ) ) * sunSpot;

    float3 color;
    [branch] if ( !isOcean )
    {
        // Water body; sky behind the surface (depth 0) has no floor to absorb against
        if ( refractedValid > 0.5f && cameraBelow < 0.5f )
            scene = ApplyWaterVolume( scene, column, WaterScatterGround( px.worldPos, WATER_VOLUME_SCATTER ) );

        float3 litDiffuse = WaterScatterGround( px.worldPos, diffuse );
        scene = lerp( scene, litDiffuse, 0.73f * max( pow( legacyFresnel, 8.0f ), 0.5f ) );
        color = lerp( scene, sceneClean, kPow4( saturate( px.surfaceViewDistance / 35000.0f ) ) );
        color = lerp( color, WaterSceneHue( color, sceneClean ), refractedValid * flatness * 0.42f );
        float3 nightColor = lerp( color, color * saturate( sceneClean * 1.10f + 0.34f ), night );
        color = lerp( sceneClean, nightColor, shore.y );
        color += sunGlint;

        // Screen-space sources where confident, the scene-limited cube structure elsewhere
        float stableGeometry = smoothstep( 0.22f, 0.72f, geometryAvailable );
        float skyAvailable = ( 1.0f - stableGeometry ) * step( 0.0001f, skyWeight ) * ssrOn;
        float cubeAvailable = 1.0f - saturate( stableGeometry + skyAvailable );
        float baseLuma = max( WaterLuma( color ), 0.0001f );

        float3 limitedCube = WaterLimitLuma( fallback, baseLuma * lerp( 1.30f, 1.12f, night ) + lerp( 0.015f, 0.004f, night ) );
        float3 cubeStructure = lerp( WaterLuma( limitedCube ).xxx, limitedCube, 0.38f ) * nightCubeDim;
        float3 hybridFallback = lerp( color, cubeStructure, lerp( 0.28f, 0.72f, 1.0f - ssrOn ) );
        float3 sceneReflection = processedReflection * stableGeometry + skyReflection * skyAvailable + hybridFallback * cubeAvailable;
        float definition = stableGeometry * lerp( 0.10f, 0.06f, night );
        sceneReflection = max( lerp( WaterLuma( sceneReflection ).xxx, sceneReflection, 1.0f + definition ), 0.0f );

        float ssrFresnel = lerp( 0.55f, 0.80f, saturate( kPow2( 1.0f - NdotV ) ) );
        // Marched sky at full strength; the fill only by Fresnel, so steep views keep showing the lake bed
        float coverage = max( saturate( lerp( schlickFresnel, 1.0f, skyMarchWeight ) * skyWeight ), stableGeometry );
        float ssrBlend = saturate( coverage * ssrFresnel * ssrStrength * 0.86f * lerp( 0.93f, 1.08f, night ) * rainVisibility * reflectionSuppress );
        float3 ssrResult = lerp( color, sceneReflection, ssrBlend * shore.x );

        // Without SSR: the static cube, held near the water's own brightness at night
        float3 nightCube = WaterLimitLuma( cubeOnlyColor, baseLuma * 1.15f + 0.003f );
        nightCube = lerp( WaterLuma( nightCube ).xxx, nightCube, 0.28f );
        float3 cubeResult = lerp( color, lerp( cubeOnlyColor, nightCube, night ),
            cubeOnlyAmount * lerp( 1.0f, 0.55f, night ) * shore.x * hemi );

        color = lerp( cubeResult, ssrResult, ssrOn );
        color = lerp( color, WaterSceneHue( color, sceneClean ), refractedValid * waterfallMask * 0.14f );
    }
    else
    {
        // Ocean: the water body replaces the texture; night and rain use their own scatter colors
        float underThick = clamp( abs( depthRefracted - px.surfaceViewZ ) * 0.35f, 0.0f, 1400.0f );
        float3 absorption, scatter;
        GetOceanOptics( fr.oceanClimate, rain, night, cameraBelow, WaterLuma( diffuse ), absorption, scatter );
        float3 transmittance = exp( -absorption * lerp( column, underThick, cameraBelow ) );
        float3 volume = scene * transmittance + scatter * ( 1.0f - transmittance );
        volume = lerp( scatter, volume, saturate( refractedValid + cameraBelow ) );
        volume = lerp( sceneClean, volume, shore.y );
        // The material texture over the body, as legacy water does; dimmed toward the scene at night like there
        float3 oceanTexture = WaterScatterGround( px.worldPos, diffuse ) * lerp( 1.0f, saturate( sceneClean * 1.10f + 0.34f ), night );
        volume = lerp( volume, oceanTexture, saturate( fr.oceanTexture ) * 0.365f * shore.y * topSide );
        // From below: sky through the surface stays clear, geometry above water only partly tinted
        volume = lerp( volume, scene, cameraBelow * ( 1.0f - refractedValid ) );
        volume = lerp( volume, lerp( scene, volume, 0.32f ), cameraBelow * refractedValid );

        float stableGeometry = saturate( smoothstep( 0.22f, 0.72f, hitConfidence ) * reflectionSuppress * ssrOn );
        float skyAvailable = ( 1.0f - stableGeometry ) * step( 0.0001f, skyWeight ) * ssrOn;
        float cubeAvailable = 1.0f - saturate( stableGeometry + skyAvailable );
        float total = lerp( cubeStrength, ssrStrength, saturate( stableGeometry + skyAvailable ) );

        float baseLuma = max( WaterLuma( volume ), 0.0001f );
        float3 limitedCube = WaterLimitLuma( fallback, baseLuma * lerp( 1.55f, 1.20f, night ) + lerp( 0.025f, 0.006f, night ) );
        float3 preparedCube = lerp( WaterLuma( limitedCube ).xxx, limitedCube, lerp( 0.38f, 0.58f, fr.oceanClimate ) ) * nightCubeDim;
        float3 reflection = processedReflection * stableGeometry + skyReflection * nightCubeDim * skyAvailable + preparedCube * cubeAvailable;

        // Day: plain Fresnel with a floor where real sky is reflected; night: the stronger of both drivers
        float skySelected = step( 0.001f, skyWeight ) * ( 1.0f - stableGeometry ) * ssrOn;
        float dayFresnel = lerp( schlickFresnel, max( schlickFresnel, 0.085f ), skySelected );
        float nightFresnel = lerp( schlickFresnel, max( schlickFresnel, 0.120f ), skySelected );
        float driver = max( reflectFresnel, skySelected * ( 1.0f - stableGeometry ) * 0.18f );
        float reflectAmount = saturate( lerp( 0.42f, 1.0f, reflectFresnel )
            * lerp( 0.68f, 1.0f, saturate( max( stableGeometry, skyConfidence ) ) ) * driver ) * reflectionSuppress;
        float dayAmount = saturate( dayFresnel * total );
        float nightAmount = saturate( max( max( nightFresnel * total * 0.92f, reflectAmount * total ),
                                           stableGeometry * ssrStrength * 0.78f ) );
        float amount = lerp( dayAmount, nightAmount, night ) * shore.x * hemi * lerp( 0.82f, 1.0f, fr.oceanClimate );
        amount *= lerp( 1.0f, 0.62f, saturate( ( skyAvailable + cubeAvailable ) * grazingFallback ) );
        float3 ssrResult = lerp( volume, reflection, amount );

        // Without SSR: the static cube, held near the water's own brightness at night
        float3 nightCube = WaterLimitLuma( cubeOnlyColor, baseLuma * 1.15f + 0.003f );
        nightCube = lerp( WaterLuma( nightCube ).xxx, nightCube, 0.28f );
        float3 cubeResult = lerp( volume, lerp( cubeOnlyColor, nightCube, night ),
            cubeOnlyAmount * lerp( 1.0f, 0.55f, night ) * shore.x * hemi );

        color = lerp( cubeResult, ssrResult, ssrOn );
        float fallbackGlintDim = lerp( 1.0f, 0.25f, saturate( ( 1.0f - stableGeometry ) * grazingFallback ) );
        color += sunGlint * max( cubeStrength, ssrStrength ) * fallbackGlintDim * lerp( 0.82f, 1.0f, fr.oceanClimate );

        // Regional tint, luma-neutral; rain and night bring their own colors
        float tintStrength = saturate( fr.oceanTintStrength ) * lerp( 1.0f, 0.35f, rain ) * lerp( 1.0f, 0.20f, night ) * shore.y;
        color = lerp( color, color * fr.oceanTint, tintStrength );
    }

    color += WaterMoonGlint( viewDirection, wavesSmall, wavesFres, fr.moonDir ) * fr.moonGlint * shore.x * flatness * topSide;

    // Shore foam goes over everything: it is rough and opaque, so it hides reflections and glints
    float foamFade = ( foamOn ? 1.0f : 0.0f ) * topSide * flatness
        * ( 1.0f - smoothstep( WATER_FOAM_FADE_DISTANCE * 0.55f, WATER_FOAM_FADE_DISTANCE, px.surfaceViewDistance ) );
    float foamMaxDepth = isOcean ? 80.0f : 32.0f;
    // The plumb walk is skipped over open sea, or where the ray's floor is too deep for any slope under ~56 degrees
    float rayFloorRun = straightColumn * length( viewDirection.xz );
    bool plumbUseful = straightColumn < 5900.0f && rayFloorDepth - 1.5f * rayFloorRun < foamMaxDepth;
    bool nearShore = fieldOn ? ( coast.distance < ( isOcean ? 2600.0f : 600.0f ) || coast.depth < foamMaxDepth * 2.0f ) : plumbUseful;
    [branch] if ( foamFade > 0.001f && nearShore )
    {
        // Depth under the pixel: the plumb line finds the waterline exactly where it sees the floor. Otherwise the
        // view ray's floor (exact looking straight down; steep banks have no shallows, so its run stands in) or
        // the baked depth.
        float plumb = -1.0f;
        [branch] if ( plumbUseful )
            plumb = WaterPlumbDepth( px.worldPos, fr.cameraPos, foamMaxDepth, viewDirection.y, footprint );
        float localDepth = min( plumb >= 0.0f ? plumb : rayFloorDepth, straightColumn );
        if ( fieldOn && plumb < 0.0f ) localDepth = min( localDepth, coast.depth );

        float2 foamXZ = px.worldPos.xz + distortionSmall.xy * 14.0f;
        float amount;
        float aeration = 0.0f;
        [branch] if ( fieldOn )
        {
            amount = WaterSurfFoamAmount( coast, surf, localDepth, px.worldPos.xz, fr.time, isOcean, isolatedShallow, aeration );
            // The bore carries the foam up the shore; the backwash stretches it along the shore normal
            foamXZ += coast.seaward * ( surf.carried - coast.distance * ( 1.0f - 1.0f / surf.stretch ) );
        }
        else
        {
            amount = WaterBandFoamAmount( px.worldPos.xz, localDepth, fr.time, isOcean, isolatedShallow );
        }
        WaterFoamSample foam = WaterFoamCover( foamXZ, amount, fr.time, footprint, isOcean );

        float3 foamNormal = normalize( float3( -foam.slope.x * WATER_FOAM_BUMP, 1.0f, -foam.slope.y * WATER_FOAM_BUMP ) );
        float3 sunDir = normalize( AC_LightPos.xyz );
        float relief = lerp( WaterFoamRelief( foamNormal, sunDir ), WaterFoamRelief( foamNormal, fr.moonDir ), night );
        relief = lerp( 1.0f, relief, lerp( 0.9f, 0.35f, rain ) );   // overcast light is flat
        // Thin foam lets the water hue through and sits in the shade of the thick foam around it
        float3 waterHue = min( color / max( WaterLuma( color ), 0.02f ), 2.0f );
        float3 body = lerp( lerp( 1.0f, waterHue, 0.35f ) * 0.80f, 1.0f, foam.thickness );
        float sparkle = pow( saturate( dot( reflect( viewDirection, foamNormal ), sunDir ) ), 60.0f )
                      * 0.20f * sunVisibility * foam.thickness;
        float3 foamLight = WaterFoamColor( px.worldPos, sceneClean, night, sunVisibility, rain, fr.moonGlint );
        float3 foamColor = foamLight * relief * body + sparkle;

        // Water churned up behind the bores turns milky before any foam sits on it
        float3 milky = lerp( color, foamLight * lerp( 1.0f, waterHue, 0.5f ) * 0.8f, 0.5f );
        color = lerp( color, milky, aeration * 0.4f * foamFade );
        color *= 1.0f - foam.rim * 0.14f * foamFade;
        color = lerp( color, foamColor, foam.opacity * foamFade );
    }

    [branch] if ( fr.shoreFieldState > 1.5f && topSide > 0.5f )
        color = WaterShoreFieldDebug( coast, surf );
    return max( color, 0.0f );
}

#endif
