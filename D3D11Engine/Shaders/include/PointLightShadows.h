//--------------------------------------------------------------------------------------
// PointLightShadows.h - Shared pointlight shadow and lighting helpers
//--------------------------------------------------------------------------------------
#ifndef POINT_LIGHT_SHADOWS_H
#define POINT_LIGHT_SHADOWS_H

#if !defined(__cplusplus)

// TiledPointLight::ShadowCubeIndex encoding, HI-LO with 0 meaning invalid in each half:
//   LO 16 bits = STATIC (core) cube slot + 1     HI 16 bits = DYNAMIC (overlay) cube slot + 1
// So 0 overall is "unshadowed", and each half is decoded by subtracting 1. Mirrors
// PointLightSlotSelector::EncodeIndex, which both backends encode with.
static const int PLS_SHADOW_SLOT_SHIFT = 16;
static const int PLS_SHADOW_SLOT_MASK = 0xFFFF;

// The static tier packs the same 90-degree face into half the texels per axis of the overlay tier, so its
// bias and PCF radius are scaled by this. Too small shows up as acne on flat walls.
static const float PLS_STATIC_TIER_COARSE = 2.0f;

// Near plane of the cube's 90-degree face projection - must match D3D11PointLight::BeginCubeRender
// (D3D12: D3D12PointShadows' PerspectiveFovLH). zFar is the light's ShadowRange * 2.
static const float PLS_SHADOW_ZNEAR = 15.0f;

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

// Denser ring for the static tier, whose texels cover more world space per tap - see PLS_STATIC_TIER_COARSE.
// Same disk radius as the 8-tap ring above, just more taps across the tier's larger texels.
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
    float coarse,
    bool taaActive,
    out float3 dir,
    out float compareDepth,
    out float fixedBias,
    out float fixedBlurScale,
    out float3 right,
    out float3 up,
    out float sinA,
    out float cosA )
{
    // Uniform world-space normal offset, exactly as D3D12's SamplePointShadow applies it. A slope-scaled,
    // distance-proportional offset used to live here; it was tuned against direction-INDEPENDENT radial
    // depth and has no counterpart in the backend this now mirrors.
    float3 toPixel = (wsPosition + N * (lightRange * 0.01f * coarse)) - lightPosWorld;
    dir = normalize( toPixel );

    // The caster writes no SV_Depth, so the cube holds the natural hyperbolic z of its 90-degree face
    // projection. Depth on a cube face is driven by the DOMINANT-AXIS distance (that face's view-space z),
    // so reconstruct it from that and re-apply the same LH projection z-map - not from the radial length.
    // Clamped at the near plane: geometry inside it was never rasterized into the cube, so it has to read
    // as lit, and a negative compare depth would instead fail every comparison.
    float zFar = lightRange * 2.0f;
    float3 axisDist = abs( toPixel );
    float zView = max( max( axisDist.x, max( axisDist.y, axisDist.z ) ), PLS_SHADOW_ZNEAR );
    compareDepth = (zFar / (zFar - PLS_SHADOW_ZNEAR)) * (1.0f - PLS_SHADOW_ZNEAR / zView);

    // Bias flat in hyperbolic depth (so it widens in world units with distance, which is what the
    // coarsening D16 quantisation there needs) and an angular disk that only creeps wider with depth -
    // both D3D12's values. The disk USED to reach 0.256 rad, ~40 texels of a 90-degree face: harmless
    // against radial depth, but the correct compare value now varies per tap direction, so a disk that
    // wide compares taps against badly wrong depths and makes the terminator follow the cube faces.
    // `coarse` is this tier's texel footprint relative to the overlay's (1 = same); the caller divides
    // both back down by it for the finer overlay sample.
    fixedBias = 0.001f * coarse;

    float baseBlur = (0.006f + 0.010f * saturate( zView / zFar )) * coarse;

    // The rotation/blur-scale jitter below is a spatial hash of wsPosition, not a temporal one - it exists
    // to break up the Poisson ring into dither that TAA/FSR resolves into smooth soft shadows over several
    // frames. A hash is discontinuous: without TAA to average it out, the sub-pixel shift in reconstructed
    // wsPosition from ordinary camera motion flips the hash output unpredictably frame to frame, which reads
    // as flicker/sparkle rather than dither. Mirrors GetPoissonRotationSCForCascade's SQ_FrameIndex==0 guard
    // in ShadowSampling.h - fall back to a fixed rotation/scale (still temporally stable, just static-banded)
    // when the caller reports no camera jitter is active.
    if ( taaActive )
    {
        float noise = PLS_AggressiveNoise(wsPosition * 50.0f);
        fixedBlurScale = baseBlur * lerp(0.5f, 1.5f, noise);

        float angle = noise * 6.2831853f;
        sincos( angle, sinA, cosA );
    }
    else
    {
        fixedBlurScale = baseBlur;
        sinA = 0.0f;
        cosA = 1.0f;
    }

    up = abs( dir.y ) < 0.999f ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    right = normalize( cross( up, dir ) );
    up = cross( dir, right );
}

