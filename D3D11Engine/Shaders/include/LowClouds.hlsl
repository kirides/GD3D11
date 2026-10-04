#ifndef LOW_CLOUDS_HLSL
#define LOW_CLOUDS_HLSL
// Low clouds: ray-marched cloud banks above the fog height, from MarcoMarwin's GD3D11 ("dynamic clouds").
// Shared by D3D11 (PS_PFX_LowClouds*) and D3D12 (D3D12/LowClouds.hlsl); colors are gamma space.
// Needs the Atmosphere constants (AC_LightPos, AC_RainFXWeight) included first. The composite also needs
// LowCloudLoadLayer, LowCloudLoadDepth, LowCloudLoadSky and LowCloudLayerSize defined before inclusion.
// Density samples the 3D noise of LowCloudNoise.hlsl: D3D11 t4/s3, D3D12 LC_NoiseIndex/s0 (LOW_CLOUD_BINDLESS_NOISE).

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
    uint   LC_NoiseIndex;         // D3D12: bindless SRV of the 3D noise texture
    float2 LC_Pad;
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

float LowCloudSunWeight() { return saturate( LC_SunVisibility ) * smoothstep( 0.04f, 0.42f, AC_LightPos.y ); }

float3 LowCloudToLinear( float3 c ) { return pow( max( c, 0.0f ), 2.2f ); }

// Warm and dimmed near the horizon, near white at noon
float3 LowCloudTransmittedSunColor()
{
    float y = AC_LightPos.y;
    float3 low = lerp( float3( 1.00f, 0.42f, 0.18f ), float3( 1.00f, 0.74f, 0.50f ), smoothstep( -0.02f, 0.12f, y ) );
    return lerp( low, float3( 1.00f, 0.95f, 0.86f ), smoothstep( 0.12f, 0.45f, y ) );
}

//--------------------------------------------------------------------------------------
// Noise and density
//--------------------------------------------------------------------------------------
#ifdef LOW_CLOUD_BINDLESS_NOISE
SamplerState LC_NoiseSampler : register( s0 );   // linear WRAP
float4 LowCloudNoise( float3 uvw ) { Texture3D t = ResourceDescriptorHeap[LC_NoiseIndex]; return t.SampleLevel( LC_NoiseSampler, uvw, 0.0f ); }
#else
Texture3D LC_NoiseTex : register( t4 );
SamplerState LC_NoiseSampler : register( s3 );   // linear WRAP
float4 LowCloudNoise( float3 uvw ) { return LC_NoiseTex.SampleLevel( LC_NoiseSampler, uvw, 0.0f ); }
#endif

static const float LOW_CLOUD_EXTINCTION = 0.00050f;   // per world unit at density 1

float LowCloudRemap( float x, float a, float b, float c, float d ) { return c + ( x - a ) / ( b - a ) * ( d - c ); }

float3 LowCloudWind() { return float3( 198.0f, 2.0f, -120.0f ) * LC_Time * max( 0.0f, LC_Speed ); }

// Per-column weather: how cloudy it is here and where the cloud starts and ends
struct LowCloudColumn
{
    float coverage;
    float base;
    float top;
};

LowCloudColumn LowCloudGetColumn( float3 p )
{
    float scale = max( 0.35f, LC_Scale );
    float heightScale = max( 0.35f, LC_HeightScale );
    float2 xz = ( p.xz + LowCloudWind().xz * 0.8f ) / ( 210000.0f * scale );
    float evolve = LC_Time * 0.00035f * max( 0.0f, LC_Speed );
    // Two incommensurate lookups so the 64^3 tile never repeats visibly
    float4 a = LowCloudNoise( float3( xz, evolve ) );
    float4 b = LowCloudNoise( float3( xz.yx * 2.31f + float2( 0.37f, 0.71f ), 0.5f - evolve * 1.7f ) );
    float weather = a.g * 0.65f + b.g * 0.35f;

    float amount = saturate( 0.40f * LC_Density );
    float threshold = lerp( 0.85f, 0.15f, amount );

    LowCloudColumn c;
    c.coverage = smoothstep( threshold - 0.10f, threshold + 0.18f, weather );
    c.base = LC_FogHeight + lerp( 600.0f, 2400.0f, a.b ) * heightScale;
    c.top = c.base + lerp( 4200.0f, 13000.0f, saturate( c.coverage * 0.75f + ( b.r - 0.5f ) * 0.8f + 0.1f ) ) * heightScale;
    return c;
}

