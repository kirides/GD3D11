//--------------------------------------------------------------------------------------
// Depth of Field - Half-res bokeh blur pass
// Samples full-res scene + depth, computes CoC, does 48-tap bokeh blur
// Outputs blurred color (rgb) + CoC (a) at half resolution
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
Texture2D TX_Scene : register( t0 );   // Full-res scene color
Texture2D TX_Depth : register( t1 );   // Full-res hardware depth
Texture2D TX_Focus : register( t2 );   // 1x1 R32_FLOAT smoothed focus depth

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float LinearizeDepth( float d )
{
    return DoF_ProjParams.z / ( d - DoF_ProjParams.w );
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

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    // Texel size of the full-res scene for sampling offsets
    float2 sceneSize;
    TX_Scene.GetDimensions( sceneSize.x, sceneSize.y );
    float2 texelSize = 1.0 / sceneSize;

    float focusDepth = TX_Focus.SampleLevel( SS_Linear, float2( 0.5, 0.5 ), 0 ).r;

    float centerDepth = TX_Depth.Sample( SS_Linear, Input.vTexcoord ).r;
    float centerLinear = LinearizeDepth( centerDepth );
    float centerCoC = ComputeCoC( centerLinear, focusDepth );

    float3 centerColor = TX_Scene.Sample( SS_Linear, Input.vTexcoord ).rgb;

    // Early out: pass through sharp pixel
    if ( centerCoC < 0.01 )
        return float4( centerColor, 0.0 );

    float blurRadius = min( centerCoC * DoF_BokehRadius, DoF_MaxBlur );

    float3 colorAccum = 0.0;
    float weightAccum = 0.0;

    [unroll]
    for ( int i = 0; i < SAMPLE_COUNT; i++ )
    {
        float2 offset = GetSpiralSample( i, SAMPLE_COUNT );
        float2 sampleUV = Input.vTexcoord + offset * blurRadius * texelSize;

        float3 sampleColor = TX_Scene.Sample( SS_Linear, sampleUV ).rgb;
        float sampleDepth = TX_Depth.Sample( SS_Linear, sampleUV ).r;
        float sampleLinear = LinearizeDepth( sampleDepth );
        float sampleCoC = ComputeCoC( sampleLinear, focusDepth );

        float weight = ( sampleCoC >= length( offset ) * centerCoC ) ? 1.0 : sampleCoC;

        float luminance = dot( sampleColor, float3( 0.2126, 0.7152, 0.0722 ) );
        weight *= 1.0 + luminance * 2.0;

        colorAccum += sampleColor * weight;
        weightAccum += weight;
    }

    colorAccum /= max( weightAccum, 0.001 );

    // Store blurred color + CoC for the full-res composite
    return float4( colorAccum, centerCoC );
}
