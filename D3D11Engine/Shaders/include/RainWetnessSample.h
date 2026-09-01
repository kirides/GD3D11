// Lightweight rain-wetness sampling for the legacy deferred point-light passes
// (PS_DS_PointLight.hlsl / PS_DS_PointLightDynShadow.hlsl).
//
// BUG THIS FIXES: PS_DS_AtmosphericScattering.hlsl computes scene wetness (ApplySceneWettness) and
// darkens/desaturates its own LOCAL copy of the G-buffer diffuse -- that darkening is never written
// back to the G-buffer. The point-light passes below read the G-buffer's diffuse/specular directly and
// additively blend into the same HDR target the atmospheric-scattering pass just wrote, so a pixel that
// sits close to a torch/lantern gets its wet darkening overpowered by an un-wetted point-light
// contribution added right on top -- wet ground reads as completely dry wherever a nearby point light
// dominates the pixel's brightness (reported by the maintainer: "no rain wetness effect if light
// touches the ground, only unlit areas get the rain effect").
//
// This header gives the point-light passes their own, cheaper wetness sample so their contribution is
// darkened/dampened consistently with the atmospheric-scattering pass instead of ignoring wetness
// entirely.
#ifndef RAIN_WETNESS_SAMPLE_H
#define RAIN_WETNESS_SAMPLE_H

// Inner 8-tap ring of ShadowSampling.h's g_PoissonDisk32, duplicated rather than pulled in via
// #include "ShadowSampling.h": that header also declares CSM-only globals (TX_ShadowmapArray /
// TX_ShadowmapAtlas, SHADOW_ATLAS) the point-light shaders have no reason to bind. Same duplication
// precedent as the D3D12 backend's Wetness.hlsl.
static const float2 g_RainPoissonInner8[8] = {
    float2( -0.94201624, -0.39906216 ), float2(  0.94558609, -0.76890725 ),
    float2( -0.09418410, -0.92938870 ), float2(  0.34495938,  0.29387760 ),
    float2( -0.91588581,  0.45771432 ), float2( -0.81544232, -0.87912464 ),
    float2( -0.38277543,  0.27676845 ), float2(  0.97484398,  0.75648379 )
};

#ifndef RAIN_WET_BLUR_WORLD
#define RAIN_WET_BLUR_WORLD 100.0f   // ~1m filter radius => ~2m wide wet/dry transition, matches ShadowSampling.h
#endif

// 1:1 with ShadowSampling.h's ComputeRainWetness, minus its optional 16-tap outer ring (SHD_FILTER_16TAP_PCF):
// this runs once per pixel PER OVERLAPPING LIGHT rather than once per pixel, so it deliberately stays cheap.
float ComputeRainWetnessLite( float3 wsPosition, Texture2D rainMap, SamplerComparisonState samplerState, matrix viewProj )
{
    float4 sp = mul( float4( wsPosition, 1 ), viewProj );
    sp.xyz /= sp.www;   // orthographic rain camera, w == 1 -- kept for parity with ComputeRainWetness
    float2 uv = sp.xy * float2( 0.5f, -0.5f ) + float2( 0.5f, 0.5f );

    // Outside the rain camera there is no occluder information; the common case out there is open sky,
    // so return "exposed" rather than drawing a dry ring at the map border.
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

// Darkens/desaturates diffuse and dampens specular for a wet surface -- the point-light-pass analogue of
// PS_DS_AtmosphericScattering.hlsl's ApplySceneWettness. Deliberately skips that function's tri-planar
// ripple normal deformation and reflection-cube sheen (an extra texture + per-axis blend that would be
// paid once per overlapping light instead of once per pixel); the diffuse darkening below is what
// actually reads as "wet" and is the part this bug report is about.
//
// `wsNormal` must be the UNDEFORMED world-space surface normal (no ripple applied here, so nothing to
// deform it with) and `sceneWetness` is GothicAPI::GetSceneWetness() (the sustained wetness level, same
// split as AC_SceneWettness/AC_RainFXWeight elsewhere).
void ApplyPointLightWetness( float3 wsPosition, float3 wsNormal, Texture2D rainMap,
    SamplerComparisonState samplerState, matrix rainViewProj, float sceneWetness,
    inout float3 diffuse, inout float specIntensity, inout float specPower )
{
    if ( sceneWetness <= 0.0f ) return;

    float wetness = ComputeRainWetnessLite( wsPosition, rainMap, samplerState, rainViewProj ) * sceneWetness;
    if ( wetness < 0.001f ) return;

    // Rain mostly settles on upward-facing, unsheltered surfaces -- same exposure test as
    // ApplySceneWettness (undeformed normal here, since there is no ripple to deform it with).
    float wDot = saturate( dot( wsNormal, float3( 0, -1, 0 ) ) );
    float wDot2 = wDot * wDot;
    wetness *= 1.0f - ( wDot2 * wDot2 );
    float exposure = saturate( wsNormal.y );
    wetness *= exposure * exposure;
    if ( wetness <= 0.0f ) return;

    specIntensity = lerp( specIntensity, 0.0f, wetness );
    specPower = lerp( specPower, 150.0f, wetness );

    float diffuseLum = dot( diffuse, float3( 0.3333f, 0.3333f, 0.3333f ) );
    float3 wetDiffuse = lerp( diffuseLum, diffuse, 0.75f ) * 0.75f;   // desaturate + darken, matches ApplySceneWettness's wetPixel
    diffuse = lerp( diffuse, wetDiffuse, wetness );
}

#endif // RAIN_WETNESS_SAMPLE_H
