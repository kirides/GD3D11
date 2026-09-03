#pragma once
#pragma warning( push )
#pragma warning( disable : 26495 )

#include "pch.h"
#include "BasePipelineStates.h"
#include "GraphicsDeviceCapabilities.h"
#include <ASSAO/ASSAO.h>

/** Struct handling all the graphical states set by the game. Can be used as Constantbuffer */
const int GSWITCH_FOG = 1;
const int GSWITCH_ALPHAREF = 2;
const int GSWITCH_LIGHING = 4;
const int GSWITCH_REFLECTIONS = 8;
const int GSWITCH_LINEAR_DEPTH = 16;
// Forward+ with hardware MSAA active: alpha-tested pixel shaders sharpen their alpha test into a
// per-pixel coverage value instead of a hard binary clip, so the MSAA alpha-to-coverage blend mode
// can dither an anti-aliased cutout edge across subsamples.
const int GSWITCH_MSAA_ALPHATOCOVERAGE = 32;

enum RenderStage {
    STAGE_DRAW_UNKNOWN = 0,
    _STAGE_DRAW_DX11_START,
    STAGE_DRAW_WORLD,
    STAGE_DRAW_SKELETAL,
    STAGE_DRAW_SKY,
    _STAGE_DRAW_DX11_END,
    STAGE_DRAW_HUD,
    STAGE_DRAW_SHADOWS,
    STAGE_DRAW_PRESENT = 0xFFFF,
};

/** A single fixed function stage */
struct FixedFunctionStage {
    enum EColorOp {
        CO_DISABLE = 1,
        CO_SELECTARG1 = 2,
        CO_SELECTART2 = 3,

        CO_MODULATE = 4,
        CO_MODULATE2X = 5,
        CO_MODULATE4X = 6,

        CO_ADD = 7,
        CO_SUBTRACT = 10
    };

    enum ETextureArg {
        TA_DIFFUSE = 0,
        TA_CURRENT = 1,
        TA_TEXTURE = 2,
        TA_TFACTOR = 3
    };

    /** Sets the default values for this struct */
    void SetDefault() {}

    EColorOp ColorOp;
    ETextureArg ColorArg1;
    ETextureArg ColorArg2;

    EColorOp AlphaOp;
    ETextureArg AlphaArg1;
    ETextureArg AlphaArg2;

    int FFS_Pad1;
    int FFS_Pad2;
};

struct GothicGraphicsState {
    /** Sets the default values for this struct */
    void SetDefault() {
        FF_FogWeight = 0.0f;
        FF_FogColor = float3( 1.0f, 1.0f, 1.0f );
        FF_FogNear = 1.0f;
        FF_FogFar = 10000.0f;

        FF_AmbientLighting = float3( 1.0f, 1.0f, 1.0f );
        FF_TextureFactor = float4( 1.0f, 1.0f, 1.0f, 1.0f );

        FF_AlphaRef = 170.0f / 255.0f;

        FF_GSwitches = 0;

        FF_Stages[0].ColorOp = FixedFunctionStage::EColorOp::CO_MODULATE;
        FF_Stages[1].ColorOp = FixedFunctionStage::EColorOp::CO_DISABLE;
        FF_Stages[0].ColorArg1 = FixedFunctionStage::ETextureArg::TA_TEXTURE;
        FF_Stages[0].ColorArg2 = FixedFunctionStage::ETextureArg::TA_DIFFUSE;
    }

    /** Sets one of the GraphicsFlags */
    void SetGraphicsSwitch( int flag, bool enabled ) {
        if ( enabled )
            FF_GSwitches |= flag;
        else
            FF_GSwitches &= ~flag;
    }

    /** Fog section */
    float FF_FogWeight;
    float3 FF_FogColor;

    float FF_FogNear;
    float FF_FogFar;
    float FF_zNear;
    float FF_zFar;

    /** Lighting section */
    float3 FF_AmbientLighting;
    float FF_Time;

    /** Texture factor section */
    float4 FF_TextureFactor;

    /** Alpha ref section
        G2: zRnd_D3D uses 0xb0 = 170 as default alpha ref
            and combines this with calculated per-vob distance-calculated alpha values.
    */
    float FF_AlphaRef;

    /** Graphical Switches (Takes GSWITCH_*) */
    unsigned int FF_GSwitches;
    float2 ggs_Pad3;

    FixedFunctionStage FF_Stages[2];
};

__declspec(align(4)) struct GothicPipelineState {
    /** Sets this state dirty, which means that it will be updated before next rendering */
    void SetDirty() {
        StateDirty = true;
        HashThis( reinterpret_cast<char*>(this), StructSize );
    }

    /** Hashes the whole struct */
    void HashThis( char* data, int size ) {
        Hash = 0;

        // Start hashing at the data of the other structs, skip the data of this one
        for ( int i = sizeof( GothicPipelineState ); i < size; i += 4 ) {
            DWORD d;
            memcpy( &d, data + i, 4 );

            Toolbox::hash_combine( Hash, d );
        }
    }

    bool operator==( const GothicPipelineState& o ) const {
        return Hash == o.Hash;
    }

    bool StateDirty;
    size_t Hash;
    int StructSize;
};

struct GothicPipelineKeyHasher {
    std::size_t operator()( const GothicPipelineState& k ) const {
        return k.Hash;
    }
};

namespace GothicStateCache {
    /** Hashmap for caching the state-objects */
    __declspec(selectany) std::unordered_map<GothicDepthBufferStateInfo, BaseDepthBufferState*, GothicPipelineKeyHasher> s_DepthBufferMap;
    __declspec(selectany) std::unordered_map<GothicBlendStateInfo, BaseBlendStateInfo*, GothicPipelineKeyHasher> s_BlendStateMap;
    __declspec(selectany) std::unordered_map<GothicRasterizerStateInfo, BaseRasterizerStateInfo*, GothicPipelineKeyHasher> s_RasterizerStateMap;
};

/** Depth buffer state information */
class BaseDepthBufferState;

struct GothicDepthBufferStateInfo : public GothicPipelineState {
    GothicDepthBufferStateInfo() {
        StructSize = sizeof( GothicDepthBufferStateInfo );
        Padding0 = Padding1 = false;
    }

    /** Layed out for D3D11 */
    enum ECompareFunc {
        CF_COMPARISON_NEVER = 1,
        CF_COMPARISON_LESS = 2,
        CF_COMPARISON_EQUAL = 3,
        CF_COMPARISON_LESS_EQUAL = 4,
        CF_COMPARISON_GREATER = 5,
        CF_COMPARISON_NOT_EQUAL = 6,
        CF_COMPARISON_GREATER_EQUAL = 7,
        CF_COMPARISON_ALWAYS = 8
    };

    static const ECompareFunc DEFAULT_DEPTH_COMP_STATE = CF_COMPARISON_GREATER_EQUAL;

    /** Sets the default values for this struct */
    void SetDefault() {
        DepthBufferEnabled = true;
        DepthWriteEnabled = true;
        DepthBufferCompareFunc = DEFAULT_DEPTH_COMP_STATE;
        Padding0 = false;
        Padding1 = false;
    }

    /** Depthbuffer settings */
    bool DepthBufferEnabled;
    bool DepthWriteEnabled;
    bool Padding0;
    bool Padding1;
    ECompareFunc DepthBufferCompareFunc;

    /** Deletes all cached states */
    static void DeleteCachedObjects() {
        for ( const auto& [k, depthBufferState] : GothicStateCache::s_DepthBufferMap ) {
            delete depthBufferState;
        }
        GothicStateCache::s_DepthBufferMap.clear();
    }

    GothicDepthBufferStateInfo Clone() {
        GothicDepthBufferStateInfo c;
        c.DepthBufferEnabled = DepthBufferEnabled;
        c.DepthWriteEnabled = DepthWriteEnabled;
        c.DepthBufferCompareFunc = DepthBufferCompareFunc;

        c.StateDirty = StateDirty;
        c.Hash = Hash;
        c.StructSize = StructSize;
        return c;
    }

    void ApplyTo( GothicDepthBufferStateInfo& c ) {
        c.DepthBufferEnabled = DepthBufferEnabled;
        c.DepthWriteEnabled = DepthWriteEnabled;
        c.DepthBufferCompareFunc = DepthBufferCompareFunc;

        c.StructSize = StructSize;
        c.SetDirty();
    }
};

/** Blend state information */
class BaseBlendStateInfo;

struct GothicBlendStateInfo : public GothicPipelineState {
    GothicBlendStateInfo() {
        StructSize = sizeof( GothicBlendStateInfo );
        Padding = false;
    }

