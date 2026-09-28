#ifndef LOW_CLOUDS_HLSL
#define LOW_CLOUDS_HLSL
// Low clouds: ray-marched cloud banks above the fog height, from MarcoMarwin's GD3D11 ("dynamic clouds").
// Shared by D3D11 (PS_PFX_LowClouds*) and D3D12 (D3D12/LowClouds.hlsl); colors are gamma space.
// Needs the Atmosphere constants (AC_LightPos, AC_RainFXWeight) included first. The composite also needs
// LowCloudLoadLayer, LowCloudLoadDepth, LowCloudLoadSky and LowCloudLayerSize defined before inclusion.

#ifndef LOW_CLOUD_CB_REGISTER
#define LOW_CLOUD_CB_REGISTER b2
#endif

cbuffer LowCloudCB : register( LOW_CLOUD_CB_REGISTER )
{
    float4x4 LC_InvView;          // view -> world, same convention as SQ_InvView
    float2 LC_InvProj;            // 1 / P._11, 1 / P._22
    float  LC_Time;               // seconds
    float  LC_NightFogBrightness;
    float3 LC_CameraPos;          float LC_FogHeight;
    float3 LC_FogColor;           float LC_FogOverride;
    float3 LC_DayColor;           float LC_Density;
    float3 LC_RainColor;          float LC_Scale;
    float3 LC_NightColor;         float LC_Speed;
    float3 LC_MoonDir;            float LC_HeightScale;
    float  LC_DistanceScale;      float LC_SunLight;     float LC_SunVisibility;  float LC_MoonVisibility;
    float4 LC_SunScreen;          // xy = uv, z = visibility
    float4 LC_MoonScreen;
    float  LC_Frame;              // advances the march jitter per frame under TAA/FSR, 0 otherwise
    float3 LC_Pad;
};

static const float LOW_CLOUD_SKY_DEPTH = 0.00001f;

float LowCloudSmootherStep( float x )
{
    x = saturate( x );
    return x * x * x * ( x * ( x * 6.0f - 15.0f ) + 10.0f );
}

// Reversed-Z infinite projection: sky (depth 0) lands 1e8 units out.
float3 LowCloudWorldPosition( float rawDepth, float2 uv )
{
    float viewZ = rcp( max( rawDepth, 1e-8f ) );
    float2 ndc = uv * float2( 2.0f, -2.0f ) + float2( -1.0f, 1.0f );
    return mul( float4( ndc * LC_InvProj * viewZ, viewZ, 1.0f ), LC_InvView ).xyz;
}

// Interleaved gradient noise: a per-pixel march offset, so neighbouring rays sample different depths
float LowCloudJitter( float2 pixel )
{
    pixel += 5.588238f * LC_Frame;
    return frac( 52.9829189f * frac( dot( pixel, float2( 0.06711056f, 0.00583715f ) ) ) );
}

float LowCloudNightBlend() { return smoothstep( 0.0f, 1.0f, saturate( -AC_LightPos.y * 4.0f ) ); }

float3 LowCloudSunDir() { return normalize( lerp( float3( -0.25f, 0.72f, 0.18f ), AC_LightPos, saturate( abs( AC_LightPos.y ) + 0.12f ) ) ); }
float3 LowCloudMoonDir() { return normalize( lerp( float3( 0.22f, 0.64f, -0.28f ), LC_MoonDir, saturate( abs( LC_MoonDir.y ) + 0.12f ) ) ); }
float LowCloudSunWeight() { return saturate( LC_SunVisibility ) * smoothstep( 0.04f, 0.42f, AC_LightPos.y ); }

//--------------------------------------------------------------------------------------
// Noise and density
//--------------------------------------------------------------------------------------
float LowCloudHash31( float3 p )
{
    p = frac( p * 0.1031f );
    p += dot( p, p.yzx + 33.33f );
    return frac( ( p.x + p.y ) * p.z );
}

