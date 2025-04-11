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
    io.MouseDrawCursor = true;
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
    char path[MAX_PATH];
    GetCurrentDirectoryA( MAX_PATH, path );
    std::string fontpath = std::string( path ) + "\\system\\GD3D11\\Fonts\\Lato-Semibold.ttf";
    io.Fonts->AddFontFromFileTTF( fontpath.c_str(), 22.0f, &config );
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
    if ( SettingsVisible )
        RenderSettingsWindow();
    //if ( DemoVisible )
    //    ImGui::ShowDemoWindow();
    ImGui::GetIO().MouseDrawCursor = IsActive;
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
}

void ImGuiShim::RenderSettingsWindow()
{
    // Autosized settings by child objects & centered
    IM_ASSERT( ImGui::GetCurrentContext() != NULL && "Missing Dear ImGui context!" );
    IMGUI_CHECKVERSION();

    // Get the center point of the screen, then shift the window by 50% of its size in both directions.
    // TIP: Don't use ImGui::GetMainViewport for framebuffer sizes since GD3D11 can undersample or oversample the game.
    // Use whatever the resolution is spit out instead.
    ImGui::SetNextWindowPos( ImVec2(CurrentResolution.x / 2, CurrentResolution.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if ( ImGui::Begin( "Settings", false, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize ) ) {
        ImVec2 buttonWidth( 275, 0 );
        // eh maybe i clean this later... OR I WONT
        {
            ImGui::BeginGroup();
            ImGui::Checkbox( "Vsync", &Engine::GAPI->GetRendererState().RendererSettings.EnableVSync );
            ImGui::Checkbox( "NormalMaps", &Engine::GAPI->GetRendererState().RendererSettings.AllowNormalmaps );
            ImGui::Checkbox( "HBAO+", &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.Enabled );
            ImGui::Checkbox( "Godrays", &Engine::GAPI->GetRendererState().RendererSettings.EnableGodRays );
            ImGui::Checkbox( "SMAA", &Engine::GAPI->GetRendererState().RendererSettings.EnableSMAA );
            ImGui::Checkbox( "HDR", &Engine::GAPI->GetRendererState().RendererSettings.EnableHDR );
            ImGui::Checkbox( "Shadows", &Engine::GAPI->GetRendererState().RendererSettings.EnableShadows );
            ImGui::Checkbox( "Shadow filtering", &Engine::GAPI->GetRendererState().RendererSettings.EnableSoftShadows );
            ImGui::Checkbox( "Compress Backbuffer", &Engine::GAPI->GetRendererState().RendererSettings.CompressBackBuffer );
            ImGui::Checkbox( "Animate Static Vobs", &Engine::GAPI->GetRendererState().RendererSettings.AnimateStaticVobs );
            ImGui::Checkbox( "Enable Rain", &Engine::GAPI->GetRendererState().RendererSettings.EnableRain );
            ImGui::Checkbox( "Enable Rain Effects", &Engine::GAPI->GetRendererState().RendererSettings.EnableRainEffects );
            ImGui::Checkbox( "Limit Light Intensity", &Engine::GAPI->GetRendererState().RendererSettings.LimitLightIntensity );
            ImGui::Checkbox( "Draw World Section Intersections", &Engine::GAPI->GetRendererState().RendererSettings.DrawSectionIntersections );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "This option draws every world chunk that intersect with GD3D11 world draw distance." );

            ImGui::Checkbox( "Occlusion Culling", &Engine::GAPI->GetRendererState().RendererSettings.EnableOcclusionCulling );
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
            std::vector<std::string> QualityEnums = { "Potato", "Ultra Low", "Low", "Medium", "High", "Ultra High" };
            //yep, i agree, this is retarded
            if ( Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize <= 32 ) {
                TextureQualityState = 0;
            } else if ( Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize <= 64 ) {
                TextureQualityState = 1;
            } else if ( Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize <= 128 ) {
                TextureQualityState = 2;
            } else if ( Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize <= 256 ) {
                TextureQualityState = 3;
            } else if ( Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize <= 512 ) {
                TextureQualityState = 4;
            } else if ( Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize <= 16384 ) {
                TextureQualityState = 5;
            }
            if ( ImGui::BeginCombo( "##TortureQuality", QualityEnums[TextureQualityState].c_str() ) ) {

                for ( size_t i = 0; i < QualityEnums.size(); i++ ) {
                    bool Selected = (TextureQualityState == i);

                    if ( ImGui::Selectable( QualityEnums[i].c_str(), Selected ) ) {
                        TextureQualityState = i;
                        switch ( TextureQualityState ) {
                        case 0:
                            Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize = 32;
                            Engine::GAPI->UpdateTextureMaxSize();
                            break;
                        case 1:
                            Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize = 64;
                            Engine::GAPI->UpdateTextureMaxSize();
                            break;
                        case 2:
                            Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize = 128;
                            Engine::GAPI->UpdateTextureMaxSize();
                            break;
                        case 3:
                            Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize = 256;
                            Engine::GAPI->UpdateTextureMaxSize();
                            break;
                        case 4:
                            Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize = 512;
                            Engine::GAPI->UpdateTextureMaxSize();
                            break;
                        default:
                        case 5:
                            Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize = 16384;
                            Engine::GAPI->UpdateTextureMaxSize();
                            break;
                        }
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
                        Engine::GAPI->GetRendererState().RendererSettings.WindowMode = i;
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
            std::vector<std::string> kwality = { "512", "1024", "2048", "4096", "8192" };
            if ( !FeatureLevel10Compatibility ) //Not sure if imgui will work on level10 with dx11 impl, idc
                kwality.emplace_back( "16384" );

            if ( Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize <= 512 ) {
                ShadowQualityState = 0;
            } else if ( Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize <= 1024 ) {
                ShadowQualityState = 1;
            } else if ( Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize <= 2048 ) {
                ShadowQualityState = 2;
            } else if ( Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize <= 4096 ) {
                ShadowQualityState = 3;
            } else if ( Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize <= 8192 ) {
                ShadowQualityState = 4;
            } else if ( Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize <= 16384 ) {
                ShadowQualityState = 5;
            }
            if ( ImGui::BeginCombo( "##ShadowQuality", kwality[ShadowQualityState].c_str() ) ) {
                for ( size_t i = 0; i < kwality.size(); i++ ) {
                    bool Selected = (ShadowQualityState == i);

                    if ( ImGui::Selectable( kwality[i].c_str(), Selected ) ) {
                        ShadowQualityState = i;
                        switch ( ShadowQualityState ) {
                        default:
                        case 0:
                            Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = 512;
                            ReloadShaders = true;
                            break;
                        case 1:
                            Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = 1024;
                            ReloadShaders = true;
                            break;
                        case 2:
                            Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = 2048;
                            ReloadShaders = true;
                            break;
                        case 3:
                            Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = 4096;
                            ReloadShaders = true;
                            break;
                        case 4:
                            Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = 8192;
                            ReloadShaders = true;
                            break;
                        case 5:
                            Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize = 16384;
                            ReloadShaders = true;
                            break;
                        }
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
            DynamicShadowState = static_cast<int>(Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows);
            if ( ImGui::BeginCombo( "##DynamicShadows", DynamicShadowEnums[DynamicShadowState].c_str() ) ) {
                for ( size_t i = 0; i < DynamicShadowEnums.size(); i++ ) {
                    bool Selected = (DynamicShadowState == i);

                    if ( ImGui::Selectable( DynamicShadowEnums[i].c_str(), Selected ) ) {
                        DynamicShadowState = i;
                        Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows = static_cast<GothicRendererSettings::EPointLightShadowMode>(i);
                    }

                    if ( Selected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "FPS Limit", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            ImGui::SliderInt( "##FPSLimit", &Engine::GAPI->GetRendererState().RendererSettings.FpsLimit, 10, 300 );

            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Object Draw Distance", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            float objectDrawDistance = Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius / 1000.0f;
            if ( ImGui::SliderFloat( "##OutdoorVobDrawRadius", &objectDrawDistance, 2.f, 100.0f ) ) {
                Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius = static_cast<float>(objectDrawDistance * 1000.0f);
            }

            float smallObjectDrawDistance = Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius / 1000.0f;
            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Small Object Draw Distance", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            if ( ImGui::SliderFloat( "##OutdoorSmallVobDrawRadius", &smallObjectDrawDistance, 2.f, 100.0f ) ) {
                Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius = static_cast<float>(smallObjectDrawDistance * 1000.0f);
            }

            float visualFXDrawDistance = Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius / 1000.0f;
            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "VisualFX Draw Distance", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            if ( ImGui::SliderFloat( "##VisualFXDrawRadius", &visualFXDrawDistance, 0.1f, 10.0f ) ) {
                Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius = static_cast<float>(visualFXDrawDistance * 1000.0f);
            }
            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "World Draw Distance", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            ImGui::SliderInt( "##SectionDrawRadius", &Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius, 2, 20 );

            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Contrast", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            ImGui::SliderFloat( "##Contrast", &Engine::GAPI->GetRendererState().RendererSettings.GammaValue, 0.1f, 2.0f );

            ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.f, 0.5f ) );
            ImGui::Button( "Brightness", buttonWidth ); ImGui::SameLine();
            ImGui::PopStyleVar();
            ImGui::SliderFloat( "##Brightness", &Engine::GAPI->GetRendererState().RendererSettings.BrightnessValue, 0.1f, 3.0f );
            ImGui::PopItemWidth();

            ImGui::EndGroup();
        }

        if ( ImGui::Button( "OK", ImVec2( ImGui::GetContentRegionAvail().x, 30.f ) ) ) { //nikt nie bedzie z tego strzelac
            SettingsVisible = false;
            IsActive = false;
            Engine::GAPI->SetEnableGothicInput( true );
            Engine::GAPI->SaveRendererWorldSettings( Engine::GAPI->GetRendererState().RendererSettings );
            Engine::GAPI->SaveMenuSettings( MENU_SETTINGS_FILE );
            if ( ReloadShaders ) {
                Engine::GraphicsEngine->ReloadShaders();
                ReloadShaders = false;
            }
        }
    }
    ImGui::End();

}

