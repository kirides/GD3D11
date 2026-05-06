//--------------------------------------------------------------------------------------
// Compute Shader - God Ray Mask Pass
// Reads backbuffer + depth, outputs mask to UAV
//--------------------------------------------------------------------------------------

SamplerState SS_Linear : register( s0 );
Texture2D TX_Texture0 : register( t0 ); // Backbuffer
Texture2D TX_Depth : register( t1 ); // Depth

RWTexture2D<float4> OutputTexture : register( u0 );

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outSize;
    OutputTexture.GetDimensions( outSize.x, outSize.y );

    if ( DTid.x >= outSize.x || DTid.y >= outSize.y )
        return;

    float2 uv = ( float2( DTid.xy ) + 0.5 ) / float2( outSize );

    float4 color = TX_Texture0.SampleLevel( SS_Linear, uv, 0 );
    float4 depth = TX_Depth.SampleLevel( SS_Linear, uv, 0 );

    if ( depth.r < 0.00001f ) // likely sky pixel
        OutputTexture[DTid.xy] = color;
    else
        OutputTexture[DTid.xy] = float4( 0, 0, 0, 0 );
}