float LowCloudValueNoise3( float3 p )
{
    float3 i = floor( p );
    float3 f = frac( p );
    float3 u = f * f * ( 3.0f - 2.0f * f );
    float nx00 = lerp( LowCloudHash31( i ), LowCloudHash31( i + float3( 1, 0, 0 ) ), u.x );
    float nx10 = lerp( LowCloudHash31( i + float3( 0, 1, 0 ) ), LowCloudHash31( i + float3( 1, 1, 0 ) ), u.x );
    float nx01 = lerp( LowCloudHash31( i + float3( 0, 0, 1 ) ), LowCloudHash31( i + float3( 1, 0, 1 ) ), u.x );
    float nx11 = lerp( LowCloudHash31( i + float3( 0, 1, 1 ) ), LowCloudHash31( i + float3( 1, 1, 1 ) ), u.x );
    return lerp( lerp( nx00, nx10, u.y ), lerp( nx01, nx11, u.y ), u.z );
}

float LowCloudFbm3( float3 p )
{
    float n = LowCloudValueNoise3( p ) * 0.62f;
    n += LowCloudValueNoise3( p * 2.03f + 17.11f ) * 0.28f;
    n += LowCloudValueNoise3( p * 4.01f + 61.73f ) * 0.10f;
    return n;
}

// Domain-warped islands of cloud in a band from the fog height up to ~16k units.
float ComputeLowCloudDensity( float3 worldPosition, float skyPixel )
{
    float cloudBase = LC_FogHeight;
    float cloudScale = max( 0.35f, LC_Scale );
    float invCloudScale = 1.0f / cloudScale;
    float heightScale = max( 0.35f, LC_HeightScale );
    float3 wind = float3( LC_Time * 3.8f, LC_Time * 0.04f, -LC_Time * 2.3f ) * max( 0.0f, LC_Speed );
    float3 macroP = ( worldPosition + wind * 52.0f ) * float3( 0.000026f, 0.000032f, 0.000026f ) * invCloudScale;
    float3 warpP = ( worldPosition + wind * 34.0f ) * float3( 0.000040f, 0.000047f, 0.000040f ) * invCloudScale;
    float3 warp = float3(
        LowCloudValueNoise3( warpP + float3( 13.1f, 2.7f, 41.9f ) ),
        LowCloudValueNoise3( warpP + float3( 57.7f, 19.3f, 8.2f ) ),
        LowCloudValueNoise3( warpP + float3( 4.8f, 63.4f, 27.5f ) ) ) * 2.0f - 1.0f;
    float3 warpedWorld = worldPosition + warp * float3( 14800.0f, 3600.0f, 14800.0f ) * cloudScale;

    float macro = LowCloudFbm3( ( warpedWorld + wind * 50.0f ) * float3( 0.000024f, 0.000030f, 0.000024f ) * invCloudScale );
    float body = LowCloudFbm3( ( warpedWorld + wind * 30.0f ) * float3( 0.000104f, 0.000118f, 0.000104f ) * invCloudScale + float3( 19.3f, 4.7f, 71.1f ) );
    float torn = LowCloudFbm3( ( warpedWorld + wind * 16.0f ) * float3( 0.000190f, 0.000170f, 0.000190f ) * invCloudScale + float3( 43.0f, 12.0f, 5.0f ) );
    float topNoise = LowCloudValueNoise3( macroP * 1.08f + float3( 77.0f, 9.0f, 23.0f ) );
    float baseNoise = LowCloudValueNoise3( macroP * 0.82f + float3( 12.0f, 51.0f, 6.0f ) );

    float islands = smoothstep( 0.43f, 0.73f, macro + body * 0.18f );
    float broadGaps = smoothstep( 0.50f, 0.82f, LowCloudValueNoise3( macroP * 1.72f + float3( 31.0f, 7.0f, 91.0f ) ) + torn * 0.12f );
    float bodyCore = smoothstep( 0.38f, 0.75f, body * 0.78f + macro * 0.36f - torn * 0.12f );
    float cloudBody = LowCloudSmootherStep( islands * lerp( bodyCore, bodyCore * 0.30f, broadGaps * 0.68f ) );

    float localBase = cloudBase + ( lerp( 500.0f, 2300.0f, baseNoise ) - broadGaps * 900.0f ) * heightScale;
    float localTop = cloudBase + ( lerp( 8200.0f, 15800.0f, topNoise ) + islands * 2100.0f + cloudBody * 1700.0f - broadGaps * 1900.0f ) * heightScale;
    float lowCenter = cloudBase + ( 3200.0f + ( baseNoise - 0.5f ) * 900.0f ) * heightScale;
    float highCenter = lerp( cloudBase + 6500.0f * heightScale, localTop - 2600.0f * heightScale, saturate( islands * 0.86f + bodyCore * 0.22f ) );

    float lowerCore = 1.0f - smoothstep( 1500.0f * heightScale, 5200.0f * heightScale, abs( worldPosition.y - lowCenter ) );
    float lowerSkirt = 1.0f - smoothstep( 3400.0f * heightScale, 9800.0f * heightScale, abs( worldPosition.y - ( localBase + 2800.0f * heightScale ) ) );
    float highBank = 1.0f - smoothstep( 2800.0f * heightScale, 8700.0f * heightScale, abs( worldPosition.y - highCenter ) );
    float regularBottomFade = smoothstep( localBase - 1200.0f * heightScale, localBase + 2400.0f * heightScale, worldPosition.y );
    float skyBottomFade = smoothstep( localBase - 2600.0f * heightScale, localBase + 4200.0f * heightScale, worldPosition.y );
    float bottomFade = lerp( regularBottomFade, skyBottomFade, saturate( skyPixel ) );
    float topFeather = lerp( 3200.0f, 6200.0f, topNoise ) * heightScale;
    float topFade = 1.0f - smoothstep( localTop - topFeather, localTop + topFeather * 0.85f, worldPosition.y );
    float verticalBand = saturate( lowerCore * 1.14f + lowerSkirt * 0.40f + highBank * 0.50f ) * bottomFade * topFade;

    return saturate( verticalBand * cloudBody * 1.42f * max( 0.0f, LC_Density ) );
}

