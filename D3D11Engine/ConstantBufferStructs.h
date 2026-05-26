#pragma once
#include "pch.h"
#include <DirectXMath.h>

#define FFX_CPU
#include "Shaders/FidelityFX/ffx_core.h"

/** Actual instance data for a vob */
struct VobInstanceInfo {
    XMFLOAT4X4 world;
    XMFLOAT4X4 prevWorld;  // Previous frame's world matrix for motion vectors
    DWORD color;
    float windStrenth;
    float canBeAffectedByPlayer;
    // General purpose slot. Used by instanced VOB rendering to store an index
    // into optional per-visual metadata buffers.
    DWORD GP_Slot;
};

struct VobWindMetadata {
    float MinHeight;
    float MaxHeight;
    float2 Padding;
};

/** Per-instance data for instanced node attachment rendering */
struct NodeAttachmentInstanceData {
    XMFLOAT4X4 World;
    XMFLOAT4X4 PrevWorld;
    float4 Color;
};

/** Remap-index for the static vobs */
struct VobInstanceRemapInfo {
    bool operator < ( const VobInstanceRemapInfo& b ) const {
        return InstanceRemapIndex < b.InstanceRemapIndex;
    }

    bool operator == ( const VobInstanceRemapInfo& o ) const {
        return InstanceRemapIndex == o.InstanceRemapIndex;
    }

    DWORD InstanceRemapIndex;
};

#pragma pack (push, 1)	
struct SkyConstantBuffer {
    float SC_TextureWeight;
    float3 SC_pad1;
};

struct GammaCorrectConstantBuffer {
    float G_Gamma;
    float G_Brightness;
    float2 G_pad;
};

struct PfxSharpenConstantBuffer {
    float2 G_TextureSize;
    float G_SharpenStrength;
    float G_pad;
};

struct BlurConstantBuffer {
    float2 B_PixelSize;
    float B_BlurSize;
    float B_Threshold;

    float4 B_ColorMod;
};

struct PerObjectState {
    float3 OS_AmbientColor;
    float OS_Pad;
};

struct PFXVS_ConstantBuffer {
    float4 PFXVS_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
};

struct HeightfogConstantBuffer {
    float4 HF_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    XMFLOAT4X4 InvView;
    float3 CameraPosition;
    float HF_FogHeight;

    float HF_HeightFalloff;
    float HF_GlobalDensity;
    float HF_WeightZNear;
    float HF_WeightZFar;

    float3 HF_FogColorMod;
    float HF_pad2;

    float2 HF_ProjAB;
    float2 HF_Pad3;
};

struct LumAdaptConstantBuffer {
    float LC_DeltaTime;
    float3 LC_Pad;
};

struct GodRayZoomConstantBuffer {
    float GR_Decay;
    float GR_Weight;
    float2 GR_Center;

    float GR_Density;
    float3 GR_ColorMod;
};

struct DepthOfFieldConstantBuffer {
    float DoF_FocusDistance;
    float DoF_FocusRange;
    float DoF_BokehRadius;
    float DoF_MaxBlur;

    float4 DoF_ProjParams;
    float DoF_NearPlane;
    float DoF_FarPlane;
    float DoF_Pad;
    float DoF_Pad2;
};

struct SAOConstantBuffer {
    float4 SAO_ProjParams;    // x = 1/P._11, y = 1/P._22, z = P._34, w = P._33
    float  SAO_Radius;        // World-space AO radius
    float  SAO_Bias;          // Normal bias to reduce self-occlusion
    float  SAO_Intensity;     // Darkening strength
    int    SAO_NumSamples;    // Spiral sample count (16-48)
    float2 SAO_InvResolution; // 1/width, 1/height
    float  SAO_BlurSharpness; // Bilateral blur edge preservation
    float  SAO_Pad;
};

struct SAOBlurConstantBuffer {
    float2 SAO_Blur_InvResolution;
    float2 SAO_Blur_Direction;
    float  SAO_Blur_Sharpness;
    float3 SAO_Blur_Pad;
    float4 SAO_Blur_ProjParams;
};

