// Sun shadow for blended surfaces, which the ray-traced mask (traced for the opaque prepass) does not cover:
// one hard, alpha-tested ray within RayTracedSunShadowDistance, cross-faded into the CSM like the mask is.
// RT_SUN_INLINE is defined only for D3D12 with ray queries. Needs ShadowCB, FogCB's CamPosWS and
// PBRLighting.hlsl's ComputeSunShadow; s3 is a linear wrap sampler for the alpha test.
#ifndef D3D12_RTSUNINLINE_HLSL
#define D3D12_RTSUNINLINE_HLSL

#if RT_SUN_INLINE
SamplerState smpRtWrap : register( s3 );
#define RT_SCENE_HEAP_BASE ( RtSunSceneIndex - 1u )
#define RT_SCENE_SAMPLER smpRtWrap
#include "RtScene.hlsl"
#undef RT_SCENE_HEAP_BASE
#undef RT_SCENE_SAMPLER

static const float kRtSunRayLength = 60000.0;   // RtShadows.hlsl's kSunRayLength
static const float kRtSunPixelAngle = 0.001;    // about one pixel at 1080p; only picks the alpha mip and the offset

float ComputeSunShadowTraced( float3 wpos, float3 N, float vertLighting )
{
    float3 toCam = CamPosWS - wpos;
    float viewDist = length( toCam );
    [branch] if ( RtSunSceneIndex == 0u || SunIntensity <= 0.0 || viewDist >= RtSunDistance )
        return ComputeSunShadow( wpos, N, vertLighting );

    // Offset off the side the camera sees, as the mask does with its depth normal
    float3 side = dot( N, toCam ) < 0.0 ? -N : N;
    float footprint = viewDist * kRtSunPixelAngle;
    RayDesc ray;
    ray.Origin = wpos + side * ( 0.5 + footprint * 2.0 );
    ray.Direction = SunDirWS;
    ray.TMin = 0.0;
    ray.TMax = kRtSunRayLength;
    float vis = TraceVisibility( ray, 0xFF, true, max( footprint, 0.5 ) );

    float weight = saturate( ( RtSunDistance - viewDist ) / RtSunFadeBand );
    [branch] if ( weight >= 1.0 ) return vis;
    return lerp( ComputeSunShadow( wpos, N, vertLighting ), vis, weight );
}
#else
float ComputeSunShadowTraced( float3 wpos, float3 N, float vertLighting )
{
    return ComputeSunShadow( wpos, N, vertLighting );
}
#endif

#endif // D3D12_RTSUNINLINE_HLSL