//--------------------------------------------------------------------------------------
// Ray march: color (not premultiplied) and alpha of the clouds between the camera and endWorld
//--------------------------------------------------------------------------------------
float4 ComputeLowCloudVolume( float3 cameraWorld, float3 endWorld, float cameraDistance, float skyPixel, int requestedSteps, float jitter )
{
    float3 ray = endWorld - cameraWorld;
    float rayDistance = max( length( ray ), 1.0f );
    float3 rayDir = ray / rayDistance;
    float cloudBase = LC_FogHeight;
    float heightScale = max( 0.35f, LC_HeightScale );
    float distanceScale = max( 0.45f, LC_DistanceScale );
    float nightBlend = LowCloudNightBlend();
    float layerMin = cloudBase - 3200.0f * heightScale;
    float layerMax = cloudBase + 18200.0f * heightScale;
    float marchDistance = lerp( min( cameraDistance, 105000.0f ), 78000.0f, skyPixel ) * distanceScale;
    float startDistance = lerp( 7000.0f, 14500.0f, skyPixel ) * distanceScale;
    float skyHorizonWeight = lerp( 1.0f, lerp( 0.12f, 1.0f, 1.0f - LowCloudSmootherStep( ( rayDir.y - 0.20f ) / 0.52f ) ), skyPixel );
    float skyHorizonFill = skyPixel * ( 1.0f - LowCloudSmootherStep( ( abs( rayDir.y ) - 0.008f ) / 0.115f ) );
    // At night keep the horizon clear so the dark banks don't read as a wall
    float nightHorizonClearance = lerp( 1.0f, LowCloudSmootherStep( ( rayDir.y - 0.085f ) / 0.220f ), nightBlend * skyPixel );

    if ( abs( rayDir.y ) > 0.035f && skyHorizonFill < 0.001f )
    {
        float t0 = ( layerMin - cameraWorld.y ) / rayDir.y;
        float t1 = ( layerMax - cameraWorld.y ) / rayDir.y;
        startDistance = max( startDistance, min( t0, t1 ) - 3600.0f * heightScale );
        marchDistance = min( marchDistance, max( t0, t1 ) + 5200.0f * heightScale );
    }
    if ( marchDistance <= startDistance + 200.0f )
        return float4( 0.0f, 0.0f, 0.0f, 0.0f );

    float usableDistance = max( marchDistance - startDistance, 1.0f );
    float dayWeight = saturate( AC_LightPos.y * 2.4f + 0.22f );
    float rainWeight = saturate( AC_RainFXWeight );
    float3 dayLitClear = lerp( LC_FogColor * 0.70f, float3( 0.78f, 0.79f, 0.76f ), 0.70f ) * max( LC_DayColor, 0.0f );
    float3 dayShadowClear = lerp( LC_FogColor * 0.38f, float3( 0.38f, 0.40f, 0.40f ), 0.68f ) * max( LC_DayColor, 0.0f );
    float3 dayLitRain = lerp( LC_FogColor * 0.58f, float3( 0.54f, 0.55f, 0.54f ), 0.72f ) * max( LC_RainColor, 0.0f );
    float3 dayShadowRain = lerp( LC_FogColor * 0.27f, float3( 0.21f, 0.23f, 0.24f ), 0.80f ) * max( LC_RainColor, 0.0f );
    // Night clouds sit in the same haze the height fog fades into (HeightfogColor's night color), not near-black
    float3 nightFog = float3( 0.12f, 0.18f, 0.27f ) / 2.5f;
    float3 nightLit = nightFog * 2.0f * max( LC_NightColor, 0.0f );
    float3 nightShadow = nightFog * 1.1f * max( LC_NightColor, 0.0f );
    float3 litColor = lerp( nightLit, lerp( dayLitClear, dayLitRain, rainWeight ), dayWeight );
    float3 shadowColor = lerp( nightShadow, lerp( dayShadowClear, dayShadowRain, rainWeight ), dayWeight );

    float3 lightDir = LowCloudSunDir();
    float viewSunForward = pow( saturate( dot( rayDir, lightDir ) * 0.5f + 0.5f ), 4.0f );
    float3 moonDir = LowCloudMoonDir();
    float viewMoonForward = pow( saturate( dot( rayDir, moonDir ) * 0.5f + 0.5f ), 4.0f );
    float moonLight = saturate( LC_MoonVisibility ) * ( 1.0f - dayWeight ) * saturate( moonDir.y * 1.35f ) * lerp( 1.0f, 0.45f, rainWeight );
    float nearFadeStart = lerp( 7800.0f, 18000.0f, skyPixel ) * distanceScale;
    float nearFadeEnd = lerp( 18000.0f, 32000.0f, skyPixel ) * distanceScale;
    float farFadeStart = lerp( 105000.0f, 72000.0f, skyPixel ) * distanceScale;
    float farFadeEnd = lerp( 140000.0f, 98000.0f, skyPixel ) * distanceScale;

    float transmittance = 1.0f;
    float3 scattering = 0.0f;
    float accumulatedAlpha = 0.0f;
    const int MAX_STEPS = 16;
    int steps = clamp( requestedSteps, 1, MAX_STEPS );
    float stepLength = usableDistance / (float)steps;

    [loop]
    for ( int i = 0; i < MAX_STEPS; ++i )
    {
        if ( i >= steps ) break;
        float sampleDistance = startDistance + ( i + jitter ) * stepLength;
        float3 sampleWorld = cameraWorld + rayDir * sampleDistance;
        float distanceFade = smoothstep( nearFadeStart, nearFadeEnd, sampleDistance )
                           * ( 1.0f - smoothstep( farFadeStart, farFadeEnd, sampleDistance ) );
        float density = ComputeLowCloudDensity( sampleWorld, skyPixel ) * distanceFade * skyHorizonWeight * nightHorizonClearance;
        if ( density <= 0.0f ) continue;

        float upperSelfLight = smoothstep( cloudBase + 2600.0f * heightScale, cloudBase + 9400.0f * heightScale, sampleWorld.y );
        float selfShadow = lerp( 0.46f, 0.94f, upperSelfLight ) * lerp( 1.0f, 0.72f, saturate( density * 1.20f ) );
        float3 cloudColor = lerp( shadowColor, litColor, selfShadow );

        float upperSunLayer = smoothstep( cloudBase + 3900.0f * heightScale, cloudBase + 9200.0f * heightScale, sampleWorld.y );
        float sunTopLight = upperSunLayer * dayWeight * saturate( lightDir.y * 1.35f ) * selfShadow
                          * lerp( 1.0f, 0.45f, rainWeight ) * max( 0.0f, LC_SunLight );
        cloudColor += float3( 0.155f, 0.156f, 0.140f ) * sunTopLight * lerp( 0.38f, 0.88f, viewSunForward );
        // Moonlit tops turn silver-blue
        cloudColor += float3( 0.42f, 0.56f, 1.0f ) * 0.10f * upperSunLayer * selfShadow * moonLight * lerp( 0.38f, 0.88f, viewMoonForward );

        float sampleAlpha = saturate( density * lerp( 0.53f, 0.75f, dayWeight ) * lerp( 1.0f, 0.92f, nightBlend ) );
        sampleAlpha = 1.0f - exp( -sampleAlpha * stepLength * 0.00022f );
        float weight = sampleAlpha * transmittance;
        scattering += cloudColor * weight;
        accumulatedAlpha += weight;
        transmittance *= 1.0f - sampleAlpha;
        if ( transmittance <= 0.001f ) break;
    }

    accumulatedAlpha = saturate( accumulatedAlpha * 1.04f );
    return float4( saturate( scattering / max( accumulatedAlpha, 0.001f ) ), accumulatedAlpha );
}

