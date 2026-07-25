cbuffer WorldCB : register(b0) { float4x4 ViewProj; };   // default column-major packing (see world shader)
cbuffer FogCB   : register(b1) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
cbuffer WaterCB : register(b2) { float TotalTime; float WaterAlpha; float2 _wpad; };

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

float3 SrgbToLinear( float3 c )   // accurate sRGB EOTF — linearize gamma-encoded albedo so lighting is done in linear space
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

struct VS_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; float2 scroll : TEXCOORD1; float4 col : DIFFUSE; };
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float fogDist : TEXCOORD2; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
    float2 ani = i.scroll * TotalTime;   // scroll delta (TexCoord2) * total time (ms), like VS_ExWater
    ani -= floor( ani );                 // wrap to [0,1) so the float stays precise over long sessions
    o.uv = i.uv + ani;
    o.col = i.col;
    o.fogDist = length( i.pos - CamPosWS );
    return o;
}

// --- Depth-only prepass (mirrors D3D11's "DrawWaterSurfaces::ZPrepass": same VS transform, null PS,
// color writes masked off, depth-write ON). Water is drawn depth-read-only in the color pass, so without
// this the main depth buffer keeps the OPAQUE geometry behind the water surface (or the far plane over
// open ocean) — and every later pass that reconstructs world position from depth (height fog, god rays)
// then fogs the sea floor / sky instead of the water surface. Also makes overlapping water surfaces blend
// once instead of stacking, since the color pass's GREATER_EQUAL test now only passes on the nearest layer.
// No alpha clip here (unlike the world/VOB prepass): water translucency comes from the WaterAlpha uniform,
// not from the diffuse texture's alpha, so clipping on it would punch holes into the depth.
//
// The prepass PSO reuses **VSMain verbatim** (like D3D11 reuses VS_ExWater for its Z-prepass) rather than a
// leaner position-only VS. That is deliberate and load-bearing: a separate VS is only *algebraically* equal,
// and DXC/the driver may emit a different instruction sequence for the same matrix multiply, so the two
// passes can disagree by an ULP. Gothic's water is full of coplanar/near-coplanar overlapping surfaces (two
// water materials meeting, double-sided quads), and a 1-ULP disagreement there makes the color pass'
// GREATER_EQUAL test reject the layer the prepass accepted on some pixels but not others — i.e. exactly the
// z-fighting/shimmering patchwork of stacked water textures. Same VS => bit-identical depth => coplanar
// layers all compare EQUAL and blend, stable. Only the PS below is swapped out.
void PSDepth( float4 clip : SV_POSITION ) {}   // subset of VS_OUT's signature; writes nothing

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    float3 rgb = SrgbToLinear( t.rgb ) * i.col.bgr;   // linearize (HDR buffer is linear)
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    rgb = lerp( rgb, SrgbToLinear( FogColor ), f );
    return float4( rgb, WaterAlpha );
}
