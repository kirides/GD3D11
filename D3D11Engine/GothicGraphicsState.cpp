#include "GothicGraphicsState.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ShadowMap.h"
#include "BaseGraphicsEngine.h"

static void ApplyFeatureLevel10Downgrades( GothicRendererSettings& s ) {
    // one 4k texture, 1/2 2k textures max.
    s.NumShadowCascades = std::min( s.NumShadowCascades, MAX_CSM_CASCADES );

    if ( s.NumShadowCascades >= 2 ) {
        s.DebugSettings.ShadowCascades.Lambda = D3D11ShadowMap::lambdaBiasTable[s.NumShadowCascades].lambda;
        s.DebugSettings.ShadowCascades.Bias = D3D11ShadowMap::lambdaBiasTable[s.NumShadowCascades].bias;
    }
}

// The shadow half of a graphics preset, split out so the settings window can offer a single shadow
// quality knob without duplicating the numbers.
static void ApplyShadowPresets( GothicRendererSettings& s, GothicRendererSettings::E_GraphicsPreset preset ) {
    switch ( preset ) {
    case GothicRendererSettings::GRAPHICS_VERY_LOW:
    case GothicRendererSettings::GRAPHICS_LOW:
        s.WorldShadowRangeScale = 1.0f;
        s.NumShadowCascades = 2;
        s.DebugSettings.FeatureSet.UseShadowAtlas = true;
        s.ShadowMapSize = preset == GothicRendererSettings::GRAPHICS_VERY_LOW ? 512 : 1024;
        s.ShadowFrustumCullingMode = GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_AGGRESSIVE;
        s.ShadowSoftness = 2.00f;
        s.SmoothShadowCameraUpdate = true;
        s.SmoothShadowFrequency = 500;
        s.ShadowFilterMode = GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_SIMPLE;
        break;
    case GothicRendererSettings::GRAPHICS_MEDIUM:
        s.WorldShadowRangeScale = 1.0f;
        s.NumShadowCascades = 3;
        s.DebugSettings.FeatureSet.UseShadowAtlas = true;
        s.ShadowMapSize = 2048;
        s.ShadowFrustumCullingMode = GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_CONSERVATIVE;
        s.ShadowSoftness = 2.00f;
        s.SmoothShadowCameraUpdate = true;
        s.SmoothShadowFrequency = 1000;
        s.ShadowFilterMode = GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_SIMPLE;
        break;
    case GothicRendererSettings::GRAPHICS_HIGH:
        s.WorldShadowRangeScale = 1.0f;
        s.NumShadowCascades = 3;
        s.DebugSettings.FeatureSet.UseShadowAtlas = false;
        s.ShadowMapSize = 4096;
        s.ShadowFrustumCullingMode = GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_CONSERVATIVE;
        s.ShadowSoftness = 2.00f;
        s.SmoothShadowCameraUpdate = false;
        s.SmoothShadowFrequency = 1000;
        s.ShadowFilterMode = GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_SIMPLE;
        break;
    case GothicRendererSettings::GRAPHICS_VERY_HIGH:
        s.WorldShadowRangeScale = 1.0f;
        s.NumShadowCascades = 4;
        s.DebugSettings.FeatureSet.UseShadowAtlas = false;
        s.ShadowMapSize = 4096;
        s.ShadowFrustumCullingMode = GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_CONSERVATIVE;
        s.ShadowSoftness = 1.0f;
        s.SmoothShadowCameraUpdate = false;
        s.SmoothShadowFrequency = 1000;
        s.ShadowFilterMode = GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_PCSS;
        break;
    default:
        return;
    }
    s.ShadowQuality = preset;
}

static void ApplyGraphicsPresets( GothicRendererSettings& s ) {
    const auto preset = s.GraphicsPreset;
    if ( preset == GothicRendererSettings::E_GraphicsPreset::GRAPHICS_CUSTOM ) {
        return;
    }

    switch ( preset ) {
    case GothicRendererSettings::GRAPHICS_VERY_LOW:
    case GothicRendererSettings::GRAPHICS_LOW:
    {
        s.ChangeWindowPreset = WINDOW_MODE_FULLSCREEN_BORDERLESS;

        s.CompressBackBuffer = true;
        ApplyShadowPresets( s, preset );

        s.EnableDynamicLighting = false;
        s.EnablePointlightShadows = GothicRendererSettings::EPointLightShadowMode::PLS_DISABLED;

        s.AoMode = AOMode::AO_NONE;

        s.textureMaxSize = static_cast<int>(GothicRendererSettings::TX_QUALITY::Medium);

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
        ApplyShadowPresets( s, preset );

        s.EnableDynamicLighting = true;
        s.EnablePointlightShadows = GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY;

        s.AoMode = AOMode::AO_ASSAO;
        s.ApplyAssaoPreset( 0 );

        s.textureMaxSize = static_cast<int>(GothicRendererSettings::TX_QUALITY::Medium);

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
        ApplyShadowPresets( s, preset );

        s.EnableDynamicLighting = true;
        s.EnablePointlightShadows = GothicRendererSettings::EPointLightShadowMode::PLS_UPDATE_DYNAMIC;

        s.AoMode = AOMode::AO_ASSAO;
        s.ApplyAssaoPreset( 1 );

        s.textureMaxSize = static_cast<int>(GothicRendererSettings::TX_QUALITY::High);

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
        ApplyShadowPresets( s, preset );

        s.EnableDynamicLighting = true;
        s.EnablePointlightShadows = GothicRendererSettings::EPointLightShadowMode::PLS_UPDATE_DYNAMIC;

        s.AoMode = AOMode::AO_ASSAO;
        s.ApplyAssaoPreset( 1 );

        s.textureMaxSize = static_cast<int>(GothicRendererSettings::TX_QUALITY::VeryHigh);

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

    if ( FeatureLevel10Compatibility ) {
        s.ApplyFeatureLevel10Downgrades();
    }
    if ( Engine::GAPI ) Engine::GAPI->UpdateTextureMaxSize();
    if ( Engine::GraphicsEngine ) Engine::GraphicsEngine->ReloadShaders();
    if ( Engine::GAPI ) Engine::GAPI->UpdateCompressBackBuffer();
}

void GothicRendererSettings::ApplyDeviceCapabilities( const GraphicsDeviceCapabilities& caps )
{
    auto resolve = []( E_FeatureOverride ov, bool supported ) {
        return ov == FEATURE_AUTO ? supported : ov == FEATURE_FORCE_ON;
    };

    auto& fs = DebugSettings.FeatureSet;
    fs.EnableDriverExtensions = resolve( fs.EnableDriverExtensionsOverride, caps.DriverExtensions );
    fs.UseMDI = resolve( fs.UseMDIOverride, caps.MultiDrawIndirect );
    fs.UseLayeredRendering = resolve( fs.UseLayeredRenderingOverride, caps.LayeredRendering );
}

void GothicRendererSettings::ApplyGraphicsPreset()
{
    ::ApplyGraphicsPresets( *this );
}

void GothicRendererSettings::ApplyShadowPreset()
{
    ::ApplyShadowPresets( *this, ShadowQuality );
    if ( FeatureLevel10Compatibility ) {
        ApplyFeatureLevel10Downgrades();
    }
}

void GothicRendererSettings::ApplyFeatureLevel10Downgrades()
{
    ::ApplyFeatureLevel10Downgrades( *this );
}
