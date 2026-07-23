cbuffer CubeCB : register(b0) { float4x4 PCR_ViewProj[6]; };   // per-light face view-projs (90-deg perspective, near 15, far range*2)
// Skeletal-only CBs (b1/b2). Unreferenced by VSCube/VSCubeVob, so they're stripped from those shaders — the
// world/VOB caster PSOs' root sig only declares b0/t0/s0. They match the main skeletal InstanceCB/BonesCB so
// the cube caster can bind the SAME per-frame instance/bone CBs (d.instCb/d.boneCb) the sun/color passes use.
cbuffer SkelInstanceCB : register(b1) { float4x4 M_World; float4 ModelColor; float Fatness; float3 _spad; };
cbuffer SkelBonesCB    : register(b2) { float4x4 Bones[96]; };
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
struct VSVOB_IN { float3 pos : POSITION; float2 uv : TEXCOORD0; float4x4 iworld : INSTANCE_WORLD_MATRIX; uint iid : SV_InstanceID; };
VS_OUT VSCubeVob( VSVOB_IN i )
{
    VS_OUT o;
    uint   face = i.iid % 6u;
    float3 wp   = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;
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
    float3 sp = float3( 0, 0, 0 );
    float3 sn = float3( 0, 0, 0 );
    [unroll]
    for ( int b = 0; b < 4; ++b )
    {
        float4x4 bone = Bones[i.boneIndices[b]];
        float    w    = i.weights[b];
        sp += w * mul( float4( i.pos[b].xyz, 1.0 ), bone ).xyz;
        sn += w * mul( i.normal, (float3x3)bone );
    }
    float3 wp = mul( float4( sp + Fatness * sn, 1.0 ), M_World ).xyz;
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
