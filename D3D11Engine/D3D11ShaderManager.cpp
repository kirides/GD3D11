#include "pch.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11HDShader.h"
#include "D3D11GShader.h"
#include "D3D11CShader.h"
#include "D3D11ConstantBuffer.h"
#include "GothicGraphicsState.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "Engine.h"
#include "ThreadPool.h"

#include "D3D11GraphicsEngineBase.h"
#include <d3dcompiler.h>
#include "D3D11PFX_TAA.h"
#include "D3D11FileRelativeInclude.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

// Patch HLSL-Compiler for http://support.microsoft.com/kb/2448404
#if D3DX_VERSION == 0xa2b
#pragma ruledisable 0x0802405f
#endif

#include <fstream>
#include <unordered_map>

const int NUM_MAX_BONES = 96;

extern bool FeatureLevel10Compatibility;
extern bool FeatureRTArrayIndexFromAnyShader;
#if !defined(BUILD_GOTHIC_2_6_fix) && !defined(BUILD_1_12F)
extern bool haveWindAnimations;
#endif

D3D11ShaderManager::D3D11ShaderManager()
    : VShaders( static_cast<size_t>(VShaderID::COUNT) )
    , PShaders( static_cast<size_t>(PShaderID::COUNT) )
    , HDShaders( static_cast<size_t>(HDShaderID::COUNT) )
    , GShaders( static_cast<size_t>(GShaderID::COUNT) )
    , CShaders( static_cast<size_t>(CShaderID::COUNT) )
    , ShaderCategoriesToReloadNextFrame( ShaderCategory::None )
{
}

D3D11ShaderManager::~D3D11ShaderManager() {
    DeleteShaders();
}

//--------------------------------------------------------------------------------------
// Find and compile the specified shader
//--------------------------------------------------------------------------------------
HRESULT D3D11ShaderManager::CompileShaderFromFile( const CHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros ) {
    auto shaderFile = Toolbox::ToWideChar( szFileName );

    return CompileShaderFromFile(
        shaderFile.c_str(),
        szEntryPoint,
        szShaderModel,
        ppBlobOut,
        makros);
}

HRESULT D3D11ShaderManager::CompileShaderFromFile( const WCHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros ) {
    HRESULT hr = S_OK;

    DWORD dwShaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(DEBUG_D3D11)
    // Set the D3DCOMPILE_DEBUG flag to embed debug information in the shaders.
    // Setting this flag improves the shader debugging experience, but still allows
    // the shaders to be optimized and to run exactly the way they will run in
    // the release configuration of this program.
    dwShaderFlags |= D3DCOMPILE_DEBUG
    // | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG_NAME_FOR_SOURCE // Very expensive, only use to debug shaders
    ;
#else
#endif
    dwShaderFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3
    ;

    // Build the final macro list, adding the required null terminator for D3DCompileFromFile
    std::vector<D3D_SHADER_MACRO> m = makros;
    m.push_back( {nullptr, nullptr} );

    Microsoft::WRL::ComPtr<ID3DBlob> pErrorBlob;

    std::filesystem::path shaderPath( szFileName );

    // absolute path
    shaderPath = Engine::GAPI->GetStartDirectory().c_str() / shaderPath;

    D3D11FileRelativeInclude includeHandler( shaderPath.parent_path() );

    hr = D3DCompileFromFile( shaderPath.wstring().c_str(), &m[0], &includeHandler, szEntryPoint, szShaderModel, dwShaderFlags, 0, ppBlobOut, &pErrorBlob);

    if ( FAILED( hr ) ) {
        LogInfo() << "Shader compilation failed!";
        if ( pErrorBlob.Get() ) {
            LogErrorBox() << reinterpret_cast<char*>(pErrorBlob->GetBufferPointer()) << "\n\n (You can ignore the next error from Gothic about too small video memory!)";
        }

        return hr;
    }

    return S_OK;
}