    /** Layed out for D3D11 */
    enum EBlendFunc {
        BF_ZERO = 1,
        BF_ONE = 2,
        BF_SRC_COLOR = 3,
        BF_INV_SRC_COLOR = 4,
        BF_SRC_ALPHA = 5,
        BF_INV_SRC_ALPHA = 6,
        BF_DEST_ALPHA = 7,
        BF_INV_DEST_ALPHA = 8,
        BF_DEST_COLOR = 9,
        BF_INV_DEST_COLOR = 10,
        BF_SRC_ALPHA_SAT = 11,
        BF_BLEND_FACTOR = 14,
        BF_INV_BLEND_FACTOR = 15,
        BF_SRC1_COLOR = 16,
        BF_INV_SRC1_COLOR = 17,
        BF_SRC1_ALPHA = 18,
        BF_INV_SRC1_ALPHA = 19
    };

    /** Layed out for D3D11 */
    enum EBlendOp {
        BO_BLEND_OP_ADD = 1,
        BO_BLEND_OP_SUBTRACT = 2,
        BO_BLEND_OP_REV_SUBTRACT = 3,
        BO_BLEND_OP_MIN = 4,
        BO_BLEND_OP_MAX = 5
    };

    /** Sets the default values for this struct */
    void SetDefault() {
        SrcBlend = BF_SRC_ALPHA;
        DestBlend = BF_INV_SRC_ALPHA;
        BlendOp = BO_BLEND_OP_ADD;
        SrcBlendAlpha = BF_ONE;
        DestBlendAlpha = BF_ZERO;
        BlendOpAlpha = BO_BLEND_OP_ADD;
        BlendEnabled = false;
        AlphaToCoverage = false;
        ColorWritesEnabled = true;
    }

    /** Sets up alphablending */
    void SetAlphaBlending() {
        SrcBlend = BF_SRC_ALPHA;
        DestBlend = BF_INV_SRC_ALPHA;
        BlendOp = BO_BLEND_OP_ADD;
        SrcBlendAlpha = BF_ONE;
        DestBlendAlpha = BF_ZERO;
        BlendOpAlpha = BO_BLEND_OP_ADD;
        BlendEnabled = true;
        AlphaToCoverage = false;
        ColorWritesEnabled = true;
    }

    /** Sets up additive blending */
    void SetAdditiveBlending() {
        SrcBlend = BF_SRC_ALPHA;
        DestBlend = BF_ONE;
        BlendOp = BO_BLEND_OP_ADD;
        SrcBlendAlpha = BF_ONE;
        DestBlendAlpha = BF_ZERO;
        BlendOpAlpha = BO_BLEND_OP_ADD;
        BlendEnabled = true;
        AlphaToCoverage = false;
        ColorWritesEnabled = true;
    }

    /** Sets up modualte blending */
    void SetModulateBlending() {
        SrcBlend = BF_DEST_COLOR;
        DestBlend = BF_ZERO;
        BlendOp = BO_BLEND_OP_ADD;
        SrcBlendAlpha = BF_ONE;
        DestBlendAlpha = BF_ZERO;
        BlendOpAlpha = BO_BLEND_OP_ADD;
        BlendEnabled = true;
        AlphaToCoverage = false;
        ColorWritesEnabled = true;
    }

    /** Sets up modualte blending */
    void SetModulate2Blending() {
        SrcBlend = BF_DEST_COLOR;
        DestBlend = BF_SRC_COLOR;
        BlendOp = BO_BLEND_OP_ADD;
        SrcBlendAlpha = BF_ONE;
        DestBlendAlpha = BF_ZERO;
        BlendOpAlpha = BO_BLEND_OP_ADD;
        BlendEnabled = true;
        AlphaToCoverage = false;
        ColorWritesEnabled = true;
    }

    EBlendFunc SrcBlend;
    EBlendFunc DestBlend;
    EBlendOp BlendOp;
    EBlendFunc SrcBlendAlpha;
    EBlendFunc DestBlendAlpha;
    EBlendOp BlendOpAlpha;
    bool BlendEnabled;
    bool AlphaToCoverage;
    bool ColorWritesEnabled;
    bool Padding;

    /** Deletes all cached states */
    static void DeleteCachedObjects() {
        for ( const auto& [k, blendState] : GothicStateCache::s_BlendStateMap ) {
            delete blendState;
        }
        GothicStateCache::s_BlendStateMap.clear();
    }

    GothicBlendStateInfo Clone() {
        GothicBlendStateInfo c;
        c.SrcBlend = SrcBlend;
        c.DestBlend = DestBlend;
        c.BlendOp = BlendOp;
        c.SrcBlendAlpha = SrcBlendAlpha;
        c.DestBlendAlpha = DestBlendAlpha;
        c.BlendOpAlpha = BlendOpAlpha;
        c.BlendEnabled = BlendEnabled;
        c.AlphaToCoverage = AlphaToCoverage;
        c.ColorWritesEnabled = ColorWritesEnabled;

        c.StateDirty = StateDirty;
        c.Hash = Hash;
        c.StructSize = StructSize;
        return c;
    }

    void ApplyTo( GothicBlendStateInfo& c ) {
        c.SrcBlend = SrcBlend;
        c.DestBlend = DestBlend;
        c.BlendOp = BlendOp;
        c.SrcBlendAlpha = SrcBlendAlpha;
        c.DestBlendAlpha = DestBlendAlpha;
        c.BlendOpAlpha = BlendOpAlpha;
        c.BlendEnabled = BlendEnabled;
        c.AlphaToCoverage = AlphaToCoverage;
        c.ColorWritesEnabled = ColorWritesEnabled;

        c.StructSize = StructSize;
        c.SetDirty();
    }
};

/** Blend state information */
class BaseRasterizerStateInfo;

struct GothicRasterizerStateInfo : public GothicPipelineState {
    GothicRasterizerStateInfo() {
        StructSize = sizeof( GothicRasterizerStateInfo );
        Padding = false;
    }

    /** Layed out for D3D11 */
    enum ECullMode {
        CM_CULL_NONE = 1,
        CM_CULL_FRONT = 2,
        CM_CULL_BACK = 3
    };

    /** Sets the default values for this struct */
    void SetDefault() {
        CullMode = CM_CULL_BACK;
        ZBias = 0;
        SlopeScaledDepthBias = 0.0f;
        FrontCounterClockwise = false;
        Wireframe = false;
        DepthClipEnable = false;
    }

    ECullMode CullMode;
    bool FrontCounterClockwise;
    bool DepthClipEnable;
    bool Wireframe;
    bool Padding;
    int ZBias;
    float SlopeScaledDepthBias;

    /** Deletes all cached states */
    static void DeleteCachedObjects() {
        for ( const auto& [k, rasterizerState] : GothicStateCache::s_RasterizerStateMap ) {
            delete rasterizerState;
        }
        GothicStateCache::s_RasterizerStateMap.clear();
    }
};

/** Sampler state information */
struct GothicSamplerStateInfo : public GothicPipelineState {
    GothicSamplerStateInfo() {
        StructSize = sizeof( GothicSamplerStateInfo );
    }

    /** Layed out for D3D11 */
    enum ETextureAddress {
        TA_WRAP = 1,
        TA_MIRROR = 2,
        TA_CLAMP = 3,
        TA_BORDER = 4,
        TA_MIRROR_ONCE = 5
    };

    /** Sets the default values for this struct */
    void SetDefault() {
        AddressU = TA_WRAP;
        AddressV = TA_WRAP;
    }

    ETextureAddress AddressU;
    ETextureAddress AddressV;
};

/** Transforms set by gothic. All of these must be transposed before sent to a shader! */
struct GothicTransformInfo {
    /** Sets the default values for this struct */
    void SetDefault() {
        XMMATRIX const& idMatrix = XMMatrixIdentity();
        XMStoreFloat4x4( &TransformWorld, idMatrix );
        XMStoreFloat4x4( &TransformView, idMatrix );
        XMStoreFloat4x4( &TransformProj, idMatrix );
    }

    /** This is actually world * view. Gothic never sets the view matrix */
    XMFLOAT4X4 TransformWorld;

    /** Though never really set by Gothic, it's listed here for completeness sake */
    XMFLOAT4X4 TransformView;

    /** Projectionmatrix */
    XMFLOAT4X4 TransformProj;
    XMFLOAT4X4 TransformProjUnjittered;
};

struct HBAOSettings {
    HBAOSettings() {
        MetersToViewSpaceUnits = 100.0f;
        Radius = 1.00f;
        Bias = 0.5f;
        PowerExponent = 3.0f;
        BlurSharpness = 4.0f;
        BlendMode = 1;
        Enabled = true;
        EnableDualLayerAO = false;
        EnableBlur = true;
        SsaoBlurRadius = 1; // GFSDK_SSAO_BlurRadius::GFSDK_SSAO_BLUR_RADIUS_4;
        SsaoStepCount = 0; // GFSDK_SSAO_StepCount::GFSDK_SSAO_STEP_COUNT_4;
    }

