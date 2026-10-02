#pragma once
#include "ConstantBufferStructs.h"
#include "WorldObjects.h"
#include <map>
#include <string_view>

struct WorldMeshSectionInfo;

/** Gives `vi` a WaterBob when its visual or vob name matches `identifiers` (WaterProfile.h's list syntax) and it
    rests on the surface of water with ground waves. */
bool AttachWaterBob( VobInfo* vi, const std::map<int, std::map<int, WorldMeshSectionInfo>>& sections, std::string_view identifiers );

/** Moves the bob to where the water's vertex waves put the surface at `totalTimeMs` (GothicAPI::GetTotalTime);
    waveMode is E_WaterWaves. */
void UpdateWaterBob( VobWaterBob& bob, float totalTimeMs, int waveMode );

/** Adds a floating vob's displacement to its freshly packed instance. */
inline void ApplyWaterBob( VobInstanceInfo& vii, const VobInfo& vi ) {
    if ( !vi.WaterBob ) return;
    const VobWaterBob& bob = *vi.WaterBob;
    vii.world._14 += bob.Offset.x;
    vii.world._24 += bob.Offset.y;
    vii.world._34 += bob.Offset.z;
    vii.prevWorld._14 += bob.PrevOffset.x;
    vii.prevWorld._24 += bob.PrevOffset.y;
    vii.prevWorld._34 += bob.PrevOffset.z;
}
