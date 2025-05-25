#include "ImGuiShim.h"
#include "GSky.h"
#include <VersionHelpers.h>
#include <ShellScalingAPI.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

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

void ImGuiShim::Init(
    HWND Window,
    const Microsoft::WRL::ComPtr<ID3D11Device1>& device,
    const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context
)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false; // don't draw two mice
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; //Not needed and it's annoying.
    OutputWindow = Window;
    ImGui_ImplWin32_Init( OutputWindow );
    ImGui_ImplDX11_Init( device.Get(), context.Get() );

    const auto actualDPI = GetDpi( Window );
    Initiated = true;

    auto& Displaylist = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetDisplayModeList();
    Resolutions.clear();
    for ( auto it = Displaylist.rbegin(); it != Displaylist.rend(); ++it ) {
        std::string s = std::to_string( (*it).Width ) + "x" + std::to_string( (*it).Height );
        Resolutions.emplace_back( s );
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
    if ( NewResolution.x != CurrentResolution.x
        || NewResolution.y != CurrentResolution.y ) {
        Engine::GraphicsEngine->OnResize( NewResolution );
        Engine::GraphicsEngine->ReloadShaders();
        CurrentResolution = NewResolution;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if ( SettingsVisible ) {
        RenderSettingsWindow();
    }
    if ( AdvancedSettingsVisible ) {
        RenderAdvancedSettingsWindow();
    }
    //if ( DemoVisible )
    //    ImGui::ShowDemoWindow();
    
    ImGui::GetIO().MouseDrawCursor = INT2( ImGui::GetMainViewport()->Size.x, ImGui::GetMainViewport()->Size.y ) != Engine::GraphicsEngine->GetResolution();
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData() );
}


void ImGuiShim::OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    if ( Initiated )
        ImGui_ImplWin32_WndProcHandler( hWnd, msg, wParam, lParam );
}

void ImGuiShim::OnResize( INT2 newSize )
{
    CurrentResolution = newSize;
    NewResolution = newSize;
}

