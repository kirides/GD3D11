#pragma once
#include "pch.h"
#include <string>
#include <string_view>
#include <type_traits>
#include <imgui.h>

/** Optional "this is what the setting does" images for the settings window.
    Adding one is meant to be a file drop, not a code change: the lookup name of an enum-valued
    setting is derived from its enum member (see NameOf), so putting
    System\GD3D11\Previews\<EnumType>_<Member>.jpg next to the mod makes the preview appear.
    A name with no matching file simply draws nothing. */
namespace ImPreview {
    /** Edge length previews are drawn at, and the width the settings window reserves for them. */
    inline constexpr float Size = 256.0f;

    /** Lookup name of an enum value: "<EnumType>_<Member>", e.g. "E_WaterSSRQuality_WATER_SSR_HIGH". */
    template <typename E> requires std::is_enum_v<E>
    std::string NameOf( E value ) {
        std::string name{ magic_enum::enum_type_name<E>() };
        name += '_';
        name += magic_enum::enum_name( value );
        return name;
    }

    /** Draws the preview `name`, swapping to `hoverName` while the mouse is over it so the two can be
        compared in place. Returns false and draws nothing when no image is shipped for `name`.
        Names carry no extension - .jpg and .png are tried in that order. */
    bool Show( std::string_view name, std::string_view hoverName = {} );

    /** Preview of an enum value, comparing against `compareTo` (usually the "off" member) on hover. */
    template <typename E> requires std::is_enum_v<E>
    bool Show( E value, E compareTo ) {
        return Show( NameOf( value ), NameOf( compareTo ) );
    }

    /** Preview of an enum value with nothing to compare against. */
    template <typename E> requires std::is_enum_v<E>
    bool Show( E value ) {
        return Show( NameOf( value ) );
    }

    /** Preview of a checkbox: "<name>_On" / "<name>_Off", hovering shows the other state. */
    bool ShowToggle( std::string_view name, bool value );

    /** Whether an image is shipped for `name`. Loads it (once) like Show does, so a layout can ask
        before it has reserved the room to draw one. */
    bool Exists( std::string_view name );

    /** Drops every cached texture. Must run while the graphics engine is still alive. */
    void Reset();
}
