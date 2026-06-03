#include "ImGuiShim.h"
#include "GSky.h"
#include <VersionHelpers.h>
#include <ShellScalingApi.h>

#include "ImGuiEditorView.h"
#include "zCParser.h"
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>
#include <chrono>
#include <numeric>
#include <codecvt>

namespace ImGui {
    void TextUnformatted( const wchar_t* text ) {
        char dest[64];
        auto len = WideCharToMultiByte(CP_UTF8, 0, text, -1, dest, sizeof(dest), NULL, NULL);
        dest[std::min(static_cast<size_t>(len), sizeof(dest) - 1)] = '\0';
        ImGui::TextUnformatted( dest );
    }
}

#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
extern bool haveWindAnimations;
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
extern float* ShadowMapLambda;
extern float* ShadowMapBias;

enum class TX_QUALITY : uint16_t {
    VeryLow = 128,
    Low = 256,
    Medium = 512,
    High = 1024,
    VeryHigh = 2048,
    MAX = 16384,
};

int GetDpi( HWND hWnd )
{
    bool v81 = IsWindows8Point1OrGreater();
    bool v10 = IsWindows10OrGreater();

    if ( v81 || v10 ) {

        typedef HRESULT( WINAPI* GetDpiForMonitor_t )(
        HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);

        HMODULE hShcore = LoadLibraryW( L"Shcore.dll" );
        if ( hShcore ) {
            GetDpiForMonitor_t pGetDpiForMonitor = reinterpret_cast<GetDpiForMonitor_t>(GetProcAddress( hShcore, "GetDpiForMonitor" ));
            if ( pGetDpiForMonitor ) {
                HMONITOR hMonitor = ::MonitorFromWindow( hWnd, MONITOR_DEFAULTTONEAREST );
                UINT xdpi, ydpi;
                LRESULT success = pGetDpiForMonitor( hMonitor, MDT_EFFECTIVE_DPI, &xdpi, &ydpi );
                if ( success == S_OK ) {
                    FreeLibrary( hShcore );
                    return static_cast<int>(ydpi);
                }
            }
            FreeLibrary( hShcore );
        }
    }

    // fallback if not available
    HDC hDC = ::GetDC( hWnd );
    INT ydpi = ::GetDeviceCaps( hDC, LOGPIXELSY );
    ::ReleaseDC( NULL, hDC );

    return ydpi;
}

void ApplyFeatureLevel10Downgrades(GothicRendererSettings& s);

void ImGuiShim::Init(
    HWND Window,
    const Microsoft::WRL::ComPtr<ID3D11Device1>& device,
    const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context
)
{ 
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; //Not needed and it's annoying.
    OutputWindow = Window;
    ImGui_ImplWin32_Init( OutputWindow );
    ImGui_ImplDX11_Init( device.Get(), context.Get() );

    const auto actualDPI = GetDpi( Window );
    Initiated = true;

    std::vector<DisplayModeInfo> modes;
    Engine::GraphicsEngine->GetDisplayModeList( &modes );
    Resolutions.clear();
    for ( auto it = modes.rbegin(); it != modes.rend(); ++it ) {
        std::string s = std::to_string( (*it).Width ) + "x" + std::to_string( (*it).Height );
        Resolutions.emplace_back( std::make_pair(INT2((*it).Width, (*it).Height), s) );
    }

    //static const ImWchar euroGlyphRanges[] = {
    //    0x0020, 0x007E, // Basic Latin
    //    0x00A0, 0x00FF, // Latin-1 Supplement
    //    0x0100, 0x017F, // Latin Extended-A
    //    0x0180, 0x018F, // Latin Extended-B
    //    0x0400, 0x04FF, // Cyrillic
    //    0x2010, 0x2015, // Various dashes
    //    0x201E, 0x201E, // low-9 quotation mark
    //    0x201C, 0x201D, // high-9 quotation marks
    //    0,              // End of ranges
    //};
    ImFontConfig config = { };
    config.MergeMode = false;
    //config.GlyphRanges = euroGlyphRanges;
    const auto path = std::filesystem::current_path();
    const auto fontpath = path / "system" / "GD3D11" / "Fonts" / "Lato-Semibold.ttf";

    auto dpiScale = actualDPI / 96.0f;
    io.Fonts->AddFontFromFileTTF( fontpath.string().c_str(), 20.0f * dpiScale, &config );
    
    m_EditorView = std::make_unique<ImGuiEditorView>();
}


ImGuiShim::~ImGuiShim()
{
    if ( Initiated ) {
        ImGui_ImplWin32_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

void ImGuiShim::RenderLoop()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::GetIO().MouseDrawCursor = GetIsActive() && INT2( ImGui::GetMainViewport()->Size.x, ImGui::GetMainViewport()->Size.y ) != Engine::GraphicsEngine->GetResolution();

    static zSTRING GDX_IMGUI_BEGINFRAME = "GDX_IMGUI_BEGINFRAME";
    static zSTRING GDX_IMGUI_ENDFRAME = "GDX_IMGUI_ENDFRAME";
    static int beginFrameFn = zCParser::GetParser()->GetIndex( GDX_IMGUI_BEGINFRAME );
    static int endFrameFn = zCParser::GetParser()->GetIndex( GDX_IMGUI_ENDFRAME );

    static int retryFindFuncs = 0;
    if ( retryFindFuncs > 120 ) {
        if ( beginFrameFn == -1 ) { beginFrameFn = zCParser::GetParser()->GetIndex( GDX_IMGUI_BEGINFRAME ); }
        if ( endFrameFn == -1 ) { endFrameFn = zCParser::GetParser()->GetIndex( GDX_IMGUI_ENDFRAME ); }
        retryFindFuncs = 0;
    }

    LibShowBlockingThisFrame = false;
    LibShowNonBlockingThisFrame = false;
    if ( beginFrameFn != -1 ) {
        zCParser::GetParser()->CallFunc( beginFrameFn );
    } else {
        retryFindFuncs++;
    }

    auto oldSettings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( SettingsVisible ) {
        RenderSettingsWindow();
    } else if ( AdvancedSettingsVisible ) {
        RenderAdvancedSettingsWindow();
    }

    if (m_EditorView->GetIsEnabled()) {
        m_EditorView->Update(ImGui::GetIO().DeltaTime);
        m_EditorView->Render();
    }

    if ( memcmp( &oldSettings, &Engine::GAPI->GetRendererState().RendererSettings, sizeof( GothicRendererSettings ) ) != 0 ) {
        if ( oldSettings.GraphicsPreset == Engine::GAPI->GetRendererState().RendererSettings.GraphicsPreset ) {
            Engine::GAPI->GetRendererState().RendererSettings.GraphicsPreset = GothicRendererSettings::E_GraphicsPreset::GRAPHICS_CUSTOM;
        }
        if ( FeatureLevel10Compatibility ) {
            ApplyFeatureLevel10Downgrades( Engine::GAPI->GetRendererState().RendererSettings );
        }
    }
    //if ( DemoVisible )
    //    ImGui::ShowDemoWindow();

    if ( GetBlockGameInput() != m_lastFrameBlockGameInput ) {
        m_lastFrameBlockGameInput = GetBlockGameInput();
        D3D11GraphicsEngine::UpdateShouldBlockGameInput();
    }

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData() );

    if ( endFrameFn != -1 ) {
        zCParser::GetParser()->CallFunc( endFrameFn );
    };
}

bool ImGuiShim::GetIsActive() {
    return Initiated && (
        SettingsVisible
        || AdvancedSettingsVisible 
        || (m_EditorView->GetIsEnabled())
        || LibShowBlockingThisFrame
        || LibShowNonBlockingThisFrame
    );
}

bool ImGuiShim::GetBlockGameInput()
{
    if ( !GetIsActive() ) {
        return false;
    }
    if ( SettingsVisible
        || AdvancedSettingsVisible
        || (m_EditorView->GetBlockGameInput())
        || LibShowBlockingThisFrame ) {
        return true;
        }
    return false;
}

LRESULT ImGuiShim::OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    if ( Initiated && GetIsActive() )
    {
        if (m_EditorView->GetIsEnabled()) {
            m_EditorView->OnWindowMessage( hWnd, msg, wParam, lParam );
        }
        return ImGui_ImplWin32_WndProcHandler( hWnd, msg, wParam, lParam );
    }
    return 0;
}

void ImGuiShim::OnResize( INT2 newSize )
{
    CurrentResolution = newSize;

    std::vector<DisplayModeInfo> modes;
    Engine::GraphicsEngine->GetDisplayModeList( &modes );
    Resolutions.clear();
    for ( auto it = modes.rbegin(); it != modes.rend(); ++it ) {
        std::string s = std::to_string( (*it).Width ) + "x" + std::to_string( (*it).Height );
        Resolutions.emplace_back( std::make_pair(INT2((*it).Width, (*it).Height), s) );
    }
}

template <typename T>
bool ImComboBoxC( const char* id, const std::vector<std::pair<const char*, T>>& items, T* storage, const std::function<void()>& selected ) {
    if ( storage == nullptr || items.size() == 0 ) {
        return ImGui::BeginCombo( id, "invalid storage" );
    }
    std::pair<const char*, T> selectedItem = items[0];
    for ( auto& it : items ) {
        if ( it.second == *storage ) {
            selectedItem = it;
            break;
        }
    }
    if ( ImGui::BeginCombo( id, selectedItem.first ) ) {
        for ( size_t i = 0; i < items.size(); i++ ) {
            bool isSelected = (*storage == items[i].second);

            if ( ImGui::Selectable( items[i].first, isSelected ) ) {
                *storage = items[i].second;
                selected();
            }

            if ( isSelected ) {
                ImGui::SetItemDefaultFocus();
            }
        }
        return true;
    }
    return false;
}

template <typename T>
bool ImComboBoxCT( const char* id, const std::vector<std::tuple<const char*, T, const char*>>& items, T* storage, const std::function<void()>& selected ) {
    if ( storage == nullptr || items.size() == 0 ) {
        return ImGui::BeginCombo( id, "invalid storage" );
    }
    auto selectedItem = items[0];
    for ( auto& it : items ) {
        if ( std::get<1>( it ) == *storage ) {
            selectedItem = it;
            break;
        }
    }
    if ( ImGui::BeginCombo( id, std::get<0>( selectedItem )) ) {
        for ( size_t i = 0; i < items.size(); i++ ) {
            bool isSelected = (*storage == std::get<1>( items[i] ));

            if ( ImGui::Selectable( std::get<0>( items[i] ), isSelected ) ) {
                *storage = std::get<1>( items[i] );
                selected();
            }
            if ( std::get<2>(items[i]) ) {
                ImGui::SetItemTooltip( "%s", std::get<2>( items[i] ) );
            }

            if ( isSelected ) {
                ImGui::SetItemDefaultFocus();
            }
        }
        return true;
    }
    return false;
}

