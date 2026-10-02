#ifndef WATER_VERTEX_WAVES_HLSL
#define WATER_VERTEX_WAVES_HLSL
// Vertex waves for water materials with a wave mode, shared by VS_ExWater (SHD_WATERANI) and the D3D12 Water.hlsl
// VS. WorldConverter packs the material's wave parameters into the vertex color as UNORM bytes: .z = max amplitude
// / 5, .w = speed * 10 (zero without waves), .xy = grid size in bits 0-14 plus a wall-mode flag in bit 15.
//
// Only the position moves. The shading normal still comes from the distortion texture, so the geometric
// normal stays flat like D3D11's.

#include "ZenGinWaveSpectrum.hlsl"

// GothicRendererSettings::E_WaterWaves
#define WATER_WAVES_OFF      0
#define WATER_WAVES_ORIGINAL 1   // ZenGin's zCPolygon::ApplyMorphing
#define WATER_WAVES_D3D11    2   // Gerstner swell

float WaterGerstnerWave( inout float3 offset, float2 dir, float amplitude, float2 pos, float speed, float frequency, float totalTimeMs )
{
    float x = dot( dir, pos ) * frequency + totalTimeMs * 0.001f * speed;
    float sinX, cosX;
    sincos( x, sinX, cosX );
    offset += float3( dir.x * ( amplitude * cosX ), amplitude * sinX, dir.y * ( amplitude * cosX ) );
    return amplitude * cosX;
}

float3 WaterGerstnerOffset( float3 positionWorld, float4 vertexColor, float totalTimeMs )
{
    float3 offset = float3( 0.0f, 0.0f, 0.0f );
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

// zVEC3::LengthApprox
float ZenGinLengthApprox( float3 v )
{
    v = abs( v );
    const float m = max( v.x, max( v.y, v.z ) );
    const float t = v.x + v.y + v.z - m;
    return m - m * ( 1.0f / 16.0f ) + t * ( 1.0f / 4.0f ) + t * ( 1.0f / 8.0f );
}

// Alg_ArcTan2Approx
float ZenGinArcTan2Approx( float y, float x )
{
    const float c1 = 0.785398163f;
    const float absY = abs( y ) + 1e-10f;
    const float angle = x >= 0.0f ? c1 - c1 * ( ( x - absY ) / ( x + absY ) ) : 3.0f * c1 - c1 * ( ( x + absY ) / ( absY - x ) );
    return y < 0.0f ? -angle : angle;
}

// zCFFT::S_CalcWave2D in [-1, 1]: a 32x32 tile of FFT cells, each gridSize wide, sampled at the nearest cell
float ZenGinWave2D( float2 p, float gridSize, float timeSec )
{
    const float TWO_PI = 6.28318531f;
    const int2 cell = int2( p * ( 1.0f / gridSize ) + 0.5f );   // truncates toward zero like the C cast
    const uint row = uint( cell.x ) & 31u;
    const uint col = uint( cell.y ) & 31u;
    const float kz = TWO_PI / 32.0f * ( float( row ) - 16.0f );

    // zCFFT::AnimateWaveMap's row-wise inverse FFT, evaluated for this one cell
    float2 h = float2( 0.0f, 0.0f );
    [loop] for ( uint i = 0; i < 8; i++ ) {
        const float4 a = ZenGinWaveSpectrum[row * 8 + i];
        [unroll] for ( uint j = 0; j < 4; j++ ) {
            const uint x = i * 4 + j;
            const float omega = sqrt( 9.81f * ZenGinLengthApprox( float3( TWO_PI / 32.0f * ( float( x ) - 16.0f ), 0.01f, kz ) ) );
            // In turns, wrapped before sincos so long sessions keep their precision
            const float turns = frac( omega * timeSec * ( 1.0f / TWO_PI ) ) + float( ( x * col ) & 31u ) * ( 1.0f / 32.0f );
            float s, c;
            sincos( turns * TWO_PI, s, c );
            h += a[j] * float2( c, -s );
        }
    }
    h *= 1.0f / 32.0f;
    return clamp( sin( ZenGinArcTan2Approx( h.y, h.x ) + length( h ) * 1e7f ), -1.0f, 1.0f );
}

// zCPolygon::ApplyMorphing: ground modes lift the vertex, wall/env/wind modes push x and z by the same amount
float3 ZenGinWaveOffset( float3 positionWorld, float4 vertexColor, float totalTimeMs )
{
    const uint packed = uint( vertexColor.x * 255.0f + 0.5f ) | ( uint( vertexColor.y * 255.0f + 0.5f ) << 8 );
    const float gridSize = max( float( packed & 0x7FFFu ), 1.0f );
    const float amplitude = vertexColor.z * 1275.0f;
    // zTFFT_SLOW/NORMAL/FAST run at 0.2/0.5/2.0, half the packed 0.4/1/4 speed
    const float timeSec = totalTimeMs * 0.001f * ( vertexColor.w * 25.5f * 0.5f );
    const bool wall = ( packed & 0x8000u ) != 0;
    const float d = ZenGinWave2D( wall ? positionWorld.yz : positionWorld.xz, gridSize, timeSec ) * amplitude;
    return wall ? float3( d, 0.0f, d ) : float3( 0.0f, d, 0.0f );
}

// World-space offset of a water vertex; zero for water without a wave mode
float3 WaterWaveOffset( float3 positionWorld, float4 vertexColor, float totalTimeMs, uint mode )
{
    if ( vertexColor.z <= 0.0f ) return float3( 0.0f, 0.0f, 0.0f );
    [branch] if ( mode == WATER_WAVES_ORIGINAL ) return ZenGinWaveOffset( positionWorld, vertexColor, totalTimeMs );
    return WaterGerstnerOffset( positionWorld, vertexColor, totalTimeMs );
}

#endif // WATER_VERTEX_WAVES_HLSL