    float Bias;
    float PowerExponent;
    float BlurSharpness;
    float Radius;
    float MetersToViewSpaceUnits;
    int BlendMode;
    bool Enabled;
    bool EnableDualLayerAO;
    bool EnableBlur;
    int SsaoBlurRadius;
    int SsaoStepCount;
};

enum class AOMode : int {
    AO_NONE = 0,
    AO_HBAO = 1,
    AO_SAO = 2,
    AO_ASSAO = 3,
};

// D3D12 only: resolution the AO compute chain (simple SSAO or XeGTAO) runs at, relative to the render
// resolution. D3D11's AO modes are untouched. The lit shaders need no changes to consume a lower-res
// mask - include/ScreenSpaceAO.hlsl already samples it through a bilinear CLAMP upsample.
enum class AoResolutionScale : int {
    Full = 0,
    Half = 1,
};

struct SAOSettings {
    SAOSettings() {
        Radius = 1.5f;
        Bias = 0.02f;
        Intensity = 3.0f;
        NumSamples = 16;
        BlurSharpness = 1.0f;
    }

    float Radius;
    float Bias;
    float Intensity;
    int NumSamples;
    float BlurSharpness;
};

// Intel XeGTAO. D3D12 ONLY — it is what AOMode::AO_ASSAO selects on that backend (D3D11 keeps its own ASSAO
// port, which reads AssaoSettings). Mirrors XeGTAO::GTAOSettings field-for-field except that Radius is in
// GOTHIC WORLD UNITS rather than metres: Intel's default of 0.5 m becomes 50 here, since 1 m == 100 units
// (cf. HBAOSettings::MetersToViewSpaceUnits). The remaining values are Intel's auto-tuned defaults, exposed
// because XE_GTAO_USE_DEFAULT_CONSTANTS is 0 in our shader so they can be tuned live.
struct GTAOSettings {
    GTAOSettings() {
        QualityLevel = 2;
        DenoisePasses = 1;
        Radius = 50.0f;
        RadiusMultiplier = 1.457f;
        FalloffRange = 0.615f;
        SampleDistributionPower = 2.0f;
        ThinOccluderCompensation = 0.0f;
        FinalValuePower = 2.2f;
        DepthMIPSamplingOffset = 3.30f;
    }

    int QualityLevel;                   // 0 low, 1 medium, 2 high, 3 ultra — picks the main-pass entry point
    int DenoisePasses;                  // 0 disabled, 1 sharp, 2 medium, 3 soft
    float Radius;                       // view-space radius of the occlusion sphere, in Gothic units
    float RadiusMultiplier;             // [0.3, 3.0] counters screen-space bias; Intel's auto-tune result
    float FalloffRange;                 // [0.0, 1.0] fades sample impact towards the radius edge
    float SampleDistributionPower;      // [1.0, 3.0] >1 concentrates samples near the centre (crevices)
    float ThinOccluderCompensation;     // [0.0, 0.7] discards samples behind the centre sooner
    float FinalValuePower;              // [0.5, 5.0] occlusion = pow( occlusion, this )
    float DepthMIPSamplingOffset;       // [2.0, 6.0] bandwidth/quality trade-off in the MIP selection
};

/** D3D12 only: the discrete roughness values the default (no _FX/_ORM map) material can be set to.
    The D3D12 backend can't sample an arbitrary roughness for these materials — it hands the shader a
    bindless 1x1 ORM texture, so every selectable value needs its own texture created up front (see
    D3D12GraphicsEngine::CreateWhiteTexture / m_DefaultOrmTextures). 14 steps of 0.05 from 0.25 to 0.90
    is 14 1x1 textures, i.e. nothing, and covers everything from damp stone to chalky plaster. kMin is
    floored at 0.25 deliberately: below that the GGX specular lobe (PBRLighting.hlsl's
    PBR_DistributionGGX) gets so tight that a directional-light highlight almost never lines up with
    a given pixel, and with no local reflection probes the lost specular energy has nowhere to go —
    default (no-map) materials just read as uniformly darker rather than glossier. Real materials with
    an authored _ORM map are unaffected; they aren't limited to this step table.
    Kept here rather than in the D3D12 headers so ImGuiShim can drive the slider without including them. */
namespace DefaultRoughness {
    constexpr float kMin  = 0.25f;
    constexpr float kStep = 0.05f;
    constexpr int   kNumSteps = 14;                         // 0.25 0.30 ... 0.90
    constexpr float kMax = kMin + kStep * ( kNumSteps - 1 );

    /** Roughness for step index i (clamped). */
    inline float ForStep( int i ) {
        if ( i < 0 ) i = 0;
        if ( i >= kNumSteps ) i = kNumSteps - 1;
        return kMin + kStep * static_cast<float>( i );
    }
    /** Nearest step index for an arbitrary roughness — the ini is free text, so this also sanitizes it. */
    inline int StepFor( float roughness ) {
        const int i = static_cast<int>( ( roughness - kMin ) / kStep + 0.5f );
        return i < 0 ? 0 : ( i >= kNumSteps ? kNumSteps - 1 : i );
    }
}

struct GothicRendererSettings {
    enum EPointLightShadowMode {
        PLS_DISABLED = 0,
        PLS_STATIC_ONLY = 1,
        PLS_UPDATE_DYNAMIC = 2,
        PLS_FULL = 3,
        _PLS_NUM_SETTINGS
    };
    enum E_HDRToneMap {
        ToneMap_jafEq4,
        Uncharted2Tonemap,
        ACESFilmTonemap,
        PerceptualQuantizerTonemap,
        ToneMap_Simple,
        ACESFittedTonemap,
    };
    enum EWindQuality {
        WIND_QUALITY_NONE = 0,
        WIND_QUALITY_ADVANCED,
    };

    enum E_ShadowFrustumCulling {
        SHD_FRUSTUM_CULLING_DISABLED = 0,
        SHD_FRUSTUM_CULLING_AGGRESSIVE = 1,
        SHD_FRUSTUM_CULLING_CONSERVATIVE = 2,
    };

    enum E_AntiAliasingMode {
        AA_NONE = 0,
        AA_SMAA = 1,
        AA_TAA = 2,
        AA_FSR = 3,
        _AA_NUM_MODES
    };

    enum E_SharpeningMode {
        SHARPEN_NONE = 0,
        SHARPEN_SIMPLE = 1,
        SHARPEN_CAS = 2,
        _SHARPEN_NUM_MODES
    };

    enum E_GraphicsPreset {
        GRAPHICS_CUSTOM,
        GRAPHICS_VERY_LOW,
        GRAPHICS_LOW,
        GRAPHICS_MEDIUM,
        GRAPHICS_HIGH,
        GRAPHICS_VERY_HIGH,
        GRAPHICS_ULTRA
    };

    enum E_Upscaler {
        UPSCALER_DEFAULT = 0,
        UPSCALER_FSR_1 = 1,
        // UPSCALER_FSR_2 = 2, // removed
        UPSCALER_FSR_3 = 3,
        _UPSCALER_NUM_MODES
    };

    enum E_ShadowFilterMode {
        SHADOW_FILTER_DISABLED = 0,
        SHADOW_FILTER_SIMPLE = 1,
        SHADOW_FILTER_PCSS = 2,
    };

    enum E_RendererMode {
        RM_Deferred = 0,
        RM_ForwardPlus = 1,
    };

    /** Selects which graphics backend the engine creates on startup. Read very early
        (before the full settings load) in Engine::CreateGraphicsEngine(). D3D12 is inert
        until the D3D12 backend lands; requesting it currently falls back to D3D11. */
    enum E_GraphicsAPI {
        GRAPHICS_API_D3D11 = 0,
        GRAPHICS_API_D3D12 = 1,
    };

    enum E_WaterSSRQuality {
        WATER_SSR_DISABLED = 0,
        WATER_SSR_LOW      = 1,
        WATER_SSR_MEDIUM   = 2,
        WATER_SSR_HIGH     = 3,
    };

    enum class TX_QUALITY : uint16_t {
        VeryLow = 128,
        Low = 256,
        Medium = 512,
        High = 1024,
        VeryHigh = 2048,
        MAX = 16384,
    };

