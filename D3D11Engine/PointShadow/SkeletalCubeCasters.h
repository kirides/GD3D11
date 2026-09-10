#pragma once
// Skeletal casters (NPC bodies, their attachments, skeletal MOBs) for the point-light cubes. Each vob's draw
// data is recorded once per point-light pass; every light and every face replays it, changing only the cube
// CB and the target the caller bound.

#include "../pch.h"
#include <functional>

struct SkeletalVobInfo;
class zCVob;

namespace SkeletalCubeCasters {

    /** Drops the recorded draws. They point at attachment meshes a later pass may release, so they never
        outlive one DrawPointlightShadows. */
    void BeginPass();

    /** Draws already range-culled casters into the bound cube: all 6 faces through SV_RenderTargetArrayIndex
        when layered, otherwise the one face the cube CB selects. */
    void Draw( std::span<SkeletalVobInfo* const> vobs, bool layered,
        const std::move_only_function<bool( const zCVob* ) const>& ignoreVob );

} // namespace SkeletalCubeCasters
