//--------------------------------------------------------------------------------------
// PointLightShadows.h - Shared pointlight shadow and lighting helpers
//--------------------------------------------------------------------------------------
#ifndef POINT_LIGHT_SHADOWS_H
#define POINT_LIGHT_SHADOWS_H

#if !defined(__cplusplus)

// TiledPointLight::ShadowCubeIndex encoding: -1 = unshadowed, else (slot | flags). Bit 30 = slot also has a
// dynamic overlay cube (min'd with the static sample). Bit 29 = slot lives in the low-res static-only tier
// instead of the full-res one (never set together with bit 30). Mirrors D3D11TiledDeferredShading.h.
static const int PLS_SHADOW_HAS_DYNAMIC = 0x40000000;
static const int PLS_SHADOW_TIER_LOW = 0x20000000;
static const int PLS_SHADOW_SLOT_MASK = 0x1FFFFFFF;

static const int PLS_SHADOW_BLUR_COUNT = 8;
static const float2 PLS_SHADOW_BLUR_OFFSETS[PLS_SHADOW_BLUR_COUNT] = {
    float2( 0.076849f, -0.078216f),
    float2(-0.165415f,  0.370808f),
    float2(-0.551062f, -0.407284f),
    float2( 0.449733f, -0.518174f),
    float2( 0.347526f,  0.730303f),
    float2(-0.840654f,  0.134261f),
    float2( 0.896791f,  0.038446f),
    float2(-0.258169f, -0.912648f)
};

// Extra ring used only for the low-res static-only tier (STATIC_SHADOW_CUBE_SIZE, 32^2 faces). At that
// resolution each texel covers a large solid angle, so the caster silhouette itself is blocky - not just
// its edge. PLS_SHADOW_BLUR_OFFSETS alone (8 taps in a tight ring) isn't enough support to average that
// away; combined with PLS_SHADOW_BLUR_TIER_LOW_COUNT taps and a wider radius (see PLS_PrepareShadowSampling's
// tierLow blur boost) this ring pushes the PCF footprint across several texels so the steps blend into a
// smooth gradient instead of visible squares.
static const int PLS_SHADOW_BLUR_TIER_LOW_COUNT = 16;
static const float2 PLS_SHADOW_BLUR_OFFSETS_TIER_LOW[PLS_SHADOW_BLUR_TIER_LOW_COUNT] = {
    float2( 0.076849f, -0.078216f),
    float2(-0.165415f,  0.370808f),
    float2(-0.551062f, -0.407284f),
    float2( 0.449733f, -0.518174f),
    float2( 0.347526f,  0.730303f),
    float2(-0.840654f,  0.134261f),
    float2( 0.896791f,  0.038446f),
    float2(-0.258169f, -0.912648f),
    float2( 0.573813f,  0.398287f),
    float2(-0.398287f,  0.573813f),
    float2( 0.184238f,  0.982878f),
    float2(-0.982878f, -0.184238f),
    float2( 0.712698f, -0.701456f),
    float2(-0.701456f, -0.712698f),
    float2( 0.994522f,  0.104528f),
    float2(-0.104528f,  0.994522f)
};

float PLS_AggressiveNoise(float3 p)
{
    float3 p3  = frac(p * 0.1031f);
    p3 += dot(p3, p3.zyx + 31.32f);
    return frac((p3.x + p3.y) * p3.z);
}

float PLS_Hash3D( float3 p )
{
    float3 p3  = frac( p * 0.1031f );
    p3 += dot( p3, p3.zyx + 31.32f );
    return frac( (p3.x + p3.y) * p3.z );
}

float PLS_CalcBlinnPhongLighting( float3 N, float3 H )
{
    return saturate( dot( N, H ) );
}

float PLS_ComputeSpecMod( float3 diffuseColor )
{
    return pow( dot( float3( 0.333f, 0.333f, 0.333f ), diffuseColor ), 2 );
}

float PLS_ComputeRangeFalloff( float distance, float lightRange )
{
    float normalizedDist = saturate( 1.0f - (distance / lightRange) );
    return normalizedDist * (normalizedDist * 0.2f + 0.8f);
}