struct HDRSettingsConstantBuffer {
    float HDR_MiddleGray;
    float HDR_LumWhite;
    float HDR_Threshold;
    float HDR_BloomStrength;
};

struct ViewportInfoConstantBuffer {
    float2 VPI_ViewportSize;
    float2 VPI_pad;
};

struct DS_PointLightConstantBuffer {
    float4 PL_Color;

    float PL_Range;
    float3 Pl_PositionWorld;

    float PL_Outdoor;
    float3 Pl_PositionView;

    float2 PL_ViewportSize;
    float2 PL_Pad2;

    float4 PL_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    XMFLOAT4X4 PL_InvView;

    float3 PL_LightScreenPos;
    float PL_Pad3;
};

constexpr int MAX_CSM_CASCADES = 4;

struct DS_ScreenQuadConstantBuffer {
    float4 SQ_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    XMFLOAT4X4 SQ_InvView;
    XMFLOAT4X4 SQ_View;

    XMFLOAT4X4 SQ_RainViewProj;

    float3 SQ_LightDirectionVS;
    float SQ_ShadowmapSize;

    float4 SQ_LightColor;
    
    // CSM: Cascade 0 (f�r Kompatibilit�t mit bestehenden Shadern)
    XMFLOAT4X4 SQ_ShadowViewProj[MAX_CSM_CASCADES];

    float SQ_ShadowStrength;
    float SQ_ShadowAOStrength;
    float SQ_WorldAOStrength;
    float SQ_ShadowSoftness;
    uint32_t SQ_FrameIndex;
    float SQ_LightSize;
    float2 SQ_Pad;

    // Shadow atlas: per-cascade UV rect (xy = offset, zw = scale)
    // Used when SHADOW_ATLAS is enabled (Feature Level 10 path)
    float4 SQ_CascadeAtlasRect[MAX_CSM_CASCADES];
};

struct CloudConstantBuffer {
    float3 C_LightDirection;
    float C_Pad;

    float3 C_CloudPosition;
    float C_Pad2;
};

struct AdvanceRainConstantBuffer {
    float3 AR_LightPosition;
    float AR_FPS;

    float3 AR_CameraPosition;
    float AR_Radius;

    float AR_Height;
    float3 AR_GlobalVelocity;

    int AR_MoveRainParticles;
    float3 AR_Pad1;
};

struct VS_ExConstantBuffer_PerFrame {
    XMFLOAT4X4 View;
    XMFLOAT4X4 Projection;
    XMFLOAT4X4 ViewProj;           // Jittered for rendering
    XMFLOAT4X4 PrevViewProj;       // Previous frame's unjittered view-projection for motion vectors
    XMFLOAT4X4 UnjitteredViewProj; // Current frame's unjittered view-projection for motion vectors
};

struct VS_ExConstantBuffer_Wind {
    float3 windDir;
    float globalTime;

    float minHeight;
    float maxHeight;
    float2 padding0;

    float3 playerPos;
    float padding1;
};

struct ParticlePointShadingConstantBuffer {
    XMFLOAT4X4 View;
    XMFLOAT4X4 Projection;
};

struct VS_ExConstantBuffer_PerInstance {
    XMFLOAT4X4 World;
    float4 Color;
};

struct VS_ExConstantBuffer_PerInstanceNode {
    XMFLOAT4X4 World;
    XMFLOAT4X4 PrevWorld; // Added for motion vectors
    float4 Color;
    float Fatness;
    float Scaling;
    float2 Pad1;
};

struct VS_ExConstantBuffer_PerInstanceSkeletal {
    XMFLOAT4X4 World;
    XMFLOAT4X4 PrevWorld; // Added for motion vectors
    float4 PI_ModelColor;
    float PI_ModelFatness;
    float3 PI_Pad1;
};

struct VS_ExConstantBuffer_SkeletalBoneRange {
    unsigned int BoneOffset;
    unsigned int PrevBoneOffset;
    unsigned int BoneCount;
    unsigned int UseStructuredBones;
};

struct ScreenFadeConstantBuffer {
    float GA_Alpha;
    float3 GA_Pad;
};

struct GhostAlphaConstantBuffer {
    float2 GA_ViewportSize;
    float GA_Alpha;
    float GA_Pad;
};

