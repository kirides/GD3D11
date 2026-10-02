#ifndef WATER_VERTEX_WAVES_HLSL
#define WATER_VERTEX_WAVES_HLSL
// Gerstner swell for water materials with a wave mode, shared by VS_ExWater (SHD_WATERANI) and the D3D12
// Water.hlsl VS. WorldConverter packs the material's wave parameters into the vertex color: .z = max
// amplitude / 5, .w = speed * 10, both as UNORM bytes, and zeroes them for water without waves.
//
// Only the position moves. The shading normal still comes from the distortion texture, so the geometric
// normal stays flat like D3D11's.

float WaterGerstnerWave( inout float3 offset, float2 dir, float amplitude, float2 pos, float speed, float frequency, float totalTimeMs )
{
    float x = dot( dir, pos ) * frequency + totalTimeMs * 0.001f * speed;
    float sinX, cosX;
    sincos( x, sinX, cosX );
    offset += float3( dir.x * ( amplitude * cosX ), amplitude * sinX, dir.y * ( amplitude * cosX ) );
    return amplitude * cosX;
}

// World-space offset of a water vertex; zero for water without a wave mode
float3 WaterWaveOffset( float3 positionWorld, float4 vertexColor, float totalTimeMs )
{
    float3 offset = float3( 0.0f, 0.0f, 0.0f );
    if ( vertexColor.z <= 0.0f ) return offset;

    const float amplitude = vertexColor.z * 1275.0f;   // byte * 5, back to world units
    const float waveSpeed = vertexColor.w * 25.5f;     // byte / 10
    const int iterations = 10;
    const float dragMult = 0.48f;

    float wsum = 0.0f;
    float wx = 1.0f;
    [unroll] for ( int j = 0; j < iterations; j++ ) {
        wsum += wx;
        wx *= 0.8f;
    }

    float2 pos = positionWorld.xz;
    float freq = 0.6f * 0.005f;
    float speed = 2.0f;
    float iter = 0.0f;
    float weight = 1.0f;
    [unroll] for ( int i = 0; i < iterations; i++ ) {
        float sinIter, cosIter;
        sincos( iter, sinIter, cosIter );
        float2 dir = float2( cosIter, sinIter );
        float res = WaterGerstnerWave( offset, dir, weight * amplitude / wsum, pos, speed * waveSpeed, freq, totalTimeMs );

        // Drag the sample point along each wave so the next one rides on it
        pos += res * weight * dir * dragMult;

        iter += 12.0f;
        weight *= 0.8f;
        freq *= 1.18f;
        speed *= 1.07f;
    }
    return offset;
}

#endif // WATER_VERTEX_WAVES_HLSL
