#pragma once
#include "Types.h"

class zCMaterial;

// Material texAniMapMode LINEAR: ZenGin scrolls the base UVs by frac(texAniMapDir * totalTime) for every
// material group (zCMaterial::ApplyTexAniMapping). World-mesh water keeps its own TexCoord2 path.
namespace TexAniScroll {
    // D3D12: a scrolling material's slot in the per-frame offset table rides in the bindless diffuse
    // index's top byte; 0 = no scroll. Every shader reading a diffuse index masks with kDiffuseSlotMask.
    constexpr uint32_t kSlotShift = 24;
    constexpr uint32_t kDiffuseSlotMask = 0x00FFFFFFu;
    constexpr uint32_t kMaxSlots = 256;

    /** This frame's UV offset; false (offset untouched) when the material doesn't scroll. */
    bool GetOffset( zCMaterial* mat, float2& offset );

    /** `diffuseSlot` with the material's table slot packed in; unchanged when it doesn't scroll. Thread-safe. */
    uint32_t PackDiffuseIndex( uint32_t diffuseSlot, zCMaterial* mat );

    /** Writes this frame's offset of every table slot to table[slot].xy; false when no slot is in use. */
    bool FillTable( float4 ( &table )[kMaxSlots] );

    void OnMaterialDeleted( zCMaterial* mat );
}