template <typename T>
bool ImComboBoxC( const char* id, std::vector<std::pair<char*, T>>& items, T* storage, const std::function<void()>& selected ) {
    if ( storage == nullptr || items.size() == 0 ) {
        return ImGui::BeginCombo( id, "invalid storage" );
    }
    std::pair<char*, T> selectedItem = items[0];
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
bool ImComboBox( const char* id, std::vector<std::pair<char*, T>>& items, T* storage ) {
    if ( storage == nullptr || items.size() == 0 ) {
        return ImGui::BeginCombo( id, "invalid storage" );
    }
    std::pair<char*, T> selectedItem = items[0];
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

void ImText( const char * label, const ImVec2& size ) {
    auto& col = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Button );

    ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_ButtonActive, col );
    ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_ButtonHovered, col );
    ImGui::PushStyleVarX( ImGuiStyleVar_::ImGuiStyleVar_ButtonTextAlign, 0 );

    ImGui::Button( label, size );
    ImGui::PopStyleVar( 1 );

    ImGui::PopStyleColor( 2 );
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

    ImGui::SetNextWindowPos( ImVec2( windowSize.x / 2, windowSize.y / 2 ), ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );
    if ( ImGui::Begin( "Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize ) ) {
        GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;
        {
            ImGui::BeginGroup();
            ImGui::Checkbox( "Vsync", &settings.EnableVSync );
            if ( ImGui::Checkbox( "NormalMaps", &settings.AllowNormalmaps ) ) {
                Engine::GAPI->UpdateTextureMaxSize();
            }

            ImGui::Checkbox( "HBAO+", &settings.HbaoSettings.Enabled );
            ImGui::Checkbox( "Godrays", &settings.EnableGodRays );
            ImGui::Checkbox( "SMAA", &settings.EnableSMAA );
            ImGui::Checkbox( "HDR", &settings.EnableHDR );
            ImGui::Checkbox( "Shadows", &settings.EnableShadows );
            ImGui::Checkbox( "Shadow filtering", &settings.EnableSoftShadows );
            ImGui::Checkbox( "Compress Backbuffer", &settings.CompressBackBuffer );
            ImGui::Checkbox( "Animate Static Vobs", &settings.AnimateStaticVobs );

#ifdef BUILD_GOTHIC_2_6_fix
            ImGui::Text( "Wind effect" ); ImGui::SameLine();
            static std::vector<std::pair<char*, int>> windEffectQuality = {
                { "Disabled", GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE },
                { "Simple", GothicRendererSettings::EWindQuality::WIND_QUALITY_SIMPLE },
                { "Advanced", GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED },
            };
            if ( ImComboBoxC( "##Wind effect", windEffectQuality, &settings.WindQuality, []() { Engine::GraphicsEngine->ReloadShaders(); } ) ) {
                ImGui::EndCombo();
            }
#endif //BUILD_GOTHIC_2_6_fix

            ImGui::Checkbox( "Enable Rain", &settings.EnableRain );
            ImGui::Checkbox( "Enable Rain Effects", &settings.EnableRainEffects );
            ImGui::Checkbox( "Limit Light Intensity", &settings.LimitLightIntensity );
            ImGui::Checkbox( "Draw World Section Intersections", &settings.DrawSectionIntersections );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "This option draws every world chunk that intersect with GD3D11 world draw distance." );

            ImGui::Checkbox( "Occlusion Culling", &settings.EnableOcclusionCulling );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Hides objects that are not visible by camera. Doesn't work properly, turn off if you don't play on potato." );


            ImGui::EndGroup();
        }

        ImGui::SameLine();

        {
            ImGui::BeginGroup();
            ImGui::PushItemWidth( 250 );

            std::string currRes = CurrentResolution.toString();
            auto it = std::find( Resolutions.begin(), Resolutions.end(), currRes );
            if ( it != Resolutions.end() ) {
                ResolutionState = std::distance( Resolutions.begin(), it );
            }
            ImText( "Resolution", buttonWidth ); ImGui::SameLine();
            if ( ImGui::BeginCombo( "##Resolution", Resolutions[ResolutionState].c_str() ) ) {
                for ( size_t i = 0; i < Resolutions.size(); i++ ) {
                    bool isSelected = (ResolutionState == i);

                    if ( ImGui::Selectable( Resolutions[i].c_str(), isSelected ) ) {
                        ResolutionState = i;
                        NewResolution = INT2( Resolutions[i] );
                    }

                    if ( isSelected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImText( "Texture Quality", buttonWidth ); ImGui::SameLine();
            static std::vector<std::pair<std::string, int>> QualityOptions = {
                { "Potato",32 },
                { "Ultra Low", 64 },
                { "Low", 128 },
                { "Medium", 256 },
                { "High", 512 },
                { "Ultra High", 16384 },
            };
            for ( int i = QualityOptions.size() - 1; i >= 0; i-- ) {
                if ( settings.textureMaxSize >= QualityOptions.at( i ).second ) {
                    TextureQualityState = i;
                    break;
                }
            }
            if ( ImGui::BeginCombo( "##TextureQuality", QualityOptions[TextureQualityState].first.c_str() ) ) {

                for ( size_t i = 0; i < QualityOptions.size(); i++ ) {
                    bool isSelected = (TextureQualityState == i);

                    if ( ImGui::Selectable( QualityOptions[i].first.c_str(), isSelected ) ) {
                        TextureQualityState = i;
                        settings.textureMaxSize = QualityOptions[i].second;
                        Engine::GAPI->UpdateTextureMaxSize();
                    }

                    if ( isSelected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImText( "Display Mode [*]", buttonWidth ); ImGui::SameLine();
            auto displayModeState = settings.WindowMode;
            static std::vector<std::pair<char *, int>> DisplayEnums = {
                { "Fullscreen Borderless", WindowModes::WINDOW_MODE_FULLSCREEN_BORDERLESS },
                { "Fullscreen Exclusive", WindowModes::WINDOW_MODE_FULLSCREEN_EXCLUSIVE },
                { "Fullscreen Lowlatency", WindowModes::WINDOW_MODE_FULLSCREEN_LOWLATENCY },
                { "Windowed", WindowModes::WINDOW_MODE_WINDOWED },
            };

            if ( ImComboBoxC( "##DisplayMode", DisplayEnums, &settings.WindowMode, [&settings]() {
                // selected
                Engine::GraphicsEngine->SetWindowMode( (WindowModes)settings.WindowMode );
                }) ) {
                ImGui::EndCombo();
            }

            ImText( "Shadow Quality", buttonWidth ); ImGui::SameLine();
            
            static std::vector<std::pair<char*, int>> shadowMapSizesMax = {
                {"very low", 512},
                {"low", 1024},
                {"medium", 2048},
                {"high", 4096},
                {"very high", 8192},
                {"ultra high", 16384},
            };
            static std::vector<std::pair<char*, int>> shadowMapSizesDxFeature10 = {
                {"very low", 512},
                {"low", 1024},
                {"medium", 2048},
                {"high", 4096},
                {"very high", 8192},
            };
            std::vector<std::pair<char*, int>>& shadowMapSizes = shadowMapSizesMax;
            if ( FeatureLevel10Compatibility ) {
                shadowMapSizes = shadowMapSizesDxFeature10;
            }

            if ( ImComboBoxC( "##ShadowQuality", shadowMapSizes, (int*)(&settings.ShadowMapSize), []() { Engine::GraphicsEngine->ReloadShaders(); } ) ) {
                ImGui::EndCombo();
            }

            ImText( "Dynamic Shadows", buttonWidth ); ImGui::SameLine();
            static std::vector<std::string> DynamicShadowEnums = { "Off", "Static", "Dynamic Update", "Full" };
            DynamicShadowState = static_cast<int>(settings.EnablePointlightShadows);
            if ( ImGui::BeginCombo( "##DynamicShadows", DynamicShadowEnums[DynamicShadowState].c_str() ) ) {
                for ( size_t i = 0; i < DynamicShadowEnums.size(); i++ ) {
                    bool isSelected = (DynamicShadowState == i);

                    if ( ImGui::Selectable( DynamicShadowEnums[i].c_str(), isSelected ) ) {
                        DynamicShadowState = i;
                        settings.EnablePointlightShadows = static_cast<GothicRendererSettings::EPointLightShadowMode>( i );
                    }

                    if ( isSelected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            static bool fpsLimitEnabled = 0;
            fpsLimitEnabled = settings.FpsLimit > 0;

            ImText( "FPS Limit", { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y }); ImGui::SameLine();
            if ( ImGui::Checkbox( "##Enable FPS Limit", &fpsLimitEnabled )) {
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
            if ( ImGui::SliderFloat( "##OutdoorSmallVobDrawRadius", &smallObjectDrawDistance, 1.f, 100.0f, "%.0f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput) ) {
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
            ImGui::SliderFloat( "##Contrast", &settings.GammaValue, 0.1f, 2.0f, "%.1f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );

            ImText( "Brightness", buttonWidth ); ImGui::SameLine();
            ImGui::SliderFloat( "##Brightness", &settings.BrightnessValue, 0.1f, 3.0f, "%.1f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
            ImGui::PopItemWidth();

            ImGui::EndGroup();
        }

        if ( ImGui::Button( "OK", ImVec2( ImGui::GetContentRegionAvail().x, 30.f ) ) ) { //nikt nie bedzie z tego strzelac
            SettingsVisible = false;
            IsActive = false;
            Engine::GAPI->SetEnableGothicInput( true );
            Engine::GAPI->SaveRendererWorldSettings( settings );
            Engine::GAPI->SaveMenuSettings( MENU_SETTINGS_FILE );
        }
    }
    ImGui::End();

}

void RenderAdvancedColumn1( GothicRendererSettings& settings, GothicAPI* gapi ) {
    if ( ImGui::Begin( "Sky", nullptr, ImGuiWindowFlags_NoCollapse ) ) {

        ImGui::Checkbox( "GodRays", &settings.EnableGodRays );
        ImGui::DragFloat( "GodRayDecay", &settings.GodRayDecay, 0.01f );
        ImGui::DragFloat( "GodRayWeight", &settings.GodRayWeight, 0.01f );
        ImGui::ColorEdit3( "GodRayColorMod", &settings.GodRayColorMod.x );
        ImGui::DragFloat( "GodRayDensity", &settings.GodRayDensity, 0.01f );
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
        ImGui::DragFloat3( "LightDirection", &atmosphereSettings.LightDirection.x, 0.001f );
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


void RenderAdvancedColumn2( GothicRendererSettings& settings, GothicAPI* gapi ) {
    if ( ImGui::Begin( "General", nullptr, ImGuiWindowFlags_NoCollapse ) ) {

        ImGui::Text( "Version: " );
        ImGui::SameLine();
        ImGui::Text( VERSION_NUMBER );

        ImGui::Checkbox( "Enable DebugLog", &settings.EnableDebugLog );
        if ( ImGui::Button( "Save ZEN-Resources" ) ) {
            gapi->SaveCustomZENResources();
        }
        if ( ImGui::Button( "Load ZEN-Resources" ) ) {
            gapi->LoadCustomZENResources();
        }
        ImGui::Separator();
        ImGui::Checkbox( "DisableRendering", &settings.DisableRendering );
        ImGui::SliderInt( "SectionDrawRadius", &settings.SectionDrawRadius, 0, 20, "%d", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
        ImGui::SliderInt( "Draw WorldMesh", &settings.DrawWorldMesh, 0, 3 );

        ImGui::Checkbox( "Draw VOBs", &settings.DrawVOBs );
        ImGui::Checkbox( "Draw Dynamic Vobs", &settings.DrawDynamicVOBs );
        ImGui::SliderFloat( "OutdoorVobDrawRadius", &settings.OutdoorVobDrawRadius, 1.0f, 100000.0f, "%.0f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
        ImGui::SliderFloat( "IndoorVobDrawRadius", &settings.IndoorVobDrawRadius, 1.0f, 100000.0f,  "%.0f", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
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
        ImGui::Checkbox( "Draw Fog", &settings.DrawFog );

        static std::vector<std::pair<char*, int>> fogRanges = { {"3", 3}, {"4", 4}, {"5", 5}, {"6", 6}, {"7", 7}, {"8", 8}, {"9", 9}, {"10", 10} };
        if ( ImComboBox( "Fog Range", fogRanges, &settings.FogRange ) ) {
            ImGui::EndCombo();
        }

        ImGui::Checkbox( "HDR", &settings.EnableHDR );

        static std::vector<std::pair<char*, int>> hdrToneMapValues = {
            {"ToneMap_jafEq4", 0},
            {"Uncharted2Tonemap", 1},
            {"ACESFilmTonemap", 2},
            {"PerceptualQuantizerTonemap", 3},
            {"ToneMap_Simple", 4},
            {"ACESFittedTonemap", 5},
        };

        ImGui::BeginDisabled( !settings.EnableHDR );
        if ( ImComboBox( "HDR ToneMap", hdrToneMapValues, (int*)(&settings.HDRToneMap) ) ) {
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        ImGui::Checkbox( "SMAA", &settings.EnableSMAA );
        ImGui::DragFloat( "Sharpen", &settings.SharpenFactor, 0.01f );
        ImGui::Checkbox( "DynamicLighting", &settings.EnableDynamicLighting );

        static std::vector<std::pair<char*, int>> pointlightShadows = {
           {"Disabled", 0},
           {"Static", 1},
           {"Update Dynamic", 2},
           {"Full", 3},
        };
        if ( ImComboBox( "PointlightShadows", pointlightShadows, (int*)(&settings.EnablePointlightShadows) ) ) {
            ImGui::EndCombo();
        }
        // ImGui::Checkbox("FastShadows", &settings.FastShadows );	
        ImGui::Checkbox( "DrawShadowGeometry", &settings.DrawShadowGeometry );
        ImGui::Checkbox( "DoZPrepass", &settings.DoZPrepass );
        ImGui::Checkbox( "VSync", &settings.EnableVSync );
        ImGui::Checkbox( "OcclusionCulling", &settings.EnableOcclusionCulling );
        ImGui::Checkbox( "Sort RenderQueue", &settings.SortRenderQueue );
        ImGui::Checkbox( "Draw Threaded", &settings.DrawThreaded );
        ImGui::Checkbox( "AtmosphericScattering", &settings.AtmosphericScattering );
        ImGui::Checkbox( "SkeletalVertexNormals", &settings.ShowSkeletalVertexNormals );

        static std::vector<std::pair<char*, int>> shadowMapSizesMax = {
          {"512", 512},
          {"1024", 1024},
          {"2048", 2048},
          {"4096", 4096},
          {"8192", 8192},
          {"16384", 16384},
        };
        static std::vector<std::pair<char*, int>> shadowMapSizesDxFeature10 = {
         {"512", 512},
         {"1024", 1024},
         {"2048", 2048},
         {"4096", 4096},
         {"8192", 8192},
        };
        std::vector<std::pair<char*, int>>& shadowMapSizes = shadowMapSizesMax;
        if ( FeatureLevel10Compatibility ) {
            shadowMapSizes = shadowMapSizesDxFeature10;
        }

        if ( ImComboBoxC( "ShadowmapSize", shadowMapSizes, (int*)(&settings.ShadowMapSize), []() { Engine::GraphicsEngine->ReloadShaders(); } ) ) {
            ImGui::EndCombo();
        }
        ImGui::DragFloat( "WorldShadowRangeScale", &settings.WorldShadowRangeScale, 0.01f, 0.0f, 0.0f, "%.2f" );
        ImGui::DragFloat( "ShadowStrength", &settings.ShadowStrength, 0.01f, 0.01f, 5.0f, "%.2f" );
        ImGui::DragFloat( "ShadowAOStrength", &settings.ShadowAOStrength, 0.01f, -5.0f, 2.0f, "%.2f" );
        ImGui::DragFloat( "WorldAOStrength", &settings.WorldAOStrength, 0.01f, -5.0f, 2.0f, "%.2f" );
        ImGui::Checkbox( "WireframeWorld", &settings.WireframeWorld );
        ImGui::Checkbox( "WireframeVobs", &settings.WireframeVobs );
        // ImGui::Checkbox("Grass AlphaToCoverage", &settings.VegetationAlphaToCoverage );	

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
        // TwAddVarRW("SmallVobSize", TW_TYPE_FLOAT, &settings.SmallVobSize );
        // ImGui::Checkbox("AtmosphericScattering", &settings.AtmosphericScattering );
        ImGui::DragFloat( "FogGlobalDensity", &settings.FogGlobalDensity, 0.00001f, 0, 1.0f, "%.5f" );
        ImGui::DragFloat( "FogHeightFalloff", &settings.FogHeightFalloff, 0.00001f, 0, 1.0f, "%.5f" );
        ImGui::DragFloat( "FogHeight", &settings.FogHeight, 1.0f, 0.0f, 0.0f, "%.0f" );
        ImGui::ColorEdit3( "FogColorMod", &settings.FogColorMod.x );
        ImGui::DragFloat( "HDRLumWhite", &settings.HDRLumWhite, 0.01f, 0.0f, 0.0f, "%.2f" );
        ImGui::DragFloat( "HDRMiddleGray", &settings.HDRMiddleGray, 0.01f, 0.0f, 0.0f, "%.2f" );
        ImGui::DragFloat( "BloomThreshold", &settings.BloomThreshold, 0.01f, 0.0f, 0.0f, "%.2f" );
        ImGui::DragFloat( "BloomStrength", &settings.BloomStrength, 0.01f, 0.0f, 0.0f, "%.2f" );
        ImGui::DragFloat( "WindStrength", &settings.GlobalWindStrength, 0.01f, 0.0f, 0.0f, "%.2f" );
        ImGui::Checkbox( "LockViewFrustum", &settings.LockViewFrustum );
        ImGui::DragFloat( "GothicUIScale", &settings.GothicUIScale, 0.01f, 0.01f, 20.0f, "%.2f" );
        ImGui::DragFloat( "FOVHoriz", &settings.FOVHoriz, 1.0f, 1.0f, 360.0f, "%.0f" );
        ImGui::DragFloat( "FOVVert", &settings.FOVVert, 1.0f, 1.0f, 360.0f, "%.0f" );
        ImGui::Checkbox( "ForceFOV", &settings.ForceFOV );
#ifdef BUILD_GOTHIC_1_08k
        ImGui::Checkbox( "DrawForestPortals", &settings.DrawG1ForestPortals );
#endif

    }
    ImGui::End();
}

void RenderAdvancedColumn3( GothicRendererSettings& settings, GothicAPI* gapi ) {
    if ( ImGui::Begin( "FrameStats", nullptr, ImGuiWindowFlags_NoCollapse ) ) {
        auto& rendererInfo = gapi->GetRendererState().RendererInfo;

        ImGui::InputInt( "FPS", &rendererInfo.FPS, 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "StateChanges", (int*)(&rendererInfo.StateChanges), 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "DrawnVobs", &rendererInfo.FrameDrawnVobs, 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "DrawnTriangles", &rendererInfo.FrameDrawnTriangles, 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "VobUpdates", &rendererInfo.FrameVobUpdates, 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "DrawnLights", &rendererInfo.FrameDrawnLights, 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SectionsDrawn", &rendererInfo.FrameNumSectionsDrawn, 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "WorldMeshDrawCalls", &rendererInfo.WorldMeshDrawCalls, 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputFloat( "FarPlane", &rendererInfo.FarPlane, 1, 100, "%.0f", ImGuiInputTextFlags_ReadOnly );
        ImGui::InputFloat( "NearPlane", &rendererInfo.NearPlane, 1, 100, "%.0f", ImGuiInputTextFlags_ReadOnly );
        ImGui::InputFloat( "WorldMeshMS", &rendererInfo.Timing.WorldMeshMS, 1, 100, "%.6f", ImGuiInputTextFlags_ReadOnly );
        ImGui::InputFloat( "VobsMS", &rendererInfo.Timing.VobsMS, 1, 100, "%.6f", ImGuiInputTextFlags_ReadOnly );
        ImGui::InputFloat( "SkeletalMeshesMS", &rendererInfo.Timing.SkeletalMeshesMS, 1, 100, "%.6f", ImGuiInputTextFlags_ReadOnly );
        ImGui::InputFloat( "LightingMS", &rendererInfo.Timing.LightingMS, 1, 100, "%.6f", ImGuiInputTextFlags_ReadOnly );
        ImGui::InputFloat( "TotalMS", &rendererInfo.Timing.TotalMS, 1, 100, "%.6f", ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_PipelineStates", (int*)&rendererInfo.FramePipelineStates, 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_Textures", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_TX], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_ConstantBuffer", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_CB], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_GeometryShader", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_GS], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_RTVDSV", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_RTVDSV], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_DomainShader", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_DS], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_HullShader", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_HS], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_PixelShader", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_PS], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_InputLayout", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_IL], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_VertexShader", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_VS], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_IndexBuffer", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_IB], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_VertexBuffer", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_VB], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_RasterizerState", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_RS], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_DepthStencilState", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_DSS], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_SamplerState", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_SMPL], 1, 100, ImGuiInputTextFlags_ReadOnly );
        ImGui::InputInt( "SC_BlendState", (int*)&rendererInfo.StateChangesByState[GothicRendererInfo::SC_BS], 1, 100, ImGuiInputTextFlags_ReadOnly );

    }
    ImGui::End();
}

void RenderAdvancedColumn4( GothicRendererSettings& settings, GothicAPI* gapi ) {
    if ( ImGui::Begin( "HBAO+", nullptr, ImGuiWindowFlags_NoCollapse ) ) {
        ImGui::Checkbox( "Enable HBAO+", &settings.HbaoSettings.Enabled );
        ImGui::DragFloat( "Radius", &settings.HbaoSettings.Radius, 0.01f );
        ImGui::DragFloat( "MetersToViewSpaceUnits", &settings.HbaoSettings.MetersToViewSpaceUnits, 0.01f );
        if ( ImGui::DragFloat( "PowerExponent", &settings.HbaoSettings.PowerExponent, 0.01f ) ) {
            settings.HbaoSettings.PowerExponent = std::clamp( settings.HbaoSettings.PowerExponent, 1.0f, 4.0f );
        }
        if ( ImGui::DragFloat( "Bias", &settings.HbaoSettings.Bias, 0.01f ) ) {
            settings.HbaoSettings.Bias = std::clamp( settings.HbaoSettings.Bias, 0.0f, 0.5f );
        }
        ImGui::Checkbox( "Enable Blur", &settings.HbaoSettings.EnableBlur );
        static std::vector<std::pair<char*, int>> ssaoRadi = { {"2", 0}, {"4", 1} };
        if ( ImComboBox( "SSAO radius", ssaoRadi, &settings.HbaoSettings.SsaoBlurRadius ) ) {
            ImGui::EndCombo();
        }
        ImGui::DragFloat( "BlurSharpness", &settings.HbaoSettings.BlurSharpness, 0.01f );
        static std::vector<std::pair<char*, int>> blendMode = { {"Replace", 0}, {"Multiply", 1} };
        if ( ImComboBox( "BlendMode", blendMode, &settings.HbaoSettings.BlendMode ) ) {
            ImGui::EndCombo();
        }

        static std::vector<std::pair<char*, int>> stepCount = { {"4", 0}, {"8", 1} };
        if ( ImComboBox( "SSAO steps", stepCount, &settings.HbaoSettings.SsaoStepCount ) ) {
            ImGui::EndCombo();
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
    auto columnWidth = windowSize.x / 4;
    auto columnOffset = 0.0f;
    auto columnHeight = std::max( 400.0f, windowSize.y / 2.f );

    GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;

    ImGui::SetNextWindowPos( ImVec2( columnOffset, 0.0f ), ImGuiCond_Appearing, ImVec2( 0, 0 ) );
    ImGui::SetNextWindowSize( ImVec2( windowSize.x / 4, columnHeight ), ImGuiCond_Appearing );
    RenderAdvancedColumn1( settings, Engine::GAPI );
    columnOffset += columnWidth;

    ImGui::SetNextWindowPos( ImVec2( columnOffset, 0.0f ), ImGuiCond_Appearing, ImVec2( 0, 0 ) );
    ImGui::SetNextWindowSize( ImVec2( windowSize.x / 4, columnHeight ), ImGuiCond_Appearing );
    RenderAdvancedColumn2( settings, Engine::GAPI );
    columnOffset += columnWidth;

    ImGui::SetNextWindowPos( ImVec2( columnOffset, 0.0f ), ImGuiCond_Appearing, ImVec2( 0, 0 ) );
    ImGui::SetNextWindowSize( ImVec2( windowSize.x / 4, columnHeight ), ImGuiCond_Appearing );
    RenderAdvancedColumn3( settings, Engine::GAPI );
    columnOffset += columnWidth;

    ImGui::SetNextWindowPos( ImVec2( columnOffset, 0.0f ), ImGuiCond_Appearing, ImVec2( 0, 0 ) );
    ImGui::SetNextWindowSize( ImVec2( windowSize.x / 4, columnHeight ), ImGuiCond_Appearing );
    RenderAdvancedColumn4( settings, Engine::GAPI );

}
