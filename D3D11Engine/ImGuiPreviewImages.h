#pragma once
#include "pch.h"
#include <string>
#include <string_view>
#include <type_traits>
#include <imgui.h>

/** Optional "this is what the setting does" images for the settings windows.

    Nothing is reserved in the settings layout: a row that is being pointed at hints its image, and a
    single panel pinned beside the window shows it. Adding one is a file drop, not a code change -
    the lookup name of an enum-valued setting is derived from its enum member (see NameOf), so
    putting System\GD3D11\Previews\<EnumType>_<Member>.jpg next to the mod makes it appear, and a
    name with no matching file simply keeps the panel hidden. */
namespace ImPreview {
    /** Edge length previews are drawn at. */
    inline constexpr float Size = 256.0f;

    /** Lookup name of an enum value: "<EnumType>_<Member>", e.g. "E_WaterSSRQuality_WATER_SSR_HIGH". */
    template <typename E> requires std::is_enum_v<E>
    std::string NameOf( E value ) {
        std::string name{ magic_enum::enum_type_name<E>() };
        name += '_';
        name += magic_enum::enum_name( value );
        return name;
    }

    /** Lookup name of a checkbox state: "<name>_On" / "<name>_Off". */
    std::string NameOfToggle( std::string_view name, bool value );

    /** Records what the mouse is pointing at. The last hint of a frame is what the panel shows, so
        callers can hint freely - only one image is ever loaded or drawn. Names carry no extension;
        .jpg and .png are tried in that order. */
    void Hint( std::string_view name, std::string_view caption = {} );

    /** Draws the pinned panel for whatever was hinted, beside the window spanning anchorMin/anchorMax
        (it flips to the other side when there is no room). Call once per frame, after that window's
        End(). Draws nothing while nothing is hinted or the hinted image isn't shipped. */
    void DrawPinned( const ImVec2& anchorMin, const ImVec2& anchorMax );

    /** Drops every cached texture. Must run while the graphics engine is still alive. */
    void Reset();
}