//--------------------------------------------------------------------------------------
// Lighting added after the march: rain veil, sun glow through thin cloud, moon disc cover
//--------------------------------------------------------------------------------------
float3 LowCloudTransmittedSunColor() { return lerp( float3( 1.00f, 0.72f, 0.42f ), float3( 1.00f, 0.92f, 0.74f ), saturate( AC_LightPos.y * 2.5f ) ); }

// Returns premultiplied color and the layer alpha (which may exceed the color alpha where it covers the moon).
float4 FinishLowClouds( float4 clouds, float3 viewDir, float cameraDistance, float skyPixel, bool sunDisc )
{
    float nightBlend = LowCloudNightBlend();
    float sunWeight = LowCloudSunWeight();
    float sunAlignment = dot( viewDir, LowCloudSunDir() );
    float moonAlignment = dot( viewDir, LowCloudMoonDir() );
    float moonDiskWeight = saturate( LC_MoonVisibility ) * smoothstep( 0.02f, 0.34f, LC_MoonDir.y ) * skyPixel;

    float3 rainVeilColor = float3( 0.12f, 0.18f, 0.27f ) * LC_NightFogBrightness / 2.5f;
    float rainVeil = saturate( AC_RainFXWeight ) * lerp( 0.045f, 0.30f, nightBlend );
    float veilAmount = rainVeil * lerp( 0.45f, 1.0f, skyPixel ) * LowCloudSmootherStep( ( cameraDistance - 3500.0f ) / 52000.0f );
    clouds.rgb = lerp( clouds.rgb, rainVeilColor, veilAmount );
    clouds.a *= 1.0f - veilAmount * 0.34f;

    float alpha = saturate( clouds.a );
    float3 sunColor = LowCloudTransmittedSunColor();
    float sunGain = sunWeight * max( 0.0f, LC_SunLight );
    if ( sunDisc )
    {
        float sunCore = smoothstep( 0.99920f, 0.99986f, sunAlignment ) * skyPixel;
        float sunHalo = smoothstep( 0.99200f, 0.99860f, sunAlignment ) * skyPixel;
        float backlightDensity = saturate( 1.0f - abs( alpha - 0.52f ) / 0.42f ) * ( 1.0f - smoothstep( 0.68f, 0.94f, alpha ) );
        clouds.rgb += sunColor * saturate( sunCore * 0.42f + sunHalo * 0.18f ) * sunGain * backlightDensity * 0.24f;
    }
    float broadBody = smoothstep( 0.14f, 0.46f, alpha ) * ( 1.0f - smoothstep( 0.72f, 0.95f, alpha ) );
    float thinEdge = smoothstep( 0.05f, 0.22f, alpha ) * ( 1.0f - smoothstep( 0.30f, 0.50f, alpha ) );
    clouds.rgb += sunColor * smoothstep( 0.82f, 0.97f, sunAlignment ) * sunGain * ( broadBody * 0.10f + thinEdge * 0.06f );

    float moonCore = smoothstep( 0.99935f, 0.99988f, moonAlignment ) * moonDiskWeight;
    float moonHalo = smoothstep( 0.99500f, 0.99900f, moonAlignment ) * moonDiskWeight;
    float moonCover = saturate( saturate( moonCore + moonHalo * 0.24f ) * saturate( alpha * 1.90f ) ) * lerp( 0.78f, 0.995f, moonCore );
    float layerAlpha = saturate( alpha + moonCover * ( 1.0f - alpha ) );

    // Gothic's world fog zones fade the whole layer, like the clouds' water reflection and god-ray cover
    float fogVisibility = 1.0f - smoothstep( 0.08f, 0.55f, saturate( LC_FogOverride ) );
    return float4( clouds.rgb * alpha * fogVisibility, layerAlpha * fogVisibility );
}

