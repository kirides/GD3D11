//--------------------------------------------------------------------------------------
// Compute Shader - Generate smooth view-space normals from depth
//
// Optional Forward+ helper: reconstructs a per-pixel view-space normal purely from the
// depth buffer and writes it octahedral-encoded (R16G16_FLOAT), matching the GBuffer
// normal encoding (EncodeNormalGBuffer). The AO producers (SAO / ASSAO) can then consume
// these normals exactly like the deferred GBuffer normals.
//
// "Smooth" normals: instead of a naive ddx/ddy cross product (which bleeds across
// silhouettes), we pick, per axis, the neighbour whose reconstructed position is closest
// to the centre. This keeps edges crisp while averaging out depth-quantization noise.
//--------------------------------------------------------------------------------------

#include "DS_Defines.h"
#include "DepthReconstruction.h"

cbuffer AONormalsConstantBuffer : register( b0 )
{
    float4 AON_ProjParams;    // x = 1/P._11, y = 1/P._22, z = P._34, w = P._33
    float2 AON_InvResolution; // 1/width, 1/height
    float2 AON_Pad;
};

SamplerState SS_Linear : register( s0 );
Texture2D    TX_Depth  : register( t0 );

RWTexture2D<float2> OutputNormals : register( u0 );

float3 VSPositionAt( int2 pixel, uint2 size )
{
    pixel = clamp( pixel, int2( 0, 0 ), int2( size ) - 1 );
    float2 texcoord = ( float2( pixel ) + 0.5 ) * AON_InvResolution;
    float rawDepth = TX_Depth.SampleLevel( SS_Linear, texcoord, 0 ).r;
    return ReconstructVSPositionFromDepthReverseZInfinite( rawDepth, texcoord, AON_ProjParams.xy );
}

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outSize;
    OutputNormals.GetDimensions( outSize.x, outSize.y );

    if ( DTid.x >= outSize.x || DTid.y >= outSize.y )
        return;

    int2 p = int2( DTid.xy );
    float3 c  = VSPositionAt( p,             outSize );

    // Sky / far plane: emit a flat up-facing normal (value is irrelevant, AO skips sky)
    if ( c.z > 50000.0 )
    {
        OutputNormals[DTid.xy] = EncodeNormalGBuffer( float3( 0, 0, 1 ) );
        return;
    }

    float3 l  = VSPositionAt( p + int2( -1,  0 ), outSize );
    float3 r  = VSPositionAt( p + int2(  1,  0 ), outSize );
    float3 d  = VSPositionAt( p + int2(  0, -1 ), outSize );
    float3 u  = VSPositionAt( p + int2(  0,  1 ), outSize );

    // Choose the horizontal / vertical neighbour closest in view-space to avoid
    // reconstructing a normal across a depth discontinuity (silhouette bleed).
    float3 dpdx = ( abs( r.z - c.z ) < abs( c.z - l.z ) ) ? ( r - c ) : ( c - l );
    float3 dpdy = ( abs( u.z - c.z ) < abs( c.z - d.z ) ) ? ( u - c ) : ( c - d );

    float3 normal = normalize( cross( dpdx, dpdy ) );

    // View-space normals should face the camera (view dir = -pos). Flip if needed.
    if ( dot( normal, -normalize( c ) ) < 0.0 )
        normal = -normal;

    OutputNormals[DTid.xy] = EncodeNormalGBuffer( normal );
}
