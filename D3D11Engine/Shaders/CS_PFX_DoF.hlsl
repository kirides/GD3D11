//--------------------------------------------------------------------------------------
// Compute Shader - Depth of Field Half-res Bokeh Blur
// Samples full-res scene + depth, computes CoC, does 48-tap bokeh blur
// Outputs blurred color (rgb) + CoC (a) at half resolution to UAV
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
Texture2D TX_Scene : register( t0 );   // Full-res scene color
Texture2D TX_Depth : register( t1 );   // Full-res hardware depth
Texture2D TX_Focus : register( t2 );   // 1x1 R32_FLOAT smoothed focus depth

RWTexture2D<float4> OutputBlur : register( u0 ); // Half-res output

float LinearizeDepth( float d )
{
    return LinearizeDepthReverseZInfinite( d );
}

float ComputeCoC( float linearDepth, float focusDepth )
{
    return saturate( ( linearDepth - focusDepth ) / DoF_FocusRange );
}

static const int SAMPLE_COUNT = 48;

float2 GetSpiralSample( int index, int count )
{
    float r = sqrt( ( float(index) + 0.5 ) / float(count) );
    float theta = float(index) * 2.39996323;
    return float2( r * cos( theta ), r * sin( theta ) );
}

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outSize;
    OutputBlur.GetDimensions( outSize.x, outSize.y );

    if ( DTid.x >= outSize.x || DTid.y >= outSize.y )
        return;

    float2 texcoord = ( float2( DTid.xy ) + 0.5 ) / float2( outSize );

    // Texel size of the full-res scene for sampling offsets
    float2 sceneSize;
    TX_Scene.GetDimensions( sceneSize.x, sceneSize.y );
    float2 texelSize = 1.0 / sceneSize;

    float focusDepth = TX_Focus.SampleLevel( SS_Linear, float2( 0.5, 0.5 ), 0 ).r;

    float centerDepth = TX_Depth.SampleLevel( SS_Linear, texcoord, 0 ).r;
    float centerLinear = LinearizeDepth( centerDepth );
    float centerCoC = ComputeCoC( centerLinear, focusDepth );

    float3 centerColor = TX_Scene.SampleLevel( SS_Linear, texcoord, 0 ).rgb;

    // Early out: pass through sharp pixel
    if ( centerCoC < 0.01 )
    {
        OutputBlur[DTid.xy] = float4( centerColor, 0.0 );
        return;
    }

    float blurRadius = min( centerCoC * DoF_BokehRadius, DoF_MaxBlur );

#ifdef DOF_GAUSS_BLUR
    // --- Simple Gaussian blur (16 taps) ---
    // Uses a radial Gaussian kernel with exp(-r^2 * 3) weights.
    // Much cheaper than the bokeh path; no highlight boost or
    // foreground rejection — just a smooth, uniform blur.
    static const int GAUSS_SAMPLE_COUNT = 16;

    float3 colorAccum = 0.0;
    float weightAccum = 0.0;

    [unroll]
    for ( int i = 0; i < GAUSS_SAMPLE_COUNT; i++ )
    {
        float2 offset = GetSpiralSample( i, GAUSS_SAMPLE_COUNT );
        float2 sampleUV = texcoord + offset * blurRadius * texelSize;

        float3 sampleColor = TX_Scene.SampleLevel( SS_Linear, sampleUV, 0 ).rgb;

        float r2 = dot( offset, offset );
        float weight = exp( -r2 * 3.0 );

        colorAccum += sampleColor * weight;
        weightAccum += weight;
    }
#else
    // --- Bokeh spiral blur (48 taps) ---
    // Seed accumulator with the center pixel so that if all 48 spiral
    // samples are foreground-rejected (e.g. background visible through
    // a leaf gap), the result falls back to the center color instead
    // of producing black.  Weight uses the same luminance-boost formula
    // applied to every spiral sample.
    float centerLum = dot( centerColor, float3( 0.2126, 0.7152, 0.0722 ) );
    float3 colorAccum = centerColor * ( 1.0 + centerLum * 2.0 );
    float weightAccum = 1.0 + centerLum * 2.0;

    [unroll]
    for ( int i = 0; i < SAMPLE_COUNT; i++ )
    {
        float2 offset = GetSpiralSample( i, SAMPLE_COUNT );
        float2 sampleUV = texcoord + offset * blurRadius * texelSize;

        float3 sampleColor = TX_Scene.SampleLevel( SS_Linear, sampleUV, 0 ).rgb;
        float sampleDepth = TX_Depth.SampleLevel( SS_Linear, sampleUV, 0 ).r;
        float sampleLinear = LinearizeDepth( sampleDepth );
        float sampleCoC = ComputeCoC( sampleLinear, focusDepth );

        float weight = ( sampleCoC >= length( offset ) * centerCoC ) ? 1.0 : sampleCoC;

        // Asymmetric foreground rejection
        float depthMargin = max( centerLinear * 0.05, 5.0 );
        float foregroundReject = saturate( ( sampleLinear - centerLinear + depthMargin ) / depthMargin );
        weight *= foregroundReject;

        float luminance = dot( sampleColor, float3( 0.2126, 0.7152, 0.0722 ) );
        weight *= 1.0 + luminance * 2.0;

        colorAccum += sampleColor * weight;
        weightAccum += weight;
    }
#endif

    colorAccum /= max( weightAccum, 0.001 );

    OutputBlur[DTid.xy] = float4( colorAccum, centerCoC );
}
