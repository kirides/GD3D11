// Compute skinning (D3D12Skinning.cpp): every skinned sub-mesh the frame prepared is posed ONCE here, into two
// world-space vertex streams that the prepass, the lit pass, the CSM cascades and the point-light cubes then draw
// like static geometry (the skeletal arena's index buffer, BaseVertexLocation = the job's DstBase).
//
//   OutPosUv   (20 B): float3 world position, float2 uv                  — all a depth/shadow pass fetches
//   OutNrmPrev (16 B): R16G16_SNORM octahedral world normal, float3 previous-frame world position
//
// One job per (vob, sub-mesh); thread groups are mapped to jobs through GroupJobs, so one dispatch covers the frame.
#include "include/SkeletalInstance.hlsl"
#define MOTIONCB_REGISTER b1
#include "include/MotionVectors.hlsl"   // EncodeOctNormal

cbuffer SkinCB : register( b0 )
{
    uint TotalGroups;
    uint GroupsPerRow;   // dispatch X; groups past 65535 wrap into Y
    uint WritePrev;      // 0 = motion vectors off: the previous position is just the current one
    uint _skinPad;
};

// Mirrors SkinJob in D3D12Skinning.cpp (16 B).
struct SkinJob
{
    uint SrcBase;       // first vertex in the skeletal arena
    uint VertexCount;
    uint DstBase;       // first vertex in the output streams
    uint InstanceRow;   // SkeletalInstanceGPU row in SkelData
};
StructuredBuffer<SkinJob> Jobs      : register( t0 );
StructuredBuffer<uint2>   GroupJobs : register( t1 );   // { job index, first vertex of this group within the job }
ByteAddressBuffer         SrcVerts  : register( t2 );   // the skeletal arena: 76-byte ExSkelVertexStruct

RWByteAddressBuffer OutPosUv   : register( u0 );
RWByteAddressBuffer OutNrmPrev : register( u1 );

static const uint kSrcStride = 76;

float4 LoadHalf4( uint address )
{
    const uint2 h = SrcVerts.Load2( address );
    return float4( f16tof32( h.x ), f16tof32( h.x >> 16 ), f16tof32( h.y ), f16tof32( h.y >> 16 ) );
}

uint PackSnorm2x16( float2 v )
{
    const int2 i = int2( round( clamp( v, -1.0, 1.0 ) * 32767.0 ) );
    return ( uint( i.x ) & 0xFFFFu ) | ( uint( i.y ) << 16 );
}

[numthreads( 64, 1, 1 )]
void CSSkin( uint3 gid : SV_GroupID, uint gi : SV_GroupIndex )
{
    const uint group = gid.y * GroupsPerRow + gid.x;
    if ( group >= TotalGroups )
        return;
    const uint2 gj = GroupJobs[group];
    const SkinJob job = Jobs[gj.x];
    const uint v = gj.y + gi;
    if ( v >= job.VertexCount )
        return;

    // ExSkelVertexStruct: Position[4] half4 @0, Normal float3 @32, BindPoseNormal @44, TexCoord @56,
    // boneIndices uint8x4 @64, weights half4 @68.
    const uint src = ( job.SrcBase + v ) * kSrcStride;
    float4 pos[4];
    [unroll]
    for ( uint b = 0; b < 4; ++b )
        pos[b] = LoadHalf4( src + b * 8 );
    const float3 normal  = asfloat( SrcVerts.Load3( src + 32 ) );
    const float2 uv      = asfloat( SrcVerts.Load2( src + 56 ) );
    const uint   boneIds = SrcVerts.Load( src + 64 );
    const uint4  bones   = uint4( boneIds & 0xFF, ( boneIds >> 8 ) & 0xFF, ( boneIds >> 16 ) & 0xFF, boneIds >> 24 );
    const float4 weights = LoadHalf4( src + 68 );

    const SkeletalInstance inst = LoadSkelInstanceAt( job.InstanceRow );
    float3 skinnedPos, skinnedNormal;
    SkinVertex( pos, normal, bones, weights, inst.BoneRow, skinnedPos, skinnedNormal );
    const float3 worldPos = SkelToWorld( inst.World, skinnedPos, skinnedNormal, inst.Fatness );
    // Rigid + ~uniform scale, as the vertex shaders always assumed.
    const float3 worldNormal = mul( (float3x3)inst.World, skinnedNormal );

    // Fatness is applied to both poses (it inflates along the normal and is itself animated for some monsters),
    // mirroring D3D11's ApplySkinningPrevious feeding PI_ModelFatness * prevNormal into M_PrevWorld.
    float3 prevPos = worldPos;
    if ( WritePrev != 0 )
    {
        float3 prevSkinned, prevNormal;
        SkinVertex( pos, normal, bones, weights, inst.PrevBoneRow, prevSkinned, prevNormal );
        prevPos = SkelToWorld( inst.PrevWorld, prevSkinned, prevNormal, inst.Fatness );
    }

    const uint dst = job.DstBase + v;
    OutPosUv.Store3( dst * 20, asuint( worldPos ) );
    OutPosUv.Store2( dst * 20 + 12, asuint( uv ) );
    OutNrmPrev.Store( dst * 16, PackSnorm2x16( EncodeOctNormal( worldNormal ) ) );   // L1-normalizes itself
    OutNrmPrev.Store3( dst * 16 + 4, asuint( prevPos ) );
}
