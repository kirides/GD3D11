//--------------------------------------------------------------------------------------
// Depth of Field - Bokeh blur pass
// Circular kernel with weighted sampling for pleasing bokeh shapes
//--------------------------------------------------------------------------------------

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
Texture2D TX_CoCScene : register( t0 ); // rgb = color, a = CoC from pass 1

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

// Golden-angle based disc sampling for smooth circular bokeh
// 48 samples arranged in a Fibonacci spiral
static const int SAMPLE_COUNT = 48;

float2 GetSpiralSample( int index, int count )
{
    // Golden angle ≈ 2.39996323 radians
    float r = sqrt( ( float(index) + 0.5 ) / float(count) );
    float theta = float(index) * 2.39996323;
    return float2( r * cos( theta ), r * sin( theta ) );
}

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float2 texelSize;
    TX_CoCScene.GetDimensions( texelSize.x, texelSize.y );
    texelSize = 1.0 / texelSize;

    float4 centerSample = TX_CoCScene.Sample( SS_Linear, Input.vTexcoord );
    // Decode CoC from [0, 1] back to [-1, 1]
    float centerCoC = centerSample.a * 2.0 - 1.0;
    float absCoC = abs( centerCoC );

    // Early out if this pixel is sharp
    if ( absCoC < 0.01 )
        return float4( centerSample.rgb, 1.0 );

    float blurRadius = absCoC * DoF_BokehRadius;
    blurRadius = min( blurRadius, DoF_MaxBlur );

    float3 colorAccum = 0.0;
    float weightAccum = 0.0;

    [unroll]
    for ( int i = 0; i < SAMPLE_COUNT; i++ )
    {
        float2 offset = GetSpiralSample( i, SAMPLE_COUNT );
        float2 sampleUV = Input.vTexcoord + offset * blurRadius * texelSize;

        float4 sampleColor = TX_CoCScene.Sample( SS_Linear, sampleUV );
        float sampleCoC = sampleColor.a * 2.0 - 1.0;
        float absSampleCoC = abs( sampleCoC );

        // Only allow background samples to bleed onto foreground if they have enough CoC
        // This prevents sharp background bleeding onto a blurry foreground
        float weight = ( absSampleCoC >= length( offset ) * absCoC ) ? 1.0 : absSampleCoC;
        
        // Boost bright samples for more pronounced bokeh highlights
        float luminance = dot( sampleColor.rgb, float3( 0.2126, 0.7152, 0.0722 ) );
        weight *= 1.0 + luminance * 2.0;

        colorAccum += sampleColor.rgb * weight;
        weightAccum += weight;
    }

    colorAccum /= max( weightAccum, 0.001 );

    return float4( colorAccum, 1.0 );
}
