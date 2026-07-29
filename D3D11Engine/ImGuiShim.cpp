#include "ImGuiShim.h"
#include "GSky.h"
#include "D3D12Engine/D3D12GraphicsEngine.h"
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
#include "zFILE_VDFS.h"

#define STB_IMAGE_IMPLEMENTATION
#include "D3D12Engine/D3D12Texture.h"
#include "vendor/stb/stb_image.h"

namespace ImGui {
    void TextUnformatted( const wchar_t* text ) {
        char dest[64];
        auto len = WideCharToMultiByte(CP_UTF8, 0, text, -1, dest, sizeof(dest), NULL, NULL);
        dest[std::min(static_cast<size_t>(len), sizeof(dest) - 1)] = '\0';
        ImGui::TextUnformatted( dest );
    }
}

#if defined(BUILD_GOTHIC_1_CLASSIC)
extern bool haveWindAnimations;
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
extern float* ShadowMapLambda;
extern float* ShadowMapBias;

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

namespace {
    // SRV-descriptor callbacks for imgui_impl_dx12: it needs to allocate/free shader-visible
    // descriptors for its textures (font atlas + any user textures). We route these through the
    // D3D12GraphicsEngine's shader-visible heap (passed via ImGui_ImplDX12_InitInfo::UserData).
    void ImGuiDX12_SrvAlloc( ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu ) {
        auto* engine = static_cast<D3D12GraphicsEngine*>( info->UserData );
        const UINT slot = engine->AllocateSrvSlot();
        *outCpu = engine->GetSrvCpuHandle( slot );
        *outGpu = engine->GetSrvGpuHandle( slot );
    }
    // Bump allocator has no free-list yet: freeing is a no-op. ImGui frees are rare (font-atlas
    // recreation on DPI/font change), so leaked slots are bounded — acceptable for now.
    void ImGuiDX12_SrvFree( ImGui_ImplDX12_InitInfo* /*info*/, D3D12_CPU_DESCRIPTOR_HANDLE /*cpu*/, D3D12_GPU_DESCRIPTOR_HANDLE /*gpu*/ ) {
    }

    void ImGuiCollectResolutions( std::vector<std::pair<INT2, std::string>>& resolutions ) {

        std::vector<DisplayModeInfo> modes;
        Engine::GraphicsEngine->GetDisplayModeList( &modes );
        resolutions.clear();

        // Don't offer a resolution bigger than the monitor's current desktop resolution. DXGI's mode list
        // enumerates every mode the OUTPUT hardware supports, which can include entries above the current
        // desktop resolution (e.g. the desktop is running scaled/non-native); picking one of those isn't
        // useful here — the window/swapchain would end up larger than the visible desktop.
        const int maxWidth = GetSystemMetrics( SM_CXSCREEN );
        const int maxHeight = GetSystemMetrics( SM_CYSCREEN );

        for ( auto it = modes.rbegin(); it != modes.rend(); ++it ) {
            if ( maxWidth > 0 && maxHeight > 0 && ( (*it).Width > maxWidth || (*it).Height > maxHeight ) )
                continue;

            std::string s = std::to_string( (*it).Width ) + "x" + std::to_string( (*it).Height )
                // disable Hz display until we implement exclusive fullscreen mode. If we ever do.
                // + " (" + std::to_string( (*it).refreshRateNumerator / std::max<unsigned int>( 1, (*it).refreshRateDenominator ) ) + " Hz)"
                ;
            resolutions.emplace_back( std::make_pair( INT2( (*it).Width, (*it).Height ), s ) );
        }
    }

