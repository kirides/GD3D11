cbuffer CubeCB : register(b0) { float4x4 PCR_ViewProj[6]; };   // per-light face view-projs (90-deg perspective, near 15, far range*2)
Texture2D    tx  : register(t0);
SamplerState smp : register(s0);
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; uint rt : SV_RenderTargetArrayIndex; };

// World caster: one draw = 6 instances, instanceID selects the face view-proj AND the target cube slice. Also
// draws skinned meshes, which SkinVertices.hlsl has already posed in world space.
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

// Depth-only (void PS, no SV_Depth) so the caster keeps early-Z / Hi-Z rejection and the PSO's hardware slope-
// scaled depth bias — the stored depth is the NATURAL hyperbolic z of the 90-deg perspective, which the sampler
// reconstructs from the dominant-axis distance (more efficient than writing linear SV_Depth). Alpha-clip cutouts.
void PSCubeClip( VS_OUT i ) { clip( tx.Sample( smp, i.uv ).a - 0.5 ); }
