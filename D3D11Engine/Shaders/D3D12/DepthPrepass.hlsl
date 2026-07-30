cbuffer WorldCB : register(b0) { float4x4 ViewProj; };   // default column-major packing (see world shader)
Texture2D    tx  : register(t0);                          // CSM shadow caster (PSShadowClip) still binds diffuse here
SamplerState smp : register(s0);
// b6 material indices (ExecuteIndirect, P2.11): the world depth prepass (PSClip) samples its diffuse bindless so
// it can be driven by ExecuteIndirect like the color pass. Only MatDiffuseIndex (DWORD 2) is read here; the
// normal/ORM slots share the layout with the world color shader's MaterialCB so ONE command signature drives both.
cbuffer MaterialCB : register(b6) { uint _matN; uint _matO; uint MatDiffuseIndex; };

// World mesh: single stream, packed 36-byte ExVertexStructGPU. Only Position (@0) + TexCoord0 (@20) are
// fetched here; the normal/tangent/uv2/color fields are not needed for a depth+alpha-clip pass.
struct VS_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };

VS_OUT VSWorld( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );   // world verts are already world-space (identity world)
    o.uv   = i.uv;
    return o;
}

float4 PSClip( VS_OUT i ) : SV_TARGET
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];   // bindless diffuse (ExecuteIndirect, P2.11)
    float4 t = difTex.Sample( smp, i.uv );
    clip( t.a - 0.5 );          // same cutout as the opaque world PS so gaps don't lay down depth
    return float4( 0, 0, 0, 1 );   // discarded: the PSO's color write mask is 0 (depth-only pass)
}

// Shadow caster (P2.9c): void PS (no SV_Target) so the depth-only shadow PSO binds NO render target without a
// validation warning. Only alpha-clips foliage/fence cutouts so their gaps don't cast solid shadows.
void PSShadowClip( VS_OUT i )
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];
    clip( difTex.Sample( smp, i.uv ).a - 0.5 );}

// --- G-buffer prepass variant: depth + motion vectors + normals ---
// Separate entry points rather than an extension of VSWorld/PSClip above, because those two blobs are ALSO what
// the CSM cascade world caster is built from (D3D12ShadowMap), and that PSO binds no render targets and does not
// bind b5 — emitting velocity/normal from the shared VS would leave its MotionCB root parameter undefined.
//
// World-mesh vertices are STATIC and already world-space, so the previous position is the current position: all
// of the resulting velocity is camera motion, and it is EXACT (not the depth-reprojection approximation that
// FillCameraVelocity falls back to for pixels no prepass draw covered). The packed normal at offset 12 is
// already octahedral world-space, so it is passed through untouched — no decode/re-encode round trip.
#include "include/MotionVectors.hlsl"

struct VS_GBUF_IN  { float3 pos : POSITION; float2 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VS_GBUF_OUT
{
    float4 clip     : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float2 octNrm   : TEXCOORD1;   // already-encoded octahedral normal, straight from the vertex
    float4 currClip : TEXCOORD2;
    float4 prevClip : TEXCOORD3;
};

VS_GBUF_OUT VSWorldGBuf( VS_GBUF_IN i )
{
    VS_GBUF_OUT o;
    o.clip     = mul( float4( i.pos, 1.0 ), ViewProj );
    o.uv       = i.uv;
    o.octNrm   = i.nrm;
    o.currClip = mul( float4( i.pos, 1.0 ), UnjitteredViewProj );
    o.prevClip = mul( float4( i.pos, 1.0 ), PrevViewProj );   // static geometry: same world position last frame
    return o;
}

GBUF_OUT PSClipGBuf( VS_GBUF_OUT i )
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];
    clip( difTex.Sample( smp, i.uv ).a - 0.5 );   // identical cutout to PSClip — same depth, same coverage
    GBUF_OUT o;
    o.velocity = CalculateVelocity( i.currClip, i.prevClip );
    o.normal   = i.octNrm;   // interpolating an octahedral pair is fine; XeGTAO renormalizes on decode
    return o;
}