// Coverage-eroded Perlin-Worley shape; h is the height within the column
float LowCloudShape( float3 p, LowCloudColumn c, out float h )
{
    h = saturate( ( p.y - c.base ) / max( c.top - c.base, 1.0f ) );
    float density = 0.0f;
    [branch] if ( c.coverage > 0.001f && p.y > c.base && p.y < c.top )
    {
        float scale = max( 0.35f, LC_Scale );
        float heightScale = max( 0.35f, LC_HeightScale );
        float3 q = ( p + LowCloudWind() ) / ( 27000.0f * scale );
        q.y *= 1.35f / heightScale;
        float4 n = LowCloudNoise( q );
        float shape = saturate( LowCloudRemap( n.r, n.b - 1.0f, 1.0f, 0.0f, 1.0f ) );

        // Flat base, rounded top
        shape *= smoothstep( 0.0f, 0.12f, h ) * ( 1.0f - smoothstep( 0.35f, 1.0f, h ) );
        density = saturate( LowCloudRemap( shape, 1.0f - c.coverage, 1.0f, 0.0f, 1.0f ) ) * c.coverage;
    }
    return density;
}

// High-frequency Worley erosion: wispy at the base, billowy on top
float LowCloudErode( float density, float3 p, float h )
{
    float scale = max( 0.35f, LC_Scale );
    float4 n = LowCloudNoise( ( p + LowCloudWind() * 1.6f ) / ( 5600.0f * scale ) + float3( 0.0f, h * 0.15f, 0.0f ) );
    float detail = n.b * 0.6f + n.a * 0.4f;
    detail = lerp( 1.0f - detail, detail, saturate( h * 5.0f ) );
    return saturate( LowCloudRemap( density, detail * 0.38f, 1.0f, 0.0f, 1.0f ) );
}

float LowCloudDensityScale() { return sqrt( max( 0.0f, LC_Density ) ); }

float3 LowCloudSafeNormalize( float3 v ) { return v / max( length( v ), 1e-4f ); }

//--------------------------------------------------------------------------------------
// Lighting: short shadow march toward the sun or moon, dual-lobe phase, multiple-scattering octaves
//--------------------------------------------------------------------------------------
float LowCloudLightDepth( float3 p, float3 lightDir, LowCloudColumn c )
{
    static const float offsets[4] = { 0.04f, 0.14f, 0.32f, 0.68f };
    static const float widths[4] = { 0.08f, 0.12f, 0.24f, 0.48f };
    float rayLength = 6400.0f * max( 0.35f, LC_HeightScale );
    float depth = 0.0f;
    [unroll] for ( int i = 0; i < 4; ++i )
    {
        float h;
        depth += LowCloudShape( p + lightDir * ( offsets[i] * rayLength ), c, h ) * widths[i];
    }
    return depth * rayLength * LOW_CLOUD_EXTINCTION * LowCloudDensityScale();
}

// Henyey-Greenstein, normalized so an isotropic medium is 1
float LowCloudHG( float cosTheta, float g )
{
    float g2 = g * g;
    return ( 1.0f - g2 ) / pow( max( 1.0f + g2 - 2.0f * g * cosTheta, 1e-4f ), 1.5f );
}

float LowCloudLightEnergy( float lightDepth, float cosTheta )
{
    float energy = 0.0f;
    float a = 1.0f;
    float k = 1.0f;
    [unroll] for ( int o = 0; o < 3; ++o )
    {
        float phase = lerp( LowCloudHG( cosTheta, -0.25f * k ), LowCloudHG( cosTheta, 0.65f * k ), 0.55f );
        energy += a * exp( -k * lightDepth ) * phase;
        a *= 0.5f;
        k *= 0.5f;
    }
    return energy;
}

