#pragma once

// Scene wetness for the D3D12 Forward+ lit passes (World/Vob/Skeletal.hlsl): D3D11's deferred wet surface
// (Shaders/include/RainWetnessSample.h, included by PBRLighting.hlsl) evaluated per fragment in world space.
// Needs ShadowCB's wetness and wet-sky blocks, FogCB's CamPosWS, and the samplers `smp` (wrap) and `shadowCmp`.

// Rain-reach Poisson disk: the first 16 entries of ShadowSampling.h's g_PoissonDisk32; the first 8 are the inner ring.
static const float2 g_RainPoisson16[16] = {
    float2( -0.94201624, -0.39906216 ), float2(  0.94558609, -0.76890725 ),
    float2( -0.09418410, -0.92938870 ), float2(  0.34495938,  0.29387760 ),
    float2( -0.91588581,  0.45771432 ), float2( -0.81544232, -0.87912464 ),
    float2( -0.38277543,  0.27676845 ), float2(  0.97484398,  0.75648379 ),
    float2(  0.44323325, -0.97511554 ), float2(  0.53742981, -0.47373420 ),
    float2( -0.26496911, -0.41893023 ), float2(  0.79197514,  0.19090188 ),
    float2( -0.24188840,  0.99706507 ), float2( -0.81409955,  0.91437590 ),
    float2(  0.19984126,  0.78641367 ), float2(  0.14383161, -0.14100790 )
};

// 1 where rain reaches wpos, 0 under cover; 1:1 with D3D11's ComputeRainWetness (ShadowSampling.h). The kernel is
// sized in world units because this rain map spans 3x D3D11's. Outside the rain camera: assume open sky.
float SampleRainReach( float3 wpos )
{
    Texture2D rainMap = ResourceDescriptorHeap[RainShadowIndex];

    float4 clip = mul( float4( wpos, 1.0 ), RainViewProj );
    clip.xyz /= clip.w;                                       // no-op for the ortho cast; kept for parity with IsWet()
    float2 uv = clip.xy * float2( 0.5, -0.5 ) + 0.5;
    if ( any( uv < 0.0 ) || any( uv > 1.0 ) || clip.z < 0.0 || clip.z > 1.0 )
        return 1.0;

    // World -> clip scale per axis (row-vector mul, so the columns of RainViewProj).
    float sx = length( float3( RainViewProj._11, RainViewProj._21, RainViewProj._31 ) );
    float sy = length( float3( RainViewProj._12, RainViewProj._22, RainViewProj._32 ) );
    float sz = length( float3( RainViewProj._13, RainViewProj._23, RainViewProj._33 ) );

    float2 radiusUV = float2( sx, sy ) * ( 0.5 * RAIN_WET_BLUR_WORLD );
    float zReceiver = clip.z - ( 0.0001 + RAIN_WET_BLUR_WORLD * sz );

    // Gaussian weights, sigma = 0.5 * radius: exp(-r^2 / (2*sigma^2)).
    const float wCenter = 1.0;
    const float rInner  = 0.45;
    const float wInner  = 0.66698;   // exp(-0.45^2 * 2)
    const float wOuter  = 0.13534;   // exp(-1.00^2 * 2)

    float sum = wCenter * rainMap.SampleCmpLevelZero( shadowCmp, uv, zReceiver );
    float weight = wCenter;

    [unroll] for ( int i = 0; i < 8; ++i )
    {
        sum += wInner * rainMap.SampleCmpLevelZero( shadowCmp, uv + g_RainPoisson16[i] * ( rInner * radiusUV ), zReceiver );
        weight += wInner;
    }
    [unroll] for ( int j = 0; j < 16; ++j )
    {
        sum += wOuter * rainMap.SampleCmpLevelZero( shadowCmp, uv + g_RainPoisson16[j] * radiusUV, zReceiver );
        weight += wOuter;
    }

    return smoothstep( 0.0, 1.0, saturate( sum / weight ) );
}

// Wet surface at one fragment. N becomes the rippled diffuse normal, albedo darkens, and roughness dips toward water,
// which is what the sky IBL and opaque SSR reflect with. geomN = the normalized interpolated vertex normal.
WetSurface ApplySceneWetness( float3 wpos, float3 geomN, inout float3 N, inout float3 albedo, inout float roughness )
{
    WetSurface wet = (WetSurface)0;
    wet.roughness = WET_FILM_ROUGHNESS;
    wet.rippleN = N;
    wet.coatN = N;
    if ( SceneWetness <= 0.0 || RainShadowIndex == 0xffffffff || DistortionIndex == 0xffffffff )
        return wet;   // not raining / dried, or the rain map / distortion2.dds never loaded

    Texture2D distTex = ResourceDescriptorHeap[DistortionIndex];
    wet = EvaluateWetSurface( SampleRainReach( wpos ) * SceneWetness, N, geomN, wpos, distance( wpos, CamPosWS ),
        distTex, smp, RainTime, RainFxWeight );
    if ( wet.wetness <= 0.0 ) return wet;

    N = wet.rippleN;
    roughness = lerp( roughness, lerp( 0.10, 0.02, wet.puddle ), wet.wetness );
    ApplyWetAlbedo( albedo, wet );
    return wet;
}

// Water film over the sun/ambient term: sun and moon streaks, plus D3D11's fog-tinted sky Fresnel where no sky IBL
// already reflects the sky. Not gated by SunSpecularEnabled, which toggles material highlights only.
float3 ApplyWetCoat( float3 rgb, WetSurface wet, float3 wpos, float shadow, float vertLighting, float ao, float ssao )
{
    [branch]
    if ( wet.wetness <= 0.0 ) return rgb;

    float3 V = normalize( CamPosWS - wpos );
    float nightBlend = saturate( -WetSunHeight * 4.0 );
    float skyOcclusion = lerp( 1.0, vertLighting, WorldAOStrength ) * ao * ssao;

    // SunIntensity is 0 below the horizon and indoors; the 0.8 is D3D11's wet dimming of the sun colour.
    float3 sun = SrgbToLinear( SunColor ) * ( SunIntensity * lerp( 1.0, 0.8, wet.wetness )
               * WetCoatSpecular( wet.coatN, V, SunDirWS, WET_SUN_DISTANCE, wet.roughness ) * shadow );
    float3 moon = SrgbToLinear( WET_MOON_COLOR ) * ( WetCoatSpecular( wet.coatN, V, WetMoonDir, WET_MOON_DISTANCE, wet.roughness )
                * WetMoonFade * nightBlend * skyOcclusion );
    float3 wetLight = WetCoatRolloff( ( sun + moon ) * ( WetLightReflections * wet.wetness ) );

    [branch]
    if ( SkySpecularIndex == 0xFFFFFFFFu )
    {
        float3 sky = SrgbToLinear( WetSkyReflectionColor( WetSkyTint, WetSunHeight ) );
        rgb = lerp( rgb, sky * skyOcclusion, WetSkyFresnel( wet.coatN, V ) * wet.wetness );
    }
    return rgb + wetLight;
}
