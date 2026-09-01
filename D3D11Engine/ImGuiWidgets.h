#pragma once
#include "pch.h"
#include <functional>
#include <string>
#include <vector>
#include <imgui.h>

#include "ImGuiPreviewImages.h"

/** ImGui helpers shared by the settings windows (ImGuiShim.cpp, ImGuiSettingsWindow.cpp). */

/** One entry of a combo box: the label shown, the value it writes, an optional tooltip, and an
    optional preview-image name. A null preview name falls back to ImPreview::NameOf for enum
    values, so an enum-valued setting needs no name here at all - see ImGuiPreviewImages.h. */
template<typename T>
struct ListItem {
    const char* label;
    T value;
    const char* toolTip;
    const char* preview;

    constexpr ListItem( const char* label, const T value ) noexcept :
        label( label ), value( value ), toolTip( nullptr ), preview( nullptr ) {}

    constexpr ListItem( const char* label, const T value, const char* toolTip ) noexcept :
        label( label ), value( value ), toolTip( toolTip ), preview( nullptr ) {}

    constexpr ListItem( const char* label, const T value, const char* toolTip, const char* preview ) noexcept :
        label( label ), value( value ), toolTip( toolTip ), preview( preview ) {}
};

namespace Internal {
    template <typename T>
    inline bool ImComboBox( const char* id, const ListItem<T>* items, size_t numItems, T* storage,
        const std::move_only_function<void() const>& selected = {} ) {
        if ( storage == nullptr || numItems == 0 ) {
            return ImGui::BeginCombo( id, "invalid storage" );
        }
        ListItem<T> selectedItem = items[0];
        for ( size_t i = 0; i < numItems; i++ ) {
            const auto& it = items[i];
            if ( it.value == *storage ) {
                selectedItem = it;
                break;
            }
        }
        if ( ImGui::BeginCombo( id, selectedItem.label ) ) {
            for ( size_t i = 0; i < numItems; i++ ) {
                bool isSelected = (*storage == items[i].value);

                if ( ImGui::Selectable( items[i].label, isSelected ) ) {
                    *storage = items[i].value;
                    if ( selected ) selected();
                }

                if ( items[i].toolTip ) {
                    ImGui::SetItemTooltip( "%s", items[i].toolTip );
                }

                if ( isSelected ) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            return true;
        }
        return false;
    }
}

template <typename T, size_t N>
inline bool ImComboBox( const char* id, const ListItem<T>( &items )[N], T* storage,
    const std::move_only_function<void() const>& selected = {} ) {
    return Internal::ImComboBox( id, items, N, storage, selected );
}

template <typename T>
inline bool ImComboBox( const char* id, const std::vector<ListItem<T>>& items, T* storage,
    const std::move_only_function<void() const>& selected = {} ) {
    return Internal::ImComboBox( id, items.data(), items.size(), storage, selected );
}

/** Left-aligned, button-styled text used as the label column of the settings windows. */
inline void ImText( const char* label, const ImVec2& size ) {
    auto& col = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Button );

    ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_ButtonActive, col );
    ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_ButtonHovered, col );
    ImGui::PushStyleVarX( ImGuiStyleVar_::ImGuiStyleVar_ButtonTextAlign, 0 );

    ImGui::Button( label, size );
    ImGui::PopStyleVar( 1 );

    ImGui::PopStyleColor( 2 );
}

/** Preview-image name of a list entry: its explicit one, else the enum member's name. Empty for a
    non-enum entry that didn't bring its own. */
template <typename T>
inline std::string PreviewNameOf( const ListItem<T>& item ) {
    if ( item.preview ) {
        return item.preview;
    }
    if constexpr ( std::is_enum_v<T> ) {
        return ImPreview::NameOf( item.value );
    } else {
        return {};
    }
}

/** Preview-image name for the entry currently selected in `items`. */
template <typename T>
inline std::string PreviewNameOf( const ListItem<T>* items, size_t numItems, const T& value ) {
    for ( size_t i = 0; i < numItems; i++ ) {
        if ( items[i].value == value ) {
            return PreviewNameOf( items[i] );
        }
    }
    return {};
}