template <typename T>
bool ImComboBox( const char* id, const std::vector<std::pair<const char*, T>>& items, T* storage ) {
    if ( storage == nullptr || items.size() == 0 ) {
        return ImGui::BeginCombo( id, "invalid storage" );
    }
    std::pair<const char*, T> selectedItem = items[0];
    for ( auto& it : items ) {
        if ( it.second == *storage ) {
            selectedItem = it;
            break;
        }
    }
    if ( ImGui::BeginCombo( id, selectedItem.first ) ) {
        for ( size_t i = 0; i < items.size(); i++ ) {
            bool isSelected = (*storage == items[i].second);

            if ( ImGui::Selectable( items[i].first, isSelected ) ) {
                *storage = items[i].second;
            }

            if ( isSelected ) {
                ImGui::SetItemDefaultFocus();
            }
        }
        return true;
    }
    return false;
}

void ImText( const char* label, const ImVec2& size ) {
    auto& col = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Button );

    ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_ButtonActive, col );
    ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_ButtonHovered, col );
    ImGui::PushStyleVarX( ImGuiStyleVar_::ImGuiStyleVar_ButtonTextAlign, 0 );

    ImGui::Button( label, size );
    ImGui::PopStyleVar( 1 );

    ImGui::PopStyleColor( 2 );
}

// Helper function to edit a direction vector using ImGuizmo::ViewManipulate
// Returns true if the direction was modified
bool ImGuizmoDirectionEdit( const char* label, XMFLOAT3& direction, float widgetSize = 100.0f )
{
    // Normalize the input direction
    XMVECTOR dirVec = XMLoadFloat3( &direction );
    dirVec = XMVector3Normalize( dirVec );

    // Build a view matrix looking in the direction
    XMFLOAT3 dirNorm;
    XMStoreFloat3( &dirNorm, dirVec );

    XMVECTOR upVec = fabsf( dirNorm.y ) < 0.99f ? XMVectorSet( 0, 1, 0, 0 ) : XMVectorSet( 1, 0, 0, 0 );
    XMVECTOR rightVec = XMVector3Normalize( XMVector3Cross( upVec, dirVec ) );
    upVec = XMVector3Normalize( XMVector3Cross( dirVec, rightVec ) );

    XMFLOAT3 right, up;
    XMStoreFloat3( &right, rightVec );
    XMStoreFloat3( &up, upVec );

    float viewMatrix[16] = {
        right.x,    up.x,    dirNorm.x,  0,
        right.y,    up.y,    dirNorm.y,  0,
        right.z,    up.z,    dirNorm.z,  0,
        0,          0,       0,          1
    };

    // Get current cursor position for the widget placement
    ImVec2 widgetPos = ImGui::GetCursorScreenPos();

    ImGui::Text( "%s", label );
    ImGui::SameLine();

    // Draw the ViewManipulate gizmo
    ImGuizmo::SetDrawlist();
    ImGuizmo::ViewManipulate( viewMatrix, 1.0f, ImVec2( widgetPos.x + 120.0f, widgetPos.y ), ImVec2( widgetSize, widgetSize ), 0x10101010 );

    // Extract the new direction from the view matrix (forward vector / third column)
    XMFLOAT3 newDirection( viewMatrix[2], viewMatrix[6], viewMatrix[10] );

    // Check if direction changed
    bool modified = (newDirection.x != direction.x || newDirection.y != direction.y || newDirection.z != direction.z);
    direction = newDirection;

    // Reserve space for the widget, use InvisibleButton to stop mouse movement from moving the current window
    ImGui::PushID( label );
    ImGui::InvisibleButton( "##invisible", ImVec2( widgetSize + 120.0f, widgetSize ) );

    // Also provide a numeric input for precise control
    modified |= ImGui::DragFloat3( "##values", &direction.x, 0.001f );
    ImGui::PopID();

    return modified;
}

void ApplyFeatureLevel10Downgrades(GothicRendererSettings& s) {
    // one 4k texture, 1/2 2k textures max.
    s.NumShadowCascades = std::min(s.NumShadowCascades, MAX_CSM_CASCADES);

    if (s.NumShadowCascades >= 2) {
        s.DebugSettings.ShadowCascades.Lambda = D3D11ShadowMap::lambdaBiasTable[s.NumShadowCascades].lambda;
        s.DebugSettings.ShadowCascades.Bias = D3D11ShadowMap::lambdaBiasTable[s.NumShadowCascades].bias;
    }
}

void ApplyGraphicsPresets( GothicRendererSettings& s ) {
    const auto preset = s.GraphicsPreset;
    if ( preset == GothicRendererSettings::E_GraphicsPreset::GRAPHICS_CUSTOM ) {
        return;
    }

    switch ( preset ) {
    case GothicRendererSettings::GRAPHICS_LOW:
    {
        s.ChangeWindowPreset = WINDOW_MODE_FULLSCREEN_BORDERLESS;

        s.CompressBackBuffer = true;
        s.WorldShadowRangeScale = 1.0f;
        s.NumShadowCascades = 2;
        s.DebugSettings.FeatureSet.UseShadowAtlas = true;
        s.ShadowMapSize = 1024;
        s.ShadowFrustumCullingMode = GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_AGGRESSIVE;
        s.ShadowSoftness = 0.85f;
        s.SmoothShadowCameraUpdate = true;
        s.SmoothShadowFrequency = 500;
        s.ShadowFilterMode = GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_DISABLED;

        s.EnableDynamicLighting = false;
        s.EnablePointlightShadows = GothicRendererSettings::EPointLightShadowMode::PLS_DISABLED;

        s.AoMode = AOMode::AO_NONE;

        s.textureMaxSize = static_cast<int>(TX_QUALITY::Medium);

        s.AntiAliasingMode = GothicRendererSettings::E_AntiAliasingMode::AA_NONE;
        s.SectionDrawRadius = 2;
        s.VisualFXDrawRadius = 5'000;
        s.OutdoorVobDrawRadius = 30'000;
        s.OutdoorSmallVobDrawRadius = 10'000;
        s.IndoorVobDrawRadius = 10'000;
        
        s.WindQuality = GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE;
        s.HeroAffectsObjects = 0;

        s.EnableGodRays = false;
    }
    break;
    case GothicRendererSettings::GRAPHICS_MEDIUM:
    {
        s.ChangeWindowPreset = WINDOW_MODE_FULLSCREEN_BORDERLESS;

        s.CompressBackBuffer = true;
        s.WorldShadowRangeScale = 1.0f;
        s.NumShadowCascades = 3;
        s.DebugSettings.FeatureSet.UseShadowAtlas = true;
        s.ShadowMapSize = 2048;
        s.ShadowFrustumCullingMode = GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_CONSERVATIVE;
        s.ShadowSoftness = 0.85f;
        s.SmoothShadowCameraUpdate = true;
        s.SmoothShadowFrequency = 1000;
        s.ShadowFilterMode = GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_SIMPLE;
            
        s.EnableDynamicLighting = true;
        s.EnablePointlightShadows = GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY;

        s.AoMode = AOMode::AO_NONE;

        s.textureMaxSize = static_cast<int>(TX_QUALITY::Medium);

        s.AntiAliasingMode = GothicRendererSettings::E_AntiAliasingMode::AA_SMAA;
        s.SectionDrawRadius = 4;
        s.VisualFXDrawRadius = 6'000;
        s.OutdoorVobDrawRadius = 30'000;
        s.OutdoorSmallVobDrawRadius = 15'000;
        s.IndoorVobDrawRadius = 15'000;

        s.WindQuality = GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE;
        s.HeroAffectsObjects = 1;

        s.EnableGodRays = true;
    }
    break;
    case GothicRendererSettings::GRAPHICS_HIGH:
    {
        s.ChangeWindowPreset = WINDOW_MODE_FULLSCREEN_BORDERLESS;

        s.CompressBackBuffer = false;
        s.WorldShadowRangeScale = 1.0f;
        s.NumShadowCascades = 3;
        s.ShadowMapSize = 4096;
        s.ShadowFrustumCullingMode = GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_CONSERVATIVE;
        s.ShadowSoftness = 1.0f;
        s.SmoothShadowCameraUpdate = false;
        s.SmoothShadowFrequency = 1000;
        s.ShadowFilterMode = GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_SIMPLE;

        s.EnableDynamicLighting = true;
        s.EnablePointlightShadows = GothicRendererSettings::EPointLightShadowMode::PLS_UPDATE_DYNAMIC;

        s.AoMode = AOMode::AO_HBAO;
        s.HbaoSettings.SsaoStepCount = 4;
        s.HbaoSettings.SsaoBlurRadius = 4;

        s.textureMaxSize = static_cast<int>(TX_QUALITY::High);

        s.AntiAliasingMode = GothicRendererSettings::E_AntiAliasingMode::AA_SMAA;
        s.SectionDrawRadius = 4;
        s.VisualFXDrawRadius = 8'000;
        s.OutdoorVobDrawRadius = 40'000;
        s.OutdoorSmallVobDrawRadius = 25'000;
        s.IndoorVobDrawRadius = 20'000;

        s.WindQuality = GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED;
        s.HeroAffectsObjects = 1;

        s.EnableGodRays = true;
    }
    break;
    case GothicRendererSettings::GRAPHICS_VERY_HIGH:
    {
        s.ChangeWindowPreset = WINDOW_MODE_FULLSCREEN_BORDERLESS;

        s.CompressBackBuffer = false;
        s.WorldShadowRangeScale = 1.0f;
        s.NumShadowCascades = 4;
        s.ShadowMapSize = 4096;
        s.ShadowFrustumCullingMode = GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_CONSERVATIVE;
        s.ShadowSoftness = 1.0f;
        s.SmoothShadowCameraUpdate = false;
        s.SmoothShadowFrequency = 1000;
        s.ShadowFilterMode = GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_PCSS;

        s.EnableDynamicLighting = true;
        s.EnablePointlightShadows = GothicRendererSettings::EPointLightShadowMode::PLS_UPDATE_DYNAMIC;

        s.AoMode = AOMode::AO_HBAO;
        s.HbaoSettings.SsaoStepCount = 8;
        s.HbaoSettings.SsaoBlurRadius = 4;

        s.textureMaxSize = static_cast<int>(TX_QUALITY::VeryHigh);

        s.AntiAliasingMode = GothicRendererSettings::E_AntiAliasingMode::AA_SMAA;
        s.SectionDrawRadius = 5;
        s.VisualFXDrawRadius = 10'000;
        s.OutdoorVobDrawRadius = 40'000;
        s.OutdoorSmallVobDrawRadius = 25'000;
        s.IndoorVobDrawRadius = 20'000;

        s.WindQuality = GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED;
        s.HeroAffectsObjects = 1;

        s.EnableGodRays = true;
    }
    break;
    default:
        return;
    }

    if (FeatureLevel10Compatibility) {
        ApplyFeatureLevel10Downgrades(s);
    }
    
    Engine::GAPI->UpdateTextureMaxSize();
    Engine::GraphicsEngine->ReloadShaders();
    Engine::GAPI->UpdateCompressBackBuffer();
}

namespace
{
    bool IsFSRUpscaler( GothicRendererSettings::E_Upscaler v ) {
        return v == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3
            || v == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_2;
    }
    void FixupSettings(GothicRendererSettings& s) {
        if (s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR) {
            if ( !IsFSRUpscaler( s.Upscaler ) ) {
                s.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3;
            }
        }
        if (s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_TAA
            && (s.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_2 || s.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3)) {
            // don't allow TAA and FSR2 at the same time.
            s.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_1;
        }
        if (s.ResolutionScalePercent > 100 && s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR) {
            // switch to regular TAA if upsampled
            s.AntiAliasingMode = GothicRendererSettings::AA_TAA;
        }
    }
}

