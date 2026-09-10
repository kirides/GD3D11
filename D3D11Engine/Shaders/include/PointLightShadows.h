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

// PCF disk radius in cube-face tangent units per `coarse`, plus the part that grows toward zFar. At the static
// tier's coarse 2 that spans ~3-4 of its 64^2 texels: a deliberately soft penumbra over fine detail.
static const float PLS_SHADOW_SOFTNESS_NEAR = 0.045f;
static const float PLS_SHADOW_SOFTNESS_FAR = 0.03f;

// Lowest fraction of the receiver's own depth the tangent plane may pull a tap to; caps the light leak a
// normal-mapped normal can cause by tilting that plane.
static const float PLS_RECEIVER_PLANE_MIN_SCALE = 0.5f;

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
    out float3 toPixel,
    out float planeNd,
    out float3 dir,
    out float fixedBias,
    out float fixedBlurScale,
    out float3 right,
    out float3 up,
    out float sinA,
    out float cosA )
{
    // Uniform world-space normal offset, as D3D12's SamplePointShadow applies it.
    toPixel = (wsPosition + N * (lightRange * 0.01f * coarse)) - lightPosWorld;
    dir = normalize( toPixel );

    // Tangent-plane numerator for PLS_TapCompareDepth; a receiver facing away gets no plane to lower toward.
    float nd = dot( toPixel, N );
    planeNd = nd < 0.0f ? nd : -1e20f;

    // Flat in hyperbolic depth, so it widens in world units where D16 quantisation coarsens. `coarse` is this
    // tier's texel footprint relative to the overlay's; the caller divides bias and disk back down for it.
    fixedBias = 0.001f * coarse;

    float zFar = lightRange * 2.0f;
    float3 axisDist = abs( toPixel );
    float zView = max( axisDist.x, max( axisDist.y, axisDist.z ) );
    float baseBlur = (PLS_SHADOW_SOFTNESS_NEAR + PLS_SHADOW_SOFTNESS_FAR * saturate( zView / zFar )) * coarse;

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

// Hyperbolic compare depth for one PCF tap: the receiver's own z in the cube face `tapDir` selects (so a wide
// disk crossing a seam stays correct), lowered to the tangent-plane hit when nearer (so grazing receivers don't
// self-shadow). Clamped at the near plane, inside which nothing was rasterized.
float PLS_TapCompareDepth( float3 toPixel, float3 N, float planeNd, float3 tapDir, float lightRange )
{
    float3 a = abs( tapDir );
    float3 face = ( a.x >= a.y && a.x >= a.z ) ? float3( 1, 0, 0 ) : ( ( a.y >= a.z ) ? float3( 0, 1, 0 ) : float3( 0, 0, 1 ) );
    float zOwn = dot( abs( toPixel ), face );
    float zPlane = planeNd / min( dot( tapDir, N ), -1e-4f ) * dot( a, face );
    float zView = max( max( min( zPlane, zOwn ), zOwn * PLS_RECEIVER_PLANE_MIN_SCALE ), PLS_SHADOW_ZNEAR );

    float zFar = lightRange * 2.0f;
    return (zFar / (zFar - PLS_SHADOW_ZNEAR)) * (1.0f - PLS_SHADOW_ZNEAR / zView);
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
    float3 toPixel;
    float planeNd;
    float3 dir;
    float fixedBias;
    float fixedBlurScale;
    float3 right;
    float3 up;
    float sinA;
    float cosA;

    PLS_PrepareShadowSampling(
        wsPosition, N, lightPosWorld, lightRange, 1.0f, taaActive,
        toPixel, planeNd, dir, fixedBias, fixedBlurScale,
        right, up, sinA, cosA );

    float shd = 0;
    [unroll] for ( int i = 0; i < PLS_SHADOW_BLUR_COUNT; i++ )
    {
        float2 kernel = PLS_SHADOW_BLUR_OFFSETS[i];
        float2 rotatedKernel = float2( kernel.x * cosA - kernel.y * sinA, kernel.x * sinA + kernel.y * cosA );
        float3 perturbedDir = normalize( dir + (right * rotatedKernel.x + up * rotatedKernel.y) * fixedBlurScale );

        float compareDepth = PLS_TapCompareDepth( toPixel, N, planeNd, perturbedDir, lightRange );
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

    float3 toPixel;
    float planeNd;
    float3 dir;
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
        toPixel, planeNd, dir, fixedBias, fixedBlurScale,
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
        float s = staticCubeArray.SampleCmpLevelZero( samplerState, float4( perturbedDir, (float)staticSlot ),
            PLS_TapCompareDepth( toPixel, N, planeNd, perturbedDir, lightRange ) - fixedBias );

        if ( hasDyn )
        {
            float3 dynDir = normalize( dir + offset * dynBlur );
            s = min( s, dynShadowCubeArray.SampleCmpLevelZero( samplerState, float4( dynDir, (float)dynSlot ),
                PLS_TapCompareDepth( toPixel, N, planeNd, dynDir, lightRange ) - dynBias ) );
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