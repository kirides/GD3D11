#pragma once
#include "pch.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "WorldObjects.h"
#include "zCMaterial.h"
#include "zCTexture.h"

/** Case-insensitive match against one identifier: X* prefix, *X suffix, *X* contains, X exact, * anything. */
inline bool MatchesWaterIdentifier( std::string_view name, std::string_view pattern ) {
    const bool leading = !pattern.empty() && pattern.front() == '*';
    if ( leading ) pattern.remove_prefix( 1 );
    const bool trailing = !pattern.empty() && pattern.back() == '*';
    if ( trailing ) pattern.remove_suffix( 1 );
    if ( pattern.size() > name.size() ) return false;

    auto equalsAt = [&]( size_t offset ) { return _strnicmp( name.data() + offset, pattern.data(), pattern.size() ) == 0; };
    if ( leading && trailing ) {
        for ( size_t i = 0; i + pattern.size() <= name.size(); ++i ) {
            if ( equalsAt( i ) ) return true;
        }
        return false;
    }
    if ( trailing ) return equalsAt( 0 );
    if ( leading ) return equalsAt( name.size() - pattern.size() );
    return pattern.size() == name.size() && equalsAt( 0 );
}

/** True when `name` matches one of the '|'-separated identifiers (spaces around them are ignored). */
inline bool MatchesIdentifierList( std::string_view name, std::string_view identifiers ) {
    for ( size_t start = 0; start <= identifiers.size(); ) {
        size_t end = identifiers.find( '|', start );
        if ( end == std::string_view::npos ) end = identifiers.size();
        std::string_view pattern = identifiers.substr( start, end - start );
        while ( !pattern.empty() && pattern.front() == ' ' ) pattern.remove_prefix( 1 );
        while ( !pattern.empty() && pattern.back() == ' ' ) pattern.remove_suffix( 1 );
        if ( !pattern.empty() && MatchesWaterIdentifier( name, pattern ) ) return true;
        start = end + 1;
    }
    return false;
}

/** Sea water gets the physical ocean body in PS_Water and D3D12 Water.hlsl; lakes, rivers and waterfalls don't.
    Sea = the material or its texture name matches one of the '|'-separated [Display] OceanIdentifiers. */
inline bool IsOceanWaterMaterial( zCMaterial* mat ) {
    if ( !mat ) return false;
    const std::string_view identifiers = Engine::GAPI->GetRendererState().RendererSettings.OceanIdentifiers;
    zCTexture* texture = mat->GetTextureSingle();
    return MatchesIdentifierList( mat->GetNameView(), identifiers )
        || ( texture && MatchesIdentifierList( texture->GetNameWithoutExtView(), identifiers ) );
}

/** Ocean look from the OceanColor setting. Tints are luma-neutral; climate 1 is Jharkendar's clear turquoise sea. */
struct OceanProfile {
    float Climate = 0.0f;
    float TintStrength = 0.18f;
    XMFLOAT3 Tint = { 0.894520f, 1.026067f, 1.052377f };
    float TextureStrength = 1.0f;   // material texture over the water body; 1 = legacy water's amount
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
        profile.TextureStrength = 0.0f;
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
        profile.TextureStrength = std::clamp( settings.OceanCustomTexture, 0.0f, 1.0f );
    }
    return profile;
}

/** WaterFrame::shoreFieldState: 0 = no field, 1 = field, 2 = field painted for debugging. */
inline float WaterShoreFieldState( bool haveField ) {
    if ( !haveField ) return 0.0f;
    return Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.WaterDebug.ShowShoreField ? 2.0f : 1.0f;
}

/** WaterFrame::shoreFoam: 0 = off, 1 = ocean only, 2 = all water. */
inline float WaterShoreFoamMode() {
    return static_cast<float>( Engine::GAPI->GetRendererState().RendererSettings.WaterShoreFoam );
}

/** 1 when the water PS should march the reflected sky in screen space. */
inline float WaterSkyReflectionEnabled() {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    return settings.WaterReflectionMode == GothicRendererSettings::WATER_REFLECTION_GEOMETRY_SKY ? 1.0f : 0.0f;
}
