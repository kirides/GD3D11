#include "pch.h"
#include "ShaderRegistry.h"
#include "GothicGraphicsState.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "Engine.h"

#include <filesystem>

// Feature flags that gate which shader variants get declared / which macros they carry.
// Owned elsewhere (graphics engine / DLLMain); the declaration table only reads them.
extern bool FeatureLevel10Compatibility;
extern bool FeatureRTArrayIndexFromAnyShader;
#if !defined(BUILD_GOTHIC_2_6_fix) && !defined(BUILD_1_12F)
extern bool haveWindAnimations;
#endif

/** Populates the shader declaration table */
void ShaderRegistry::Build() {
    m_Shaders = std::vector<ShaderInfo>();
    static const char* sNums[] = { "0","1","2","3","4","5","6","7","8","9","10","11","12","13","14","15" };

    auto& Shaders = m_Shaders;

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Ex>( "VS_Ex.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 )  );

    // Position-only variant used by opaque depth/shadow passes (12-byte vertex stream).
    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExDepth>( "VS_ExDepth.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_POS_ONLY )  );

    // Packed variant for the wrapped world mesh (36-byte vertex stream; forwards precomputed tangent).
    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExPacked>( "VS_ExPacked.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_PACKED_EX )  );

    // Node attachments draw the packed 36-byte VOB meshes.
    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNode>( "VS_ExNode.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Decal>( "VS_Decal.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_DecalInstanced>( "VS_DecalInstanced.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_15_VS_DecalInstanced )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExWater>( "VS_ExWater.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_PACKED_EX )
        .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
            const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
#ifdef BUILD_GOTHIC_2_6_fix
            list.push_back( {"SHD_WATERANI", s.EnableWaterAnimation ? "1" : "0"} );
#else
            list.push_back( {"SHD_WATERANI", "0"} );
#endif
        }) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ParticlePoint>( "VS_ParticlePoint.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_11_VS_ParticlePoint ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ParticlePointShaded>( "VS_ParticlePointShaded.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_13 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExWS>( "VS_ExWS.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExSkeletal>( "VS_ExSkeletal.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_3_VS_ExSkeletal )
        .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
            list.push_back( { "SKINNING_STRUCTURED", FeatureLevel10Compatibility ? "0" : "1" } );
        } ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExSkeletalVN>( "VS_ExSkeletalVN.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_3_VS_ExSkeletal )
        .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
            list.push_back( { "SKINNING_STRUCTURED", FeatureLevel10Compatibility ? "0" : "1" } );
        } ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_TransformedEx>( "VS_TransformedEx.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_TransformedEx_MAX_Z>( "VS_TransformedEx.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 )
        .with_macros( { {"OVERRIDE_MAX_Z", "1"} } ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExPointLight>( "VS_ExPointLight.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_XYZRHW_DIF_T1>( "VS_XYZRHW_DIF_T1.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_7_VS_XYZRHW_DIF_T1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_XYZRHW_DIF_T1_MAX_Z>( "VS_XYZRHW_DIF_T1.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_7_VS_XYZRHW_DIF_T1 )
        .with_macros( { {"OVERRIDE_MAX_Z", "1"} }));

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExInstancedObj>( "VS_ExInstancedObj.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_10_VS_ExInstancedObj )
        .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
            const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
#ifdef BUILD_GOTHIC_2_6_fix
            list.push_back( {"SHD_WIND",      s.WindQuality == GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED ? "1" : "0"} );
            list.push_back( {"SHD_INFLUENCE", s.HeroAffectsObjects ? "1" : "0"} );
            list.push_back( {"WIND_META_SRV", (!FeatureLevel10Compatibility && (s.WindQuality == GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED || s.HeroAffectsObjects)) ? "1" : "0"} );
#elif defined(BUILD_1_12F)
            list.push_back( {"SHD_WIND",      "0"} );
            list.push_back( {"SHD_INFLUENCE", "0"} );
            list.push_back( {"WIND_META_SRV", "0"} );
#else
            list.push_back( {"SHD_WIND",      (haveWindAnimations && s.WindQuality == GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED) ? "1" : "0"} );
            list.push_back( {"SHD_INFLUENCE", (haveWindAnimations && s.HeroAffectsObjects) ? "1" : "0"} );
            list.push_back( {"WIND_META_SRV", (!FeatureLevel10Compatibility && haveWindAnimations && (s.WindQuality == GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED || s.HeroAffectsObjects)) ? "1" : "0"} );
#endif
        }) );


    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExInstanced>( "VS_ExInstanced.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_4_VS_ExInstanced ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_GrassInstanced>( "VS_GrassInstanced.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_9_VS_GrassInstanced )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_GrassInstancedShadow>( "VS_GrassInstancedShadow.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_9_VS_GrassInstanced )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Lines>( "VS_Lines.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_6_Lines )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Lines_XYZRHW>( "VS_Lines_XYZRHW.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_6_Lines )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Lines>( "PS_Lines.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_LinesSel>( "PS_LinesSel.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Simple>( "PS_Simple.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Simple_FF>( "PS_Simple.hlsl" )
        .with_macros( { { "USE_FFDATA", "1" } } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Rain>( "PS_Rain.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Rain_Snow>( "PS_Rain.hlsl" )
        .with_macros( { { "SNOW_FEATURE", "1" } }));

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Transparency>( "PS_Transparency.hlsl" )  );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_TransparencySkel>( "PS_TransparencySkel.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_World>( "PS_World.hlsl" ).with_macros({ {"MOTION_VECTORS", "1"}})  );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_World_NoMV>( "PS_World.hlsl" )  );


    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Water>( "PS_Water.hlsl" )
        .with_macros( []( std::vector<D3D_SHADER_MACRO>& list ) {
            const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
            list.push_back( { "SSR_QUALITY", sNums[std::clamp<size_t>( s.WaterSSRQuality, 0, 3 )] } );
        } )
        .with_category( ShaderCategory::Water )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_ParticleDistortion>( "PS_ParticleDistortion.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_ApplyParticleDistortion>( "PS_PFX_ApplyParticleDistortion.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Grass>( "PS_Grass.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_GrassShadow>( "PS_GrassShadow.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_PFX>( "VS_PFX.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_CinemaScope>( "VS_CinemaScope.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Simple>( "PS_PFX_Simple.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Simple_R8>( "PS_PFX_Simple_R8.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_VelocityDebug>( "PS_PFX_VelocityDebug.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_GaussBlur>( "PS_PFX_GaussBlur.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Heightfog>( "PS_PFX_Heightfog.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_UnderwaterFinal>( "PS_PFX_UnderwaterFinal.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Alpha_Blend>( "PS_PFX_Alpha_Blend.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_CinemaScope>( "PS_PFX_CinemaScope.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DistanceBlur>( "PS_PFX_DistanceBlur.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_LumConvert>( "PS_PFX_LumConvert.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_LumAdapt>( "PS_PFX_LumAdapt.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_HDR>( "PS_PFX_HDR.hlsl" )
        .with_category(ShaderCategory::Tonemapping)
        .with_macros( []( std::vector<D3D_SHADER_MACRO>& list ) {
            const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
            list.push_back( { "USE_TONEMAP", sNums[std::clamp( size_t(s.HDRToneMap), size_t(0), std::size(sNums)-1)]});
        } )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_GodRayMask>( "PS_PFX_GodRayMask.hlsl" ) );
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_GodRayZoom>( "PS_PFX_GodRayZoom.hlsl" ) );

    // PostFX Composition uber shader
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Composition>( "PS_PFX_Composition.hlsl" )
        .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
            const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
            // AO is now applied in the lighting pass (indirect light only), not in composition.
            list.push_back( { "COMPOSE_GODRAYS", s.EnableGodRays ? "1" : "0" } );
            list.push_back( { "COMPOSE_HEIGHTFOG", s.DrawFog ? "1" : "0" } );
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Tonemap>( "PS_PFX_Tonemap.hlsl" )
        .with_category(ShaderCategory::Tonemapping)
        .with_macros( []( std::vector<D3D_SHADER_MACRO>& list ) {
            const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
            list.push_back( { "USE_TONEMAP", sNums[std::clamp( size_t( s.HDRToneMap ), size_t( 0 ), std::size( sNums ) - 1 )] } );
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_AtmosphereGround>( "PS_AtmosphereGround.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Atmosphere>( "PS_Atmosphere.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_AtmosphereOuter>( "PS_AtmosphereOuter.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_FixedFunctionPipe>( "PS_FixedFunctionPipe.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Video>( "PS_Video.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DS_PointLight>( "PS_DS_PointLight.hlsl" )
        .with_category( ShaderCategory::LightsAndShadows ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DS_PointLightDynShadow>( "PS_DS_PointLightDynShadow.hlsl" )
        .with_category( ShaderCategory::LightsAndShadows ) );

    // Shadow macro builder shared by both atmospheric scattering shader variants
    ShaderInfo::MacroBuilder shadowMacroBuilder = [](std::vector<D3D_SHADER_MACRO>& list) {
        const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
        const bool isTaaEnabled = s.GetIsTAAEnabled();

        list.push_back( {"SHD_ENABLE",           s.EnableShadows ? "1" : "0"} );
        list.push_back( {"SHD_FILTER_16TAP_PCF", (s.ShadowFilterMode >= GothicRendererSettings::SHADOW_FILTER_SIMPLE) ? "1" : "0"} );
        list.push_back( {"SHD_FILTER_PCSS",      (s.ShadowFilterMode == GothicRendererSettings::SHADOW_FILTER_PCSS) ? "1" : "0"} );
        list.push_back( {"MAX_CSM_CASCADES",     TO_LITERAL(MAX_CSM_CASCADES)} );
        list.push_back( {"NUM_CSM_CASCADES",     sNums[std::clamp<size_t>(s.NumShadowCascades, 1, MAX_CSM_CASCADES)]} );
        list.push_back( {"CSM_PCF_LIMIT",        sNums[std::clamp<size_t>(s.ShadowCascadePCFLimit, 0, MAX_CSM_CASCADES)]} );
        list.push_back( {"SHADOW_ATLAS",         (FeatureLevel10Compatibility || s.DebugSettings.FeatureSet.UseShadowAtlas) ? "1" : "0"} );
        list.push_back( {"FP_USE_SHADOW_MASK",   s.DebugSettings.FeatureSet.UseScreenSpaceShadowMask ? "1" : "0"} );
        // If we have TAA we reduce the number of samples for shadow filtering to save performance, since TAA will help smooth out the results. If we don't have TAA, we need to afford more samples to get better quality.
        list.push_back( {"SHD_BLUE_NOISE",       isTaaEnabled ? "1" : "0"} );
        list.push_back( {"PCSS_BLOCKER_TAPS",     isTaaEnabled ? "8" : "16"} );
        list.push_back( {"PCSS_FILTER_TAPS_NEAR", isTaaEnabled ? "8" : "32"} );
        list.push_back( {"PCSS_FILTER_TAPS_FAR",  isTaaEnabled ? "4" : "16"} );
        list.push_back( {"PCF_FILTER_TAPS_NEAR",  isTaaEnabled ? "8" : "16"} );
        list.push_back( {"PCF_FILTER_TAPS_FAR",   isTaaEnabled ? "4" : "8"} );
    };

    ShaderInfo::MacroBuilder normalmappingConfigurationBuilder = [](std::vector<D3D_SHADER_MACRO>& list) {
        const auto& s = Engine::GAPI->GetRendererState().RendererSettings;
        list.push_back( {"NORMAL_MAP_RESTORE_Z", s.CompressedNormalsSupport ? "1" : "0"} );
		list.push_back( {"NORMAL_MAP_MODE", sNums[std::clamp<size_t>(s.AllowNormalmaps, 1, 2)]} );
    };

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DS_AtmosphericScattering>( "PS_DS_AtmosphericScattering.hlsl" )
        .with_macros( shadowMacroBuilder )
        .with_category( ShaderCategory::LightsAndShadows ) );

    Shaders.push_back( ShaderInfo::make<GShaderID::GS_VertexNormals>( "GS_VertexNormals.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Diffuse>( "PS_Diffuse.hlsl" )
        .with_macros( {
            {"NORMALMAPPING", "0"},
            {"ALPHATEST", "0"}
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PortalDiffuse>( "PS_PortalDiffuse.hlsl" ) ); //forest portals, doors, etc.
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_WaterfallFoam>( "PS_WaterfallFoam.hlsl" ) );     //foam on at the base of waterfalls

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DS_AtmosphericScattering_Rain>( "PS_DS_AtmosphericScattering.hlsl" )
        .with_macros( { { "APPLY_RAIN_EFFECTS", "1" } })
        .with_macros( shadowMacroBuilder )
        .with_category( ShaderCategory::LightsAndShadows ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_LinDepth>( "PS_LinDepth.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmapped>( "PS_Diffuse.hlsl" )
        .with_macros(normalmappingConfigurationBuilder)
        .with_macros( {
            {"NORMALMAPPING", "1"},
            {"ALPHATEST", "0"},
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmappedFxMap>( "PS_Diffuse.hlsl" )
        .with_macros(normalmappingConfigurationBuilder)
        .with_macros( {
            {"NORMALMAPPING", "1"},
            {"ALPHATEST", "0"},
            {"FXMAP", "1"}
        } ) );


    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseAlphaTest>( "PS_Diffuse.hlsl" )
        .with_macros( {
            {"NORMALMAPPING", "0"},
            {"ALPHATEST", "1"},
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseAlphaTestShadows>( "PS_Diffuse.hlsl" )
        .with_macros( {
            {"NORMALMAPPING", "0"},
            {"ALPHATEST_SHADOWS", "1"},
            } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmappedAlphaTest>( "PS_Diffuse.hlsl" )
        .with_macros(normalmappingConfigurationBuilder)
        .with_macros( {
            {"NORMALMAPPING", "1"},
            {"ALPHATEST", "1"},
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmappedAlphaTestFxMap>( "PS_Diffuse.hlsl" )
        .with_macros(normalmappingConfigurationBuilder)
        .with_macros( {
            {"NORMALMAPPING", "1"},
            {"ALPHATEST", "1"},
            {"FXMAP", "1"}
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Preview_White>( "PS_Preview.hlsl" )
        .with_macros( { {"RENDERMODE", "0"} }));

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Preview_Textured>( "PS_Preview.hlsl" )
        .with_macros( { {"RENDERMODE", "1"} } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Preview_TexturedLit>( "PS_Preview.hlsl" )
        .with_macros( { {"RENDERMODE", "2"} } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Sharpen>( "PS_PFX_Sharpen.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_GammaCorrectInv>( "PS_PFX_GammaCorrectInv.hlsl" ) );

    if ( FeatureRTArrayIndexFromAnyShader ) {
        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExLayered>( "VS_ExLayered.hlsl" )
            .with_layout( VERTEX_INPUT_LAYOUT_1 ) );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNodeLayered>( "VS_ExNodeLayered.hlsl" )
            .with_layout( VERTEX_INPUT_LAYOUT_PACKED_EX )  );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExSkeletalLayered>( "VS_ExSkeletalLayered.hlsl" )
            .with_layout( VERTEX_INPUT_LAYOUT_3_VS_ExSkeletal )
            .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
                list.push_back( { "SKINNING_STRUCTURED", FeatureLevel10Compatibility ? "0" : "1" } );
            } ) ); // cbPerCubeRender for layered rendering
    }
    /*else: always compile fallback shaders*/
    {
        Shaders.push_back( ShaderInfo::make<GShaderID::GS_Cubemap>( "GS_Cubemap.hlsl" )  );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExCube>( "VS_ExCube.hlsl" )
            .with_layout( VERTEX_INPUT_LAYOUT_1 )  );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNodeCube>( "VS_ExNodeCube.hlsl" )
            .with_layout( VERTEX_INPUT_LAYOUT_PACKED_EX )  );

        Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExSkeletalCube>( "VS_ExSkeletalCube.hlsl" )
            .with_layout( VERTEX_INPUT_LAYOUT_3_VS_ExSkeletal )
            .with_macros( [](std::vector<D3D_SHADER_MACRO>& list) {
                list.push_back( { "SKINNING_STRUCTURED", FeatureLevel10Compatibility ? "0" : "1" } );
            } ) );
    }

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNodeInstanced>( "VS_ExNodeInstanced.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_14_VS_ExNodeInstanced ) );

    Shaders.push_back( ShaderInfo::make<GShaderID::GS_ParticleStreamOut>( "VS_AdvanceRain.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_13 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_AdvanceRain>( "VS_AdvanceRain.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_13 ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DoF_FocusResolve>( "PS_PFX_DoF_FocusResolve.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DoF>( "PS_PFX_DoF.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DoF_Gauss>( "PS_PFX_DoF.hlsl" )
        .with_macros( {{ "DOF_GAUSS_BLUR", "1" }} ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_DoF_Composite>( "PS_PFX_DoF_Composite.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_BloomComposite>( "PS_PFX_BloomComposite.hlsl" ) );

    // TAA Shader
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_TAA>( "PS_PFX_TAA.hlsl" )  );

    // Velocity Buffer Shader
    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_Velocity>( "PS_PFX_Velocity.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_CAS>( "PS_PFX_CAS.hlsl" ));


    if ( !FeatureLevel10Compatibility ) {
        // FSR1 EASU (Edge Adaptive Spatial Upsampling) Shader
        Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_FSR1_EASU>( "PS_PFX_FSR1_EASU.hlsl" ));

        // FSR1 RCAS (Robust Contrast Adaptive Sharpening) Shader
        Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_FSR1_RCAS>( "PS_PFX_FSR1_RCAS.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_AdvanceRain>( "CS_AdvanceRain.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_LightCulling>( "CS_LightCulling.hlsl" )
        .with_macros( {
            { "TILE_SIZE", TO_LITERAL( TILE_SIZE ) },
            { "MAX_LIGHTS_PER_TILE", TO_LITERAL( MAX_LIGHTS_PER_TILE ) },
        }));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_TiledShading>( "CS_TiledShading.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_GodRayMask>( "CS_PFX_GodRayMask.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_GodRayZoom>( "CS_PFX_GodRayZoom.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_DoF_FocusResolve>( "CS_PFX_DoF_FocusResolve.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_DoF>( "CS_PFX_DoF.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_DoF_Gauss>( "CS_PFX_DoF.hlsl" )
            .with_macros( {{ "DOF_GAUSS_BLUR", "1" }} ) );

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_DoF_Composite>( "CS_PFX_DoF_Composite.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_SAO>( "CS_PFX_SAO.hlsl" ));

        // Depth-only SAO variant: reconstructs view normals from depth (Forward+ fallback)
        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_SAO_DepthNormals>( "CS_PFX_SAO.hlsl" )
            .with_macros( {{ "SAO_RECONSTRUCT_NORMALS", "1" }} ) );

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_SAO_Blur>( "CS_PFX_SAO_Blur.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_Sharpen>( "CS_PFX_Sharpen.hlsl" ));

        // Bloom pyramid (FL11+): prefilter (bright-pass) + plain downsample share one shader
        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_Bloom_Prefilter>( "CS_PFX_Bloom_Downsample.hlsl" )
            .with_macros( {{ "BLOOM_PREFILTER", "1" }} ) );

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_Bloom_Downsample>( "CS_PFX_Bloom_Downsample.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_Bloom_Upsample>( "CS_PFX_Bloom_Upsample.hlsl" ));

        // Optional Forward+ smooth-normals-from-depth pass (feeds SAO/ASSAO AO producers)
        Shaders.push_back( ShaderInfo::make<CShaderID::CS_GenerateNormalsFromDepth>( "CS_GenerateNormalsFromDepth.hlsl" ));

        // Forward+ pixel shader variants
        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_Diffuse>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "0" },
                { "ALPHATEST", "0" },
            }).with_category(ShaderCategory::LightsAndShadows));

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_DiffuseNormalmapped>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros(normalmappingConfigurationBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "1" },
                { "ALPHATEST", "0" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_DiffuseNormalmappedFxMap>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros(normalmappingConfigurationBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "1" },
                { "ALPHATEST", "0" },
                { "FXMAP", "1" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_DiffuseAlphaTest>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "0" },
                { "ALPHATEST", "1" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_DiffuseNormalmappedAlphaTest>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros(normalmappingConfigurationBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "1" },
                { "ALPHATEST", "1" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_DiffuseNormalmappedAlphaTestFxMap>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros(normalmappingConfigurationBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "1" },
                { "ALPHATEST", "1" },
                { "FXMAP", "1" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_ShadowMask>( "PS_FP_ShadowMask.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_ResolveDepthMSAA>( "PS_ResolveDepthMSAA.hlsl" )
            .with_category( ShaderCategory::LightsAndShadows ) );
    }
}

static size_t HashCombine( size_t seed, size_t val ) noexcept {
    return seed ^ (val + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

size_t ShaderRegistry::ComputeShaderHash( const ShaderInfo& si ) {
    size_t h = 0;

    // Hash file last-modified timestamp
    std::string fullPath = Engine::GAPI->GetStartDirectory() + "\\system\\GD3D11\\shaders\\" + si.fileName;
    std::error_code ec;
    auto lwt = std::filesystem::last_write_time( std::filesystem::path( fullPath ), ec );
    if ( !ec ) {
        h = HashCombine( h, static_cast<size_t>(lwt.time_since_epoch().count()) );
    }

    // Hash per-shader macros
    for ( const auto& macro : si.shaderMakros ) {
        if ( macro.Name )       h = HashCombine( h, std::hash<std::string_view>{}( macro.Name ) );
        if ( macro.Definition ) h = HashCombine( h, std::hash<std::string_view>{}( macro.Definition ) );
    }

    // Hash dynamic macros via the per-shader builder (only macros this shader actually uses).
    // Shaders without a builder have no renderer-setting-dependent macros to hash.
    if ( si.macroBuilder ) {
        std::vector<D3D_SHADER_MACRO> dynamicMakros;
        si.macroBuilder( dynamicMakros );
        for ( const auto& macro : dynamicMakros ) {
            if ( macro.Name )       h = HashCombine( h, std::hash<std::string_view>{}( macro.Name ) );
            if ( macro.Definition ) h = HashCombine( h, std::hash<std::string_view>{}( macro.Definition ) );
        }
    }

    return h;
}
