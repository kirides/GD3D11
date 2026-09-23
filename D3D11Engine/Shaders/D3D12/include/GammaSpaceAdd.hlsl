// DX7/D3D11 add in gamma space; the D3D12 scene target is linear HDR, where a dim additive term over a
// bright background comes out several times weaker. For an ADD draw (SrcAlpha, One) GammaSpaceAddSource
// returns the source color whose linear add lands on lin(srgb(dst) + a*c), dst = the opaque scene copy.
#ifndef D3D12_GAMMA_SPACE_ADD_HLSL
#define D3D12_GAMMA_SPACE_ADD_HLSL

// Local names: several includers define their own SrgbToLinear. Both are unclamped so HDR values round-trip.
float3 GSA_ToLinear( float3 c )
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

float3 GSA_ToSrgb( float3 c )
{
    return select( c <= 0.0031308, c * 12.92, 1.055 * pow( c, 1.0 / 2.4 ) - 0.055 );
}

// Callers guard alpha > 1/255; srgbColor is the draw's color before linearization.
float3 GammaSpaceAddSource( uint opaqueSceneIndex, float2 pixel, float3 srgbColor, float alpha )
{
    Texture2D opaqueScene = ResourceDescriptorHeap[opaqueSceneIndex];
    float3 dst = max( opaqueScene.Load( int3( pixel, 0 ) ).rgb, 0.0 );
    float3 target = GSA_ToLinear( GSA_ToSrgb( dst ) + alpha * srgbColor );
    return max( target - dst, 0.0 ) / alpha;
}

#endif // D3D12_GAMMA_SPACE_ADD_HLSL
