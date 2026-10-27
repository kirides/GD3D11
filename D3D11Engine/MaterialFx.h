#pragma once
#include "Types.h"
#include <functional>

class zCMaterial;
class zCTexture;

// ZenGin's per-material stages on top of the base texture: texAniMap UV scroll (every material group,
// zCMaterial::ApplyTexAniMapping), vertex waves (zCPolygon::ApplyMorphing) and, on objects, the
// environmentalMapping overlay and the detail texture (zRenderManager.cpp). Water-shaded water keeps its own
// TexCoord2 scroll and vertex-color waves.
namespace MaterialFx {
    // D3D12: a material's slot in the MaterialFx table rides in the bindless diffuse index's top 16 bits;
    // 0 = none. Every shader reading a diffuse index masks with kDiffuseSlotMask.
    constexpr uint32_t kSlotShift = 16;
    constexpr uint32_t kDiffuseSlotMask = 0x0000FFFFu;
    constexpr uint32_t kMaxSlots = 4096;

    // Mirrors Shaders/D3D12/include/MaterialFx.hlsl. Entry 0 carries the frame globals instead.
    struct Entry {
        float ScrollU, ScrollV;
        float EnvStrength;      // < 0: additive (water)
        float DetailScale;      // 0 = no detail texture
        uint32_t DetailSlot;
        uint32_t EnvCubeSlot;   // entry 0: the reflection cube, 0xFFFFFFFF = env mapping off
        float EnvGlobal;        // entry 0: sky-fog luma * EnvMappingStrength
        float WaveTimeMs;       // entry 0: this frame's wave clock
        float Wave[4];          // see GetWave; entry 0: [0] = the previous frame's wave clock
    };
    static_assert( sizeof( Entry ) == 48, "must match MaterialFx.hlsl" );

    /** This frame's UV offset; false (offset untouched) when the material doesn't scroll. */
    bool GetOffset( zCMaterial* mat, float2& offset );

    /** The env overlay strength of an object material, negative when additive; 0 without env mapping or with it off. */
    float GetEnvStrength( zCMaterial* mat );

    /** Sky-fog luma times the user strength, 0 with env mapping off: ZenGin's stage alpha is strength * this. */
    float GetEnvGlobal();

    /** Whether the material has a waveMode the non-water shaders animate. Ignores settings, so it is stable. */
    bool HasWaves( zCMaterial* mat );

    /** { max amplitude, grid size, zTFFT time scale, 1 = wall mode (pushes x and z) } of a waving material;
        false when it doesn't wave or WaterWaves is off. */
    bool GetWave( zCMaterial* mat, float4& wave );

    /** The wave clock (total ms) of this frame and of the previous one, for motion vectors. Main thread. */
    void GetWaveTimes( float& nowMs, float& prevMs );

    /** The material's detail texture and its uv scale; nullptr when it has none or detail textures are off. */
    zCTexture* GetDetailTexture( zCMaterial* mat, float& scale );

    /** `diffuseSlot` with the material's table slot packed in; unchanged when it has no stage. Thread-safe. */
    uint32_t PackDiffuseIndex( uint32_t diffuseSlot, zCMaterial* mat );

    /** Refreshes the first `count` entries; resolveTexture gives a detail texture's bindless slot (UINT32_MAX = not
        resident). Returns whether they differ from the previous call. Main thread. */
    bool FillTable( Entry* table, uint32_t& count, uint32_t envCubeSlot, const std::function<uint32_t( zCTexture* )>& resolveTexture );

    void OnMaterialDeleted( zCMaterial* mat );
}