//--------------------------------------------------------------------------------------
// Half-resolution generate pass: one texel covers a 2x2 block of full-resolution depth
//--------------------------------------------------------------------------------------
struct LowCloudLayerTexel
{
    float4 clouds;      // premultiplied
    float  depth;       // nearest raw depth of the footprint
    float4 skyClouds;   // clouds against the sky; a = -1 when the footprint has no sky
};

LowCloudLayerTexel GenerateLowCloudTexel( float2 uv, float4 footprintDepth, float2 pixel )
{
    float closestDepth = max( max( footprintDepth.x, footprintDepth.y ), max( footprintDepth.z, footprintDepth.w ) );
    bool hasSky = any( footprintDepth < LOW_CLOUD_SKY_DEPTH );
    bool hasGeometry = any( footprintDepth >= LOW_CLOUD_SKY_DEPTH );
    float skyPixel = closestDepth < LOW_CLOUD_SKY_DEPTH ? 1.0f : 0.0f;

    float3 worldPosition = LowCloudWorldPosition( closestDepth, uv );
    float cameraDistance = length( worldPosition - LC_CameraPos );
    float jitter = LowCloudJitter( pixel );
    float4 clouds = ComputeLowCloudVolume( LC_CameraPos, worldPosition, cameraDistance, skyPixel, skyPixel > 0.5f ? 16 : 8, jitter );

    float3 skyWorldPosition = LowCloudWorldPosition( 0.0f, uv );
    float skyCameraDistance = length( skyWorldPosition - LC_CameraPos );
    float4 skyClouds = clouds;
    if ( hasSky && hasGeometry )
        skyClouds = ComputeLowCloudVolume( LC_CameraPos, skyWorldPosition, skyCameraDistance, 1.0f, 16, jitter );

    LowCloudLayerTexel o;
    o.clouds = FinishLowClouds( clouds, normalize( worldPosition - LC_CameraPos ), cameraDistance, skyPixel, true );
    o.depth = closestDepth;
    o.skyClouds = hasSky
        ? FinishLowClouds( skyClouds, normalize( skyWorldPosition - LC_CameraPos ), skyCameraDistance, 1.0f, true )
        : float4( 0.0f, 0.0f, 0.0f, -1.0f );
    return o;
}