// Keeps the shadow ray out of the horizontal so it still leaves the layer at sunrise and sunset
float3 LowCloudLightRayDir( float3 dir ) { return normalize( float3( dir.x, max( dir.y, 0.03f ), dir.z ) ); }

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

    // Sky light from above, darker bounce from below; overcast skies lean on it harder
    float overcast = rainWeight * ( 1.0f - saturate( LC_SunVisibility ) );
    float3 ambientBottom = LowCloudToLinear( shadowColor ) * 0.85f;
    float3 ambientTop = LowCloudToLinear( lerp( shadowColor, litColor, lerp( 0.35f, 0.85f, overcast ) ) );
    float3 hazeColor = LowCloudToLinear( lerp( shadowColor, litColor, 0.5f ) );

    // One main light: the sun (down to the horizon, for sunset light) or the moon
    float rainDim = lerp( 1.0f, 0.45f, rainWeight );
    float sunIntensity = saturate( LC_SunVisibility ) * smoothstep( -0.03f, 0.08f, AC_LightPos.y ) * max( 0.0f, LC_SunLight ) * rainDim;
    float moonIntensity = saturate( LC_MoonVisibility ) * smoothstep( 0.0f, 0.15f, LC_MoonDir.y ) * rainDim;
    bool moonIsMain = moonIntensity > sunIntensity;
    float3 lightDir = LowCloudLightRayDir( moonIsMain ? LC_MoonDir : AC_LightPos.xyz );
    float3 lightColor = moonIsMain
        ? LowCloudToLinear( float3( 0.42f, 0.56f, 1.0f ) * 0.20f ) * moonIntensity
        : LowCloudToLinear( LowCloudTransmittedSunColor() ) * max( LC_DayColor, 0.0f ) * sunIntensity;
    bool lightOn = max( sunIntensity, moonIntensity ) > 0.001f;
    float cosTheta = dot( rayDir, LowCloudSafeNormalize( moonIsMain ? LC_MoonDir : AC_LightPos.xyz ) );
    // Beer-powder: dark crinkled edges when the light is behind the viewer, none when looking into it
    float powderStrength = 0.75f * saturate( 0.5f - 0.5f * cosTheta );

    float nearFadeStart = lerp( 7800.0f, 18000.0f, skyPixel ) * distanceScale;
    float nearFadeEnd = lerp( 18000.0f, 32000.0f, skyPixel ) * distanceScale;
    float farFadeStart = lerp( 105000.0f, 72000.0f, skyPixel ) * distanceScale;
    float farFadeEnd = lerp( 140000.0f, 98000.0f, skyPixel ) * distanceScale;
    float densityScale = LowCloudDensityScale();

    float transmittance = 1.0f;
    float3 scattering = 0.0f;
    float accumulatedAlpha = 0.0f;
    const int MAX_STEPS = 32;
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
        float fade = distanceFade * skyHorizonWeight * nightHorizonClearance;
        if ( fade <= 0.0f ) continue;

        LowCloudColumn column = LowCloudGetColumn( sampleWorld );
        float h;
        float shape = LowCloudShape( sampleWorld, column, h );
        if ( shape <= 0.0f ) continue;
        float density = LowCloudErode( shape, sampleWorld, h ) * densityScale;
        if ( density <= 0.0f ) continue;

        float3 sampleLight = lerp( ambientBottom, ambientTop, smoothstep( 0.0f, 0.9f, h ) );
        [branch] if ( lightOn )
        {
            float lightDepth = LowCloudLightDepth( sampleWorld, lightDir, column );
            float powder = lerp( 1.0f, 1.0f - exp( -density * 6.0f ), powderStrength );
            sampleLight += lightColor * LowCloudLightEnergy( lightDepth, cosTheta ) * powder * 0.45f;
        }
        sampleLight = lerp( sampleLight, hazeColor, 0.35f * smoothstep( 0.25f, 0.9f, sampleDistance / farFadeEnd ) );

        float sampleAlpha = 1.0f - exp( -density * fade * LOW_CLOUD_EXTINCTION * stepLength );
        float weight = sampleAlpha * transmittance;
        scattering += sampleLight * weight;
        accumulatedAlpha += weight;
        transmittance *= 1.0f - sampleAlpha;
        if ( transmittance <= 0.003f ) break;
    }

    accumulatedAlpha = saturate( accumulatedAlpha );
    float3 color = pow( max( scattering / max( accumulatedAlpha, 0.001f ), 0.0f ), 1.0f / 2.2f );
    return float4( saturate( color ), accumulatedAlpha );
}

//--------------------------------------------------------------------------------------
// After the march: rain veil, sun disc through thin cloud, moon disc cover
//--------------------------------------------------------------------------------------
// Returns premultiplied color and the layer alpha (which may exceed the color alpha where it covers the moon).
float4 FinishLowClouds( float4 clouds, float3 viewDir, float cameraDistance, float skyPixel, bool sunDisc )
{
    float nightBlend = LowCloudNightBlend();
    float sunWeight = LowCloudSunWeight();
    float sunAlignment = dot( viewDir, LowCloudSafeNormalize( AC_LightPos.xyz ) );
    float moonAlignment = dot( viewDir, LowCloudSafeNormalize( LC_MoonDir ) );
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
    float4 clouds = ComputeLowCloudVolume( LC_CameraPos, worldPosition, cameraDistance, skyPixel, skyPixel > 0.5f ? 32 : 12, jitter );

    float3 skyWorldPosition = LowCloudWorldPosition( 0.0f, uv );
    float skyCameraDistance = length( skyWorldPosition - LC_CameraPos );
    float4 skyClouds = clouds;
    if ( hasSky && hasGeometry )
        skyClouds = ComputeLowCloudVolume( LC_CameraPos, skyWorldPosition, skyCameraDistance, 1.0f, 32, jitter );

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
    float4 clouds = ComputeLowCloudVolume( LC_CameraPos, worldPosition, cameraDistance, skyPixel, 8, LowCloudJitter( pixel ) );
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
