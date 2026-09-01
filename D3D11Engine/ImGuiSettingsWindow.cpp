#include "ImGuiSettingsWindow.h"
#include "ImGuiShim.h"
#include "ImGuiWidgets.h"

#include "Engine.h"
#include "GothicAPI.h"
#include "BaseGraphicsEngine.h"
#include "ConstantBufferStructs.h"
#include "Toolbox.h"

#include <sstream>

#if defined(BUILD_GOTHIC_1_CLASSIC)
extern bool haveWindAnimations;
#endif

namespace {
    /** Width of the label column every row starts with. */
    constexpr float LabelWidth = 250.0f;

    /** Two-column settings layout: the control on the left, the preview image for whatever it is set
        to on the right. The preview column is only reserved once a table has actually drawn one -
        most installs ship no preview images, and an empty column would just eat the window width. */
    class SettingsTable {
    public:
        bool Begin( const char* id ) {
            m_State = &s_States[id];
            m_Columns = m_State->Reserve ? 2 : 1;
            m_State->Drawn = false;

            m_Open = ImGui::BeginTable( id, m_Columns, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV );
            if ( m_Open ) {
                ImGui::TableSetupColumn( "Settings", ImGuiTableColumnFlags_WidthStretch );
                if ( m_Columns == 2 ) {
                    ImGui::TableSetupColumn( "Preview", ImGuiTableColumnFlags_WidthFixed, ImPreview::Size );
                }
            }
            return m_Open;
        }

        void End() {
            if ( !m_Open ) {
                return; // the table never drew, so its rows say nothing about what to reserve
            }
            ImGui::EndTable();
            m_State->Reserve = m_State->Drawn;
            m_Open = false;
        }

        /** Starts a row, leaving the cursor in the control column. */
        void Row() {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
        }

        /** Fills the row's preview column, if this table has one. */
        void Preview( std::string_view name, std::string_view hover = {} ) {
            if ( name.empty() ) {
                return;
            }
            if ( m_Columns == 2 ) {
                ImGui::TableNextColumn();
                m_State->Drawn |= ImPreview::Show( name, hover );
            } else {
                m_State->Drawn |= ImPreview::Exists( name );
            }
        }

        void Toggle( const char* preview, bool value ) {
            if ( !preview ) {
                return;
            }
            const std::string on = std::string( preview ) + "_On";
            const std::string off = std::string( preview ) + "_Off";
            Preview( value ? on : off, value ? off : on );
        }

    private:
        struct State {
            bool Reserve = false;   // did this table draw a preview last frame?
            bool Drawn = false;     // ... and this one?
        };
        static inline std::unordered_map<std::string_view, State> s_States;

        State* m_State = nullptr;
        bool m_Open = false;
        int m_Columns = 1;
    };

