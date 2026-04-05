//--------------------------------------------------------------------------------------
// Compute Shader - God Ray Zoom (Radial Blur) Pass
// Reads mask texture, applies radial blur toward sun center, writes to UAV
//--------------------------------------------------------------------------------------

cbuffer GodRayZoomConstantBuffer : register( b0 )
{
    float GR_Decay;
    float GR_Weight;
    float2 GR_Center;

    float GR_Density;
    float3 GR_ColorMod;
};

SamplerState SS_Linear : register( s0 );
Texture2D TX_Texture0 : register( t0 ); // Mask from pass 1

RWTexture2D<float4> OutputTexture : register( u0 );

// Interleaved Gradient Noise for cheap, effective dithering
float InterleavedGradientNoise( float2 uv )
{
    float3 magic = float3( 0.06711056f, 0.00583715f, 52.9829189f );
    return frac( magic.z * frac( dot( uv, magic.xy ) ) );
}

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 texSize;
    OutputTexture.GetDimensions( texSize.x, texSize.y );

    if ( DTid.x >= texSize.x || DTid.y >= texSize.y )
        return;

    float2 texcoord = ( float2( DTid.xy ) + 0.5 ) / float2( texSize );

    const int NUM_SAMPLES = 64;
    float2 center = GR_Center;
    float3 color = 0;
    float illumDecay = 1.0f;

    float2 deltaTexCoord = texcoord - center;
    deltaTexCoord *= 1.0f / NUM_SAMPLES * GR_Density;

    float2 uv = texcoord;

    // Dithering: Offset the starting UV by a random sub-texel fraction
    float jitter = InterleavedGradientNoise( float2( DTid.xy ) );
    uv -= deltaTexCoord * jitter;

    [unroll(64)]
    for ( int i = 0; i < NUM_SAMPLES; i++ )
    {
        uv -= deltaTexCoord;

        // Anti-Smearing: Prevent sampling out of bounds
        if ( uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f )
        {
            continue;
        }

        color += TX_Texture0.SampleLevel( SS_Linear, uv, 0 ).rgb * illumDecay * GR_Weight;

        illumDecay *= GR_Decay;
    }
    color /= NUM_SAMPLES;

    OutputTexture[DTid.xy] = float4( color * GR_ColorMod, 1.0f );
}