#ifdef LOW_CLOUDS_COMPOSITE
//--------------------------------------------------------------------------------------
// Full-resolution composite: depth-aware upsample, re-march where the half-res layer has no match
//--------------------------------------------------------------------------------------
float LowCloudDepthWeight( float targetDepth, float sourceDepth )
{
    bool targetIsSky = targetDepth < LOW_CLOUD_SKY_DEPTH;
    bool sourceIsSky = sourceDepth < LOW_CLOUD_SKY_DEPTH;
    if ( targetIsSky != sourceIsSky ) return 0.0f;
    if ( targetIsSky ) return 1.0f;
    float delta = abs( targetDepth - sourceDepth ) / max( max( targetDepth, sourceDepth ), LOW_CLOUD_SKY_DEPTH );
    return delta >= 0.18f ? 0.0f : exp2( -delta * 32.0f );
}

float4 SampleSkyLowClouds( float2 uv )
{
    int2 size = LowCloudLayerSize();
    float2 position = uv * float2( size ) - 0.5f;
    int2 center = int2( floor( position + 0.5f ) );
    float4 sum = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for ( int y = -1; y <= 1; ++y )
    {
        [unroll]
        for ( int x = -1; x <= 1; ++x )
        {
            int2 texel = clamp( center + int2( x, y ), int2( 0, 0 ), size - 1 );
            float4 s = LowCloudLoadSky( texel );
            if ( s.a < 0.0f ) continue;
            float2 d = float2( texel ) - position;
            float weight = exp2( -dot( d, d ) * 0.55f ) * lerp( 0.35f, 0.85f, saturate( s.a * 2.0f ) );
            sum += s * weight;
            totalWeight += weight;
        }
    }
    return totalWeight > 0.00001f ? sum / totalWeight : float4( 0.0f, 0.0f, 0.0f, 0.0f );
}

