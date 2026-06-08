#pragma once
#pragma warning( push )
#pragma warning( disable : 26495 )

#include "pch.h"
#include "BasePipelineStates.h"
#include <ASSAO/ASSAO.h>

/** Struct handling all the graphical states set by the game. Can be used as Constantbuffer */
const int GSWITCH_FOG = 1;
const int GSWITCH_ALPHAREF = 2;
const int GSWITCH_LIGHING = 4;
const int GSWITCH_REFLECTIONS = 8;
const int GSWITCH_LINEAR_DEPTH = 16;

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
    static const size_t bucket_size = 10; // mean bucket size that the container should try not to exceed
    static const size_t min_buckets = (1 << 10); // minimum number of buckets, power of 2, >0

    static std::size_t hash_value( float value ) {
        std::hash<float> hasher;
        return hasher( value );
    }

    static void hash_combine( std::size_t& seed, float value ) {
        seed ^= hash_value( value ) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

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
        AA_FSR3 = 4, // Dummy value! only used for settings!
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
        UPSCALER_FSR_2 = 2,
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

    /** Sets the default values for this struct */
    void SetDefault() {
        SectionDrawRadius = 4;

        FpsLimit = 0;
        DrawVOBs = true;
        DrawWorldMesh = 3;
        DrawSkeletalMeshes = true;
        DrawMobs = true;
        DrawDynamicVOBs = true;

        DrawParticleEffects = true;

        DrawSky = true;
        DrawFog = true;
        FogRange = SWITCH_ENGINE12(1.0f, 3.0f);
        EnableHDR = false;
        HDRToneMap = E_HDRToneMap::ToneMap_Simple;
        ReplaceSunDirection = false;
        AtmosphericScattering = true; // Use original sky
        ShowSkeletalVertexNormals = false;
        EnableDynamicLighting = true;

        DrawG1ForestPortals = false;    //enables the textures around forests and some doors to darken them
                                        //these are only applicable to G1, they don't appear to have been used in G2
        DrawRainThroughTransformFeedback = false; // Default to compute shaders

        FastShadows = false;
        MaxNumFaces = 0;
        IndoorVobDrawRadius = 5000.0f;
        OutdoorVobDrawRadius = 30000.0f;
        SkeletalMeshDrawRadius = 6000.0f;
        VisualFXDrawRadius = 10000.0f;

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

        BloomStrength = 1.0f;
        GlobalWindStrength = 1.0f;
        VegetationAlphaToCoverage = true;

        BrightnessValue = 1.0f;
        GammaValue = 1.0f;

        EnableOcclusionCulling = false;
        ShadowFilterMode = E_ShadowFilterMode::SHADOW_FILTER_SIMPLE;

        EnableShadows = true;
        ThreadedShadowCulling = false;
        EnableVSync = true;
        DoZPrepass = false;
        SortRenderQueue = false;
        DrawThreaded = false;

        WindQuality = WIND_QUALITY_ADVANCED;
        HeroAffectsObjects = true;
        EnablePointlightShadows = PLS_UPDATE_DYNAMIC;
        MinLightShadowUpdateRange = 300.0f;
        PartialDynamicShadowUpdates = true;
        EnableTiledLighting = false;
        RendererMode = RM_Deferred;
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

        LimitLightIntesity = false;
        AllowNormalmaps = false;

        AllowNumpadKeys = false;
        EnableDebugLog = true;
        EnableCustomFontRendering = true;

        ForceFOV = false;

        ChangeWindowPreset = 2; // WINDOW_MODE_FULLSCREEN_BORDERLESS;
        StretchWindow = true;
        SmoothShadowCameraUpdate = true;
        SmoothShadowFrequency = 500.0f;
        DisplayFlip = false;
        LowLatency = false;
        HDR_Monitor = false;
        EnableInactiveFpsLock = true;
        MTResoureceManager = false;
        CompressBackBuffer = false;
        AnimateStaticVobs = true;
        RunInSpacerNet = false;
        BinkVideoRunning = false;
        EnableWaterAnimation = false;

        GraphicsPreset = E_GraphicsPreset::GRAPHICS_CUSTOM;
        ApplyAssaoPreset(1);

        ResetDebugSettings();
    }

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
        DebugSettings.FeatureSet.EnableDriverExtensions = true;
        DebugSettings.FeatureSet.UseWorldSectionBVH = true;
        DebugSettings.FeatureSet.UseScreenSpaceShadowMask = false;
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
    bool DrawVOBs;
    bool DrawDynamicVOBs;
    int DrawWorldMesh;
    bool DrawSkeletalMeshes;
    bool DrawMobs;
    bool DrawParticleEffects;
    bool DrawSky;
    bool DrawFog;
    float FogRange;
    int WindQuality;
    bool HeroAffectsObjects;
    bool DrawG1ForestPortals;
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
    bool SortRenderQueue;
    bool DrawThreaded;
    EPointLightShadowMode EnablePointlightShadows;
    float MinLightShadowUpdateRange;
    bool PartialDynamicShadowUpdates;
    bool EnableTiledLighting;
    E_RendererMode RendererMode;
    bool DrawSectionIntersections;

    int MaxNumFaces;

    float SharpenFactor;

    int SectionDrawRadius;
    float IndoorVobDrawRadius;
    float OutdoorVobDrawRadius;
    float SkeletalMeshDrawRadius;
    float OutdoorSmallVobDrawRadius;
    float VisualFXDrawRadius;
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
    float BloomThreshold;
    float BloomStrength;
    float GothicUIScale;
    float FOVHoriz;
    float FOVVert;

    float ShadowStrength;
    float ShadowAOStrength;
    float WorldAOStrength;
    float ShadowSoftness;
    float PCSSLightSize;

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
    AOMode AoMode;

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
    bool AllowNormalmaps;

    bool AllowNumpadKeys;
    bool EnableDebugLog;

    bool EnableCustomFontRendering;
    bool ForceFOV;
    bool DisplayFlip;
    bool LowLatency;
    bool HDR_Monitor;
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
    E_AntiAliasingMode AntiAliasingMode;
    E_SharpeningMode SharpeningMode;
    E_GraphicsPreset GraphicsPreset;
    
    struct {
        struct {
            bool DepthMotionVectors;
            bool DisplayVelocity;
        } TAA;
        struct {
            bool LazyCascadeUpdate;
            float ExtendBack;
            float ExtendFront;
            float ExtendSide;
            float Lambda;
            float Bias;
        } ShadowCascades;
        struct {
            bool CullVobs;
            bool CullBspSections;
        } Culling;
        struct {
            bool EnableDriverExtensions;
            bool UseWorldSectionBVH;
            bool UseMDI;
            bool UseLayeredRendering;
            bool UseShadowAtlas;
            bool UseScreenSpaceShadowMask;
            bool ForceFeatureLevel10;
        } FeatureSet;
    } DebugSettings;

    bool GetIsTAAEnabled() const {
        return AntiAliasingMode == E_AntiAliasingMode::AA_TAA
            || AntiAliasingMode == E_AntiAliasingMode::AA_FSR
            || AntiAliasingMode == E_AntiAliasingMode::AA_FSR3;
    }
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

    unsigned int VOBVerticesDataSize;
    unsigned int SkeletalVerticesDataSize;
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
