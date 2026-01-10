#pragma once

/**
 * ImGui C-API Wrapper für Daedalus-Scripting
 * 
 * Diese Funktionen werden von ddraw.dll exportiert und können von Daedalus-Scripts
 * über LoadLibrary/GetProcAddress aufgerufen werden.
 * 
 * Alle Funktionen verwenden die C-Calling-Convention (__cdecl) und nehmen nur
 * einfache Typen (int, float, const char*) als Parameter an.
 */

class zCParser;
void DefineExternals( zCParser* parser );

#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllexport) int __cdecl imgui_is_ready();
__declspec(dllexport) void __cdecl imgui_set_next_window_pos( int virtualX, int virtualY, int cond, float pivotX, float pivotY );
__declspec(dllexport) void __cdecl imgui_set_next_window_size( int virtualX, int virtualY, int cond );
__declspec(dllexport) void __cdecl imgui_set_next_window_bg_alpha( float value );
__declspec(dllexport) void __cdecl imgui_set_next_window_collapsed( int boolValue, int cond );
__declspec(dllexport) int __cdecl imgui_begin(const char* title, int* openPtr, int windowflags );
__declspec(dllexport) int __cdecl imgui_begin_overlay(const char* title, int* openPtr, int windowflags );
__declspec(dllexport) void __cdecl imgui_end();
__declspec(dllexport) void __cdecl imgui_text(const char* text);
__declspec(dllexport) void __cdecl imgui_text_unformatted(const char* text);
__declspec(dllexport) int __cdecl imgui_button(const char* label, int width, int height);
__declspec(dllexport) int __cdecl imgui_checkbox(const char* label, int* value);
__declspec(dllexport) int __cdecl imgui_slider_float(const char* label, float* value, float min_value, float max_value);
__declspec(dllexport) int __cdecl imgui_input_text(const char* label, char* buffer, int buffer_size);
__declspec(dllexport) void __cdecl imgui_same_line(float offset_x, float spacing);
__declspec(dllexport) void __cdecl imgui_new_line();
__declspec(dllexport) void __cdecl imgui_separator();
__declspec(dllexport) int __cdecl imgui_begin_child(const char* title, int width, int height, int border);
__declspec(dllexport) void __cdecl imgui_end_child();
__declspec(dllexport) int __cdecl imgui_collapsing_header(const char* label);
__declspec(dllexport) int __cdecl imgui_begin_main_menu_bar();
__declspec(dllexport) void __cdecl imgui_end_main_menu_bar();
__declspec(dllexport) int __cdecl imgui_begin_menu(const char* label, int enabled);
__declspec(dllexport) void __cdecl imgui_end_menu();
__declspec(dllexport) int __cdecl imgui_menu_item(const char* label, const char* shortcut, int selected, int enabled);
__declspec(dllexport) void __cdecl imgui_push_id(int id);
__declspec(dllexport) void __cdecl imgui_pop_id();
__declspec(dllexport) int __cdecl imgui_get_content_region_avail_x();
__declspec(dllexport) void __cdecl imgui_set_item_tooltip( const char* text );

__declspec(dllexport) void __cdecl imgui_begin_table( int count, const char* id, int tableFlags, int outerX, int outerY, int innerWidth);
__declspec(dllexport) void __cdecl imgui_end_table();
__declspec(dllexport) int __cdecl imgui_table_next_column();
__declspec(dllexport) void __cdecl imgui_table_next_row(int rowFlags, int minRowHeight);
__declspec(dllexport) void __cdecl imgui_table_set_column_index(int index);
__declspec(dllexport) void __cdecl imgui_table_setup_column(const char* label, int flags, int init_width_or_weight, int user_id);

#ifdef __cplusplus
}
#endif
