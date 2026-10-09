#ifndef DOF_SKY_H
#define DOF_SKY_H

// The sky is never blurred: its half-res texels carry only the spill of nearby blurred geometry, so
// silhouettes stay soft while stars and clouds stay sharp. Include after LinearizeDepth/ComputeCoC.
// Sky texels store alpha = DOF_SKY_ALPHA_BASE + spill coverage; geometry texels store their CoC (<= 1).
static const float DOF_SKY_ALPHA_BASE = 2.0;
static const float DOF_SKY_ALPHA_MIN = 1.5;
static const int DOF_SKY_SPILL_SAMPLES = 32;

float2 GetSpiralSample( int index, int count )
{
    float r = sqrt( ( float(index) + 0.5 ) / float(count) );
    float theta = float(index) * 2.39996323;
    float sinT, cosT;
    sincos( theta, sinT, cosT );
    return float2( r * cosT, r * sinT );
}

// Scatter-as-gather for a sky texel: a geometry tap counts when its own blur disc reaches this texel,
// weighted by its blend strength over its disc area. Sky taps contribute nothing.
float4 DoFSkySpill( Texture2D sceneTex, Texture2D depthTex, SamplerState ss, float2 uv, float2 texelSize,
    float3 skyColor, float focusDepth )
{
    float searchPx = max( min( DoF_BokehRadius, DoF_MaxBlur ), 1.0 );
    float3 colorAccum = 0.0;
    float weightAccum = 0.0;
    float boostAccum = 0.0;

    [loop]
    for ( int i = 0; i < DOF_SKY_SPILL_SAMPLES; i++ )
    {
        float2 offsetPx = GetSpiralSample( i, DOF_SKY_SPILL_SAMPLES ) * searchPx;
        float2 sampleUV = uv + offsetPx * texelSize;
        float sampleDepth = depthTex.SampleLevel( ss, sampleUV, 0 ).r;
        if ( sampleDepth <= 0.0 )
            continue;

        float sampleCoC = ComputeCoC( LinearizeDepth( sampleDepth ), focusDepth );
        float reachPx = max( min( sampleCoC * DoF_BokehRadius, DoF_MaxBlur ), 1.0 );
        float reach = saturate( reachPx - length( offsetPx ) + 0.5 );
        float weight = reach * smoothstep( 0.0, 1.0, sampleCoC ) * ( searchPx * searchPx ) / ( reachPx * reachPx );
        if ( weight <= 0.0 )
            continue;

        float3 sampleColor = sceneTex.SampleLevel( ss, sampleUV, 0 ).rgb;
#ifdef DOF_GAUSS_BLUR
        float boost = 1.0;
#else
        // Same luminance boost as the bokeh gather, so both sides of a silhouette mix alike.
        float boost = 1.0 + dot( sampleColor, float3( 0.2126, 0.7152, 0.0722 ) ) * 2.0;
#endif
        colorAccum += sampleColor * weight * boost;
        boostAccum += weight * boost;
        weightAccum += weight;
    }

    float coverage = saturate( weightAccum / DOF_SKY_SPILL_SAMPLES );
#ifndef DOF_GAUSS_BLUR
    // The sharp sky stands in for the sky share of the bokeh gather, boosted the same way.
    if ( weightAccum > 0.0 )
    {
        float geometryShare = coverage * boostAccum / weightAccum;
        float skyShare = ( 1.0 - coverage ) * ( 1.0 + dot( skyColor, float3( 0.2126, 0.7152, 0.0722 ) ) * 2.0 );
        coverage = geometryShare / ( geometryShare + skyShare );
    }
#endif

    // Uncovered texels keep the sky colour so bilinear taps between texels never pull in black.
    float3 color = boostAccum > 0.0 ? colorAccum / boostAccum : skyColor;
    return float4( color, DOF_SKY_ALPHA_BASE + coverage );
}

// Bilinear upsample of the half-res blur that keeps sky and geometry texels apart. For a sky pixel, .a is
// the blend factor (spill coverage); geometry pixels use only .rgb and blend by their own eroded CoC.
float4 DoFUpsampleBlur( Texture2D blurTex, SamplerState ss, float2 uv, bool skyPixel )
{
    float2 size;
    blurTex.GetDimensions( size.x, size.y );
    float2 f = frac( uv * size - 0.5 );

    // Gather order: x = (0,1), y = (1,1), z = (1,0), w = (0,0).
    float4 bilinear = float4( ( 1.0 - f.x ) * f.y, f.x * f.y, f.x * ( 1.0 - f.y ), ( 1.0 - f.x ) * ( 1.0 - f.y ) );
    float4 r = blurTex.GatherRed( ss, uv );
    float4 g = blurTex.GatherGreen( ss, uv );
    float4 b = blurTex.GatherBlue( ss, uv );
    float4 a = blurTex.GatherAlpha( ss, uv );
    float4 isSky = step( DOF_SKY_ALPHA_MIN, a );

    float4 w = skyPixel
        ? bilinear * lerp( smoothstep( 0.0, 1.0, saturate( a ) ), a - DOF_SKY_ALPHA_BASE, isSky )
        : bilinear * lerp( 1.0, 1e-3, isSky );   // sky texels only as a fallback for thin geometry
    float wSum = dot( w, 1.0 );
    if ( wSum <= 0.0 )
        return 0.0;
    return float4( float3( dot( r, w ), dot( g, w ), dot( b, w ) ) / wSum, wSum );
}

#endif