float4 MarchLowCloudsAt( float2 uv, float rawDepth, float skyPixel, float2 pixel )
{
    float3 worldPosition = LowCloudWorldPosition( rawDepth, uv );
    float cameraDistance = length( worldPosition - LC_CameraPos );
    float4 clouds = ComputeLowCloudVolume( LC_CameraPos, worldPosition, cameraDistance, skyPixel, 6, LowCloudJitter( pixel ) );
    return FinishLowClouds( clouds, normalize( worldPosition - LC_CameraPos ), cameraDistance, skyPixel, false );
}

float4 SampleSkyAwareLowClouds( float2 uv, float2 pixel )
{
    // Thin cloud edges near the horizon get re-marched at full resolution
    float4 skyClouds = SampleSkyLowClouds( uv );
        float3 rayDir = normalize( LowCloudWorldPosition( 0.0f, uv ) - LC_CameraPos );
        float refine = ( 1.0f - smoothstep( 0.015f, 0.18f, abs( rayDir.y ) ) )
                     * smoothstep( 0.001f, 0.04f, skyClouds.a ) * ( 1.0f - smoothstep( 0.02f, 0.75f, skyClouds.a ) );
    [branch] if ( refine > 0.01f )
    {
        float sampledAlpha = saturate( skyClouds.a );
        float refinedAlpha = saturate( MarchLowCloudsAt( uv, 0.0f, 1.0f, pixel ).a );
        skyClouds.rgb *= sampledAlpha > 0.00001f ? lerp( 1.0f, refinedAlpha / sampledAlpha, refine ) : 0.0f;
        skyClouds.a = lerp( sampledAlpha, refinedAlpha, refine );
    }
    return skyClouds;
}