    /** Sets the default values for this struct */
    void SetDefault() {
        SectionDrawRadius = 4;

        FpsLimit = 0;
        PausedFpsLimit = 30;
        DrawVOBs = true;
        DrawWorldMesh = 3;
        DrawSkeletalMeshes = true;
        // Off by default: ZENGIN's own deform stays the shipping path until the reimplementation has been
        // verified on real hardware. VerifyMorphBlend runs both and reports the difference.
        UseReimplementedMorphBlend = false;
        VerifyMorphBlend = false;
        // On where the backend has a working fold pipeline (D3D12 today): it is what removed the per-frame
        // CPU deform AND the per-instance CPU-writable vertex buffers those needed. Read exactly once, at
        // the first MorphGpu::IsActive() - see the comment on the member.
        UseGpuMorphFold = true;
        DrawMobs = true;
        DrawDynamicVOBs = true;

        DrawParticleEffects = true;

        DrawSky = true;
        DrawFog = true;
        FogRange = SwitchG1G2(1.0f, 3.0f);
        EnableHDR = false;
        HDRToneMap = E_HDRToneMap::ToneMap_Simple;
        ReplaceSunDirection = false;
        AtmosphericScattering = true; // Use original sky
        ShowSkeletalVertexNormals = false;
        EnableDynamicLighting = true;

        SortedTransparency = true;      // one back-to-front pass instead of one fixed pass per category

        DrawG1ForestPortals = false;    //enables the textures around forests and some doors to darken them
                                        //these are only applicable to G1, they don't appear to have been used in G2
        G1HighlightInteractiveFocus = true; // G1 only: toggles the interactive focus which brightens up focus vobs/mobs
        DrawRainThroughTransformFeedback = false; // Default to compute shaders

        FastShadows = false;
        MaxNumFaces = 0;
        IndoorVobDrawRadius = 5000.0f;
        OutdoorVobDrawRadius = 30000.0f;
        SkeletalMeshDrawRadius = 6000.0f;
        VisualFXDrawRadius = 10000.0f;
        // Past readable-silhouette range, but well inside OutdoorVobDrawRadius so it covers most of the
        // visible VOB population.
        VobLodDrawRadius = 8000.0f;

#if BUILD_SPACER_NET
        VisualFXDrawRadius = 16000.0f;
#endif

        OutdoorSmallVobDrawRadius = 10000.0f;
        SmallVobSize = 1500.0f;


#ifdef  BUILD_SPACER_NET
        OutdoorSmallVobDrawRadius = 30000.0f;
        IndoorVobDrawRadius = 10000.0f;
        SectionDrawRadius = 8;
#endif //  BUILD_SPACER_NET


#ifdef BUILD_GOTHIC_1_08k
        SetupOldWorldSpecificValues();
#else
        SetupNewWorldSpecificValues();
#endif

        SunLightColor = float3::FromColor( 255, 255, 255 );
        SunLightStrength = 1.5f;

        HDRLumWhite = 11.2f;
        HDRMiddleGray = 0.8f;
        BloomThreshold = 0.9f;
        Exposure = 1.0f;

        AutoExposureMiddleGray = 0.18f;
        AutoExposureStrength = 0.65f;
        AutoExposureMin = 0.5f;
        AutoExposureMax = 2.0f;
        AutoExposureSpeed = 0.5f;

        WireframeVobs = false;
        WireframeWorld = false;
        DrawShadowGeometry = true;
        FixViewFrustum = false;
        DisableWatermark = true;
        DisableRendering = false;
        DisableDrawcalls = false;

#ifdef BUILD_SPACER
        EnableEditorPanel = true;
#else
        EnableEditorPanel = false;
#endif
        AntiAliasingMode = E_AntiAliasingMode::AA_SMAA;

        TesselationFactor = 20.0f;
        TesselationRange = 8.0f;

        textureMaxSize = 16384;
        ShadowMapSize = 2048;
        WorldShadowRangeScale = 1.0f;
        NumShadowCascades = 3; // looks OK and performance friendly
        ShadowCascadePCFLimit = 1;
        ShadowFrustumCullingMode = E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_CONSERVATIVE;

        ShadowStrength = 0.40f;
        ShadowAOStrength = 0.50f;
        WorldAOStrength = 0.50f;
        ShadowSoftness = 1.0f; // 1.0 = default softness, higher = softer shadows
        PCSSLightSize = 0.140f; // Shadow-UV light radius used by PCSS blocker search

        SkyIblIntensity = 0.0f; // D3D12 only: scales the sky image-based indirect light (0 = flat ambient only)
        SkyOcclusionStrength = 0.85f; // D3D12 only: how hard a roof cuts the sky ambient (0 = off, 1 = interiors get none)
        SkyIblNightFloor = 0.14f; // D3D12 only: minimum night sky radiance for the IBL (see D3D12SkyIbl.cpp)
        DefaultMaterialRoughness = 0.80f; // D3D12 only: roughness for materials with no _FX/_ORM map

        BloomStrength = 1.0f;
        EnableBloom = false;
        BloomKnee = 0.5f;
        BloomRadius = 1.0f;
        GlobalWindStrength = 1.0f;
        VegetationAlphaToCoverage = true;

        BrightnessValue = 1.0f;
        GammaValue = 1.0f;

        EnableOcclusionCulling = false;
        EnablePortalCulling = true;
        PortalCullingNearRadius = 1500.0f;
        EnablePortalShadowSkip = false;
        ShadowFilterMode = E_ShadowFilterMode::SHADOW_FILTER_SIMPLE;

        EnableShadows = true;
        ThreadedShadowCulling = false;
        GpuVobCulling = false;
        GpuVobOcclusionCulling = false;
        EnableVSync = true;
        DoZPrepass = false;
        SortRenderQueue = false;
        DrawThreaded = false;
        EnableMeshOptimization = true;
        EnableShadowIndexBuffers = true;
        MeshOptimizeCacheFlags = MOC_DISK | MOC_MEMORY;

        WindQuality = WIND_QUALITY_ADVANCED;
        HeroAffectsObjects = true;
        EnablePointlightShadows = PLS_UPDATE_DYNAMIC;
        MinLightShadowUpdateRange = 300.0f;
        PartialDynamicShadowUpdates = true;
        EnableTiledLighting = false;
        RendererMode = RM_Deferred;
        GraphicsAPI = GRAPHICS_API_D3D11;
        MSAASamples = 1;
        DrawSectionIntersections = true;

        EnableGodRays = true;

        FOVHoriz = 90.0f;
        FOVVert = 90.0f;

        SharpeningMode = E_SharpeningMode::SHARPEN_CAS;
        SharpenFactor = 0.2f;

        RainRadiusRange = 5000.0f;
        RainHeightRange = 1000.0f;
        RainNumParticles = 50000;
        RainMoveParticles = true;
        RainGlobalVelocity = XMFLOAT3( 250, -1000, 0 );
        RainUseInitialSet = false;
        RainSceneWettness = 0.0f;
        RainSunLightStrength = 0.50f;
        RainFogColor = XMFLOAT3( 0.28f, 0.28f, 0.28f );
        RainFogDensity = 0.00050f;

        EnableRain = true;
        EnableRainEffects = true;

        GodRayDecay = 0.97f;
        GodRayWeight = 0.85f;
        GodRayDensity = 0.70f;
        GodRayColorMod = float3( 1.0f, 0.8f, 0.6f );

        EnableDoF = false;
        DoFGaussBlur = false;
        DoFFocusDistance = 5000.0f;
        DoFFocusRange = 8000.0f;
        DoFBokehRadius = 8.0f;
        DoFMaxBlur = 12.0f;

        AoMode = AOMode::AO_HBAO;

        RECT desktopRect;
        GetClientRect( GetDesktopWindow(), &desktopRect );

        // Match the resolution with the current desktop resolution
        LoadedResolution = INT2( desktopRect.right, desktopRect.bottom );

        ResolutionScalePercent = 100;
        Upscaler = E_Upscaler::UPSCALER_FSR_1;

        GothicUIScale = 1.0f;
        //DisableEverything();

        LimitLightIntesity = true;
        AllowNormalmaps = 0;
        CompressedNormalsSupport = true;

        AllowNumpadKeys = false;
        EnableDebugLog = false;
        EnableCustomFontRendering = true;
        FastInventoryRendering = true;

        ForceFOV = false;

        ChangeWindowPreset = 2; // WINDOW_MODE_FULLSCREEN_BORDERLESS;
        StretchWindow = true;
        SmoothShadowCameraUpdate = true;
        SmoothShadowFrequency = 500.0f;
        DisplayFlip = true;
        LowLatency = false;
        HDR_Monitor = false;
        HDR_AutoMaxBrightness = true;
        HDR_MaxBrightness = 1000.0f;
        HDR_PaperWhite = 200.0f;
        EnableInactiveFpsLock = false;
        MTResoureceManager = false;
        CompressBackBuffer = false;
        AnimateStaticVobs = true;
        RunInSpacerNet = false;
        BinkVideoRunning = false;
        EnableWaterAnimation = false;
        WaterSSRQuality = WATER_SSR_MEDIUM;
        OpaqueSSRQuality = WATER_SSR_MEDIUM;   // D3D12 only — temporal SSR on wet/glossy opaque surfaces

        GraphicsPreset = E_GraphicsPreset::GRAPHICS_MEDIUM;
        ShadowQuality = E_GraphicsPreset::GRAPHICS_MEDIUM;
        AllowSelfShadowingPointlights = false;
        PointlightShadowCasterFlags = PLSC_DYNAMIC_LIGHTS;
        DisableStaticPointlights = false;
        SpecularHighlightsFlags = SH_SUN | SH_POINTLIGHTS;

        ApplyGraphicsPreset();
        ApplyAssaoPreset(1);

        ResetDebugSettings();
    }

