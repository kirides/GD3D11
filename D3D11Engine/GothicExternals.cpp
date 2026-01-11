#include "GothicExternals.h"
#include "Engine.h"
#include "ImGuiShim.h"
#include <imgui.h>
#include "zCParser.h"
#include "zSTRING.h"

/**
 * ImGui C-API Wrapper Implementation für Daedalus-Scripting
 * 
 * Diese Funktionen werden von ddraw.dll exportiert und können von Daedalus-Scripts
 * über LoadLibrary/GetProcAddress aufgerufen werden.
 */

// ============================================================================
// Hilfsfunktion: Prüft ob ImGui bereit ist
// ============================================================================

static bool IsImGuiReady() {
    return Engine::ImGuiHandle != nullptr && Engine::ImGuiHandle->Initiated;
}

static void _VirtualToScreen(int vx, int vy, float& x, float& y) {
    const auto res = Engine::GraphicsEngine->GetResolution();

    constexpr float VIRTUAL_WIDTH = 4096;
    constexpr float VIRTUAL_HEIGHT = 4096;

    x = (static_cast<float>(vx) / VIRTUAL_WIDTH) * static_cast<float>(res.x);
    y = (static_cast<float>(vy) / VIRTUAL_HEIGHT) * static_cast<float>(res.y);
}
static float _VirtualToScreenX(int vx) {
    const auto res = Engine::GraphicsEngine->GetResolution();
    constexpr float VIRTUAL_WIDTH = 4096;
    return (static_cast<float>(vx) / VIRTUAL_WIDTH) * static_cast<float>(res.x);
}

static float _VirtualToScreenY(int vy) {
    const auto res = Engine::GraphicsEngine->GetResolution();
    constexpr float VIRTUAL_HEIGHT = 4096;
    return (static_cast<float>(vy) / VIRTUAL_HEIGHT) * static_cast<float>(res.y);
}

static int _ScreenToVirtualX(float x) {
    const auto res = Engine::GraphicsEngine->GetResolution();
    constexpr float VIRTUAL_WIDTH = 4096;
    return static_cast<int>((x / static_cast<float>(res.x)) * VIRTUAL_WIDTH);
}
// ============================================================================
// Window Management
// ============================================================================
extern "C" __declspec(dllexport) void __cdecl imgui_set_next_window_pos( int virtualX, int virtualY, int cond, float pivotX, float pivotY ) {
    if (!IsImGuiReady()) {
        return;
    }
    float xF = 0;
    float yF = 0;
    _VirtualToScreen(virtualX, virtualY, xF, yF);
    ImGui::SetNextWindowPos( ImVec2( xF, yF ), cond, ImVec2( pivotX, pivotY ) );
}

extern "C" __declspec(dllexport) void __cdecl imgui_set_next_window_size( int virtualX, int virtualY, int cond) {
    if (!IsImGuiReady()) {
        return;
    }
    float xF = 0;
    float yF = 0;
    _VirtualToScreen(virtualX, virtualY, xF, yF);
    
    ImGui::SetNextWindowSize( ImVec2( xF, yF ), cond );
}

extern "C" __declspec(dllexport) void __cdecl imgui_set_next_window_bg_alpha( float value) {
    if (!IsImGuiReady()) {
        return;
    }
    ImGui::SetNextWindowBgAlpha(value );
}

extern "C" __declspec(dllexport) void __cdecl imgui_set_item_tooltip( const char* text ) {
    if (!IsImGuiReady()) {
        return;
    }
    ImGui::SetItemTooltip("%s", text);
}

extern "C" __declspec(dllexport) void __cdecl imgui_set_next_window_collapsed( int boolValue, int cond) {
    if (!IsImGuiReady()) {
        return;
    }
    ImGui::SetNextWindowCollapsed(boolValue, cond);
}

extern "C" __declspec(dllexport) int __cdecl imgui_begin(const char* title, int* openPtr, int windowflags ) {
    if (!IsImGuiReady() || title == nullptr) {
        return 0;
    }
    auto ret = ImGui::Begin(title, reinterpret_cast<bool*>(openPtr), windowflags) ? 1 : 0;
    if ( ret ) {
        Engine::ImGuiHandle->LibShowBlockingThisFrame = true;
    }
    return ret;
}

