#include "ImGuiShim.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

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

    Initiated = true;

    auto& Displaylist = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetDisplayModeList();
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
    ImFontConfig config;
    config.MergeMode = false;
    //config.GlyphRanges = euroGlyphRanges;
    const auto path = std::filesystem::current_path();
    const auto fontpath = path / "system" / "GD3D11" / "Fonts" / "Lato-Semibold.ttf";
    io.Fonts->AddFontFromFileTTF( fontpath.string().c_str(), 22.0f, &config);
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
    if ( SettingsVisible ) {
        RenderSettingsWindow();
    }
    //if ( DemoVisible )
    //    ImGui::ShowDemoWindow();
    //ImGui::GetIO().MouseDrawCursor = IsActive;
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
    if ( CurrentResolution.x >= 2160 ) {
        ImGui::GetIO().FontGlobalScale = 1.30f;
    } else if ( CurrentResolution.x < 1024 && CurrentResolution.y < 768 ) {
        ImGui::GetIO().FontGlobalScale = 0.80f;  // Scale down for smaller resolutions
    } else {
        ImGui::GetIO().FontGlobalScale = 1.0f;
    }

    // Get the center point of the screen, then shift the window by 50% of its size in both directions.
    // TIP: Don't use ImGui::GetMainViewport for framebuffer sizes since GD3D11 can undersample or oversample the game.
    // Use whatever the resolution is spit out instead.
    ImGui::SetNextWindowPos( ImVec2( CurrentResolution.x / 2, CurrentResolution.y / 2 ), ImGuiCond_Always, ImVec2( 0.5f, 0.5f ) );
}