    void ApplyDx12Defaults()
    {
        EnableHDR = true;
        HDRToneMap = ACESFittedTonemap;
        ThreadedShadowCulling = true;
        
        ShadowStrength = 0.20f;
        
        EnableBloom = true;
    }

    /** Resolves the capability-driven FeatureSet entries: FEATURE_AUTO takes the device's answer, a
        forced override wins. Called by every backend at the end of device init and by the settings
        window's reset button, so a reset lands back on what the hardware actually supports. */
    void ApplyDeviceCapabilities( const GraphicsDeviceCapabilities& caps );

    void ApplyGraphicsPreset();
    /** Applies just the shadow values of the ShadowQuality preset. */
    void ApplyShadowPreset();
    void ApplyFeatureLevel10Downgrades();

    void ApplyAssaoPreset( int preset ) {
        AssaoSettings = ASSAO_Settings();
        // personal taste.
        AssaoSettings.ShadowPower = 1.0f; // i feel defaults are too dark
        AssaoSettings.HorizonAngleThreshold = 0.2f; // way too harsh shadowing otherwise

        if ( preset <= 0 ) {
            // default
        } else if ( preset == 1 ) {
            // higher quality but still default look
            AssaoSettings.QualityLevel = 3;
            AssaoSettings.AdaptiveQualityLimit = 0.6f;
        } else if ( preset == 2 ) {
            // Fake HBAO+ look, dark punchy shadowing
            AssaoSettings.Radius = 1.0f;
            AssaoSettings.ShadowMultiplier = 1.3f;
            AssaoSettings.ShadowPower = 1.5f;
            AssaoSettings.ShadowClamp = 1.0f;
            AssaoSettings.HorizonAngleThreshold = 0.200f;
            AssaoSettings.QualityLevel = 3;
            AssaoSettings.AdaptiveQualityLimit = 0.6f;

            AssaoSettings.BlurPassCount = 4;
            AssaoSettings.Sharpness = 1.0f;
            AssaoSettings.DetailShadowStrength = 0.5f;
        } else if ( preset >= 3 ) {
            // Fake GTAO look, broader radius, more details
            AssaoSettings.Radius = 1.6f;
            AssaoSettings.ShadowPower = 1.3f;
            AssaoSettings.ShadowClamp = 0.95f;
            AssaoSettings.HorizonAngleThreshold = 0.150f;
            AssaoSettings.QualityLevel = 3;
            AssaoSettings.AdaptiveQualityLimit = 0.6f;
            AssaoSettings.DetailShadowStrength = 2.5f;
        }
    }
    
    void ResetDebugSettings() {
        DebugSettings = {};
        DebugSettings.Culling.CullBspSections = true;
        DebugSettings.Culling.CullVobs = true;
        DebugSettings.ShadowCascades.LazyCascadeUpdate = true;
        DebugSettings.ShadowCascades.ShadowDepthSlopeBias = 0.0f;
        DebugSettings.ShadowCascades.FirstLodCascade = -1; // auto
        // Deliberately conservative: at 2 texels only props that resolve to a smudge get dropped. Raise it
        // against a capture - this is the one lever left on VOB casters dominating the shadow pass.
        DebugSettings.ShadowCascades.CasterMinTexels = 2.0f;
        // Capability-driven: AUTO everywhere, then resolved against the device by ApplyDeviceCapabilities().
        DebugSettings.FeatureSet.EnableDriverExtensionsOverride = FEATURE_AUTO;
        DebugSettings.FeatureSet.UseMDIOverride = FEATURE_AUTO;
        DebugSettings.FeatureSet.UseLayeredRenderingOverride = FEATURE_AUTO;
        DebugSettings.FeatureSet.EnableDriverExtensions = true;
        DebugSettings.FeatureSet.UseWorldSectionBVH = true;
        DebugSettings.FeatureSet.UseScreenSpaceShadowMask = false;
        DebugSettings.FeatureSet.GenerateAONormalsFromDepth = true;
    }

    void SetupOldWorldSpecificValues() {
        FogGlobalDensity = 0.00002f;
        FogHeightFalloff = 0.00018f;
        FogColorMod = float3::FromColor( 189, 146, 107 );
        FogHeight = 4000;
    }

    void SetupNewWorldSpecificValues() {
        FogGlobalDensity = 0.00004f;
        FogHeightFalloff = 0.0005f;
        FogColorMod = float3::FromColor( 180, 180, 255 );
        FogHeight = 800;
    }

    void SetupAddonWorldSpecificValues() {
        FogGlobalDensity = 0.00004f;
        FogHeightFalloff = 0.0005f;
        FogColorMod = float3::FromColor( 128, 173, 239 );
        FogHeight = 0;
    }

    void DisableEverything() {}

    bool IsShadowFrustumCullingEnabled() { return ShadowFrustumCullingMode != SHD_FRUSTUM_CULLING_DISABLED && NumShadowCascades > 1; }