    // Shared post-backend-init setup (DPI-scaled font, resolution list, editor view). The DX11 and
    // DX12 init paths differ only in which ImGui_ImplXXXX_Init they call; everything else is common.
    void FinishImGuiInit( HWND window, std::vector<std::pair<INT2, std::string>>& resolutions, std::unique_ptr<ImGuiEditorView>& editorView ) {
        const auto actualDPI = GetDpi( window );

        ImGuiCollectResolutions( resolutions );

        ImFontConfig config = {};

        config.MergeMode = true;
        //config.GlyphRanges = euroGlyphRanges;
        const auto path = std::filesystem::current_path();
        std::filesystem::path fonts[] = {
            // path / "system" / "GD3D11" / "Fonts" / "GII_-_Die_Nacht_des_Raben.ttf",
            path / "system" / "GD3D11" / "Fonts" / "Lato-Semibold.ttf",
        };

        bool firstFont = true;
        for ( auto fontpath : fonts ) {
            if ( std::filesystem::exists( fontpath ) ) {
                config.MergeMode = !firstFont;
                const auto font = ImGui::GetIO().Fonts->AddFontFromFileTTF( fontpath.string().c_str(), 20.0f, &config );
                if ( font && firstFont ) {
                    firstFont = false;
                }
            }
        }

        auto dpiScale = actualDPI / 96.0f;
    
        auto& style = ImGui::GetStyle();
        style.FontScaleDpi = dpiScale;

        style.Alpha = 1.0f;
        style.Colors[ImGuiCol_Text] = ImVec4( 1.f, 0.87f, 0.68f, 1.f );
        style.Colors[ImGuiCol_TextDisabled] = ImVec4( 1.f, 0.87f, 0.68f, 0.28f );
        style.Colors[ImGuiCol_Border] = ImVec4( 0.84f, 0.54f, 0.15f, 1.0f );
        style.Colors[ImGuiCol_BorderShadow] = ImVec4( 0.00f, 0.00f, 0.00f, 0.00f );
        
        editorView = std::make_unique<ImGuiEditorView>();
    }
}

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
    m_Backend = Backend::D3D11;

    Initiated = true;
    FinishImGuiInit( Window, Resolutions, m_EditorView );
}

void ImGuiShim::InitD3D12(
    HWND Window,
    D3D12GraphicsEngine* engine,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    int numFramesInFlight,
    DXGI_FORMAT rtvFormat,
    ID3D12DescriptorHeap* srvHeap
)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    OutputWindow = Window;
    ImGui_ImplWin32_Init( OutputWindow );

    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = device;
    initInfo.CommandQueue = queue;
    initInfo.NumFramesInFlight = numFramesInFlight;
    initInfo.RTVFormat = rtvFormat;
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.UserData = engine;
    initInfo.SrvDescriptorHeap = srvHeap;
    initInfo.SrvDescriptorAllocFn = &ImGuiDX12_SrvAlloc;
    initInfo.SrvDescriptorFreeFn = &ImGuiDX12_SrvFree;
    ImGui_ImplDX12_Init( &initInfo );
    m_Backend = Backend::D3D12;

    Initiated = true;
    FinishImGuiInit( Window, Resolutions, m_EditorView );
}


ImGuiShim::~ImGuiShim()
{
    if ( Initiated ) {
        if ( m_Backend == Backend::D3D12 ) {
            ImGui_ImplDX12_Shutdown();
        } else {
            ImGui_ImplDX11_Shutdown();
        }
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

void ImGuiShim::BuildFrameUI()
{
    const auto bbres = Engine::GraphicsEngine->GetResolution();
    ImGui::GetIO().MouseDrawCursor = GetIsActive() && INT2( ImGui::GetMainViewport()->Size.x, ImGui::GetMainViewport()->Size.y ) != bbres;
    ImGui::GetIO().FontGlobalScale = bbres.y < 1080 ? static_cast<float>( bbres.y ) / 1080.0f : 1.0f;

    static zSTRING GDX_IMGUI_BEGINFRAME = "GDX_IMGUI_BEGINFRAME";
    static zSTRING GDX_IMGUI_ENDFRAME = "GDX_IMGUI_ENDFRAME";
    if ( !m_scriptFnsResolved ) {
        m_beginFrameFn = zCParser::GetParser()->GetIndex( GDX_IMGUI_BEGINFRAME );
        m_endFrameFn = zCParser::GetParser()->GetIndex( GDX_IMGUI_ENDFRAME );
        m_scriptFnsResolved = true;
    }
    if ( m_retryFindFuncs > 120 ) {
        if ( m_beginFrameFn == -1 ) { m_beginFrameFn = zCParser::GetParser()->GetIndex( GDX_IMGUI_BEGINFRAME ); }
        if ( m_endFrameFn == -1 ) { m_endFrameFn = zCParser::GetParser()->GetIndex( GDX_IMGUI_ENDFRAME ); }
        m_retryFindFuncs = 0;
    }

    LibShowBlockingThisFrame = false;
    LibShowNonBlockingThisFrame = false;
    if ( m_beginFrameFn != -1 ) {
        zCParser::GetParser()->CallFunc( m_beginFrameFn );
    } else {
        m_retryFindFuncs++;
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
            Engine::GAPI->GetRendererState().RendererSettings.ApplyFeatureLevel10Downgrades();
        }
    }
    //if ( DemoVisible )
    //    ImGui::ShowDemoWindow();

    if ( GetBlockGameInput() != m_lastFrameBlockGameInput ) {
        m_lastFrameBlockGameInput = GetBlockGameInput();
        GothicAPI::UpdateShouldBlockGameInput();
    }
}

void ImGuiShim::CallEndFrameScript()
{
    if ( m_endFrameFn != -1 ) {
        zCParser::GetParser()->CallFunc( m_endFrameFn );
    }
}

void ImGuiShim::RenderLoop()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    BuildFrameUI();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData() );

    CallEndFrameScript();
}

