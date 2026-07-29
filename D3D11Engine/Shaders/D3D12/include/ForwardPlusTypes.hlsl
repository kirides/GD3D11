#pragma once

// Forward+ tiled point-light + CSM shared types/constants — shared by World.hlsl, Vob.hlsl, Skeletal.hlsl.
// #include this BEFORE each shader's own cbuffer/resource declarations: the register slots those bind to
// (LightCB/ShadowCB live at different b-slots per shader) still belong to each file, only the *shapes* are
// common here.

// Per-frame visible point light (torches/campfires/spells), 64 B. Must stay in lockstep with the C++ GPULight
// in D3D12Engine/D3D12EngineCommon.h and with LightCull.hlsl's TiledPointLight copy.
// Forward shaders read PositionWorld/Range/Color for shading; PositionView feeds the tile cull;
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
struct LightGrid { uint Offset; uint Count; };

// ShadowCubeIndex encoding: -1 = unshadowed, else (slot | tier). Bit 30 selects the low-res static cube array
// over the full-res dynamic one, and keeps the value positive so "ShadowCubeIndex >= 0" still means shadowed.
static const int kShadowTierLow  = 0x40000000;
static const int kShadowSlotMask = 0x3FFFFFFF;

#define TILE_SIZE 16u
// Must match LightCull.hlsl's copy AND kMaxLightsPerTile in D3D12Scene.cpp (it strides RW_LightIndexList).
#define MAX_LIGHTS_PER_TILE 64u
#define NUM_CSM_CASCADES 3