void ImGuiShim::RenderSettingsWindow()
{
    // Autosized settings by child objects & centered
    IM_ASSERT( ImGui::GetCurrentContext() != NULL && "Missing Dear ImGui context!" );
    IMGUI_CHECKVERSION();

    auto windowSize = CurrentResolution;
    // Get the center point of the screen, then shift the window by 50% of its size in both directions.
    // TIP: Don't use ImGui::GetMainViewport for framebuffer sizes since GD3D11 can undersample or oversample the game.
    // Use whatever the resolution is spit out instead.
    ImVec2 buttonWidth( 275, 0 );
    auto& style = ImGui::GetStyle();

#ifdef IS_DEV_BUILD
    static const char* settingsLabel = "GD3D11 " VERSION_NUMBER " - (" BUILD_DATE ")";
#else
    static const char* settingsLabel = "GD3D11 " VERSION_NUMBER;
#endif

    ShaderCategory shadersToReload = ShaderCategory::None;

    ImGui::SetNextWindowPos( ImVec2( windowSize.x / 2, windowSize.y / 2 ), ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );
    if ( ImGui::Begin( settingsLabel, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize ) ) {
        GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;
        FixupSettings(settings);

        static std::vector<std::pair<const char*, int>> graphicsPresets = {
            {"Custom", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_CUSTOM},
            {"Low", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_LOW},
            {"Medium", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_MEDIUM},
            {"High", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_HIGH},
            {"Very High", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_VERY_HIGH},
        };

        ImGui::TextUnformatted("Graphics Preset"); ImGui::SameLine();
        
        ImGui::PushItemWidth( 250 );
        if ( ImComboBoxC( "##GraphicsPreset", graphicsPresets, (int*)&settings.GraphicsPreset, [&settings]() {
            ApplyGraphicsPresets( settings );
            } ) ) {
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::Separator();
        
        {
            ImGui::BeginGroup();
            ImGui::Checkbox( "Vsync", &settings.EnableVSync );
            if ( ImGui::Checkbox( "NormalMaps", &settings.AllowNormalmaps ) ) {
                Engine::GAPI->UpdateTextureMaxSize();
            }

            static std::vector<std::tuple<const char*, AOMode, const char*>> aoModes = {
                    {"Disabled", AOMode::AO_NONE, nullptr},
                    {"HBAO+", AOMode::AO_HBAO, "NVIDIA HBAO+ (Horizon-Based Ambient Occlusion Plus)"},
                    {"SAO", AOMode::AO_SAO, nullptr},
                    {"ASSAO", AOMode::AO_ASSAO, "Intel ASSAO (Adaptive Screen Space Ambient Occlusion)"},
            };
            if ( ImComboBoxCT( "AO Mode", aoModes, &settings.AoMode, [] {
                Engine::GraphicsEngine->ReloadShaders( ShaderCategory::Other );
                } ) ) {
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip( "Screen-Space ambient occlusion mode.\nChanging this will reload shaders." );

            if ( ImGui::Checkbox( "Godrays", &settings.EnableGodRays ) ) {
                Engine::GraphicsEngine->ReloadShaders( ShaderCategory::Other );
            }
            ImGui::SetItemTooltip( "Changing this will reload shaders." );

            ImGui::Checkbox( "Depth of Field", &settings.EnableDoF );
            ImGui::SetItemTooltip( "Enable Depth of Field with bokeh blur." );
            static std::vector<std::tuple<const char*, GothicRendererSettings::E_AntiAliasingMode, const char*>> antiAliasing = {
                {"Disabled", GothicRendererSettings::E_AntiAliasingMode::AA_NONE, nullptr },
                {"SMAA", GothicRendererSettings::E_AntiAliasingMode::AA_SMAA, nullptr },
                {"TAA", GothicRendererSettings::E_AntiAliasingMode::AA_TAA, "Temporal Anti-Aliasing" },
                {"FSR 2", GothicRendererSettings::E_AntiAliasingMode::AA_FSR, "FidelityFX Super Resolution 2" },
                {"FSR 3", GothicRendererSettings::E_AntiAliasingMode::AA_FSR3, "FidelityFX Super Resolution 3"},

            };
            {
                ImGui::PushID( "AntiAliasingSettings" );
                auto selectedMode = settings.AntiAliasingMode;
                if ( selectedMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR && settings.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3 ) {
                    selectedMode = GothicRendererSettings::E_AntiAliasingMode::AA_FSR3;
                }
                if ( ImComboBoxCT( "Anti Aliasing", antiAliasing, &selectedMode, [&selectedMode, &settings] {
                    if ( selectedMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR3 ) {
                        selectedMode = GothicRendererSettings::E_AntiAliasingMode::AA_FSR;
                        settings.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3;
                    } else if ( selectedMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR ) {
                        settings.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_2;
                    }
                    settings.AntiAliasingMode = selectedMode;
                    } ) ) {
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }

            ImGui::Checkbox( "HDR", &settings.EnableHDR );
            if ( ImGui::Checkbox( "Shadows", &settings.EnableShadows ) ) {
                shadersToReload |= ShaderCategory::LightsAndShadows;
            }
            {
                static std::vector<std::pair<const char*, GothicRendererSettings::E_ShadowFilterMode>> shadowFilterModes = {
                    {"Disabled", GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_DISABLED},
                    {"Simple", GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_SIMPLE},
                    {"PCSS", GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_PCSS},
                };
                if ( ImComboBoxC( "Shadow filtering", shadowFilterModes, &settings.ShadowFilterMode, [&shadersToReload]() {
                    shadersToReload |= ShaderCategory::LightsAndShadows;
                    } ) ) {
                    ImGui::EndCombo();
                }
            }

            if ( ImGui::Checkbox( "Compress Backbuffer", &settings.CompressBackBuffer ) ) {
                Engine::GAPI->UpdateCompressBackBuffer();
            }
            ImGui::Checkbox( "Animate Static Vobs", &settings.AnimateStaticVobs );

#if defined(BUILD_GOTHIC_2_6_fix) || (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F))
#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
            if ( haveWindAnimations )
#endif
            {
                bool windEffect = settings.WindQuality != GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE;
                if ( ImGui::Checkbox( "Wind effect", &windEffect ) ) {
                    settings.WindQuality = windEffect
                        ? GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED
                        : GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE;
                    shadersToReload |= ShaderCategory::Other;
                }
                ImGui::SetItemTooltip( "Enables trees, grass and wheats to wave with the wind" );

                ImGui::Text( "Wind strength" ); ImGui::SameLine();

                ImGui::BeginDisabled( settings.WindQuality == GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE );
                ImGui::SliderFloat( "##Wind strength", &settings.GlobalWindStrength, 0.1f, 5.0f, "%.2f" );
                ImGui::EndDisabled();
            }

            if ( ImGui::Checkbox( "Hero affects objects", &settings.HeroAffectsObjects ) ) {
                shadersToReload |= ShaderCategory::Other;
            }
            ImGui::SetItemTooltip( "Grass and wheats may move when the player runs through it." );
#endif //BUILD_GOTHIC_2_6_fix

            ImGui::Checkbox( "Enable Rain", &settings.EnableRain );
            ImGui::Checkbox( "Enable Rain Effects", &settings.EnableRainEffects );
            if ( ImGui::Checkbox( "Enable Water waves", &settings.EnableWaterAnimation ) ) {
                shadersToReload |= ShaderCategory::Water;
            }
            ImGui::Checkbox( "Limit Light Intensity", &settings.LimitLightIntesity );
            ImGui::Checkbox( "Draw World Section Intersections", &settings.DrawSectionIntersections );
            ImGui::SetItemTooltip( "This option draws every world chunk that intersect with GD3D11 world draw distance." );

            ImGui::Checkbox( "Occlusion Culling", &settings.EnableOcclusionCulling );
            ImGui::SetItemTooltip( "Hides objects that are not visible by camera. Doesn't work properly, turn off if you don't play on potato." );

            ImGui::EndGroup();
        }

        ImGui::SameLine();

        {
            ImGui::BeginGroup();
            ImGui::PushItemWidth( 250 );

            for (size_t i = 0; i < Resolutions.size(); ++i){
                if (Resolutions[i].first == CurrentResolution) {
                    ResolutionState = i;
                    break;
                }
            }

            static std::string resolutionLabel = "Resolution";

            if ( settings.ResolutionScalePercent != 100 ) {
                std::stringstream ss;
                ss << "Resolution (scaled: " << (CurrentResolution.x * settings.ResolutionScalePercent / 100)
                    << " x "
                    << (CurrentResolution.y * settings.ResolutionScalePercent / 100)
                    << ")";
                resolutionLabel = ss.str();
            }

            ImText( settings.ResolutionScalePercent != 100 ? resolutionLabel.c_str() : "Resolution", buttonWidth ); ImGui::SameLine();
            if ( ImGui::BeginCombo( "##Resolution", Resolutions[ResolutionState].second.c_str() ) ) {
                for ( size_t i = 0; i < Resolutions.size(); i++ ) {
                    bool isSelected = (ResolutionState == i);

                    if ( ImGui::Selectable( Resolutions[i].second.c_str(), isSelected ) ) {
                        Engine::GraphicsEngine->TriggerResize(Resolutions[i].first);
                    }

                    if ( isSelected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImText( "Resolution Scale", buttonWidth ); ImGui::SameLine();
            if ( settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_2 || settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3 ) {
                settings.ResolutionScalePercent = std::clamp( settings.ResolutionScalePercent, 33, 100 );
                // Display "levels" as typical for FSR
                static std::vector<std::pair<const char*, int>> fsrLevels = {
                    { "Native AA", 100 },
                    { "High Quality", 83 },
                    { "Quality", 75 },
                    { "Balanced", 66 },
                    { "Performance", 50 },
                    { "Ultra Performance", 33 },
                };
                if (ImComboBox( "##ResolutionScalePercent", fsrLevels, &settings.ResolutionScalePercent ) ) {
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("Effective resolution: %d x %d",
                    CurrentResolution.x * settings.ResolutionScalePercent / 100,
                    CurrentResolution.y * settings.ResolutionScalePercent / 100
                );
            } else {
                static float previousResolutionScale = static_cast<float>(settings.ResolutionScalePercent);
                if ( ImGui::SliderFloat( "##ResolutionScalePercent", &previousResolutionScale, 25.0f, 200.0f, "%.0f%%" ) ) {
                    previousResolutionScale = std::clamp( previousResolutionScale, 25.0f, 200.0f );
                    settings.ResolutionScalePercent = static_cast<int>(previousResolutionScale);
                }
                ImGui::SetItemTooltip("Effective resolution: %d x %d",
                    CurrentResolution.x * settings.ResolutionScalePercent / 100,
                    CurrentResolution.y * settings.ResolutionScalePercent / 100
                );
            }

            ImText( "Upscaler", buttonWidth ); ImGui::SameLine();
            static std::vector<std::pair<const char*, GothicRendererSettings::E_Upscaler>> upscalers = {
                { "Simple", GothicRendererSettings::E_Upscaler::UPSCALER_DEFAULT },
                { "FSR 1", GothicRendererSettings::E_Upscaler::UPSCALER_FSR_1 },
                { "FSR 2", GothicRendererSettings::E_Upscaler::UPSCALER_FSR_2 },
                { "FSR 3", GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3 },
            };
            if ( ImComboBox( "##Upscaler", upscalers, &settings.Upscaler ) ) {
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled( settings.ResolutionScalePercent >= 100 );
            {
                if ( settings.Upscaler ) {
                    ImText( "Upscaler sharpening", buttonWidth ); ImGui::SameLine();
                    if ( ImGui::SliderFloat( "##Upscale sharpening", &settings.SharpenFactor, 0.0f, 1.0f, "%.3f%" ) ) {
                        settings.SharpenFactor = std::clamp( settings.SharpenFactor, 0.0f, 1.0f );
                    }
                }

                ImGui::EndDisabled();
            }


            ImText( "Texture Quality", buttonWidth ); ImGui::SameLine();
            static std::vector<std::pair<const char*, int>> QualityOptions = {
                { "Very Low", static_cast<int>(TX_QUALITY::VeryLow) },
                { "Low", static_cast<int>(TX_QUALITY::Low) },
                { "Medium", static_cast<int>(TX_QUALITY::Medium) },
                { "High", static_cast<int>(TX_QUALITY::High) },
                { "Very High", static_cast<int>(TX_QUALITY::VeryHigh) },
                { "Extreme", static_cast<int>(TX_QUALITY::MAX) }, // TODO: this should depend on the GPU capabilities like in the original game
            };
            
            if (settings.textureMaxSize > QualityOptions.back().second) {
                settings.textureMaxSize = QualityOptions.back().second;
                Engine::GAPI->UpdateTextureMaxSize();
            }
            if (settings.textureMaxSize < QualityOptions.front().second) {
                settings.textureMaxSize = QualityOptions.front().second;
                Engine::GAPI->UpdateTextureMaxSize();
            }

            if (ImComboBoxC("##TextureQuality", QualityOptions, &settings.textureMaxSize, []{
                Engine::GAPI->UpdateTextureMaxSize();
            } ))
            {
                ImGui::EndCombo();
            }

            ImText( "Display Mode [*]", buttonWidth );
            ImGui::SetItemTooltip("some changes may require a restart");
            ImGui::SameLine();

            static auto displayModeState = InterpretWindowMode( settings );
            static std::vector<std::tuple<const char*, WindowModes, const char*>> DisplayEnums = {
                { "Fullscreen Borderless", WindowModes::WINDOW_MODE_FULLSCREEN_BORDERLESS, nullptr },
                { "Fullscreen Lowlatency [*]", WindowModes::WINDOW_MODE_FULLSCREEN_LOWLATENCY, "switching requires restarting the game"},
                { "Fullscreen Exclusive [*]", WindowModes::WINDOW_MODE_FULLSCREEN_EXCLUSIVE, "switching requires restarting the game"},
                { "Windowed", WindowModes::WINDOW_MODE_WINDOWED, nullptr},
            };
            
            if ( ImComboBoxCT( "##DisplayMode", DisplayEnums, &displayModeState, [&settings] {
                // selected
                settings.ChangeWindowPreset = displayModeState;
                } ) ) {
                ImGui::EndCombo();
            }

            ImText( "Shadow Quality", buttonWidth ); ImGui::SameLine();

            const static std::vector<std::pair<const char*, int>> shadowMapSizesMax = {
                {"very low", 512},
                {"low", 1024},
                {"medium", 2048},
                {"high", 4096},
                {"very high", 8192},
                {"ultra high", 16384},
            };
            const static std::vector<std::pair<const char*, int>> shadowMapSizesDxFeature10 = {
                {"very low", 512},
                {"low", 1024},
                {"medium", 2048},
                {"high", 4096},
                {"very high", 8192},
            };
            const std::vector<std::pair<const char*, int>>& shadowMapSizes = FeatureLevel10Compatibility
                ? shadowMapSizesDxFeature10
                : shadowMapSizesMax;

            if ( ImComboBoxC( "##ShadowQuality", shadowMapSizes, &settings.ShadowMapSize, [&shadersToReload]{
                shadersToReload |= ShaderCategory::LightsAndShadows;
            } ) ) {
                ImGui::EndCombo();
            }

            ImText( "Dynamic Shadows", buttonWidth ); ImGui::SameLine();
            
            const static std::vector<std::tuple<const char*, GothicRendererSettings::EPointLightShadowMode, const char*>> dynamicShadowValues = {
                { "Off", GothicRendererSettings::EPointLightShadowMode::PLS_DISABLED, nullptr },
                { "Static", GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY, nullptr },
                { "Dynamic Update", GothicRendererSettings::EPointLightShadowMode::PLS_UPDATE_DYNAMIC, nullptr },
                { "Full", GothicRendererSettings::EPointLightShadowMode::PLS_FULL, "Very expensive. Don't use unless you encounter visual bugs." },
            };

            if ( ImComboBoxCT( "##DynamicShadows", dynamicShadowValues, &settings.EnablePointlightShadows, [] {} ) ) {
                ImGui::EndCombo();
            }

            static bool fpsLimitEnabled = 0;
            fpsLimitEnabled = settings.FpsLimit > 0;

            ImText( "FPS Limit", { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            if ( ImGui::Checkbox( "##Enable FPS Limit", &fpsLimitEnabled ) ) {
                if ( !fpsLimitEnabled ) {
                    settings.FpsLimit = 0;
                } else {
                    settings.FpsLimit = 60;
                }
            }
            ImGui::SameLine();

            ImGui::BeginDisabled( !fpsLimitEnabled );
            ImGui::SliderInt( "##FPSLimit", &settings.FpsLimit, 10, 300 );
            ImGui::EndDisabled();

            ImText( "Object Draw Distance", buttonWidth ); ImGui::SameLine();
            float objectDrawDistance = settings.OutdoorVobDrawRadius / 1000.0f;
            if ( ImGui::SliderFloat( "##OutdoorVobDrawRadius", &objectDrawDistance, 1.f, 100.0f, "%.0f" ) ) {
                settings.OutdoorVobDrawRadius = static_cast<float>(objectDrawDistance * 1000.0f);
            }

            float smallObjectDrawDistance = settings.OutdoorSmallVobDrawRadius / 1000.0f;
            ImText( "Small Object Draw Distance", buttonWidth ); ImGui::SameLine();
            if ( ImGui::SliderFloat( "##OutdoorSmallVobDrawRadius", &smallObjectDrawDistance, 1.f, 100.0f, "%.0f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput ) ) {
                settings.OutdoorSmallVobDrawRadius = static_cast<float>(smallObjectDrawDistance * 1000.0f);
            }

            float visualFXDrawDistance = settings.VisualFXDrawRadius / 1000.0f;
            ImText( "VisualFX Draw Distance", buttonWidth ); ImGui::SameLine();
            if ( ImGui::SliderFloat( "##VisualFXDrawRadius", &visualFXDrawDistance, 0.1f, 10.0f, "%.0f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput ) ) {
                settings.VisualFXDrawRadius = static_cast<float>(visualFXDrawDistance * 1000.0f);
            }
            ImText( "World Draw Distance", buttonWidth ); ImGui::SameLine();
            ImGui::SliderInt( "##SectionDrawRadius", &settings.SectionDrawRadius, 1, 20, "%d", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );

            ImText( "Contrast", buttonWidth ); ImGui::SameLine();
            ImGui::SliderFloat( "##Contrast", &settings.GammaValue, 0.20f, 2.0f, "%.2f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );

            ImText( "Brightness", buttonWidth ); ImGui::SameLine();
            ImGui::SliderFloat( "##Brightness", &settings.BrightnessValue, 0.10f, 3.0f, "%.2f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
            ImGui::PopItemWidth();


            ImGui::Spacing();
            auto availableSize = ImGui::GetWindowSize();
            static const char* advancedSettingsHint = "Advanced settings: CTRL+F11 ";
            auto textSize = ImGui::CalcTextSize( advancedSettingsHint );
            ImGui::SetCursorPos( ImVec2( (availableSize.x - textSize.x) - 15, availableSize.y - textSize.y - 50 ) );
            ImGui::TextUnformatted( advancedSettingsHint );

            ImGui::EndGroup();
        }
        
        auto saved = ImGui::Button( "Save Settings", ImVec2( ImGui::GetContentRegionAvail().x, 30.f ) );
        auto worldSettingsPath = Engine::GAPI->GetLoadedWorldSettingsPath(false);
        const bool isInWorld = !worldSettingsPath.empty();
        const bool hasWorldSettings = Toolbox::FileExists( worldSettingsPath );
        if ( ( ImGui::GetIO().KeyCtrl || hasWorldSettings ) && isInWorld ) {
            ImGui::SetItemTooltip("Save settings to \"%s\"", worldSettingsPath.c_str());
        } else {
            ImGui::SetItemTooltip("Save settings.\nCTRL+Click to save just for the current world.");
        }
        
        if ( saved ) {
            Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::UI_ClosedSettings );
            if ( (ImGui::GetIO().KeyCtrl || hasWorldSettings) && isInWorld ) {
                Engine::GAPI->SaveRendererWorldSettings( settings );
            } else {
                Engine::GAPI->SaveRendererWorldSettings( settings, MENU_SETTINGS_FILE);
            }
            Engine::GAPI->SaveMenuSettings( MENU_SETTINGS_FILE );
        }
    }
    ImGui::End();

    if ( shadersToReload != ShaderCategory::None ) {
        Engine::GraphicsEngine->ReloadShaders( shadersToReload );
    }
}

void RenderAdvancedColumn1( GothicRendererSettings& settings, GothicAPI* gapi ) {
    if ( ImGui::Begin( "Sky", nullptr, ImGuiWindowFlags_NoCollapse ) ) {

        ImGui::SeparatorText( "GodRays" );
        {
            ImGui::PushID( "GodRaysSettings" );
            if ( ImGui::Checkbox( "GodRays", &settings.EnableGodRays ) ) {
                Engine::GraphicsEngine->ReloadShaders( ShaderCategory::Other );
            }
            ImGui::SetItemTooltip( "Changing this will reload shaders." );
            ImGui::DragFloat( "GodRayDecay", &settings.GodRayDecay, 0.01f );
            ImGui::DragFloat( "GodRayWeight", &settings.GodRayWeight, 0.01f );
            ImGui::ColorEdit3( "GodRayColorMod", &settings.GodRayColorMod.x );
            ImGui::DragFloat( "GodRayDensity", &settings.GodRayDensity, 0.01f );
            ImGui::PopID();
        }

        ImGui::SeparatorText( "Depth of Field" );
        {
            ImGui::PushID( "DoFSettings" );
            ImGui::Checkbox( "Enabled", &settings.EnableDoF );
            ImGui::DragFloat( "Focus Range", &settings.DoFFocusRange, 50.0f, 500.0f, 50000.0f, "%.0f" );
            ImGui::SetItemTooltip( "Range around the auto-focus point that remains sharp." );
            const char* bokehOrBlurLabel = settings.DoFGaussBlur ? "Blur Radius" : "Bokeh Radius";
            ImGui::DragFloat( bokehOrBlurLabel, &settings.DoFBokehRadius, 0.5f, 1.0f, 32.0f, "%.1f" );
            ImGui::SetItemTooltip( "Size of the blur disc in pixels." );
            ImGui::DragFloat( "Max Blur", &settings.DoFMaxBlur, 0.5f, 1.0f, 32.0f, "%.1f" );
            ImGui::SetItemTooltip( "Maximum blur radius." );
            ImGui::Checkbox( "Simple Gaussian Blur", &settings.DoFGaussBlur );
            ImGui::SetItemTooltip( "Use a fast Gaussian blur instead of the full bokeh kernel (cheaper, ~3x fewer taps)." );
            ImGui::PopID();
        }

        ImGui::SeparatorText( "SkySettings" );
        auto& atmosphereSettings = gapi->GetSky()->GetAtmoshpereSettings();
        ImGui::DragFloat( "G", &atmosphereSettings.G, 0.01f );
        ImGui::SetItemTooltip( "Size of the Sun" );

        ImGui::DragFloat( "RayleightScaleDepth", &atmosphereSettings.RayleightScaleDepth, 0.01f, 0.1f );
        ImGui::DragFloat( "ESun", &atmosphereSettings.ESun, 0.1f, 0.2f );
        ImGui::SetItemTooltip( "Brightness of the sun" );

        ImGui::DragFloat( "InnerRadius", &atmosphereSettings.InnerRadius, 1.0f, 0.0f, 0.0f, "%.0f" );
        ImGui::SetItemTooltip( "Inner Radius of the fake-planet. This must be greater than SphereOffset.y" );

        ImGui::DragFloat( "OuterRadius", &atmosphereSettings.OuterRadius, 1.0f, 0.0f, 0.0f, "%.0f" );
        ImGui::SetItemTooltip( "Outer Radius of the fake-planet" );

        ImGui::DragFloat( "Km", &atmosphereSettings.Km, 0.0001f, 0.01f );
        ImGui::DragFloat( "Kr", &atmosphereSettings.Kr, 0.0001f, 0.01f );
        ImGui::InputInt( "Samples", &atmosphereSettings.Samples );
        ImGui::DragFloat3( "WaveLengths", &atmosphereSettings.WaveLengths.x, 0.01f );
        ImGui::DragFloat( "SphereOffset.y", &atmosphereSettings.SphereOffsetY, 1.0f, 0.0f, 0.0f, "%.0f" );
        ImGui::Checkbox( "ReplaceSunDirection", &settings.ReplaceSunDirection );
        ImGui::SetItemTooltip( "Outer Radius of the fake-planet" );


        ImGui::BeginDisabled( !settings.ReplaceSunDirection );

        ImGuizmoDirectionEdit( "LightDirection", atmosphereSettings.LightDirection );
        ImGui::SetItemTooltip( "The direction the sun should come from. Only active when ReplaceSunDirection is active.\nAlso useful to fix the sun in one position" );

        ImGui::EndDisabled();

        ImGui::ColorEdit3( "SunLightColor", &settings.SunLightColor.x );
        ImGui::SetItemTooltip( "Color of the sunlight" );

        ImGui::DragFloat( "SunLightStrength", &settings.SunLightStrength, 0.01f );
        ImGui::DragFloat( "SkyTimeScale", &atmosphereSettings.SkyTimeScale, 0.01f );
        ImGui::SetItemTooltip( "This makes the skys time pass slower or faster" );

    }
    ImGui::End();
}


void ImGuiShim::RenderAdvancedColumn2( GothicRendererSettings& settings, GothicAPI* gapi ) {
    if ( ImGui::Begin( "General", nullptr, ImGuiWindowFlags_NoCollapse ) ) {

#ifdef IS_DEV_BUILD
        ImGui::Text( "Version: %s", VERSION_NUMBER " - (" BUILD_DATE ")" );
#else
        ImGui::Text( "Version: %s", VERSION_NUMBER );
#endif
        
        ImGui::Checkbox( "Enable DebugLog", &settings.EnableDebugLog );
        ImGui::Checkbox( "Toggle frame stats", &m_FrameStatisticsVisible );
        if ( ImGui::Button( "Save ZEN-Resources", ImVec2( ImGui::GetContentRegionAvail().x, 30.f ) ) ) {
            gapi->SaveCustomZENResources();
        }
        if ( ImGui::Button( "Load ZEN-Resources", ImVec2( ImGui::GetContentRegionAvail().x, 30.f ) ) ) {
            gapi->LoadCustomZENResources();
        }
        auto worldSettingsPath = Engine::GAPI->GetLoadedWorldSettingsPath(false);
        if (!worldSettingsPath.empty() && Toolbox::FileExists( worldSettingsPath ) ) {
            const bool shouldDelete = ImGui::Button( "Delete World-Settings", ImVec2( ImGui::GetContentRegionAvail().x, 30.f ) );
            ImGui::SetItemTooltip("Delete the world-settings file for the current world.\nThe current values will be saved into the global settings file.");
            if ( shouldDelete ) {
                std::error_code ec;
                std::filesystem::remove(worldSettingsPath, ec);
                Engine::GAPI->SaveRendererWorldSettings(settings, MENU_SETTINGS_FILE);
            }
        }
        if ( ImGui::Button( "Reset Settings", ImVec2( ImGui::GetContentRegionAvail().x, 30.f ) ) ) {
            settings.SetDefault();
            Engine::GraphicsEngine->ReloadShaders( ShaderCategory::All );
        }
        ImGui::SetItemTooltip( "Reset all settings to their default values." );
        if ( ImGui::Button( "Reload all Shaders", ImVec2( ImGui::GetContentRegionAvail().x, 30.f ) ) ) {
            Engine::GraphicsEngine->ReloadShaders( ShaderCategory::All );
        }

        ImGui::Separator();
        ImGui::Checkbox( "DisableRendering", &settings.DisableRendering );
        ImGui::SliderInt( "SectionDrawRadius", &settings.SectionDrawRadius, 0, 20, "%d", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
        ImGui::SliderInt( "Draw WorldMesh", &settings.DrawWorldMesh, 0, 3 );

        ImGui::Checkbox( "Draw VOBs", &settings.DrawVOBs );
        ImGui::Checkbox( "Draw Dynamic Vobs", &settings.DrawDynamicVOBs );
        ImGui::SliderFloat( "OutdoorVobDrawRadius", &settings.OutdoorVobDrawRadius, 1.0f, 100000.0f, "%.0f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
        ImGui::SliderFloat( "IndoorVobDrawRadius", &settings.IndoorVobDrawRadius, 1.0f, 100000.0f, "%.0f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
        ImGui::SliderFloat( "OutdoorSmallVobRadius", &settings.OutdoorSmallVobDrawRadius, 1.0f, 100000.0f, "%.0f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );

        ImGui::Checkbox( "Draw Skeletal Meshes", &settings.DrawSkeletalMeshes );
        ImGui::BeginDisabled( !settings.DrawSkeletalMeshes );
        ImGui::SliderFloat( "SkeletalMeshDrawRadius", &settings.SkeletalMeshDrawRadius, 0.0f, 18000.0f, "%.0f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
        ImGui::SetItemTooltip( "Draw distance for NPCs" );
        ImGui::EndDisabled();

        ImGui::Checkbox( "Draw Mobs", &settings.DrawMobs );

        ImGui::Checkbox( "Draw ParticleEffects", &settings.DrawParticleEffects );
        ImGui::BeginDisabled( !settings.DrawParticleEffects );
        ImGui::SliderFloat( "VisualFXDrawRadius", &settings.VisualFXDrawRadius, 0.0f, 50000.0f, "%.0f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
        ImGui::SetItemTooltip( "Draw distance for Special effects, like torches, spells, campfires..." );
        ImGui::EndDisabled();

        // ImGui::Checkbox( "Draw Sky", &settings.DrawSky );
        if ( ImGui::Checkbox( "Draw Fog", &settings.DrawFog ) ) {
            Engine::GraphicsEngine->ReloadShaders( ShaderCategory::Other );
        }
        ImGui::SetItemTooltip( "Changing this will reload shaders." );

        ImGui::BeginDisabled( !settings.DrawFog );
        {
            // caution, FogRange is reduced by 0.5f (secScale - 0.5f) in D3D11PFX_HeightFog
            ImGui::SliderFloat( "Fog Range", &settings.FogRange, 0.50f, 10.0f, "%.2f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
            ImGui::EndDisabled();
        }

        ImGui::Checkbox( "HDR", &settings.EnableHDR );

        static std::vector<std::pair<const char*, int>> hdrToneMapValues = {
            {"ToneMap_jafEq4", 0},
            {"Uncharted2Tonemap", 1},
            {"ACESFilmTonemap", 2},
            {"PerceptualQuantizerTonemap", 3},
            {"ToneMap_Simple", 4},
            {"ACESFittedTonemap", 5},
        };

        ImGui::BeginDisabled( !settings.EnableHDR );
        if ( ImComboBoxC( "HDR ToneMap", hdrToneMapValues, reinterpret_cast<int*>(&settings.HDRToneMap), []
        {
            Engine::GraphicsEngine->ReloadShaders( ShaderCategory::Tonemapping );
        } ) ) {
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        ImGui::Checkbox( "DynamicLighting", &settings.EnableDynamicLighting );
        ImGui::BeginDisabled( !settings.EnableDynamicLighting );
        {
            const static std::vector<std::tuple<const char*, GothicRendererSettings::EPointLightShadowMode, const char*>> dynamicShadowValues = {
                { "Off", GothicRendererSettings::EPointLightShadowMode::PLS_DISABLED, nullptr },
                { "Static", GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY, nullptr },
                { "Dynamic Update", GothicRendererSettings::EPointLightShadowMode::PLS_UPDATE_DYNAMIC, nullptr },
                { "Full", GothicRendererSettings::EPointLightShadowMode::PLS_FULL, "Very expensive. Don't use unless you encounter visual bugs." },
            };

            if ( ImComboBoxCT( "##DynamicShadows", dynamicShadowValues, &settings.EnablePointlightShadows, [] {} ) ) {
                ImGui::EndCombo();
            }

            ImGui::EndDisabled();
        }
        // ImGui::Checkbox("FastShadows", &settings.FastShadows );	
        ImGui::Checkbox( "DrawShadowGeometry", &settings.DrawShadowGeometry );
        if ( settings.RendererMode != GothicRendererSettings::RM_ForwardPlus) {
            ImGui::Checkbox( "DoZPrepass", &settings.DoZPrepass );
            ImGui::SetItemTooltip("Perform a lightweight Z Prepass.\nMIGHT improve performance on low bandwidth devices.");
        }
        ImGui::Checkbox( "VSync", &settings.EnableVSync );
        ImGui::Checkbox( "OcclusionCulling", &settings.EnableOcclusionCulling );
        ImGui::Checkbox( "Sort RenderQueue", &settings.SortRenderQueue );
        ImGui::Checkbox( "Draw Threaded", &settings.DrawThreaded );
        ImGui::Checkbox( "AtmosphericScattering", &settings.AtmosphericScattering );
        ImGui::Checkbox( "SkeletalVertexNormals", &settings.ShowSkeletalVertexNormals );

        static std::vector<std::pair<const char*, int>> shadowMapSizesMax = {
          {"512", 512},
          {"1024", 1024},
          {"2048", 2048},
          {"4096", 4096},
          {"8192", 8192},
          {"16384", 16384},
        };
        static std::vector<std::pair<const char*, int>> shadowMapSizesDxFeature10 = {
         {"512", 512},
         {"1024", 1024},
         {"2048", 2048},
         {"4096", 4096},
         {"8192", 8192},
        };
        std::vector<std::pair<const char*, int>>& shadowMapSizes = shadowMapSizesMax;
        if ( FeatureLevel10Compatibility ) {
            shadowMapSizes = shadowMapSizesDxFeature10;
        }

        ImGui::Checkbox( "Enable Shadows", &settings.EnableShadows );
        ImGui::BeginDisabled( !settings.EnableShadows );
        { 
            ImGui::Checkbox( "Fast Shadows", &settings.FastShadows );
            ImGui::SetItemTooltip( "Renders only static world meshes" );
            ImGui::Checkbox( "Fixed shadow update", &settings.SmoothShadowCameraUpdate );
            ImGui::SetItemTooltip( "on: Higher values mean more frequent shadow position updates.\noff: real-time shadow updates." );
            ImGui::DragFloat( "Fixed shadow frequency", &settings.SmoothShadowFrequency, 200.0f, 1, 20000.f, "%.0f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
            ImGui::SetItemTooltip( "on: Higher values mean more frequent shadow position updates.\noff: real-time shadow updates." );

            if ( ImComboBoxC( "ShadowmapSize", shadowMapSizes, (int*)(&settings.ShadowMapSize), []() { Engine::GraphicsEngine->ReloadShaders( ShaderCategory::LightsAndShadows ); } ) ) {
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip( "Changing this will reload shaders." );

            ImGui::DragFloat( "Shadow Distance", &settings.WorldShadowRangeScale, 0.01f, 0.00f, 10.0f, "%.2f" );
            ImGui::SetItemTooltip( "Larger values produce less detailed shadows\nEffective Distance: %.0f", 12000 * settings.WorldShadowRangeScale );

            constexpr int max_cascaded_supported = MAX_CSM_CASCADES;

            settings.NumShadowCascades = std::clamp( settings.NumShadowCascades, 1, max_cascaded_supported );
            if ( settings.DebugSettings.ShadowCascades.Lambda < 0.00001f ) {
                settings.DebugSettings.ShadowCascades.Lambda = D3D11ShadowMap::lambdaBiasTable[settings.NumShadowCascades].lambda;
                settings.DebugSettings.ShadowCascades.Bias = D3D11ShadowMap::lambdaBiasTable[settings.NumShadowCascades].bias;
            }
            if ( ImGui::SliderInt( "Shadow Cascade count", &settings.NumShadowCascades, 1, max_cascaded_supported, "%d", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput) ) {
                settings.NumShadowCascades = std::clamp( settings.NumShadowCascades, 1, max_cascaded_supported );
                settings.DebugSettings.ShadowCascades.Lambda = D3D11ShadowMap::lambdaBiasTable[settings.NumShadowCascades].lambda;
                settings.DebugSettings.ShadowCascades.Bias = D3D11ShadowMap::lambdaBiasTable[settings.NumShadowCascades].bias;
                ApplyFeatureLevel10Downgrades(settings);
                Engine::GraphicsEngine->ReloadShaders( ShaderCategory::LightsAndShadows );
            }
            ImGui::SetItemTooltip( "Changing this will reload shaders." );

            ImGui::BeginDisabled( settings.NumShadowCascades <= 1 );
            {
                static std::vector<std::pair<const char*, GothicRendererSettings::E_ShadowFrustumCulling>> shadowFrustumCullingModes = {
                    {"Disabled", GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_DISABLED},
                    {"Conservative", GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_CONSERVATIVE},
                    {"Aggressive", GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_AGGRESSIVE},
                };
                if ( ImComboBox( "Shadow Frustum Culling Mode", shadowFrustumCullingModes, &settings.ShadowFrustumCullingMode ) ) {
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip( "Improve performance by ignoring non-visible models" );
                ImGui::EndDisabled();
            }

            {
                static std::vector<std::pair<const char*, GothicRendererSettings::E_ShadowFilterMode>> shadowFilterModes = {
                    {"Disabled", GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_DISABLED},
                    {"Simple", GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_SIMPLE},
                    {"PCSS", GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_PCSS},
                };
                if ( ImComboBoxC( "Shadow filtering", shadowFilterModes, &settings.ShadowFilterMode, []() {
                    Engine::GraphicsEngine->ReloadShaders( ShaderCategory::LightsAndShadows );
                    } ) ) {
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip( "Changing this will reload shaders." );
            }
            settings.ShadowCascadePCFLimit = std::clamp( settings.ShadowCascadePCFLimit, 1, settings.NumShadowCascades );
            if ( ImGui::SliderInt( "Soft shadow limit", &settings.ShadowCascadePCFLimit, 1, settings.NumShadowCascades, "%d", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput ) ) {
                settings.ShadowCascadePCFLimit = std::clamp( settings.ShadowCascadePCFLimit, 1, settings.NumShadowCascades );
                Engine::GraphicsEngine->ReloadShaders( ShaderCategory::LightsAndShadows );
            }
            ImGui::SetItemTooltip( "Which shadow cascades should be filtered using '16xPCF'.\nChanging this will reload shaders." );
            
            ImGui::DragFloat( "ShadowStrength", &settings.ShadowStrength, 0.01f, 0.01f, 5.0f, "%.2f" );
            ImGui::DragFloat( "ShadowSoftness", &settings.ShadowSoftness, 0.05f, 0.2f, 8.0f, "%.2f" );
            ImGui::SetItemTooltip( "PCF kernel scale (1.0=sharp default, <1.0=sharper, >1.0=softer)" );
            ImGui::DragFloat( "PCSSLightSize", &settings.PCSSLightSize, 0.002f, 0.005f, 0.5f, "%.3f" );
            ImGui::SetItemTooltip( "PCSS light radius in shadow UV space. Higher values increase contact hardening and penumbra growth." );
            ImGui::DragFloat( "ShadowAOStrength", &settings.ShadowAOStrength, 0.01f, -5.0f, 2.0f, "%.2f" );
            ImGui::DragFloat( "WorldAOStrength", &settings.WorldAOStrength, 0.01f, -5.0f, 2.0f, "%.2f" );
            ImGui::EndDisabled();
        }
        ImGui::Separator();

        ImGui::Checkbox( "WireframeWorld", &settings.WireframeWorld );
        ImGui::Checkbox( "WireframeVobs", &settings.WireframeVobs );
        // ImGui::Checkbox("Grass AlphaToCoverage", &settings.VegetationAlphaToCoverage );	

        ImGui::SeparatorText("Rain Settings##AdvancedRainSettings");
        ImGui::DragFloat( "RainRadius", &settings.RainRadiusRange, 1.0f, 0.0f, 0.0f, "%.0f" );
        ImGui::DragFloat( "RainHeight", &settings.RainHeightRange, 1.0f, 0.0f, 0.0f, "%.0f" );
        ImGui::DragInt( "NumRainParticles", (int*)&settings.RainNumParticles, 1.0f, 0, 200000 );
        ImGui::Checkbox( "RainMoveParticles", &settings.RainMoveParticles );
        ImGui::Checkbox( "RainUseInitialSet", &settings.RainUseInitialSet );
        ImGui::DragFloat3( "RainGlobalVelocity", &settings.RainGlobalVelocity.x, 1.0f, -5000.0f, 5000.0f, "%.0f" );
        ImGui::DragFloat( "RainSceneWettness", &settings.RainSceneWettness, 0.01f );
        ImGui::DragFloat( "RainSunLightStrength", &settings.RainSunLightStrength, 0.01f, 0.0f, 0.0f, "%.2f" );
        ImGui::DragFloat( "RainFogDensity", &settings.RainFogDensity, 0.001f );
        ImGui::ColorEdit3( "RainFogColor", &settings.RainFogColor.x );
        ImGui::Separator();
        // TwAddVarRW("SmallVobSize", TW_TYPE_FLOAT, &settings.SmallVobSize );
        // ImGui::Checkbox("AtmosphericScattering", &settings.AtmosphericScattering );
        ImGui::SeparatorText("Fog Settings##AdvancedFogSettings");
        ImGui::DragFloat( "FogGlobalDensity", &settings.FogGlobalDensity, 0.00001f, 0, 1.0f, "%.5f" );
        ImGui::DragFloat( "FogHeightFalloff", &settings.FogHeightFalloff, 0.00001f, 0, 1.0f, "%.5f" );
        ImGui::DragFloat( "FogHeight", &settings.FogHeight, 1.0f, 0.0f, 0.0f, "%.0f" );
        ImGui::ColorEdit3( "FogColorMod", &settings.FogColorMod.x );
        ImGui::Separator();

        ImGui::DragFloat( "HDRLumWhite", &settings.HDRLumWhite, 0.01f, 0.0f, 0.0f, "%.2f" );
        ImGui::DragFloat( "HDRMiddleGray", &settings.HDRMiddleGray, 0.01f, 0.0f, 0.0f, "%.2f" );
        ImGui::DragFloat( "BloomThreshold", &settings.BloomThreshold, 0.01f, 0.0f, 0.0f, "%.2f" );
        ImGui::DragFloat( "BloomStrength", &settings.BloomStrength, 0.01f, 0.0f, 0.0f, "%.2f" );

#if defined(BUILD_GOTHIC_2_6_fix) || (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F))
#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
        if ( haveWindAnimations )
#endif
        {
            ImGui::DragFloat( "WindStrength", &settings.GlobalWindStrength, 0.01f, 0.0f, 0.0f, "%.2f" );
        }
#endif //BUILD_GOTHIC_2_6_fix

        ImGui::Checkbox( "FixViewFrustum", &settings.FixViewFrustum );
        ImGui::DragFloat( "GothicUIScale", &settings.GothicUIScale, 0.01f, 0.01f, 20.0f, "%.2f" );
        ImGui::DragFloat( "FOVHoriz", &settings.FOVHoriz, 1.0f, 1.0f, 360.0f, "%.0f" );
        ImGui::DragFloat( "FOVVert", &settings.FOVVert, 1.0f, 1.0f, 360.0f, "%.0f" );
        ImGui::Checkbox( "ForceFOV", &settings.ForceFOV );
#ifdef BUILD_GOTHIC_1_08k
        ImGui::Checkbox( "DrawForestPortals", &settings.DrawG1ForestPortals );
#endif

        ImGui::SeparatorText("Debugging");

        if (ImGui::Button("Reset##ResetDebugValues", ImVec2( 100.0f, 30.f ) )) {
            settings.ResetDebugSettings();
        }

        if (ImGui::BeginTabBar("#DebugTabs")) {
            if (ImGui::BeginTabItem("TAA Debug", nullptr, ImGuiTabItemFlags_::ImGuiTabItemFlags_NoReorder)) {
                ImGui::Checkbox("Use Depth based Velocity", &settings.DebugSettings.TAA.DepthMotionVectors);
                ImGui::SetItemTooltip("Instead of per-Object");
                ImGui::Checkbox("Display Velocity", &settings.DebugSettings.TAA.DisplayVelocity);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Shadows", nullptr, ImGuiTabItemFlags_::ImGuiTabItemFlags_NoReorder)) {
                ImGui::Checkbox("Lazy update", &settings.DebugSettings.ShadowCascades.LazyCascadeUpdate );
                ImGui::SetItemTooltip("Update last cascades less frequently to improve performance, may cause uneven frametimes");

                ImGui::Checkbox("Threaded Culling", &settings.ThreadedShadowCulling );
                ImGui::SetItemTooltip("Perform shadow frustum culling in a separate thread to improve performance");

                ImGui::SliderFloat("Extend Back", &settings.DebugSettings.ShadowCascades.ExtendBack, -10000, 50000, "%.0f");
                ImGui::SliderFloat("Extend Front", &settings.DebugSettings.ShadowCascades.ExtendFront, -10000, 50000, "%.0f");
                ImGui::SliderFloat("Extend Side", &settings.DebugSettings.ShadowCascades.ExtendSide, -10000, 20000, "%.0f");
                ImGui::SliderFloat("Split Lambda", &settings.DebugSettings.ShadowCascades.Lambda, 0.0f, 1.00f, "%.2f");
                ImGui::SliderFloat("Split Bias", &settings.DebugSettings.ShadowCascades.Bias, 0.0f, 10.0f, "%.1f");
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("Culling", nullptr, ImGuiTabItemFlags_::ImGuiTabItemFlags_NoReorder)) {
                ImGui::Checkbox("BSP Nodes", &settings.DebugSettings.Culling.CullBspSections );
                ImGui::Checkbox("Vobs", &settings.DebugSettings.Culling.CullVobs );
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("Featureset", nullptr, ImGuiTabItemFlags_::ImGuiTabItemFlags_NoReorder)) {
                ImGui::Checkbox("Enable GPU Driver Extensions", &settings.DebugSettings.FeatureSet.EnableDriverExtensions );
                ImGui::SetItemTooltip("Allow Driver Extensions (AMD, Nvidia, Intel).\nRequires restart.");

                {
                    static const std::vector<std::pair<const char*, GothicRendererSettings::E_RendererMode>> rendererModes = {
                        { "Deferred",   GothicRendererSettings::RM_Deferred },
                        { "Forward+",   GothicRendererSettings::RM_ForwardPlus },
                    };
                    if ( ImComboBox( "Renderer Mode", rendererModes, &settings.RendererMode ) ) {
                        ImGui::EndCombo();
                    }
                    ImGui::SetItemTooltip( "Deferred: GBuffer + tiled deferred lighting.  Forward+: depth prepass + per-pixel lit geometry pass." );
                }
                if (!FeatureLevel10Compatibility){
                    ImGui::Checkbox("Use MDI", &settings.DebugSettings.FeatureSet.UseMDI );
                    ImGui::SetItemTooltip("Support for MultiDrawInstancedIndirect via Driver Extensions (AMD, Nvidia, Intel).");

                    ImGui::Checkbox("Use Layered Drawing", &settings.DebugSettings.FeatureSet.UseLayeredRendering );
                    ImGui::Checkbox("Use Tiled Lighting", &settings.EnableTiledLighting );
                    ImGui::SetItemTooltip( "Uses compute shader light culling for point lights. Reduces draw calls and overdraw." );
                }
                if ( ImGui::Checkbox( "Use Shadow Atlas", &settings.DebugSettings.FeatureSet.UseShadowAtlas ) ) {
                    ApplyFeatureLevel10Downgrades( settings );
                }
                ImGui::SetItemTooltip("Enables a less intensive but lower quality shadow solution.");
                if ( ImGui::Checkbox( "Use Screen-Space Shadow Mask", &settings.DebugSettings.FeatureSet.UseScreenSpaceShadowMask ) ) {
                    Engine::GraphicsEngine->ReloadShaders( ShaderCategory::LightsAndShadows );
                }
                ImGui::SetItemTooltip( "Forward+ debug option: precompute sun shadows in a separate screen-space pass. Changing this reloads light/shadow shaders." );

                ImGui::Checkbox("Use World Section BVH", &settings.DebugSettings.FeatureSet.UseWorldSectionBVH );
                ImGui::SetItemTooltip("Use Bounding Volume Hierarchy for world sections. Improves culling performance.");

                ImGui::Checkbox("Force Feature Level 10", &settings.DebugSettings.FeatureSet.ForceFeatureLevel10 );
                ImGui::SetItemTooltip("Force DirectX 10 era feature support. Requires restart.");
                ImGui::EndTabItem();
            }
 
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void RenderAdvancedColumn3( GothicRendererSettings& settings, GothicAPI* gapi ) {
    if ( ImGui::Begin( "FrameStats", nullptr, ImGuiWindowFlags_NoCollapse ) ) {
        ImGui::PushID( "FrameStatsValues" );

        auto& rendererInfo = gapi->GetRendererState().RendererInfo;

        if ( ImGui::BeginTable( "##FrameStats", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp ) ) {
            ImGui::TableSetupColumn( "Label", ImGuiTableColumnFlags_WidthStretch, 0.45f );
            ImGui::TableSetupColumn( "Value", ImGuiTableColumnFlags_WidthStretch, 0.55f );

            static auto addRowLabel = []( const char* label ) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex( 0 );
                ImGui::TextUnformatted( label );
                ImGui::TableSetColumnIndex( 1 );
                };

            static auto addRowInt = []( const char* label, int value ) {
                addRowLabel( label );
                ImGui::Text( "%d", value );
                };

            static auto addRowUInt = []( const char* label, unsigned int value ) {
                addRowLabel( label );
                ImGui::Text( "%u", value );
                };

            static auto addRowFloat = []( const char* label, float value, const char* fmt ) {
                addRowLabel( label );
                ImGui::Text( fmt, value );
                };

            addRowInt( "FPS", rendererInfo.FPS );
            addRowUInt( "StateChanges", rendererInfo.StateChanges );
            addRowInt( "DrawnVobs", rendererInfo.FrameDrawnVobs );
            addRowInt( "DrawnTriangles", rendererInfo.FrameDrawnTriangles );
            addRowInt( "VobUpdates", rendererInfo.FrameVobUpdates );
            addRowInt( "DrawnLights", rendererInfo.FrameDrawnLights );
            addRowInt( "SectionsDrawn", rendererInfo.FrameNumSectionsDrawn );
            addRowInt( "WorldMeshDrawCalls", rendererInfo.WorldMeshDrawCalls );
            addRowFloat( "FarPlane", rendererInfo.FarPlane, "%.0f" );
            addRowFloat( "NearPlane", rendererInfo.NearPlane, "%.0f" );

            addRowInt( "SC_PipelineStates", rendererInfo.FramePipelineStates );
            addRowInt( "SC_Textures", rendererInfo.StateChangesByState[GothicRendererInfo::SC_TX] );
            addRowInt( "SC_ConstantBuffer", rendererInfo.StateChangesByState[GothicRendererInfo::SC_CB] );
            addRowInt( "SC_GeometryShader", rendererInfo.StateChangesByState[GothicRendererInfo::SC_GS] );
            addRowInt( "SC_RTVDSV", rendererInfo.StateChangesByState[GothicRendererInfo::SC_RTVDSV] );
            addRowInt( "SC_DomainShader", rendererInfo.StateChangesByState[GothicRendererInfo::SC_DS] );
            addRowInt( "SC_HullShader", rendererInfo.StateChangesByState[GothicRendererInfo::SC_HS] );
            addRowInt( "SC_PixelShader", rendererInfo.StateChangesByState[GothicRendererInfo::SC_PS] );
            addRowInt( "SC_InputLayout", rendererInfo.StateChangesByState[GothicRendererInfo::SC_IL] );
            addRowInt( "SC_VertexShader", rendererInfo.StateChangesByState[GothicRendererInfo::SC_VS] );
            addRowInt( "SC_IndexBuffer", rendererInfo.StateChangesByState[GothicRendererInfo::SC_IB] );
            addRowInt( "SC_VertexBuffer", rendererInfo.StateChangesByState[GothicRendererInfo::SC_VB] );
            addRowInt( "SC_RasterizerState", rendererInfo.StateChangesByState[GothicRendererInfo::SC_RS] );
            addRowInt( "SC_DepthStencilState", rendererInfo.StateChangesByState[GothicRendererInfo::SC_DSS] );
            addRowInt( "SC_SamplerState", rendererInfo.StateChangesByState[GothicRendererInfo::SC_SMPL] );
            addRowInt( "SC_BlendState", rendererInfo.StateChangesByState[GothicRendererInfo::SC_BS] );

            ImGui::EndTable();
        }

        ImGui::PopID(); // FrameStatsValues
    }
    ImGui::End();
}

void RenderAdvancedColumn4( GothicRendererSettings& settings, GothicAPI* gapi ) {
    if ( ImGui::Begin( "Post Processing Effects", nullptr, ImGuiWindowFlags_NoCollapse ) ) {
            ImGui::SeparatorText( "Ambient Occlusion" );
            {
                ImGui::PushID( "AOSettings" );
                static std::vector<std::tuple<const char*, AOMode, const char*>> aoModes = {
                    {"Disabled", AOMode::AO_NONE, nullptr},
                    {"HBAO+", AOMode::AO_HBAO, "NVIDIA HBAO+ (Horizon-Based Ambient Occlusion Plus)"},
                    {"SAO", AOMode::AO_SAO, nullptr},
                    {"ASSAO", AOMode::AO_ASSAO, "Intel ASSAO (Adaptive Screen Space Ambient Occlusion)"},
                };
                if ( ImComboBoxCT( "AO Mode", aoModes, &settings.AoMode, [] {
                        Engine::GraphicsEngine->ReloadShaders( ShaderCategory::Other );
                    } ) ) {
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip( "Changing this will reload shaders." );

                if ( settings.AoMode == AOMode::AO_HBAO ) {
                    ImGui::SeparatorText( "HBAO+ Settings" );
                    ImGui::DragFloat( "Radius", &settings.HbaoSettings.Radius, 0.01f );
                    ImGui::DragFloat( "MetersToViewSpaceUnits", &settings.HbaoSettings.MetersToViewSpaceUnits, 0.01f );
                    if ( ImGui::DragFloat( "PowerExponent", &settings.HbaoSettings.PowerExponent, 0.01f ) ) {
                        settings.HbaoSettings.PowerExponent = std::clamp( settings.HbaoSettings.PowerExponent, 1.0f, 4.0f );
                    }
                    if ( ImGui::DragFloat( "Bias", &settings.HbaoSettings.Bias, 0.01f ) ) {
                        settings.HbaoSettings.Bias = std::clamp( settings.HbaoSettings.Bias, 0.0f, 0.5f );
                    }

                    ImGui::Checkbox( "Enable Blur", &settings.HbaoSettings.EnableBlur );
                    static std::vector<std::pair<const char*, int>> ssaoRadi = { {"2", 0}, {"4", 1} };
                    if ( ImComboBox( "SSAO radius", ssaoRadi, &settings.HbaoSettings.SsaoBlurRadius ) ) {
                        ImGui::EndCombo();
                    }
                    ImGui::DragFloat( "BlurSharpness", &settings.HbaoSettings.BlurSharpness, 0.01f );
                    static std::vector<std::pair<const char*, int>> blendMode = { {"Replace", 0}, {"Multiply", 1} };
                    if ( ImComboBox( "BlendMode", blendMode, &settings.HbaoSettings.BlendMode ) ) {
                        ImGui::EndCombo();
                    }

                    static std::vector<std::pair<const char*, int>> stepCount = { {"4", 0}, {"8", 1} };
                    if ( ImComboBox( "SSAO steps", stepCount, &settings.HbaoSettings.SsaoStepCount ) ) {
                        ImGui::EndCombo();
                    }
                } else if ( settings.AoMode == AOMode::AO_SAO ) {
                    ImGui::SeparatorText( "SAO Settings" );
                    ImGui::DragFloat( "Radius", &settings.SaoSettings.Radius, 0.01f, 0.1f, 10.0f );
                    ImGui::DragFloat( "Bias", &settings.SaoSettings.Bias, 0.001f, 0.0f, 0.1f );
                    ImGui::DragFloat( "Intensity", &settings.SaoSettings.Intensity, 0.01f, 0.0f, 10.0f );
                    ImGui::SliderInt( "Samples", &settings.SaoSettings.NumSamples, 4, 64 );
                    ImGui::DragFloat( "Blur Sharpness", &settings.SaoSettings.BlurSharpness, 0.01f, 0.0f, 16.0f );
                } else if ( settings.AoMode == AOMode::AO_ASSAO ) {
                    ImGui::SeparatorText( "ASSAO Settings" );

                    ImGui::TextUnformatted( "Preset" ); ImGui::SameLine();
                    if ( ImGui::Button( "Low" ) ) {
                        settings.ApplyAssaoPreset(0);
                    }
                    ImGui::SameLine();
                    if ( ImGui::Button( "High" ) ) {
                        settings.ApplyAssaoPreset( 1 );
                    }
                    ImGui::SameLine();

                    if ( ImGui::Button( "Dark" ) ) {
                        settings.ApplyAssaoPreset( 2 );
                    }
                    ImGui::SetItemTooltip( "Mimics HBAO+" );
                    ImGui::SameLine();

                    if ( ImGui::Button( "Soft" ) ) {
                        settings.ApplyAssaoPreset( 3 );
                    }
                    ImGui::SetItemTooltip("Mimics GTAO");

                    ImGui::DragFloat( "Radius", &settings.AssaoSettings.Radius, 0.01f, 0.0f, 0.0f, "%.2f" );
                    ImGui::SetItemTooltip( "[0.0, ~] World (view) space size of the occlusion sphere." );
                    ImGui::DragFloat( "Shadow Multiplier", &settings.AssaoSettings.ShadowMultiplier, 0.01f, 0.0f, 5.0f, "%.2f", ImGuiSliderFlags_ClampOnInput );
                    ImGui::SetItemTooltip( "[0.0, 5.0] Effect strength linear multiplier." );
                    ImGui::DragFloat( "Shadow Power", &settings.AssaoSettings.ShadowPower, 0.01f, 0.5f, 5.0f, "%.2f", ImGuiSliderFlags_ClampOnInput );
                    ImGui::SetItemTooltip( "[0.5, 5.0] Effect strength pow modifier." );
                    ImGui::DragFloat( "Shadow Clamp", &settings.AssaoSettings.ShadowClamp, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_ClampOnInput );
                    ImGui::SetItemTooltip( "[0.0, 1.0] Effect max limit." );
                    ImGui::DragFloat( "Horizon Angle Threshold", &settings.AssaoSettings.HorizonAngleThreshold, 0.001f, 0.0f, 0.2f, "%.3f", ImGuiSliderFlags_ClampOnInput );
                    ImGui::SetItemTooltip( "[0.0, 0.2] Limits self-shadowing." );
                    ImGui::DragFloat( "Fade Out From", &settings.AssaoSettings.FadeOutFrom, 1.0f, 0.0f, 0.0f, "%.0f" );
                    ImGui::SetItemTooltip( "[0.0, ~] Distance to start fading out the effect." );
                    ImGui::DragFloat( "Fade Out To", &settings.AssaoSettings.FadeOutTo, 1.0f, 0.0f, 0.0f, "%.0f" );
                    ImGui::SetItemTooltip( "[0.0, ~] Distance at which the effect is fully faded out." );
                    static std::vector<std::pair<const char*, int>> assaoQuality = {
                        {"Lowest (-1)", -1}, {"Low (0)", 0}, {"Medium (1)", 1}, {"High (2)", 2}, {"Very High/Adaptive (3)", 3}
                    };
                    if ( ImComboBox( "Quality Level", assaoQuality, &settings.AssaoSettings.QualityLevel ) ) {
                        ImGui::EndCombo();
                    }
                    ImGui::SetItemTooltip( "[-1, 3] Effect quality. Each level is ~2x more costly than the previous." );
                    ImGui::BeginDisabled( settings.AssaoSettings.QualityLevel != 3 );
                    ImGui::DragFloat( "Adaptive Quality Limit", &settings.AssaoSettings.AdaptiveQualityLimit, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_ClampOnInput );
                    ImGui::SetItemTooltip( "[0.0, 1.0] Only for Quality Level 3." );
                    ImGui::EndDisabled();
                    ImGui::SliderInt( "Blur Pass Count", &settings.AssaoSettings.BlurPassCount, 0, 6 );
                    ImGui::SetItemTooltip( "[0, 6] Number of edge-sensitive smart blur passes." );
                    ImGui::DragFloat( "Sharpness", &settings.AssaoSettings.Sharpness, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_ClampOnInput );
                    ImGui::SetItemTooltip( "[0.0, 1.0] How much to bleed over edges." );
                    ImGui::DragFloat( "Detail Shadow Strength", &settings.AssaoSettings.DetailShadowStrength, 0.01f, 0.0f, 5.0f, "%.2f", ImGuiSliderFlags_ClampOnInput );
                    ImGui::SetItemTooltip( "[0.0, 5.0] High-res detail AO; adds detail but reduces temporal stability." );
                }
                ImGui::PopID();
            }

        ImGui::SeparatorText( "Anti Aliasing" );
        {
            ImGui::PushID( "AntiAliasingSettings" );
            static std::vector<std::pair<const char*, GothicRendererSettings::E_AntiAliasingMode>> antiAliasing = {
                {"Disabled", GothicRendererSettings::E_AntiAliasingMode::AA_NONE},
                {"SMAA", GothicRendererSettings::E_AntiAliasingMode::AA_SMAA},
                {"TAA", GothicRendererSettings::E_AntiAliasingMode::AA_TAA},
                {"FSR 2", GothicRendererSettings::E_AntiAliasingMode::AA_FSR},
                {"FSR 3", GothicRendererSettings::E_AntiAliasingMode::AA_FSR3},
            };
            auto selectedMode = settings.AntiAliasingMode;
            if ( selectedMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR && settings.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3 ) {
                selectedMode = GothicRendererSettings::E_AntiAliasingMode::AA_FSR3;
            }
            if ( ImComboBoxC( "Anti Aliasing", antiAliasing, &selectedMode, [&selectedMode, &settings] {
                if ( selectedMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR3 ) {
                    selectedMode = GothicRendererSettings::E_AntiAliasingMode::AA_FSR;
                    settings.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3;
                } else if ( selectedMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR ) {
                    settings.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_2;
                }
                settings.AntiAliasingMode = selectedMode;
                } ) ) {
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }

        ImGui::SeparatorText( "Sharpening" );
        {
            ImGui::PushID( "SharpeningSettings" );
            static std::vector<std::pair<const char*, GothicRendererSettings::E_SharpeningMode>> sharpenModes = {
                {"Disabled", GothicRendererSettings::E_SharpeningMode::SHARPEN_NONE},
                {"Simple", GothicRendererSettings::E_SharpeningMode::SHARPEN_SIMPLE},
                {"CAS", GothicRendererSettings::E_SharpeningMode::SHARPEN_CAS},
            };
            if ( ImComboBox( "Mode", sharpenModes, &settings.SharpeningMode ) ) {
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled( !settings.SharpeningMode );
            {
                ImGui::DragFloat( "Factor", &settings.SharpenFactor, 0.001f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_::ImGuiSliderFlags_AlwaysClamp );
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }

    }
    ImGui::End();
}

void ImGuiShim::RenderAdvancedSettingsWindow()
{
    // Autosized settings by child objects & centered
    IM_ASSERT( ImGui::GetCurrentContext() != NULL && "Missing Dear ImGui context!" );
    IMGUI_CHECKVERSION();

    auto windowSize = CurrentResolution;
    
    int numCols = m_FrameStatisticsVisible ? 4 : 3;
    auto columnWidth = static_cast<float>(windowSize.x) / numCols;
    auto columnOffset = 0.0f;
    auto columnHeight = std::max( 400.0f, static_cast<float>(windowSize.y) / 2.f );

    GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;
    FixupSettings(settings);

    static bool lastStatisticsVisible = m_FrameStatisticsVisible;
    bool forceReappear = false;
    if ( m_FrameStatisticsVisible != lastStatisticsVisible ) {
        lastStatisticsVisible = m_FrameStatisticsVisible;
        forceReappear = true;
    }
    int ImGuiCond_Appearing_Or_ForceReappear = forceReappear ? ImGuiCond_Always : ImGuiCond_Appearing;
    
    ImGui::SetNextWindowPos( ImVec2( columnOffset, 0.0f ), ImGuiCond_Appearing_Or_ForceReappear, ImVec2( 0, 0 ) );
    ImGui::SetNextWindowSize( ImVec2( columnWidth, columnHeight ), ImGuiCond_Appearing_Or_ForceReappear );
    RenderAdvancedColumn1( settings, Engine::GAPI );
    columnOffset += columnWidth;

    ImGui::SetNextWindowPos( ImVec2( columnOffset, 0.0f ), ImGuiCond_Appearing_Or_ForceReappear, ImVec2( 0, 0 ) );
    ImGui::SetNextWindowSize( ImVec2( columnWidth, columnHeight ), ImGuiCond_Appearing_Or_ForceReappear );
    RenderAdvancedColumn2( settings, Engine::GAPI );
    columnOffset += columnWidth;

    if (m_FrameStatisticsVisible)
    {
        ImGui::SetNextWindowPos( ImVec2( columnOffset, 0.0f ), ImGuiCond_Appearing_Or_ForceReappear, ImVec2( 0, 0 ) );
        ImGui::SetNextWindowSize( ImVec2( columnWidth, columnHeight ), ImGuiCond_Appearing_Or_ForceReappear );
        RenderAdvancedColumn3( settings, Engine::GAPI );
        columnOffset += columnWidth;
    }

    ImGui::SetNextWindowPos( ImVec2( columnOffset, 0.0f ), ImGuiCond_Appearing_Or_ForceReappear, ImVec2( 0, 0 ) );
    ImGui::SetNextWindowSize( ImVec2( columnWidth, columnHeight ), ImGuiCond_Appearing_Or_ForceReappear );
    RenderAdvancedColumn4( settings, Engine::GAPI );

}