void ImGuiShim::RenderLoopD3D12( ID3D12GraphicsCommandList* commandList )
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    BuildFrameUI();

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData( ImGui::GetDrawData(), commandList );

    CallEndFrameScript();
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

    ImGuiCollectResolutions( Resolutions );
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


namespace
{
    bool IsFSRUpscaler( GothicRendererSettings::E_Upscaler v ) {
        return v == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3;
    }
    void FixupSettings(GothicRendererSettings& s) {
        if (s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR) {
            if ( !IsFSRUpscaler( s.Upscaler ) ) {
                s.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3;
            }
        }
        if (s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_TAA
            && ( s.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3)) {
            // don't allow TAA and FSR2 at the same time.
            s.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_1;
        }
        if (s.ResolutionScalePercent > 100 && s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR) {
            // switch to regular TAA if upsampled
            s.AntiAliasingMode = GothicRendererSettings::AA_TAA;
        }

        // MSAA (Forward+ only) and TAA/FSR are mutually exclusive: both do their own edge/temporal
        // resolve and combining them adds no value while doubling the resolve complexity.
        if ( s.MSAASamples > 1 && (s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_TAA
            || s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR) ) {
            s.AntiAliasingMode = GothicRendererSettings::E_AntiAliasingMode::AA_NONE;
        }
        if ( s.RendererMode != GothicRendererSettings::E_RendererMode::RM_ForwardPlus ) {
            s.MSAASamples = 1;
        }
    }
}

static std::vector<std::unique_ptr<GfxTexture>> s_previewTextures = {};

static ImTextureID GetImTextureIdFromGfx( GfxTexture* tex) {
    switch (Engine::GraphicsEngine->GetBackendAPI())
    {
    case EGraphicsEngineBackend::D3D11:
        return (ImTextureID)(intptr_t)D3D11Texture::From(tex)->GetShaderResourceView().Get();
    case EGraphicsEngineBackend::D3D12:
        return (ImTextureID)(intptr_t)D3D12Texture::From(tex)->GetSrvGpuHandle().ptr;
    }
    return ImTextureID{};
}

static ImTextureID GetOrLoadTexture( int textureId, std::string_view textureName ) {

    if ( textureId < s_previewTextures.size() && s_previewTextures[textureId] ) {
        return GetImTextureIdFromGfx( s_previewTextures[textureId].get() );
    }

    std::string path;
    path.reserve( 255 );
    path.append( R"(\System\GD3D11\Previews\)" );
    path.append( textureName );

    auto filePtr = zFILE_VDFS::Create( path.c_str() );

    if ( !filePtr->Exists() || filePtr->Open( false ) != zERRORS::zERROR_NONE ) {
        return ImTextureID{};
    }

    std::vector<uint8_t> data;
    data.resize( filePtr->Size() );
    filePtr->Read( data.data(), data.size() );
    filePtr->Close();

    int image_width = 0;
    int image_height = 0;
    unsigned char* image_data = stbi_load_from_memory( (const unsigned char*)data.data(), (int)data.size(), &image_width, &image_height, NULL, 4);
    if ( image_data == NULL )
        return ImTextureID{};

    GfxTexture* tex;
    Engine::GraphicsEngine->CreateTexture( &tex );

    auto r = tex->Init( INT2( image_width, image_height ), GfxTexture::ETextureFormat::TF_R8G8B8A8, 1, image_data, std::string(textureName.data(), textureName.size()) );
    stbi_image_free( image_data );

    if ( r != XR_SUCCESS ) {
        return ImTextureID{};
    }
    if ( s_previewTextures.size() < textureId+1 ) {
        s_previewTextures.resize( (textureId+1) * 2 );
    }
    s_previewTextures.push_back( {} );
    s_previewTextures[textureId].reset( tex );

    return GetImTextureIdFromGfx( s_previewTextures[textureId].get() );
}

