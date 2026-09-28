#pragma once
#include "pch.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "WorldObjects.h"
#include "zCTexture.h"

/** Sea water gets the physical ocean body in PS_Water and D3D12 Water.hlsl; lakes, rivers and waterfalls don't. */
inline bool IsOceanWaterTexture( zCTexture* texture ) {
    if ( !texture ) return false;
    constexpr std::string_view kPrefix = "NW_WATER_LAKE";
    const std::string_view name = texture->GetNameWithoutExtView();
    if ( name.size() < kPrefix.size() ) return false;
    return _strnicmp( name.data(), kPrefix.data(), kPrefix.size() ) == 0;
}

/** Per-world ocean look. Tints are luma-neutral; climate 1 is Jharkendar's clear turquoise sea. */
struct OceanProfile {
    float Climate = 0.0f;
    float TintStrength = 0.18f;
    XMFLOAT3 Tint = { 0.894520f, 1.026067f, 1.052377f };
};

inline OceanProfile GetOceanProfile() {
    OceanProfile profile;
    const WorldInfo* world = Engine::GAPI->GetLoadedWorldInfo();
    if ( world && world->WorldName == "ADDONWORLD" ) {
        profile.Climate = 1.0f;
        profile.TintStrength = 0.48f;
        profile.Tint = XMFLOAT3( 0.426781f, 1.138082f, 1.321880f );
    }
    return profile;
}