    /** Rendering options */
    int FpsLimit;
    /** Cap applied on top of FpsLimit while an in-game menu holds the game paused. ZENGIN renders
        those frames from a nested loop it never throttles in-game, which lets an idle menu reach
        thousands of FPS and has been seen to crash drivers - so this cap is mandatory and only
        its value is configurable, clamped to [PausedFpsLimitMin, PausedFpsLimitMax]. */
    int PausedFpsLimit;
    static constexpr int PausedFpsLimitMin = 10;
    static constexpr int PausedFpsLimitMax = 100;
    bool DrawVOBs;
    bool DrawDynamicVOBs;
    int DrawWorldMesh;
    bool DrawSkeletalMeshes;
    /** Morph attachments (heads, bow/crossbow draw meshes) fold their blend shapes with our own
        reimplementation (MorphBlend) instead of calling ZENGIN's CalcVertPositions. Same result, but the
        state it captures is what a GPU deform would consume - this is the A/B switch for that port.
        AdvanceAnis still runs either way; it is game state, not geometry. */
    bool UseReimplementedMorphBlend;
    /** Every morph update also runs ZENGIN's deform and logs the worst per-component deviation from ours.
        Diagnostic only - it does BOTH deforms, so it is slower than either path. */
    bool VerifyMorphBlend;
    /** Fold morph attachments in a compute shader (MorphGpu / Shaders/D3D12/MorphFold.hlsl) instead of
        deforming them on the CPU and re-uploading the vertex stream every animation frame. Ignored by a
        backend that has no fold pipeline, which keeps the CPU deform.
        NOT a live toggle: MorphGpu::IsActive() reads it once and freezes the answer, because it also decides
        how morph vertex buffers are CREATED (a GPU-written DEFAULT+UAV buffer vs a CPU-writable DYNAMIC one).
        Changing it in the ImGui window therefore takes effect on the next restart. */
    bool UseGpuMorphFold;
    bool DrawMobs;
    bool DrawParticleEffects;
    bool DrawSky;
    bool DrawFog;
    float FogRange;
    int WindQuality;
    bool HeroAffectsObjects;
    bool SortedTransparency;
    bool DrawG1ForestPortals;
    bool G1HighlightInteractiveFocus;
    bool DrawRainThroughTransformFeedback;
    bool EnableHDR;
    E_HDRToneMap HDRToneMap;
    bool EnableVSync;
    bool FastShadows;
    bool ReplaceSunDirection;
    bool AtmosphericScattering;
    bool ShowSkeletalVertexNormals;
    bool EnableDynamicLighting;
    bool WireframeWorld;
    bool WireframeVobs;
    E_ShadowFilterMode ShadowFilterMode;
    bool EnableShadows;
    bool ThreadedShadowCulling;
    // GPU-driven static-VOB culling (D3D12 only; D3D11 ignores both). GpuVobCulling replaces the CPU per-VOB
    // frustum test with a distance-only collection plus a compute frustum cull that compacts the instance
    // stream and rewrites the ExecuteIndirect instance counts. GpuVobOcclusionCulling additionally rejects
    // instances hidden behind the world mesh, using a Hi-Z pyramid built from the world depth prepass — split
    // out as its own toggle because it is the part that can wrongly hide geometry.
    bool GpuVobCulling;
    bool GpuVobOcclusionCulling;
    int ShadowCascadePCFLimit;
    E_ShadowFrustumCulling ShadowFrustumCullingMode;
    bool DrawShadowGeometry;
    bool VegetationAlphaToCoverage;
    bool DisableWatermark;
    bool DisableRendering;
    bool DisableDrawcalls;
    bool EnableEditorPanel;
    // deferred render pass geometry Z-Prepass.
    // doesn't help much if at all.
    bool DoZPrepass;
    bool EnableAutoupdates;
    bool EnableOcclusionCulling;
    /** Skip VOBs of rooms the camera cannot see into through any chain of portals.
        Only applies to portal-compiled outdoor worlds; see BspPortalCuller. */
    bool EnablePortalCulling;
    /** Rooms within this distance of the camera are never portal-culled (Gothic units, 100 = 1m).
        Raise it if interiors pop while standing near a doorway. */
    float PortalCullingNearRadius;
    /** Skip the sun-shadow cascades while the view is fully enclosed by sectors, clearing the slices to
        "shadowed" instead. Requires EnablePortalCulling; see BspPortalCuller::IsOutdoorVisible.
        Off by default: a false "enclosed" verdict drops every sun shadow, which is very visible. */
    bool EnablePortalShadowSkip;
    bool SortRenderQueue;
    bool DrawThreaded;
    /** Run meshoptimizer's vertex-cache/fetch reorder (+ face reorder) on world sections and VOB
        sub-meshes at load time. This is most of what makes big worlds slow to load; off leaves geometry
        in its as-authored order (correct, just worse GPU vertex-cache/fetch locality at runtime). Also
        gates shadow-index welding and LOD simplification, both byproducts of the same pass - see
        EnableShadowIndexBuffers. A world/VOB set already optimized this session is memoized regardless
        (MeshOptimizeCache.h), so this only controls whether the FIRST load in a session pays for it. */
    bool EnableMeshOptimization;
    /** Build the separate welded shadow index buffer (MeshShadowIndexBuilder.h) when
        EnableMeshOptimization is on. Off falls back to the render index buffer for shadow passes
        (slightly more shadow-map vertex work, no alpha-test capability loss - see
        MeshShadowIndexBuilder.h) in exchange for skipping the weld and one buffer per sub-mesh. */
    bool EnableShadowIndexBuffers;
    // Which tiers of the mesh-optimize cache (MeshOptimizeCache.h) may serve/store a hit, as an OR of
    // flags. MOC_MEMORY is only ever armed for one world load regardless of this flag - see MeshOptimizeCache.h.
    enum EMeshOptimizeCacheFlags : int {
        MOC_OFF = 0,
        MOC_DISK = 1 << 0,
        MOC_MEMORY = 1 << 1,
    };
    int MeshOptimizeCacheFlags;
    EPointLightShadowMode EnablePointlightShadows;
    float MinLightShadowUpdateRange;
    bool PartialDynamicShadowUpdates;
    bool EnableTiledLighting;
    E_RendererMode RendererMode;
    /** Requested graphics backend (see E_GraphicsAPI). Inert until the D3D12 backend lands. */
    E_GraphicsAPI GraphicsAPI;
    /** Hardware MSAA sample count (1/2/4/8). Only applied by the Forward+ renderer; Deferred always stays single-sample. */
    int MSAASamples;
    bool DrawSectionIntersections;

    int MaxNumFaces;

    float SharpenFactor;

    int SectionDrawRadius;
    float IndoorVobDrawRadius;
    float OutdoorVobDrawRadius;
    float SkeletalMeshDrawRadius;
    float OutdoorSmallVobDrawRadius;
    float VisualFXDrawRadius;
    // Distance past which an instanced VOB draws its reduced index buffer (D3D12 main view; cascades pick
    // LOD by cascade index). 0 = off. Bucketed per INSTANCE in CSCull - Gothic reuses visuals map-wide.
    float VobLodDrawRadius;
    float SmallVobSize;
    float WorldShadowRangeScale;
    int NumShadowCascades;
    float GammaValue;
    float BrightnessValue;
    int ShadowMapSize;
    int textureMaxSize;

    float GlobalWindStrength;
    float FogGlobalDensity;
    float FogHeightFalloff;
    float FogHeight;
    float3 FogColorMod;
    float3 SunLightColor;
    float SunLightStrength;
    INT2 LoadedResolution;
    int ResolutionScalePercent;
    E_Upscaler Upscaler;

    float TesselationFactor;
    float TesselationRange;
    float HDRLumWhite;
    float HDRMiddleGray;
    float Exposure;

    // --- Dynamic exposure (D3D12 only; the D3D11 HDR path uses HDRMiddleGray/Exposure alone) ------------------
    // CS_LumReduce/CS_LumAdapt meter the finished HDR scene and Tonemap.hlsl divides by the result, so the
    // renderer's default behaviour is to drive EVERY scene to the same average brightness. That is wrong for
    // Gothic: an unlit cave is meant to read as dark, not to be normalized up to the same level as open daylight.
    // These five knobs are what bound that. See PSTonemap for the exact formula.
    //
    // Metering target in linear luminance - the average the auto-exposure aims the scene at. The classic
    // photographic 0.18; raise for a brighter overall image, lower for a darker one. NOT HDRMiddleGray, which is
    // calibrated for D3D11's own compressed tonemap curves and would overexpose by ~4.4x here.
    float AutoExposureMiddleGray;
    // How much of the full normalization to apply, 0..1, as an exponent on the exposure factor. 1 = classic
    // "every scene becomes middle gray" (bright caves, the reason this knob exists); 0 = auto-exposure off, the
    // manual Exposure multiplier alone. In between, a scene that meters darker than the target still ends up
    // darker than the target, just less so - which is what keeps interiors and nights readable but still dim.
    float AutoExposureStrength;
    // Hard limits on the resulting exposure multiplier, applied after Strength. These, not the luminance floor
    // in CS_LumReduce, are what cap how far a dark scene can be lifted (or a bright one pulled down).
    float AutoExposureMin;
    float AutoExposureMax;
    // Adaptation rate (Pattanaik tau) used by CS_LumAdapt's exponential blend toward the new measurement.
    // Higher = the eye adjusts faster; lower = a longer, more cinematic transition walking into a cave.
    float AutoExposureSpeed;

    float BloomThreshold;
    float BloomStrength;
    bool EnableBloom;
    float BloomKnee;
    float BloomRadius;
    float GothicUIScale;
    float FOVHoriz;
    float FOVVert;

    float ShadowStrength;
    float ShadowAOStrength;
    float WorldAOStrength;
    float ShadowSoftness;
    float PCSSLightSize;
    // D3D12 only (the D3D11 backend has no PBR path): scale on the sky-IBL indirect term that replaced the
    // flat ambient floor in Shaders/D3D12/include/PBRLighting.hlsl. 0 falls back to that flat term entirely.
    float SkyIblIntensity;
    // D3D12 only: how strongly Gothic's baked vertex light gates the sky-IBL indirect term — see
    // Shaders/D3D12/include/PBRLighting.hlsl ComputeSunLightingPBR. The IBL is the OPEN SKY's radiance, and
    // without this it is added with no visibility term at all, so caves and portal rooms read as sunlit at
    // noon and only go dark at night. ShadowAOStrength cannot do this job: it floors at 1-ShadowAOStrength
    // (0.5 by default), which still let a pitch-black cave catch half the daytime sky. 0 = off (the old
    // behaviour); 1 = interiors receive no sky ambient at all. The default leaves a small floor so cave
    // ceilings keep a trace of bounce light rather than crushing to black. Applies ONLY to the IBL branch —
    // the flat ambient fallback already carries vertLighting through shadowAO and matches D3D11 verbatim.
    float SkyOcclusionStrength;
    // D3D12 only: minimum LINEAR sky radiance (green channel; R/B follow a fixed blue-weighted ratio) used as a
    // night floor for the sky IBL. Gothic's night is not physically lit — zCSkyState's night fogColor is
    // (5,5,20), i.e. ~0.002 linear — so a faithful IBL leaves horizontal/downward normals with nothing. This is
    // the deliberate non-physical fill that Gothic itself applies; D3D11's atmospheric scattering hardcodes the
    // equivalent (AtmosphericScattering.h: nightColor = (0.2,0.2,0.4) * NIGHT_BRIGHTNESS). 0 disables the floor.
    float SkyIblNightFloor;
    // D3D12 only: the roughness handed to materials Gothic ships with no _FX/_ORM map, which is most of them.
    // Snapped to one of the DefaultRoughness:: steps on load and on every ImGui edit, because the backend can
    // only serve values it built a 1x1 ORM texture for at startup. AO stays 1 and metallic 0 for these.
    float DefaultMaterialRoughness;

