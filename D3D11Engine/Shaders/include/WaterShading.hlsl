#ifndef WATER_SHADING_HLSL
#define WATER_SHADING_HLSL
// Water shading shared by PS_Water (D3D11, gamma space) and D3D12/Water.hlsl; model and constants from
// MarcoMarwin's GD3D11 fork. Include after defining WaterSceneRawDepth, WaterSurfaceRawDepth,
// WaterLinearDepth, WaterWorldToView and WaterViewToUV.

static const float3 WATER_LUMA = float3( 0.2126f, 0.7152f, 0.0722f );

float WaterSmootherStep01( float t )
{
    t = saturate( t );
    return t * t * t * ( t * ( t * 6.0f - 15.0f ) + 10.0f );
}

//--------------------------------------------------------------------------------------
// Water body: Beer-Lambert absorption + in-scatter over the refracted water column
//--------------------------------------------------------------------------------------
static const float3 WATER_VOLUME_ABSORPTION = float3( 0.0020f, 0.00115f, 0.00165f ); // per world unit
static const float3 WATER_VOLUME_SCATTER    = float3( 0.050f, 0.080f, 0.060f );      // gamma space

// View-Z gap between surface and scene, stretched to the length along the view ray (1/cos, capped).
float WaterColumnLength( float sceneViewZ, float surfaceViewZ, float surfaceViewDistance )
{
    float viewRayScale = clamp( surfaceViewDistance / max( abs( surfaceViewZ ), 1.0f ), 1.0f, 8.0f );
    return clamp( max( sceneViewZ - surfaceViewZ, 0.0f ) * viewRayScale, 0.0f, 6000.0f );
}

// Fades in over 18..198 units so shorelines stay clear; optical depth soft-saturates at 1400 units.
float3 ApplyWaterVolume( float3 scene, float columnLength, float3 absorption, float3 scatter )
{
    float influence = WaterSmootherStep01( ( columnLength - 18.0f ) / 180.0f );
    float opticalDepth = 1400.0f * ( 1.0f - exp( -columnLength / 1400.0f ) );
    float3 transmittance = exp( -absorption * opticalDepth );
    float3 volume = scene * transmittance + scatter * ( 1.0f - transmittance );
    return lerp( scene, volume, influence );
}

//--------------------------------------------------------------------------------------
// Ocean (NW_WATER_LAKE* textures). climate: 0 = dense coastal water, 1 = clear turquoise (ADDONWORLD)
//--------------------------------------------------------------------------------------
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

// The static reflection cube is a daytime image; at night hold it near the water's own brightness.
float3 LimitOceanCube( float3 cube, float3 waterColor, float night, float climate )
{
    float baseLuma = max( dot( waterColor, WATER_LUMA ), 0.0001f );
    float cubeLuma = max( dot( cube, WATER_LUMA ), 0.0001f );
    float limit = baseLuma * lerp( 1.55f, 1.20f, night ) + lerp( 0.025f, 0.006f, night );
    float3 limited = cube * min( 1.0f, limit / cubeLuma );
    limited = lerp( dot( limited, WATER_LUMA ).xxx, limited, lerp( 0.38f, 0.58f, climate ) );
    return lerp( cube, limited, night );
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
    float3 sceneChroma = sceneClean / max( dot( sceneClean, WATER_LUMA ), 0.001f );
    float3 hued = sceneChroma * max( dot( color, WATER_LUMA ), 0.001f );
    float3 limit = max( abs( color ) * 0.35f, 0.005f );
    return clamp( hued, color - limit, color + limit );
}

//--------------------------------------------------------------------------------------
// Moon on the water: sparkles on the small waves plus a broad sheen that forms the moon path
//--------------------------------------------------------------------------------------
static const float3 WATER_MOON_GLINT_COLOR = float3( 0.60f, 0.70f, 1.00f );

// Gamma-space intensity; moonDir points toward the moon.
float3 WaterMoonGlint( float3 viewDirection, float3 smallWaveNormal, float3 bigWaveNormal, float3 moonDir )
{
    float sparkle = pow( saturate( dot( reflect( -viewDirection, smallWaveNormal ), -moonDir ) ), 420.0f ) * 0.15f;
    float path = pow( saturate( dot( reflect( -viewDirection, bigWaveNormal ), -moonDir ) ), 48.0f ) * 0.035f;
    return WATER_MOON_GLINT_COLOR * ( sparkle + path );
}

#endif
