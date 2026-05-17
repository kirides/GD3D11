//--------------------------------------------------------------------------------------
// PointLightShadows.h - Shared pointlight shadow and lighting helpers
//--------------------------------------------------------------------------------------
#ifndef POINT_LIGHT_SHADOWS_H
#define POINT_LIGHT_SHADOWS_H

#if !defined(__cplusplus)

static const int PLS_SHADOW_BLUR_COUNT = 8;
static const float2 PLS_SHADOW_BLUR_OFFSETS[PLS_SHADOW_BLUR_COUNT] = {
    float2( 0.500f, 0.000f ),
    float2( 0.000f, 0.500f ),
    float2( -0.500f, 0.000f ),
    float2( 0.000f, -0.500f ),
    float2( 0.924f, 0.383f ),
    float2( -0.383f, 0.924f ),
    float2( -0.924f, -0.383f ),
    float2( 0.383f, -0.924f ),
};

float PLS_Hash2D( float2 p )
{
    float3 p3 = frac( float3( p.xyx ) * 0.1031f );
    p3 += dot( p3, p3.yzx + 33.33f );
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
    float3 lightPosWorld,
    float lightRange,
    out float3 dir,
    out float compareDistance,
    out float fixedBias,
    out float fixedBlurScale,
    out float3 right,
    out float3 up,
    out float sinA,
    out float cosA )
{
    float3 toPixel = wsPosition - lightPosWorld;
    dir = normalize( toPixel );

    float distance = length( toPixel );
    float zFar = lightRange * 2.0f;
    compareDistance = distance / zFar;

    float distance01 = saturate( compareDistance );
    fixedBias = lerp( 0.006f, 0.009f, distance01 );
    fixedBlurScale = lerp( 0.034f, 0.050f, distance01 * distance01 );

    up = abs( dir.y ) < 0.999f ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    right = normalize( cross( up, dir ) );
    up = cross( dir, right );

    float angle = PLS_Hash2D( wsPosition.xz * 0.1f ) * 6.2831853f;
    sincos( angle, sinA, cosA );
}

float PLS_SampleShadowCube(
    TextureCube shadowCube,
    SamplerComparisonState samplerState,
    float3 wsPosition,
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
        wsPosition,
        lightPosWorld,
        lightRange,
        dir,
        compareDistance,
        fixedBias,
        fixedBlurScale,
        right,
        up,
        sinA,
        cosA );

    float shd = 0;
    [unroll] for ( int i = 0; i < PLS_SHADOW_BLUR_COUNT; i++ )
    {
        float2 kernel = PLS_SHADOW_BLUR_OFFSETS[i];
        float2 rotatedKernel = float2( kernel.x * cosA - kernel.y * sinA, kernel.x * sinA + kernel.y * cosA );
        float3 perturbedDir = normalize( dir + (right * rotatedKernel.x + up * rotatedKernel.y) * fixedBlurScale );

        shd += shadowCube.SampleCmpLevelZero( samplerState, perturbedDir, compareDistance - fixedBias );
    }

    return shd / PLS_SHADOW_BLUR_COUNT;
}

float PLS_SampleShadowCube(
    TextureCubeArray shadowCubeArray,
    SamplerComparisonState samplerState,
    float3 wsPosition,
    float3 lightPosWorld,
    float lightRange,
    int cubeIndex )
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
        wsPosition,
        lightPosWorld,
        lightRange,
        dir,
        compareDistance,
        fixedBias,
        fixedBlurScale,
        right,
        up,
        sinA,
        cosA );

    float shd = 0;
    [unroll] for ( int i = 0; i < PLS_SHADOW_BLUR_COUNT; i++ )
    {
        float2 kernel = PLS_SHADOW_BLUR_OFFSETS[i];
        float2 rotatedKernel = float2( kernel.x * cosA - kernel.y * sinA, kernel.x * sinA + kernel.y * cosA );
        float3 perturbedDir = normalize( dir + (right * rotatedKernel.x + up * rotatedKernel.y) * fixedBlurScale );
        float4 sampleCoord = float4( perturbedDir, (float)cubeIndex );

        shd += shadowCubeArray.SampleCmpLevelZero( samplerState, sampleCoord, compareDistance - fixedBias );
    }

    return shd / PLS_SHADOW_BLUR_COUNT;
}

#endif // !defined(__cplusplus)

#endif // POINT_LIGHT_SHADOWS_H
