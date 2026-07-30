// Hierarchical depth buffer ("Hi-Z") — the occlusion source for GPU VOB culling (D3D12Cull.cpp).
//
// Built right after the WORLD-MESH depth prepass, so it contains exactly the "world-mesh data" the VOB
// cull tests against: BSP/terrain geometry only, no VOBs, no NPCs (a VOB must never occlude itself, and
// the VOB depth is laid down *after* the cull anyway).
//
//   CSCopyDepth : full-res reversed-Z depth -> Hi-Z mip 0 (HALF resolution, min of the 2x2 footprint)
//   CSReduce    : mip N-1 -> mip N          (min of the 2x2 footprint, + the odd-dimension edge fold)
//
// REVERSED-Z: larger depth == closer. The pyramid therefore stores the **minimum** (= the FARTHEST
// occluder) over each footprint, which is the conservative choice: a box is only rejected when *every*
// pixel it covers is already occluded by something closer, so a min-reduce can only ever under-cull.
// Sky pixels sit at the cleared 0.0 (infinitely far), so any footprint touching sky reduces to 0 and
// occludes nothing at all.
//
// Both passes fetch everything bindlessly (SM6.6 ResourceDescriptorHeap) from root-constant heap indices,
// so the shared root signature is just b0 + no descriptor tables (same shape as GodRays.hlsl). The source
// mip is read through its **UAV** rather than an SRV: typed R32_FLOAT UAV loads are mandatory-support, and
// it keeps the whole chain in UNORDERED_ACCESS so consecutive levels only need a UAV barrier between
// dispatches instead of a per-mip state transition. Two cbuffers at b0 — only the one the compiled entry
// point references survives into that PSO (same trick SSAO.hlsl/GodRays.hlsl use).

//--------------------------------------------------------------------------------------
// Pass 1 — full-res depth -> mip 0 (half res)
//--------------------------------------------------------------------------------------
cbuffer HiZCopyCB : register( b0 )
{
    uint HZC_DepthIndex;   // SRV: full-res reversed-Z depth (R32_FLOAT view of the D32 buffer)
    uint HZC_DstIndex;     // UAV: Hi-Z mip 0
    uint HZC_Pad0;
    uint HZC_Pad1;
};

[numthreads(8, 8, 1)]
void CSCopyDepth( uint3 DTid : SV_DispatchThreadID )
{
    RWTexture2D<float> dst = ResourceDescriptorHeap[HZC_DstIndex];
    uint2 dstSize;
    dst.GetDimensions( dstSize.x, dstSize.y );
    if ( DTid.x >= dstSize.x || DTid.y >= dstSize.y )
        return;

    Texture2D<float> depthTex = ResourceDescriptorHeap[HZC_DepthIndex];
    uint2 srcSize;
    uint  srcMips;
    depthTex.GetDimensions( 0, srcSize.x, srcSize.y, srcMips );

    // mip 0 is half resolution: min over the 2x2 depth footprint, clamped so an odd source dimension
    // still folds its last row/column in (never sample outside — Load() returns 0 = "sky" there, which
    // would fake a non-occluding texel and defeat the whole pyramid).
    int2 s  = int2( DTid.xy ) * 2;
    int2 s1 = int2( min( s.x + 1, (int)srcSize.x - 1 ), min( s.y + 1, (int)srcSize.y - 1 ) );

    float d = depthTex.Load( int3( s.x, s.y, 0 ) );
    d = min( d, depthTex.Load( int3( s1.x, s.y,  0 ) ) );
    d = min( d, depthTex.Load( int3( s.x,  s1.y, 0 ) ) );
    d = min( d, depthTex.Load( int3( s1.x, s1.y, 0 ) ) );
    dst[DTid.xy] = d;
}

//--------------------------------------------------------------------------------------
// Pass 2 — mip N-1 -> mip N
//--------------------------------------------------------------------------------------
cbuffer HiZReduceCB : register( b0 )
{
    uint HZR_SrcIndex;   // UAV of the parent mip
    uint HZR_DstIndex;   // UAV of this mip
    uint HZR_Pad0;
    uint HZR_Pad1;
};

[numthreads(8, 8, 1)]
void CSReduce( uint3 DTid : SV_DispatchThreadID )
{
    RWTexture2D<float> dst = ResourceDescriptorHeap[HZR_DstIndex];
    uint2 dstSize;
    dst.GetDimensions( dstSize.x, dstSize.y );
    if ( DTid.x >= dstSize.x || DTid.y >= dstSize.y )
        return;

    RWTexture2D<float> src = ResourceDescriptorHeap[HZR_SrcIndex];
    uint2 srcSize;
    src.GetDimensions( srcSize.x, srcSize.y );

    uint2 s  = DTid.xy * 2;
    uint2 s1 = uint2( min( s.x + 1, srcSize.x - 1 ), min( s.y + 1, srcSize.y - 1 ) );

    float d = src[uint2( s.x, s.y )];
    d = min( d, src[uint2( s1.x, s.y  )] );
    d = min( d, src[uint2( s.x,  s1.y )] );
    d = min( d, src[uint2( s1.x, s1.y )] );

    // An odd parent dimension means dstSize == (srcSize+1)/2 and the LAST destination texel's 2x2 window
    // only covers one source texel in that axis — the parent's final row/column would otherwise be dropped
    // from the pyramid entirely, making the level NON-conservative (it could report a closer occluder than
    // the region really has, over-culling). Fold the leftovers into the edge texel.
    if ( ( srcSize.x & 1 ) && DTid.x == dstSize.x - 1 )
    {
        uint x = srcSize.x - 1;
        d = min( d, src[uint2( x, s.y  )] );
        d = min( d, src[uint2( x, s1.y )] );
    }
    if ( ( srcSize.y & 1 ) && DTid.y == dstSize.y - 1 )
    {
        uint y = srcSize.y - 1;
        d = min( d, src[uint2( s.x,  y )] );
        d = min( d, src[uint2( s1.x, y )] );
    }

    dst[DTid.xy] = d;
}
