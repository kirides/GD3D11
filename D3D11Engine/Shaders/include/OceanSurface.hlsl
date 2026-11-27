// Evaluates the simulated sea (OceanFFT.hlsl's displacement / derivative cascades) at an undisplaced world position. Shared by the
// D3D11 and D3D12 water. The including file provides: OceanTile (float4, tile sizes in m, 0 = unused), OceanUnitsPerMetre,
// OceanTexels (N), and OceanFetchDisp / OceanFetchDeriv(cascade, uv, lod) -> float4 (SampleLevel on the cascade arrays).
#ifndef GD_OCEAN_SURFACE_HLSL
#define GD_OCEAN_SURFACE_HLSL

// Damping towards land, x for the long cascades and y for the short ones. Near the coast the long, wind-driven waves hand
// over to the shoreward swell (WaterSurf.hlsl, same distances); the short ones keep the surface bumpy up to the waterline.
// CPU twin: OceanFFT::WaveWeightsForShore (OceanSpectrum.h); keep both in step.
#define OCEAN_SHORE_NEAR 100.0f
#define OCEAN_SHORE_FAR 1500.0f
#define OCEAN_SHORE_DEPTH 200.0f
#define OCEAN_COAST_NEAR 1500.0f
#define OCEAN_COAST_FAR 5000.0f
#define OCEAN_LONG_TILE 30.0f   // metres: tiles from this size up are the long cascades
float2 OceanShoreWeights( float depth, float distanceToShore )
{
    float detail = smoothstep( 0.0f, OCEAN_SHORE_DEPTH, depth ) * smoothstep( OCEAN_SHORE_NEAR, OCEAN_SHORE_FAR, distanceToShore );
    return float2( detail * smoothstep( OCEAN_COAST_NEAR, OCEAN_COAST_FAR, distanceToShore ), detail );
}

struct OceanSurfaceSample
{
    float3 offset;   // world units, added to the undisplaced position
    float2 slope;    // d height / d x, d height / d z of the displaced surface
    float  foam;     // whitecap coverage 0..1
};

float2 OceanRotate( float2 v, float2 cs ) { return float2( v.x * cs.x - v.y * cs.y, v.x * cs.y + v.y * cs.x ); }

// footprintM: world size of one sample in metres (picks the mip so tiles smaller than it fade out); weights: OceanShoreWeights;
// drift: rotation of the whole sea about the world origin (wind drift), so it needs no new spectrum
OceanSurfaceSample OceanEvaluate( float2 xz, float footprintM, float2 weights, float drift )
{
    float2 cs;
    sincos( drift, cs.y, cs.x );
    float2 p = OceanRotate( xz, float2( cs.x, -cs.y ) );
    float3 d = 0.0f;
    float4 dv = 0.0f;
    float foam = 0.0f;
    [unroll] for ( uint c = 0; c < 4; ++c )
    {
        float L = OceanTile[c];
        [branch] if ( L > 0.0f )
        {
            float w = L >= OCEAN_LONG_TILE ? weights.x : weights.y;
            float2 uv = p / ( OceanUnitsPerMetre * L );
            float lod = log2( max( footprintM * OceanTexels / L, 1.0f ) );
            float4 a = OceanFetchDisp( c, uv, lod );
            d += a.xyz * w;
            dv += OceanFetchDeriv( c, uv, lod ) * w;
            foam = max( foam, a.w * w );
        }
    }
    OceanSurfaceSample s;
    s.offset = float3( 0.0f, d.y, 0.0f );
    s.offset.xz = OceanRotate( d.xz, cs );
    s.offset *= OceanUnitsPerMetre;
    s.slope = OceanRotate( dv.xy / max( 1.0f + dv.zw, 0.2f ), cs );
    s.foam = saturate( foam );
    return s;
}

// field: the shore field texel at xz (distance, depth, seaward xz)
OceanSurfaceSample OceanEvaluateAtShore( float2 xz, float footprintM, float4 field, float drift )
{
    return OceanEvaluate( xz, footprintM, OceanShoreWeights( field.y, field.x ), drift );
}

#endif