void ImGuiShim::RenderSettingsWindow()
{
    // Autosized settings by child objects & centered
    IM_ASSERT( ImGui::GetCurrentContext() != NULL && "Missing Dear ImGui context!" );
    IMGUI_CHECKVERSION();

    if ( ImGui::Begin( "Settings", false, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize ) ) {
        GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;
        ImVec2 buttonWidth( 275, 0 );
        // eh maybe i clean this later... OR I WONT
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
            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Resolution", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            if ( ImGui::BeginCombo( "##Resolution", Resolutions[ResolutionState].c_str() ) ) {
                for ( size_t i = 0; i < Resolutions.size(); i++ ) {
                    bool Selected = (ResolutionState == i);

                    if ( ImGui::Selectable( Resolutions[i].c_str(), Selected ) ) {
                        ResolutionState = i;
                        Engine::GraphicsEngine->OnResize( INT2( Resolutions[i] ) );
                        Engine::GraphicsEngine->ReloadShaders();
                    }

                    if ( Selected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Texture Quality", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
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
                    bool Selected = (TextureQualityState == i);

                    if ( ImGui::Selectable( QualityOptions[i].first.c_str(), Selected ) ) {
                        TextureQualityState = i;
                        settings.textureMaxSize = QualityOptions[i].second;
                        Engine::GAPI->UpdateTextureMaxSize();
                    }

                    if ( Selected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            std::vector<std::string> DisplayEnums = {
                "Fullscreen Borderless",
                "Fullscreen Exclusive",
                "Fullscreen Lowlatency",
                "Windowed"
            };
            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Display Mode [*]", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            DisplayModeState = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine)->GetWindowMode();
            if ( ImGui::BeginCombo( "##DisplayMode", DisplayEnums[DisplayModeState].c_str() ) ) {
                for ( size_t i = 0; i < DisplayEnums.size(); i++ ) {
                    bool Selected = (DisplayModeState == i);
                    if ( ImGui::Selectable( DisplayEnums[i].c_str(), Selected ) ) {
                        DisplayModeState = i;
                        settings.WindowMode = i;
                    }
                    if ( ImGui::IsItemHovered() ) {
                        ImGui::SetTooltip( "[*] You need to restart for this to take effect." );
                    }
                    if ( Selected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Shadow Quality", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            std::vector<std::pair<std::string, int>> shadowResolution = { { "very low", 512 }, { "low", 1024 }, { "medium", 2048 }, { "high", 4096 }, { "very high", 8192 } };
            if ( !FeatureLevel10Compatibility ) //Not sure if imgui will work on level10 with dx11 impl, idc
                shadowResolution.emplace_back( std::make_pair<std::string, int>( "ultra high", 16384 ) );

            for ( int i = shadowResolution.size() - 1; i >= 0; i-- ) {
                if ( settings.ShadowMapSize >= shadowResolution.at( i ).second ) {
                    ShadowQualityState = i;
                    break;
                }
            }

            if ( ImGui::BeginCombo( "##ShadowQuality", shadowResolution[ShadowQualityState].first.c_str() ) ) {
                for ( size_t i = 0; i < shadowResolution.size(); i++ ) {
                    bool Selected = (ShadowQualityState == i);

                    if ( ImGui::Selectable( shadowResolution[i].first.c_str(), Selected ) ) {
                        ShadowQualityState = i;
                        settings.ShadowMapSize = shadowResolution[i].second;
                        Engine::GraphicsEngine->ReloadShaders();
                    }
                    if ( ImGui::IsItemHovered() ) {
                        ImGui::SetTooltip( std::to_string( shadowResolution[i].second ).c_str() );
                    }

                    if ( Selected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Dynamic Shadows", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            std::vector<std::string> DynamicShadowEnums = { "Off", "Static", "Dynamic Update", "Full" };
            DynamicShadowState = static_cast<int>(settings.EnablePointlightShadows);
            if ( ImGui::BeginCombo( "##DynamicShadows", DynamicShadowEnums[DynamicShadowState].c_str() ) ) {
                for ( size_t i = 0; i < DynamicShadowEnums.size(); i++ ) {
                    bool Selected = (DynamicShadowState == i);

                    if ( ImGui::Selectable( DynamicShadowEnums[i].c_str(), Selected ) ) {
                        DynamicShadowState = i;
                        settings.EnablePointlightShadows = static_cast<GothicRendererSettings::EPointLightShadowMode>( i );
                    }

                    if ( Selected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            static bool fpsLimitEnabled = 0;
            fpsLimitEnabled = settings.FpsLimit > 0;

            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            if ( ImGui::Button( fpsLimitEnabled ? "[x] FPS Limit" : "[ ] FPS Limit", buttonWidth ) ) {
                fpsLimitEnabled = !fpsLimitEnabled;
                if ( !fpsLimitEnabled ) {
                    settings.FpsLimit = 0;
                } else {
                    settings.FpsLimit = 60;
                }
            }
            ImGui::SameLine();
            ImGui::PopStyleVar();

            ImGui::BeginDisabled( !fpsLimitEnabled );
            ImGui::SliderInt( "##FPSLimit", &settings.FpsLimit, 10, 300 );
            ImGui::EndDisabled();

            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Object Draw Distance", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            float objectDrawDistance = settings.OutdoorVobDrawRadius / 1000.0f;
            if ( ImGui::SliderFloat( "##OutdoorVobDrawRadius", &objectDrawDistance, 2.f, 100.0f ) ) {
                settings.OutdoorVobDrawRadius = static_cast<float>(objectDrawDistance * 1000.0f);
            }

            float smallObjectDrawDistance = settings.OutdoorSmallVobDrawRadius / 1000.0f;
            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Small Object Draw Distance", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            if ( ImGui::SliderFloat( "##OutdoorSmallVobDrawRadius", &smallObjectDrawDistance, 2.f, 100.0f ) ) {
                settings.OutdoorSmallVobDrawRadius = static_cast<float>(smallObjectDrawDistance * 1000.0f);
            }

            float visualFXDrawDistance = settings.VisualFXDrawRadius / 1000.0f;
            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "VisualFX Draw Distance", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            if ( ImGui::SliderFloat( "##VisualFXDrawRadius", &visualFXDrawDistance, 0.1f, 10.0f ) ) {
                settings.VisualFXDrawRadius = static_cast<float>(visualFXDrawDistance * 1000.0f);
            }
            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "World Draw Distance", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            ImGui::SliderInt( "##SectionDrawRadius", &settings.SectionDrawRadius, 2, 20 );

            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Contrast", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            ImGui::SliderFloat( "##Contrast", &settings.GammaValue, 0.1f, 2.0f );

            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Brightness", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            ImGui::SliderFloat( "##Brightness", &settings.BrightnessValue, 0.1f, 3.0f );
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

