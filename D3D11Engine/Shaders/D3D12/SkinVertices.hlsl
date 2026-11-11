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
#include "include/VertexFetch.hlsl"     // LoadSkel76, PackSnorm2x16

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

    const SkelVertexIn sv = LoadSkel76( SrcVerts, job.SrcBase + v );
    const float4 pos[4]  = sv.pos;
    const float3 normal  = sv.normal;
    const float2 uv      = sv.uv;
    const uint4  bones   = sv.bones;
    const float4 weights = sv.weights;

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
