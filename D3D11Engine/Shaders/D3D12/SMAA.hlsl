// SMAA.hlsl (D3D12 wrapper)
// ---------------------------------------------------
// Connects the header-only SMAA core (include/SMAA.hlsl) to the D3D12 Forward+ backend.
// Compiled at runtime with DXC to SM6.6. Unlike the D3D11 wrapper (Shaders/SMAA_Wrapper.hlsl),
// which binds the SMAA textures through a t0..t4 descriptor table, this variant fetches every
// texture bindlessly from the shader-visible heap (ResourceDescriptorHeap[index]) so the effect
// needs only root constants + two static samplers — no per-pass descriptor-table juggling.
//
// 3 passes (mirrors D3D11PFX_SMAA / D3D11SMAA::Render exactly):
//   1. Edge detection      color        -> edges
//   2. Blend-weight calc    edges+area+search -> blend weights
//   3. Neighborhood blend   color+blend  -> final AA'd image
// ---------------------------------------------------

// 1. Configuration (matches the D3D11 wrapper: SMAA_PRESET_HIGH, custom shading-language macros).
#define SMAA_PRESET_HIGH
#define SMAA_CUSTOM_SL

// ---------------------------------------------------
// 2. Constants + bindless indices (b0 root constants; 4 floats metrics + 5 SRV heap indices).
// ---------------------------------------------------
cbuffer cbSMAA : register(b0)
{
    float4 SMAA_RT_METRICS;   // (1/w, 1/h, w, h)
    uint   ColorTexIndex;     // scene LDR color (SMAA input)
    uint   EdgesTexIndex;     // pass-1 output / pass-2 input
    uint   BlendTexIndex;     // pass-2 output / pass-3 input
    uint   AreaTexIndex;      // precomputed SMAA area LUT (R8G8)
    uint   SearchTexIndex;    // precomputed SMAA search LUT (R8)
};

// Static samplers (declared in the root signature): s0 linear-clamp, s1 point-clamp.
SamplerState LinearSampler : register(s0);
SamplerState PointSampler  : register(s1);

// ---------------------------------------------------
// 3. Custom porting macros (required for SMAA_CUSTOM_SL) — identical to the D3D11 wrapper.
// ---------------------------------------------------
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)

// ---------------------------------------------------
// 4. Include core logic.
// ---------------------------------------------------
#include "include/SMAA.hlsl"

// ---------------------------------------------------
// 5. Vertex shaders (fullscreen triangle via SV_VertexID; identical to the D3D11 wrapper).
// ---------------------------------------------------
struct VS_EdgeOut {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
    float4 Offsets[3] : TEXCOORD1;
};

struct VS_BlendOut {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
    float2 Pix : TEXCOORD1;
    float4 Offsets[3] : TEXCOORD2;
};

struct VS_NeighborOut {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
    float4 Offset : TEXCOORD1;
};

void GetQuadAttributes(uint id, out float4 pos, out float2 tex) {
    tex = float2((id << 1) & 2, id & 2);
    pos = float4(tex * float2(2, -2) + float2(-1, 1), 0, 1);
}

VS_EdgeOut EdgeDetectionVS(uint id : SV_VertexID) {
    VS_EdgeOut o;
    GetQuadAttributes(id, o.Pos, o.Tex);
    SMAAEdgeDetectionVS(o.Tex, o.Offsets);
    return o;
}

VS_BlendOut BlendingWeightCalculationVS(uint id : SV_VertexID) {
    VS_BlendOut o;
    GetQuadAttributes(id, o.Pos, o.Tex);
    SMAABlendingWeightCalculationVS(o.Tex, o.Pix, o.Offsets);
    return o;
}

VS_NeighborOut NeighborhoodBlendingVS(uint id : SV_VertexID) {
    VS_NeighborOut o;
    GetQuadAttributes(id, o.Pos, o.Tex);
    SMAANeighborhoodBlendingVS(o.Tex, o.Offset);
    return o;
}

// ---------------------------------------------------
// 6. Pixel shaders (bindless texture fetch from the shader-visible heap).
// ---------------------------------------------------
float4 LumaEdgeDetectionPS(VS_EdgeOut input) : SV_TARGET {
    Texture2D colorTex = ResourceDescriptorHeap[ColorTexIndex];
    return float4(SMAALumaEdgeDetectionPS(input.Tex, input.Offsets, colorTex), 0, 0);
}

float4 BlendingWeightCalculationPS(VS_BlendOut input) : SV_TARGET {
    Texture2D edgesTex  = ResourceDescriptorHeap[EdgesTexIndex];
    Texture2D areaTex   = ResourceDescriptorHeap[AreaTexIndex];
    Texture2D searchTex = ResourceDescriptorHeap[SearchTexIndex];
    return SMAABlendingWeightCalculationPS(input.Tex, input.Pix, input.Offsets, edgesTex, areaTex, searchTex, float4(0, 0, 0, 0));
}

float4 NeighborhoodBlendingPS(VS_NeighborOut input) : SV_TARGET {
    Texture2D colorTex = ResourceDescriptorHeap[ColorTexIndex];
    Texture2D blendTex = ResourceDescriptorHeap[BlendTexIndex];
    return SMAANeighborhoodBlendingPS(input.Tex, input.Offset, colorTex, blendTex);
}
