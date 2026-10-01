#ifndef WATER_WAVES_HLSL
#define WATER_WAVES_HLSL
// Wave normals and the geometry reflection ray, shared by ShadeWater and the D3D12 ray-traced reflection pass
// (WaterRT.hlsl), which must trace exactly the ray the pixel shader later looks up. The includer defines
// WaterDistortion( float2 uv ).

static const float3 WATER_UP = float3( 0.0f, 1.0f, 0.0f );

static const float DIST_SMALL_SPEED  = -0.01f;
static const float DIST_SMALL_AMOUNT = 0.01f;
static const float DIST_SMALL_SCALE  = 0.3f;
static const float DIST_BIG_SCALE    = 0.1f;
static const float DIST_BIG_SPEED    = -0.005f;

float WaterSmootherStep01( float t )
{
    t = saturate( t );
    return t * t * t * ( t * ( t * 6.0f - 15.0f ) + 10.0f );
}

// Large-scale wave distortion in [-1, 1]; worldTexCoord = worldPos.xz / 1000
float3 WaterBigDistortion( float2 worldTexCoord, float time )
{
    float3 d = WaterDistortion( worldTexCoord * DIST_BIG_SCALE + time * DIST_BIG_SPEED ) * 2 - 1;
    d += WaterDistortion( worldTexCoord * float2( -1, 0.7 ) * DIST_BIG_SCALE + time * DIST_BIG_SPEED * 1.2 ) * 2 - 1;
    return d * 0.5f;
}

float3 WaterWaveNormal( float3 distortion ) { return normalize( distortion.xzy * float3( 1, 10, 1 ) ); }

// The ray SSR and ray tracing follow: the big waves, flattened toward the horizon
float3 WaterGeometryReflectionDir( float3 viewDirection, float3 wavesFres, float surfaceViewDistance )
{
    float normalSmooth = 0.34f + 0.18f * WaterSmootherStep01( ( surfaceViewDistance - 1500.0f ) / 12000.0f );
    return reflect( viewDirection, normalize( lerp( wavesFres, WATER_UP, normalSmooth ) ) );
}

#endif // WATER_WAVES_HLSL
