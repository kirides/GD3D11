// Single-VOB inventory-item preview (GInventory::DrawVobSingle). Mirrors D3D11's VS_Ex + PS_Preview_Textured
// (RENDERMODE==1: plain textured, alpha-clip, no lighting/fog) — default (column-major) matrix packing, same
// as World.hlsl/Vob.hlsl, so mul(float4(pos,1), M) is byte-for-byte identical to the row-major upload.
cbuffer ViewProjCB : register(b0) { float4x4 ViewProj; };
cbuffer WorldCB    : register(b1) { float4x4 World; };

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

struct VS_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    float3 worldPos = mul( float4( i.pos, 1.0 ), World ).xyz;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv = i.uv;
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    return t;
}

// Ghost/transparency VOBs (D3D12PipelineState::CreateGhost): reuses VSMain's single-object World/ViewProj
// layout. Mirrors D3D11's PS_Transparency — unlit diffuse sample, alpha multiplied by a per-vob fade factor,
// no alpha-clip (a fading ghost should smoothly disappear, not pop).
#include "include/GhostCB.hlsl"

// Verbatim copy of include/PBRLighting.hlsl's helper (that header pulls in the whole Forward+ lighting
// stack, which this standalone shader has no root signature for). NOTE the `select` — a vector ternary is
// a hard error under SM6.
float3 SrgbToLinear( float3 c )   // accurate sRGB EOTF
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

float4 PSGhost( VS_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    // Linearize: this blends into m_SceneColor, which is a LINEAR HDR target on D3D12. D3D11's PS_Transparency
    // returns the raw texel because its HDR buffer is gamma-space (PS_PFX_HDR pow(...,2.2)s at the end), so a
    // verbatim port reads far too bright here — the same reason Decal/Fx/Particle/World/Vob all linearize.
    return float4( SrgbToLinear( t.rgb ), t.a * GhostAlpha );
}