    /** Draws the label cell and leaves the next item stretched across the rest of the control column. */
    void Label( const char* label, const char* tooltip ) {
        ImText( label, ImVec2( LabelWidth, 0 ) );
        if ( tooltip ) {
            ImGui::SetItemTooltip( "%s", tooltip );
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth( -FLT_MIN );
    }

    void SectionRow( SettingsTable& table, const char* title ) {
        table.Row();
        ImGui::SeparatorText( title );
    }

    /** Label + combo + preview row. The preview follows the selected entry (its explicit name, or the
        enum member's) and swaps to the FIRST entry's on hover - list the "off"/lowest option first. */
    template <typename T, size_t N>
    void ComboRow( SettingsTable& table, const char* label, const char* id, const ListItem<T>( &items )[N],
        T* storage, const char* tooltip = nullptr, const std::move_only_function<void() const>& changed = {} ) {
        table.Row();
        Label( label, tooltip );
        if ( ImComboBox( id, items, storage, changed ) ) {
            ImGui::EndCombo();
        }
        table.Preview( PreviewNameOf( items, N, *storage ), PreviewNameOf( items[0] ) );
    }

    /** Label + checkbox row. `preview` names a "<preview>_On" / "<preview>_Off" image pair. */
    bool CheckRow( SettingsTable& table, const char* label, bool* value, const char* tooltip = nullptr,
        const char* preview = nullptr ) {
        table.Row();
        ImText( label, ImVec2( LabelWidth, 0 ) );
        if ( tooltip ) {
            ImGui::SetItemTooltip( "%s", tooltip );
        }
        ImGui::SameLine();
        ImGui::PushID( label );
        const bool changed = ImGui::Checkbox( "##Value", value );
        ImGui::PopID();
        table.Toggle( preview, *value );
        return changed;
    }

    bool SliderIntRow( SettingsTable& table, const char* label, const char* id, int* value, int min, int max,
        const char* format = "%d", const char* tooltip = nullptr ) {
        table.Row();
        Label( label, tooltip );
        return ImGui::SliderInt( id, value, min, max, format, ImGuiSliderFlags_::ImGuiSliderFlags_AlwaysClamp );
    }

    bool SliderFloatRow( SettingsTable& table, const char* label, const char* id, float* value, float min, float max,
        const char* format = "%.2f", const char* tooltip = nullptr ) {
        table.Row();
        Label( label, tooltip );
        return ImGui::SliderFloat( id, value, min, max, format, ImGuiSliderFlags_::ImGuiSliderFlags_AlwaysClamp );
    }

    /** A draw distance, edited in meters but stored in Gothic units (100 = 1m). */
    void DistanceRow( SettingsTable& table, const char* label, const char* id, float* value, float min, float max,
        const char* tooltip = nullptr ) {
        float meters = *value / 100.0f;
        if ( SliderFloatRow( table, label, id, &meters, min, max, "%.0f m", tooltip ) ) {
            *value = meters * 100.0f;
        }
    }

    bool IsD3D12() {
        // Ask the ENGINE, not settings.GraphicsAPI - that one is the REQUESTED api, which a failed init
        // falls back from without a restart.
        return Engine::IsD3D12Backend;
    }

    void ReloadD3D11Shaders( ShaderCategory category ) {
        if ( Engine::GraphicsEngine->GetBackendAPI() == EGraphicsEngineBackend::D3D11 ) {
            Engine::GraphicsEngine->ReloadShaders( category );
        }
    }
}

void ImGuiSettings::FixupSettings( GothicRendererSettings& s ) {
    if ( s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR ) {
        if ( s.Upscaler != GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3 ) {
            s.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3;
        }
    }
    if ( s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_TAA
        && ( s.Upscaler == GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3 ) ) {
        // don't allow TAA and FSR2 at the same time.
        s.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_1;
    }
    if ( s.ResolutionScalePercent > 100 && s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR ) {
        // switch to regular TAA if upsampled
        s.AntiAliasingMode = GothicRendererSettings::AA_TAA;
    }

    // MSAA (Forward+ only) and TAA/FSR are mutually exclusive: both do their own edge/temporal
    // resolve and combining them adds no value while doubling the resolve complexity.
    if ( s.MSAASamples > 1 && ( s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_TAA
        || s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR ) ) {
        s.AntiAliasingMode = GothicRendererSettings::E_AntiAliasingMode::AA_NONE;
    }
    if ( s.RendererMode != GothicRendererSettings::E_RendererMode::RM_ForwardPlus ) {
        s.MSAASamples = 1;
    }
}

namespace {

void RenderDisplayTab( ImGuiShim& shim, GothicRendererSettings& settings ) {
    if ( !ImGui::BeginTabItem( "Display" ) ) {
        return;
    }

    SettingsTable table;
    if ( table.Begin( "##TblDisplay" ) ) {
        for ( size_t i = 0; i < shim.Resolutions.size(); ++i ) {
            if ( shim.Resolutions[i].first == shim.CurrentResolution ) {
                shim.ResolutionState = i;
                break;
            }
        }

        static std::string resolutionLabel = "Resolution";
        if ( settings.ResolutionScalePercent != 100 ) {
            std::stringstream ss;
            ss << "Resolution (scaled: " << ( shim.CurrentResolution.x * settings.ResolutionScalePercent / 100 )
                << " x " << ( shim.CurrentResolution.y * settings.ResolutionScalePercent / 100 ) << ")";
            resolutionLabel = ss.str();
        }

        table.Row();
        Label( settings.ResolutionScalePercent != 100 ? resolutionLabel.c_str() : "Resolution", nullptr );
        if ( !shim.Resolutions.empty() && ImGui::BeginCombo( "##Resolution", shim.Resolutions[shim.ResolutionState].second.c_str() ) ) {
            for ( size_t i = 0; i < shim.Resolutions.size(); i++ ) {
                const bool isSelected = ( shim.ResolutionState == i );
                if ( ImGui::Selectable( shim.Resolutions[i].second.c_str(), isSelected ) ) {
                    Engine::GraphicsEngine->TriggerResize( shim.Resolutions[i].first );
                }
                if ( isSelected ) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        constexpr ListItem<WindowModes> displayModes[] = {
            { "Fullscreen Borderless", WindowModes::WINDOW_MODE_FULLSCREEN_BORDERLESS },
            { "Fullscreen Lowlatency", WindowModes::WINDOW_MODE_FULLSCREEN_LOWLATENCY, "Switching requires restarting the game." },
            { "Fullscreen Exclusive", WindowModes::WINDOW_MODE_FULLSCREEN_EXCLUSIVE, "Switching requires restarting the game." },
            { "Windowed", WindowModes::WINDOW_MODE_WINDOWED },
        };
        // ChangeWindowPreset is a request that GothicAPI clears once applied, so the combo keeps its own
        // copy of what the player picked.
        static WindowModes displayMode = ImGuiShim::InterpretWindowMode( settings );
        ComboRow( table, "Display Mode [*]", "##DisplayMode", displayModes, &displayMode,
            "Some changes require a restart.", [&settings] { settings.ChangeWindowPreset = displayMode; } );

        constexpr ListItem<GothicRendererSettings::E_GraphicsAPI> graphicsApis[] = {
            { "Direct3D 11", GothicRendererSettings::GRAPHICS_API_D3D11 },
            { "Direct3D 12", GothicRendererSettings::GRAPHICS_API_D3D12, "Falls back to Direct3D 11 if the device can't be created." },
        };
        ComboRow( table, "Graphics API [*]", "##GraphicsAPI", graphicsApis, &settings.GraphicsAPI,
            "Takes effect after restarting the game." );

        CheckRow( table, "V-Sync", &settings.EnableVSync );

        table.Row();
        ImText( "FPS Limit", ImVec2( LabelWidth, 0 ) );
        ImGui::SameLine();
        bool fpsLimitEnabled = settings.FpsLimit > 0;
        if ( ImGui::Checkbox( "##EnableFpsLimit", &fpsLimitEnabled ) ) {
            settings.FpsLimit = fpsLimitEnabled ? 60 : 0;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled( !fpsLimitEnabled );
        ImGui::SetNextItemWidth( -FLT_MIN );
        ImGui::SliderInt( "##FpsLimit", &settings.FpsLimit, 10, 300 );
        ImGui::EndDisabled();

        // Always on by design - Gothic renders paused frames from its own unthrottled loop, and letting
        // that run free crashes some drivers. Only the value is up to the player.
        SliderIntRow( table, "Paused FPS Limit", "##PausedFpsLimit", &settings.PausedFpsLimit,
            GothicRendererSettings::PausedFpsLimitMin, GothicRendererSettings::PausedFpsLimitMax, "%d",
            "Framerate while an in-game menu has the game paused.\n"
            "Gothic renders those frames from its own loop without any limit, which can\n"
            "reach thousands of FPS and crash some drivers, so this cap can't be turned off." );

        SectionRow( table, "Render Resolution" );

        // D3D12 has the FSR 3 temporal upscaler (D3D12Fsr3.cpp) but no FSR 1 spatial one.
        const bool noFsr1 = IsD3D12();

        table.Row();
        Label( "Resolution Scale", nullptr );
        if ( settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3 ) {
            settings.ResolutionScalePercent = std::clamp( settings.ResolutionScalePercent, 33, 100 );
            // Display "levels" as typical for FSR
            constexpr ListItem<int> fsrLevels[] = {
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
        } else {
            ImGui::SliderInt( "##ResolutionScalePercent", &settings.ResolutionScalePercent, 25, 200, "%d %%",
                ImGuiSliderFlags_::ImGuiSliderFlags_AlwaysClamp );
        }
        ImGui::SetItemTooltip( "Effective resolution: %d x %d",
            shim.CurrentResolution.x * settings.ResolutionScalePercent / 100,
            shim.CurrentResolution.y * settings.ResolutionScalePercent / 100 );

        constexpr ListItem<GothicRendererSettings::E_Upscaler> upscalers[] = {
            { "Simple", GothicRendererSettings::E_Upscaler::UPSCALER_DEFAULT },
            { "FSR 1", GothicRendererSettings::E_Upscaler::UPSCALER_FSR_1 },
            { "FSR 3", GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3 },
        };
        constexpr ListItem<GothicRendererSettings::E_Upscaler> upscalersNoFsr1[] = {
            { "Simple", GothicRendererSettings::E_Upscaler::UPSCALER_DEFAULT },
            { "FSR 3", GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3 },
        };
        // A stored FSR 1 choice must survive a switch back to D3D11, so it is NOT written back here - it
        // simply behaves as "Simple" while the combo shows nothing selected.
        if ( noFsr1 ) {
            ComboRow( table, "Upscaler", "##Upscaler", upscalersNoFsr1, &settings.Upscaler,
                "FSR 1 needs the Direct3D 11 backend." );
        } else {
            ComboRow( table, "Upscaler", "##Upscaler", upscalers, &settings.Upscaler );
        }

        ImGui::BeginDisabled( settings.ResolutionScalePercent >= 100 || !settings.Upscaler );
        SliderFloatRow( table, "Upscaler Sharpening", "##SharpenFactor", &settings.SharpenFactor, 0.0f, 1.0f, "%.2f" );
        ImGui::EndDisabled();

        SectionRow( table, "Image" );

        constexpr ListItem<GothicRendererSettings::E_AntiAliasingMode> antiAliasing[] = {
            { "Disabled", GothicRendererSettings::E_AntiAliasingMode::AA_NONE },
            { "SMAA", GothicRendererSettings::E_AntiAliasingMode::AA_SMAA },
            { "TAA", GothicRendererSettings::E_AntiAliasingMode::AA_TAA, "Temporal Anti-Aliasing" },
            { "FSR 3", GothicRendererSettings::E_AntiAliasingMode::AA_FSR, "FidelityFX Super Resolution 3" },
        };
        ComboRow( table, "Anti-Aliasing", "##AntiAliasing", antiAliasing, &settings.AntiAliasingMode, nullptr,
            [&settings] {
                if ( settings.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR ) {
                    settings.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3;
                }
            } );

        if ( settings.RendererMode == GothicRendererSettings::RM_ForwardPlus ) {
            constexpr ListItem<int> msaaSamples[] = {
                { "Off", 1, nullptr, "MSAA_Off" },
                { "2x",  2, nullptr, "MSAA_2x" },
                { "4x",  4, nullptr, "MSAA_4x" },
                { "8x",  8, nullptr, "MSAA_8x" },
            };
            ComboRow( table, "MSAA", "##MSAA", msaaSamples, &settings.MSAASamples,
                "Forward+ only: hardware multisample anti-aliasing for opaque geometry.\n"
                "Mutually exclusive with TAA/FSR." );
        }

        SliderFloatRow( table, "Brightness", "##Brightness", &settings.BrightnessValue, 0.10f, 3.0f );
        SliderFloatRow( table, "Contrast", "##Contrast", &settings.GammaValue, 0.20f, 2.0f );

        SectionRow( table, "HDR Display Output" );

        CheckRow( table, "HDR Monitor Output [*]", &settings.HDR_Monitor,
            "Real HDR scanout. Needs the Direct3D 12 backend and an HDR-enabled display.\n"
            "The swapchain colour space is decided at startup, so this takes effect after a restart." );

        float detectedMax = 0.0f, detectedMin = 0.0f, detectedMaxFullFrame = 0.0f;
        const bool hdrActive = Engine::GraphicsEngine
            && Engine::GraphicsEngine->GetHdrOutputInfo( detectedMax, detectedMin, detectedMaxFullFrame );

        table.Row();
        if ( hdrActive ) {
            ImGui::Text( "Active. Monitor reports %.0f nits peak (%.0f full-frame, %.4f black).",
                detectedMax, detectedMaxFullFrame, detectedMin );
        } else if ( settings.HDR_Monitor ) {
            ImGui::TextDisabled( "Not active (needs the Direct3D 12 backend and an HDR-enabled display in Windows)." );
        } else {
            ImGui::TextDisabled( "Disabled - the SDR tonemapper is in use." );
        }

        ImGui::BeginDisabled( !settings.HDR_Monitor );
        CheckRow( table, "Use Monitor Metadata", &settings.HDR_AutoMaxBrightness,
            "Take the peak brightness from what the display reports instead of the slider below." );
        ImGui::BeginDisabled( settings.HDR_AutoMaxBrightness );
        SliderFloatRow( table, "HDR Max Brightness", "##HdrMaxBrightness", &settings.HDR_MaxBrightness,
            400.0f, 2000.0f, "%.0f nits",
            "Peak luminance the highlight roll-off targets. Monitor-reported metadata is\n"
            "frequently wrong - lower this until the brightest highlights stop clipping." );
        ImGui::EndDisabled();
        SliderFloatRow( table, "Paper White / UI Brightness", "##HdrPaperWhite", &settings.HDR_PaperWhite,
            80.0f, 500.0f, "%.0f nits",
            "Nit level that SDR white maps to. Sets the brightness of the HUD/menus and of\n"
            "diffuse-white surfaces; the headroom above it is what highlights get to use." );
        ImGui::EndDisabled();
    }
    table.End();

    ImGui::EndTabItem();
}

void RenderGraphicsTab( GothicRendererSettings& settings, ShaderCategory& shadersToReload ) {
    if ( !ImGui::BeginTabItem( "Graphics" ) ) {
        return;
    }

    SettingsTable table;
    if ( table.Begin( "##TblGraphics" ) ) {
        constexpr ListItem<int> textureQuality[] = {
            { "Very Low", static_cast<int>( GothicRendererSettings::TX_QUALITY::VeryLow ), nullptr, "TextureQuality_VeryLow" },
            { "Low", static_cast<int>( GothicRendererSettings::TX_QUALITY::Low ), nullptr, "TextureQuality_Low" },
            { "Medium", static_cast<int>( GothicRendererSettings::TX_QUALITY::Medium ), nullptr, "TextureQuality_Medium" },
            { "High", static_cast<int>( GothicRendererSettings::TX_QUALITY::High ), nullptr, "TextureQuality_High" },
            { "Very High", static_cast<int>( GothicRendererSettings::TX_QUALITY::VeryHigh ), nullptr, "TextureQuality_VeryHigh" },
            { "Extreme", static_cast<int>( GothicRendererSettings::TX_QUALITY::MAX ), nullptr, "TextureQuality_Extreme" },
        };
        settings.textureMaxSize = std::clamp( settings.textureMaxSize,
            textureQuality[0].value, textureQuality[std::size( textureQuality ) - 1].value );
        ComboRow( table, "Texture Quality", "##TextureQuality", textureQuality, &settings.textureMaxSize, nullptr,
            [] { Engine::GAPI->UpdateTextureMaxSize(); } );

        constexpr ListItem<int> normalMapModes[] = {
            { "Disabled", 0, nullptr, "NormalMapping_Disabled" },
            { "OpenGL (Y+)", 1, nullptr, "NormalMapping_OpenGL" },
            { "DirectX (Y-)", 2, nullptr, "NormalMapping_DirectX" },
        };
        ComboRow( table, "Normal Mapping", "##NormalMapping", normalMapModes, &settings.AllowNormalmaps,
            "Needs a texture pack that ships normal maps. If in doubt, ask its author which\n"
            "channel layout it uses. Changing this reloads shaders.",
            [] {
                ReloadD3D11Shaders( ShaderCategory::All );
                Engine::GAPI->UpdateTextureMaxSize();
            } );

        SectionRow( table, "Shadows" );

        if ( CheckRow( table, "Shadows", &settings.EnableShadows, nullptr, "Shadows" ) ) {
            shadersToReload |= ShaderCategory::LightsAndShadows;
        }

        ImGui::BeginDisabled( !settings.EnableShadows );
        {
            // Restricted to these five power-of-two steps - 8192 is the hard ceiling on both backends.
            constexpr ListItem<int> shadowMapSizes[] = {
                { "Very Low", 512, nullptr, "ShadowQuality_VeryLow" },
                { "Low", 1024, nullptr, "ShadowQuality_Low" },
                { "Medium", 2048, nullptr, "ShadowQuality_Medium" },
                { "High", 4096, nullptr, "ShadowQuality_High" },
                { "Very High", 8192, nullptr, "ShadowQuality_VeryHigh" },
            };
            ComboRow( table, "Shadow Quality", "##ShadowQuality", shadowMapSizes, &settings.ShadowMapSize, nullptr,
                [&shadersToReload] { shadersToReload |= ShaderCategory::LightsAndShadows; } );

            constexpr ListItem<GothicRendererSettings::E_ShadowFilterMode> shadowFilterModes[] = {
                { "Disabled", GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_DISABLED },
                { "Simple", GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_SIMPLE },
                { "PCSS", GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_PCSS,
                  "Contact-hardening shadows: sharp where an object touches the ground, softer further away." },
            };
            ComboRow( table, "Shadow Filtering", "##ShadowFiltering", shadowFilterModes, &settings.ShadowFilterMode, nullptr,
                [&shadersToReload] { shadersToReload |= ShaderCategory::LightsAndShadows; } );

            if ( SliderIntRow( table, "Shadow Cascades", "##ShadowCascades", &settings.NumShadowCascades,
                1, MAX_CSM_CASCADES, "%d",
                "How many sun-shadow slices the view is split into. More reaches further with\n"
                "the same detail near the camera, at the cost of one extra shadow pass each.\n"
                "Changing this reloads shaders." ) ) {
                settings.ApplyFeatureLevel10Downgrades();
                shadersToReload |= ShaderCategory::LightsAndShadows;
            }

            SliderFloatRow( table, "Shadow Distance", "##ShadowDistance", &settings.WorldShadowRangeScale, 0.25f, 4.0f, "%.2f",
                "Scales how far the sun shadows reach. Larger covers more of the world with\n"
                "the same shadow map, so everything gets less detailed." );

            SliderFloatRow( table, "Shadow Strength", "##ShadowStrength", &settings.ShadowStrength, 0.0f, 1.0f, "%.2f",
                "How dark a shadowed surface gets." );
        }
        ImGui::EndDisabled();

        SectionRow( table, "Lighting" );

        CheckRow( table, "Dynamic Lighting", &settings.EnableDynamicLighting, nullptr, "DynamicLighting" );

        ImGui::BeginDisabled( !settings.EnableDynamicLighting );
        {
            constexpr ListItem<GothicRendererSettings::EPointLightShadowMode> pointLightShadows[] = {
                { "Off", GothicRendererSettings::EPointLightShadowMode::PLS_DISABLED },
                { "Static", GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY,
                  "Only the world geometry casts shadows from torches and spells." },
                { "Dynamic Update", GothicRendererSettings::EPointLightShadowMode::PLS_UPDATE_DYNAMIC },
                { "Full", GothicRendererSettings::EPointLightShadowMode::PLS_FULL,
                  "Very expensive. Don't use unless you encounter visual bugs." },
            };
            ComboRow( table, "Point Light Shadows", "##PointLightShadows", pointLightShadows,
                &settings.EnablePointlightShadows );

            CheckRow( table, "Limit Light Intensity", &settings.LimitLightIntesity, nullptr, "LimitLightIntensity" );
        }
        ImGui::EndDisabled();

        table.Row();
        ImText( "Specular Highlights", ImVec2( LabelWidth, 0 ) );
        ImGui::SetItemTooltip( "Some players find specular highlights (bright glints on lit surfaces) visually\n"
            "distracting. Untick a category to keep its diffuse lighting but drop its highlight." );
        ImGui::SameLine();
        ImGui::CheckboxFlags( "Sun##SpecSun", &settings.SpecularHighlightsFlags, GothicRendererSettings::SH_SUN );
        ImGui::SameLine();
        ImGui::CheckboxFlags( "Lights##SpecPointlights", &settings.SpecularHighlightsFlags, GothicRendererSettings::SH_POINTLIGHTS );
        ImGui::SameLine();
        ImGui::CheckboxFlags( "Ambient##SpecAtmospheric", &settings.SpecularHighlightsFlags, GothicRendererSettings::SH_ATMOSPHERIC );
        ImGui::SetItemTooltip( "The static, co-located fill lights Gothic pre-places to brighten rooms/caves." );

        SectionRow( table, "Ambient Occlusion" );

        constexpr ListItem<AOMode> aoModes[] = {
            { "Disabled", AOMode::AO_NONE },
            { "HBAO+", AOMode::AO_HBAO, "NVIDIA HBAO+ (Horizon-Based Ambient Occlusion Plus)" },
            { "SAO", AOMode::AO_SAO },
            { "ASSAO / XeGTAO", AOMode::AO_ASSAO,
              "D3D11: Intel ASSAO (Adaptive Screen Space Ambient Occlusion).\nD3D12: Intel XeGTAO (ground-truth ambient occlusion)." },
        };
        ComboRow( table, "Ambient Occlusion", "##AoMode", aoModes, &settings.AoMode,
            "Darkens creases and contact points. Changing this reloads shaders.",
            [] { ReloadD3D11Shaders( ShaderCategory::Other ); } );

        SectionRow( table, "Reflections" );

        constexpr ListItem<GothicRendererSettings::E_WaterSSRQuality> waterSsr[] = {
            { "Disabled", GothicRendererSettings::WATER_SSR_DISABLED },
            { "Low", GothicRendererSettings::WATER_SSR_LOW },
            { "Medium", GothicRendererSettings::WATER_SSR_MEDIUM },
            { "High", GothicRendererSettings::WATER_SSR_HIGH },
        };
        ComboRow( table, "Water Reflections", "##WaterSSR", waterSsr, &settings.WaterSSRQuality, nullptr,
            [&shadersToReload] { shadersToReload |= ShaderCategory::Water; } );

        // D3D12 treats the quality as a runtime loop bound, so there is no shader recompile here.
        if ( IsD3D12() ) {
            constexpr ListItem<GothicRendererSettings::E_WaterSSRQuality> opaqueSsr[] = {
                { "Disabled", GothicRendererSettings::WATER_SSR_DISABLED, nullptr, "OpaqueSSR_Disabled" },
                { "Low", GothicRendererSettings::WATER_SSR_LOW, nullptr, "OpaqueSSR_Low" },
                { "Medium", GothicRendererSettings::WATER_SSR_MEDIUM, nullptr, "OpaqueSSR_Medium" },
                { "High", GothicRendererSettings::WATER_SSR_HIGH, nullptr, "OpaqueSSR_High" },
            };
            ComboRow( table, "Wet Surface Reflections", "##OpaqueSSR", opaqueSsr, &settings.OpaqueSSRQuality,
                "Screen-space reflections on wet/glossy ground and metal. One frame of lag; D3D12 only." );
        }
    }
    table.End();

    ImGui::EndTabItem();
}

void RenderEffectsTab( GothicRendererSettings& settings, ShaderCategory& shadersToReload ) {
    if ( !ImGui::BeginTabItem( "Effects" ) ) {
        return;
    }

    SettingsTable table;
    if ( table.Begin( "##TblEffects" ) ) {
        SectionRow( table, "Tone Mapping" );

        CheckRow( table, "HDR Rendering", &settings.EnableHDR,
            "Render the scene in high dynamic range and tone-map it down. Not the same as HDR\n"
            "monitor output, which lives on the Display tab.", "HDR" );

        ImGui::BeginDisabled( !settings.EnableHDR );
        {
            constexpr ListItem<GothicRendererSettings::E_HDRToneMap> toneMaps[] = {
                { "Simple", GothicRendererSettings::ToneMap_Simple },
                { "jafEq4", GothicRendererSettings::ToneMap_jafEq4 },
                { "Uncharted 2", GothicRendererSettings::Uncharted2Tonemap },
                { "ACES Film", GothicRendererSettings::ACESFilmTonemap },
                { "ACES Fitted", GothicRendererSettings::ACESFittedTonemap },
                { "Perceptual Quantizer", GothicRendererSettings::PerceptualQuantizerTonemap },
            };
            ComboRow( table, "Tone Mapping", "##ToneMap", toneMaps, &settings.HDRToneMap,
                "The curve that maps the HDR scene onto the display. Changing this reloads shaders.",
                [] { ReloadD3D11Shaders( ShaderCategory::Tonemapping ); } );

            SliderFloatRow( table, "Exposure", "##Exposure", &settings.Exposure, 0.0f, 4.0f );
        }
        ImGui::EndDisabled();

        SectionRow( table, "Post Processing" );

        CheckRow( table, "Bloom", &settings.EnableBloom, "Bright surfaces bleed a soft glow.", "Bloom" );
        ImGui::BeginDisabled( !settings.EnableBloom );
        SliderFloatRow( table, "Bloom Strength", "##BloomStrength", &settings.BloomStrength, 0.0f, 4.0f );
        ImGui::EndDisabled();

        if ( CheckRow( table, "God Rays", &settings.EnableGodRays,
            "Shafts of sunlight. Changing this reloads shaders.", "GodRays" ) ) {
            ReloadD3D11Shaders( ShaderCategory::Other );
        }

        CheckRow( table, "Depth of Field", &settings.EnableDoF, "Blurs whatever the camera isn't focused on.", "DepthOfField" );
        ImGui::BeginDisabled( !settings.EnableDoF );
        SliderFloatRow( table, "Focus Range", "##DoFFocusRange", &settings.DoFFocusRange, 500.0f, 50000.0f, "%.0f",
            "Range around the auto-focus point that remains sharp." );
        ImGui::EndDisabled();

        SectionRow( table, "Weather & Water" );

        CheckRow( table, "Rain", &settings.EnableRain, nullptr, "Rain" );
        ImGui::BeginDisabled( !settings.EnableRain );
        CheckRow( table, "Rain Effects", &settings.EnableRainEffects,
            "Wet surfaces, puddles and splashes while it rains.", "RainEffects" );
        ImGui::EndDisabled();

        if ( CheckRow( table, "Water Waves", &settings.EnableWaterAnimation, nullptr, "WaterWaves" ) ) {
            shadersToReload |= ShaderCategory::Water;
        }

#if defined(BUILD_GOTHIC_2_6_fix) || defined(BUILD_GOTHIC_1_CLASSIC)
#if defined(BUILD_GOTHIC_1_CLASSIC)
        if ( haveWindAnimations )
#endif
        {
            SectionRow( table, "Vegetation" );

            bool windEffect = settings.WindQuality != GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE;
            if ( CheckRow( table, "Wind", &windEffect, "Trees, grass and wheat wave in the wind.", "Wind" ) ) {
                settings.WindQuality = windEffect
                    ? GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED
                    : GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE;
                shadersToReload |= ShaderCategory::Other;
            }

            ImGui::BeginDisabled( !windEffect );
            SliderFloatRow( table, "Wind Strength", "##WindStrength", &settings.GlobalWindStrength, 0.1f, 5.0f );
            ImGui::EndDisabled();
        }

        if ( CheckRow( table, "Hero Affects Objects", &settings.HeroAffectsObjects,
            "Grass and wheat move when the player runs through it.", "HeroAffectsObjects" ) ) {
            shadersToReload |= ShaderCategory::Other;
        }
#endif //BUILD_GOTHIC_2_6_fix
    }
    table.End();

    ImGui::EndTabItem();
}

void RenderWorldTab( GothicRendererSettings& settings ) {
    if ( !ImGui::BeginTabItem( "World" ) ) {
        return;
    }

    SettingsTable table;
    if ( table.Begin( "##TblWorld" ) ) {
        SectionRow( table, "Draw Distance" );

        SliderIntRow( table, "World", "##SectionDrawRadius", &settings.SectionDrawRadius, 1, 20, "%d",
            "How many world chunks around the camera are drawn." );
        DistanceRow( table, "Objects", "##OutdoorVobDrawRadius", &settings.OutdoorVobDrawRadius, 10.0f, 1000.0f );
        DistanceRow( table, "Small Objects", "##OutdoorSmallVobDrawRadius", &settings.OutdoorSmallVobDrawRadius, 10.0f, 1000.0f );
        DistanceRow( table, "Indoor Objects", "##IndoorVobDrawRadius", &settings.IndoorVobDrawRadius, 10.0f, 1000.0f );
        DistanceRow( table, "NPCs", "##SkeletalMeshDrawRadius", &settings.SkeletalMeshDrawRadius, 10.0f, 500.0f );
        DistanceRow( table, "Effects", "##VisualFXDrawRadius", &settings.VisualFXDrawRadius, 1.0f, 500.0f,
            "Draw distance for torches, spells, campfires and other special effects." );

        SectionRow( table, "Culling" );

        CheckRow( table, "Portal Culling", &settings.EnablePortalCulling,
            "Skip rooms the camera cannot see into through any doorway. Portal-compiled worlds only." );
        CheckRow( table, "Occlusion Culling", &settings.EnableOcclusionCulling,
            "Hides objects that are not visible by camera. Doesn't work properly, turn off if you don't play on potato." );

        SectionRow( table, "Objects" );

        CheckRow( table, "Animate Static Objects", &settings.AnimateStaticVobs );
        CheckRow( table, "Draw World Section Intersections", &settings.DrawSectionIntersections,
            "Draws every world chunk that intersects with the GD3D11 world draw distance." );

#ifdef BUILD_GOTHIC_1_08k
        CheckRow( table, "Forest Portals", &settings.DrawG1ForestPortals,
            "The darkening textures Gothic 1 places around forests and some doors." );
        CheckRow( table, "Highlight Interactive Focus", &settings.G1HighlightInteractiveFocus,
            "Brightens whatever the player currently has in focus." );
#endif
    }
    table.End();

    ImGui::EndTabItem();
}

void RenderSystemTab( ImGuiShim& shim, GothicRendererSettings& settings ) {
    if ( !ImGui::BeginTabItem( "System" ) ) {
        return;
    }

    SettingsTable table;
    if ( table.Begin( "##TblSystem" ) ) {
        SectionRow( table, "Interface" );

        SliderFloatRow( table, "UI Scale", "##GothicUIScale", &settings.GothicUIScale, 0.5f, 4.0f );
        CheckRow( table, "Custom Font Rendering", &settings.EnableCustomFontRendering );
        CheckRow( table, "Fast Inventory Rendering", &settings.FastInventoryRendering,
            "Skips ZenGin's per-slot pseudo-world render for inventory items." );
        CheckRow( table, "Allow Numpad Keys", &settings.AllowNumpadKeys,
            "Lets the mod's debug hotkeys on the numpad through to the engine." );

        SectionRow( table, "Memory" );

        if ( CheckRow( table, "Compress Backbuffer", &settings.CompressBackBuffer,
            "Halves the backbuffer's memory cost at a small quality loss. This is a 32-bit\n"
            "process, so on a large resolution it can be the difference between running and\n"
            "running out of address space." ) ) {
            Engine::GAPI->UpdateCompressBackBuffer();
        }

        CheckRow( table, "Optimize Meshes On Load", &settings.EnableMeshOptimization,
            "Reorders geometry for GPU cache locality when a world is first converted. This is\n"
            "most of what makes a big world slow to load; each world only pays for it once.\n"
            "Takes effect on the next world load." );

        SectionRow( table, "Troubleshooting" );

        CheckRow( table, "Classic Settings Window", &shim.UseClassicSettingsWindow,
            "Switches back to the pre-tab settings window. Everything not listed here lives in\n"
            "the advanced windows (CTRL+F11) either way." );

        table.Row();
        ImGui::TextDisabled( "Advanced settings: CTRL+F11" );

        table.Row();
        if ( ImGui::Button( "Reset All Settings", ImVec2( LabelWidth, 0 ) ) ) {
            settings.SetDefault();
            if ( IsD3D12() ) {
                settings.ApplyDx12Defaults();
            }
            Engine::GraphicsEngine->ReloadShaders( ShaderCategory::All );
        }
        ImGui::SetItemTooltip( "Reset all settings to their default values." );
        ImGui::SameLine();
        if ( ImGui::Button( "Reload Shaders" ) ) {
            Engine::GraphicsEngine->ReloadShaders( ShaderCategory::All );
        }
    }
    table.End();

    ImGui::EndTabItem();
}

/** Preset combo + save button, drawn above the tabs so they apply to whatever tab is open. */
void RenderHeader( GothicRendererSettings& settings ) {
    constexpr ListItem<GothicRendererSettings::E_GraphicsPreset> graphicsPresets[] = {
        { "Custom", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_CUSTOM },
        { "Low", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_LOW },
        { "Medium", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_MEDIUM },
        { "High", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_HIGH },
        { "Very High", GothicRendererSettings::E_GraphicsPreset::GRAPHICS_VERY_HIGH },
    };

    ImText( "Graphics Preset", ImVec2( LabelWidth, 0 ) );
    ImGui::SameLine();
    ImGui::SetNextItemWidth( 250 );
    if ( ImComboBox( "##GraphicsPreset", graphicsPresets, &settings.GraphicsPreset,
        [&settings] { settings.ApplyGraphicsPreset(); } ) ) {
        ImGui::EndCombo();
    }
    ImGui::SameLine();

    const auto worldSettingsPath = Engine::GAPI->GetLoadedWorldSettingsPath( false );
    const bool isInWorld = !worldSettingsPath.empty();
    const bool hasWorldSettings = Toolbox::FileExists( worldSettingsPath );
    const bool saveForWorld = ( ImGui::GetIO().KeyCtrl || hasWorldSettings ) && isInWorld;

    const bool saved = ImGui::Button( "Save Settings", ImVec2( ImGui::GetContentRegionAvail().x, 0 ) );
    if ( saveForWorld ) {
        ImGui::SetItemTooltip( "Save settings to \"%s\"", worldSettingsPath.c_str() );
    } else {
        ImGui::SetItemTooltip( "Save settings.\nCTRL+Click to save just for the current world." );
    }
    if ( saved ) {
        Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::UI_ClosedSettings );
        if ( saveForWorld ) {
            Engine::GAPI->SaveRendererWorldSettings( settings );
        } else {
            Engine::GAPI->SaveRendererWorldSettings( settings, MENU_SETTINGS_FILE );
        }
        Engine::GAPI->SaveMenuSettings( MENU_SETTINGS_FILE );
    }

    ImGui::Separator();
}

} // namespace

void ImGuiSettings::RenderWindow( ImGuiShim& shim ) {
    IM_ASSERT( ImGui::GetCurrentContext() != NULL && "Missing Dear ImGui context!" );
    IMGUI_CHECKVERSION();

    static std::string title;
    if ( title.empty() ) {
        title.append( "GD3D11 " ).append( VERSION_NUMBER );
        if ( Engine::GraphicsEngine ) {
            title.append( Engine::GraphicsEngine->GetBackendAPI() == EGraphicsEngineBackend::D3D12 ? " - D3D12" : " - D3D11" );
        }
#ifdef IS_DEV_BUILD
        title.append( " (" BUILD_DATE ")" );
#endif
    }

    // TIP: Don't use ImGui::GetMainViewport for framebuffer sizes since GD3D11 can undersample or
    // oversample the game. Use whatever resolution the engine reports instead.
    const auto windowSize = shim.CurrentResolution;
    ImGui::SetNextWindowPos( ImVec2( windowSize.x / 2.0f, windowSize.y / 2.0f ), ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );
    ImGui::SetNextWindowSize( ImVec2( 900, 640 ), ImGuiCond_Appearing );

    ShaderCategory shadersToReload = ShaderCategory::None;

    if ( ImGui::Begin( title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse ) ) {
        GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;
        FixupSettings( settings );

        ImGui::PushFont( nullptr, 18 );
        RenderHeader( settings );

        if ( ImGui::BeginTabBar( "##SettingsTabs" ) ) {
            RenderDisplayTab( shim, settings );
            RenderGraphicsTab( settings, shadersToReload );
            RenderEffectsTab( settings, shadersToReload );
            RenderWorldTab( settings );
            RenderSystemTab( shim, settings );

            ImGui::EndTabBar();
        }
        ImGui::PopFont();
    }
    ImGui::End();

    if ( shadersToReload != ShaderCategory::None ) {
        Engine::GraphicsEngine->ReloadShaders( shadersToReload );
    }
}