/** Creates list with ShaderInfos */
XRESULT D3D11ShaderManager::Init() {
    Shaders = std::vector<ShaderInfo>();
    static const char* sNums[] = { "0","1","2","3","4","5","6","7","8","9","10","11","12","13","14","15" };

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Ex>( "VS_Ex.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExNode>( "VS_ExNode.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 ) );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_Decal>( "VS_Decal.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_DecalInstanced>( "VS_DecalInstanced.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_15_VS_DecalInstanced )  );

    Shaders.push_back( ShaderInfo::make<VShaderID::VS_ExWater>( "VS_ExWater.hlsl" )
        .with_layout( VERTEX_INPUT_LAYOUT_1 )
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
        .with_category( ShaderCategory::Water )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_ParticleDistortion>( "PS_ParticleDistortion.hlsl" )  );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_PFX_ApplyParticleDistortion>( "PS_PFX_ApplyParticleDistortion.hlsl" ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_Grass>( "PS_Grass.hlsl" ) );

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
            list.push_back( { "COMPOSE_SAO", (s.AoMode == AOMode::AO_SAO) ? "1" : "0" } );
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
        .with_macros( {
            {"NORMALMAPPING", "1"},
            {"ALPHATEST", "0"},
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmappedFxMap>( "PS_Diffuse.hlsl" )
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
        .with_macros( {
            {"NORMALMAPPING", "1"},
            {"ALPHATEST", "1"},
        } ) );

    Shaders.push_back( ShaderInfo::make<PShaderID::PS_DiffuseNormalmappedAlphaTestFxMap>( "PS_Diffuse.hlsl" )
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
            .with_layout( VERTEX_INPUT_LAYOUT_1 )  );

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
            .with_layout( VERTEX_INPUT_LAYOUT_1 )  );

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

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_SAO_Blur>( "CS_PFX_SAO_Blur.hlsl" ));

        Shaders.push_back( ShaderInfo::make<CShaderID::CS_PFX_Sharpen>( "CS_PFX_Sharpen.hlsl" ));

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
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "1" },
                { "ALPHATEST", "0" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_DiffuseNormalmappedFxMap>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
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
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "1" },
                { "ALPHATEST", "1" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_DiffuseNormalmappedAlphaTestFxMap>( "PS_Diffuse.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_macros( {
                { "FORWARD_PLUS", "1" },
                { "NORMALMAPPING", "1" },
                { "ALPHATEST", "1" },
                { "FXMAP", "1" },
            } ).with_category( ShaderCategory::LightsAndShadows ) );

        Shaders.push_back( ShaderInfo::make<PShaderID::PS_FP_ShadowMask>( "PS_FP_ShadowMask.hlsl" )
            .with_macros(shadowMacroBuilder)
            .with_category( ShaderCategory::LightsAndShadows ) );
    }

    return XR_SUCCESS;
}

