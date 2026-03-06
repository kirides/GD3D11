#pragma once
#include <cstddef>
#include <parallel_hashmap/phmap.h>

enum class VShaderID : size_t {
    VS_Ex,
    VS_ExNode,
    VS_Decal,
    VS_ExWater,
    VS_ParticlePoint,
    VS_ParticlePointShaded,
    VS_ExWS,
    VS_ExSkeletal,
    VS_ExSkeletalVN,
    VS_TransformedEx,
    VS_ExPointLight,
    VS_XYZRHW_DIF_T1,
    VS_ExInstancedObj,
    VS_ExInstanced,
    VS_ExInstancedObjIndirectAtlas,
    VS_GrassInstanced,
    VS_Lines,
    VS_Lines_XYZRHW,
    VS_PFX,
    VS_CinemaScope,
    VS_AdvanceRain,
    VS_ExLayered,
    VS_ExNodeLayered,
    VS_ExSkeletalLayered,
    VS_ExCube,
    VS_ExNodeCube,
    VS_ExSkeletalCube,
    COUNT
};

enum class PShaderID : size_t {
    PS_Lines,
    PS_LinesSel,
    PS_Simple,
    PS_Rain,
    PS_Rain_Snow,
    PS_Transparency,
    PS_World,
    PS_Water,
    PS_ParticleDistortion,
    PS_PFX_ApplyParticleDistortion,
    PS_Grass,
    PS_PFX_Simple,
    PS_PFX_VelocityDebug,
    PS_PFX_GaussBlur,
    PS_PFX_Heightfog,
    PS_PFX_UnderwaterFinal,
    PS_PFX_Alpha_Blend,
    PS_PFX_CinemaScope,
    PS_PFX_DistanceBlur,
    PS_PFX_LumConvert,
    PS_PFX_LumAdapt,
    PS_PFX_HDR,
    PS_PFX_GodRayMask,
    PS_PFX_GodRayZoom,
    PS_PFX_Tonemap,
    PS_AtmosphereGround,
    PS_Atmosphere,
    PS_AtmosphereOuter,
    PS_FixedFunctionPipe,
    PS_Video,
    PS_DS_PointLight,
    PS_DS_PointLightDynShadow,
    PS_DS_AtmosphericScattering,
    PS_Diffuse,
    PS_PortalDiffuse,
    PS_WaterfallFoam,
    PS_DS_AtmosphericScattering_Rain,
    PS_LinDepth,
    PS_DiffuseNormalmapped,
    PS_DiffuseNormalmappedFxMap,
    PS_DiffuseAlphaTest,
    PS_DiffuseAlphaTestShadows,
    PS_DiffuseNormalmappedAlphaTest,
    PS_DiffuseNormalmappedAlphaTestFxMap,
    PS_DiffuseAtlas,
    PS_DiffuseAtlasAlphaTest,
    PS_Preview_White,
    PS_Preview_Textured,
    PS_Preview_TexturedLit,
    PS_PFX_Sharpen,
    PS_PFX_GammaCorrectInv,
    PS_PFX_DoF_FocusResolve,
    PS_PFX_DoF,
    PS_PFX_DoF_Gauss,
    PS_PFX_DoF_Composite,
    PS_PFX_TAA,
    PS_PFX_Velocity,
    PS_PFX_CAS,
    PS_PFX_FSR1_EASU,
    PS_PFX_FSR1_RCAS,
    COUNT
};

enum class GShaderID : size_t {
    GS_VertexNormals,
    GS_Cubemap,
    GS_ParticleStreamOut,
    COUNT
};

enum class HDShaderID : size_t {
    COUNT
};

enum class CShaderID : size_t {
    CS_AdvanceRain,
    CS_LightCulling,
    CS_TiledShading,
    CS_CullVobs,
    CS_BuildHiZ,
    COUNT
};