    float GodRayDecay;
    float GodRayWeight;
    float GodRayDensity;
    float3 GodRayColorMod;
    bool EnableGodRays;

    bool EnableDoF;
    bool DoFGaussBlur;
    float DoFFocusDistance;
    float DoFFocusRange;
    float DoFBokehRadius;
    float DoFMaxBlur;

    HBAOSettings HbaoSettings;
    SAOSettings SaoSettings;
    ASSAO_Settings AssaoSettings;
    GTAOSettings GtaoSettings;   // D3D12's AO_ASSAO implementation — see the struct comment
    AOMode AoMode;
    AoResolutionScale AoResolution = AoResolutionScale::Full;   // D3D12 only — see the enum's comment

    bool FixViewFrustum;

    float RainRadiusRange;
    float RainHeightRange;
    UINT RainNumParticles;
    bool RainMoveParticles;
    bool RainUseInitialSet;
    XMFLOAT3 RainGlobalVelocity;
    float RainSceneWettness;

    float RainSunLightStrength;
    XMFLOAT3 RainFogColor;
    float RainFogDensity;

    bool EnableRain;
    bool EnableRainEffects;

    bool LimitLightIntesity;
    int AllowNormalmaps;

    bool AllowNumpadKeys;
    bool EnableDebugLog;

    bool EnableCustomFontRendering;
    /** Skips ZenGin's per-inventory-slot pseudo-world render (see zCWorld::hooked_Render). Off = fall back to
        the original oCItem::RenderItem path, for comparing against vanilla behavior. */
    bool FastInventoryRendering;
    bool ForceFOV;
    bool DisplayFlip;
    bool LowLatency;
    bool HDR_Monitor;
    /** Real HDR scanout (D3D12 only, requires HDR_Monitor). Monitor HDR metadata is frequently wrong, so the
        peak the tonemapper rolls off to is user-overridable: HDR_AutoMaxBrightness takes DXGI's reported
        DXGI_OUTPUT_DESC1::MaxLuminance, otherwise HDR_MaxBrightness (nits) is used verbatim. HDR_PaperWhite is
        the nit level that "SDR white" maps to — it sets the brightness of the UI/HUD and of diffuse-white
        surfaces, and is the reference the highlight headroom is measured against. */
    bool HDR_AutoMaxBrightness;
    float HDR_MaxBrightness;
    float HDR_PaperWhite;
    bool StretchWindow;
    int ChangeWindowPreset;
    bool SmoothShadowCameraUpdate;
    float SmoothShadowFrequency;
    bool EnableInactiveFpsLock;
    bool MTResoureceManager;
    bool CompressBackBuffer;
    bool AnimateStaticVobs;
    bool RunInSpacerNet;
    bool BinkVideoRunning;
    bool EnableWaterAnimation;
    E_WaterSSRQuality WaterSSRQuality;
    // D3D12 only: temporal SSR on opaque wet/glossy surfaces (D3D12Ssr.cpp's history + the Forward+ PS
    // marcher). Reuses E_WaterSSRQuality's step-count tiers/DISABLED value rather than a parallel enum —
    // same quality/cost tradeoff, different geometry class. See D3D12_SSR_WET_SURFACES_PLAN.md.
    E_WaterSSRQuality OpaqueSSRQuality;
    E_AntiAliasingMode AntiAliasingMode;
    E_SharpeningMode SharpeningMode;
    E_GraphicsPreset GraphicsPreset;
    // Which preset's shadow values are in effect. The graphics presets set it along the way; the
    // settings window uses it as the single shadow-quality knob (see ApplyShadowPreset).
    E_GraphicsPreset ShadowQuality;
    bool CompressedNormalsSupport;
    bool AllowSelfShadowingPointlights;
    // Which categories of point light are allowed to cast shadows from VOBs/NPCs (not just the world mesh),
    // as an OR of flags - on both backends (see D3D11PointLight::RenderStaticShadowPass/RenderFullCubemap/
    // GetCurrentShadowMode and D3D12PointShadows::SelectShadowedLights/Prepare). A light whose category bit
    // is unset here is restricted to world-mesh-only casters:
    //   - PLSC_STATIC_LIGHTS off (the default) keeps every IsStatic() light on the cheap world-only/
    //     PLS_STATIC_ONLY path Gothic's 10-30-per-room atmospheric fill lights would otherwise cost.
    //   - PLSC_PARTICLE_FX off (the default) protects PFX-driven lights (torches/campfires/spell effects):
    //     their vob-tree parent chain doesn't reliably resolve to a single origin item, so the usual
    //     self-shadow exclusion can't be trusted and a carried/attached PFX light could throw a large shadow
    //     off its own carrier. Enabling it accepts that risk for the extra shadow detail.
    //   - PLSC_DYNAMIC_LIGHTS covers every other point light (the common case) and is on by default.
    // A light matches at most one category - PFX takes precedence over Static when a light is both (see the
    // category-resolution comment at each use site).
    enum EPointlightShadowCasterFlags : int {
        PLSC_NONE = 0,
        PLSC_DYNAMIC_LIGHTS = 1 << 0,
        PLSC_STATIC_LIGHTS = 1 << 1,
        PLSC_PARTICLE_FX = 1 << 2,
    };
    int PointlightShadowCasterFlags;
    // Drop every zCVobLight with IsStatic() from the frame's point-light set. Gothic lights its rooms and caves
    // with 10-30 co-located "atmospheric" static fill lights that exist only to raise the ambient level; under
    // an HDR pipeline they stack into a badly over-bright interior. D3D12 backend only (see BuildFrameLightBuffer).
    bool DisableStaticPointlights;
    // Which lights are allowed to produce a specular highlight, as an OR of flags. Some players find specular
    // highlights visually distracting (torches/spell-light glints on every surface) and want them gone without
    // losing diffuse lighting entirely. Reuses the existing "isStatic -> Color.w=0" suppression D3D11/D3D12
    // already apply to atmospheric fill lights (see BuildFrameLightBuffer / D3D11TiledDeferredShading) - that
    // trick just becomes flag-driven instead of hardcoded, and the sun gets the same treatment via a new
    // SQ_SunSpecularEnabled / SunSpecularEnabled constant (see D3D11ShadowMap::Upload*Constants / D3D12ShadowMap::
    // UploadSamplingConstants). Both backends.
    enum ESpecularHighlights : int {
        SH_NONE = 0,
        SH_SUN = 1 << 0,
        SH_POINTLIGHTS = 1 << 1,
        SH_ATMOSPHERIC = 1 << 2,
    };
    int SpecularHighlightsFlags;

    /** Tri-state for the settings that follow the device's capabilities. FEATURE_AUTO takes whatever
        the backend reported; a forced value lets a player override a bad or unlucky driver report. */
    enum E_FeatureOverride : int {
        FEATURE_AUTO = -1,
        FEATURE_FORCE_OFF = 0,
        FEATURE_FORCE_ON = 1,
    };

    struct {
        struct {
            bool DepthMotionVectors;
            bool DisplayVelocity;
            // D3D12 only: overlays the octahedral normal G-buffer the depth prepass writes (the XeGTAO input).
            // D3D11 has no such buffer — it reconstructs normals from depth — so its renderer ignores this.
            bool DisplayNormals;
        } TAA;
        struct {
            bool LazyCascadeUpdate;
            float ExtendBack;
            float ExtendFront;
            float ExtendSide;
            float Lambda;
            float Bias;
            float ShadowDepthSlopeBias;
            // First shadow cascade allowed to draw the baked progressive-mesh LOD index buffer.
            // -1 = auto (D3D11: last cascade, but never below SHADOW_LOD_FIRST_CASCADE; D3D12:
            // SHADOW_LOD_FIRST_CASCADE). A value >= NumShadowCascades disables the LOD entirely.
            // Cascades below SHADOW_LOD_FIRST_CASCADE are biased tighter than the deviation an edge
            // collapse introduces, so they self-shadow the full-detail surface black - see WorldConverter.h.
            int FirstLodCascade;
            // Drop a VOB caster from a cascade when its bounding-box diagonal covers fewer than this many
            // texels OF THAT CASCADE. Scaled per cascade by D3D12ShadowMap::m_CascadeTexelWorld, so the near
            // cascade (fine texels) drops almost nothing and only the coarse far ones prune hard, where a
            // prop resolves to a pixel or two of shadow but still costs its full vertex + raster work.
            // 0 disables it entirely.
            float CasterMinTexels;
        } ShadowCascades;
        struct {
            bool CullVobs;
            bool CullBspSections;
        } Culling;
        struct {
            // The three entries below are resolved from the device capabilities and the matching
            // *Override tri-state by ApplyDeviceCapabilities(); renderer code reads these, only the
            // overrides are persisted.
            bool EnableDriverExtensions;
            bool UseMDI;
            bool UseLayeredRendering;