float PLS_ApplyShadowDistanceFade( float finalShadow, float normalizedDist )
{
    // Keep fade-out for mostly lit samples, but preserve strong occlusion to avoid wall bleed.
    float shadowFade = smoothstep( 0.65f, 0.95f, normalizedDist );
    float fadeWeight = shadowFade * smoothstep( 0.45f, 0.90f, finalShadow );
    return lerp( finalShadow, 1.0f, fadeWeight );
}

float3 PLS_ComputePointLightLighting(
    float3 diffuseColor,
    float3 lightColor,
    float ndl,
    float falloff,
    float spec,
    float specIntensity,
    float specPower,
    float specMod )
{
    float3 specBare = pow( spec, specPower ) * specIntensity * lightColor * falloff;
    float3 specColored = lerp( specBare, specBare * diffuseColor, specMod );

    float3 color = saturate( falloff * ndl * lightColor );
    return color * diffuseColor + specColored;
}

void PLS_PrepareShadowSampling(
    float3 wsPosition,
    float3 N,
    float3 lightPosWorld,
    float lightRange,
    bool tierLow,
    out float3 dir,
    out float compareDistance,
    out float fixedBias,
    out float fixedBlurScale,
    out float3 right,
    out float3 up,
    out float sinA,
    out float cosA )
{
    float3 toPixelOriginal = wsPosition - lightPosWorld;
    float distOriginal = length( toPixelOriginal );
    float3 L = toPixelOriginal / distOriginal; 

    // Slope-Scaled Normal Bias
    float nDotL = saturate( dot( N, -L ) );
    float slopeScale = 1.0f - nDotL; 
    float normalOffsetScale = distOriginal * 0.02f * (slopeScale + 0.1f); 
    float3 biasedWsPosition = wsPosition + N * normalOffsetScale;

    // Recalculate vectors
    float3 toPixel = biasedWsPosition - lightPosWorld;
    dir = normalize( toPixel );

    float distance = length( toPixel );
    float zFar = lightRange * 2.0f; 
    compareDistance = distance / zFar;
    float distance01 = saturate( compareDistance );
    
    float depthCurve = distance01 * distance01; 
    
    fixedBias = lerp( 0.002f, 0.008f, depthCurve );

    float baseBlur = lerp( 0.02f, 0.08f, depthCurve );

    // The low-res static-only tier packs the same 90-deg cube face into 1/4 the texels per axis, so its
    // texels cover ~4x more world space at the same distance. Widen the bias/blur to match (mirrors D3D12's
    // `coarse` factor in PBRLighting.hlsl's SamplePointShadow) - otherwise the kernel tuned for the full-res
    // array is far narrower than that tier's actual texel footprint and the PCF barely blurs anything,
    // leaving the low-res cube's texel edges visibly blocky.
    float tierScale = tierLow ? 4.0f : 1.0f;
    fixedBias *= tierScale;
    baseBlur *= tierScale;

    // Texel-matching alone (tierScale) keeps the SAME relative footprint as the full-res tier, but at 32^2
    // only a handful of texels span the whole penumbra, so a texel-proportional blur still steps visibly.
    // Widen the low tier further (blur only - not bias, to avoid light leaking through casters) so the PCF
    // kernel spans several static-tier texels and the boundary reads as a soft gradient instead of blocks.
    const float tierLowBlurBoost = 2.5f;
    if ( tierLow )
        baseBlur *= tierLowBlurBoost;

    float noise = PLS_AggressiveNoise(wsPosition * 50.0f);
    fixedBlurScale = baseBlur * lerp(0.5f, 1.5f, noise);

    up = abs( dir.y ) < 0.999f ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    right = normalize( cross( up, dir ) );
    up = cross( dir, right );

    float angle = noise * 6.2831853f;
    sincos( angle, sinA, cosA );
}