struct GrassConstantBuffer {
    float3 G_NormalVS;
    float G_Time;
    float G_WindStrength;
    float3 G_Pad1;
};

struct DefaultHullShaderConstantBuffer {
    float H_EdgesPerScreenHeight;
    float H_Proj11;
    float H_GlobalTessFactor;
    float H_FarPlane;
    float2 H_ScreenResolution;
    float2 h_pad2;
};

struct CubemapGSConstantBuffer {
    XMFLOAT4X4 PCR_View[6]; // View matrices for cube map rendering
    XMFLOAT4X4 PCR_ViewProj[6];
};

struct ParticleGSInfoConstantBuffer {
    float3 CameraPosition;
    float PGS_RainFxWeight;
    float PGS_RainHeight;
    float PGS_Pad;
    float2 PGS_RainScale;
};

struct PNAENConstantBuffer {
    XMFLOAT4X4    f4x4Projection;           // Projection matrix 
    float4      f4Eye;                    // Eye 
    float4      f4TessFactors;            // Tessellation factors 
                                            // x=Edge  
    float4      f4ViewportScale;          // The X and Y half  
                                            // resolution, 0, 0 
    INT4       adaptive;                 // Should use adaptive  
                                            // tessellation 
    INT4       clipping;                 // Should run clipping  
                                            // tests. 
};

struct RefractionInfoConstantBuffer {
    XMFLOAT4X4 RI_Projection;
    float2 RI_ViewportSize;
    float RI_Time;
    float RI_Far;

    float3 RI_CameraPosition;
    float RI_Pad2;
};

struct AtmosphereConstantBuffer {
    float AC_Kr4PI;
    float AC_Km4PI;
    float AC_g;
    float AC_KrESun;

    float AC_KmESun;
    float AC_InnerRadius;
    float AC_OuterRadius;
    float AC_Scale;

    float3 AC_Wavelength;
    float AC_RayleighScaleDepth;


    float AC_RayleighOverScaleDepth;
    int AC_nSamples;
    float AC_fSamples;
    float AC_CameraHeight;

    float3 AC_CameraPos;
    float AC_Time;
    float3 AC_LightPos;
    float AC_SceneWettness;

    float3 AC_SpherePosition;
    float AC_RainFXWeight;
};

struct CASConstantBuffer {
    FfxUInt32x4 const0;  // CasSetup output
    FfxUInt32x4 const1;  // CasSetup output
};

struct FSR1EASUConstantBuffer {
    uint32_t Const0[4];  // FsrEasuCon output
    uint32_t Const1[4];  // FsrEasuCon output
    uint32_t Const2[4];  // FsrEasuCon output
    uint32_t Const3[4];  // FsrEasuCon output
};

struct FSR1RCASConstantBuffer {
    uint32_t RCASConst[4];  // FsrRcasCon output (only first element used)
};

struct VelocityDebugConstantBuffer {
    float Amplification;    // Multiplier for velocity values (e.g., 10-100)
    float ShowMagnitude;    // 0 = show direction as RG, 1 = show magnitude as grayscale
    float AbsoluteMode;     // 0 = signed (-1 to 1), 1 = absolute values
    float Padding;
};

#define TILE_SIZE 16
// 64 Tiles seemed enough to fix issues in G1 old camp
#define MAX_LIGHTS_PER_TILE 64

struct LightCullingConstantBuffer {
    XMFLOAT4X4 Proj;
    uint32_t ScreenWidth;
    uint32_t ScreenHeight;
    uint32_t TotalLights;
    uint32_t MaxBufferIndices;
};

struct TiledShadingConstantBuffer {
    float2 ViewportSize;
    float2 Pad0;
    float4 ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    uint32_t LimitLightIntensity;
    uint32_t NumTilesX;
    float2 Pad1;
    XMFLOAT4X4 InvView; // For world-space reconstruction (shadow sampling)
};

struct ForwardPlusTileConstantBuffer {
    float2 ViewportSize;
    uint32_t NumTilesX;
    uint32_t LimitLightIntensity;
};

struct PsSimpleFFdata {
    float4 textureFactor;
};

#pragma pack (pop)

