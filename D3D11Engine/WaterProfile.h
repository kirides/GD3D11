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

/** Ocean look from the OceanColor setting. Tints are luma-neutral; climate 1 is Jharkendar's clear turquoise sea. */
struct OceanProfile {
    float Climate = 0.0f;
    float TintStrength = 0.18f;
    XMFLOAT3 Tint = { 0.894520f, 1.026067f, 1.052377f };
};

inline OceanProfile GetOceanProfile() {
    using S = GothicRendererSettings;
    const S& settings = Engine::GAPI->GetRendererState().RendererSettings;
    OceanProfile profile;

    bool tropical = settings.OceanColor == S::OCEAN_COLOR_TROPICAL;
    if ( settings.OceanColor == S::OCEAN_COLOR_PER_WORLD ) {
        const WorldInfo* world = Engine::GAPI->GetLoadedWorldInfo();
        tropical = world && world->WorldName == "ADDONWORLD";
    }

    if ( tropical ) {
        profile.Climate = 1.0f;
        profile.TintStrength = 0.48f;
        profile.Tint = XMFLOAT3( 0.426781f, 1.138082f, 1.321880f );
    } else if ( settings.OceanColor == S::OCEAN_COLOR_CUSTOM ) {
        // Normalize the picked color to luma 1 so it shifts hue only; cap saturated picks.
        const float3& c = settings.OceanCustomColor;
        const float luma = c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f;
        if ( luma > 0.001f ) {
            profile.Tint = XMFLOAT3( std::min( c.x / luma, 2.0f ), std::min( c.y / luma, 2.0f ), std::min( c.z / luma, 2.0f ) );
        } else {
            profile.Tint = XMFLOAT3( 1.0f, 1.0f, 1.0f );
        }
        profile.TintStrength = std::clamp( settings.OceanCustomColorStrength, 0.0f, 1.0f );
        profile.Climate = std::clamp( settings.OceanCustomClarity, 0.0f, 1.0f );
    }
    return profile;
}

/** 1 when the water PS should march the reflected sky in screen space. */
inline float WaterSkyReflectionEnabled() {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    return settings.WaterReflectionMode == GothicRendererSettings::WATER_REFLECTION_GEOMETRY_SKY ? 1.0f : 0.0f;
}
