// Evaluates the simulated sea (OceanFFT.hlsl's displacement / derivative cascades) at an undisplaced world position. Shared by the
// D3D11 and D3D12 water. The including file provides: OceanTile (float4, tile sizes in m, 0 = unused), OceanUnitsPerMetre,
// OceanTexels (N), and OceanFetchDisp / OceanFetchDeriv(cascade, uv, lod) -> float4 (SampleLevel on the cascade arrays).
#ifndef GD_OCEAN_SURFACE_HLSL
#define GD_OCEAN_SURFACE_HLSL

struct OceanSurfaceSample
{
    float3 offset;   // world units, added to the undisplaced position
    float2 slope;    // d height / d x, d height / d z of the displaced surface
    float  foam;     // whitecap coverage 0..1
};

// footprintM: world size of one sample in metres (picks the mip so tiles smaller than it fade out); weight: shore damping
OceanSurfaceSample OceanEvaluate( float2 xz, float footprintM, float weight )
{
    float3 d = 0.0f;
    float4 dv = 0.0f;
    float foam = 0.0f;
    [unroll] for ( uint c = 0; c < 4; ++c )
    {
        float L = OceanTile[c];
        [branch] if ( L > 0.0f )
        {
            float2 uv = xz / ( OceanUnitsPerMetre * L );
            float lod = log2( max( footprintM * OceanTexels / L, 1.0f ) );
            float4 a = OceanFetchDisp( c, uv, lod );
            d += a.xyz;
            dv += OceanFetchDeriv( c, uv, lod );
            foam = max( foam, a.w );
        }
    }
    OceanSurfaceSample s;
    s.offset = d * ( OceanUnitsPerMetre * weight );
    s.slope = dv.xy / max( 1.0f + dv.zw, 0.2f ) * weight;
    s.foam = saturate( foam * weight );
    return s;
}

#endif
