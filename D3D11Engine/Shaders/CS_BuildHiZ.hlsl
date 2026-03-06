//--------------------------------------------------------------------------------------
// Hi-Z Pyramid Build Compute Shader
// Builds a MAX-depth mip chain for hierarchical occlusion culling (reversed-Z).
// Each mip texel stores the NEAREST depth (highest reversed-Z) in its 2x2 source region.
// Mip 0: copy from depth buffer.
// Mip N>0: 2x2 MAX downsample from previous mip.
//
// D3D11 forbids binding the same resource as both SRV and UAV, so we use a
// scratch texture as the UAV target, then CopySubresourceRegion into the
// real Hi-Z texture after each dispatch.
//--------------------------------------------------------------------------------------

cbuffer HiZCB : register( b0 )
{
    uint outputWidth;
    uint outputHeight;
    uint inputMipLevel;
    uint isCopyPass;    // 1 = mip 0 (copy from depth), 0 = downsample
};

Texture2D<float> InputTexture : register( t0 );
RWTexture2D<float> OutputTexture : register( u0 );

[numthreads( 8, 8, 1 )]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    if ( DTid.x >= outputWidth || DTid.y >= outputHeight )
        return;

    if ( isCopyPass )
    {
        // Mip 0: straight copy from the depth buffer (reversed-Z, so 0 = far)
        OutputTexture[DTid.xy] = InputTexture.Load( int3( DTid.xy, 0 ) );
    }
    else
    {
        // 2x2 MAX downsample from the previous mip level of the Hi-Z texture.
        // With reversed-Z depth (near=1, far=0), we take the MAX to get the
        // NEAREST (closest to camera) surface per tile.
        //
        // CS_CullVobs then takes the MIN across footprint texels of this MAX chain,
        // finding the least-occluded tile in the AABB's screen projection.
        // The test "maxDepth < hiZDepth" passes only when the AABB's nearest corner
        // (maxDepth) is farther than the nearest occluder in every tile of the footprint.
        //
        // Using MIN here instead would collapse every tile touching the sky to ~0,
        // making the test never fire since depth values are non-negative.
        uint2 srcBase = DTid.xy * 2;

        float d00 = InputTexture.Load( int3( srcBase + uint2( 0, 0 ), inputMipLevel ) );
        float d10 = InputTexture.Load( int3( srcBase + uint2( 1, 0 ), inputMipLevel ) );
        float d01 = InputTexture.Load( int3( srcBase + uint2( 0, 1 ), inputMipLevel ) );
        float d11 = InputTexture.Load( int3( srcBase + uint2( 1, 1 ), inputMipLevel ) );

        OutputTexture[DTid.xy] = max( max( d00, d10 ), max( d01, d11 ) );
    }
}
