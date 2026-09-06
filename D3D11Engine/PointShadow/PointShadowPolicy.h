#pragma once
// The PLS_* update policy - how OFTEN a light's shadow is re-rendered and what is cached between renders.
// A policy, not a technique: every technique honours all four modes, and neither of them re-implements
// these predicates.

#include "../pch.h"

struct VobLightInfo;

namespace PointShadowPolicy {

    /** The global point-light shadow mode this light renders and samples with. A static light drops to
        PLS_STATIC_ONLY when its category is not opted into VOB/NPC casters - it has nothing to overlay. */
    int CurrentShadowMode( const VobLightInfo* info );

    /** What NeedsUpdate has to know about a light, so the predicate itself stays technique-free. */
    struct LightUpdateState {
        bool Moved = false;
        bool StaticShadowReady = false;
        bool DrawnOnce = false;
    };

    /** Does this light's shadow have to be re-rendered? `lastShadowMode` is the mode it last rendered
        with; a pending transition is REPORTED here but must be latched elsewhere (the handler that acts
        on it runs later, and consuming it here would make that handler a no-op). */
    bool NeedsUpdate( int shadowMode, int lastShadowMode, const LightUpdateState& state );

    /** Could use an update, but it's not very important. */
    bool WantsUpdate( int shadowMode, const VobLightInfo* info, DWORD lastUpdateColor );

    bool PositionEqualEps( const XMFLOAT3& a, const XMFLOAT3& b, float eps = 1.0f );

} // namespace PointShadowPolicy