            E_FeatureOverride EnableDriverExtensionsOverride;
            E_FeatureOverride UseMDIOverride;
            E_FeatureOverride UseLayeredRenderingOverride;

            // EnableDriverExtensions can only be resolved once the device is up, but the init path has
            // to decide whether to even load NVAPI/AGS - so it asks the override instead.
            bool WantsDriverExtensions() const { return EnableDriverExtensionsOverride != FEATURE_FORCE_OFF; }

            bool UseWorldSectionBVH;
            bool UseShadowAtlas;
            bool UseScreenSpaceShadowMask;
            bool GenerateAONormalsFromDepth; // Forward+: build smooth normals from depth for SAO/ASSAO
            bool ForceFeatureLevel10;
        } FeatureSet;
        struct {
            // Draws a wireframe range-sphere at every active point light and opens an ImGui window with
            // stats + the raw shadow-cube faces (unwrapped as a cross) for whichever light is nearest the
            // camera. See ImGuiShim::RenderPointLightShadowDebugWindow.
            bool Enabled;
        } PointLightDebug;
    } DebugSettings;

    bool GetIsTAAEnabled() const {
        return AntiAliasingMode == E_AntiAliasingMode::AA_TAA
            || AntiAliasingMode == E_AntiAliasingMode::AA_FSR;
    }

    // Specular scale (0/1) for a point light's Color.w, per SpecularHighlightsFlags. Static lights are Gothic's
    // "atmospheric" fill lights; everything else is a regular dynamic/static point light.
    float PointLightSpecularScale( bool isStatic ) const {
        const int flag = isStatic ? SH_ATMOSPHERIC : SH_POINTLIGHTS;
        return ( SpecularHighlightsFlags & flag ) ? 1.0f : 0.0f;
    }
};

/** Event rate over a SLIDING one-second window, kept as ten 100 ms buckets that PerSecond() sums.
    Deliberately not a single tumbling window: that only publishes a count once the second it was counting
    in has ended, so a stat you watch while provoking the thing you are measuring reads 0 for the entire
    time the events are happening and then reports them after the fact - which makes it look broken and,
    worse, easy to miss. Summing the buckets (the one in progress included) surfaces an event within
    100 ms and lets it decay out again over the following second.

    Self-sampling: the buckets advance inside both Note() and PerSecond(), so nothing needs a per-frame
    tick and a metric no one has reported in a while falls back to 0 on its own.

    MAIN THREAD ONLY - the bucket rotation is not synchronized. Every current caller is on Gothic's own
    thread; anything reporting from the worker pool needs its own accumulation. */
class RollingSecondCounter {
public:
    void Note( unsigned int n = 1 ) {
        Advance();
        m_Buckets[m_Head] += n;
        m_Total += n;
    }

    /** Events in the last second, including the bucket still being filled. */
    unsigned int PerSecond() {
        Advance();
        unsigned int sum = 0;
        for ( unsigned int b : m_Buckets ) sum += b;
        return sum;
    }

    /** Every event since startup. Unwindowed, so it separates "nothing is happening" from "the window
        already let it go" - which is the question a rate alone cannot answer. */
    unsigned long long Total() const { return m_Total; }

private:
    static const size_t NUM_BUCKETS = 10;
    static const long long BUCKET_MS = 100;   // NUM_BUCKETS * BUCKET_MS = the window length

    void Advance() {
        const auto now = std::chrono::steady_clock::now();
        const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>( now - m_BucketStart ).count();
        if ( elapsed < BUCKET_MS ) return;

        const long long steps = elapsed / BUCKET_MS;
        if ( steps >= static_cast<long long>( NUM_BUCKETS ) ) {
            // Idle (or paused) for longer than the whole window - everything in it has expired.
            m_Buckets.fill( 0 );
            m_Head = 0;
            m_BucketStart = now;
            return;
        }
        // Keep the phase rather than restarting from `now`, so buckets stay on a fixed 100 ms grid.
        m_BucketStart += std::chrono::milliseconds( BUCKET_MS * steps );
        for ( long long i = 0; i < steps; ++i ) {
            m_Head = ( m_Head + 1 ) % NUM_BUCKETS;
            m_Buckets[m_Head] = 0;
        }
    }

    std::chrono::steady_clock::time_point m_BucketStart = std::chrono::steady_clock::now();
    std::array<unsigned int, NUM_BUCKETS> m_Buckets = {};
    size_t m_Head = 0;
    unsigned long long m_Total = 0;
};

struct GothicRendererInfo {
    GothicRendererInfo() {
        VOBVerticesDataSize = 0;
        SkeletalVerticesDataSize = 0;
        Reset();
    }

    void Reset() {
        FrameDrawnTriangles = 0;
        FrameDrawnVobs = 0;
        FPS = 0;
        FrameVobUpdates = 0;
        FrameNumSectionsDrawn = 0;

        FarPlane = 0;
        NearPlane = 0;
        FrameDrawnLights = 0;
        WorldMeshDrawCalls = 0;
        FramePipelineStates = 0;

        StateChanges = 0;
        memset( StateChangesByState, 0, sizeof( StateChangesByState ) );
        RenderStage = STAGE_DRAW_UNKNOWN;
    }

    enum EStateChange {
        SC_TX,
        SC_GS,
        SC_RTVDSV,
        SC_DS,
        SC_HS,
        SC_PS,
        SC_IL,
        SC_VS,
        SC_IB,
        SC_VB,
        SC_RS,
        SC_CB,
        SC_DSS,
        SC_SMPL,
        SC_BS,
        SC_NUM_STATES // Total number of states we have
    };

    unsigned int StateChanges;
    unsigned int StateChangesByState[SC_NUM_STATES];
    unsigned int FramePipelineStates;

    int FrameDrawnTriangles;
    int FrameDrawnVobs;
    int FrameVobUpdates;
    int FrameNumSectionsDrawn;
    int FPS;
    float FarPlane;
    float NearPlane;
    int FrameDrawnLights;
    int WorldMeshDrawCalls;

    // Cached static point-light shadows dropped per second - a caster appearing, vanishing or starting to
    // move inside a light's range, or the light itself moving. Deliberately NOT reset per frame (it owns its
    // own window). A steady non-zero rate means point-light cubes are being re-baked continuously instead of
    // cached, which is the expensive failure mode this cache exists to avoid; see ImGuiShim's Point Light
    // Shadow Debug window.
    RollingSecondCounter PointLightStaticInvalidations;

    // Point-light shadow-cube slot occupancy, per tier, as of the last frame's selection (D3D12 only; D3D11
    // leaves them 0). `Starved` counts lights that wanted a cube and could not be given one because their tier
    // was full of lights at comparable distance - the one number that separates "the tier is too small for this
    // scene" from "something is wrong with slot assignment", which look identical from inside the game.
    unsigned int PointLightSlotsUsed = 0;
    unsigned int PointLightSlotsMax = 0;
    unsigned int PointLightStaticSlotsUsed = 0;
    unsigned int PointLightStaticSlotsMax = 0;
    unsigned int PointLightSlotsStarved = 0;
    // Visible point lights that did not fit in the per-frame light buffer at all (kMaxFrameLights). These are
    // not shaded at all, not merely unshadowed - a different failure that looks the same from inside the game.
    unsigned int PointLightsDropped = 0;

    unsigned int VOBVerticesDataSize;
    // Skeletal meshes can be extracted (and their SkeletalMeshInfo destroyed) from background
    // worker threads (see GothicAPI::LoadzCModelData), so this counter needs to be atomic.
    std::atomic<unsigned int> SkeletalVerticesDataSize;
    RenderStage RenderStage;
    
    bool IsRenderStageDx11() const {
        return RenderStage > _STAGE_DRAW_DX11_START && RenderStage < _STAGE_DRAW_DX11_END;
    }
};

/** This handles more device specific settings */
struct GothicRendererState {
    GothicRendererState() {
        DepthState.SetDefault();
        BlendState.SetDefault();
        RasterizerState.SetDefault();
        GraphicsState.SetDefault();
        SamplerState.SetDefault();
        TransformState.SetDefault();
        RendererSettings.SetDefault();

        DepthState.SetDirty();
        BlendState.SetDirty();
        RasterizerState.SetDirty();
        SamplerState.SetDirty();
    }

    GothicDepthBufferStateInfo DepthState;

    GothicBlendStateInfo BlendState;

    GothicRasterizerStateInfo RasterizerState;

    GothicSamplerStateInfo SamplerState;

    GothicGraphicsState GraphicsState;
    GothicTransformInfo TransformState;
    GothicRendererSettings RendererSettings;
    GothicRendererInfo RendererInfo;
};
#pragma warning( pop )
