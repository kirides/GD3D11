//--------------------------------------------------------------------------------------
// Compute Shader - Simple Sharpen (Unsharp Mask)
// Reads the source color, subtracts a 3x3 box-blur to get the high-frequency mask,
// and adds it back scaled by the sharpen strength. Writes the result directly to a UAV,
// removing the two plumbing copies of the pixel-shader path.
//
// Compute equivalent of PS_PFX_Sharpen.hlsl. Used on FeatureLevel 11+; the pixel-shader
// path remains as the FeatureLevel 10 fallback.
//--------------------------------------------------------------------------------------

SamplerState SS_Linear : register( s0 );
Texture2D TX_Texture0 : register( t0 );

RWTexture2D<float4> OutputTexture : register( u0 );

cbuffer PfxSharpenConstantBuffer : register( b0 )
{
    float2 G_TextureSize;
    float G_SharpenStrength;
    float G_pad1;
}

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 texSize;
    OutputTexture.GetDimensions( texSize.x, texSize.y );

    if ( DTid.x >= texSize.x || DTid.y >= texSize.y )
        return;

    float2 uv = ( float2( DTid.xy ) + 0.5f ) / float2( texSize );

    float4 source = TX_Texture0.SampleLevel( SS_Linear, uv, 0 );

    // 3x3 box blur
    float3 blurred = 0.0f;
    const int N = 3;
    [unroll] for ( int y = 0; y < N; y++ )
    {
        [unroll] for ( int x = 0; x < N; x++ )
        {
            float2 offset = float2( x - N / 2, y - N / 2 ) * ( 1.0f / G_TextureSize );
            blurred += TX_Texture0.SampleLevel( SS_Linear, uv + offset, 0 ).rgb;
        }
    }
    blurred /= ( N * N );

    float3 mask = max( 0.0f, source.rgb - blurred );

    OutputTexture[DTid.xy] = source + float4( mask * G_SharpenStrength, 0.0f );
}
