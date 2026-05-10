//--------------------------------------------------------------------------------------
// Depth of Field - Focus Resolve pass
// Samples depth in a disc around screen center and temporally smooths the result
// Renders to a 1x1 R32_FLOAT target storing the linearized focus distance
//--------------------------------------------------------------------------------------

#include "DepthReconstruction.h"

cbuffer DepthOfFieldConstantBuffer : register( b0 )
{
    float DoF_FocusDistance;
    float DoF_FocusRange;
    float DoF_BokehRadius;
    float DoF_MaxBlur;

    float4 DoF_ProjParams;
    float DoF_NearPlane;
    float DoF_FarPlane;
    float DoF_Pad;
    float DoF_Pad2;
};

SamplerState SS_Linear : register( s0 );
Texture2D TX_Depth : register( t0 );
Texture2D TX_PrevFocus : register( t1 ); // 1x1 R32_FLOAT from previous frame

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float LinearizeDepth( float d )
{
    return LinearizeDepthReverseZInfinite( d );
}

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    // Sample depth across a center-weighted disc (~12% of screen)
    // Weighted toward center for "what are you looking at" semantic
    static const float2 offsets[13] = {
        float2( 0.000,  0.000),
        float2(-0.035,  0.000), float2( 0.035,  0.000),
        float2( 0.000, -0.035), float2( 0.000,  0.035),
        float2(-0.025, -0.025), float2( 0.025, -0.025),
        float2(-0.025,  0.025), float2( 0.025,  0.025),
        float2(-0.060,  0.000), float2( 0.060,  0.000),
        float2( 0.000, -0.060), float2( 0.000,  0.060),
    };

    static const float weights[13] = {
        0.20,
        0.08, 0.08,
        0.08, 0.08,
        0.06, 0.06,
        0.06, 0.06,
        0.04, 0.04,
        0.04, 0.04,
    };

    float targetDepth = 0.0;
    float totalWeight = 0.0;
    for ( int i = 0; i < 13; i++ )
    {
        float2 uv = float2( 0.5, 0.5 ) + offsets[i];
        float d = TX_Depth.SampleLevel( SS_Linear, uv, 0 ).r;
        float linearD = LinearizeDepth( d );
        targetDepth += linearD * weights[i];
        totalWeight += weights[i];
    }
    targetDepth /= totalWeight;

    float prevFocus = TX_PrevFocus.SampleLevel( SS_Linear, float2( 0.5, 0.5 ), 0 ).r;

    // First frame: no previous data
    if ( prevFocus <= 0.0 )
        return float4( targetDepth, 0, 0, 1 );

    // Adaptive smoothing: large focus jumps converge faster to avoid sluggish response,
    // small changes (view bobbing) are heavily damped for stability
    float relDiff = abs( targetDepth - prevFocus ) / max( prevFocus, 1.0 );
    float smoothing = lerp( 0.015, 0.20, saturate( relDiff * 2.0 ) );

    float newFocus = lerp( prevFocus, targetDepth, smoothing );

    return float4( newFocus, 0, 0, 1 );
}
