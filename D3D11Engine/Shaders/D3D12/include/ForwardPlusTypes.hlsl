#pragma once

// Forward+ tiled point-light + CSM shared types/constants — shared by World.hlsl, Vob.hlsl, Skeletal.hlsl.
// #include this BEFORE each shader's own cbuffer/resource declarations: the register slots those bind to
// (LightCB/ShadowCB live at different b-slots per shader) still belong to each file, only the *shapes* are
// common here.

// Per-frame visible point light (torches/campfires/spells). Byte-identical to D3D11 TiledPointLight (48 B);
// forward shaders read PositionWorld/Range/Color — PositionView/ShadowCubeIndex feed the cull/shadow paths.
struct GPULight { float3 PositionView; float Range; float4 Color; float3 PositionWorld; int ShadowCubeIndex; };
struct LightGrid { uint Offset; uint Count; };

#define TILE_SIZE 16u
#define MAX_LIGHTS_PER_TILE 32u
#define NUM_CSM_CASCADES 3