extern "C" __declspec(dllexport) int __cdecl imgui_begin_overlay( const char* title, int* openPtr, int windowflags ) {
    if ( !IsImGuiReady() || title == nullptr ) {
        return 0;
    }
    windowflags |= ImGuiWindowFlags_NoFocusOnAppearing;
    auto ret = ImGui::Begin( title, reinterpret_cast<bool*>(openPtr), windowflags ) ? 1 : 0;
    if ( ret ) {
        Engine::ImGuiHandle->LibShowNonBlockingThisFrame = true;
    }
    return ret;
}

extern "C" __declspec(dllexport) void __cdecl imgui_end() {
    if (!IsImGuiReady()) {
        return;
    }
    ImGui::End();
}

// ============================================================================
// Text Display
// ============================================================================

extern "C" __declspec(dllexport) void __cdecl imgui_text(const char* text) {
    if (!IsImGuiReady() || text == nullptr) {
        return;
    }
    ImGui::Text("%s", text);
}

extern "C" __declspec(dllexport) void __cdecl imgui_text_unformatted(const char* text) {
    if (!IsImGuiReady() || text == nullptr) {
        return;
    }
    ImGui::TextUnformatted(text);
}

// ============================================================================
// Buttons and Interactive Elements
// ============================================================================

extern "C" __declspec(dllexport) int __cdecl imgui_button(const char* label, int width, int height) {
    if (!IsImGuiReady() || label == nullptr) {
        return 0;
    }
    
    float w = 0;
    float h = 0;
    _VirtualToScreen(width, height, w, h);
    return ImGui::Button(label, ImVec2(w, h)) ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl imgui_checkbox(const char* label, int* value) {
    if (!IsImGuiReady() || label == nullptr || value == nullptr) {
        return 0;
    }
    // ImGui erwartet bool*, wir haben int* - konvertieren
    bool boolValue = (*value != 0);
    bool changed = ImGui::Checkbox(label, &boolValue);
    *value = boolValue ? 1 : 0;
    return changed ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl imgui_slider_float(const char* label, float* value, float min_value, float max_value) {
    if (!IsImGuiReady() || label == nullptr || value == nullptr) {
        return 0;
    }
    return ImGui::SliderFloat(label, value, min_value, max_value) ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl imgui_input_text(const char* label, char* buffer, int buffer_size) {
    if (!IsImGuiReady() || label == nullptr || buffer == nullptr || buffer_size <= 0) {
        return 0;
    }
    return ImGui::InputText(label, buffer, static_cast<size_t>(buffer_size)) ? 1 : 0;
}

// ============================================================================
// Layout
// ============================================================================

extern "C" __declspec(dllexport) void __cdecl imgui_same_line(float offset_x, float spacing) {
    if (!IsImGuiReady()) {
        return;
    }
    // ImGui::SameLine verwendet -1.0f als Standard für "keine Änderung"
    // Wir konvertieren 0 zu -1.0f für Daedalus-Kompatibilität
    float offset = (offset_x == 0.0f) ? 0.0f : offset_x;
    float space = (spacing == 0.0f) ? -1.0f : spacing;
    ImGui::SameLine(offset, space);
}

extern "C" __declspec(dllexport) void __cdecl imgui_new_line() {
    if (!IsImGuiReady()) {
        return;
    }
    ImGui::NewLine();
}

extern "C" __declspec(dllexport) void __cdecl imgui_separator() {
    if (!IsImGuiReady()) {
        return;
    }
    ImGui::Separator();
}

extern "C" __declspec(dllexport) int __cdecl imgui_begin_child(const char* title, int width, int height, int border) {
    if (!IsImGuiReady() || title == nullptr) {
        return 0;
    }
    ImGuiChildFlags childFlags = (border != 0) ? ImGuiChildFlags_Borders : ImGuiChildFlags_None;
    
    float w = 0;
    float h = 0;
    _VirtualToScreen(width, height, w, h);
    return ImGui::BeginChild(title, ImVec2(w, h), childFlags) ? 1 : 0;
}

extern "C" __declspec(dllexport) void __cdecl imgui_end_child() {
    if (!IsImGuiReady()) {
        return;
    }
    ImGui::EndChild();
}

// ============================================================================
// Collapsing Header
// ============================================================================

extern "C" __declspec(dllexport) int __cdecl imgui_collapsing_header(const char* label) {
    if (!IsImGuiReady() || label == nullptr) {
        return 0;
    }
    return ImGui::CollapsingHeader(label) ? 1 : 0;
}

// ============================================================================
// Menu Bar
// ============================================================================

extern "C" __declspec(dllexport) int __cdecl imgui_begin_main_menu_bar() {
    if (!IsImGuiReady()) {
        return 0;
    }
    return ImGui::BeginMainMenuBar() ? 1 : 0;
}

extern "C" __declspec(dllexport) void __cdecl imgui_end_main_menu_bar() {
    if (!IsImGuiReady()) {
        return;
    }
    ImGui::EndMainMenuBar();
}

extern "C" __declspec(dllexport) int __cdecl imgui_begin_menu(const char* label, int enabled) {
    if (!IsImGuiReady() || label == nullptr) {
        return 0;
    }
    return ImGui::BeginMenu(label, enabled != 0) ? 1 : 0;
}

extern "C" __declspec(dllexport) void __cdecl imgui_end_menu() {
    if (!IsImGuiReady()) {
        return;
    }
    ImGui::EndMenu();
}

extern "C" __declspec(dllexport) int __cdecl imgui_menu_item(const char* label, const char* shortcut, int selected, int enabled) {
    if (!IsImGuiReady() || label == nullptr) {
        return 0;
    }
    // shortcut kann nullptr sein, das ist OK für ImGui::MenuItem
    return ImGui::MenuItem(label, shortcut, selected != 0, enabled != 0) ? 1 : 0;
}

// ============================================================================
// ID Stack
// ============================================================================

extern "C" __declspec(dllexport) void __cdecl imgui_push_id(int id) {
    if (!IsImGuiReady()) {
        return;
    }
    ImGui::PushID(id);
}

extern "C" __declspec(dllexport) void __cdecl imgui_pop_id() {
    if (!IsImGuiReady()) {
        return;
    }
    ImGui::PopID();
}

extern "C" __declspec(dllexport) int __cdecl imgui_is_ready() {
    return IsImGuiReady() ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl imgui_get_content_region_avail_x() {
    if (!IsImGuiReady()) {
        return 0;
    }

    return _ScreenToVirtualX(ImGui::GetContentRegionAvail().x);
}

extern "C" __declspec(dllexport) void __cdecl imgui_begin_table( int count, const char* id, int tableFlags, int outerX, int outerY, int innerWidth) {
    if (!IsImGuiReady()) {
        return;
    }
    float x;
    float y;
    _VirtualToScreen(outerX, outerY, x, y);
    ImGui::BeginTable(id, count, tableFlags, ImVec2( x, y), _VirtualToScreenX(innerWidth) );
}

extern "C" __declspec(dllexport) void __cdecl imgui_end_table() {
    if (!IsImGuiReady()) {
        return;
    }
    ImGui::EndTable();
}

extern "C" __declspec(dllexport) int __cdecl imgui_table_next_column() {
    if (!IsImGuiReady()) {
        return 0;
    }

    return ImGui::TableNextColumn();
}

extern "C" __declspec(dllexport) void __cdecl imgui_table_next_row(int rowFlags, int minRowHeight) {
    if (!IsImGuiReady()) {
        return;
    }

    ImGui::TableNextRow(rowFlags, _VirtualToScreenY(minRowHeight));
}
extern "C" __declspec(dllexport) void __cdecl imgui_table_set_column_index(int index) {
    if (!IsImGuiReady()) {
        return;
    }

    ImGui::TableSetColumnIndex(index);
}

extern "C" __declspec(dllexport) void __cdecl imgui_table_setup_column(const char* label, int flags, int init_width_or_weight, int user_id) {
    if (!IsImGuiReady()) {
        return;
    }

    ImGui::TableSetupColumn(label, flags, _VirtualToScreenX(init_width_or_weight), user_id);
}

static int GDX_ImGui_Begin() {
    auto parser = zCParser::GetParser();

    int flags;
    int pOpen;
    zSTRING label;
    parser->GetParameter(flags);
    parser->GetParameter(pOpen);
    parser->GetParameter(label);

    int ret = imgui_begin( label.ToChar(), (int*)pOpen, flags);
    parser->SetReturn( ret );
    return 0;
}

// ============================================================================
// External Wrappers for Daedalus
// ============================================================================

static int GDX_ImGui_SetNextWindowPos() {
    auto parser = zCParser::GetParser();
    float pivotY, pivotX;
    int cond, virtualY, virtualX;
    parser->GetParameter(pivotY);
    parser->GetParameter(pivotX);
    parser->GetParameter(cond);
    parser->GetParameter(virtualY);
    parser->GetParameter(virtualX);
    imgui_set_next_window_pos(virtualX, virtualY, cond, pivotX, pivotY);
    return 0;
}

static int GDX_ImGui_SetNextWindowSize() {
    auto parser = zCParser::GetParser();
    int cond, virtualY, virtualX;
    parser->GetParameter(cond);
    parser->GetParameter(virtualY);
    parser->GetParameter(virtualX);
    imgui_set_next_window_size(virtualX, virtualY, cond);
    return 0;
}

static int GDX_ImGui_SetNextWindowBgAlpha() {
    auto parser = zCParser::GetParser();
    float value;
    parser->GetParameter(value);
    imgui_set_next_window_bg_alpha(value);
    return 0;
}

static int GDX_ImGui_SetItemTooltip() {
    auto parser = zCParser::GetParser();
    zSTRING text;
    parser->GetParameter(text);
    imgui_set_item_tooltip(text.ToChar());
    return 0;
}

static int GDX_ImGui_SetNextWindowCollapsed() {
    auto parser = zCParser::GetParser();
    int cond, boolValue;
    parser->GetParameter(cond);
    parser->GetParameter(boolValue);
    imgui_set_next_window_collapsed(boolValue, cond);
    return 0;
}

static int GDX_ImGui_BeginOverlay() {
    auto parser = zCParser::GetParser();
    int windowflags, pOpen;
    zSTRING title;
    parser->GetParameter(windowflags);
    parser->GetParameter(pOpen);
    parser->GetParameter(title);
    int ret = imgui_begin_overlay(title.ToChar(), &pOpen, windowflags);
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_End() {
    imgui_end();
    return 0;
}

static int GDX_ImGui_Text() {
    auto parser = zCParser::GetParser();
    zSTRING text;
    parser->GetParameter(text);
    imgui_text(text.ToChar());
    return 0;
}

static int GDX_ImGui_TextUnformatted() {
    auto parser = zCParser::GetParser();
    zSTRING text;
    parser->GetParameter(text);
    imgui_text_unformatted(text.ToChar());
    return 0;
}

static int GDX_ImGui_Button() {
    auto parser = zCParser::GetParser();
    int height, width;
    zSTRING label;
    parser->GetParameter(height);
    parser->GetParameter(width);
    parser->GetParameter(label);
    int ret = imgui_button(label.ToChar(), width, height);
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_Checkbox() {
    auto parser = zCParser::GetParser();
    int value;
    zSTRING label;
    parser->GetParameter(value);
    parser->GetParameter(label);

    int ret = imgui_checkbox(label.ToChar(), (int*)value);
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_SliderFloat() {
    auto parser = zCParser::GetParser();
    float maxValue, minValue;
    int value;
    zSTRING label;
    parser->GetParameter(maxValue);
    parser->GetParameter(minValue);
    parser->GetParameter(value);
    parser->GetParameter(label);
    int ret = imgui_slider_float(label.ToChar(), (float*)value, minValue, maxValue);
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_InputText() {
    auto parser = zCParser::GetParser();
    int bufferSize;
    int buffer;
    zSTRING label;
    parser->GetParameter(bufferSize);
    parser->GetParameter(buffer);
    parser->GetParameter(label);

    int ret = imgui_input_text(label.ToChar(), (char*)buffer, bufferSize);
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_SameLine() {
    auto parser = zCParser::GetParser();
    float spacing, offsetX;
    parser->GetParameter(spacing);
    parser->GetParameter(offsetX);
    imgui_same_line(offsetX, spacing);
    return 0;
}

static int GDX_ImGui_NewLine() {
    imgui_new_line();
    return 0;
}

static int GDX_ImGui_Separator() {
    imgui_separator();
    return 0;
}

static int GDX_ImGui_BeginChild() {
    auto parser = zCParser::GetParser();
    int border, height, width;
    zSTRING title;
    parser->GetParameter(border);
    parser->GetParameter(height);
    parser->GetParameter(width);
    parser->GetParameter(title);
    int ret = imgui_begin_child(title.ToChar(), width, height, border);
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_EndChild() {
    imgui_end_child();
    return 0;
}

static int GDX_ImGui_CollapsingHeader() {
    auto parser = zCParser::GetParser();
    zSTRING label;
    parser->GetParameter(label);
    int ret = imgui_collapsing_header(label.ToChar());
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_BeginMainMenuBar() {
    auto parser = zCParser::GetParser();
    int ret = imgui_begin_main_menu_bar();
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_EndMainMenuBar() {
    imgui_end_main_menu_bar();
    return 0;
}

static int GDX_ImGui_BeginMenu() {
    auto parser = zCParser::GetParser();
    int enabled;
    zSTRING label;
    parser->GetParameter(enabled);
    parser->GetParameter(label);
    int ret = imgui_begin_menu(label.ToChar(), enabled);
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_EndMenu() {
    imgui_end_menu();
    return 0;
}

static int GDX_ImGui_MenuItem() {
    auto parser = zCParser::GetParser();
    int enabled, selected;
    zSTRING shortcut, label;
    parser->GetParameter(enabled);
    parser->GetParameter(selected);
    parser->GetParameter(shortcut);
    parser->GetParameter(label);
    const char* shortcutPtr = shortcut.Length() > 0 ? shortcut.ToChar() : nullptr;
    int ret = imgui_menu_item(label.ToChar(), shortcutPtr, selected, enabled);
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_PushID() {
    auto parser = zCParser::GetParser();
    int id;
    parser->GetParameter(id);
    imgui_push_id(id);
    return 0;
}

static int GDX_ImGui_PopID() {
    imgui_pop_id();
    return 0;
}

static int GDX_ImGui_IsReady() {
    auto parser = zCParser::GetParser();
    int ret = imgui_is_ready();
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_GetContentRegionAvailX() {
    auto parser = zCParser::GetParser();
    int ret = imgui_get_content_region_avail_x();
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_BeginTable() {
    auto parser = zCParser::GetParser();
    int innerWidth, outerY, outerX, tableFlags;
    zSTRING id;
    int count;
    parser->GetParameter(innerWidth);
    parser->GetParameter(outerY);
    parser->GetParameter(outerX);
    parser->GetParameter(tableFlags);
    parser->GetParameter(id);
    parser->GetParameter(count);
    imgui_begin_table(count, id.ToChar(), tableFlags, outerX, outerY, innerWidth);
    return 0;
}

static int GDX_ImGui_EndTable() {
    imgui_end_table();
    return 0;
}

static int GDX_ImGui_TableNextColumn() {
    auto parser = zCParser::GetParser();
    int ret = imgui_table_next_column();
    parser->SetReturn(ret);
    return 0;
}

static int GDX_ImGui_TableNextRow() {
    auto parser = zCParser::GetParser();
    int minRowHeight, rowFlags;
    parser->GetParameter(minRowHeight);
    parser->GetParameter(rowFlags);
    imgui_table_next_row(rowFlags, minRowHeight);
    return 0;
}

static int GDX_ImGui_TableSetColumnIndex() {
    auto parser = zCParser::GetParser();
    int index;
    parser->GetParameter(index);
    imgui_table_set_column_index(index);
    return 0;
}

static int GDX_ImGui_TableSetupColumn() {
    auto parser = zCParser::GetParser();
    int userId, initWidthOrWeight, flags;
    zSTRING label;
    parser->GetParameter(userId);
    parser->GetParameter(initWidthOrWeight);
    parser->GetParameter(flags);
    parser->GetParameter(label);
    imgui_table_setup_column(label.ToChar(), flags, initWidthOrWeight, userId);
    return 0;
}

void DefineExternals(zCParser* parser) {

    // Window Management
    parser->DefineExternal( zSTRING( "GDX_ImGui_SetNextWindowPos" ), GDX_ImGui_SetNextWindowPos, zPAR_TYPE_VOID, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_FLOAT, zPAR_TYPE_FLOAT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_SetNextWindowSize" ), GDX_ImGui_SetNextWindowSize, zPAR_TYPE_VOID, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_SetNextWindowBgAlpha" ), GDX_ImGui_SetNextWindowBgAlpha, zPAR_TYPE_VOID, zPAR_TYPE_FLOAT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_SetItemTooltip" ), GDX_ImGui_SetItemTooltip, zPAR_TYPE_VOID, zPAR_TYPE_STRING, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_SetNextWindowCollapsed" ), GDX_ImGui_SetNextWindowCollapsed, zPAR_TYPE_VOID, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_Begin" ), GDX_ImGui_Begin, zPAR_TYPE_INT, zPAR_TYPE_STRING, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_BeginOverlay" ), GDX_ImGui_BeginOverlay, zPAR_TYPE_INT, zPAR_TYPE_STRING, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_End" ), GDX_ImGui_End, zPAR_TYPE_VOID, zPAR_TYPE_VOID );

    // Text Display
    parser->DefineExternal( zSTRING( "GDX_ImGui_Text" ), GDX_ImGui_Text, zPAR_TYPE_VOID, zPAR_TYPE_STRING, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_TextUnformatted" ), GDX_ImGui_TextUnformatted, zPAR_TYPE_VOID, zPAR_TYPE_STRING, zPAR_TYPE_VOID );

    // Buttons and Interactive Elements
    parser->DefineExternal( zSTRING( "GDX_ImGui_Button" ), GDX_ImGui_Button, zPAR_TYPE_INT, zPAR_TYPE_STRING, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_Checkbox" ), GDX_ImGui_Checkbox, zPAR_TYPE_INT, zPAR_TYPE_STRING, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_SliderFloat" ), GDX_ImGui_SliderFloat, zPAR_TYPE_INT, zPAR_TYPE_STRING, zPAR_TYPE_FLOAT, zPAR_TYPE_FLOAT, zPAR_TYPE_FLOAT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_InputText" ), GDX_ImGui_InputText, zPAR_TYPE_INT, zPAR_TYPE_STRING, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_VOID );

    // Layout
    parser->DefineExternal( zSTRING( "GDX_ImGui_SameLine" ), GDX_ImGui_SameLine, zPAR_TYPE_VOID, zPAR_TYPE_FLOAT, zPAR_TYPE_FLOAT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_NewLine" ), GDX_ImGui_NewLine, zPAR_TYPE_VOID, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_Separator" ), GDX_ImGui_Separator, zPAR_TYPE_VOID, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_BeginChild" ), GDX_ImGui_BeginChild, zPAR_TYPE_INT, zPAR_TYPE_STRING, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_EndChild" ), GDX_ImGui_EndChild, zPAR_TYPE_VOID, zPAR_TYPE_VOID );

    // Collapsing Header
    parser->DefineExternal( zSTRING( "GDX_ImGui_CollapsingHeader" ), GDX_ImGui_CollapsingHeader, zPAR_TYPE_INT, zPAR_TYPE_STRING, zPAR_TYPE_VOID );

    // Menu Bar
    parser->DefineExternal( zSTRING( "GDX_ImGui_BeginMainMenuBar" ), GDX_ImGui_BeginMainMenuBar, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_EndMainMenuBar" ), GDX_ImGui_EndMainMenuBar, zPAR_TYPE_VOID, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_BeginMenu" ), GDX_ImGui_BeginMenu, zPAR_TYPE_INT, zPAR_TYPE_STRING, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_EndMenu" ), GDX_ImGui_EndMenu, zPAR_TYPE_VOID, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_MenuItem" ), GDX_ImGui_MenuItem, zPAR_TYPE_INT, zPAR_TYPE_STRING, zPAR_TYPE_STRING, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_VOID );

    // ID Stack
    parser->DefineExternal( zSTRING( "GDX_ImGui_PushID" ), GDX_ImGui_PushID, zPAR_TYPE_VOID, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_PopID" ), GDX_ImGui_PopID, zPAR_TYPE_VOID, zPAR_TYPE_VOID );

    // Utility
    parser->DefineExternal( zSTRING( "GDX_ImGui_IsReady" ), GDX_ImGui_IsReady, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_GetContentRegionAvailX" ), GDX_ImGui_GetContentRegionAvailX, zPAR_TYPE_INT, zPAR_TYPE_VOID );

    // Table
    parser->DefineExternal( zSTRING( "GDX_ImGui_BeginTable" ), GDX_ImGui_BeginTable, zPAR_TYPE_VOID, zPAR_TYPE_INT, zPAR_TYPE_STRING, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_EndTable" ), GDX_ImGui_EndTable, zPAR_TYPE_VOID, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_TableNextColumn" ), GDX_ImGui_TableNextColumn, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_TableNextRow" ), GDX_ImGui_TableNextRow, zPAR_TYPE_VOID, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING( "GDX_ImGui_TableSetColumnIndex" ), GDX_ImGui_TableSetColumnIndex, zPAR_TYPE_VOID, zPAR_TYPE_INT, zPAR_TYPE_VOID );
    parser->DefineExternal( zSTRING("GDX_ImGui_TableSetupColumn"), GDX_ImGui_TableSetupColumn, zPAR_TYPE_VOID, zPAR_TYPE_STRING, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_INT, zPAR_TYPE_VOID );
}

