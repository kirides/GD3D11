#ifndef HIZ_TEST_HLSL
#define HIZ_TEST_HLSL
// Hi-Z occlusion test shared by VobCull and WorldCull. The pyramid (HiZ.hlsl) holds the farthest reversed-Z
// depth per texel, so a rect is occluded only when every pixel under it is closer than the rect's nearest point.

bool HiZRectOccluded( float2 uvMin, float2 uvMax, float closestDepth, uint hizIndex, uint2 hizSize, uint mipCount )
{
    Texture2D<float> hiz = GD_HEAP( hizIndex );
    const float2 p0 = saturate( uvMin ) * float2( hizSize );
    const float2 p1 = saturate( uvMax ) * float2( hizSize );
    // The level where the footprint spans at most 2x2 texels, so four taps cover it.
    const float2 extent = max( p1 - p0, 1e-4 );
    const int mip = (int)clamp( ceil( log2( max( extent.x, extent.y ) ) ), 0.0, (float)( mipCount - 1 ) );
    const float mipScale = exp2( (float)mip );
    const int2 mipSize = max( int2( hizSize.x >> mip, hizSize.y >> mip ), int2( 1, 1 ) );
    const int2 t0 = clamp( int2( p0 / mipScale ), int2( 0, 0 ), mipSize - 1 );
    const int2 t1 = clamp( int2( p1 / mipScale ), int2( 0, 0 ), mipSize - 1 );
    float occluder = hiz.Load( int3( t0.x, t0.y, mip ) );
    occluder = min( occluder, hiz.Load( int3( t1.x, t0.y, mip ) ) );
    occluder = min( occluder, hiz.Load( int3( t0.x, t1.y, mip ) ) );
    occluder = min( occluder, hiz.Load( int3( t1.x, t1.y, mip ) ) );
    // Reversed-Z: greater == closer. Strict, so geometry is never hidden by its own depth.
    return occluder > closestDepth;
}

// World-space box against a reversed-Z view-proj. False when a corner is behind the eye (unbounded rect).
bool HiZBoxOccluded( float3 mn, float3 mx, float4x4 viewProj, uint hizIndex, uint2 hizSize, uint mipCount )
{
    float2 uvMin = float2( 1e30, 1e30 ), uvMax = -float2( 1e30, 1e30 );
    float closestDepth = 0.0;
    [unroll]
    for ( uint c = 0; c < 8; ++c )
    {
        const float3 corner = float3( ( c & 1 ) ? mx.x : mn.x, ( c & 2 ) ? mx.y : mn.y, ( c & 4 ) ? mx.z : mn.z );
        const float4 clip = mul( float4( corner, 1.0 ), viewProj );
        if ( clip.w <= 1e-4 ) return false;
        const float3 ndc = clip.xyz / clip.w;
        const float2 uv = ndc.xy * float2( 0.5, -0.5 ) + 0.5;
        uvMin = min( uvMin, uv );
        uvMax = max( uvMax, uv );
        closestDepth = max( closestDepth, ndc.z );
    }
    return HiZRectOccluded( uvMin, uvMax, closestDepth, hizIndex, hizSize, mipCount );
}

#endif