// 3x3 depth-aware Gaussian: smooths the march jitter; re-marches where too few texels share this depth.
float4 SampleGeometryLowClouds( float2 uv, float targetDepth, float2 pixel )
{
    int2 size = LowCloudLayerSize();
    float2 position = uv * float2( size ) - 0.5f;
    int2 center = int2( floor( position + 0.5f ) );
    float4 sum = 0.0f;
    float totalWeight = 0.0f;
    float spatialWeight = 0.0f;
    [unroll]
    for ( int y = -1; y <= 1; ++y )
    {
        [unroll]
        for ( int x = -1; x <= 1; ++x )
        {
            int2 texel = clamp( center + int2( x, y ), int2( 0, 0 ), size - 1 );
            float2 d = float2( texel ) - position;
            float spatial = exp2( -dot( d, d ) * 0.9f );
            float weight = spatial * LowCloudDepthWeight( targetDepth, LowCloudLoadDepth( texel ) );
            sum += LowCloudLoadLayer( texel ) * weight;
            totalWeight += weight;
            spatialWeight += spatial;
        }
    }
    const float refineStart = 0.45f;
    const float confident = 0.60f;
    float confidence = totalWeight / max( spatialWeight, 0.00001f );
    float4 result = sum / max( totalWeight, 0.00001f );
    [branch] if ( confidence < confident )
    {
        float4 refined = MarchLowCloudsAt( uv, targetDepth, 0.0f, pixel );
        result = totalWeight > 0.00001f ? lerp( refined, result, smoothstep( refineStart, confident, confidence ) ) : refined;
    }
    return result;
}

float4 SampleDepthAwareLowClouds( float2 uv, float targetDepth, float2 pixel )
{
    float4 clouds;
    [branch] if ( targetDepth < LOW_CLOUD_SKY_DEPTH )
        clouds = SampleSkyAwareLowClouds( uv, pixel );
    else
        clouds = SampleGeometryLowClouds( uv, targetDepth, pixel );
    return clouds;
}

// Blend weights for ONE / INV_SRC_ALPHA onto the scene: rain and night veil folded in, sun and moon kept visible.
float4 CompositeLowClouds( float2 uv, float targetDepth, float2 pixel )
{
    float4 clouds = SampleDepthAwareLowClouds( uv, targetDepth, pixel );

    float rain = saturate( AC_RainFXWeight );
    float nightBlend = LowCloudNightBlend();
    float rainVisibility = 1.0f - smoothstep( 0.18f, 0.88f, rain );
    float veil = saturate( rain * lerp( 0.050f, 0.22f, nightBlend ) + ( 1.0f - rain ) * nightBlend * 0.12f );
    float alpha = saturate( clouds.a ) * rainVisibility;

    // The veil pulls the cloud color toward the scene behind it: lerp(c, scene, v) over a blend is (1 - v) less cloud
    float sceneShare = 0.0f;
    if ( alpha > 0.001f && veil > 0.0001f )
    {
        sceneShare = veil * lerp( 0.65f, 1.0f, nightBlend );
        alpha *= 1.0f - veil * lerp( 0.08f, 0.22f, nightBlend );
    }

    float sunMask = ( 1.0f - smoothstep( 0.018f, 0.060f, length( uv - LC_SunScreen.xy ) ) ) * saturate( LC_SunScreen.z );
    float moonMask = ( 1.0f - smoothstep( 0.018f, 0.060f, length( uv - LC_MoonScreen.xy ) ) ) * saturate( LC_MoonScreen.z );
    float effectiveAlpha = min( alpha, lerp( 1.0f, 0.88f, max( sunMask, moonMask ) ) );
    float colorScale = saturate( clouds.a ) > 0.00001f ? effectiveAlpha / saturate( clouds.a ) : 0.0f;

    return float4( clouds.rgb * colorScale * ( 1.0f - sceneShare ), effectiveAlpha * ( 1.0f - sceneShare ) );
}

#endif // LOW_CLOUDS_COMPOSITE

#endif
