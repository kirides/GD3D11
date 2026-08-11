#pragma once

// Forward+ tiled point-light + CSM shared types/constants — shared by World.hlsl, Vob.hlsl, Skeletal.hlsl.
// #include this BEFORE each shader's own cbuffer/resource declarations: the register slots those bind to
// (LightCB/ShadowCB live at different b-slots per shader) still belong to each file, only the *shapes* are
// common here.

#define TILE_SIZE 16u
// Clustered Forward+ (P2.14): a global cap on SIMULTANEOUSLY-SHADED point lights, not a per-tile cap. The
// frame's light buffer (kMaxFrameLights in D3D12Scene.cpp, sorted nearest-to-camera) feeds only its first
// MAX_ACTIVE_LIGHTS entries into the cluster bitmask — bit i IS light i, so this is load-bearing on both
// sides, not just a cap; lights past this rank in the sorted buffer never light anything. Must match
// kMaxFrameLights in D3D12Scene.cpp. LightCull.hlsl also packs a light index into 10 bits of its candidate
// list, so raising this past 1024 needs that packing widened too.
// Raised from 64 (P2.14's initial cap clipped visibly in ambient-heavy scenes — Gothic can have 400+
// simultaneously visible point lights once static "atmospheric" fill lights are counted) to 1024 = 32 * 32-bit
// words. Each cluster's mask is MASK_WORDS * 4 bytes: at 1080p (~8160 16x16 tiles * NUM_Z_SLICES=16 slices =
// 130560 clusters) that's ~16.7 MB total — small next to a single HDR scene target, so still "low" by this
// codebase's standards despite the 16x jump from the 64-bit original.
#define MAX_ACTIVE_LIGHTS 1024u
#define MASK_WORDS (MAX_ACTIVE_LIGHTS / 32u)
// Cluster Z slice count (log-distributed over [NearZ, FarZ], see LightCB's NearZ/FarZ/ProjA/ProjB and
// PBRLighting.hlsl's ComputeZSlice). Must match kNumZSlices in D3D12Scene.cpp — it sizes the LightGrid buffer
// and both sides derive the SAME cluster index from it, so a mismatch silently misindexes every cluster.
#define NUM_Z_SLICES 16u
#define NUM_CSM_CASCADES 3

// Per-frame visible point light (torches/campfires/spells), 64 B. Must stay in lockstep with the C++ GPULight
// in D3D12Engine/D3D12EngineCommon.h and with LightCull.hlsl's TiledPointLight copy.
// Forward shaders read PositionWorld/Range/Color for shading (Color.w = 0 for an isStatic() light, and is used as
// the specular scale so those diffuse-only fill lights cast no highlight); PositionView feeds the tile cull;
// ShadowCubeIndex/ShadowOrigin/ShadowRange feed the point-shadow lookup.
//
// ShadowOrigin/ShadowRange are the CUBE's centre and far-plane basis, which are NOT the light's own whenever
// several co-located static lights share one cube (see the clustering in BuildFrameLightBuffer). Shading
// always uses PositionWorld/Range; only SamplePointShadow uses the Shadow* pair.
struct GPULight {
    float3 PositionView; float Range;
    float4 Color;
    float3 PositionWorld; int ShadowCubeIndex;
    float3 ShadowOrigin;  float ShadowRange;
};
// Clustered Forward+ grid cell: a MAX_ACTIVE_LIGHTS-bit membership mask over the frame's active light list (bit
// i set = light i is visible in this cluster), NOT an {Offset,Count} index slice — see LightCull.hlsl/
// PBRLighting.hlsl's AccumTiledPointLights. (MASK_WORDS + 1) * 4 bytes per cluster.
//
// WordOccupancy is a summary of Mask: bit w is set iff Mask[w] != 0, so a lit pixel bit-scans one word instead
// of testing all MASK_WORDS to discover an empty cluster (the common case — BuildFrameLightBuffer sorts
// nearest-first, so the set bits bunch into the low words). The loop bound stays a popcount, so a corrupt grid
// entry cannot spin away. MASK_WORDS is 32, so the summary fits exactly one word.
struct LightGrid { uint WordOccupancy; uint Mask[MASK_WORDS]; };

// ShadowCubeIndex encoding: -1 = unshadowed, else (slot | tier). Bit 30 selects the low-res static cube array
// over the full-res dynamic one, and keeps the value positive so "ShadowCubeIndex >= 0" still means shadowed.
static const int kShadowTierLow  = 0x40000000;
// Bit 29: the slot also has a valid dynamic (skeletal overlay) cube — sample it too and min the two results.
// Full-res slots only; absent means a pure static shadow and no second sample. Mirrors D3D12EngineCommon.h.
static const int kShadowHasDynamic = 0x20000000;
static const int kShadowSlotMask = 0x1FFFFFFF;
