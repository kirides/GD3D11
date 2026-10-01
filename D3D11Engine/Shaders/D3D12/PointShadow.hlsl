cbuffer CubeCB : register(b0) { float4x4 PCR_ViewProj[6]; };   // per-light face view-projs (90-deg perspective, near 15, far range*2)
// Skeletal-only t3/b10, the same instance records the sun/color passes read (include/SkeletalInstance.hlsl).
// Unreferenced by VSCube/VSCubeVob, so the world/VOB caster PSOs' root sig only declares b0/t0/s0.
#include "include/SkeletalInstance.hlsl"
Texture2D    tx  : register(t0);
SamplerState smp : register(s0);
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; uint rt : SV_RenderTargetArrayIndex; };

// World caster: one draw = 6 instances, instanceID selects the face view-proj AND the target cube slice.
struct VS_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; uint iid : SV_InstanceID; };
VS_OUT VSCube( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), PCR_ViewProj[i.iid] );   // 90-deg perspective from the light, per face
    o.uv   = i.uv;
    o.rt   = i.iid;   // face 0..5 → cube slice (relative to the bound slot's 6-slice DSV)
    return o;
}

// VOB caster: instanceID spans (numInstances * 6). The per-instance world stream uses InstanceDataStepRate=6, so
// each real instance is fetched for 6 consecutive instanceIDs; face = iid % 6 picks the face view-proj + slice.
// iworld is row_major (applied as mul(M, v)) so element k is row k in both DXIL and SPIR-V.
struct VSVOB_IN { float3 pos : POSITION; float2 uv : TEXCOORD0; row_major float4x4 iworld : INSTANCE_WORLD_MATRIX; uint iid : SV_InstanceID; };
VS_OUT VSCubeVob( VSVOB_IN i )
{
    VS_OUT o;
    uint   face = i.iid % 6u;
    float3 wp   = mul( i.iworld, float4( i.pos, 1.0 ) ).xyz;
    o.clip = mul( float4( wp, 1.0 ), PCR_ViewProj[face] );
    o.uv   = i.uv;
    o.rt   = face;
    return o;
}

// Skeletal caster: 6 instances → face = iid. Matrix-palette skin (matches the main/sun VSDepth incl. Fatness) so
// the cast depth is bit-consistent with the color pass, then project through the face's 90-deg view-proj.
struct VSSKEL_IN { float4 pos[4] : POSITION; float3 normal : NORMAL; float3 bindPoseNormal : TEXCOORD0; float2 uv : TEXCOORD1; uint4 boneIndices : BONEIDS; float4 weights : WEIGHTS; uint iid : SV_InstanceID; };
VS_OUT VSCubeSkel( VSSKEL_IN i )
{
    const SkeletalInstance inst = LoadSkelInstance();
    float3 sp, sn;
    SkinVertex( i.pos, i.normal, i.boneIndices, i.weights, inst.BoneRow, sp, sn );
    float3 wp = SkelToWorld( inst.World, sp, sn, inst.Fatness );
    VS_OUT o;
    o.clip = mul( float4( wp, 1.0 ), PCR_ViewProj[i.iid] );
    o.uv   = i.uv;
    o.rt   = i.iid;
    return o;
}

// Depth-only (void PS, no SV_Depth) so the caster keeps early-Z / Hi-Z rejection and the PSO's hardware slope-
// scaled depth bias — the stored depth is the NATURAL hyperbolic z of the 90-deg perspective, which the sampler
// reconstructs from the dominant-axis distance (more efficient than writing linear SV_Depth). Alpha-clip cutouts.
void PSCubeClip( VS_OUT i ) { clip( tx.Sample( smp, i.uv ).a - 0.5 ); }