static void ImRenderPreview(const std::vector<std::pair<int, std::string_view>>& previewImages, int defaultImage, int hoverImage ) {

    auto texBefore = GetOrLoadTexture( defaultImage, previewImages[defaultImage].second);
    auto texAfter = GetOrLoadTexture( hoverImage, previewImages[hoverImage].second );

    if ( texBefore && texAfter ) {
        ImVec2 imageSize( 256, 256 );

        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        ImVec2 maxPos = ImVec2( cursorPos.x + imageSize.x, cursorPos.y + imageSize.y );

        bool isHovered = ImGui::IsMouseHoveringRect( cursorPos, maxPos );

        ImTextureID displayTex = isHovered ? texAfter : texBefore;
        ImGui::Image( displayTex, imageSize );
    }
}

static int s_currentTextureId = 0;

void ImGuiShim::RenderSettingsWindowModern() {
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
    static const char* settingsLabel = "GD3D11 " VERSION_NUMBER " - (" BUILD_DATE ")##XX";
#else
    static const char* settingsLabel = "GD3D11 " VERSION_NUMBER "##XX";
#endif
    ShaderCategory shadersToReload = ShaderCategory::None;

    auto renderDisplayTab = [this]() {
        if ( ImGui::BeginTabItem( "Display" ) ) {
            ImGui::PushFont( nullptr, 18 );

            GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;
            ImVec2 buttonWidth( 275, 0 );

            for ( size_t i = 0; i < Resolutions.size(); ++i ) {
                if ( Resolutions[i].first == CurrentResolution ) {
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
                        Engine::GraphicsEngine->TriggerResize( Resolutions[i].first );
                    }

                    if ( isSelected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImText( "Resolution Scale", buttonWidth ); ImGui::SameLine();
            if ( settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3 ) {
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
                if ( ImComboBox( "##ResolutionScalePercent", fsrLevels, &settings.ResolutionScalePercent ) ) {
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip( "Effective resolution: %d x %d",
                    CurrentResolution.x * settings.ResolutionScalePercent / 100,
                    CurrentResolution.y * settings.ResolutionScalePercent / 100
                );
            } else {
                static float previousResolutionScale = static_cast<float>(settings.ResolutionScalePercent);
                if ( ImGui::SliderFloat( "##ResolutionScalePercent", &previousResolutionScale, 25.0f, 200.0f, "%.0f%%" ) ) {
                    previousResolutionScale = std::clamp( previousResolutionScale, 25.0f, 200.0f );
                    settings.ResolutionScalePercent = static_cast<int>(previousResolutionScale);
                }
                ImGui::SetItemTooltip( "Effective resolution: %d x %d",
                    CurrentResolution.x * settings.ResolutionScalePercent / 100,
                    CurrentResolution.y * settings.ResolutionScalePercent / 100
                );
            }

            ImText( "Upscaler", buttonWidth ); ImGui::SameLine();
            static std::vector<std::pair<const char*, GothicRendererSettings::E_Upscaler>> upscalers = {
                { "Simple", GothicRendererSettings::E_Upscaler::UPSCALER_DEFAULT },
                { "FSR 1", GothicRendererSettings::E_Upscaler::UPSCALER_FSR_1 },
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

            ImGui::PopFont();
            ImGui::EndTabItem();
        }
    };

    auto renderGraphicsTab = [this, &shadersToReload]() {
        if ( ImGui::BeginTabItem( "Graphics" ) ) {
            ImGui::PushFont( nullptr, 18 );

            GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;

            static std::vector<std::pair<const char*, int>> graphicsPresets = {
                {"Custom", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_CUSTOM},
                {"Low", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_LOW},
                {"Medium", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_MEDIUM},
                {"High", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_HIGH},
                {"Very High", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_VERY_HIGH},
            };

            ImGui::TextUnformatted( "Graphics Preset" ); ImGui::SameLine();

            ImGui::PushItemWidth( 250 );
            if ( ImComboBoxC( "##GraphicsPreset", graphicsPresets, (int*)&settings.GraphicsPreset, [&settings]() {
                settings.ApplyGraphicsPreset();
                } ) ) {
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();
            ImGui::Separator();

            if ( ImGui::BeginTable( "##TblGraphics", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV ) ) {
                ImGui::TableSetupColumn( "Main Content", ImGuiTableColumnFlags_WidthStretch );
                ImGui::TableSetupColumn( "Preview", ImGuiTableColumnFlags_WidthFixed, 256.0f );

                {
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    static const char* ssrLevels[] = { "Disabled", "Low", "Medium", "High" };
                    int ssr = std::clamp<int>(settings.WaterSSRQuality, 0, std::size( ssrLevels ) - 1);
                    if ( ImGui::SliderInt( "Water Reflections", &ssr, 0, std::size( ssrLevels ) - 1, ssrLevels[ssr], ImGuiSliderFlags_::ImGuiSliderFlags_AlwaysClamp ) ) {
                        settings.WaterSSRQuality = (GothicRendererSettings::E_WaterSSRQuality)ssr;
                        shadersToReload |= ShaderCategory::Water; // recompile PS_Water with the new SSR_QUALITY
                    }

                    ImGui::TableNextColumn();
                    static std::vector<std::pair<int, std::string_view>> ssrPreviews = {
                        {s_currentTextureId++, "SSR_0_Disabled.jpg" },
                        {s_currentTextureId++, "SSR_1_Low.jpg" },
                        {s_currentTextureId++, "SSR_2_Medium.jpg" },
                        {s_currentTextureId++, "SSR_3_High.jpg" },
                    };

                    int before = ssrPreviews[0].first;
                    int after = ssrPreviews[3].first;
                    if ( ssr == 1 ) {
                        before = ssrPreviews[1].first;
                        after = ssrPreviews[0].first;
                    } else if ( ssr == 2 ) {
                        before = ssrPreviews[2].first;
                        after = ssrPreviews[0].first;
                    } else if ( ssr == 3 ) {
                        before = ssrPreviews[3].first;
                        after = ssrPreviews[0].first;
                    }

                    ImRenderPreview( ssrPreviews, before, after );
                }

                ImGui::EndTable();
            }

            ImGui::PopFont();
            ImGui::EndTabItem();
        }
    };

    auto renderFeaturesTab = [this]() {
        if ( ImGui::BeginTabItem( "Features" ) ) {
            ImGui::PushFont( nullptr, 18 );
            // TODO: additional features, like vob highlighting in G1
            
            ImGui::PopFont();
            ImGui::EndTabItem();
        }
    };


    ImGui::SetNextWindowPos( ImVec2( windowSize.x / 2, windowSize.y / 2 ), ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );
    ImGui::SetNextWindowSize( ImVec2( 800, 600 ), ImGuiCond_Appearing );
    if ( ImGui::Begin( settingsLabel, nullptr, ImGuiWindowFlags_NoCollapse ) ) {
        GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;
        FixupSettings( settings );
        if (ImGui:: BeginTabBar("##TabModernSettings") ) {
            renderDisplayTab();
            renderGraphicsTab();
            renderFeaturesTab();

            ImGui::EndTabBar();
        }
    }

    ImGui::End();

    if ( shadersToReload != ShaderCategory::None ) {
        Engine::GraphicsEngine->ReloadShaders( shadersToReload );
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

    // RenderSettingsWindowModern(); -- Disabled while in development ;)

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
            settings.ApplyGraphicsPreset();
            } ) ) {
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::Separator();
        
        {
            ImGui::BeginGroup();
            ImGui::Checkbox( "Vsync", &settings.EnableVSync );
            bool enabled = settings.AllowNormalmaps > 0; 
            if ( ImGui::Checkbox( "NormalMaps", &enabled ) ) {
                if (enabled) {
                    settings.AllowNormalmaps = 1;
                } else {
                    settings.AllowNormalmaps = 0;
                }
                Engine::GraphicsEngine->ReloadShaders();
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
                {"FSR 3", GothicRendererSettings::E_AntiAliasingMode::AA_FSR, "FidelityFX Super Resolution 3"},

            };
            {
                ImGui::PushID( "AntiAliasingSettings" );
                auto selectedMode = settings.AntiAliasingMode;
                if ( ImComboBoxCT( "Anti Aliasing", antiAliasing, &selectedMode, [&selectedMode, &settings] {
                    if ( selectedMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR ) {
                        settings.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3;
                    }
                    settings.AntiAliasingMode = selectedMode;
                    } ) ) {
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }

            if ( settings.RendererMode == GothicRendererSettings::RM_ForwardPlus ) {
                static const std::vector<std::pair<const char*, int>> msaaSamples = {
                    { "Off", 1 },
                    { "2x",  2 },
                    { "4x",  4 },
                    { "8x",  8 },
                };
                if ( ImComboBox( "MSAA", msaaSamples, &settings.MSAASamples ) ) {
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip( "Forward+ only: hardware multisample anti-aliasing for opaque geometry. Mutually exclusive with TAA/FSR." );
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

#if defined(BUILD_GOTHIC_2_6_fix) || defined(BUILD_GOTHIC_1_CLASSIC)
#if defined(BUILD_GOTHIC_1_CLASSIC)
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
            {
                const char* ssrLevels[] = { "Disabled", "Low", "Medium", "High" };
                int ssr = settings.WaterSSRQuality;
                if ( ImGui::Combo( "Water Reflections (SSR)", &ssr, ssrLevels, IM_ARRAYSIZE( ssrLevels ) ) ) {
                    settings.WaterSSRQuality = (GothicRendererSettings::E_WaterSSRQuality)ssr;
                    shadersToReload |= ShaderCategory::Water; // recompile PS_Water with the new SSR_QUALITY
                }
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
            if ( settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3 ) {
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
                auto previousResolutionScale = settings.ResolutionScalePercent;
                if ( ImGui::SliderInt( "##ResolutionScalePercent", &previousResolutionScale, 25, 200, "%d %%" ) ) {
                    settings.ResolutionScalePercent = std::clamp( previousResolutionScale, 25, 200 );
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
                { "Very Low", static_cast<int>(GothicRendererSettings::TX_QUALITY::VeryLow) },
                { "Low", static_cast<int>(GothicRendererSettings::TX_QUALITY::Low) },
                { "Medium", static_cast<int>(GothicRendererSettings::TX_QUALITY::Medium) },
                { "High", static_cast<int>(GothicRendererSettings::TX_QUALITY::High) },
                { "Very High", static_cast<int>(GothicRendererSettings::TX_QUALITY::VeryHigh) },
                { "Extreme", static_cast<int>(GothicRendererSettings::TX_QUALITY::MAX) }, // TODO: this should depend on the GPU capabilities like in the original game
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

            // Resolutions are restricted to these five power-of-two steps — 8192 is the hard ceiling on both
            // backends/feature levels (a 16384 cascade slice is ~1GB, not worth the VRAM for a shadow map).
            const static std::vector<std::pair<const char*, int>> shadowMapSizes = {
                {"very low", 512},
                {"low", 1024},
                {"medium", 2048},
                {"high", 4096},
                {"very high", 8192},
            };

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

        ImGui::DragFloat( "Exposure", &settings.Exposure, 0.01f, 0.0f, 8.0f, "%.2f" );

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

        // --- Real HDR display output (ST.2084 scanout; D3D12 backend only) -----------------------------
        ImGui::SeparatorText( "HDR Display Output" );
        if ( ImGui::Checkbox( "HDR Monitor Output", &settings.HDR_Monitor ) ) {
            // The swapchain colour space + the display-buffer format are decided at device/swapchain setup,
            // so this only takes effect on the next launch (same as the GraphicsAPI switch).
        }
        ImGui::SameLine();
        ImGui::TextDisabled( "(restart required)" );

        float detectedMax = 0.0f, detectedMin = 0.0f, detectedMaxFullFrame = 0.0f;
        const bool hdrActive = Engine::GraphicsEngine
            && Engine::GraphicsEngine->GetHdrOutputInfo( detectedMax, detectedMin, detectedMaxFullFrame );
        if ( hdrActive ) {
            ImGui::Text( "Active. Monitor reports %.0f nits peak (%.0f full-frame, %.4f black).",
                detectedMax, detectedMaxFullFrame, detectedMin );
        } else if ( settings.HDR_Monitor ) {
            ImGui::TextDisabled( "Not active (needs the D3D12 backend and an HDR-enabled display in Windows)." );
        } else {
            ImGui::TextDisabled( "Disabled - the SDR tonemapper is in use." );
        }

        ImGui::BeginDisabled( !settings.HDR_Monitor );
        ImGui::Checkbox( "Auto Max Brightness (use monitor metadata)", &settings.HDR_AutoMaxBrightness );
        ImGui::BeginDisabled( settings.HDR_AutoMaxBrightness );
        ImGui::SliderFloat( "HDR Max Brightness", &settings.HDR_MaxBrightness, 400.0f, 2000.0f, "%.0f nits",
            ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() ) {
            ImGui::SetTooltip( "Peak luminance the highlight roll-off targets. Monitor-reported metadata is\n"
                "frequently wrong - lower this until the brightest highlights stop clipping." );
        }
        ImGui::SliderFloat( "Paper White / UI Brightness", &settings.HDR_PaperWhite, 80.0f, 500.0f, "%.0f nits",
            ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
        if ( ImGui::IsItemHovered() ) {
            ImGui::SetTooltip( "Nit level that SDR white maps to. Sets the brightness of the HUD/menus and of\n"
                "diffuse-white surfaces; the headroom above it is what highlights get to use." );
        }
        ImGui::EndDisabled();

        ImGui::Checkbox( "Bloom", &settings.EnableBloom );
        ImGui::BeginDisabled( !settings.EnableBloom );
        {
            ImGui::DragFloat( "Bloom Threshold", &settings.BloomThreshold, 0.01f, 0.0f, 10.0f, "%.2f" );
            ImGui::DragFloat( "Bloom Strength", &settings.BloomStrength, 0.01f, 0.0f, 10.0f, "%.2f" );
            ImGui::DragFloat( "Bloom Knee", &settings.BloomKnee, 0.01f, 0.0f, 1.0f, "%.2f" );
            ImGui::DragFloat( "Bloom Radius", &settings.BloomRadius, 0.01f, 0.0f, 5.0f, "%.2f" );
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
        ImGui::Checkbox( "Allow Pointlights self-shadowing", &settings.AllowSelfShadowingPointlights );
        ImGui::SetItemTooltip("Lets things like torches and lights from players also cast shadow for players.");

        ImGui::Checkbox( "Disable static Pointlights", &settings.DisableStaticPointlights );
        ImGui::SetItemTooltip("D3D12 only. Drops every static light from the scene.\n"
            "Gothic fills rooms and caves with 10-30 co-located 'atmospheric' static lights that only raise the\n"
            "ambient level; with HDR output they stack up and make interiors far too bright.");

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

        // Resolutions are restricted to these five power-of-two steps — 8192 is the hard ceiling on both
        // backends/feature levels (a 16384 cascade slice is ~1GB, not worth the VRAM for a shadow map).
        static std::vector<std::pair<const char*, int>> shadowMapSizes = {
          {"512", 512},
          {"1024", 1024},
          {"2048", 2048},
          {"4096", 4096},
          {"8192", 8192},
        };

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
                settings.ApplyFeatureLevel10Downgrades();
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

        // Sky image-based lighting — the D3D12 indirect-light term. Deliberately OUTSIDE the shadow block's
        // BeginDisabled/EndDisabled: it is ambient lighting and stays live with shadows switched off. Sits here
        // rather than under Atmosphere because both knobs act on the same term ShadowStrength scales.
        ImGui::SeparatorText( "Sky Lighting (D3D12)##AdvancedSkyIbl" );
        ImGui::DragFloat( "SkyIblIntensity", &settings.SkyIblIntensity, 0.02f, 0.0f, 5.0f, "%.2f" );
        ImGui::SetItemTooltip( "D3D12 only. Scales the sky image-based indirect light (diffuse irradiance +\n"
            "prefiltered specular) that replaced the flat ambient term. Multiplies with\n"
            "ShadowStrength; 1.0 is neutral. 0 disables the IBL and falls back to the flat\n"
            "ambient, which has no indirect specular at all (metals go black off-sun)." );
        ImGui::DragFloat( "SkyIblNightFloor", &settings.SkyIblNightFloor, 0.005f, 0.0f, 0.5f, "%.3f" );
        ImGui::SetItemTooltip( "D3D12 only. Minimum night sky radiance for the IBL, in linear units.\n"
            "Gothic's night is not physically lit - zCSkyState's night fogColor is (5,5,20),\n"
            "so a faithful sky IBL leaves nights nearly black. This is the deliberate fill\n"
            "(D3D11's atmospheric scattering hardcodes the same idea). 0 disables the floor.\n"
            "Blue-weighted, so it reads as moonlight rather than grey underexposure." );

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

#if defined(BUILD_GOTHIC_2_6_fix) || defined(BUILD_GOTHIC_1_CLASSIC)
#if defined(BUILD_GOTHIC_1_CLASSIC)
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
        ImGui::Checkbox( "Highlight interactive focus", &settings.G1HighlightInteractiveFocus );
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
                ImGui::SliderFloat("Depth Slope Bias", &settings.DebugSettings.ShadowCascades.ShadowDepthSlopeBias, 0.0f, 8.0f, "%.6f");
                ImGui::SetItemTooltip("Slope-scaled depth bias for the shadow caster pass. Higher removes shadow acne/stepping on thin geometry; too high detaches contact shadows (peter-panning)");
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Culling", nullptr, ImGuiTabItemFlags_::ImGuiTabItemFlags_NoReorder)) {
                ImGui::TextUnformatted("GPU-driven VOB culling (D3D12 only)");
                ImGui::Checkbox("GPU VOB culling", &settings.GpuVobCulling );
                ImGui::SetItemTooltip("Collect static VOBs distance-only on the CPU and frustum-cull them in a compute shader instead. Off = the classic CPU per-VOB frustum test");
                ImGui::BeginDisabled( !settings.GpuVobCulling );
                ImGui::Checkbox("GPU occlusion culling", &settings.GpuVobOcclusionCulling );
                ImGui::SetItemTooltip("Additionally reject VOB instances hidden behind the world mesh, using a Hi-Z pyramid built from the world depth prepass");
                ImGui::EndDisabled();
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
                if ( settings.RendererMode == GothicRendererSettings::RM_ForwardPlus ) {
                    static const std::vector<std::pair<const char*, int>> msaaSamples = {
                        { "Off", 1 },
                        { "2x",  2 },
                        { "4x",  4 },
                        { "8x",  8 },
                    };
                    if ( ImComboBox( "MSAA", msaaSamples, &settings.MSAASamples ) ) {
                        ImGui::EndCombo();
                    }
                    ImGui::SetItemTooltip( "Forward+ only: hardware multisample anti-aliasing for opaque geometry. Falls back to the highest supported level if the requested one isn't available. Mutually exclusive with TAA/FSR." );
                }
                if (!FeatureLevel10Compatibility){
                    ImGui::Checkbox("Use MDI", &settings.DebugSettings.FeatureSet.UseMDI );
                    ImGui::SetItemTooltip("Support for MultiDrawInstancedIndirect via Driver Extensions (AMD, Nvidia, Intel).");

                    ImGui::Checkbox("Use Layered Drawing", &settings.DebugSettings.FeatureSet.UseLayeredRendering );
                    ImGui::Checkbox("Use Tiled Lighting", &settings.EnableTiledLighting );
                    ImGui::SetItemTooltip( "Uses compute shader light culling for point lights. Reduces draw calls and overdraw." );
                }
                if ( ImGui::Checkbox( "Use Shadow Atlas", &settings.DebugSettings.FeatureSet.UseShadowAtlas ) ) {
                    settings.ApplyFeatureLevel10Downgrades();
                }
                ImGui::SetItemTooltip("Enables a less intensive but lower quality shadow solution.");
                if ( ImGui::Checkbox( "Use Screen-Space Shadow Mask", &settings.DebugSettings.FeatureSet.UseScreenSpaceShadowMask ) ) {
                    Engine::GraphicsEngine->ReloadShaders( ShaderCategory::LightsAndShadows );
                }
                ImGui::SetItemTooltip( "Forward+ debug option: precompute sun shadows in a separate screen-space pass. Changing this reloads light/shadow shaders." );

                ImGui::Checkbox( "Generate AO Normals From Depth", &settings.DebugSettings.FeatureSet.GenerateAONormalsFromDepth );
                ImGui::SetItemTooltip( "Forward+ only: run a compute pass that builds smooth normals from depth for SAO/ASSAO. Off = depth-only AO fallback." );

                ImGui::Checkbox("Use World Section BVH", &settings.DebugSettings.FeatureSet.UseWorldSectionBVH );
                ImGui::SetItemTooltip("Use Bounding Volume Hierarchy for world sections. Improves culling performance.");

                if (ImGui::Checkbox("Compressed Normalmaps support", &settings.CompressedNormalsSupport )) {
                    Engine::GraphicsEngine->ReloadShaders();
                }
                ImGui::SetItemTooltip("Enables support for BC5 compressed Normalmaps.");

                static const std::vector<std::pair<const char*, int>> normalMapType = {
                    { "Disabled",   0 },
                    { "OpenGL (Y+)",   1 }, 
                    { "DirectX (Y-)",   2 },
                };
                if ( ImComboBoxC( "Normalmapping texture mode", normalMapType, &settings.AllowNormalmaps, []
                {
                    Engine::GraphicsEngine->ReloadShaders();
                    Engine::GAPI->UpdateTextureMaxSize();
                } ) ) {
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("Enable Normalmapping.\nIf in doubt ask the creator of the Normalmaps you use.\nRequires Normalmaps with RGB channel layout.");

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
                {"FSR 3", GothicRendererSettings::E_AntiAliasingMode::AA_FSR},
            };
            auto selectedMode = settings.AntiAliasingMode;
            if ( ImComboBoxC( "Anti Aliasing", antiAliasing, &selectedMode, [&selectedMode, &settings] {
                if ( selectedMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR ) {
                    settings.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3;
                }
                settings.AntiAliasingMode = selectedMode;
                } ) ) {
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }

        if ( settings.RendererMode == GothicRendererSettings::RM_ForwardPlus ) {
            static const std::vector<std::pair<const char*, int>> msaaSamples = {
                { "Off", 1 },
                { "2x",  2 },
                { "4x",  4 },
                { "8x",  8 },
            };
            if ( ImComboBox( "MSAA", msaaSamples, &settings.MSAASamples ) ) {
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip( "Forward+ only: hardware multisample anti-aliasing for opaque geometry. Mutually exclusive with TAA/FSR." );
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