float PLS_SampleShadowCube(
    TextureCube shadowCube,
    SamplerComparisonState samplerState,
    float3 wsPosition,
    float3 N,
    float3 lightPosWorld,
    float lightRange,
    bool taaActive )
{
    float3 dir;
    float compareDepth;
    float fixedBias;
    float fixedBlurScale;
    float3 right;
    float3 up;
    float sinA;
    float cosA;

    PLS_PrepareShadowSampling(
        wsPosition, N, lightPosWorld, lightRange, 1.0f, taaActive,
        dir, compareDepth, fixedBias, fixedBlurScale,
        right, up, sinA, cosA );

    float shd = 0;
    [unroll] for ( int i = 0; i < PLS_SHADOW_BLUR_COUNT; i++ )
    {
        float2 kernel = PLS_SHADOW_BLUR_OFFSETS[i];
        float2 rotatedKernel = float2( kernel.x * cosA - kernel.y * sinA, kernel.x * sinA + kernel.y * cosA );
        float3 perturbedDir = normalize( dir + (right * rotatedKernel.x + up * rotatedKernel.y) * fixedBlurScale );

        shd += shadowCube.SampleCmpLevelZero( samplerState, perturbedDir, compareDepth - fixedBias );
    }

    float finalShadow = shd / PLS_SHADOW_BLUR_COUNT;

    // Shadow Distance Fading
    // Calculate how far we are through the light's actual range (0.0 to 1.0)
    float distanceToLight = length(wsPosition - lightPosWorld);
    float normalizedDist = saturate(distanceToLight / lightRange);

    return PLS_ApplyShadowDistanceFade( finalShadow, normalizedDist );
}

// Two independent tiers, addressed by the HI-LO halves of `encodedIndex`. min() of the two comparisons is
// "occluded by either"; a light with no overlay has a zero HI half and takes no second sample at all.
float PLS_SampleShadowCubeArray(
    TextureCubeArray staticCubeArray,
    TextureCubeArray dynShadowCubeArray,
    SamplerComparisonState samplerState,
    float3 wsPosition,
    float3 N,
    float3 lightPosWorld,
    float lightRange,
    int encodedIndex,
    bool taaActive )
{
    int staticSlot = ( encodedIndex & PLS_SHADOW_SLOT_MASK ) - 1;
    int dynSlot = ( ( encodedIndex >> PLS_SHADOW_SLOT_SHIFT ) & PLS_SHADOW_SLOT_MASK ) - 1;
    if ( staticSlot < 0 )
        return 1.0f;

    float3 dir;
    float compareDepth;
    float fixedBias;
    float fixedBlurScale;
    float3 right;
    float3 up;
    float sinA;
    float cosA;

    // Prepared against the STATIC tier, the sample every shadowed light takes; the finer overlay scales back
    // down by the same factor.
    PLS_PrepareShadowSampling(
        wsPosition, N, lightPosWorld, lightRange, PLS_STATIC_TIER_COARSE, taaActive,
        dir, compareDepth, fixedBias, fixedBlurScale,
        right, up, sinA, cosA );

    const float dynBias = fixedBias / PLS_STATIC_TIER_COARSE;
    const float dynBlur = fixedBlurScale / PLS_STATIC_TIER_COARSE;
    const bool hasDyn = dynSlot >= 0;

    float shd = 0;
    [unroll] for ( int i = 0; i < PLS_SHADOW_BLUR_TIER_LOW_COUNT; i++ )
    {
        float2 kernel = PLS_SHADOW_BLUR_OFFSETS_TIER_LOW[i];
        float2 rotatedKernel = float2( kernel.x * cosA - kernel.y * sinA, kernel.x * sinA + kernel.y * cosA );
        float3 offset = right * rotatedKernel.x + up * rotatedKernel.y;

        float3 perturbedDir = normalize( dir + offset * fixedBlurScale );
        float s = staticCubeArray.SampleCmpLevelZero( samplerState,
            float4( perturbedDir, (float)staticSlot ), compareDepth - fixedBias );

        if ( hasDyn )
        {
            float3 dynDir = normalize( dir + offset * dynBlur );
            s = min( s, dynShadowCubeArray.SampleCmpLevelZero( samplerState,
                float4( dynDir, (float)dynSlot ), compareDepth - dynBias ) );
        }
        shd += s;
    }

    float finalShadow = shd / (float)PLS_SHADOW_BLUR_TIER_LOW_COUNT;

    // Shadow Distance Fading
    float distanceToLight = length(wsPosition - lightPosWorld);
    float normalizedDist = saturate(distanceToLight / lightRange);

    return PLS_ApplyShadowDistanceFade( finalShadow, normalizedDist );
}

#endif // !defined(__cplusplus)

#endif // POINT_LIGHT_SHADOWS_H