float PLS_SampleShadowCube(
    TextureCube shadowCube,
    SamplerComparisonState samplerState,
    float3 wsPosition,
    float3 N, 
    float3 lightPosWorld,
    float lightRange )
{
    float3 dir;
    float compareDistance;
    float fixedBias;
    float fixedBlurScale;
    float3 right;
    float3 up;
    float sinA;
    float cosA;

    PLS_PrepareShadowSampling(
        wsPosition, N, lightPosWorld, lightRange, false,
        dir, compareDistance, fixedBias, fixedBlurScale,
        right, up, sinA, cosA );

    float shd = 0;
    [unroll] for ( int i = 0; i < PLS_SHADOW_BLUR_COUNT; i++ )
    {
        float2 kernel = PLS_SHADOW_BLUR_OFFSETS[i];
        float2 rotatedKernel = float2( kernel.x * cosA - kernel.y * sinA, kernel.x * sinA + kernel.y * cosA );
        float3 perturbedDir = normalize( dir + (right * rotatedKernel.x + up * rotatedKernel.y) * fixedBlurScale );

        shd += shadowCube.SampleCmpLevelZero( samplerState, perturbedDir, compareDistance - fixedBias );
    }

    float finalShadow = shd / PLS_SHADOW_BLUR_COUNT;

    // Shadow Distance Fading
    // Calculate how far we are through the light's actual range (0.0 to 1.0)
    float distanceToLight = length(wsPosition - lightPosWorld);
    float normalizedDist = saturate(distanceToLight / lightRange);
    
    return PLS_ApplyShadowDistanceFade( finalShadow, normalizedDist );
}

float PLS_SampleShadowCubeArray(
    TextureCubeArray shadowCubeArray,
    TextureCubeArray dynShadowCubeArray,
    TextureCubeArray staticOnlyCubeArray,
    SamplerComparisonState samplerState,
    float3 wsPosition,
    float3 N,
    float3 lightPosWorld,
    float lightRange,
    int encodedIndex )
{
    int cubeIndex = encodedIndex & PLS_SHADOW_SLOT_MASK;
    bool tierLow = ( encodedIndex & PLS_SHADOW_TIER_LOW ) != 0;
    bool hasDyn = !tierLow && ( encodedIndex & PLS_SHADOW_HAS_DYNAMIC ) != 0;

    float3 dir;
    float compareDistance;
    float fixedBias;
    float fixedBlurScale;
    float3 right;
    float3 up;
    float sinA;
    float cosA;

    PLS_PrepareShadowSampling(
        wsPosition, N, lightPosWorld, lightRange, tierLow,
        dir, compareDistance, fixedBias, fixedBlurScale,
        right, up, sinA, cosA );

    float shd = 0;
    int tapCount = tierLow ? PLS_SHADOW_BLUR_TIER_LOW_COUNT : PLS_SHADOW_BLUR_COUNT;

    [unroll] for ( int i = 0; i < PLS_SHADOW_BLUR_TIER_LOW_COUNT; i++ )
    {
        if ( i >= tapCount )
            break;

        // '& 7' keeps this a valid compile-time-constant index into the 8-element array even though the
        // [unroll] loop runs i up to 15 - the else branch is only ever taken (via tapCount/break above)
        // for i < 8, so the wrap is never actually observed, just required for FXC to accept the index.
        float2 kernel = tierLow ? PLS_SHADOW_BLUR_OFFSETS_TIER_LOW[i] : PLS_SHADOW_BLUR_OFFSETS[i & 7];
        float2 rotatedKernel = float2( kernel.x * cosA - kernel.y * sinA, kernel.x * sinA + kernel.y * cosA );
        float3 perturbedDir = normalize( dir + (right * rotatedKernel.x + up * rotatedKernel.y) * fixedBlurScale );
        float4 sampleCoord = float4( perturbedDir, (float)cubeIndex );

        float s;
        if ( tierLow )
        {
            s = staticOnlyCubeArray.SampleCmpLevelZero( samplerState, sampleCoord, compareDistance - fixedBias );
        }
        else
        {
            // min() of the static and moving-caster samples reproduces the old composited cube without
            // needing to re-copy the static depth into it on every dynamic update.
            s = shadowCubeArray.SampleCmpLevelZero( samplerState, sampleCoord, compareDistance - fixedBias );
            if ( hasDyn )
            {
                s = min( s, dynShadowCubeArray.SampleCmpLevelZero( samplerState, sampleCoord, compareDistance - fixedBias ) );
            }
        }
        shd += s;
    }

    float finalShadow = shd / (float)tapCount;

    // Shadow Distance Fading
    float distanceToLight = length(wsPosition - lightPosWorld);
    float normalizedDist = saturate(distanceToLight / lightRange);

    return PLS_ApplyShadowDistanceFade( finalShadow, normalizedDist );
}

#endif // !defined(__cplusplus)

#endif // POINT_LIGHT_SHADOWS_H