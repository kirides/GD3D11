#ifndef LOW_CLOUD_NOISE_HLSL
#define LOW_CLOUD_NOISE_HLSL
// Tileable 3D cloud noise, generated once into a 64^3 RGBA8 texture (D3D11 CS_LowCloudNoise, D3D12 CSNoise).
// R = Perlin-Worley (shape), G = Perlin fBm (weather), B/A = inverted Worley fBm at base frequency 4/8 (erosion).

static const uint LOW_CLOUD_NOISE_SIZE = 64;

uint3 LowCloudPcg3d( uint3 v )
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

float3 LowCloudHashCell( int3 cell, int period, uint seed )
{
    uint3 wrapped = uint3( cell + period ) % uint( period );   // cell >= -1
    return float3( LowCloudPcg3d( wrapped + seed * uint3( 1031u, 2053u, 4099u ) ) >> 8u ) * ( 1.0f / 16777216.0f );
}

// 1 - distance to the nearest feature point, wrapping every `period` cells
float LowCloudWorley( float3 uvw, int period, uint seed )
{
    float3 p = uvw * period;
    int3 cell = int3( floor( p ) );
    float3 f = frac( p );
    float minDist = 1.0f;
    [unroll] for ( int z = -1; z <= 1; ++z )
    [unroll] for ( int y = -1; y <= 1; ++y )
    [unroll] for ( int x = -1; x <= 1; ++x )
    {
        int3 offset = int3( x, y, z );
        float3 feature = float3( offset ) + LowCloudHashCell( cell + offset, period, seed );
        minDist = min( minDist, length( feature - f ) );
    }
    return 1.0f - saturate( minDist );
}

// Gradient noise in [0,1], wrapping every `period` cells
float LowCloudPerlin( float3 uvw, int period, uint seed )
{
    float3 p = uvw * period;
    int3 cell = int3( floor( p ) );
    float3 f = frac( p );
    float3 u = f * f * f * ( f * ( f * 6.0f - 15.0f ) + 10.0f );
    float corners[8];
    [unroll] for ( int i = 0; i < 8; ++i )
    {
        int3 offset = int3( i & 1, ( i >> 1 ) & 1, i >> 2 );
        float3 gradient = normalize( LowCloudHashCell( cell + offset, period, seed ) * 2.0f - 1.0f + 1e-4f );
        corners[i] = dot( gradient, f - float3( offset ) );
    }
    float x00 = lerp( corners[0], corners[1], u.x );
    float x10 = lerp( corners[2], corners[3], u.x );
    float x01 = lerp( corners[4], corners[5], u.x );
    float x11 = lerp( corners[6], corners[7], u.x );
    return saturate( lerp( lerp( x00, x10, u.y ), lerp( x01, x11, u.y ), u.z ) * 0.9f + 0.5f );
}

float4 LowCloudNoiseTexel( uint3 id )
{
    float3 uvw = ( float3( id ) + 0.5f ) / LOW_CLOUD_NOISE_SIZE;

    float perlin = LowCloudPerlin( uvw, 4, 1u ) * 0.5333f + LowCloudPerlin( uvw, 8, 2u ) * 0.2667f
                 + LowCloudPerlin( uvw, 16, 3u ) * 0.1333f + LowCloudPerlin( uvw, 32, 4u ) * 0.0667f;
    // fBm bunches around 0.5; stretch it so coverage thresholds have room
    float weather = saturate( ( perlin - 0.5f ) * 2.5f + 0.5f );
    // Billowy: fold the gradient noise around its midpoint
    float billow = saturate( abs( weather * 2.0f - 1.0f ) * 1.2f );

    float w4 = LowCloudWorley( uvw, 4, 5u );
    float w8 = LowCloudWorley( uvw, 8, 6u );
    float w16 = LowCloudWorley( uvw, 16, 7u );
    float w32 = LowCloudWorley( uvw, 32, 8u );

    float shapeWorley = w4 * 0.625f + w8 * 0.25f + w16 * 0.125f;
    float perlinWorley = saturate( shapeWorley + billow * ( 1.0f - shapeWorley ) );   // remap(perlin, 0, 1, worley, 1)

    return float4( perlinWorley, weather, shapeWorley, w8 * 0.625f + w16 * 0.25f + w32 * 0.125f );
}

#endif
