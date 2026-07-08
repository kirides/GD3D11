//--------------------------------------------------------------------------------------
// Compute Shader - Bloom Upsample
// 3x3 tent-filter upsample of the smaller (lower) mip (t0), added to the same-size
// downsampled mip (t1), written to the destination UAV (u0). Running this up the pyramid
// progressively accumulates the blurred mips into a wide, smooth glow (Jimenez / COD).
//
// Separate down/up buffers are used so we never sample and UAV-write the same texture.
//--------------------------------------------------------------------------------------

SamplerState SS_LinearClamp : register( s0 );
Texture2D TX_Source : register( t0 ); // lower (smaller) mip to upsample
Texture2D TX_Base   : register( t1 ); // this level's downsampled mip

RWTexture2D<float4> OutputTexture : register( u0 );

cbuffer BloomConstantBuffer : register( b0 )
{
    float2 B_TexelSize;    // 1 / source (lower mip) dimensions
    float  B_Threshold;
    float  B_Knee;
    float  B_Intensity;
    float  B_FilterRadius; // tent radius in source texels
    float2 B_Pad;
};

float3 SampleSrc( float2 uv, float2 offset )
{
    return TX_Source.SampleLevel( SS_LinearClamp, uv + offset * B_TexelSize * B_FilterRadius, 0 ).rgb;
}

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outSize;
    OutputTexture.GetDimensions( outSize.x, outSize.y );

    if ( DTid.x >= outSize.x || DTid.y >= outSize.y )
        return;

    float2 uv = ( float2( DTid.xy ) + 0.5f ) / float2( outSize );

    // 3x3 tent filter (weights 1,2,1 / 2,4,2 / 1,2,1) / 16
    float3 s;
    s  = SampleSrc( uv, float2( -1, -1 ) );
    s += SampleSrc( uv, float2(  0, -1 ) ) * 2.0f;
    s += SampleSrc( uv, float2(  1, -1 ) );
    s += SampleSrc( uv, float2( -1,  0 ) ) * 2.0f;
    s += SampleSrc( uv, float2(  0,  0 ) ) * 4.0f;
    s += SampleSrc( uv, float2(  1,  0 ) ) * 2.0f;
    s += SampleSrc( uv, float2( -1,  1 ) );
    s += SampleSrc( uv, float2(  0,  1 ) ) * 2.0f;
    s += SampleSrc( uv, float2(  1,  1 ) );
    s *= ( 1.0f / 16.0f );

    float3 base = TX_Base.SampleLevel( SS_LinearClamp, uv, 0 ).rgb;

    OutputTexture[DTid.xy] = float4( base + s, 1.0f );
}
