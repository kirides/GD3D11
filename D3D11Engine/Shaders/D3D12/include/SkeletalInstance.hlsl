#ifndef SKELETAL_INSTANCE_HLSL
#define SKELETAL_INSTANCE_HLSL
// Per-frame skeletal data: every instance record and bone palette of the frame in one float4 buffer (the
// skeletal ring, bound once per pass), addressed by row. SkelInstanceRow, a per-draw root constant, selects the
// draw's instance record, so a skeletal ExecuteIndirect command is root constants + a draw and nothing else.
//
// Record layout (SkeletalInstanceGPU in D3D12Scene.cpp): rows 0-3 World, 4-7 PrevWorld, 8 ModelColor,
// 9 { Fatness, BoneRow, PrevBoneRow, - }. A bone is 4 rows at BoneRow + 4 * index. Matrices are applied as
// mul(M, v) with row k = stored row k; the fourth row is never needed, since only .xyz of the result is used.
#ifndef SKELDATA_REGISTER
#define SKELDATA_REGISTER t3
#endif
#ifndef SKELDRAWCB_REGISTER
#define SKELDRAWCB_REGISTER b10
#endif
StructuredBuffer<float4> SkelData : register(SKELDATA_REGISTER);
cbuffer SkelDrawCB : register(SKELDRAWCB_REGISTER) { uint SkelInstanceRow; };

struct SkeletalInstance
{
    float3x4 World;
    float3x4 PrevWorld;
    float4   ModelColor;
    float    Fatness;
    uint     BoneRow;
    uint     PrevBoneRow;   // == BoneRow's pose copy when the vob has no history
};

float3x4 SkelMatrix( uint row )
{
    return float3x4( SkelData[row], SkelData[row + 1], SkelData[row + 2] );
}

SkeletalInstance LoadSkelInstanceAt( uint r )
{
    const float4 misc = SkelData[r + 9];
    SkeletalInstance s;
    s.World       = SkelMatrix( r );
    s.PrevWorld   = SkelMatrix( r + 4 );
    s.ModelColor  = SkelData[r + 8];
    s.Fatness     = misc.x;
    s.BoneRow     = asuint( misc.y );
    s.PrevBoneRow = asuint( misc.z );
    return s;
}

SkeletalInstance LoadSkelInstance() { return LoadSkelInstanceAt( SkelInstanceRow ); }

// Matrix-palette skin of one vertex in model space; pos[b] is the vertex baked into bone b's space.
void SkinVertex( float4 pos[4], float3 normal, uint4 bones, float4 weights, uint boneRow,
                 out float3 skinnedPos, out float3 skinnedNormal )
{
    skinnedPos    = float3( 0, 0, 0 );
    skinnedNormal = float3( 0, 0, 0 );
    [unroll]
    for ( int b = 0; b < 4; ++b )
    {
        const float3x4 bone = SkelMatrix( boneRow + bones[b] * 4 );
        skinnedPos    += weights[b] * mul( bone, float4( pos[b].xyz, 1.0 ) );
        skinnedNormal += weights[b] * mul( (float3x3)bone, normal );
    }
}

// Model space -> world; Fatness inflates along the skinned normal (D3D11's PI_ModelFatness).
float3 SkelToWorld( float3x4 world, float3 skinnedPos, float3 skinnedNormal, float fatness )
{
    return mul( world, float4( skinnedPos + fatness * skinnedNormal, 1.0 ) );
}
#endif