static size_t HashCombine( size_t seed, size_t val ) noexcept {
    return seed ^ (val + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

static size_t ComputeShaderHash( const ShaderInfo& si ) {
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

XRESULT D3D11ShaderManager::CompileShader( ShaderInfo& si ) {
    // Compute hash (file timestamp + per-shader macros + global renderer macros).
    // Skip recompilation when the shader is already loaded and nothing has changed.
    size_t newHash = ComputeShaderHash( si );

    auto IsKnown = [&]() -> bool {
        switch ( si.type ) {
        case ShaderType::Vertex:     return IsVShaderKnown( si.shaderIndex );
        case ShaderType::Pixel:      return IsPShaderKnown( si.shaderIndex );
        case ShaderType::Geometry:   return IsGShaderKnown( si.shaderIndex );
        case ShaderType::HullDomain: return IsHDShaderKnown( si.shaderIndex );
        case ShaderType::Compute:    return IsCShaderKnown( si.shaderIndex );
        default: return false;
        }
    };

    if ( IsKnown() && newHash != 0 && si.compiledHash == newHash ) {
        return XR_SUCCESS;
    }

    // Build compile-time macro list: static shaderMakros merged with any dynamic builder macros.
    std::vector<D3D_SHADER_MACRO> compileMakros = si.shaderMakros;
    if ( si.macroBuilder ) {
        si.macroBuilder( compileMakros );
    }

    //Check if shader src-file exists
    std::string fileName = Engine::GAPI->GetStartDirectory() + "\\system\\GD3D11\\shaders\\" + si.fileName;
    std::error_code ec;
    if ( std::filesystem::exists(fileName, ec)) {
        //Check shader's type
        if ( si.type == ShaderType::Vertex ) {
            // See if this is a reload
            D3D11VShader* vs = new D3D11VShader();
            if ( IsVShaderKnown( si.shaderIndex ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != vs->LoadShader( si, compileMakros, ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete vs;
                } else {
                    UpdateVShader( si.shaderIndex, vs );
                    si.compiledHash = newHash;
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( vs->LoadShader( si, compileMakros, ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) );
                UpdateVShader( si.shaderIndex, vs );
                si.compiledHash = newHash;
            }
        } else if ( si.type == ShaderType::Pixel ) {
            // See if this is a reload
            D3D11PShader* ps = new D3D11PShader();
            if ( IsPShaderKnown( si.shaderIndex ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != ps->LoadShader( si, compileMakros, ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete ps;
                } else {
                    UpdatePShader( si.shaderIndex, ps );
                    si.compiledHash = newHash;
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( ps->LoadShader( si, compileMakros, ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) );
                UpdatePShader( si.shaderIndex, ps );
                si.compiledHash = newHash;
            }
        } else if ( si.type == ShaderType::Geometry ) {
            // See if this is a reload
            D3D11GShader* gs = new D3D11GShader();
            if ( IsGShaderKnown( si.shaderIndex ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != gs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), compileMakros, si.layout != 0, si.layout ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete gs;
                } else {
                    // Compilation succeeded, switch the shader
                    UpdateGShader( si.shaderIndex, gs );
                    si.compiledHash = newHash;
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( gs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), compileMakros, si.layout != 0, si.layout ) );
                UpdateGShader( si.shaderIndex, gs );
                si.compiledHash = newHash;
            }
        } else if ( si.type == ShaderType::Compute ) {
            // See if this is a reload
            D3D11CShader* cs = new D3D11CShader();
            if ( IsCShaderKnown( si.shaderIndex ) ) {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Reloading shader: " << si.name;

                if ( XR_SUCCESS != cs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), !si.entryPoint.empty() ? si.entryPoint.c_str() : nullptr, compileMakros ) ) {
                    LogError() << "Failed to reload shader: " << si.fileName;

                    delete cs;
                } else {
                    UpdateCShader( si.shaderIndex, cs );
                    si.compiledHash = newHash;
                }
            } else {
                if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
                    LogInfo() << "Loading shader: " << si.name;

                XLE( cs->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(), !si.entryPoint.empty() ? si.entryPoint.c_str() : nullptr, compileMakros ) );
                UpdateCShader( si.shaderIndex, cs );
                si.compiledHash = newHash;
            }
        }
    }

    // Hull/Domain shaders are handled differently, they check inside for missing file
    if ( si.type == ShaderType::HullDomain ) {
        // See if this is a reload
        D3D11HDShader* hds = new D3D11HDShader();
        if ( IsHDShaderKnown( si.shaderIndex ) ) {
            if ( XR_SUCCESS != hds->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(),
                ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) ) {
                LogError() << "Failed to reload shader: " << si.fileName;

                delete hds;
            } else {
                // Compilation succeeded, switch the shader
                UpdateHDShader( si.shaderIndex, hds );
                si.compiledHash = newHash;
            }
        } else {
            XLE( hds->LoadShader( ("system\\GD3D11\\shaders\\" + si.fileName).c_str(),
                ("system\\GD3D11\\shaders\\" + si.fileName).c_str() ) );
            UpdateHDShader( si.shaderIndex, hds );
            si.compiledHash = newHash;
        }
    }
    return XR_SUCCESS;
}

/** Loads/Compiles Shaderes from list */
XRESULT D3D11ShaderManager::LoadShaders( ShaderCategory categories ) {
    // Temporarily disable multi-core shader compilation

    /*size_t numThreads = std::thread::hardware_concurrency();
    if ( numThreads > 1 ) {
        numThreads = numThreads - 1;
    }
    auto compilationTP = std::make_unique<ThreadPool>( numThreads );
    LogInfo() << "Compiling/Reloading shaders with " << compilationTP->getNumThreads() << " threads";
    */
    LogInfo() << "Compiling/Reloading shaders";
    for ( ShaderInfo& si : Shaders ) {
        // Determine shader type category
        ShaderCategory shaderTypeCategory = ShaderCategory::None;
        if ( si.type == ShaderType::Vertex ) {
            shaderTypeCategory = ShaderCategory::Vertex;
        } else if ( si.type == ShaderType::Pixel ) {
            shaderTypeCategory = ShaderCategory::Pixel;
        } else if ( si.type == ShaderType::Geometry ) {
            shaderTypeCategory = ShaderCategory::Geometry;
        } else if ( si.type == ShaderType::HullDomain ) {
            shaderTypeCategory = ShaderCategory::HullDomain;
        } else if ( si.type == ShaderType::Compute ) {
            shaderTypeCategory = ShaderCategory::Compute;
        }

        // Check if shader type matches requested categories
        bool typeMatches = HasCategory( categories, shaderTypeCategory );

        // Check if shader content category matches requested categories
        bool contentMatches = HasCategory( categories, si.contentCategory );

        if ( !typeMatches && !contentMatches ) {
            // Skip if neither type nor content category matches
            continue;
        }

        CompileShader( si );
        // compilationTP->enqueue( [this, si]() { CompileShader( si ); } );
    }

    // Join all threads (call Threadpool destructor)
    // compilationTP.reset();

    return XR_SUCCESS;
}

/** Deletes all shaders and loads them again */
XRESULT D3D11ShaderManager::ReloadShaders( ShaderCategory categories ) {
    ShaderCategoriesToReloadNextFrame |= categories;

    return XR_SUCCESS;
}

/** Called on frame start */
XRESULT D3D11ShaderManager::OnFrameStart() {
    if ( ShaderCategoriesToReloadNextFrame != ShaderCategory::None ) {
        LoadShaders( ShaderCategoriesToReloadNextFrame );
        ShaderCategoriesToReloadNextFrame = ShaderCategory::None;
    }

    return XR_SUCCESS;
}

/** Deletes all shaders */
XRESULT D3D11ShaderManager::DeleteShaders() {
    for ( auto& shader : VShaders ) {
        shader.reset();
    }
    for ( auto& shader : PShaders ) {
        shader.reset();
    }
    for ( auto& shader : HDShaders ) {
        shader.reset();
    }
    for ( auto& shader : GShaders ) {
        shader.reset();
    }
    for ( auto& shader : CShaders ) {
        shader.reset();
    }

    return XR_SUCCESS;
}

void D3D11ShaderManager::UpdateShaderInfo( ShaderInfo& shader ) {
    for ( size_t i = 0; i < Shaders.size(); i++ ) {
        if ( Shaders[i].type == shader.type && Shaders[i].shaderIndex == shader.shaderIndex ) {
            Shaders[i] = shader;
            CompileShader( Shaders[i] );
            return;
        }
    }
    Shaders.push_back( shader );
    CompileShader( Shaders.back() );
}
