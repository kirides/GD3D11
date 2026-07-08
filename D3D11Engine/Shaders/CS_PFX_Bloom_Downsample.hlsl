//--------------------------------------------------------------------------------------
// Compute Shader - Bloom Downsample
// 13-tap downsample filter (Jimenez / Call of Duty "Next Generation Post Processing").
// Reads the source texture (t0), writes the half-size result to a UAV (u0).
//
// With BLOOM_PREFILTER defined this is the first (bright-pass) step: it Karis-averages
// each 2x2 group to suppress fireflies, then applies a soft-knee brightness threshold so
// only bright pixels contribute to the glow. Without the macro it is a plain downsample
// used to build the rest of the bloom pyramid.
//--------------------------------------------------------------------------------------

SamplerState SS_LinearClamp : register( s0 );
Texture2D TX_Source : register( t0 );

RWTexture2D<float4> OutputTexture : register( u0 );

cbuffer BloomConstantBuffer : register( b0 )
{
    float2 B_TexelSize;    // 1 / source dimensions
    float  B_Threshold;    // brightness threshold (prefilter only)
    float  B_Knee;         // soft-knee width (prefilter only)
    float  B_Intensity;    // composite strength (unused here)
    float  B_FilterRadius; // upsample radius (unused here)
    float2 B_Pad;
};

float Luma( float3 c )
{
    return dot( c, float3( 0.2126f, 0.7152f, 0.0722f ) );
}

float3 SampleSrc( float2 uv, float2 offset )
{
    return TX_Source.SampleLevel( SS_LinearClamp, uv + offset * B_TexelSize, 0 ).rgb;
}

#if BLOOM_PREFILTER
// Karis average weight: bias toward darker samples so a single very bright texel cannot
// dominate the group (firefly reduction). Weighting happens in a rough perceptual space.
float KarisWeight( float3 c )
{
    return 1.0f / ( 1.0f + Luma( c ) );
}

float3 KarisGroup( float3 a, float3 b, float3 c, float3 d )
{
    float wa = KarisWeight( a );
    float wb = KarisWeight( b );
    float wc = KarisWeight( c );
    float wd = KarisWeight( d );
    float wsum = wa + wb + wc + wd;
    return ( a * wa + b * wb + c * wc + d * wd ) / max( wsum, 1e-5f );
}

// Soft-knee quadratic threshold (Unreal/COD). Below (threshold-knee): 0. Above
// (threshold): passes through. In between: smooth quadratic ramp.
float3 ApplyThreshold( float3 color )
{
    float knee = max( B_Knee, 1e-4f );
    float3 curve = float3( B_Threshold - knee, 2.0f * knee, 0.25f / knee );
    float br = max( color.r, max( color.g, color.b ) );
    float rq = clamp( br - curve.x, 0.0f, curve.y );
    rq = curve.z * rq * rq;
    color *= max( rq, br - B_Threshold ) / max( br, 1e-5f );
    return color;
}
#endif

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outSize;
    OutputTexture.GetDimensions( outSize.x, outSize.y );

    if ( DTid.x >= outSize.x || DTid.y >= outSize.y )
        return;

    float2 uv = ( float2( DTid.xy ) + 0.5f ) / float2( outSize );

    // 13 bilinear taps (offsets in source-texel units)
    float3 a = SampleSrc( uv, float2( -2, -2 ) );
    float3 b = SampleSrc( uv, float2(  0, -2 ) );
    float3 c = SampleSrc( uv, float2(  2, -2 ) );
    float3 d = SampleSrc( uv, float2( -2,  0 ) );
    float3 e = SampleSrc( uv, float2(  0,  0 ) );
    float3 f = SampleSrc( uv, float2(  2,  0 ) );
    float3 g = SampleSrc( uv, float2( -2,  2 ) );
    float3 h = SampleSrc( uv, float2(  0,  2 ) );
    float3 i = SampleSrc( uv, float2(  2,  2 ) );
    float3 j = SampleSrc( uv, float2( -1, -1 ) );
    float3 k = SampleSrc( uv, float2(  1, -1 ) );
    float3 l = SampleSrc( uv, float2( -1,  1 ) );
    float3 m = SampleSrc( uv, float2(  1,  1 ) );

#if BLOOM_PREFILTER
    // Karis-average each 2x2 group, weight the groups, then threshold.
    float3 result  = KarisGroup( j, k, l, m ) * 0.5f;
    result += KarisGroup( a, b, d, e ) * 0.125f;
    result += KarisGroup( b, c, e, f ) * 0.125f;
    result += KarisGroup( d, e, g, h ) * 0.125f;
    result += KarisGroup( e, f, h, i ) * 0.125f;
    result = ApplyThreshold( result );
#else
    // Standard weighted downsample (overlapping 2x2 boxes, weights sum to 1).
    float3 result  = e * 0.125f;
    result += ( a + c + g + i ) * 0.03125f;
    result += ( b + d + f + h ) * 0.0625f;
    result += ( j + k + l + m ) * 0.125f;
#endif

    OutputTexture[DTid.xy] = float4( result, 1.0f );
}
