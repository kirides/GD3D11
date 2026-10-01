// Ray-traced shadow mask (RtShadows.hlsl writes it, PBRLighting.hlsl reads it), one R32G32_UINT per pixel:
//   x  0-7   sun visibility          x  8-11  sun weight (0 = use the CSM, 15 = ray traced only)
//   x 12-15  cluster Z slice          x 16-30  point lights 0-4, 3 bits each (7 = lit)    x 31  traced
//   y  0-29  point lights 5-14
// Point slot k belongs to the k-th group of the pixel's cluster walk (bit order, lights with RtShadowMask != 0);
// a group is a run of consecutive lights at one position with one mask. The lit pass finds a slot by walking the
// same cluster; a different slice means a different list, and the mask is ignored.
#ifndef D3D12_RTSHADOWMASK_HLSL
#define D3D12_RTSHADOWMASK_HLSL

static const uint kRtMaskTraced = 0x80000000u;
static const uint kRtMaxPointLights = 15u;
static const uint2 kRtMaskAllLit = uint2( 0x7FFF0000u, 0x3FFFFFFFu );

float RtMaskSunVisibility( uint2 m ) { return float( m.x & 0xFFu ) / 255.0; }
float RtMaskSunWeight( uint2 m )     { return float( ( m.x >> 8 ) & 0xFu ) / 15.0; }
uint  RtMaskSlice( uint2 m )         { return ( m.x >> 12 ) & 0xFu; }

float RtMaskPointVisibility( uint2 m, uint k )
{
    uint v = k < 5u ? ( m.x >> ( 16u + 3u * k ) ) : ( m.y >> ( 3u * ( k - 5u ) ) );
    return float( v & 7u ) / 7.0;
}

uint2 RtMaskSetPoint( uint2 m, uint k, float visibility )
{
    uint v = (uint)round( saturate( visibility ) * 7.0 );
    if ( k < 5u )
    {
        uint s = 16u + 3u * k;
        m.x = ( m.x & ~( 7u << s ) ) | ( v << s );
    }
    else
    {
        uint s = 3u * ( k - 5u );
        m.y = ( m.y & ~( 7u << s ) ) | ( v << s );
    }
    return m;
}

#endif // D3D12_RTSHADOWMASK_HLSL
