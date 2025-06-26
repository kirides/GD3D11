#include "BaseAntTweakBar.h"

#include "BaseGraphicsEngine.h"
#include "GSky.h"
#include "zCMaterial.h"

#pragma comment(lib, "AntTweakBar.lib")

BaseAntTweakBar::BaseAntTweakBar() {
    Bar_General = nullptr;
    Bar_HBAO = nullptr;
    Bar_Info = nullptr;
    Bar_ShaderMakros = nullptr;
    Bar_TextureSettings = nullptr;
    IsActive = false;
    Bar_Sky = nullptr;
    TS_Active = false;
    TS_TextureName[0] = 0;
    ActiveMaterialInfo = 0;
    LastFrameActiveMaterialInfo = 0;
}

BaseAntTweakBar::~BaseAntTweakBar() {
    //TwTerminate();
}

/** Creates the resources */
XRESULT BaseAntTweakBar::Init() {
    // Sky
    Bar_Sky = TwNewBar( "Sky" );
    TwDefine( " Sky position='400 0'" );

    TwAddVarRW( Bar_Sky, "GodRays", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.EnableGodRays, nullptr );

    TwAddVarRW( Bar_Sky, "GodRayDecay", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.GodRayDecay, nullptr );
    TwDefine( " Sky/GodRayDecay  step=0.01 " );

    TwAddVarRW( Bar_Sky, "GodRayWeight", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.GodRayWeight, nullptr );
    TwDefine( " Sky/GodRayWeight  step=0.01 " );

    TwAddVarRW( Bar_Sky, "GodRayColorMod", TW_TYPE_COLOR3F, &Engine::GAPI->GetRendererState().RendererSettings.GodRayColorMod, nullptr );

    TwAddVarRW( Bar_Sky, "GodRayDensity", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.GodRayDensity, nullptr );
    TwDefine( " Sky/GodRayDensity  step=0.01 " );

    TwAddSeparator( Bar_Sky, "SkySettings", nullptr );

    TwAddVarRW( Bar_Sky, "G", TW_TYPE_FLOAT, &Engine::GAPI->GetSky()->GetAtmoshpereSettings().G, nullptr );
    TwDefine( " Sky/G  step=0.01 " );
    TwDefine( " Sky/G  help='Size of the Sun' " );

    TwAddVarRW( Bar_Sky, "RayleightScaleDepth", TW_TYPE_FLOAT, &Engine::GAPI->GetSky()->GetAtmoshpereSettings().RayleightScaleDepth, nullptr );
    TwDefine( " Sky/RayleightScaleDepth  step=0.01 " );
    TwDefine( " Sky/RayleightScaleDepth  help='Controls scattering' " );

    TwAddVarRW( Bar_Sky, "ESun", TW_TYPE_FLOAT, &Engine::GAPI->GetSky()->GetAtmoshpereSettings().ESun, nullptr );
    TwDefine( " Sky/ESun  step=0.1 " );
    TwDefine( " Sky/ESun  help='Brightness of the sun' " );

    TwAddVarRW( Bar_Sky, "InnerRadius", TW_TYPE_FLOAT, &Engine::GAPI->GetSky()->GetAtmoshpereSettings().InnerRadius, nullptr );
    TwAddVarRW( Bar_Sky, "OuterRadius", TW_TYPE_FLOAT, &Engine::GAPI->GetSky()->GetAtmoshpereSettings().OuterRadius, nullptr );
    TwDefine( " Sky/InnerRadius  help='Inner Radius of the fake-planet. This must be greater than SphereOffset.y.' " );
    TwDefine( " Sky/OuterRadius  help='Outer Radius of the fake-planet' " );

    TwAddVarRW( Bar_Sky, "Km", TW_TYPE_FLOAT, &Engine::GAPI->GetSky()->GetAtmoshpereSettings().Km, nullptr );
    TwDefine( " Sky/Km  step=0.0001 " );

    TwAddVarRW( Bar_Sky, "Kr", TW_TYPE_FLOAT, &Engine::GAPI->GetSky()->GetAtmoshpereSettings().Kr, nullptr );
    TwDefine( " Sky/Kr  step=0.0001 " );

    TwAddVarRW( Bar_Sky, "Samples", TW_TYPE_INT32, &Engine::GAPI->GetSky()->GetAtmoshpereSettings().Samples, nullptr );
    TwAddVarRW( Bar_Sky, "WaveLengths", TW_TYPE_DIR3F, &Engine::GAPI->GetSky()->GetAtmoshpereSettings().WaveLengths, nullptr );
    TwAddVarRW( Bar_Sky, "SphereOffset.y", TW_TYPE_FLOAT, &Engine::GAPI->GetSky()->GetAtmoshpereSettings().SphereOffsetY, nullptr );

    TwAddVarRW( Bar_Sky, "ReplaceSunDirection", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.ReplaceSunDirection, nullptr );
    TwDefine( " Sky/ReplaceSunDirection  help='Activates the LightDirection-Vector so you can modify the Direction of the sun' " );

    TwAddVarRW( Bar_Sky, "LightDirection", TW_TYPE_DIR3F, &Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection, nullptr );
    TwDefine( " Sky/ReplaceSunDirection  help='The direction the sun should come from. Only active when ReplaceSunDirection is active.\n"
        "Also useful to fix the sun in one position'" );

    TwAddVarRW( Bar_Sky, "SunLightColor", TW_TYPE_COLOR3F, &Engine::GAPI->GetRendererState().RendererSettings.SunLightColor, nullptr );
    TwDefine( " Sky/SunLightColor  help='Color of the sunlight' " );

    TwAddVarRW( Bar_Sky, "SunLightStrength", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.SunLightStrength, nullptr );

    TwAddVarRW( Bar_Sky, "SkyTimeScale", TW_TYPE_FLOAT, &Engine::GAPI->GetSky()->GetAtmoshpereSettings().SkyTimeScale, nullptr );
    TwDefine( " Sky/SkyTimeScale  help='This makes the skys time pass slower or faster' " );

    Bar_General = TwNewBar( "General" );
    TwDefine( " General position='600 0'" );

    TwAddButton( Bar_General, "Save ZEN-Resources", (TwButtonCallback)SaveZENResourcesCallback, this, nullptr );
    TwAddButton( Bar_General, "Load ZEN-Resources", (TwButtonCallback)LoadZENResourcesCallback, this, nullptr );
    TwAddButton( Bar_General, "Open Settings Dialog", (TwButtonCallback)OpenSettingsCallback, this, nullptr );

    TwAddVarRW( Bar_General, "Enable DebugLog", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog, nullptr );
    TwAddVarRW( Bar_General, "DisableRendering", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DisableRendering, nullptr );
    TwAddVarRW( Bar_General, "Draw VOBs", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs, nullptr );
    TwAddVarRW( Bar_General, "Draw Dynamic Vobs", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DrawDynamicVOBs, nullptr );
    TwAddVarRW( Bar_General, "Draw WorldMesh", TW_TYPE_INT32, &Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh, nullptr );
    TwAddVarRW( Bar_General, "Draw Skeletal Meshes", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes, nullptr );
    TwAddVarRW( Bar_General, "Draw Mobs", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DrawMobs, nullptr );
    TwAddVarRW( Bar_General, "Draw ParticleEffects", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DrawParticleEffects, nullptr );
    //TwAddVarRW(Bar_General, "Draw Sky", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DrawSky, nullptr);
    TwAddVarRW( Bar_General, "Draw Fog", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DrawFog, nullptr );
    TwAddVarRW( Bar_General, "Fog Range", TwDefineEnumFromString( "FogRangeEnum", "3, 4, 5, 6, 7, 8, 9, 10" ), &Engine::GAPI->GetRendererState().RendererSettings.FogRange, nullptr );

    TwAddVarRW( Bar_General, "HDR", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.EnableHDR, nullptr );
    TwEnumVal hdrToneMapValues[] = { {0, "ToneMap_jafEq4"}, {1, "Uncharted2Tonemap"}, {2, "ACESFilmTonemap"}, {3, "PerceptualQuantizerTonemap"}, {4, "ToneMap_Simple"}, {5, "ACESFittedTonemap"} };
    TwType hdrToneMapType = TwDefineEnum( "HDR_TONEMAP", hdrToneMapValues, 6 );
    TwAddVarRW( Bar_General, "HDR ToneMap", hdrToneMapType, &Engine::GAPI->GetRendererState().RendererSettings.HDRToneMap, nullptr );

    TwAddVarRW( Bar_General, "SMAA", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.EnableSMAA, nullptr );
    TwAddVarRW( Bar_General, "Sharpen", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.SharpenFactor, nullptr );
    TwDefine( " General/Sharpen  step=0.01 min=0" );

    TwAddVarRW( Bar_General, "DynamicLighting", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.EnableDynamicLighting, nullptr );

    TwType epls = TwDefineEnumFromString( "PointlightShadowsEnum", "0 {Disabled}, 1 {Static}, 2 {Update Dynamic}, 3 {Full}" );
    TwAddVarRW( Bar_General, "PointlightShadows", epls, &Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows, nullptr );

    //TwAddVarRW(Bar_General, "FastShadows", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.FastShadows, nullptr);	
    TwAddVarRW( Bar_General, "DrawShadowGeometry", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DrawShadowGeometry, nullptr );
    TwAddVarRW( Bar_General, "DoZPrepass", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DoZPrepass, nullptr );

    TwAddVarRW( Bar_General, "VSync", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.EnableVSync, nullptr );
    TwAddVarRW( Bar_General, "OcclusionCulling", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.EnableOcclusionCulling, nullptr );
    TwAddVarRW( Bar_General, "Sort RenderQueue", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.SortRenderQueue, nullptr );
    TwAddVarRW( Bar_General, "Draw Threaded", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DrawThreaded, nullptr );

    TwAddVarRW( Bar_General, "AtmosphericScattering", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.AtmosphericScattering, nullptr );
    TwAddVarRW( Bar_General, "SkeletalVertexNormals", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.ShowSkeletalVertexNormals, nullptr );

    TwType t;
    if ( FeatureLevel10Compatibility ) {
        t = TwDefineEnumFromString( "ShadowmapSizeEnum", "512, 1024, 2048, 4096, 8192" );
    } else {
        t = TwDefineEnumFromString( "ShadowmapSizeEnum", "512, 1024, 2048, 4096, 8192, 16384" );
    }
    TwAddVarRW( Bar_General, "ShadowmapSize", t, &Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize, nullptr );
    TwAddVarRW( Bar_General, "WorldShadowRangeScale", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.WorldShadowRangeScale, nullptr );
    TwDefine( " General/WorldShadowRangeScale  step=0.01 min=0" );

    TwAddVarRW( Bar_General, "ShadowStrength", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.ShadowStrength, nullptr );
    TwDefine( " General/ShadowStrength  step=0.01 min=0" );

    TwAddVarRW( Bar_General, "ShadowAOStrength", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.ShadowAOStrength, nullptr );
    TwDefine( " General/ShadowAOStrength  step=0.01 min=0" );

    TwAddVarRW( Bar_General, "WorldAOStrength", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.WorldAOStrength, nullptr );
    TwDefine( " General/WorldAOStrength  step=0.01 min=0" );

    TwAddVarRW( Bar_General, "ShadowStrength", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.ShadowStrength, nullptr );
    TwDefine( " General/ShadowStrength  step=0.01 min=0" );

    TwAddVarRW( Bar_General, "WireframeWorld", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.WireframeWorld, nullptr );
    TwAddVarRW( Bar_General, "WireframeVobs", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.WireframeVobs, nullptr );

    TwAddVarRW( Bar_General, "TesselationFactor", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.TesselationFactor, nullptr );
    TwDefine( " General/TesselationFactor  step=0.1 min=1" );

    TwAddVarRW( Bar_General, "TesselationRange", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.TesselationRange, nullptr );
    TwDefine( " General/TesselationRange  step=0.1 min=1" );

    //TwAddVarRW(Bar_General, "Grass AlphaToCoverage", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.VegetationAlphaToCoverage, nullptr);	

    TwAddVarRW( Bar_General, "SectionDrawRadius", TW_TYPE_INT32, &Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius, nullptr );
    TwDefine( " General/SectionDrawRadius  help='Draw distance for the sections' " );

    TwAddVarRW( Bar_General, "OutdoorVobDrawRadius", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius, nullptr );
    TwDefine( " General/OutdoorVobDrawRadius  help='Draw distance for the static outdoor vobs' " );

    TwAddVarRW( Bar_General, "IndoorVobDrawRadius", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.IndoorVobDrawRadius, nullptr );
    TwDefine( " General/IndoorVobDrawRadius  help='Draw distance for the static indoor vobs' " );

    TwAddVarRW( Bar_General, "OutdoorSmallVobRadius", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.OutdoorSmallVobDrawRadius, nullptr );
    TwAddVarRW( Bar_General, "SkeletalMeshDrawRadius", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.SkeletalMeshDrawRadius, nullptr );

    TwAddVarRW( Bar_General, "VisualFXDrawRadius", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius, nullptr );

    TwAddVarRW( Bar_General, "RainRadius", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.RainRadiusRange, nullptr );
    TwAddVarRW( Bar_General, "RainHeight", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.RainHeightRange, nullptr );
    TwAddVarRW( Bar_General, "NumRainParticles", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererSettings.RainNumParticles, nullptr );
    TwDefine( " General/NumRainParticles  step=1000" );

    TwAddVarRW( Bar_General, "RainMoveParticles", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.RainMoveParticles, nullptr );

    TwAddVarRW( Bar_General, "RainGlobalVelocity", TW_TYPE_DIR3F, &Engine::GAPI->GetRendererState().RendererSettings.RainGlobalVelocity, nullptr );
    TwAddVarRW( Bar_General, "RainSceneWettness", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.RainSceneWettness, nullptr );
    TwDefine( " General/RainSceneWettness  step=0.01 min=0 max=1" );

    TwAddVarRW( Bar_General, "RainSunLightStrength", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.RainSunLightStrength, nullptr );
    TwDefine( " General/RainSunLightStrength  step=0.01 min=0" );

    TwAddVarRW( Bar_General, "RainFogDensity", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.RainFogDensity, nullptr );
    TwDefine( " General/RainFogDensity  step=0.00001 min=0" );

    TwAddVarRW( Bar_General, "RainFogColor", TW_TYPE_COLOR3F, &Engine::GAPI->GetRendererState().RendererSettings.RainFogColor, nullptr );

    //TwAddVarRW(Bar_General, "SmallVobSize", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.SmallVobSize, nullptr);
    //TwAddVarRW(Bar_General, "AtmosphericScattering", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.AtmosphericScattering, nullptr);

    TwAddVarRW( Bar_General, "FogGlobalDensity", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.FogGlobalDensity, nullptr );
    TwDefine( " General/FogGlobalDensity  step=0.00001 min=0" );
    TwDefine( " General/FogGlobalDensity  help='Controls thickness of the fog' " );

    TwAddVarRW( Bar_General, "FogHeightFalloff", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.FogHeightFalloff, nullptr );
    TwDefine( " General/FogHeightFalloff  step=0.00001 min=0" );
    TwDefine( " General/FogHeightFalloff  help='Controls how sharp the falloff of the fog should be' " );

    TwAddVarRW( Bar_General, "FogHeight", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.FogHeight, nullptr );
    TwDefine( " General/FogHeight  help='Height at where the Fog should start' " );

    TwAddVarRW( Bar_General, "FogColorMod", TW_TYPE_COLOR3F, &Engine::GAPI->GetRendererState().RendererSettings.FogColorMod, nullptr );
    TwDefine( " General/FogColorMod  help='Color of the fog' " );

    TwAddVarRW( Bar_General, "HDRLumWhite", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.HDRLumWhite, nullptr );
    TwDefine( " General/HDRLumWhite  step=0.01" );
    TwDefine( " General/HDRLumWhite" );

    TwAddVarRW( Bar_General, "HDRMiddleGray", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.HDRMiddleGray, nullptr );
    TwDefine( " General/HDRMiddleGray  step=0.01" );
    TwDefine( " General/HDRMiddleGray" );

    TwAddVarRW( Bar_General, "BloomThreshold", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.BloomThreshold, nullptr );
    TwDefine( " General/BloomThreshold  step=0.01" );
    TwDefine( " General/BloomThreshold" );

    TwAddVarRW( Bar_General, "BloomStrength", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.BloomStrength, nullptr );
    TwDefine( " General/BloomStrength  step=0.01" );
    TwDefine( " General/BloomStrength" );

    TwAddVarRW( Bar_General, "WindStrength", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.GlobalWindStrength, nullptr );
    TwDefine( " General/WindStrength  step=0.01" );

    TwAddVarRW( Bar_General, "LockViewFrustum", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.FixViewFrustum, nullptr );
    TwAddVarRW( Bar_General, "DisableWatermark", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DisableWatermark, nullptr );

    TwAddVarRW( Bar_General, "GothicUIScale", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale, nullptr );
    TwDefine( " General/GothicUIScale  step=0.01" );
    TwAddVarRW( Bar_General, "FOVHoriz", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.FOVHoriz, nullptr );
    TwAddVarRW( Bar_General, "FOVVert", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.FOVVert, nullptr );
    TwAddVarRW( Bar_General, "ForceFOV", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.ForceFOV, nullptr );
    //TwAddVarRW(Bar_General, "Max Num Indices", TW_TYPE_INT32, &Engine::GAPI->GetRendererState().RendererSettings.MaxNumFaces, nullptr);

#ifdef BUILD_GOTHIC_1_08k
    TwAddVarRW( Bar_General, "DrawForestPortals", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.DrawG1ForestPortals, nullptr );
#endif

    Bar_Info = TwNewBar( "FrameStats" );
    TwDefine( " FrameStats refresh=0.3" );
    TwDefine( " FrameStats position='800 0'" );
    //TwAddVarRO(Bar_Info, "Version", TW_TYPE_CDSTRING, VERSION_NUMBER_STR, nullptr);
    TwAddVarRO( Bar_Info, "FPS", TW_TYPE_INT32, &Engine::GAPI->GetRendererState().RendererInfo.FPS, nullptr );
    TwAddVarRO( Bar_Info, "StateChanges", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChanges, nullptr );
    TwAddVarRO( Bar_Info, "DrawnVobs", TW_TYPE_INT32, &Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnVobs, nullptr );
    TwAddVarRO( Bar_Info, "DrawnTriangles", TW_TYPE_INT32, &Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles, nullptr );
    TwAddVarRO( Bar_Info, "VobUpdates", TW_TYPE_INT32, &Engine::GAPI->GetRendererState().RendererInfo.FrameVobUpdates, nullptr );
    TwAddVarRO( Bar_Info, "DrawnLights", TW_TYPE_INT32, &Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnLights, nullptr );
    TwAddVarRO( Bar_Info, "SectionsDrawn", TW_TYPE_INT32, &Engine::GAPI->GetRendererState().RendererInfo.FrameNumSectionsDrawn, nullptr );
    TwAddVarRO( Bar_Info, "WorldMeshDrawCalls", TW_TYPE_INT32, &Engine::GAPI->GetRendererState().RendererInfo.WorldMeshDrawCalls, nullptr );

    TwAddVarRO( Bar_Info, "FarPlane", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererInfo.FarPlane, nullptr );
    TwAddVarRO( Bar_Info, "NearPlane", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererInfo.NearPlane, nullptr );
    //TwAddVarRO(Bar_Info, "VOBVerticesDataSize", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize, nullptr);
    //TwAddVarRO(Bar_Info, "SkeletalVerticesDataSize", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.SkeletalVerticesDataSize, nullptr);

    TwAddVarRO( Bar_Info, "WorldMeshMS", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererInfo.Timing.WorldMeshMS, nullptr );
    TwAddVarRO( Bar_Info, "VobsMS", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererInfo.Timing.VobsMS, nullptr );
    TwAddVarRO( Bar_Info, "SkeletalMeshesMS", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererInfo.Timing.SkeletalMeshesMS, nullptr );
    TwAddVarRO( Bar_Info, "LightingMS", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererInfo.Timing.LightingMS, nullptr );
    TwAddVarRO( Bar_Info, "TotalMS", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererInfo.Timing.TotalMS, nullptr );

    TwAddVarRO( Bar_Info, "SC_PipelineStates,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.FramePipelineStates, nullptr );
    TwAddVarRO( Bar_Info, "SC_Textures,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_TX], nullptr );
    TwAddVarRO( Bar_Info, "SC_ConstantBuffer,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_CB], nullptr );
    TwAddVarRO( Bar_Info, "SC_GeometryShader,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_GS], nullptr );
    TwAddVarRO( Bar_Info, "SC_RTVDSV,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_RTVDSV], nullptr );
    TwAddVarRO( Bar_Info, "SC_DomainShader,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_DS], nullptr );
    TwAddVarRO( Bar_Info, "SC_HullShader,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_HS], nullptr );
    TwAddVarRO( Bar_Info, "SC_PixelShader,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_PS], nullptr );
    TwAddVarRO( Bar_Info, "SC_InputLayout,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_IL], nullptr );
    TwAddVarRO( Bar_Info, "SC_VertexShader,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_VS], nullptr );
    TwAddVarRO( Bar_Info, "SC_IndexBuffer,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_IB], nullptr );
    TwAddVarRO( Bar_Info, "SC_VertexBuffer,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_VB], nullptr );
    TwAddVarRO( Bar_Info, "SC_RasterizerState,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_RS], nullptr );
    TwAddVarRO( Bar_Info, "SC_DepthStencilState,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_DSS], nullptr );
    TwAddVarRO( Bar_Info, "SC_SamplerState,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_SMPL], nullptr );
    TwAddVarRO( Bar_Info, "SC_BlendState,", TW_TYPE_UINT32, &Engine::GAPI->GetRendererState().RendererInfo.StateChangesByState[GothicRendererInfo::SC_BS], nullptr );

    Bar_HBAO = TwNewBar( "HBAO+" );
    TwDefine( " HBAO+ position='1000 0'" );

    /** --- hbao  --- */
    TwAddVarRW( Bar_HBAO, "Enable HBAO+", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.Enabled, nullptr );

    TwAddVarRW( Bar_HBAO, "Radius", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.Radius, nullptr );
    TwDefine( " HBAO+/Radius  step=0.01 min=0 " );

    TwAddVarRW( Bar_HBAO, "MetersToViewSpaceUnits", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.MetersToViewSpaceUnits, nullptr );
    TwDefine( " HBAO+/MetersToViewSpaceUnits  step=0.01 min=0 " );

    TwAddVarRW( Bar_HBAO, "PowerExponent", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.PowerExponent, nullptr );
    TwDefine( " HBAO+/PowerExponent  step=0.01 min=1.0 max=4.0 " );

    TwAddVarRW( Bar_HBAO, "Bias", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.Bias, nullptr );
    TwDefine( " HBAO+/Bias  step=0.01 min=0.0 max=0.5 " );

    TwAddVarRW( Bar_HBAO, "Enable Blur", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.EnableBlur, nullptr );

    TwEnumVal hbaoBlurRadii[] = { {0, "2"}, {1, "4"} };
    TwType hbaoBlurRadiiType = TwDefineEnum( "GFSDK_SSAO_BLUR_RADIUS", hbaoBlurRadii, 2 );
    TwAddVarRW( Bar_HBAO, "SSAO radius", hbaoBlurRadiiType, &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.SsaoBlurRadius, nullptr );

    TwAddVarRW( Bar_HBAO, "BlurSharpness", TW_TYPE_FLOAT, &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.BlurSharpness, nullptr );
    TwDefine( " HBAO+/BlurSharpness  step=0.01" );

    TwType tbm = TwDefineEnumFromString( "BlendModeEnum", "0 {Replace}, 1 {Multiply}" );
    TwAddVarRW( Bar_HBAO, "BlendMode", tbm, &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.BlendMode, nullptr );

    //TwAddVarRW( Bar_HBAO, "DualLayerAO", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.EnableDualLayerAO, nullptr );

    TwEnumVal hbaoStepCounts[] = { {0, "4"}, {1, "8"} };
    TwType hbaoStepCountType = TwDefineEnum( "GFSDK_SSAO_STEP_COUNT", hbaoStepCounts, 2 );
    TwAddVarRW( Bar_HBAO, "SSAO steps", hbaoStepCountType, &Engine::GAPI->GetRendererState().RendererSettings.HbaoSettings.SsaoStepCount, nullptr );

    Bar_ShaderMakros = TwNewBar( "ShaderMakros" );
    TwDefine( " ShaderMakros position='1200 0'" );

    TwAddSeparator( Bar_ShaderMakros, "Reload shaders to apply changes!", nullptr );
    TwAddVarRW( Bar_ShaderMakros, "Enable Shadows", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.EnableShadows, nullptr );
    TwAddVarRW( Bar_ShaderMakros, "Filter shadows", TW_TYPE_BOOLCPP, &Engine::GAPI->GetRendererState().RendererSettings.EnableSoftShadows, nullptr );
    TwAddButton( Bar_ShaderMakros, "Apply", (TwButtonCallback)ReloadShadersButtonCallback, this, nullptr );

    /*Bar_TextureSettings = TwNewBar("TextureSettings");
    TwAddVarRW(Bar_TextureSettings, "Texture", TW_TYPE_CSSTRING(256), TS_TextureName, "");
    TwDefine(" TextureSettings size='455 180' valueswidth=200 position='400 320'");*/

    return XR_SUCCESS;
}

/** On window message */
LRESULT BaseAntTweakBar::OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) {
    if ( IsActive ) {
        return TwEventWin( hWnd, msg, wParam, lParam );
    }

    return true;
}

/** Sets the bars active and visible */
void BaseAntTweakBar::SetActive( bool value ) {
    IsActive = value;

    if ( IsActive ) {
        //InitTextureSettingsBar();
    }
}

bool BaseAntTweakBar::GetActive() {
    return IsActive;
}

/** Draws the bars */
void BaseAntTweakBar::Draw() {
    if ( IsActive ) {
        TwDraw();

        // Check texture name
        /*if (LastFrameActiveMaterialInfo != ActiveMaterialInfo)
        {
            if (ActiveMaterialInfo)
            {
                // Update the bar
                UpdateTextureSettingsBar();
            }

            LastFrameActiveMaterialInfo = ActiveMaterialInfo;
        }

        // Check for change
        if (TS_Active)
        {
            if (ActiveMaterialInfo)
            {
                // Check for a change
                if (memcmp(&TS_OldMaterialInfo, &TS_FrameTexturesInfos[ActiveMaterialInfo].Info->buffer, sizeof(MaterialInfo)) != 0)
                {
                    TS_FrameTexturesInfos[ActiveMaterialInfo].Info->WriteToFile(TS_FrameTexturesInfos[ActiveMaterialInfo].Name);
                    TS_OldMaterialInfo = *TS_FrameTexturesInfos[ActiveMaterialInfo].Info;

                    LogInfo() << "Saved MaterialInfo: " << TS_TextureName;
                }
            }

            if (ActiveMaterialInfo && TS_FrameTexturesInfos[ActiveMaterialInfo].Texture)
            {
                const int previewSize = 512;
                const int previewDownscale = 2;
                INT2 pPosition;
                pPosition.x = Engine::GraphicsEngine->GetResolution().x - (previewSize / previewDownscale);
                pPosition.y = 0;

                INT2 pSize = INT2(previewSize / previewDownscale,previewSize / previewDownscale);

                TS_FrameTexturesInfos[ActiveMaterialInfo].Texture->Bind(0);
                Engine::GraphicsEngine->DrawQuad(pPosition, pSize);

                // Do this every frame on this one info to get the changes to the constantbuffer from the anttweakbar
                TS_FrameTexturesInfos[ActiveMaterialInfo].Info->UpdateConstantbuffer();
            }
        }*/
    }
}

/** Updates the TS_Bar */
void BaseAntTweakBar::UpdateTextureSettingsBar() {
    /*TwRemoveVar(Bar_TextureSettings, "NormalmapDepth");
    TwRemoveVar(Bar_TextureSettings, "SpecularIntensity");
    TwRemoveVar(Bar_TextureSettings, "SpecularPower");
    TwRemoveVar(Bar_TextureSettings, "DisplacementFactor");
    TwRemoveVar(Bar_TextureSettings, "TriplanarTextureScale");

    TwAddVarRW(Bar_TextureSettings, "NormalmapDepth", TW_TYPE_FLOAT, &TS_FrameTexturesInfos[ActiveMaterialInfo].Info->buffer.NormalmapStrength, nullptr);
    TwDefine(" TextureSettings/NormalmapDepth  step=0.01");

    TwAddVarRW(Bar_TextureSettings, "SpecularIntensity", TW_TYPE_FLOAT, &TS_FrameTexturesInfos[ActiveMaterialInfo].Info->buffer.SpecularIntensity, nullptr);
    TwDefine(" TextureSettings/SpecularIntensity  step=0.01");

    TwAddVarRW(Bar_TextureSettings, "SpecularPower", TW_TYPE_FLOAT, &TS_FrameTexturesInfos[ActiveMaterialInfo].Info->buffer.SpecularPower, nullptr);
    TwDefine(" TextureSettings/SpecularPower  step=0.1");

    TwAddVarRW(Bar_TextureSettings, "DisplacementFactor", TW_TYPE_FLOAT, &TS_FrameTexturesInfos[ActiveMaterialInfo].Info->buffer.DisplacementFactor, nullptr);
    TwDefine(" TextureSettings/DisplacementFactor  step=0.01");

    TwAddVarRW(Bar_TextureSettings, "TriplanarTextureScale", TW_TYPE_FLOAT, &TS_FrameTexturesInfos[ActiveMaterialInfo].Info->buffer.TextureScale, nullptr);
    TwDefine(" TextureSettings/TriplanarTextureScale  step=0.1");

    TwAddVarRW(Bar_TextureSettings, "TriplanarFresnelFactor", TW_TYPE_FLOAT, &TS_FrameTexturesInfos[ActiveMaterialInfo].Info->buffer.FresnelFactor, nullptr);
    TwDefine(" TextureSettings/TriplanarFresnelFactor  step=0.01");*/
}

static bool FrameTexEnumCompare( TS_TextureInfo& a, TS_TextureInfo& b ) {
    return a.Name < b.Name;
}

/** Initializes the TextureSettingsBar */
void BaseAntTweakBar::InitTextureSettingsBar() {
    TwRemoveAllVars( Bar_TextureSettings );
    //TwRemoveVar(Bar_TextureSettings, "FrameTextures");
    TS_FrameTexturesInfos.clear();

    std::vector<TS_TextureInfo> e;

    std::string enumDef = "";

    static std::set<zCTexture*> emptyTextures;
    const std::set<zCTexture*>& frameTextures = emptyTextures;
    if ( !frameTextures.size() ) {
        return;
    }

    for ( std::set<zCTexture*>::const_iterator it = frameTextures.cbegin(); it != frameTextures.cend(); ++it ) {
        e.push_back( TS_TextureInfo( Engine::GAPI->GetMaterialInfoFrom( (*it) ), (*it)->GetNameWithoutExt(), (*it) ) );
    }

    // Sort alphabetically
    std::sort( e.begin(), e.end(), FrameTexEnumCompare );

    int prefTexIdx = 0;
    for ( unsigned int i = 0; i < e.size(); i++ ) {
        // Pointer to int. Since Gothic is 32-Bit this can't run in 64-bit mode anyways.
        enumDef += std::to_string( i ) + " {" + e[i].Name + "}";

        TS_FrameTexturesInfos.push_back( TS_TextureInfo( e[i].Info, e[i].Name, e[i].Texture ) );

        if ( e[i].Name == TS_PreferredTexture ) {
            prefTexIdx = i;
            ActiveMaterialInfo = i;
        }

        if ( i != e.size() - 1 ) {
            enumDef += ", ";
        }
    }

    enumDef += "";

    // Add the list to the bar
    TwType t = TwDefineEnumFromString( "frameTextureList", enumDef.c_str() );
    std::string def = std::to_string( prefTexIdx ) + " {" + TS_PreferredTexture + "}";
    TwAddVarRW( Bar_TextureSettings, "FrameTextures", t, &ActiveMaterialInfo, nullptr );

    TS_Active = true;
}

/** Sets the preferred texture for the texture settings */
void BaseAntTweakBar::SetPreferredTextureForSettings( const std::string& texture ) {
    TS_PreferredTexture = texture;
}

void TW_CALL BaseAntTweakBar::ReloadShadersButtonCallback( void* clientData ) {
    Engine::GraphicsEngine->ReloadShaders();
}

void TW_CALL BaseAntTweakBar::SaveZENResourcesCallback( void* clientData ) {
    Engine::GAPI->SaveCustomZENResources();
}

void TW_CALL BaseAntTweakBar::LoadZENResourcesCallback( void* clientData ) {
    Engine::GAPI->LoadCustomZENResources();
}

/** Called on load ZEN resources */
void TW_CALL BaseAntTweakBar::OpenSettingsCallback( void* clientdata ) {
    Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::EUIEvent::UI_OpenSettings );
}

/** Resizes the anttweakbar */
XRESULT BaseAntTweakBar::OnResize( INT2 newRes ) {
    TwWindowSize( newRes.x, newRes.y );

    return XR_SUCCESS;
}
