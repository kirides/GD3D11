#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12LineRenderer.h"
#include "D3D12VertexBuffer.h"
#include "D3D12Texture.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"
#include "../zCMorphMesh.h"
#include "../zCTexture.h"
#include "../D3D7/MyDirectDrawSurface7.h"
#include "../zCView.h"
#include "../zCModel.h"
#include "../zCMaterial.h"
#include "../zCVob.h"
#include "../zCVobLight.h"
#include "../zCDecal.h"
#include "../WorldConverter.h"
#include "../VertexTypes.h"
#include "../ImGuiShim.h"
#include "../zFont.h"
#include "../zCCamera.h"
#include "../oCGame.h"
#include "../DXGIHelpers.h"

#include <dxcapi.h>

#include "D3D12RenderQueue.h"
#include "InstancingUtils.h"

// imgui_impl_dx12 calls CreateDXGIFactory1 directly (for tearing detection). dxgi.dll is present on
// every Windows 7+ and the D3D11 fallback swapchain already needs it at runtime, so a load-time link
// here is safe — it does NOT reintroduce the D3D12 soft-dependency that lets old systems fall back.
#pragma comment(lib, "dxgi.lib")

// TODO: Replace dependency with runtime dynamic load of dxcompiler.dll (like D3D12CreateDevice) to avoid shipping it on systems that don't support D3D12.
#pragma comment(lib, "dxcompiler.lib")

using Microsoft::WRL::ComPtr;


// Why is BeginEvent not working as intended with Context on debugging this 32 bit app !!
// A global ring-buffer tracking recent recording phases mapped directly to command list slots
struct CPUBreadcrumbContext {
    UINT opIndex = 0;
    const wchar_t* pContextText = nullptr;
};

// Allocate space for tracking up to 2048 sequential draw states per frame execution
inline thread_local std::array<CPUBreadcrumbContext, 2048> g_CpuContextHistory;
inline thread_local UINT g_CurrentRecordingOpIndex = 0;

struct DXMarker {
    DXMarker( const Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& commandList, const wchar_t* text ) :
        c( commandList.Get() )
    {
        if ( c && text ) {
            // Track exactly what string context we are assigning to the CURRENT command slot
            if ( g_CurrentRecordingOpIndex < g_CpuContextHistory.size() ) {
                g_CpuContextHistory[g_CurrentRecordingOpIndex] = { g_CurrentRecordingOpIndex, text };
            }

            UINT byteSize = static_cast<UINT>( (wcslen( text ) + 1) * sizeof( wchar_t ) );
            c->BeginEvent( 0, text, byteSize );

            // Increment tracking slot to match what DRED maps under the hood
            g_CurrentRecordingOpIndex++;
        }
    }

    ~DXMarker() {
        if ( c ) {
            c->EndEvent();
            g_CurrentRecordingOpIndex++;
        }
    }

    DXMarker( const DXMarker& ) = delete;
    DXMarker& operator=( const DXMarker& ) = delete;

private:
    ID3D12GraphicsCommandList* c;
};

// Reset this counter to 0 EVERY TIME you call Reset() on your command list!
inline void ResetCpuContextTracker() {
    g_CurrentRecordingOpIndex = 0;
    for ( auto& slot : g_CpuContextHistory ) {
        slot.pContextText = nullptr;
    }
}

#define DX_ZONE(cmdList, nameStr) DXMarker marker_local_evt_##__LINE__(cmdList, L##nameStr)

struct FrameVobUpload { MeshVisualInfo* visual; D3D12_VERTEX_BUFFER_VIEW instView; UINT numInstances; };

namespace {
    // Swapchain / final-output format. R10G10B10A2 (10-bit) instead of R8: the tonemapped output has much
    // finer gradients (kills banding in sky/fog/soft shadows) at the same 32bpp. Also the format the 2D UI +
    // ImGui + the tonemap resolve write to. Flip-model swapchains support R10G10B10A2_UNORM natively.
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    // HDR scene-color format: the 3D world/VOB/skeletal/water/decal/particle passes accumulate lighting here in
    // linear-ish FLOAT (values may exceed 1.0 — bright sun + additive point lights no longer clip to white), then
    // a fullscreen tonemap resolves it into the swapchain. R16F 4-channel = 64bpp intermediate (recreated on resize).
    constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr UINT kSrvHeapCapacity = 65536;                  // texture SRVs (tier-1 max is 1M; bump-allocated)
    constexpr UINT kUIVertexBufferBytes = 16 * 1024 * 1024;   // per-frame 2D vertex ring (~280k ExVertex)
    constexpr UINT kVobInstanceBufferBytes = 8 * 1024 * 1024; // per-frame VOB instance ring (~58k instances @144B)
    constexpr UINT kParticleInstanceBufferBytes = 8 * 1024 * 1024; // per-frame particle instance ring (~150k @56B)
    constexpr UINT kDecalInstanceBufferBytes = 4 * 1024 * 1024; // per-frame decal instance ring (~52k decals @80B)

    // D3D12SerializeRootSignature is exported from the already-loaded d3d12.dll (we don't link d3d12.lib).
    typedef HRESULT( WINAPI* PFN_SERIALIZE_ROOT_SIG )( const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob** );
    PFN_SERIALIZE_ROOT_SIG LoadSerializeRootSignature() {
        HMODULE d3d12 = LoadLibraryA( "d3d12.dll" );
        if ( !d3d12 ) return nullptr;
        return reinterpret_cast<PFN_SERIALIZE_ROOT_SIG>( GetProcAddress( d3d12, "D3D12SerializeRootSignature" ) );
    }

    // TODO: in the future make this depend on device capabilities.
    constexpr const char* Shadermodel_PS = "ps_6_6";
    constexpr const char* Shadermodel_VS = "vs_6_6";
    constexpr const char* Shadermodel_CS = "cs_6_6";

    static std::wstring ToWideString( LPCSTR str ) {
        if ( !str ) return L"";
        int size_needed = MultiByteToWideChar( CP_UTF8, 0, str, -1, NULL, 0 );
        std::wstring wstr( size_needed, 0 );
        MultiByteToWideChar( CP_UTF8, 0, str, -1, &wstr[0], size_needed );
        // Trim internal null-terminator sizing artifacts from MultiByteToWideChar
        if ( !wstr.empty() && wstr.back() == L'\0' ) {
            wstr.pop_back();
        }
        return wstr;
    }

    bool CompileShaderD3D12(
    _In_reads_bytes_( SrcDataSize ) LPCVOID pSrcData,
    _In_ SIZE_T SrcDataSize,
    _In_opt_ LPCSTR pSourceName,
    _In_reads_opt_( _Inexpressible_( pDefines->Name != NULL ) ) CONST D3D_SHADER_MACRO* pDefines,
    _In_opt_ ID3DInclude* pInclude, // Note: Modern DXC uses IDxcIncludeHandler instead of ID3DInclude!
    _In_opt_ LPCSTR pEntrypoint,
    _In_ LPCSTR pTarget,
    _In_ UINT Flags1,
    _In_ UINT Flags2,
    _Out_ ID3DBlob** ppCode )
    {
        using Microsoft::WRL::ComPtr;

        // 1. Initialize DXC Compiler Instances
        ComPtr<IDxcCompiler3> compiler;
        ComPtr<IDxcUtils> dxcUtils;

        if ( FAILED( DxcCreateInstance( CLSID_DxcCompiler, IID_PPV_ARGS( compiler.GetAddressOf() ) ) ) ||
            FAILED( DxcCreateInstance( CLSID_DxcUtils, IID_PPV_ARGS( dxcUtils.GetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: Failed to create DXC compiler instances. Make sure dxcompiler.dll is loaded.";
            return false;
        }

        // 2. Wrap the source memory block into a DXC Buffer
        DxcBuffer sourceBuffer;
        sourceBuffer.Ptr = pSrcData;
        sourceBuffer.Size = SrcDataSize;
        sourceBuffer.Encoding = DXC_CP_ACP; // Standard ANSI/UTF-8 codepage

        // 3. Build up the DXC CLI argument array
        std::vector<LPCWSTR> arguments;

        // Source filename (for debug/error tracking symbols)
        std::wstring wSourceName = ToWideString( pSourceName ? pSourceName : "ShaderSource" );
        arguments.push_back( wSourceName.c_str() );

        // Entrypoint function name (e.g., -E main)
        std::wstring wEntrypoint = ToWideString( pEntrypoint ? pEntrypoint : "main" );
        arguments.push_back( L"-E" );
        arguments.push_back( wEntrypoint.c_str() );

        // Target Profile / Shader Model (e.g., -T vs_6_0, ps_6_6)
        std::wstring wTarget = ToWideString( pTarget );
        arguments.push_back( L"-T" );
        arguments.push_back( wTarget.c_str() );

        // Handle Debug Configuration Flags
#ifdef DEBUG_D3D11
        arguments.push_back( DXC_ARG_DEBUG );                 // -Zi (Enable debug information)
        arguments.push_back( DXC_ARG_SKIP_OPTIMIZATIONS );    // -Od (Disable optimizations)
#else
        arguments.push_back( DXC_ARG_OPTIMIZATION_LEVEL3 );   // -O3 (Maximum optimization for release)
#endif

        // Translate any legacy macro preprocessors into modern DXC -D parameters
        std::vector<std::wstring> wDefinesStore;
        if ( pDefines ) {
            for ( const D3D_SHADER_MACRO* macro = pDefines; macro->Name != nullptr; ++macro ) {
                std::wstring defineArg = ToWideString( macro->Name );
                if ( macro->Definition ) {
                    defineArg += L"=";
                    defineArg += ToWideString( macro->Definition );
                }
                wDefinesStore.push_back( defineArg );
            }
            for ( const auto& wDef : wDefinesStore ) {
                arguments.push_back( L"-D" );
                arguments.push_back( wDef.c_str() );
            }
        }

        // 4. Run the DXIL compilation pipeline
        ComPtr<IDxcResult> compileResult;
        HRESULT hr = compiler->Compile(
            &sourceBuffer,
            arguments.data(),
            static_cast<UINT32>(arguments.size()),
            nullptr, // Default include handler. Pass a custom IDxcIncludeHandler here if needed.
            IID_PPV_ARGS( compileResult.GetAddressOf() )
        );

        if ( FAILED( hr ) ) {
            LogWarn() << "D3D12: HRESULT compilation failure.";
            return false;
        }

        // 5. Inspect and intercept potential compile errors
        ComPtr<IDxcBlobUtf8> errorBuffer;
        if ( SUCCEEDED( compileResult->GetOutput( DXC_OUT_ERRORS, IID_PPV_ARGS( errorBuffer.GetAddressOf() ), nullptr ) ) ) {
            if ( errorBuffer && errorBuffer->GetStringLength() > 0 ) {
                LogWarn() << "D3D12: DXC Shader Compilation warning/error:\n" << errorBuffer->GetStringPointer();
            }
        }

        // Check if the overall operation succeeded or failed
        HRESULT status;
        if ( FAILED( compileResult->GetStatus( &status ) ) || FAILED( status ) ) {
            return false;
        }

        // 6. Extract the compiled byte code blob and translate it to an ID3DBlob container
        ComPtr<IDxcBlob> shaderCodeBlob;
        if ( SUCCEEDED( compileResult->GetOutput( DXC_OUT_OBJECT, IID_PPV_ARGS( shaderCodeBlob.GetAddressOf() ), nullptr ) ) ) {
            // Since your graphics core architecture expects ID3DBlob interfaces down the stream, 
            // query the DXC utilities layer to cast/wrap the compiled DXC blob back into standard ID3DBlob memory block!
            if ( SUCCEEDED( dxcUtils->CreateBlobFromBlob(
                shaderCodeBlob.Get(),
                0,
                static_cast<UINT32>(shaderCodeBlob->GetBufferSize()),
                reinterpret_cast<IDxcBlob**>(ppCode) ) ) ) {
                return true;
            }
        }

        return false;
    }

    // Inline HLSL for the 2D UI path. VS mirrors VS_TransformedEx (screen-space xyzrhw -> clip space,
    // rhw packed in Normal.x). PS emulates the fixed-function texture-stage pipeline (mirrors
    // Shaders/FixedFunctionPipeline.h): FF_Stages[0/1] color ops + args + diffuse-alpha test, driven by
    // the FFPipelineConstantBuffer (b1) which receives Gothic's GraphicsState each draw. Per-draw BLEND
    // modes (opaque/alpha/additive/modulate/...) are handled by selecting a matching PSO, not here.
    // Limitation: only texture0 is bound, so a 2nd stage samples texture0 (menus use 1 stage -> exact;
    // the world's 2-texture lightmap path is a Phase-2 concern).
    constexpr char kUIShaderSource[] = R"(
cbuffer Viewport : register(b0) { float2 V_ViewportPos; float2 V_ViewportSize; };

struct TextureStage { int colorop; int colorarg1; int colorarg2; int alphaop; int alphaarg1; int alphaarg2; int2 pad; };
cbuffer FFPipelineConstantBuffer : register(b1)
{
    float  FF_FogWeight;   float3 FF_FogColor;
    float  FF_FogNear;     float  FF_FogFar;   float FF_zNear; float FF_zFar;
    float3 FF_AmbientLighting; float FF_Time;
    float4 FF_TextureFactor;
    float  FF_AlphaRef;    uint   FF_GSwitches; float2 ggs_Pad3;
    TextureStage FF_Stages[2];
};

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

#define TA_DIFFUSE 0
#define TA_CURRENT 1
#define TA_TEXTURE 2
#define TA_TFACTOR 3
#define TOP_DISABLE    1
#define TOP_SELECTARG1 2
#define TOP_SELECTARG2 3
#define TOP_MODULATE   4
#define TOP_MODULATE2X 5
#define TOP_MODULATE4X 6
#define TOP_ADD        7
#define TOP_SUBTRACT   10
#define GSWITCH_ALPHAREF 2

struct VS_IN  { float3 pos:POSITION; float3 nrm:NORMAL; float2 t0:TEXCOORD0; float2 t1:TEXCOORD1; float4 dif:DIFFUSE; };
struct VS_OUT { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float2 uv2:TEXCOORD1; float4 dif:TEXCOORD2; };

VS_OUT VSMain( VS_IN i ) {
    VS_OUT o;
    float rhw = i.nrm.x;                 // rhw stored in Normal.x
    float2 ndc;
    ndc.x = ((2.0 * (i.pos.x - V_ViewportPos.x)) / V_ViewportSize.x) - 1.0;
    ndc.y = 1.0 - ((2.0 * (i.pos.y - V_ViewportPos.y)) / V_ViewportSize.y);
    float actualW = 1.0 / rhw;
    o.pos = float4(float3(ndc, i.pos.z) * actualW, actualW);
    o.uv  = i.t0;
    o.uv2 = i.t1;
    o.dif = i.dif;
    return o;
}

float4 SelectArg( int arg, float4 current, float4 diffuse, float2 uv ) {
    switch ( arg ) {
    case TA_DIFFUSE: return diffuse;
    case TA_CURRENT: return current;
    case TA_TEXTURE: return tx.Sample( smp, uv );
    case TA_TFACTOR: return FF_TextureFactor;
    }
    return float4( 0, 1, 0, 0 );
}

float4 RunStage( int op, int a1, int a2, float4 current, float4 diffuse, float2 uv ) {
    switch ( op ) {
    case TOP_DISABLE:    return 0;
    case TOP_SELECTARG1: return SelectArg( a1, current, diffuse, uv );
    case TOP_SELECTARG2: return SelectArg( a2, current, diffuse, uv );
    case TOP_MODULATE:   return SelectArg( a1, current, diffuse, uv ) * SelectArg( a2, current, diffuse, uv );
    case TOP_MODULATE2X: return SelectArg( a1, current, diffuse, uv ) * SelectArg( a2, current, diffuse, uv ) * 2;
    case TOP_MODULATE4X: return SelectArg( a1, current, diffuse, uv ) * SelectArg( a2, current, diffuse, uv ) * 4;
    case TOP_ADD:        return SelectArg( a1, current, diffuse, uv ) + SelectArg( a2, current, diffuse, uv );
    case TOP_SUBTRACT:   return SelectArg( a1, current, diffuse, uv ) - SelectArg( a2, current, diffuse, uv );
    }
    return SelectArg( a1, current, diffuse, uv );   // graceful fallback for unhandled ops
}

float4 PSMain( VS_OUT i ) : SV_TARGET {
    float4 diffuse = i.dif.bgra;         // vertex color 0xAARRGGBB read as RGBA -> swizzle back to RGBA
    float4 color = RunStage( FF_Stages[0].colorop, FF_Stages[0].colorarg1, FF_Stages[0].colorarg2, diffuse, diffuse, i.uv );
    [branch] if ( FF_Stages[1].colorop != TOP_DISABLE )
        color = RunStage( FF_Stages[1].colorop, FF_Stages[1].colorarg1, FF_Stages[1].colorarg2, color, diffuse, i.uv2 );
    [branch] if ( ( FF_GSwitches & GSWITCH_ALPHAREF ) != 0 )
        clip( color.a - FF_AlphaRef );
    return color;
}
)";

    // Phase-2 world shader (textured). The wrapped world mesh is the packed 36-byte ExVertexStructGPU;
    // we bind Position (float3 @0), TexCoord0 (float2 @20) and Color (R8G8B8A8 @32), ignoring the packed
    // normal/tangent/uv2 for now. World-mesh verts are already in world space, so the transform is just
    // ViewProj (identity world) — matches D3D11 VS_ExPacked: mul(float4(pos,1), ViewProj), reversed-Z.
    // PS samples the diffuse texture, modulates by the baked vertex color (Gothic packs a DWORD read as
    // R8G8B8A8 -> .bgr recovers RGB), and does a fixed alpha-test cutout (foliage/fences). No G-buffer /
    // no deferred lighting yet: the baked vertex color stands in for lighting.
    constexpr char kWorldShaderSource[] = R"(
// Default (column-major) matrix packing — matches D3D11's VS_ExPacked, which reads the same
// row-major XMFLOAT4X4 bytes we upload here, so mul(float4(pos,1), ViewProj) is byte-for-byte identical.
cbuffer WorldCB : register(b0) { float4x4 ViewProj; };
cbuffer FogCB   : register(b1) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
cbuffer LightCB : register(b2) { uint LightCount; uint NumTilesX; uint2 _lpad; };   // Forward+ tiled: light count + tiles/row

// Per-frame visible point light (torches/campfires/spells). Byte-identical to D3D11 TiledPointLight (48 B);
// this pass reads PositionWorld/Range/Color — PositionView/ShadowCubeIndex feed the cull/shadow paths.
// Bound as a ROOT descriptor SRV (no descriptor-table slot). The per-tile grid produced by the light-cull
// compute (DispatchLightCulling) narrows this loop to only the lights that touch each 16x16 screen tile.
struct GPULight { float3 PositionView; float Range; float4 Color; float3 PositionWorld; int ShadowCubeIndex; };
struct LightGrid { uint Offset; uint Count; };
StructuredBuffer<GPULight>  Lights        : register(t1);   // root SRV — all visible lights, indexed by the grid
StructuredBuffer<LightGrid> LightGridBuf  : register(t2);   // root SRV — per-tile {Offset,Count}
StructuredBuffer<uint>      LightIndexBuf : register(t3);   // root SRV — per-tile light-index slices
#define TILE_SIZE 16u
#define MAX_LIGHTS_PER_TILE 32u

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

// CSM sun-shadow sampling (P2.9c-4a). b3 = the per-frame shadow constants (cascade view-projs + sun dir +
// darkening strength + per-cascade world texel size); t4 = the D32 cascade array (normal-Z, 1.0 == far);
// s2 = a LESS_EQUAL PCF comparison sampler. Uploaded row-major, read column-major → mul(pos, CascadeVP[c])
// matches the caster exactly. Shadow modulates the BAKED vertex lighting (darken sun-facing surfaces the
// sun can't reach); replacing baked lighting with a full computed sun term (FP_ComputeSunLighting) is later.
#define NUM_CSM_CASCADES 3
cbuffer ShadowCB : register(b3)
{
    float4x4 CascadeViewProj[NUM_CSM_CASCADES];
    float3   SunDirWS;          float ShadowMapSize;    // dir TOWARD sun; shadow-map resolution
    float3   SunColor;          float SunIntensity;     // sun color (sRGB) + strength (0 when sun below horizon)
    float3   CascadeTexelWorld; float AmbientStrength;  // world units/texel; SQ_ShadowStrength (ambient/sky term)
    float    ShadowAOStrength;  float WorldAOStrength;  float2 _shpad;   // vertLighting -> AO modulation weights
};
Texture2DArray          ShadowMap : register(t4);
SamplerComparisonState  shadowCmp : register(s2);
// Per-material bindless indices (root consts b6): SM6.6 ResourceDescriptorHeap[...] indices for this material's
// diffuse + normal + ORM maps. The world mesh is drawn via ExecuteIndirect (P2.11), which sets these three per
// draw — so the diffuse is sampled bindless too (no per-draw descriptor table). MatNormalIndex == 0xFFFFFFFF ->
// no normal map (skip perturb); MatOrmIndex is always valid (1x1 default = AO 1 / rough 0.5 / metal 0).
cbuffer MaterialCB : register(b6) { uint MatNormalIndex; uint MatOrmIndex; uint MatDiffuseIndex; };
TextureCubeArray        PointShadowCubes : register(t5);   // point-light shadow cubes (P2.10d), R16 linear depth

// Point-light shadow: returns 1 = lit, 0 = occluded. The cube stores the NATURAL hyperbolic z of the caster's
// 90-deg PerspectiveFovLH(near 15, far range*2). Reconstruct the same z from the fragment: the depth on a cube
// face is driven by the DOMINANT-AXIS distance (the face's view-space z), so zView = max(|dx|,|dy|,|dz|), then
// apply the LH projection z-map. Most acne bias is the PSO's hardware slope bias; add a small normal offset +
// constant. 4-tap rotated-disk PCF softens the edges; a camera-distance fade is applied at the call site.
float SamplePointShadow( int cubeIndex, float3 wpos, float3 N, float3 lightPos, float range )
{
    float3 d  = ( wpos + N * ( range * 0.01 ) ) - lightPos;   // normal-offset bias (world-space, uniform)
    float3 ad = abs( d );
    float  zView = max( ad.x, max( ad.y, ad.z ) );            // dominant cube-axis depth = the face's view-space z
    const float n = 15.0;
    float  f = range * 2.0;
    float  compareDepth = ( f / ( f - n ) ) * ( 1.0 - n / zView ) - 0.001;   // same LH hyperbolic z the caster wrote
    float3 L = normalize( d );

    // P2.10e polish: 4-tap rotated-disk PCF on a basis perpendicular to L (cube sampling follows the offset dir,
    // so a small angular offset lands on neighbouring texels). Softens the previously single-tap hard edges. The
    // offset grows a little with distance so the world-space penumbra stays roughly constant across the range.
    float3 up = abs( L.y ) < 0.99 ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    float3 t  = normalize( cross( up, L ) );
    float3 bt = cross( L, t );
    float  r  = 0.006 + 0.010 * saturate( zView / f );
    static const float2 kDisk[4] = { float2( 0.7, 0.7 ), float2( -0.7, 0.7 ), float2( 0.7, -0.7 ), float2( -0.7, -0.7 ) };
    float sh = 0.0;
    [unroll]
    for ( int s = 0; s < 4; ++s )
    {
        float3 o = normalize( L + ( kDisk[s].x * t + kDisk[s].y * bt ) * r );
        sh += PointShadowCubes.SampleCmpLevelZero( shadowCmp, float4( o, (float)cubeIndex ), compareDepth );
    }
    return sh * 0.25;
}

// Returns 1.0 = fully lit, 0.0 = fully occluded. Picks the first cascade whose footprint contains the point
// (0 tightest), applies a per-cascade world-space normal bias, and does a 3x3 PCF tap. Mirrors the D3D11
// ComputeCascadedShadowValueSoft selection/bounds (GetCascadeUVAndBounds) minus the blue-noise/PCSS machinery.
float ComputeSunShadow( float3 wpos, float3 N )
{
    const float margin = 1.5 / ShadowMapSize;
    const float texel  = 1.0 / ShadowMapSize;
    [unroll]
    for ( int c = 0; c < NUM_CSM_CASCADES; ++c )
    {
        float3 biased = wpos + N * ( CascadeTexelWorld[c] * 1.5 );   // normal bias scaled to this cascade's texel
        float4 sp = mul( float4( biased, 1.0 ), CascadeViewProj[c] );
        float2 uv = sp.xy * float2( 0.5, -0.5 ) + 0.5;
        if ( uv.x > margin && uv.x < 1.0 - margin && uv.y > margin && uv.y < 1.0 - margin &&
             sp.z >= 0.0 && sp.z <= 1.0 )
        {
            // Wider, cascade-scaled PCF (P2.9c-3c): far cascades cover more world per texel (sub-texel foliage
            // → temporal "blinking"), so widen the kernel step with the cascade index to spatially average that
            // flicker into a soft, stable penumbra. 5x5 taps; near cascade stays near-1-texel (crisp).
            float pcfStep = texel * ( 1.0 + float( c ) * 1.5 );
            float sh = 0.0;
            [unroll] for ( int y = -2; y <= 2; ++y )
            [unroll] for ( int x = -2; x <= 2; ++x )
                sh += ShadowMap.SampleCmpLevelZero( shadowCmp, float3( uv + float2( x, y ) * pcfStep, c ), sp.z - 0.0015 );
            return sh / 25.0;
        }
    }
    return 1.0;   // outside all cascades → treat as lit
}

// Octahedral normal decode — matches Shaders/VertexPacking.h DecodeOctNormal (the packed 36-byte vertex
// stores the normal as R16G16_SNORM at offset 12; world-mesh normals are already world-space).
float3 DecodeOctNormal( float2 e )
{
    float3 n = float3( e.xy, 1.0 - abs( e.x ) - abs( e.y ) );
    float t = saturate( -n.z );
    n.xy += select(n.xy >= 0., -t, t);
    return normalize( n );
}

// Accumulate dynamic point lights at a world-space surface point using the Forward+ tile grid: derive the
// tile from SV_Position.xy, read its {Offset,Count} slice, and loop ONLY the lights culled into that tile
// (indices into the global Lights buffer). Lighting math ports D3D11's FP_ComputePointLighting: range cull,
// N.L, the exact falloff = nd*(nd*0.2+0.8), per-light saturate, additive. The count is clamped to the
// per-tile capacity so a garbage grid entry can never spin the loop away (that reads as a GPU timeout).
// --- Cook-Torrance GGX PBR (ported verbatim from the D3D11 feat/pbr branch: Shaders/include/PointLightShadows.h) ---
// Staged PBR (P3-PBR-1): albedo is sRGB-decoded to LINEAR in the PS; Gothic's baked vertex lighting is kept as the
// diffuse/ambient base; the SUN adds a specular-only glint and the tiled POINT lights use the full BRDF. Material
// params are constant defaults for now (no ORM/normal maps yet — that's a later increment with texture-loading infra).
static const float PBR_PI = 3.14159265;
static const float MI_Roughness = 0.9;   // default perceptual roughness (Gothic surfaces are mostly rough dielectrics)
static const float MI_Metallic  = 0.0;   // default metallic (dielectric)
static const float SunSpecIntensity = 1.0;

float3 SrgbToLinear( float3 c )   // accurate sRGB EOTF — linearize gamma-encoded albedo so lighting is done in linear space
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

float  PBR_SafeRoughness( float r ) { return max( saturate( r ), 0.045 ); }
float  PBR_DistributionGGX( float NdotH, float roughness )
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * ( a2 - 1.0 ) + 1.0;
    return a2 / max( PBR_PI * denom * denom, 1e-4 );
}
float  PBR_GeometrySchlickGGX( float NdotX, float roughness )
{
    float r = roughness + 1.0;
    float k = ( r * r ) / 8.0;
    return NdotX / max( NdotX * ( 1.0 - k ) + k, 1e-4 );
}
float  PBR_GeometrySmith( float NdotV, float NdotL, float roughness )
{
    return PBR_GeometrySchlickGGX( NdotV, roughness ) * PBR_GeometrySchlickGGX( NdotL, roughness );
}
float  PBR_Pow5( float x ) { float x2 = x * x; return x2 * x2 * x; }
float3 PBR_FresnelSchlick( float cosTheta, float3 F0 ) { return F0 + ( 1.0 - F0 ) * PBR_Pow5( saturate( 1.0 - cosTheta ) ); }

// Full Cook-Torrance (energy-conserving diffuse + specular). attenuation folds in falloff/shadow; NdotL applied here.
float3 PBR_DirectLighting( float3 baseColor, float3 lightColor, float3 N, float3 V, float3 L,
                           float roughness, float metallic, float attenuation )
{
    float NdotL = saturate( dot( N, L ) );
    float NdotV = saturate( dot( N, V ) );
    if ( NdotL <= 0.0 || NdotV <= 0.0 || attenuation <= 0.0 ) return 0.0;
    float3 H = normalize( V + L );
    float NdotH = saturate( dot( N, H ) );
    float VdotH = saturate( dot( V, H ) );
    float  cr = PBR_SafeRoughness( roughness * roughness );   // perceptual->physical (the branch squares here)
    float  cm = saturate( metallic );
    float3 F0 = lerp( float3( 0.04, 0.04, 0.04 ), baseColor, cm );
    float  D = PBR_DistributionGGX( NdotH, cr );
    float  G = PBR_GeometrySmith( NdotV, NdotL, cr );
    float3 F = PBR_FresnelSchlick( VdotH, F0 );
    float3 specular = ( D * G * F ) / max( 4.0 * NdotV * NdotL, 1e-4 );
    float3 kD = ( 1.0 - F ) * ( 1.0 - cm );
    float3 diffuse = kD * baseColor / PBR_PI;
    return ( diffuse + specular ) * lightColor * ( NdotL * attenuation );
}

// Specular-only variant — used for the SUN so we don't double-count Gothic's baked diffuse sun.
float3 PBR_DirectSpecularOnly( float3 baseColor, float3 lightColor, float3 N, float3 V, float3 L,
                               float roughness, float metallic, float attenuation )
{
    float NdotL = saturate( dot( N, L ) );
    float NdotV = saturate( dot( N, V ) );
    if ( NdotL <= 0.0 || NdotV <= 0.0 || attenuation <= 0.0 ) return 0.0;
    float3 H = normalize( V + L );
    float NdotH = saturate( dot( N, H ) );
    float VdotH = saturate( dot( V, H ) );
    float  cr = PBR_SafeRoughness( roughness * roughness );
    float  cm = saturate( metallic );
    float3 F0 = lerp( float3( 0.04, 0.04, 0.04 ), baseColor, cm );
    float  D = PBR_DistributionGGX( NdotH, cr );
    float  G = PBR_GeometrySmith( NdotV, NdotL, cr );
    float3 F = PBR_FresnelSchlick( VdotH, F0 );
    float3 specular = ( D * G * F ) / max( 4.0 * NdotV * NdotL, 1e-4 );
    return specular * lightColor * ( NdotL * attenuation );
}

// Tangent-space normal-map support (ported from feat/pbr Toolbox.h). Z is ALWAYS reconstructed from XY, so BC5
// (2-channel) and BC1 (we ignore B, recompute it) both decode with one path. `p` = world position for the
// derivative-based TBN basis. If normal-mapped specular looks mirrored, flip the handedness comparison sign.
float3x3 CotangentFrame( float3 N, float3 p, float2 uv )
{
    float3 dp1 = ddx( p ), dp2 = ddy( p );
    float2 duv1 = ddx( uv ), duv2 = ddy( uv );
    float3 dp2perp = cross( dp2, N ), dp1perp = cross( N, dp1 );
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float handedness = ( duv1.x * duv2.y - duv1.y * duv2.x ) < 0.0 ? 1.0 : -1.0;
    T *= handedness;
    float invmax = rsqrt( max( dot( T, T ), dot( B, B ) ) );
    return float3x3( T * invmax, B * invmax, N );
}
float3 PerturbNormal( float3 N, float3 p, Texture2D nrmTex, float2 uv, SamplerState samp )
{
    float2 nxy = nrmTex.Sample( samp, uv ).xy * 2.0 - 1.0;
    float  nz  = sqrt( saturate( 1.0 - dot( nxy, nxy ) ) );   // reconstruct Z (BC5/BC1)
    float3 nrm = normalize( float3( nxy, nz ) );
    return normalize( mul( nrm, CotangentFrame( N, p, uv ) ) );
}

// PBR sun lighting (stage 2 — ports feat/pbr FP_ComputeSunLighting): ambient (sky/GI) + direct Cook-Torrance
// (diffuse + specular). vertLighting = Gothic's baked vertex-light scalar, used as an AO modulator. roughness/
// metallic/ao come from the ORM map (or its 1x1 default). ssao still 1 (no SSAO pass yet). shadow = 1 lit / 0 occ.
float3 ComputeSunLightingPBR( float3 wpos, float3 N, float3 albedo, float vertLighting, float shadow,
                              float roughness, float metallic, float ao )
{
    float3 V = normalize( CamPosWS - wpos );
    float3 L = SunDirWS;                            // dir toward the sun (world space)
    float3 sunCol = SrgbToLinear( SunColor );
    float  sunLum = dot( sunCol, float3( 0.3333, 0.3333, 0.3333 ) );
    const float ssao = 1.0;
    float sun      = saturate( dot( L, N ) * shadow );
    float shadowAO = lerp( 1.0, vertLighting, ShadowAOStrength ) * ao;
    float worldAO  = lerp( 1.0, vertLighting, WorldAOStrength ) * ao;
    float sunAtten = sun * worldAO * SunIntensity;
    float3 directSun  = PBR_DirectLighting( albedo, sunCol, N, V, L, roughness, metallic, sunAtten );
    float3 ambientSun = albedo * AmbientStrength * sunLum * shadowAO * ssao;   // ambient/sky term
    return ambientSun + directSun;
}

// Tiled point lights via the Forward+ grid, now with the full Cook-Torrance BRDF. `albedo` is LINEAR (sRGB-decoded
// in the PS). Ports D3D11 feat/pbr FP_ComputePointLighting (PLS_ComputePointLightLightingPBR).
float3 AccumTiledPointLights( float2 svpos, float3 wpos, float3 N, float3 albedo, float roughness, float metallic )
{
    uint2 tile = uint2( svpos ) / TILE_SIZE;
    uint  tileIndex = tile.y * NumTilesX + tile.x;
    LightGrid g = LightGridBuf[tileIndex];
    uint n = min( g.Count, MAX_LIGHTS_PER_TILE );
    float3 V = normalize( CamPosWS - wpos );
    float3 total = 0;
    for ( uint k = 0; k < n; k++ )
    {
        GPULight L = Lights[ LightIndexBuf[g.Offset + k] ];
        float3 dir = L.PositionWorld - wpos;
        float dist = length( dir );
        if ( dist >= L.Range ) continue;
        dir /= dist;
        float nd  = saturate( 1.0 - dist / L.Range );
        float falloff = nd * ( nd * 0.2 + 0.8 );   // PLS_ComputeRangeFalloff
        float3 lit = PBR_DirectLighting( albedo, L.Color.rgb, N, V, dir, roughness, metallic, falloff );
        if ( L.ShadowCubeIndex >= 0 )
        {
            float sh = SamplePointShadow( L.ShadowCubeIndex, wpos, N, L.PositionWorld, L.Range );
            float camDist = length( L.PositionView );
            float fade    = saturate( ( camDist - L.Range * 6.0 ) / ( L.Range * 3.0 ) );
            lit *= lerp( sh, 1.0, fade );
        }
        total += lit;
    }
    return total;
}

struct VS_IN  { float3 pos : POSITION; float2 nrm : NORMAL; float2 uv : TEXCOORD0; float4 col : DIFFUSE; };
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float fogDist : TEXCOORD2; float3 wpos : TEXCOORD3; float3 wnrm : TEXCOORD4; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = i.col;
    o.wpos = i.pos;                          // world verts are already world-space
    o.wnrm = DecodeOctNormal( i.nrm );       // already world-space
    o.fogDist = length( i.pos - CamPosWS );
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];   // bindless diffuse (ExecuteIndirect, P2.11)
    float4 t = difTex.Sample( smp, i.uv );
    clip( t.a - 0.5 );                        // fixed alpha-test cutout (opaque textures have a==1 -> kept)
    float3 N = normalize( i.wnrm );
    if ( MatNormalIndex != 0xffffffff )       // bindless normal map (BC5/BC1, Z reconstructed) if this material has one
    {
        Texture2D nrmTex = ResourceDescriptorHeap[MatNormalIndex];
        N = PerturbNormal( N, i.wpos, nrmTex, i.uv, smp );
    }
    Texture2D ormTex = ResourceDescriptorHeap[MatOrmIndex];   // r=AO g=roughness b=metallic (1x1 default when no _FX)
    float3 orm = ormTex.Sample( smp, i.uv ).rgb;
    float3 albedo = SrgbToLinear( t.rgb );    // linearize for PBR (all HDR-buffer values are linear now)
    float vertLighting = i.col.g;             // Gothic baked vertex lighting (green channel) as the AO modulator
    float shadow = ComputeSunShadow( i.wpos, N );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, vertLighting, shadow, orm.g, orm.b, orm.r );
    rgb += AccumTiledPointLights( i.clip.xy, i.wpos, N, albedo, orm.g, orm.b );
    // Linear distance fog toward the (linearized) atmosphere color — keeps the HDR buffer consistently linear.
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    rgb = lerp( rgb, SrgbToLinear( FogColor ), f );
    return float4( rgb, 1.0 );
}
)";

    // Phase-2 instanced VOB shader. Slot 0 = packed vertex (Position@0, TexCoord0@20); slot 1 =
    // per-instance data (world matrix as 4 rows + instance color) from VobInstanceInfo. Mirrors
    // VS_ExInstancedObj.hlsl's core: worldPos = mul(pos, InstanceWorldMatrix); clip = mul(worldPos,
    // ViewProj). PS samples the diffuse, alpha-tests, modulates by the per-instance (ground-light)
    // color. Wind / player-influence / motion-vectors / normalmap are skipped for first-light.
    constexpr char kVobShaderSource[] = R"(
cbuffer WorldCB : register(b0) { float4x4 ViewProj; };   // default column-major packing (see world shader)
cbuffer FogCB   : register(b1) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
cbuffer LightCB : register(b2) { uint LightCount; uint NumTilesX; uint2 _lpad; };

// Forward+ tiled point lights (root-descriptor SRVs + per-tile grid) — see the world shader for the rationale.
struct GPULight { float3 PositionView; float Range; float4 Color; float3 PositionWorld; int ShadowCubeIndex; };
struct LightGrid { uint Offset; uint Count; };
StructuredBuffer<GPULight>  Lights        : register(t1);
StructuredBuffer<LightGrid> LightGridBuf  : register(t2);
StructuredBuffer<uint>      LightIndexBuf : register(t3);
#define TILE_SIZE 16u
#define MAX_LIGHTS_PER_TILE 32u

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

// CSM sun-shadow sampling (P2.9c-4b) — identical to the world shader's block (b3/t4/s2 free here too).
#define NUM_CSM_CASCADES 3
cbuffer ShadowCB : register(b3)
{
    float4x4 CascadeViewProj[NUM_CSM_CASCADES];
    float3   SunDirWS;          float ShadowMapSize;
    float3   SunColor;          float SunIntensity;
    float3   CascadeTexelWorld; float AmbientStrength;
    float    ShadowAOStrength;  float WorldAOStrength;  float2 _shpad;
};
Texture2DArray          ShadowMap : register(t4);
SamplerComparisonState  shadowCmp : register(s2);
// Per-material bindless indices (root consts b6): SM6.6 ResourceDescriptorHeap[...] indices for this material's
// normal + ORM maps. MatNormalIndex == 0xFFFFFFFF -> no normal map (skip perturb); MatOrmIndex is always valid
// (the 1x1 default ORM = AO 1 / rough 0.5 / metal 0 when the material has no _FX map), so ORM is sampled branchlessly.
cbuffer MaterialCB : register(b6) { uint MatNormalIndex; uint MatOrmIndex; };
TextureCubeArray        PointShadowCubes : register(t5);   // point-light shadow cubes (P2.10d), R16 linear depth

// Point-light shadow: returns 1 = lit, 0 = occluded. The cube stores the NATURAL hyperbolic z of the caster's
// 90-deg PerspectiveFovLH(near 15, far range*2). Reconstruct the same z from the fragment: the depth on a cube
// face is driven by the DOMINANT-AXIS distance (the face's view-space z), so zView = max(|dx|,|dy|,|dz|), then
// apply the LH projection z-map. Most acne bias is the PSO's hardware slope bias; add a small normal offset +
// constant. 4-tap rotated-disk PCF softens the edges; a camera-distance fade is applied at the call site.
float SamplePointShadow( int cubeIndex, float3 wpos, float3 N, float3 lightPos, float range )
{
    float3 d  = ( wpos + N * ( range * 0.01 ) ) - lightPos;   // normal-offset bias (world-space, uniform)
    float3 ad = abs( d );
    float  zView = max( ad.x, max( ad.y, ad.z ) );            // dominant cube-axis depth = the face's view-space z
    const float n = 15.0;
    float  f = range * 2.0;
    float  compareDepth = ( f / ( f - n ) ) * ( 1.0 - n / zView ) - 0.001;   // same LH hyperbolic z the caster wrote
    float3 L = normalize( d );

    // P2.10e polish: 4-tap rotated-disk PCF on a basis perpendicular to L (cube sampling follows the offset dir,
    // so a small angular offset lands on neighbouring texels). Softens the previously single-tap hard edges. The
    // offset grows a little with distance so the world-space penumbra stays roughly constant across the range.
    float3 up = abs( L.y ) < 0.99 ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    float3 t  = normalize( cross( up, L ) );
    float3 bt = cross( L, t );
    float  r  = 0.006 + 0.010 * saturate( zView / f );
    static const float2 kDisk[4] = { float2( 0.7, 0.7 ), float2( -0.7, 0.7 ), float2( 0.7, -0.7 ), float2( -0.7, -0.7 ) };
    float sh = 0.0;
    [unroll]
    for ( int s = 0; s < 4; ++s )
    {
        float3 o = normalize( L + ( kDisk[s].x * t + kDisk[s].y * bt ) * r );
        sh += PointShadowCubes.SampleCmpLevelZero( shadowCmp, float4( o, (float)cubeIndex ), compareDepth );
    }
    return sh * 0.25;
}

float ComputeSunShadow( float3 wpos, float3 N )
{
    const float margin = 1.5 / ShadowMapSize;
    const float texel  = 1.0 / ShadowMapSize;
    [unroll]
    for ( int c = 0; c < NUM_CSM_CASCADES; ++c )
    {
        float3 biased = wpos + N * ( CascadeTexelWorld[c] * 1.5 );
        float4 sp = mul( float4( biased, 1.0 ), CascadeViewProj[c] );
        float2 uv = sp.xy * float2( 0.5, -0.5 ) + 0.5;
        if ( uv.x > margin && uv.x < 1.0 - margin && uv.y > margin && uv.y < 1.0 - margin &&
             sp.z >= 0.0 && sp.z <= 1.0 )
        {
            // Wider, cascade-scaled PCF (P2.9c-3c): far cascades cover more world per texel (sub-texel foliage
            // → temporal "blinking"), so widen the kernel step with the cascade index to spatially average that
            // flicker into a soft, stable penumbra. 5x5 taps; near cascade stays near-1-texel (crisp).
            float pcfStep = texel * ( 1.0 + float( c ) * 1.5 );
            float sh = 0.0;
            [unroll] for ( int y = -2; y <= 2; ++y )
            [unroll] for ( int x = -2; x <= 2; ++x )
                sh += ShadowMap.SampleCmpLevelZero( shadowCmp, float3( uv + float2( x, y ) * pcfStep, c ), sp.z - 0.0015 );
            return sh / 25.0;
        }
    }
    return 1.0;
}

float3 DecodeOctNormal( float2 e )
{
    float3 n = float3( e.xy, 1.0 - abs( e.x ) - abs( e.y ) );
    float t = saturate( -n.z );
    n.xy += select(n.xy >= 0., -t, t);
    return normalize( n );
}

// --- Cook-Torrance GGX PBR (ported verbatim from the D3D11 feat/pbr branch: Shaders/include/PointLightShadows.h) ---
// Staged PBR (P3-PBR-1): albedo is sRGB-decoded to LINEAR in the PS; Gothic's baked vertex lighting is kept as the
// diffuse/ambient base; the SUN adds a specular-only glint and the tiled POINT lights use the full BRDF. Material
// params are constant defaults for now (no ORM/normal maps yet — that's a later increment with texture-loading infra).
static const float PBR_PI = 3.14159265;
static const float MI_Roughness = 0.9;   // default perceptual roughness (Gothic surfaces are mostly rough dielectrics)
static const float MI_Metallic  = 0.0;   // default metallic (dielectric)
static const float SunSpecIntensity = 1.0;

float3 SrgbToLinear( float3 c )   // accurate sRGB EOTF — linearize gamma-encoded albedo so lighting is done in linear space
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

float  PBR_SafeRoughness( float r ) { return max( saturate( r ), 0.045 ); }
float  PBR_DistributionGGX( float NdotH, float roughness )
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * ( a2 - 1.0 ) + 1.0;
    return a2 / max( PBR_PI * denom * denom, 1e-4 );
}
float  PBR_GeometrySchlickGGX( float NdotX, float roughness )
{
    float r = roughness + 1.0;
    float k = ( r * r ) / 8.0;
    return NdotX / max( NdotX * ( 1.0 - k ) + k, 1e-4 );
}
float  PBR_GeometrySmith( float NdotV, float NdotL, float roughness )
{
    return PBR_GeometrySchlickGGX( NdotV, roughness ) * PBR_GeometrySchlickGGX( NdotL, roughness );
}
float  PBR_Pow5( float x ) { float x2 = x * x; return x2 * x2 * x; }
float3 PBR_FresnelSchlick( float cosTheta, float3 F0 ) { return F0 + ( 1.0 - F0 ) * PBR_Pow5( saturate( 1.0 - cosTheta ) ); }

// Full Cook-Torrance (energy-conserving diffuse + specular). attenuation folds in falloff/shadow; NdotL applied here.
float3 PBR_DirectLighting( float3 baseColor, float3 lightColor, float3 N, float3 V, float3 L,
                           float roughness, float metallic, float attenuation )
{
    float NdotL = saturate( dot( N, L ) );
    float NdotV = saturate( dot( N, V ) );
    if ( NdotL <= 0.0 || NdotV <= 0.0 || attenuation <= 0.0 ) return 0.0;
    float3 H = normalize( V + L );
    float NdotH = saturate( dot( N, H ) );
    float VdotH = saturate( dot( V, H ) );
    float  cr = PBR_SafeRoughness( roughness * roughness );   // perceptual->physical (the branch squares here)
    float  cm = saturate( metallic );
    float3 F0 = lerp( float3( 0.04, 0.04, 0.04 ), baseColor, cm );
    float  D = PBR_DistributionGGX( NdotH, cr );
    float  G = PBR_GeometrySmith( NdotV, NdotL, cr );
    float3 F = PBR_FresnelSchlick( VdotH, F0 );
    float3 specular = ( D * G * F ) / max( 4.0 * NdotV * NdotL, 1e-4 );
    float3 kD = ( 1.0 - F ) * ( 1.0 - cm );
    float3 diffuse = kD * baseColor / PBR_PI;
    return ( diffuse + specular ) * lightColor * ( NdotL * attenuation );
}

// Specular-only variant — used for the SUN so we don't double-count Gothic's baked diffuse sun.
float3 PBR_DirectSpecularOnly( float3 baseColor, float3 lightColor, float3 N, float3 V, float3 L,
                               float roughness, float metallic, float attenuation )
{
    float NdotL = saturate( dot( N, L ) );
    float NdotV = saturate( dot( N, V ) );
    if ( NdotL <= 0.0 || NdotV <= 0.0 || attenuation <= 0.0 ) return 0.0;
    float3 H = normalize( V + L );
    float NdotH = saturate( dot( N, H ) );
    float VdotH = saturate( dot( V, H ) );
    float  cr = PBR_SafeRoughness( roughness * roughness );
    float  cm = saturate( metallic );
    float3 F0 = lerp( float3( 0.04, 0.04, 0.04 ), baseColor, cm );
    float  D = PBR_DistributionGGX( NdotH, cr );
    float  G = PBR_GeometrySmith( NdotV, NdotL, cr );
    float3 F = PBR_FresnelSchlick( VdotH, F0 );
    float3 specular = ( D * G * F ) / max( 4.0 * NdotV * NdotL, 1e-4 );
    return specular * lightColor * ( NdotL * attenuation );
}

// Tangent-space normal-map support (ported from feat/pbr Toolbox.h). Z is ALWAYS reconstructed from XY, so BC5
// (2-channel) and BC1 (we ignore B, recompute it) both decode with one path. `p` = world position for the
// derivative-based TBN basis. If normal-mapped specular looks mirrored, flip the handedness comparison sign.
float3x3 CotangentFrame( float3 N, float3 p, float2 uv )
{
    float3 dp1 = ddx( p ), dp2 = ddy( p );
    float2 duv1 = ddx( uv ), duv2 = ddy( uv );
    float3 dp2perp = cross( dp2, N ), dp1perp = cross( N, dp1 );
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float handedness = ( duv1.x * duv2.y - duv1.y * duv2.x ) < 0.0 ? 1.0 : -1.0;
    T *= handedness;
    float invmax = rsqrt( max( dot( T, T ), dot( B, B ) ) );
    return float3x3( T * invmax, B * invmax, N );
}
float3 PerturbNormal( float3 N, float3 p, Texture2D nrmTex, float2 uv, SamplerState samp )
{
    float2 nxy = nrmTex.Sample( samp, uv ).xy * 2.0 - 1.0;
    float  nz  = sqrt( saturate( 1.0 - dot( nxy, nxy ) ) );   // reconstruct Z (BC5/BC1)
    float3 nrm = normalize( float3( nxy, nz ) );
    return normalize( mul( nrm, CotangentFrame( N, p, uv ) ) );
}

// PBR sun lighting (stage 2 — ports feat/pbr FP_ComputeSunLighting): ambient (sky/GI) + direct Cook-Torrance
// (diffuse + specular). vertLighting = Gothic's baked vertex-light scalar, used as an AO modulator. roughness/
// metallic/ao come from the ORM map (or its 1x1 default). ssao still 1 (no SSAO pass yet). shadow = 1 lit / 0 occ.
float3 ComputeSunLightingPBR( float3 wpos, float3 N, float3 albedo, float vertLighting, float shadow,
                              float roughness, float metallic, float ao )
{
    float3 V = normalize( CamPosWS - wpos );
    float3 L = SunDirWS;                            // dir toward the sun (world space)
    float3 sunCol = SrgbToLinear( SunColor );
    float  sunLum = dot( sunCol, float3( 0.3333, 0.3333, 0.3333 ) );
    const float ssao = 1.0;
    float sun      = saturate( dot( L, N ) * shadow );
    float shadowAO = lerp( 1.0, vertLighting, ShadowAOStrength ) * ao;
    float worldAO  = lerp( 1.0, vertLighting, WorldAOStrength ) * ao;
    float sunAtten = sun * worldAO * SunIntensity;
    float3 directSun  = PBR_DirectLighting( albedo, sunCol, N, V, L, roughness, metallic, sunAtten );
    float3 ambientSun = albedo * AmbientStrength * sunLum * shadowAO * ssao;   // ambient/sky term
    return ambientSun + directSun;
}

// Tiled point lights via the Forward+ grid, now with the full Cook-Torrance BRDF. `albedo` is LINEAR (sRGB-decoded
// in the PS). Ports D3D11 feat/pbr FP_ComputePointLighting (PLS_ComputePointLightLightingPBR).
float3 AccumTiledPointLights( float2 svpos, float3 wpos, float3 N, float3 albedo, float roughness, float metallic )
{
    uint2 tile = uint2( svpos ) / TILE_SIZE;
    uint  tileIndex = tile.y * NumTilesX + tile.x;
    LightGrid g = LightGridBuf[tileIndex];
    uint n = min( g.Count, MAX_LIGHTS_PER_TILE );
    float3 V = normalize( CamPosWS - wpos );
    float3 total = 0;
    for ( uint k = 0; k < n; k++ )
    {
        GPULight L = Lights[ LightIndexBuf[g.Offset + k] ];
        float3 dir = L.PositionWorld - wpos;
        float dist = length( dir );
        if ( dist >= L.Range ) continue;
        dir /= dist;
        float nd  = saturate( 1.0 - dist / L.Range );
        float falloff = nd * ( nd * 0.2 + 0.8 );   // PLS_ComputeRangeFalloff
        float3 lit = PBR_DirectLighting( albedo, L.Color.rgb, N, V, dir, roughness, metallic, falloff );
        if ( L.ShadowCubeIndex >= 0 )
        {
            float sh = SamplePointShadow( L.ShadowCubeIndex, wpos, N, L.PositionWorld, L.Range );
            float camDist = length( L.PositionView );
            float fade    = saturate( ( camDist - L.Range * 6.0 ) / ( L.Range * 3.0 ) );
            lit *= lerp( sh, 1.0, fade );
        }
        total += lit;
    }
    return total;
}

struct VS_IN
{
    float3   pos     : POSITION;
    float2   nrm     : NORMAL;                  // packed object-space normal (R16G16_SNORM @12)
    float2   uv      : TEXCOORD0;
    float4x4 iworld  : INSTANCE_WORLD_MATRIX;   // 4 per-instance rows (semantic index 0..3)
    float4   icolor  : INSTANCE_COLOR;
};
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float fogDist : TEXCOORD2; float3 wpos : TEXCOORD3; float3 wnrm : TEXCOORD4; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    float3 worldPos = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = i.icolor;
    o.wpos = worldPos;
    // Object->world normal (rigid + ~uniform-scale VOB matrices; inverse-transpose not needed for MVP).
    o.wnrm = mul( DecodeOctNormal( i.nrm ), (float3x3)i.iworld );
    o.fogDist = length( worldPos - CamPosWS );
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    float3 N = normalize( i.wnrm );
    if ( MatNormalIndex != 0xffffffff )
    {
        Texture2D nrmTex = ResourceDescriptorHeap[MatNormalIndex];
        N = PerturbNormal( N, i.wpos, nrmTex, i.uv, smp );
    }
    Texture2D ormTex = ResourceDescriptorHeap[MatOrmIndex];
    float3 orm = ormTex.Sample( smp, i.uv ).rgb;   // r=AO g=roughness b=metallic
    float3 albedo = SrgbToLinear( t.rgb );
    float vertLighting = i.col.g;          // per-instance ground light (green channel) as the AO modulator
    float shadow = ComputeSunShadow( i.wpos, N );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, vertLighting, shadow, orm.g, orm.b, orm.r );
    rgb += AccumTiledPointLights( i.clip.xy, i.wpos, N, albedo, orm.g, orm.b );
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, SrgbToLinear( FogColor ), f ), 1.0 );
}

// --- Depth-prepass variant (P2.9b-4a: adds instanced VOBs to the Forward+ opaque depth prepass) ---
// Minimal: transform by the instance world matrix and carry uv for the same alpha cutout; write depth only
// (the PSO masks color). Reads only b0 (ViewProj) + t0/s0 — NOT fog/light CBs — so it needs no BindFrameLights.
struct VS_DEPTH_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; float4x4 iworld : INSTANCE_WORLD_MATRIX; };
struct VS_DEPTH_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };
VS_DEPTH_OUT VSDepth( VS_DEPTH_IN i )
{
    VS_DEPTH_OUT o;
    float3 worldPos = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv = i.uv;
    return o;
}
float4 PSDepthClip( VS_DEPTH_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );          // same cutout as PSMain so foliage/fence gaps don't lay down depth
    return float4( 0, 0, 0, 1 );   // discarded: the PSO's color write mask is 0 (depth-only pass)
}
// Shadow caster (P2.9c-2): void PS so the depth-only shadow PSO binds NO render target without a validation
// warning; only alpha-clips the cutout so foliage/fence gaps don't cast solid shadows.
void PSShadowClip( VS_DEPTH_OUT i )
{
    clip( tx.Sample( smp, i.uv ).a - 0.5 );
}
)";

    // Forward+ opaque DEPTH PREPASS shader (P2.9b-1). Lays down the opaque world-mesh depth before the
    // lit color passes so the later tiled light-culling compute (P2.9b-2) has a per-pixel depth to tighten
    // each tile's frustum. Writes DEPTH ONLY — the PSO sets the color write mask to 0, so the float4 the PS
    // returns is discarded (it exists solely to run the alpha-test clip). The clip cutoff matches the opaque
    // world PS's clip( t.a - 0.5 ) exactly, so cutout foliage/fence gaps do NOT write depth (otherwise the
    // main pass would see background occluded through the gaps). Reuses m_WorldRootSig: only b0 (ViewProj)
    // and t0/s0 are referenced — fog/light params are NOT bound (no light loop here, so no hang risk).
    constexpr char kDepthPrepassShaderSource[] = R"(
cbuffer WorldCB : register(b0) { float4x4 ViewProj; };   // default column-major packing (see world shader)
Texture2D    tx  : register(t0);                          // CSM shadow caster (PSShadowClip) still binds diffuse here
SamplerState smp : register(s0);
// b6 material indices (ExecuteIndirect, P2.11): the world depth prepass (PSClip) samples its diffuse bindless so
// it can be driven by ExecuteIndirect like the color pass. Only MatDiffuseIndex (DWORD 2) is read here; the
// normal/ORM slots share the layout with the world color shader's MaterialCB so ONE command signature drives both.
cbuffer MaterialCB : register(b6) { uint _matN; uint _matO; uint MatDiffuseIndex; };

// World mesh: single stream, packed 36-byte ExVertexStructGPU. Only Position (@0) + TexCoord0 (@20) are
// fetched here; the normal/tangent/uv2/color fields are not needed for a depth+alpha-clip pass.
struct VS_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };

VS_OUT VSWorld( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );   // world verts are already world-space (identity world)
    o.uv   = i.uv;
    return o;
}

float4 PSClip( VS_OUT i ) : SV_TARGET
{
    Texture2D difTex = ResourceDescriptorHeap[MatDiffuseIndex];   // bindless diffuse (ExecuteIndirect, P2.11)
    float4 t = difTex.Sample( smp, i.uv );
    clip( t.a - 0.5 );          // same cutout as the opaque world PS so gaps don't lay down depth
    return float4( 0, 0, 0, 1 );   // discarded: the PSO's color write mask is 0 (depth-only pass)
}

// Shadow caster (P2.9c): void PS (no SV_Target) so the depth-only shadow PSO binds NO render target without a
// validation warning. Only alpha-clips foliage/fence cutouts so their gaps don't cast solid shadows.
void PSShadowClip( VS_OUT i )
{
    clip( tx.Sample( smp, i.uv ).a - 0.5 );
}
)";

    // Forward+ tiled light-culling COMPUTE shader (P2.9b-2). One thread group per 16x16 screen tile; each
    // group builds its tile's view-space AABB from the prepass depth and records which point lights touch it
    // into a per-tile slice of RW_LightIndexList (fixed MAX_LIGHTS_PER_TILE stride — no global atomic counter,
    // so no counter buffer/clear), with the per-tile {Offset,Count} landing in RW_LightGrid. This is the D3D11
    // reference Shaders/CS_LightCulling.hlsl (min/max-depth AABB + SphereInsideAABB) with two divergences:
    //   1. Reversed-Z + world-only prepass. The reference's ScreenToView z-inverse is written for finite
    //      standard-Z; we feed it the ACTUAL projection z-row terms (viewZ = Proj._43/(depth-Proj._33)), which
    //      is correct for our reversed-Z infinite-far camera. And because our depth prepass lays down WORLD
    //      MESH ONLY, an "empty" tile (no world depth — sky, or a VOB/NPC in front of nothing) falls back to an
    //      all-encompassing AABB (stay conservatively lit) rather than the reference's collapse-to-near-plane,
    //      which would unlight characters against the sky. The bounded near+far AABB is the whole point: it
    //      pulls in only lights whose sphere actually reaches the tile's geometry, so a wall at 15m no longer
    //      overflows the 32-light cap the way the earlier camera-origin cone (near fixed at 1) did. NOTE prior
    //      retired versions here: an unbounded [1,100000] AABB (far corners blew up off-axis), then side-planes
    //      with a fixed [1,100000] slab and then a far-only slab — all still over-included and overflowed.
    //      Residual: a short-range light on a VOB/NPC that pokes in front of distant world geometry and can't
    //      reach that world can still be culled; completing the prepass (VOB+skeletal depth) removes it.
    //   2. Fixed per-tile index slice instead of a compacted global list, dropping RW_IndexCounter and its
    //      per-frame clear. At 1080p this is ~1 MB (8160 tiles * 32 * 4 B) — negligible, and simpler/safer.
    // PositionView is filled CPU-side in BuildFrameLightBuffer using the same transpose(view) transform the
    // D3D11 CullLights uses, so this shader's view space matches. SM6.6: no ternary (use min()/select()).
    constexpr char kLightCullShaderSource[] = R"(
#define TILE_SIZE 16
#define MAX_LIGHTS_PER_TILE 32

struct TiledPointLight { float3 PositionView; float Range; float4 Color; float3 PositionWorld; int ShadowCubeIndex; };
struct LightGrid { uint Offset; uint Count; };

StructuredBuffer<TiledPointLight> SB_Lights       : register(t0);
RWStructuredBuffer<LightGrid>     RW_LightGrid     : register(u0);
RWStructuredBuffer<uint>          RW_LightIndexList : register(u1);
Texture2D<float>                  DepthTex          : register(t1);   // prepass depth (reversed-Z); per-tile far-Z

cbuffer CullCB : register(b0) {
    float2 ProjScale;    // (Proj._11, Proj._22): view->clip x/y scale (diagonal terms, layout-invariant)
    uint2  ScreenDim;    // render-target pixel size
    uint   TotalLights;  // valid light count in SB_Lights (<= light buffer capacity)
    uint   NumTilesX;    // ceil(ScreenDim.x / TILE_SIZE)
    float2 ProjZ;        // (Proj._33, Proj._43): reversed-Z depth->viewZ  (viewZ = ProjZ.y / (depth - ProjZ.x))
};

groupshared uint gs_Count;
groupshared uint gs_Indices[MAX_LIGHTS_PER_TILE];
groupshared uint gs_MinDepthInt;   // nearest/farthest opaque depth in the tile, as sortable float bits
groupshared uint gs_MaxDepthInt;

// View-space position of a screen pixel at a given reversed-Z depth. The four packed projection scalars
// (ProjScale = _11/_22, ProjZ = _33/_43) are exactly the terms D3D11's ScreenToView uses; the z inverse is
// valid for our reversed-Z infinite-far camera because they are the actual matrix entries. ndc.y flipped.
float3 ScreenToView( float2 pixel, float depth ) {
    float2 ndc;
    ndc.x = pixel.x / (float)ScreenDim.x * 2.0 - 1.0;
    ndc.y = -(pixel.y / (float)ScreenDim.y * 2.0 - 1.0);
    float zView = ProjZ.y / ( depth - ProjZ.x );
    return float3( ndc.x / ProjScale.x * zView, ndc.y / ProjScale.y * zView, zView );
}

// Closest-point sphere/AABB overlap (view space). Keeps a light iff its sphere touches the box.
bool SphereInsideAABB( float3 center, float radius, float3 aabbMin, float3 aabbMax ) {
    float3 closest = clamp( center, aabbMin, aabbMax );
    float3 delta = closest - center;
    return dot( delta, delta ) <= radius * radius;
}

[numthreads( TILE_SIZE, TILE_SIZE, 1 )]
void CSMain( uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID ) {
    uint ti = threadID.y * TILE_SIZE + threadID.x;
    if ( ti == 0 ) { gs_Count = 0; gs_MinDepthInt = 0x7F7FFFFF; gs_MaxDepthInt = 0; }
    GroupMemoryBarrierWithGroupSync();

    // Per-tile depth bounds from the prepass. Each of the 256 threads owns exactly one tile pixel and folds
    // its reversed-Z depth into the group min/max (asuint keeps float ordering for depth in [0,1]). Sky /
    // cleared pixels read 0 and are skipped. This is the D3D11 CS_LightCulling min/max; the AABB it builds is
    // bounded on BOTH the near and far side by real geometry, so the tile only pulls in lights whose sphere
    // actually reaches the surfaces in it — the earlier camera-origin cone (near fixed at 1) instead counted
    // every light between the camera and the geometry, so a wall at 15m still overflowed the 32-light cap.
    {
        uint2 px = uint2( groupID.xy ) * TILE_SIZE + threadID.xy;
        if ( px.x < ScreenDim.x && px.y < ScreenDim.y ) {
            float d = DepthTex.Load( int3( int2( px ), 0 ) );
            if ( d > 0.0 ) {   // reversed-Z: 0 == cleared (sky / no opaque geometry)
                uint di = asuint( d );
                InterlockedMin( gs_MinDepthInt, di );
                InterlockedMax( gs_MaxDepthInt, di );
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Tile AABB in view space, spanning the near..far corners of the tile's screen rect at the min/max depth.
    float2 tileMin = float2( groupID.xy ) * TILE_SIZE;
    float2 tileMax = float2( groupID.xy + uint2( 1, 1 ) ) * TILE_SIZE;
    float3 aabbMin, aabbMax;
    if ( gs_MinDepthInt == 0x7F7FFFFF ) {
        // No world-mesh depth in this tile (sky, or a VOB/NPC the world-only prepass didn't lay down). Fall
        // back to an all-encompassing box so those tiles stay conservatively lit — DELIBERATELY divergent from
        // D3D11 (which collapses empty tiles to the near plane), because our prepass is world-mesh only and
        // collapsing here would unlight characters standing against the sky. (Complete the prepass to remove.)
        aabbMin = float3( -1e9, -1e9, -1e9 );
        aabbMax = float3(  1e9,  1e9,  1e9 );
    } else {
        float minDepth = asfloat( gs_MinDepthInt );   // reversed-Z: smaller depth == farther
        float maxDepth = asfloat( gs_MaxDepthInt );
        float3 c0 = ScreenToView( float2( tileMin.x, tileMin.y ), minDepth );
        float3 c1 = ScreenToView( float2( tileMax.x, tileMin.y ), minDepth );
        float3 c2 = ScreenToView( float2( tileMin.x, tileMax.y ), minDepth );
        float3 c3 = ScreenToView( float2( tileMax.x, tileMax.y ), minDepth );
        float3 c4 = ScreenToView( float2( tileMin.x, tileMin.y ), maxDepth );
        float3 c5 = ScreenToView( float2( tileMax.x, tileMin.y ), maxDepth );
        float3 c6 = ScreenToView( float2( tileMin.x, tileMax.y ), maxDepth );
        float3 c7 = ScreenToView( float2( tileMax.x, tileMax.y ), maxDepth );
        aabbMin = min( min( min( c0, c1 ), min( c2, c3 ) ), min( min( c4, c5 ), min( c6, c7 ) ) );
        aabbMax = max( max( max( c0, c1 ), max( c2, c3 ) ), max( max( c4, c5 ), max( c6, c7 ) ) );
    }

    // Distribute the light test across the group's threads. Clamp the external count (root constant) to the
    // buffer capacity so a garbage TotalLights can never spin the loop (lasting D3D12 rule).
    uint total = min( TotalLights, 4096u );
    uint numThreads = TILE_SIZE * TILE_SIZE;
    for ( uint i = ti; i < total; i += numThreads ) {
        TiledPointLight L = SB_Lights[i];
        if ( SphereInsideAABB( L.PositionView, L.Range * 1.05, aabbMin, aabbMax ) ) {
            uint idx;
            InterlockedAdd( gs_Count, 1, idx );
            if ( idx < MAX_LIGHTS_PER_TILE ) gs_Indices[idx] = i;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if ( ti == 0 ) {
        uint count = min( gs_Count, (uint)MAX_LIGHTS_PER_TILE );
        uint tileIndex = groupID.y * NumTilesX + groupID.x;
        uint offset = tileIndex * MAX_LIGHTS_PER_TILE;   // fixed per-tile slice (no global counter)
        RW_LightGrid[tileIndex].Offset = offset;
        RW_LightGrid[tileIndex].Count  = count;
        for ( uint j = 0; j < count; ++j )
            RW_LightIndexList[offset + j] = gs_Indices[j];
    }
}
)";

    // Phase-2 water shader (MVP). Same wrapped-world-mesh vertex as the opaque pass, but the packed
    // TexCoord2 (@28, half2) carries the per-material UV-scroll delta (set in WorldConverter for water
    // materials); the VS scrolls the diffuse UV by (delta * totalTime) exactly like VS_ExWater. The PS
    // samples the scrolled diffuse, applies distance fog, and outputs a constant alpha so the surface
    // blends translucently over the already-drawn opaque scene beneath it. Refraction / reflection /
    // scene-color + depth sampling + Gerstner waves (the full D3D11 PS_Water/VS_ExWater) are out of MVP.
    constexpr char kWaterShaderSource[] = R"(
cbuffer WorldCB : register(b0) { float4x4 ViewProj; };   // default column-major packing (see world shader)
cbuffer FogCB   : register(b1) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
cbuffer WaterCB : register(b2) { float TotalTime; float WaterAlpha; float2 _wpad; };

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

struct VS_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; float2 scroll : TEXCOORD1; float4 col : DIFFUSE; };
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float fogDist : TEXCOORD2; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
    float2 ani = i.scroll * TotalTime;   // scroll delta (TexCoord2) * total time (ms), like VS_ExWater
    ani -= floor( ani );                 // wrap to [0,1) so the float stays precise over long sessions
    o.uv = i.uv + ani;
    o.col = i.col;
    o.fogDist = length( i.pos - CamPosWS );
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    float3 rgb = pow( t.rgb, 2.2 ) * i.col.bgr;   // linearize (HDR buffer is linear; ~pow2.2 approximates sRGB)
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    rgb = lerp( rgb, pow( FogColor, 2.2 ), f );
    return float4( rgb, WaterAlpha );
}
)";

    // Water surfaces peeled out of the opaque world pass (DrawWorldMesh) and drawn later, alpha-blended,
    // by DrawWaterSurfaces. Both run on the same thread within one frame (OnStartWorldRendering), so a
    // file-scope scratch map is safe — grouped by texture to minimize SRV binds. Cleared each frame.
    std::unordered_map<zCTexture*, std::vector<MeshInfo*>> g_FrameWaterSurfaces;

    // Per-frame visible-vob/light/mob collection, hoisted out of DrawVobsInstanced so ALL geometry passes
    // (world, VOBs, skeletal) light against the same set. CollectVisibleVobs has side effects (fills each
    // visual's Instances list) and must run EXACTLY ONCE per frame. Single-threaded within OnStartWorldRendering.
    std::vector<VobInfo*>         g_FrameVobs;
    std::vector<VobLightInfo*>    g_FrameLights;
    std::vector<SkeletalVobInfo*> g_FrameMobs;

    // Point-light shadow selection (P2.10b/c): the shadowed lights chosen this frame (closest-in-range, capped
    // at kMaxShadowCubes) — filled by BuildFrameLightBuffer (which also writes ShadowCubeIndex into the GPU light
    // struct), consumed by RenderPointShadows to render each light's 6 cube faces.
    // Static/dynamic split (P2.10g): every winner gets its active cube = (static-aside copy) + dynamic overlay
    // each frame. renderStatic=true → also re-render the STATIC casters into the static-aside slot first (only
    // when the slot is fresh / the light moved / range changed); otherwise the cached static depth is reused.
    struct FramePointShadow { DirectX::XMFLOAT3 posWS; float range; UINT slot; bool renderStatic; };
    std::vector<FramePointShadow> g_FramePointShadows;

    // Per-frame VOB instance-ring snapshot (P2.9b-4a). UploadFrameVobInstances memcpys each visible visual's
    // instances into the ring ONCE (before the light cull), recording the resulting stream view + count here.
    // The depth prepass (DrawVobDepthPrepass) and the color pass (DrawVobsInstanced) then BOTH draw from these
    // records — no second upload, so the color pass's ring usage is unchanged. Rebuilt every frame.
    std::vector<FrameVobUpload> g_FrameVobUploads;

    // Per-frame skeletal shared-upload records (P2.9b-4b). PrepareFrameSkeletals runs the once-per-frame
    // animation update and uploads each vob's bone/instance CBs (base meshes) and its node attachments'
    // VOB-instance data (into the VOB ring) BEFORE the cull, recording GPU addresses here. Then
    // DrawSkeletalDepthPrepass (pre-cull, depth-only) and DrawSkeletalColor (post-cull, lit) both draw from
    // these — so the animation update is never run twice and nothing is uploaded twice. Rebuilt each frame.
    struct FrameSkelDraw   { SkeletalVobInfo* vobInfo;  SkeletalMeshVisualInfo* visual; D3D12_GPU_VIRTUAL_ADDRESS instCb; D3D12_GPU_VIRTUAL_ADDRESS boneCb; };
    struct FrameAttachDraw { MeshInfo* mesh; zCTexture* tex; D3D12_VERTEX_BUFFER_VIEW instView; };
    std::vector<FrameSkelDraw>   g_FrameSkelDraws;
    std::vector<FrameAttachDraw> g_FrameAttachDraws;

    // Forward+ MVP light buffer (P2.9a): the whole visible-light list is rebuilt from offset 0 each frame,
    // so the ring is just kBackBufferCount snapshots (no per-draw offset). Cap matches D3D11 MAX_TILED_LIGHTS.
    constexpr UINT kMaxFrameLights = 400;

    // Per-frame GPU point light — byte-identical to D3D11 TiledPointLight (48 B) so the layout is reusable
    // when the compute tiled-culling step (P2.9b) lands. Brute-force MVP fills PositionWorld/Range/Color only.
    struct GPULight {
        DirectX::XMFLOAT3 PositionView;    // 0  (filled with tiling; world-space shading for now)
        float             Range;           // 12
        DirectX::XMFLOAT4 Color;           // 16 (.w = static flag 0/1)
        DirectX::XMFLOAT3 PositionWorld;   // 32
        int32_t           ShadowCubeIndex; // 44 (-1 = no shadow)
    };
    static_assert( sizeof( GPULight ) == 48, "GPULight must match D3D11 TiledPointLight (48 bytes)" );

    constexpr UINT kSkeletalConstantBufferBytes = 8 * 1024 * 1024; // per-frame skeletal CB ring (instance + bone palettes)
    constexpr UINT kSkeletalMaxBones = 96;                         // NUM_MAX_BONES — matches every skeletal HLSL

    // Per-instance skeletal constant buffer (register b1). A minimal subset of the D3D11
    // VS_ExConstantBuffer_PerInstanceSkeletal — just what the first-light skeletal shader reads
    // (world matrix + model color + fatness). PrevWorld / motion vectors are a later step.
    struct SkeletalInstanceCB {
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT4   ModelColor;
        float               Fatness;
        float               Pad[3];
    };
    static_assert( sizeof( SkeletalInstanceCB ) == 96, "SkeletalInstanceCB must stay 16-byte-aligned" );

    // Phase-2 skeletal (animated) mesh shader — NPCs, monsters, animated MOBs. Matrix-palette skinning:
    // each vertex stores its position baked into up to 4 influencing bones' local spaces (half4 each),
    // plus per-influence bone index + weight, so skinnedPos = sum_i weight_i * mul(pos_i, Bones[idx_i]).
    // Mirrors VS_ExSkeletal.hlsl's ApplySkinningCurrent core (minus motion vectors / view-space normal /
    // prev-frame bones). b0 = ViewProj (root consts), b1 = per-instance (world + color + fatness), b2 =
    // bone-matrix palette (<=96). Default column-major packing: matrices are uploaded as row-major
    // XMFLOAT4X4 and read the same way the D3D11 skeletal VS does, so mul() is byte-for-byte identical.
    constexpr char kSkeletalShaderSource[] = R"(
cbuffer FrameCB    : register(b0) { float4x4 ViewProj; };
cbuffer InstanceCB : register(b1) { float4x4 M_World; float4 ModelColor; float Fatness; float3 _pad; };
cbuffer BonesCB    : register(b2) { float4x4 Bones[96]; };
cbuffer FogCB      : register(b3) { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; };
cbuffer LightCB    : register(b4) { uint LightCount; uint NumTilesX; uint2 _lpad; };   // Forward+ tiled: light count + tiles/row

// Forward+ tiled point lights (root-descriptor SRVs + per-tile grid) — see the world shader for the rationale.
struct GPULight { float3 PositionView; float Range; float4 Color; float3 PositionWorld; int ShadowCubeIndex; };
struct LightGrid { uint Offset; uint Count; };
StructuredBuffer<GPULight>  Lights        : register(t1);
StructuredBuffer<LightGrid> LightGridBuf  : register(t2);
StructuredBuffer<uint>      LightIndexBuf : register(t3);
#define TILE_SIZE 16u
#define MAX_LIGHTS_PER_TILE 32u

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

// CSM sun-shadow sampling (P2.9c-4b). Skeletal already uses b3 (fog) + b4 (light count), so the shadow CB
// lands at b5 here (world/VOB use b3); t4/s2 are free. Same select+PCF math as the world/VOB block.
#define NUM_CSM_CASCADES 3
cbuffer ShadowCB : register(b5)
{
    float4x4 CascadeViewProj[NUM_CSM_CASCADES];
    float3   SunDirWS;          float ShadowMapSize;
    float3   SunColor;          float SunIntensity;
    float3   CascadeTexelWorld; float AmbientStrength;
    float    ShadowAOStrength;  float WorldAOStrength;  float2 _shpad;
};
Texture2DArray          ShadowMap : register(t4);
SamplerComparisonState  shadowCmp : register(s2);
// Per-material bindless indices (root consts b6): SM6.6 ResourceDescriptorHeap[...] indices for this material's
// normal + ORM maps. MatNormalIndex == 0xFFFFFFFF -> no normal map (skip perturb); MatOrmIndex is always valid
// (the 1x1 default ORM = AO 1 / rough 0.5 / metal 0 when the material has no _FX map), so ORM is sampled branchlessly.
cbuffer MaterialCB : register(b6) { uint MatNormalIndex; uint MatOrmIndex; };
TextureCubeArray        PointShadowCubes : register(t5);   // point-light shadow cubes (P2.10d), R16 linear depth

// Point-light shadow: returns 1 = lit, 0 = occluded. The cube stores the NATURAL hyperbolic z of the caster's
// 90-deg PerspectiveFovLH(near 15, far range*2). Reconstruct the same z from the fragment: the depth on a cube
// face is driven by the DOMINANT-AXIS distance (the face's view-space z), so zView = max(|dx|,|dy|,|dz|), then
// apply the LH projection z-map. Most acne bias is the PSO's hardware slope bias; add a small normal offset +
// constant. 4-tap rotated-disk PCF softens the edges; a camera-distance fade is applied at the call site.
float SamplePointShadow( int cubeIndex, float3 wpos, float3 N, float3 lightPos, float range )
{
    float3 d  = ( wpos + N * ( range * 0.01 ) ) - lightPos;   // normal-offset bias (world-space, uniform)
    float3 ad = abs( d );
    float  zView = max( ad.x, max( ad.y, ad.z ) );            // dominant cube-axis depth = the face's view-space z
    const float n = 15.0;
    float  f = range * 2.0;
    float  compareDepth = ( f / ( f - n ) ) * ( 1.0 - n / zView ) - 0.001;   // same LH hyperbolic z the caster wrote
    float3 L = normalize( d );

    // P2.10e polish: 4-tap rotated-disk PCF on a basis perpendicular to L (cube sampling follows the offset dir,
    // so a small angular offset lands on neighbouring texels). Softens the previously single-tap hard edges. The
    // offset grows a little with distance so the world-space penumbra stays roughly constant across the range.
    float3 up = abs( L.y ) < 0.99 ? float3( 0, 1, 0 ) : float3( 1, 0, 0 );
    float3 t  = normalize( cross( up, L ) );
    float3 bt = cross( L, t );
    float  r  = 0.006 + 0.010 * saturate( zView / f );
    static const float2 kDisk[4] = { float2( 0.7, 0.7 ), float2( -0.7, 0.7 ), float2( 0.7, -0.7 ), float2( -0.7, -0.7 ) };
    float sh = 0.0;
    [unroll]
    for ( int s = 0; s < 4; ++s )
    {
        float3 o = normalize( L + ( kDisk[s].x * t + kDisk[s].y * bt ) * r );
        sh += PointShadowCubes.SampleCmpLevelZero( shadowCmp, float4( o, (float)cubeIndex ), compareDepth );
    }
    return sh * 0.25;
}

float ComputeSunShadow( float3 wpos, float3 N )
{
    const float margin = 1.5 / ShadowMapSize;
    const float texel  = 1.0 / ShadowMapSize;
    [unroll]
    for ( int c = 0; c < NUM_CSM_CASCADES; ++c )
    {
        float3 biased = wpos + N * ( CascadeTexelWorld[c] * 1.5 );
        float4 sp = mul( float4( biased, 1.0 ), CascadeViewProj[c] );
        float2 uv = sp.xy * float2( 0.5, -0.5 ) + 0.5;
        if ( uv.x > margin && uv.x < 1.0 - margin && uv.y > margin && uv.y < 1.0 - margin &&
             sp.z >= 0.0 && sp.z <= 1.0 )
        {
            // Wider, cascade-scaled PCF (P2.9c-3c): far cascades cover more world per texel (sub-texel foliage
            // → temporal "blinking"), so widen the kernel step with the cascade index to spatially average that
            // flicker into a soft, stable penumbra. 5x5 taps; near cascade stays near-1-texel (crisp).
            float pcfStep = texel * ( 1.0 + float( c ) * 1.5 );
            float sh = 0.0;
            [unroll] for ( int y = -2; y <= 2; ++y )
            [unroll] for ( int x = -2; x <= 2; ++x )
                sh += ShadowMap.SampleCmpLevelZero( shadowCmp, float3( uv + float2( x, y ) * pcfStep, c ), sp.z - 0.0015 );
            return sh / 25.0;
        }
    }
    return 1.0;
}

// Accumulate dynamic point lights via the Forward+ tile grid (identical math to the world/VOB shaders:
// range cull, N.L, falloff = nd*(nd*0.2+0.8), per-light saturate, additive; specular/shadows are later).
// --- Cook-Torrance GGX PBR (ported verbatim from the D3D11 feat/pbr branch: Shaders/include/PointLightShadows.h) ---
// Staged PBR (P3-PBR-1): albedo is sRGB-decoded to LINEAR in the PS; Gothic's baked vertex lighting is kept as the
// diffuse/ambient base; the SUN adds a specular-only glint and the tiled POINT lights use the full BRDF. Material
// params are constant defaults for now (no ORM/normal maps yet — that's a later increment with texture-loading infra).
static const float PBR_PI = 3.14159265;
static const float MI_Roughness = 0.9;   // default perceptual roughness (Gothic surfaces are mostly rough dielectrics)
static const float MI_Metallic  = 0.0;   // default metallic (dielectric)
static const float SunSpecIntensity = 1.0;

float3 SrgbToLinear( float3 c )   // accurate sRGB EOTF — linearize gamma-encoded albedo so lighting is done in linear space
{
    return select( c <= 0.04045, c / 12.92, pow( ( c + 0.055 ) / 1.055, 2.4 ) );
}

float  PBR_SafeRoughness( float r ) { return max( saturate( r ), 0.045 ); }
float  PBR_DistributionGGX( float NdotH, float roughness )
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * ( a2 - 1.0 ) + 1.0;
    return a2 / max( PBR_PI * denom * denom, 1e-4 );
}
float  PBR_GeometrySchlickGGX( float NdotX, float roughness )
{
    float r = roughness + 1.0;
    float k = ( r * r ) / 8.0;
    return NdotX / max( NdotX * ( 1.0 - k ) + k, 1e-4 );
}
float  PBR_GeometrySmith( float NdotV, float NdotL, float roughness )
{
    return PBR_GeometrySchlickGGX( NdotV, roughness ) * PBR_GeometrySchlickGGX( NdotL, roughness );
}
float  PBR_Pow5( float x ) { float x2 = x * x; return x2 * x2 * x; }
float3 PBR_FresnelSchlick( float cosTheta, float3 F0 ) { return F0 + ( 1.0 - F0 ) * PBR_Pow5( saturate( 1.0 - cosTheta ) ); }

// Full Cook-Torrance (energy-conserving diffuse + specular). attenuation folds in falloff/shadow; NdotL applied here.
float3 PBR_DirectLighting( float3 baseColor, float3 lightColor, float3 N, float3 V, float3 L,
                           float roughness, float metallic, float attenuation )
{
    float NdotL = saturate( dot( N, L ) );
    float NdotV = saturate( dot( N, V ) );
    if ( NdotL <= 0.0 || NdotV <= 0.0 || attenuation <= 0.0 ) return 0.0;
    float3 H = normalize( V + L );
    float NdotH = saturate( dot( N, H ) );
    float VdotH = saturate( dot( V, H ) );
    float  cr = PBR_SafeRoughness( roughness * roughness );   // perceptual->physical (the branch squares here)
    float  cm = saturate( metallic );
    float3 F0 = lerp( float3( 0.04, 0.04, 0.04 ), baseColor, cm );
    float  D = PBR_DistributionGGX( NdotH, cr );
    float  G = PBR_GeometrySmith( NdotV, NdotL, cr );
    float3 F = PBR_FresnelSchlick( VdotH, F0 );
    float3 specular = ( D * G * F ) / max( 4.0 * NdotV * NdotL, 1e-4 );
    float3 kD = ( 1.0 - F ) * ( 1.0 - cm );
    float3 diffuse = kD * baseColor / PBR_PI;
    return ( diffuse + specular ) * lightColor * ( NdotL * attenuation );
}

// Specular-only variant — used for the SUN so we don't double-count Gothic's baked diffuse sun.
float3 PBR_DirectSpecularOnly( float3 baseColor, float3 lightColor, float3 N, float3 V, float3 L,
                               float roughness, float metallic, float attenuation )
{
    float NdotL = saturate( dot( N, L ) );
    float NdotV = saturate( dot( N, V ) );
    if ( NdotL <= 0.0 || NdotV <= 0.0 || attenuation <= 0.0 ) return 0.0;
    float3 H = normalize( V + L );
    float NdotH = saturate( dot( N, H ) );
    float VdotH = saturate( dot( V, H ) );
    float  cr = PBR_SafeRoughness( roughness * roughness );
    float  cm = saturate( metallic );
    float3 F0 = lerp( float3( 0.04, 0.04, 0.04 ), baseColor, cm );
    float  D = PBR_DistributionGGX( NdotH, cr );
    float  G = PBR_GeometrySmith( NdotV, NdotL, cr );
    float3 F = PBR_FresnelSchlick( VdotH, F0 );
    float3 specular = ( D * G * F ) / max( 4.0 * NdotV * NdotL, 1e-4 );
    return specular * lightColor * ( NdotL * attenuation );
}

// Tangent-space normal-map support (ported from feat/pbr Toolbox.h). Z is ALWAYS reconstructed from XY, so BC5
// (2-channel) and BC1 (we ignore B, recompute it) both decode with one path. `p` = world position for the
// derivative-based TBN basis. If normal-mapped specular looks mirrored, flip the handedness comparison sign.
float3x3 CotangentFrame( float3 N, float3 p, float2 uv )
{
    float3 dp1 = ddx( p ), dp2 = ddy( p );
    float2 duv1 = ddx( uv ), duv2 = ddy( uv );
    float3 dp2perp = cross( dp2, N ), dp1perp = cross( N, dp1 );
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float handedness = ( duv1.x * duv2.y - duv1.y * duv2.x ) < 0.0 ? 1.0 : -1.0;
    T *= handedness;
    float invmax = rsqrt( max( dot( T, T ), dot( B, B ) ) );
    return float3x3( T * invmax, B * invmax, N );
}
float3 PerturbNormal( float3 N, float3 p, Texture2D nrmTex, float2 uv, SamplerState samp )
{
    float2 nxy = nrmTex.Sample( samp, uv ).xy * 2.0 - 1.0;
    float  nz  = sqrt( saturate( 1.0 - dot( nxy, nxy ) ) );   // reconstruct Z (BC5/BC1)
    float3 nrm = normalize( float3( nxy, nz ) );
    return normalize( mul( nrm, CotangentFrame( N, p, uv ) ) );
}

// PBR sun lighting (stage 2 — ports feat/pbr FP_ComputeSunLighting): ambient (sky/GI) + direct Cook-Torrance
// (diffuse + specular). vertLighting = Gothic's baked vertex-light scalar, used as an AO modulator. roughness/
// metallic/ao come from the ORM map (or its 1x1 default). ssao still 1 (no SSAO pass yet). shadow = 1 lit / 0 occ.
float3 ComputeSunLightingPBR( float3 wpos, float3 N, float3 albedo, float vertLighting, float shadow,
                              float roughness, float metallic, float ao )
{
    float3 V = normalize( CamPosWS - wpos );
    float3 L = SunDirWS;                            // dir toward the sun (world space)
    float3 sunCol = SrgbToLinear( SunColor );
    float  sunLum = dot( sunCol, float3( 0.3333, 0.3333, 0.3333 ) );
    const float ssao = 1.0;
    float sun      = saturate( dot( L, N ) * shadow );
    float shadowAO = lerp( 1.0, vertLighting, ShadowAOStrength ) * ao;
    float worldAO  = lerp( 1.0, vertLighting, WorldAOStrength ) * ao;
    float sunAtten = sun * worldAO * SunIntensity;
    float3 directSun  = PBR_DirectLighting( albedo, sunCol, N, V, L, roughness, metallic, sunAtten );
    float3 ambientSun = albedo * AmbientStrength * sunLum * shadowAO * ssao;   // ambient/sky term
    return ambientSun + directSun;
}

// Tiled point lights via the Forward+ grid, now with the full Cook-Torrance BRDF. `albedo` is LINEAR (sRGB-decoded
// in the PS). Ports D3D11 feat/pbr FP_ComputePointLighting (PLS_ComputePointLightLightingPBR).
float3 AccumTiledPointLights( float2 svpos, float3 wpos, float3 N, float3 albedo, float roughness, float metallic )
{
    uint2 tile = uint2( svpos ) / TILE_SIZE;
    uint  tileIndex = tile.y * NumTilesX + tile.x;
    LightGrid g = LightGridBuf[tileIndex];
    uint n = min( g.Count, MAX_LIGHTS_PER_TILE );
    float3 V = normalize( CamPosWS - wpos );
    float3 total = 0;
    for ( uint k = 0; k < n; k++ )
    {
        GPULight L = Lights[ LightIndexBuf[g.Offset + k] ];
        float3 dir = L.PositionWorld - wpos;
        float dist = length( dir );
        if ( dist >= L.Range ) continue;
        dir /= dist;
        float nd  = saturate( 1.0 - dist / L.Range );
        float falloff = nd * ( nd * 0.2 + 0.8 );   // PLS_ComputeRangeFalloff
        float3 lit = PBR_DirectLighting( albedo, L.Color.rgb, N, V, dir, roughness, metallic, falloff );
        if ( L.ShadowCubeIndex >= 0 )
        {
            float sh = SamplePointShadow( L.ShadowCubeIndex, wpos, N, L.PositionWorld, L.Range );
            float camDist = length( L.PositionView );
            float fade    = saturate( ( camDist - L.Range * 6.0 ) / ( L.Range * 3.0 ) );
            lit *= lerp( sh, 1.0, fade );
        }
        total += lit;
    }
    return total;
}

struct VS_IN
{
    float4 pos[4]         : POSITION;    // 4 per-bone-space positions (half4)
    float3 normal         : NORMAL;
    float3 bindPoseNormal : TEXCOORD0;   // unused (view-space normal is a later step)
    float2 uv             : TEXCOORD1;
    uint4  boneIndices    : BONEIDS;
    float4 weights        : WEIGHTS;
};
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; float fogDist : TEXCOORD2; float3 wpos : TEXCOORD3; float3 wnrm : TEXCOORD4; };

VS_OUT VSMain( VS_IN i )
{
    float3 skinnedPos    = float3( 0, 0, 0 );
    float3 skinnedNormal = float3( 0, 0, 0 );
    [unroll]
    for ( int b = 0; b < 4; ++b )
    {
        float4x4 bone = Bones[i.boneIndices[b]];
        float    w    = i.weights[b];
        skinnedPos    += w * mul( float4( i.pos[b].xyz, 1.0 ), bone ).xyz;
        skinnedNormal += w * mul( i.normal, (float3x3)bone );
    }
    float3 worldPos = mul( float4( skinnedPos + Fatness * skinnedNormal, 1.0 ), M_World ).xyz;

    VS_OUT o;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = ModelColor;
    o.wpos = worldPos;
    // skinnedNormal is in model space (bone-rotated); rotate into world by M_World (rigid + ~uniform scale).
    o.wnrm = mul( skinnedNormal, (float3x3)M_World );
    o.fogDist = length( worldPos - CamPosWS );
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    float3 N = normalize( i.wnrm );
    if ( MatNormalIndex != 0xffffffff )
    {
        Texture2D nrmTex = ResourceDescriptorHeap[MatNormalIndex];
        N = PerturbNormal( N, i.wpos, nrmTex, i.uv, smp );
    }
    Texture2D ormTex = ResourceDescriptorHeap[MatOrmIndex];
    float3 orm = ormTex.Sample( smp, i.uv ).rgb;   // r=AO g=roughness b=metallic
    float3 albedo = SrgbToLinear( t.rgb );
    float vertLighting = i.col.g;               // ModelColor green (white=1 for NPCs → no baked AO reduction)
    float shadow = ComputeSunShadow( i.wpos, N );
    float3 rgb = ComputeSunLightingPBR( i.wpos, N, albedo, vertLighting, shadow, orm.g, orm.b, orm.r );
    rgb += AccumTiledPointLights( i.clip.xy, i.wpos, N, albedo, orm.g, orm.b );   // dynamic point lights on top (PBR)
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, SrgbToLinear( FogColor ), f ), 1.0 );
}

// --- Depth-prepass variant (P2.9b-4b: adds skinned NPC/monster meshes to the Forward+ opaque depth prepass) ---
// Same matrix-palette skinning as VSMain (so the depth matches the color pass bit-for-bit) but outputs only
// clip + uv; reads b0/b1/b2 + t0/s0, NOT fog/light CBs — so it needs no BindFrameLights (no light-loop hang).
struct VS_DEPTH_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };
VS_DEPTH_OUT VSDepth( VS_IN i )
{
    float3 skinnedPos    = float3( 0, 0, 0 );
    float3 skinnedNormal = float3( 0, 0, 0 );
    [unroll]
    for ( int b = 0; b < 4; ++b )
    {
        float4x4 bone = Bones[i.boneIndices[b]];
        float    w    = i.weights[b];
        skinnedPos    += w * mul( float4( i.pos[b].xyz, 1.0 ), bone ).xyz;
        skinnedNormal += w * mul( i.normal, (float3x3)bone );
    }
    float3 worldPos = mul( float4( skinnedPos + Fatness * skinnedNormal, 1.0 ), M_World ).xyz;
    VS_DEPTH_OUT o;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv = i.uv;
    return o;
}
float4 PSDepthClip( VS_DEPTH_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );          // same cutout as PSMain so alpha edges don't lay down depth
    return float4( 0, 0, 0, 1 );   // discarded: the PSO's color write mask is 0 (depth-only pass)
}
// Shadow caster (P2.9c-2): void PS so the depth-only shadow PSO binds NO render target without a validation
// warning; only alpha-clips the cutout so alpha edges don't cast solid shadows.
void PSShadowClip( VS_DEPTH_OUT i )
{
    clip( tx.Sample( smp, i.uv ).a - 0.5 );
}
)";

    // Phase-2 particle (PFX) shader — instanced camera-facing billboards. One instance per live particle
    // (ParticleInstanceInfo, 56B, all PER_INSTANCE); the VS expands a 4-vertex triangle strip from
    // SV_VertexID, doing all billboard orientation itself (mirrors VS_ParticlePoint.hlsl). `type` encodes
    // the alignment: >=10 => quad-poly (half size); the low digit selects camera / y-locked / plane /
    // velocity-aligned. DIFFUSE is a full float4 here (unlike the packed DWORD paths) so no swizzle. b0 =
    // ViewProj (root consts, default column-major packing, same as the world shader); b1 = camera world pos.
    constexpr char kParticleShaderSource[] = R"(
cbuffer FrameCB    : register(b0) { float4x4 ViewProj; };
cbuffer ParticleCB : register(b1) { float3 CameraPosition; float _ppad; };

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

struct VS_IN {
    uint   vertexID : SV_VertexID;
    float3 pos      : POSITION;
    float4 dif      : DIFFUSE;
    float3 size     : SIZE;
    uint   type     : TYPE;
    float3 vel      : VELOCITY;
};
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 dif : TEXCOORD1; };

static const float tu[4] = { 0.0, 1.0, 0.0, 1.0 };
static const float tv[4] = { 1.0, 1.0, 0.0, 0.0 };
static const float vr[4] = { -1.0,  1.0, -1.0, 1.0 };
static const float vu[4] = { -1.0, -1.0,  1.0, 1.0 };

VS_OUT VSMain( VS_IN i )
{
    float3 planeNormal = normalize( -( i.pos - CameraPosition ) );
    float3 position = i.pos;
    float3 upVector;
    float3 rightVector;

    int visIsQuadPoly = int( step( 10.0, float( i.type ) ) );
    int visOrientation = int( i.type ) - ( 10 * visIsQuadPoly );
    float sizeScale = ( 0.5 * float( visIsQuadPoly ) ) + 0.5;

    if ( visOrientation == 2 ) {
        rightVector = i.size;
        upVector = i.vel;
    } else if ( visOrientation == 3 ) {
        float3 velY = normalize( i.vel );
        float3 velX = normalize( cross( planeNormal, velY ) );
        rightVector = velX * i.size.x * sizeScale;
        upVector = velY * i.size.y * sizeScale;
    } else if ( visOrientation == 1 ) {
        float3 velY = normalize( i.vel );
        float3 velX = normalize( cross( planeNormal, velY ) );
        velY = normalize( cross( planeNormal, velX ) );
        rightVector = velX * i.size.x * sizeScale;
        upVector = velY * i.size.y * sizeScale;
    } else {
        upVector = float3( 0.0, 1.0, 0.0 );
        rightVector = normalize( cross( planeNormal, upVector ) );
        upVector = normalize( cross( planeNormal, rightVector ) );
        rightVector = rightVector * i.size.x * sizeScale;
        upVector = upVector * i.size.y * sizeScale;
        position += float3( i.size.x * 0.5, -i.size.y * 0.5, 0.0 ) * float( 1 - visIsQuadPoly );
    }

    position += rightVector * vr[i.vertexID];
    position += upVector * vu[i.vertexID];

    VS_OUT o;
    o.clip = mul( float4( position, 1.0 ), ViewProj );
    o.uv   = float2( tu[i.vertexID], tv[i.vertexID] );
    o.dif  = float4( i.dif.rgb, pow( i.dif.a, 2.2 ) );   // gamma the alpha, like VS_ParticlePoint
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 c = tx.Sample( smp, i.uv ) * i.dif;   // color = texture * particle diffuse (blend picks add/alpha/mul)
    return float4( pow( saturate( c.rgb ), 2.2 ), c.a );   // linearize rgb for the linear HDR buffer (emissive)
}
)";

    // Per-decal instance data (per-instance vertex stream, slot 1). World = world*offset*scale (view NOT
    // baked in — unlike D3D11, the D3D12 decal VS applies the standard ViewProj, so the CPU only needs the
    // model matrix). Color.a = the material's ghost alpha ((GetColor()>>24)/255); rgb is unused. 80 bytes.
    struct DecalInstanceInfo {
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT4   Color;
    };
    static_assert( sizeof( DecalInstanceInfo ) == 80, "DecalInstanceInfo layout must match the decal input layout" );

    // Shared unit-quad vertex (per-vertex stream, slot 0). Matches D3D11's decal QuadVertexBuffer verts.
    struct DecalQuadVertex { float px, py, pz; float u, v; };
    static_assert( sizeof( DecalQuadVertex ) == 20, "DecalQuadVertex must be tightly packed (stride 20)" );

    // Decal shader. The quad is expanded by the per-instance world matrix (built on the CPU from the vob's
    // world matrix + DecalOffset/DecalSize + camera-alignment, exactly like D3D11's DrawDecalList), then
    // transformed by the standard ViewProj. Two pixel shaders: PSMainLit (opaque/alpha-test cutout — blood,
    // arrows) and PSMainBlend (texture * material alpha; the PSO blend state does add/alpha/modulate).
    constexpr char kDecalShaderSource[] = R"(
cbuffer FrameCB : register(b0) { float4x4 ViewProj; };   // default column-major packing (see world shader)

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

struct VS_IN
{
    float3   pos    : POSITION;
    float2   uv     : TEXCOORD0;
    float4x4 iworld : INSTANCE_WORLD_MATRIX;   // per-instance model matrix (world*offset*scale)
    float4   icolor : INSTANCE_COLOR;          // .a = ghost alpha, .rgb unused
};
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float alpha : TEXCOORD1; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    float3 worldPos = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;
    o.clip  = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv    = i.uv;
    o.alpha = i.icolor.a;
    return o;
}

float4 PSMainLit( VS_OUT i ) : SV_TARGET   // opaque / alpha-test cutout, fully opaque output
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    return float4( pow( t.rgb, 2.2 ), 1.0 );   // linearize for the linear HDR buffer
}

float4 PSMainBlend( VS_OUT i ) : SV_TARGET // transparent — the PSO blend state picks add/alpha/modulate
{
    float4 t = tx.Sample( smp, i.uv );
    return float4( pow( t.rgb, 2.2 ), t.a * i.alpha );   // linearize rgb; alpha unchanged
}
)";

    // Round a ring offset up so the next allocation starts on a 256-byte boundary (D3D12 requires root
    // CBV addresses to be 256-byte aligned).
    UINT AlignCB( UINT offset ) { return ( offset + 255u ) & ~255u; }

    // Per-frame linear-fog parameters, bound to the 3D shaders as 8 root 32-bit constants. Field order
    // MUST match the HLSL `cbuffer FogCB { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; }`
    // (root constants map by DWORD offset). The VS computes distance(worldPos, CamPosWS) (== view-space
    // distance for a rigid view transform), the PS lerps toward FogColor over [FogNear, FogFar].
    struct FogConstants {
        float FogColor[3];
        float FogNear;
        float CamPos[3];
        float FogFar;
    };
    static_assert( sizeof( FogConstants ) == 32, "FogConstants must be 8 DWORDs to match the fog root constants" );

    // Builds this frame's fog constants from Gothic's sky state. FogColor = GetFogColor() (0..1, weather /
    // sky-override correct — the same color used to clear the sky); FogFar = the sky controller's far-Z,
    // FogNear = 0.3*FarZ (mirrors Gothic's own 0.3 factor). Safe only in-game (world loaded).
    FogConstants MakeFogConstants() {
        FogConstants fog = {};
        DirectX::XMFLOAT3 fc;
        DirectX::XMStoreFloat3( &fc, Engine::GAPI->GetFogColor() );
        fog.FogColor[0] = fc.x; fog.FogColor[1] = fc.y; fog.FogColor[2] = fc.z;

        float farZ = Engine::GAPI->GetFarZ();
        if ( !( farZ > 1.0f ) ) farZ = 40000.0f;   // fallback if the controller reports 0 / invalid
        fog.FogFar = farZ;
        fog.FogNear = 0.3f * farZ;

        DirectX::XMFLOAT3 cp;
        DirectX::XMStoreFloat3( &cp, Engine::GAPI->GetCameraPositionXM() );
        fog.CamPos[0] = cp.x; fog.CamPos[1] = cp.y; fog.CamPos[2] = cp.z;
        return fog;
    }

    D3D12_RESOURCE_BARRIER TransitionBarrier( ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after ) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        return b;
    }

    bool EnsureCopyCommandObjects( ID3D12Device* device, ComPtr<ID3D12CommandAllocator>& allocator, ComPtr<ID3D12GraphicsCommandList>& cmdList ) {
        if ( !device ) return false;
        if ( !allocator ) {
            if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_COPY,
                IID_PPV_ARGS( allocator.ReleaseAndGetAddressOf() ) ) ) ) {
                return false;
            }
        }
        if ( !cmdList ) {
            if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_COPY,
                allocator.Get(), nullptr, IID_PPV_ARGS( cmdList.ReleaseAndGetAddressOf() ) ) ) ) {
                return false;
            }
        }
        return true;
    }

    gtl::flat_hash_map<BaseVisualInfo*, int16_t> g_vobInfoVisualToBucket;
    std::vector<BaseVisualInfo*> g_vobInfoVisualIndexToVisualInfo;
    RenderView g_GeometryPassVobs;
    RenderView g_ShadowPassVobs[3]; // per shadow cascade
}

D3D12GraphicsEngine::D3D12GraphicsEngine() {
    m_LineRenderer = std::make_unique<D3D12LineRenderer>();
}

D3D12GraphicsEngine::~D3D12GraphicsEngine() {
    if ( m_SwapChainReady ) {
        WaitForGpuIdle();
        // Force-run all remaining cleanups
        for ( UINT i = 0; i < kBackBufferCount; ++i ) {
            for ( auto& cleanupCallback : m_PerFrameCleanupItems[i] ) {
                cleanupCallback();
            }
            m_PerFrameCleanupItems[i].clear();
        }
    }
    if ( m_FenceEvent ) CloseHandle( m_FenceEvent );
    if ( m_UploadEvent ) CloseHandle( m_UploadEvent );
}

XRESULT D3D12GraphicsEngine::Init() {
    if ( !m_Device.Init() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: device creation failed.";
        return XR_FAILED;
    }
    if ( !CreateAllocators() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create allocators.";
        return XR_FAILED;
    }
    if ( !CreateUploadObjects() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create upload objects.";
        return XR_FAILED;
    }
    if ( !InitCopyQueue() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to initialize the copy queue.";
        return XR_FAILED;
    }
    if ( !CreateSrvHeap() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create SRV heap.";
        return XR_FAILED;
    }
    if ( !CreateUIPipeline() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the 2D/UI pipeline.";
        return XR_FAILED;
    }
    if ( !CreateWhiteTexture() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the white fallback texture.";
        return XR_FAILED;
    }
    if ( !CreateWorldPipeline() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the world-mesh pipeline.";
        return XR_FAILED;
    }
    if ( !CreateDepthPrepassPipeline() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the depth prepass pipeline.";
        return XR_FAILED;
    }
    if ( !CreateWorldIndirect() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the world ExecuteIndirect resources.";
        return XR_FAILED;
    }
    if ( !CreateLightCullPipeline() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the light-culling compute pipeline.";
        return XR_FAILED;
    }
    if ( !CreateVobPipeline() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the VOB pipeline.";
        return XR_FAILED;
    }
    if ( !CreateLightBuffer() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the point-light buffer.";
        return XR_FAILED;
    }
    if ( !CreateSkeletalPipeline() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the skeletal pipeline.";
        return XR_FAILED;
    }
    if ( !CreateShadowMap() ) {
        // Fatal: the lit world PSO samples the shadow map (t4) + CB (b3) unconditionally, so a missing map would
        // leave those root slots unbound. Failing here cleanly falls back to D3D11 (D3D12 is dev-forced/opt-in).
        // Runs after the depth-prepass + VOB + skeletal pipelines so the caster PSOs can reuse all three depth VS blobs.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the sun shadow map.";
        return XR_FAILED;
    }
    if ( !CreatePointShadowCubes() ) {
        // Fatal: the lit PSOs sample the cube array (t5) unconditionally once P2.10d lands, so a missing resource
        // would leave that root slot unbound. Failing here cleanly falls back to D3D11 (D3D12 is dev-forced).
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create point-light shadow cubes.";
        return XR_FAILED;
    }
    if ( !CreateWaterPipeline() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the water pipeline.";
        return XR_FAILED;
    }
    if ( !CreateParticlePipeline() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the particle pipeline.";
        return XR_FAILED;
    }
    if ( !CreateDecalPipeline() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the decal pipeline.";
        return XR_FAILED;
    }
    if ( !CreateTonemapPipeline() ) {
        // Fatal: the 3D scene PSOs now target the HDR scene-color RT (kSceneColorFormat), so without the tonemap
        // resolve nothing reaches the swapchain. Failing here cleanly falls back to D3D11 (D3D12 is dev-forced).
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the tonemap pipeline.";
        return XR_FAILED;
    }
    LogInfo() << "D3D12GraphicsEngine initialized (device + 2D + world + VOB + skeletal + water + particle + decal + HDR tonemap pipelines up). Swapchain is created once the game window is set.";
    return XR_SUCCESS;
}

void D3D12GraphicsEngine::OnAddVob(VobInfo* vi) {
    auto [it, inserted] = g_vobInfoVisualToBucket.try_emplace(vi->VisualInfo);
    if (inserted) {
        // newly seen visual, add it to our vob instancing helpers
        it->second = static_cast<int16_t>(g_vobInfoVisualIndexToVisualInfo.size());
        g_vobInfoVisualIndexToVisualInfo.push_back(it->first);
        g_GeometryPassVobs.buckets.push_back({});
        
        for (auto& v : g_ShadowPassVobs) {
            v.buckets.push_back({});
        }
    }
    vi->VisualIndex = it->second;
}

void D3D12GraphicsEngine::OnLoadWorld()
{
    g_vobInfoVisualToBucket.clear();
    g_vobInfoVisualIndexToVisualInfo.clear();
    g_GeometryPassVobs.Reset();
    for (auto& v : g_ShadowPassVobs) {
        v.Reset();
    }
}

bool D3D12GraphicsEngine::CreateAllocators() {
    D3D12MA::ALLOCATOR_DESC allocatorDesc{};
    allocatorDesc.pDevice = m_Device.GetDevice();
    allocatorDesc.pAdapter = m_Device.GetAdapter();
    allocatorDesc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
    if (FAILED(D3D12MA::CreateAllocator(&allocatorDesc, m_Allocator.ReleaseAndGetAddressOf()))) {
        return false;
    }
    
    return m_Allocator != nullptr;
}

bool D3D12GraphicsEngine::CreateUploadObjects() {
    ID3D12Device* device = m_Device.GetDevice();
    if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS( m_UploadAllocator.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_UploadAllocator.Get(), nullptr, IID_PPV_ARGS( m_UploadCmdList.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_UploadCmdList->Close();
    if ( FAILED( device->CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( m_UploadFence.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_UploadEvent = CreateEvent( nullptr, FALSE, FALSE, nullptr );
    return m_UploadEvent != nullptr;
}

bool D3D12GraphicsEngine::InitCopyQueue() {
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    if ( FAILED( device->CreateCommandQueue( &queueDesc, IID_PPV_ARGS( m_CopyQueue.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( FAILED( device->CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( m_CopyFence.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    m_CopyFenceEvent = CreateEvent( nullptr, FALSE, FALSE, nullptr );
    return m_CopyFenceEvent != nullptr;
}

void D3D12GraphicsEngine::ReleaseCompletedCopyResources( UINT64 fenceValue ) {
    while ( !m_PendingCopyReleases.empty() ) {
        auto& pending = m_PendingCopyReleases.front();
        if ( pending.FenceValue > fenceValue ) break;
        m_PendingCopyReleases.erase( m_PendingCopyReleases.begin() );
    }
}

void D3D12GraphicsEngine::WaitForCopyFence( UINT64 fenceValue ) {
    if ( !m_CopyFence || !m_CopyFenceEvent ) return;
    if ( m_CopyFence->GetCompletedValue() >= fenceValue ) return;
    m_CopyFence->SetEventOnCompletion( fenceValue, m_CopyFenceEvent );
    WaitForSingleObject( m_CopyFenceEvent, INFINITE );
}

void D3D12GraphicsEngine::TransitionTextureToSRVOnDirectQueue( ID3D12Resource* texture ) {
    if ( !texture || !m_Device.GetDevice() ) return;

    ID3D12Device* device = m_Device.GetDevice();
    ComPtr<ID3D12CommandAllocator> transitionAllocator;
    ComPtr<ID3D12GraphicsCommandList> transitionCmdList;

    if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS( transitionAllocator.ReleaseAndGetAddressOf() ) ) ) )
        return;
    if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        transitionAllocator.Get(), nullptr, IID_PPV_ARGS( transitionCmdList.ReleaseAndGetAddressOf() ) ) ) )
        return;

    auto toSRV = TransitionBarrier( texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
    transitionCmdList->ResourceBarrier( 1, &toSRV );
    if ( FAILED( transitionCmdList->Close() ) ) return;

    ID3D12CommandList* lists[] = { transitionCmdList.Get() };
    m_Device.GetDirectQueue()->ExecuteCommandLists( 1, lists );

    const UINT64 waitValue = ++m_UploadFenceValue;
    if ( FAILED( m_Device.GetDirectQueue()->Signal( m_UploadFence.Get(), waitValue ) ) ) return;
    if ( m_UploadFence && m_UploadEvent ) {
        m_UploadFence->SetEventOnCompletion( waitValue, m_UploadEvent );
        WaitForSingleObject( m_UploadEvent, INFINITE );
    }
}

bool D3D12GraphicsEngine::UploadTextureSubresources( ID3D12Resource* dst, const D3D12_SUBRESOURCE_DATA* subresources, UINT numSubresources ) {
    if ( !dst || !subresources || numSubresources == 0 ) return false;
    ID3D12Device* device = m_Device.GetDevice();

    D3D12_RESOURCE_DESC desc = dst->GetDesc();

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts( numSubresources );
    std::vector<UINT>    numRows( numSubresources );
    std::vector<UINT64>  rowSizes( numSubresources );
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints( &desc, 0, numSubresources, 0, layouts.data(), numRows.data(), rowSizes.data(), &totalBytes );

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = totalBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    
    ComPtr<D3D12MA::Allocation> uploadAllocation;
    ComPtr<ID3D12Resource> upload;
    if ( FAILED( m_Allocator->CreateResource(
        &allocDesc,
        &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        uploadAllocation.ReleaseAndGetAddressOf(),
        IID_PPV_ARGS( upload.ReleaseAndGetAddressOf() ) ) ) ) {
        return false;
    }

    BYTE* mapped = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    if ( FAILED( upload->Map( 0, &noRead, reinterpret_cast<void**>( &mapped ) ) ) )
        return false;

    for ( UINT i = 0; i < numSubresources; ++i ) {
        BYTE* dstSlice = mapped + layouts[i].Offset;
        const BYTE* srcData = reinterpret_cast<const BYTE*>( subresources[i].pData );
        for ( UINT row = 0; row < numRows[i]; ++row ) {
            memcpy( dstSlice + static_cast<SIZE_T>( layouts[i].Footprint.RowPitch ) * row,
                srcData + static_cast<SIZE_T>( subresources[i].RowPitch ) * row,
                static_cast<SIZE_T>( rowSizes[i] ) );
        }
    }
    upload->Unmap( 0, nullptr );

    ComPtr<ID3D12CommandAllocator> copyAllocator;
    ComPtr<ID3D12GraphicsCommandList> copyCmdList;
    if ( !EnsureCopyCommandObjects( device, copyAllocator, copyCmdList ) )
        return false;

    for ( UINT i = 0; i < numSubresources; ++i ) {
        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = dst;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = i;

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = upload.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = layouts[i];

        copyCmdList->CopyTextureRegion( &dstLoc, 0, 0, 0, &srcLoc, nullptr );
    }

    if ( FAILED( copyCmdList->Close() ) )
        return false;

    std::lock_guard<std::mutex> lock( m_CopyQueueMutex );

    ID3D12CommandList* lists[] = { copyCmdList.Get() };
    m_CopyQueue->ExecuteCommandLists( 1, lists );

    const UINT64 fenceValue = ++m_CopyFenceValue;
    if ( FAILED( m_CopyQueue->Signal( m_CopyFence.Get(), fenceValue ) ) )
        return false;

    // Asynchronous GPU-side queue barrier: Direct Queue waits on GPU for copy execution to finish
    m_Device.GetDirectQueue()->Wait( m_CopyFence.Get(), fenceValue );

    m_PendingCopyReleases.push_back( PendingCopyRelease {
        fenceValue,
        std::move( uploadAllocation ),
        std::move( upload ),
        std::move( copyAllocator ),
        std::move( copyCmdList )
    } );

    ReleaseCompletedCopyResources( m_CopyFence->GetCompletedValue() );
    return true;
}

bool D3D12GraphicsEngine::CreateSrvHeap() {
    ID3D12Device* device = m_Device.GetDevice();
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = kSrvHeapCapacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if ( FAILED( device->CreateDescriptorHeap( &desc, IID_PPV_ARGS( m_SrvHeap.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_SrvDescriptorSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
    m_SrvHeapCapacity = kSrvHeapCapacity;
    m_SrvAllocated = 0;
    return true;
}

UINT D3D12GraphicsEngine::AllocateSrvSlot() {
    // Try to reuse a freed slot first
    if ( !m_FreeSrvSlots.empty() ) {
        UINT slot = m_FreeSrvSlots.back();
        m_FreeSrvSlots.pop_back();
        return slot;
    }

    // Fall back to bump allocation
    if ( m_SrvAllocated >= m_SrvHeapCapacity ) {
        LogWarn() << "D3D12: SRV heap exhausted (" << m_SrvHeapCapacity << " descriptors).";
        return UINT_MAX;
    }
    return m_SrvAllocated++;
}

void D3D12GraphicsEngine::FreeSrvSlot( UINT slot ) {
    if ( slot == UINT_MAX 
        || slot == m_WhiteTexture->GetSrvSlot() 
        || slot == m_BlackTexture->GetSrvSlot()
        || slot == m_DefaultOrmTexture->GetSrvSlot()
        )
    {
        return;
    }

    // Nullify the descriptor to prevent pointing to dead memory
    ID3D12Device* device = m_Device.GetDevice();
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = GetSrvCpuHandle( slot );

    D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
    nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullDesc.Texture2D.MipLevels = 1;

    // Bind white texture to free slot.

    // Writing a null resource view to this descriptor slot safely clears it
    device->CreateShaderResourceView( m_WhiteTexture->GetResource(), &nullDesc, cpuHandle);

    m_FreeSrvSlots.push_back( slot );
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvCpuHandle( UINT slot ) const {
    if ( std::ranges::contains( m_FreeSrvSlots, slot ) ) {
        // Ensure invalid slots provide some texture instead of breaking
        return GetSrvCpuHandle( m_BlackTexture->GetSrvSlot() );
    }

    D3D12_CPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>( slot ) * m_SrvDescriptorSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvGpuHandle( UINT slot ) const {
    if ( std::ranges::contains( m_FreeSrvSlots, slot ) ) {
        // Ensure invalid slots provide some texture instead of breaking
        return GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    }

    D3D12_GPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>( slot ) * m_SrvDescriptorSize; 
    return h;
}

bool D3D12GraphicsEngine::CreateUIPipeline() {
    ID3D12Device* device = m_Device.GetDevice();

    // --- Root signature: b0 root constants (viewport), t0 SRV table, static linear-wrap sampler s0 ---
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;         // t0
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // FFPipelineConstantBuffer (b1) is fed Gothic's GraphicsState each draw as root constants — the
    // struct layout matches the HLSL cbuffer 1:1 (same reason D3D11 memcpy's it into the CB).
    static_assert( sizeof( GothicGraphicsState ) == 144, "FF constant layout must match FFPipelineConstantBuffer" );

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;  // b0
    params[0].Constants.Num32BitValues = 4;  // float2 pos + float2 size
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 1;  // b1
    params[2].Constants.Num32BitValues = sizeof( GothicGraphicsState ) / 4;  // 36 DWORDs
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;              // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: root signature serialize error: " << static_cast<const char*>( rsErr->GetBufferPointer() );
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( m_UIRootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    // --- Compile inline shaders ---
    UINT compileFlags = 0;
    ComPtr<ID3DBlob> err;
    if ( !CompileShaderD3D12( kUIShaderSource, sizeof( kUIShaderSource ) - 1, "UIShader", nullptr, nullptr,
        "VSMain", Shadermodel_VS, compileFlags, 0, m_UIVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !CompileShaderD3D12( kUIShaderSource, sizeof( kUIShaderSource ) - 1, "UIShader", nullptr, nullptr,
        "PSMain", Shadermodel_PS, compileFlags, 0, m_UIPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // PSOs are built per blend state on demand (GetOrCreateUIPipeline). Warm the default (opaque) one so
    // any Init-time failure surfaces here rather than mid-frame.
    GothicBlendStateInfo defaultBlend;
    defaultBlend.SetDefault();

    GothicDepthBufferStateInfo defaultDepth;
    defaultDepth.SetDefault();
    if ( !GetOrCreateUIPipeline( defaultBlend, defaultDepth ) ) {
        LogWarn() << "D3D12: failed to create the default 2D/UI pipeline state.";
        return false;
    }

    return CreateUIVertexBuffers();
}

namespace {
    // Packs the blend-relevant fields of a Gothic blend state into a stable key for the PSO cache.
    // Gothic's EBlendFunc/EBlendOp are "laid out for D3D11" and D3D12_BLEND/_OP share those numeric
    // values, so they slot straight into the packed key (and cast directly into the PSO below).
    uint32_t BlendKey( const GothicBlendStateInfo& b ) {
        uint32_t k = 0;
        k |= (b.BlendEnabled ? 1u : 0u);
        k |= (b.ColorWritesEnabled ? 1u : 0u) << 1;
        k |= (b.AlphaToCoverage ? 1u : 0u) << 2;
        k |= (static_cast<uint32_t>(b.SrcBlend) & 0x1F) << 3;
        k |= (static_cast<uint32_t>(b.DestBlend) & 0x1F) << 8;
        k |= (static_cast<uint32_t>(b.BlendOp) & 0x07) << 13;
        k |= (static_cast<uint32_t>(b.SrcBlendAlpha) & 0x1F) << 16;
        k |= (static_cast<uint32_t>(b.DestBlendAlpha) & 0x1F) << 21;
        k |= (static_cast<uint32_t>(b.BlendOpAlpha) & 0x07) << 26;
        return k;
    }

    // Packs the depth-relevant fields into a stable key. ECompareFunc is "laid out for D3D11" and
    // D3D12_COMPARISON_FUNC shares those numeric values, so it casts straight into the PSO below.
    uint32_t DepthKey( const GothicDepthBufferStateInfo& d ) {
        uint32_t k = 0;
        k |= (d.DepthBufferEnabled ? 1u : 0u);
        k |= (d.DepthWriteEnabled ? 1u : 0u) << 1;
        k |= (static_cast<uint32_t>(d.DepthBufferCompareFunc) & 0x0F) << 2;
        return k;
    }
}

ID3D12PipelineState* D3D12GraphicsEngine::GetOrCreateUIPipeline(
    const GothicBlendStateInfo& blend,
    const GothicDepthBufferStateInfo& depth ) {
    const uint64_t key = static_cast<uint64_t>(BlendKey( blend )) | (static_cast<uint64_t>(DepthKey( depth )) << 32);
    auto it = m_UIPipelines.find( key );
    if ( it != m_UIPipelines.end() ) return it->second.Get();

    // --- Input layout: mirrors layout1 (the ExVertexStruct HUD layout; tangent treated as padding) ---
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "DIFFUSE",  0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // --- PSO: no depth (2D), cull-none, triangle list; blend emulates Gothic's per-draw state ---
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_UIRootSig.Get();
    pso.VS = { m_UIVsBlob->GetBufferPointer(), m_UIVsBlob->GetBufferSize() };
    pso.PS = { m_UIPsBlob->GetBufferPointer(), m_UIPsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kBackBufferFormat;   // 2D UI draws straight to the swapchain (after the tonemap resolve)
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    // Depth clip OFF for the 2D path (matches GothicRasterizerStateInfo::SetDefault's D3D11 default). The
    // pre-transformed UI/glyph verts carry z = camera near+1 (AppendGlyphs), which exceeds the [0,1] clip
    // range — with clipping enabled the driver discards them ("depth clipped"); disabled, z is just clamped.
    pso.RasterizerState.DepthClipEnable = FALSE;

    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = blend.BlendEnabled ? TRUE : FALSE;
    rt.SrcBlend = static_cast<D3D12_BLEND>(blend.SrcBlend);
    rt.DestBlend = static_cast<D3D12_BLEND>(blend.DestBlend);
    rt.BlendOp = static_cast<D3D12_BLEND_OP>(blend.BlendOp);
    rt.SrcBlendAlpha = static_cast<D3D12_BLEND>(blend.SrcBlendAlpha);
    rt.DestBlendAlpha = static_cast<D3D12_BLEND>(blend.DestBlendAlpha);
    rt.BlendOpAlpha = static_cast<D3D12_BLEND_OP>(blend.BlendOpAlpha);
    rt.RenderTargetWriteMask = blend.ColorWritesEnabled ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
    pso.BlendState.AlphaToCoverageEnable = blend.AlphaToCoverage ? TRUE : FALSE;

    // Honor the caller's depth state. A DSV is bound for the whole frame (OnBeginFrame), so DSVFormat must
    // match it (D32_FLOAT) even when the test is disabled — otherwise the bound-DSV/PSO-format mismatch makes
    // the driver reject the draw ("depth test failed"). DrawString forces this state off so text never tests.
    if ( depth.DepthBufferEnabled ) {
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = depth.DepthWriteEnabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(depth.DepthBufferCompareFunc);
    } else {
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    }
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    ComPtr<ID3D12PipelineState> state;
    if ( FAILED( m_Device.GetDevice()->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( state.GetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed for UI pipeline key 0x" << std::hex << key << ".";
        return nullptr;
    }
    ID3D12PipelineState* raw = state.Get();
    m_UIPipelines.emplace( key, std::move( state ) );
    return raw;
}

bool D3D12GraphicsEngine::CreateUIVertexBuffers() {
    ID3D12Device* device = m_Device.GetDevice();
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kUIVertexBufferBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( m_UIVertexBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_UIVertexBuffer[i]->SetName( i == 0 ? L"UIVertexRing0" : L"UIVertexRing1" );
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( m_UIVertexBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_UIVertexBufferPtr[i] ) ) ) )
            return false;
    }
    m_UIVertexBufferCapacity = kUIVertexBufferBytes;
    return true;
}

bool D3D12GraphicsEngine::CreateWhiteTexture() {
    CreateTexture(m_WhiteTexture);
    const uint32_t white = 0xFFFFFFFFu;
    if (XR_SUCCESS != m_WhiteTexture->Init(INT2(1,1), GfxTexture::ETextureFormat::TF_R8G8B8A8, 1, &white, "WhiteFallbackTexture" )) {
        return false;
    }
    
    CreateTexture(m_BlackTexture);
    const uint32_t black = 0xFF000000;
    if (XR_SUCCESS != m_BlackTexture->Init(INT2(1,1), GfxTexture::ETextureFormat::TF_R8G8B8A8, 1, &black, "BlackFallbackTexture" )) {
        return false;
    }
    
    CreateTexture(m_DefaultOrmTexture);
    const uint32_t orm = 0xFF00E6FFu;
    if (XR_SUCCESS != m_DefaultOrmTexture->Init(INT2(1,1), GfxTexture::ETextureFormat::TF_R8G8B8A8, 1, &orm, "DefaultOrmTexture(1,0.9,0)" )) {
        return false;
    }
    
    return true;
}

void D3D12GraphicsEngine::BindSurfaceTextures( int /*slot*/, GfxTexture* diffuse, GfxTexture* /*normalmap*/, unsigned int /*numTextures*/ ) {
    // Record the diffuse texture for the next 2D draw. Slot 0 only for now (normalmap unused in the UI path).
    m_CurrentTexture = diffuse;
}

// Glyph-quad builder — shared with the D3D11 backend (external linkage, defined in D3D11GraphicsEngine.cpp).
// Pure geometry (font atlas -> ExVertexStruct triangle list), no backend dependency, so we reuse it verbatim.
namespace UI::zFont {
    void AppendGlyphs( std::vector<ExVertexStruct>& vertices, std::string_view str, float x, float y,
        const ::zFont* font, zColor fontColor, float scale, zCCamera* camera );
}

void D3D12GraphicsEngine::DrawString( std::string_view str, float x, float y, const zFont* font, zColor& fontColor ) {
    if ( !m_FrameOpen || !font || !font->tex )
        return;

    // Strip trailing '/' markers (Gothic control chars), like D3D11 DrawString.
    size_t maxLen = str.size();
    while ( maxLen > 0 && str[maxLen - 1] == '/' ) --maxLen;
    if ( !maxLen ) return;
    str = str.substr( 0, maxLen );

    constexpr float FONT_CACHE_PRIO = -1.0f;
    zCTexture* tx = font->tex;
    if ( tx->CacheIn( FONT_CACHE_PRIO ) != zRES_CACHED_IN )
        return;

    // UIScale mirrors D3D11 DrawString: swim-bar width / 180 (the custom-font multiplier defaults to 1).
    float UIScale = 1.0f;
    static int savedBarSize = -1;
    if ( oCGame::GetGame() ) {
        if ( savedBarSize == -1 && oCGame::GetGame()->swimBar )
            savedBarSize = oCGame::GetGame()->swimBar->psizex;
        if ( savedBarSize > 0 )
            UIScale = static_cast<float>(savedBarSize) / 180.f;
    }

    // Build glyph quads over the font atlas (screen-space xyzrhw ExVertexStruct triangle list).
    static std::vector<ExVertexStruct> vertices;
    vertices.clear();
    UI::zFont::AppendGlyphs( vertices, str, x, y, font, fontColor, UIScale, zCCamera::GetCamera() );
    if ( vertices.empty() )
        return;

    // Text = texture * vertex color, alpha-blended. Force the FF stage + blend that DrawVertexArray reads,
    // save/restore so Gothic's tracked 2D state is undisturbed (mirrors D3D11 DrawString exactly).
    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    GothicBlendStateInfo savedBlend = rs.BlendState.Clone();
    GothicDepthBufferStateInfo savedDepth = rs.DepthState.Clone();
    FixedFunctionStage::EColorOp    savedOp0 = rs.GraphicsState.FF_Stages[0].ColorOp;
    FixedFunctionStage::EColorOp    savedOp1 = rs.GraphicsState.FF_Stages[1].ColorOp;
    FixedFunctionStage::ETextureArg savedA1 = rs.GraphicsState.FF_Stages[0].ColorArg1;
    FixedFunctionStage::ETextureArg savedA2 = rs.GraphicsState.FF_Stages[0].ColorArg2;

    rs.BlendState.SetAlphaBlending();
    // Text is drawn over the finished scene (during the world pass the depth buffer holds scene depth); the
    // glyph quads must never depth-test, so force depth off — GetOrCreateUIPipeline picks the no-test PSO.
    rs.DepthState.DepthBufferEnabled = false;
    rs.DepthState.DepthWriteEnabled = false;
    rs.GraphicsState.FF_Stages[0].ColorOp = FixedFunctionStage::EColorOp::CO_MODULATE;
    rs.GraphicsState.FF_Stages[1].ColorOp = FixedFunctionStage::EColorOp::CO_DISABLE;
    rs.GraphicsState.FF_Stages[0].ColorArg1 = FixedFunctionStage::ETextureArg::TA_TEXTURE;
    rs.GraphicsState.FF_Stages[0].ColorArg2 = FixedFunctionStage::ETextureArg::TA_DIFFUSE;

    // Bind the font atlas as the diffuse texture for this draw only.
    GfxTexture* prevTex = m_CurrentTexture;
    if ( MyDirectDrawSurface7* surface = tx->GetSurface() )
        m_CurrentTexture = surface->GetEngineTexture();

    DrawVertexArray( vertices.data(), static_cast<unsigned int>(vertices.size()), 0, sizeof( ExVertexStruct ) );

    m_CurrentTexture = prevTex;
    rs.BlendState = savedBlend;
    rs.DepthState = savedDepth;
    rs.GraphicsState.FF_Stages[0].ColorOp = savedOp0;
    rs.GraphicsState.FF_Stages[1].ColorOp = savedOp1;
    rs.GraphicsState.FF_Stages[0].ColorArg1 = savedA1;
    rs.GraphicsState.FF_Stages[0].ColorArg2 = savedA2;
}

bool D3D12GraphicsEngine::CreateDepthBuffer( INT2 size ) {
    if ( size.x <= 0 || size.y <= 0 ) return false;
    ID3D12Device* device = m_Device.GetDevice();

    // DSV heap (single descriptor) — created once, reused across resizes.
    if ( !m_DsvHeap ) {
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if ( FAILED( device->CreateDescriptorHeap( &dsvHeapDesc, IID_PPV_ARGS( m_DsvHeap.ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_DsvDescriptorSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );
    }

    D3D12_HEAP_PROPERTIES heapDefault = {};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = static_cast<UINT64>(size.x);
    dd.Height = static_cast<UINT>(size.y);
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_R32_TYPELESS;   // typeless so the same texels serve a D32_FLOAT DSV and an R32_FLOAT SRV
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // Reversed-Z: the world clears depth to 0.0, so make that the optimized clear value.
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 0.0f;

    // Born in DEPTH_WRITE. Now also SRV-readable: DispatchLightCulling brackets a NON_PIXEL_SHADER_RESOURCE
    // read of it (per-tile far-Z) and transitions back to DEPTH_WRITE, so it is DEPTH_WRITE at every other point.
    if ( FAILED( device->CreateCommittedResource( &heapDefault, D3D12_HEAP_FLAG_NONE, &dd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS( m_DepthBuffer.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: failed to create the depth buffer (" << size.x << "x" << size.y << ").";
        return false;
    }
    m_DepthBuffer->SetName( L"DepthBuffer(D32)" );

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;   // typeless resource viewed as depth here
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView( m_DepthBuffer.Get(), &dsv, m_DsvHeap->GetCPUDescriptorHandleForHeapStart() );

    // R32_FLOAT SRV of the same texels for the light cull's per-tile far-Z read. Slot allocated once; the view is
    // (re)created every call so it always points at the current (post-resize) resource.
    if ( m_DepthSrvSlot == UINT_MAX ) {
        m_DepthSrvSlot = AllocateSrvSlot();
        if ( m_DepthSrvSlot == UINT_MAX ) return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC dsrv = {};
    dsrv.Format = DXGI_FORMAT_R32_FLOAT;   // typeless resource viewed as a single float channel
    dsrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    dsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dsrv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView( m_DepthBuffer.Get(), &dsrv, GetSrvCpuHandle( m_DepthSrvSlot ) );

    // Forward+ tile grid storage is resolution-dependent too — (re)build it here so it always tracks the
    // depth buffer's size (called from both init and the resize path). GPU is idle at both call sites.
    if ( !CreateLightCullBuffers( size ) ) return false;
    return true;
}

bool D3D12GraphicsEngine::CreateSceneColorTarget( INT2 size ) {
    // HDR scene-color render target (Phase 3): the 3D world/VOB/skeletal/water/decal/particle passes render into
    // this R16F target so lighting can exceed 1.0 (bright sun + stacked additive point lights keep their detail
    // instead of clipping to white). ResolveSceneToBackBuffer then tonemaps it into the swapchain. Resolution-
    // sized → (re)created here on init and every resize (RTV heap + SRV slot persist; only the resource + views
    // are rebuilt). DEFAULT-heap GPU memory (64bpp), so it barely touches the 32-bit CPU address space.
    if ( size.x <= 0 || size.y <= 0 ) return false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_RtvHeap ) return false;

    D3D12_HEAP_PROPERTIES heapDefault = {};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width  = static_cast<UINT64>( size.x );
    dd.Height = static_cast<UINT>( size.y );
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = kSceneColorFormat;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kSceneColorFormat;   // matches DrawSky's fog-color clear (values set at clear time)

    // Born in RENDER_TARGET (the world pass renders straight into it; ResolveSceneToBackBuffer flips it to
    // PIXEL_SHADER_RESOURCE and back next frame). GPU is idle at every call site (init / post-WaitForGpuIdle resize).
    if ( FAILED( device->CreateCommittedResource( &heapDefault, D3D12_HEAP_FLAG_NONE, &dd,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS( m_SceneColor.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: failed to create the HDR scene-color target (" << size.x << "x" << size.y << ").";
        return false;
    }
    m_SceneColor->SetName( L"SceneColorHDR(R16F)" );
    m_SceneColorInPixelState = false;

    // RTV in the extra heap slot (index kBackBufferCount, past the swapchain RTVs).
    m_SceneColorRtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_SceneColorRtv.ptr += static_cast<SIZE_T>( kBackBufferCount ) * m_RtvDescriptorSize;
    device->CreateRenderTargetView( m_SceneColor.Get(), nullptr, m_SceneColorRtv );

    // SRV for the tonemap resolve (slot allocated once; view re-created each call to point at the current resource).
    if ( m_SceneColorSrvSlot == UINT_MAX ) {
        m_SceneColorSrvSlot = AllocateSrvSlot();
        if ( m_SceneColorSrvSlot == UINT_MAX ) return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = kSceneColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView( m_SceneColor.Get(), &srv, GetSrvCpuHandle( m_SceneColorSrvSlot ) );
    return true;
}

bool D3D12GraphicsEngine::CreateTonemapPipeline() {
    // Fullscreen HDR->swapchain resolve (Phase 3). Exposure * scene HDR -> ACES filmic curve -> R10G10B10A2. Runs
    // once per world frame after all 3D. No vertex buffer (SV_VertexID fullscreen triangle), no depth. Created once.
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;   // t0 scene HDR
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;   // b0 { Exposure }
    params[1].Constants.Num32BitValues = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;   // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof( params );
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (tonemap)."; return false; }
    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: tonemap root sig error: " << static_cast<const char*>( rsErr->GetBufferPointer() );
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( m_TonemapRootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    constexpr char kTonemapShaderSource[] = R"(
cbuffer TonemapCB : register(b0) { float Exposure; };
Texture2D    SceneHDR : register(t0);
SamplerState smp      : register(s0);

struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUT VSFullscreen( uint vid : SV_VertexID )
{
    VS_OUT o;
    o.uv  = float2( ( vid << 1 ) & 2, vid & 2 );          // (0,0)(2,0)(0,2) covering the screen with one triangle
    o.pos = float4( o.uv * float2( 2, -2 ) + float2( -1, 1 ), 0, 1 );
    return o;
}

// Narkowicz ACES filmic fit: compresses linear HDR into [0,1] with a filmic highlight rolloff, so bright sun +
// stacked additive point lights keep their color/detail instead of clipping to flat white.
float3 ACESFilm( float3 x )
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate( ( x * ( a * x + b ) ) / ( x * ( c * x + d ) + e ) );
}
// The scene HDR buffer is LINEAR (albedo is sRGB-decoded in the lit passes). Re-encode to sRGB/gamma on the way to
// the UNORM swapchain (which stores plain gamma-encoded values, same space as the 2D UI that composites on top).
float3 LinearToSrgb( float3 c )
{
    return select( c <= 0.0031308, c * 12.92, 1.055 * pow( c, 1.0 / 2.4 ) - 0.055 );
}
float4 PSTonemap( VS_OUT i ) : SV_TARGET
{
    float3 hdr = SceneHDR.Sample( smp, i.uv ).rgb * Exposure;
    return float4( LinearToSrgb( ACESFilm( hdr ) ), 1.0 );
}
)";
    UINT compileFlags = 0;
    if ( !CompileShaderD3D12( kTonemapShaderSource, sizeof( kTonemapShaderSource ) - 1, "Tonemap",
        nullptr, nullptr, "VSFullscreen", Shadermodel_VS, compileFlags, 0, m_TonemapVsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !CompileShaderD3D12( kTonemapShaderSource, sizeof( kTonemapShaderSource ) - 1, "Tonemap",
        nullptr, nullptr, "PSTonemap", Shadermodel_PS, compileFlags, 0, m_TonemapPsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_TonemapRootSig.Get();
    pso.VS = { m_TonemapVsBlob->GetBufferPointer(), m_TonemapVsBlob->GetBufferSize() };
    pso.PS = { m_TonemapPsBlob->GetBufferPointer(), m_TonemapPsBlob->GetBufferSize() };
    pso.InputLayout = { nullptr, 0 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kBackBufferFormat;   // resolves to the swapchain
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_TonemapPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (tonemap).";
        return false;
    }
    return true;
}

void D3D12GraphicsEngine::BindSceneColorTarget() {
    // Make the HDR scene-color target the world pass's render target (+ keep the shared depth buffer). Transitions
    // it back from PIXEL_SHADER_RESOURCE (last frame's resolve left it there) to RENDER_TARGET when needed.
    if ( !m_SceneColor || !m_CmdList ) return;
    if ( m_SceneColorInPixelState ) {
        auto toRT = TransitionBarrier( m_SceneColor.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
        m_CmdList->ResourceBarrier( 1, &toRT );
        m_SceneColorInPixelState = false;
    }
    const bool haveDepth = m_DepthBuffer && m_DsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
    if ( haveDepth ) dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, haveDepth ? &dsv : nullptr );
}

void D3D12GraphicsEngine::ResolveSceneToBackBuffer() {
    // Tonemap the finished HDR scene into the swapchain backbuffer, then leave the backbuffer bound so the 2D UI
    // (drawn after OnStartWorldRendering) composites on top in LDR. If HDR is unavailable, no-op (nothing to show).
    if ( !m_SceneColor || !m_TonemapPSO || !m_TonemapRootSig || !m_CmdList ) return;
    DX_ZONE( m_CmdList, "Tonemap resolve (HDR->swapchain)" );

    if ( !m_SceneColorInPixelState ) {
        auto toSrv = TransitionBarrier( m_SceneColor.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
        m_CmdList->ResourceBarrier( 1, &toSrv );
        m_SceneColorInPixelState = true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>( m_FrameIndex ) * m_RtvDescriptorSize;
    m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, nullptr );   // no depth for the fullscreen resolve

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->SetPipelineState( m_TonemapPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_TonemapRootSig.Get() );
    m_CmdList->SetGraphicsRootDescriptorTable( 0, GetSrvGpuHandle( m_SceneColorSrvSlot ) );
    const float exposure = m_Exposure > 0.0f ? m_Exposure : 1.0f;
    m_CmdList->SetGraphicsRoot32BitConstant( 1, *reinterpret_cast<const UINT*>( &exposure ), 0 );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    m_CmdList->IASetVertexBuffers( 0, 0, nullptr );
    m_CmdList->DrawInstanced( 3, 1, 0, 0 );
}

bool D3D12GraphicsEngine::CreateWorldPipeline() {
    ID3D12Device* device = m_Device.GetDevice();

    // Root signature: b0 = ViewProj (16 root 32-bit constants, VS); t0 = diffuse SRV table (PS);
    // b1 = fog (8 root 32-bit constants, VS reads CamPosWS, PS reads color/near/far); static sampler s0.
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;         // t0
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // params[3] = point-light StructuredBuffer as a ROOT SRV at t1 (no descriptor slot consumed — a GPU VA
    // straight in the root; aligns with the CPU-offload/bindless direction). params[4] = b2 { light count,
    // NumTilesX }. params[5]/[6] = the Forward+ per-tile grid + index-list root SRVs at t2/t3. All four MUST
    // be bound (BindFrameLights) by every draw using this root sig with a light-reading PSO (m_WorldPSO/
    // m_VobPSO), else the count/grid are undefined root values and the shader loops away.
    // params[7] = shadow-sampling CB (b3) as a ROOT CBV (cascade view-projs are too big for root constants).
    // params[8] = the CSM shadow-map Texture2DArray SRV (t4) via a one-entry descriptor table off the shared
    // SRV heap. Both are read only by the lit world PS (PSMain); the depth-prepass/caster PSOs sharing this
    // root sig don't reference them, so those draws simply leave the slots unbound.
    D3D12_DESCRIPTOR_RANGE shadowSrvRange = {};
    shadowSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowSrvRange.NumDescriptors = 1;
    shadowSrvRange.BaseShaderRegister = 4;   // t4
    shadowSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    // t5 = point-light shadow cube array SRV (P2.10d), sampled by the tiled point-light loop.
    D3D12_DESCRIPTOR_RANGE cubeSrvRange = {};
    cubeSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    cubeSrvRange.NumDescriptors = 1;
    cubeSrvRange.BaseShaderRegister = 5;   // t5
    cubeSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[11] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0
    params[0].Constants.Num32BitValues = 16;  // float4x4
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 1;   // b1 fog
    params[2].Constants.Num32BitValues = 8;   // FogConstants (8 DWORDs)
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;  // VS: CamPosWS; PS: color/near/far
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 1;  // t1 light StructuredBuffer
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].Constants.ShaderRegister = 2;   // b2 { LightCount, NumTilesX, pad, pad }
    params[4].Constants.Num32BitValues = 4;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[5].Descriptor.ShaderRegister = 2;  // t2 per-tile LightGrid {Offset,Count}
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[6].Descriptor.ShaderRegister = 3;  // t3 per-tile light-index list
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[7].Descriptor.ShaderRegister = 3;  // b3 shadow-sampling CB (cascade view-projs + sun + strength)
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[8].DescriptorTable.NumDescriptorRanges = 1;
    params[8].DescriptorTable.pDescriptorRanges = &shadowSrvRange;
    params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[9].DescriptorTable.NumDescriptorRanges = 1;
    params[9].DescriptorTable.pDescriptorRanges = &cubeSrvRange;   // t5 point-shadow cube array
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // params[10] = per-material bindless indices { normalSrvIndex, ormSrvIndex } as root constants (b6). The PS
    // reads the normal/ORM maps via ResourceDescriptorHeap[...] (SM6.6 bindless) — no per-material descriptor
    // tables. normalIndex == 0xFFFFFFFF means "no normal map" (skip the TBN/perturb); ormIndex is always valid
    // (the default ORM slot when a material has no _FX map), so ORM is sampled branchlessly.
    params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[10].Constants.ShaderRegister = 6;   // b6 MaterialCB { MatNormalIndex, MatOrmIndex, MatDiffuseIndex }
    params[10].Constants.Num32BitValues = 3;   // 3rd = bindless diffuse index (world mesh ExecuteIndirect, P2.11)
    params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    // s0 diffuse: 16x anisotropic (matches D3D11's main texture sampler) — sharpens surfaces at grazing
    // angles and in the distance, which trilinear alone smears badly.
    samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
    samplers[0].MaxAnisotropy = 16;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;          // s0 diffuse
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s2: PCF comparison sampler for the CSM depth. Normal-Z map (LESS_EQUAL): SampleCmp returns 1 where the
    // fragment is closer-or-equal to the light than the stored occluder (lit), 0 where behind it (shadowed).
    // BORDER address + opaque-white border → taps past a cascade's edge read as far (lit), not spurious shadow.
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 2;          // s2 shadow comparison
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof( params );
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = _countof( samplers );
    rsDesc.pStaticSamplers = samplers;
    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED enables SM6.6 ResourceDescriptorHeap[...] bindless sampling of the
    // per-material normal/ORM maps out of the shared SRV heap (tier-3; present on the target AMD GPU).
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                 | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (world)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: world root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( m_WorldRootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    UINT compileFlags = 0;
    if ( !CompileShaderD3D12( kWorldShaderSource, sizeof( kWorldShaderSource ) - 1, "WorldShader", nullptr, nullptr,
        "VSMain", Shadermodel_VS, compileFlags, 0, m_WorldVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !CompileShaderD3D12( kWorldShaderSource, sizeof( kWorldShaderSource ) - 1, "WorldShader", nullptr, nullptr,
        "PSMain", Shadermodel_PS, compileFlags, 0, m_WorldPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // Bind Position/TexCoord0/Color from the packed 36-byte ExVertexStructGPU via explicit offsets;
    // the packed normal (@12), tangent (@16) and uv2 (@28) are skipped (not read by this PS yet).
    //   Position float3   @ 0
    //   [Normal  i16x2    @12]   [Tangent R10G10B10A2 @16]  (skipped)
    //   TexCoord float2   @20
    //   [TexCoord2 half2  @28]                              (skipped)
    //   Color    R8G8B8A8 @32
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R16G16_SNORM,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },  // octahedral, world-space
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "DIFFUSE",  0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_WorldRootSig.Get();
    pso.VS = { m_WorldVsBlob->GetBufferPointer(), m_WorldVsBlob->GetBufferSize() };
    pso.PS = { m_WorldPsBlob->GetBufferPointer(), m_WorldPsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Cull NONE for first-light so a wrong winding assumption can't hide the whole world.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Reversed-Z: test + write depth, pass on GREATER_EQUAL (matches Gothic's infinite-far projection).
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_WorldPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (world).";
        return false;
    }
    return true;
}

bool D3D12GraphicsEngine::CreateDepthPrepassPipeline() {
    // Forward+ opaque depth prepass (P2.9b-1): a depth-only variant of the world-mesh pass. Reuses
    // m_WorldRootSig (only b0 ViewProj + t0/s0 are referenced by the prepass shaders) and the world's
    // packed 36-byte vertex, but binds just Position + TexCoord0, writes NO color (write mask 0), and
    // keeps the exact reversed-Z GREATER_EQUAL depth-write state so the depth it lays down is bit-identical
    // to what the opaque world pass would write. Must run AFTER CreateWorldPipeline (needs m_WorldRootSig).
    ID3D12Device* device = m_Device.GetDevice();
    if ( !m_WorldRootSig ) { LogWarn() << "D3D12: depth prepass needs the world root sig."; return false; }

    UINT compileFlags = 0;
    if ( !CompileShaderD3D12( kDepthPrepassShaderSource, sizeof( kDepthPrepassShaderSource ) - 1, "DepthPrepass",
        nullptr, nullptr, "VSWorld", Shadermodel_VS, compileFlags, 0, m_DepthPrepassWorldVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !CompileShaderD3D12( kDepthPrepassShaderSource, sizeof( kDepthPrepassShaderSource ) - 1, "DepthPrepass",
        nullptr, nullptr, "PSClip", Shadermodel_PS, compileFlags, 0, m_DepthPrepassPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // Only Position (@0) + TexCoord0 (@20) from the packed 36-byte ExVertexStructGPU (stride comes from the VBV).
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_WorldRootSig.Get();
    pso.VS = { m_DepthPrepassWorldVsBlob->GetBufferPointer(), m_DepthPrepassWorldVsBlob->GetBufferSize() };
    pso.PS = { m_DepthPrepassPsBlob->GetBufferPointer(), m_DepthPrepassPsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // Keep NumRenderTargets=1 with the HDR scene-color format so the PSO matches the RTV bound during the world
    // pass (OnStartWorldRendering) — but mask off all color writes so only depth is touched.
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // match the world PSO's winding treatment
    pso.RasterizerState.DepthClipEnable = TRUE;

    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;   // DEPTH ONLY — discard color

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;   // reversed-Z
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_DepthPrepassWorldPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (depth prepass).";
        return false;
    }

    // Instanced-VOB depth prepass PSO (P2.9b-4a): same depth-only state, but the VOB two-stream input layout
    // (packed vertex slot 0 + per-instance world matrix slot 1) and the VOB shader's VSDepth/PSDepthClip.
    if ( !CompileShaderD3D12( kVobShaderSource, sizeof( kVobShaderSource ) - 1, "VobShader",
        nullptr, nullptr, "VSDepth", Shadermodel_VS, compileFlags, 0, m_DepthPrepassVobVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !CompileShaderD3D12( kVobShaderSource, sizeof( kVobShaderSource ) - 1, "VobShader",
        nullptr, nullptr, "PSDepthClip", Shadermodel_PS, compileFlags, 0, m_DepthPrepassVobPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // Position (@0) + TexCoord0 (@20) from the packed vertex (slot 0); the 4 instance world-matrix rows (slot 1).
    // Normal + instance color are dropped — VSDepth doesn't read them.
    const D3D12_INPUT_ELEMENT_DESC vobLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };

    // pso still carries the depth-only state (color mask 0, GREATER_EQUAL depth-write) — only swap VS/PS/layout.
    pso.VS = { m_DepthPrepassVobVsBlob->GetBufferPointer(), m_DepthPrepassVobVsBlob->GetBufferSize() };
    pso.PS = { m_DepthPrepassVobPsBlob->GetBufferPointer(), m_DepthPrepassVobPsBlob->GetBufferSize() };
    pso.InputLayout = { vobLayout, _countof( vobLayout ) };
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_DepthPrepassVobPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB depth prepass).";
        return false;
    }
    return true;
}

bool D3D12GraphicsEngine::CreateShadowMap() {
    // CSM sun shadow map (P2.9c-1): a Texture2DArray of kShadowCascades D32 slices + a caster PSO. Reuses the
    // depth-prepass world VS (b0 = a view-proj, t0 diffuse for alpha-clip) but with NORMAL-Z (LESS_EQUAL, clear
    // 1.0) state — the directional caster is NOT reversed-Z (mirrors the D3D11 shadow map). Created once at init
    // (fixed resolution, not swapchain-sized). Needs the depth-prepass shaders + m_WorldRootSig to exist.
    ID3D12Device* device = m_Device.GetDevice();
    if ( !m_WorldRootSig || !m_DepthPrepassWorldVsBlob ) return false;

    // Resolution from the shared quality setting (same knob D3D11 uses), clamped to a sane range. Bigger = smaller
    // world-units/texel = far less sub-texel foliage flicker + tighter near shadows. DEFAULT-heap (GPU) memory, so
    // 4096 (~192MB across 3 D32 slices) barely touches the 32-bit CPU address space.
    int desired = Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize;
    m_ShadowMapSize = static_cast<UINT>( std::min( std::max( desired, 1024 ), 4096 ) );

    D3D12_HEAP_PROPERTIES heapDefault = {};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = m_ShadowMapSize;
    dd.Height = m_ShadowMapSize;
    dd.DepthOrArraySize = static_cast<UINT16>( kShadowCascades );
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_R32_TYPELESS;   // D32 DSV per slice + one R32_FLOAT array SRV for the lit-pass sampler
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;        // normal-Z: 1.0 == far (NOT reversed-Z)

    // Born in DEPTH_WRITE; each frame RenderSunShadows writes then transitions to PIXEL_SHADER_RESOURCE and back.
    if ( FAILED( device->CreateCommittedResource( &heapDefault, D3D12_HEAP_FLAG_NONE, &dd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS( m_ShadowMap.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_ShadowMap->SetName( L"SunShadowMap(D32 array)" );
    m_ShadowInPixelState = false;

    // DSV heap: one D32 DSV per cascade slice.
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = kShadowCascades;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if ( FAILED( device->CreateDescriptorHeap( &dsvHeapDesc, IID_PPV_ARGS( m_ShadowDsvHeap.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_ShadowDsvSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );
    D3D12_CPU_DESCRIPTOR_HANDLE dsvH = m_ShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    for ( UINT c = 0; c < kShadowCascades; ++c ) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.FirstArraySlice = c;
        dsv.Texture2DArray.ArraySize = 1;
        device->CreateDepthStencilView( m_ShadowMap.Get(), &dsv, dsvH );
        dsvH.ptr += m_ShadowDsvSize;
    }

    // Array SRV (R32_FLOAT) covering all cascades — bound by the lit passes in a later increment.
    m_ShadowSrvSlot = AllocateSrvSlot();
    if ( m_ShadowSrvSlot == UINT_MAX ) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.ArraySize = kShadowCascades;
    device->CreateShaderResourceView( m_ShadowMap.Get(), &srv, GetSrvCpuHandle( m_ShadowSrvSlot ) );

    // Caster PSO. Void PS (PSShadowClip) so no RTV is needed; front-face cull + slope-scaled depth bias fight
    // shadow acne (front-culling casts back faces, standard for opaque shadow maps).
    UINT compileFlags = 0;
    if ( !CompileShaderD3D12( kDepthPrepassShaderSource, sizeof( kDepthPrepassShaderSource ) - 1, "DepthPrepass",
        nullptr, nullptr, "PSShadowClip", Shadermodel_PS, compileFlags, 0, m_ShadowCasterPsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_WorldRootSig.Get();
    pso.VS = { m_DepthPrepassWorldVsBlob->GetBufferPointer(), m_DepthPrepassWorldVsBlob->GetBufferSize() };
    pso.PS = { m_ShadowCasterPsBlob->GetBufferPointer(), m_ShadowCasterPsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 0;                    // depth-only shadow pass (no color target bound)
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;   // cast back faces
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.DepthBias = 0;                   // normal-Z: positive bias pushes casters away from the light
    pso.RasterizerState.SlopeScaledDepthBias = 0.0f;
    pso.RasterizerState.DepthBiasClamp = 0.0f;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;   // normal-Z
    pso.DepthStencilState.StencilEnable = FALSE;
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_ShadowCasterWorldPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (shadow caster).";
        return false;
    }

    // VOB caster PSO (P2.9c-2): reuse the VOB depth-prepass VSDepth (two-stream: packed vertex + per-instance
    // world matrix) + m_WorldRootSig, with the same caster state (front cull, bias, LESS_EQUAL, no RTV). Also
    // used for node attachments (weapons/heads) which are packed vertex + instance like ordinary VOBs.
    if ( m_DepthPrepassVobVsBlob ) {
        if ( !CompileShaderD3D12( kVobShaderSource, sizeof( kVobShaderSource ) - 1, "VobShader",
            nullptr, nullptr, "PSShadowClip", Shadermodel_PS, compileFlags, 0, m_ShadowCasterVobPsBlob.ReleaseAndGetAddressOf() ) )
            return false;
        const D3D12_INPUT_ELEMENT_DESC vobLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        };
        pso.pRootSignature = m_WorldRootSig.Get();
        pso.VS = { m_DepthPrepassVobVsBlob->GetBufferPointer(), m_DepthPrepassVobVsBlob->GetBufferSize() };
        pso.PS = { m_ShadowCasterVobPsBlob->GetBufferPointer(), m_ShadowCasterVobPsBlob->GetBufferSize() };
        pso.InputLayout = { vobLayout, _countof( vobLayout ) };
        if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_ShadowCasterVobPSO.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB shadow caster).";
            return false;
        }
    }

    // Skeletal caster PSO (P2.9c-2): reuse the skeletal depth-prepass VSDepth (matrix-palette skinning) +
    // m_SkeletalRootSig + the skinned input layout, same caster state.
    if ( m_DepthPrepassSkeletalVsBlob && m_SkeletalRootSig ) {
        if ( !CompileShaderD3D12( kSkeletalShaderSource, sizeof( kSkeletalShaderSource ) - 1, "SkeletalShader",
            nullptr, nullptr, "PSShadowClip", Shadermodel_PS, compileFlags, 0, m_ShadowCasterSkeletalPsBlob.ReleaseAndGetAddressOf() ) )
            return false;
        const D3D12_INPUT_ELEMENT_DESC skelLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "POSITION", 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0,  8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "POSITION", 2, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "POSITION", 3, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BONEIDS",  0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 64, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "WEIGHTS",  0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 68, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        pso.pRootSignature = m_SkeletalRootSig.Get();
        pso.VS = { m_DepthPrepassSkeletalVsBlob->GetBufferPointer(), m_DepthPrepassSkeletalVsBlob->GetBufferSize() };
        pso.PS = { m_ShadowCasterSkeletalPsBlob->GetBufferPointer(), m_ShadowCasterSkeletalPsBlob->GetBufferSize() };
        pso.InputLayout = { skelLayout, _countof( skelLayout ) };
        if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_ShadowCasterSkeletalPSO.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: CreateGraphicsPipelineState failed (skeletal shadow caster).";
            return false;
        }
    }

    // Per-frame-in-flight shadow-sampling CB (b3 in the lit passes): cascade view-projs + sun dir + strength +
    // texel sizes. Small + written once per frame, so a persistently-mapped UPLOAD buffer per frame context
    // (no ring offset needed — one struct per frame). Filled in RenderSunShadows, bound by the lit draws.
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = 256;   // one 256-aligned CB (cascade matrices + sun data fit well under 256B)
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( m_ShadowCB[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_ShadowCB[i]->SetName( L"ShadowSamplingCB" );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( m_ShadowCB[i]->Map( 0, &noRead, &mapped ) ) ) return false;
        m_ShadowCBMapped[i] = static_cast<uint8_t*>( mapped );
        m_ShadowCBGpu[i] = m_ShadowCB[i]->GetGPUVirtualAddress();
    }
    return true;
}

bool D3D12GraphicsEngine::CreatePointShadowCubes() {
    // P2.10a: the shared point-light shadow cube ARRAY + its single-pass instanced world caster PSO. Mirrors
    // D3D11's Forward+ TextureCubeArray (SHADOW_CUBE_SIZE 128, MAX_SHADOW_CUBEMAPS shared slots, R16 depth,
    // NORMAL-Z). Nothing renders/samples yet (that's P2.10b/d) — this only builds the resource + pipeline, so
    // it's compile-safe and inert. Non-fatal at init: on failure the point lights simply stay unshadowed.
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    // --- Root signature: b0 = the 6 face view-projs as a root CBV (VS); t0 = diffuse SRV table (PS alpha-clip);
    // static linear sampler s0. (b0 is a CBV not root consts — 6 matrices = 384B exceed the root-const budget.)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;   // t0
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;   // b0 PCR_ViewProj[6]
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;   // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof( params );
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (point shadows)."; return false; }
    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: point-shadow root sig error: " << static_cast<const char*>( rsErr->GetBufferPointer() );
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( m_PointShadowRootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    // --- Caster shader: single-pass 6-face via instancing. instanceID (0..5) picks the face view-proj AND is
    // written to SV_RenderTargetArrayIndex to route the primitive to that cube face slice (no geometry shader —
    // needs VS-stage RT-array-index support, present on the target AMD GPU). World verts are already world-space.
    constexpr char kPointShadowShaderSource[] = R"(
cbuffer CubeCB : register(b0) { float4x4 PCR_ViewProj[6]; };   // per-light face view-projs (90-deg perspective, near 15, far range*2)
// Skeletal-only CBs (b1/b2). Unreferenced by VSCube/VSCubeVob, so they're stripped from those shaders — the
// world/VOB caster PSOs' root sig only declares b0/t0/s0. They match the main skeletal InstanceCB/BonesCB so
// the cube caster can bind the SAME per-frame instance/bone CBs (d.instCb/d.boneCb) the sun/color passes use.
cbuffer SkelInstanceCB : register(b1) { float4x4 M_World; float4 ModelColor; float Fatness; float3 _spad; };
cbuffer SkelBonesCB    : register(b2) { float4x4 Bones[96]; };
Texture2D    tx  : register(t0);
SamplerState smp : register(s0);
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; uint rt : SV_RenderTargetArrayIndex; };

// World caster: one draw = 6 instances, instanceID selects the face view-proj AND the target cube slice.
struct VS_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; uint iid : SV_InstanceID; };
VS_OUT VSCube( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), PCR_ViewProj[i.iid] );   // 90-deg perspective from the light, per face
    o.uv   = i.uv;
    o.rt   = i.iid;   // face 0..5 → cube slice (relative to the bound slot's 6-slice DSV)
    return o;
}

// VOB caster: instanceID spans (numInstances * 6). The per-instance world stream uses InstanceDataStepRate=6, so
// each real instance is fetched for 6 consecutive instanceIDs; face = iid % 6 picks the face view-proj + slice.
struct VSVOB_IN { float3 pos : POSITION; float2 uv : TEXCOORD0; float4x4 iworld : INSTANCE_WORLD_MATRIX; uint iid : SV_InstanceID; };
VS_OUT VSCubeVob( VSVOB_IN i )
{
    VS_OUT o;
    uint   face = i.iid % 6u;
    float3 wp   = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;
    o.clip = mul( float4( wp, 1.0 ), PCR_ViewProj[face] );
    o.uv   = i.uv;
    o.rt   = face;
    return o;
}

// Skeletal caster: 6 instances → face = iid. Matrix-palette skin (matches the main/sun VSDepth incl. Fatness) so
// the cast depth is bit-consistent with the color pass, then project through the face's 90-deg view-proj.
struct VSSKEL_IN { float4 pos[4] : POSITION; float3 normal : NORMAL; float3 bindPoseNormal : TEXCOORD0; float2 uv : TEXCOORD1; uint4 boneIndices : BONEIDS; float4 weights : WEIGHTS; uint iid : SV_InstanceID; };
VS_OUT VSCubeSkel( VSSKEL_IN i )
{
    float3 sp = float3( 0, 0, 0 );
    float3 sn = float3( 0, 0, 0 );
    [unroll]
    for ( int b = 0; b < 4; ++b )
    {
        float4x4 bone = Bones[i.boneIndices[b]];
        float    w    = i.weights[b];
        sp += w * mul( float4( i.pos[b].xyz, 1.0 ), bone ).xyz;
        sn += w * mul( i.normal, (float3x3)bone );
    }
    float3 wp = mul( float4( sp + Fatness * sn, 1.0 ), M_World ).xyz;
    VS_OUT o;
    o.clip = mul( float4( wp, 1.0 ), PCR_ViewProj[i.iid] );
    o.uv   = i.uv;
    o.rt   = i.iid;
    return o;
}

// Depth-only (void PS, no SV_Depth) so the caster keeps early-Z / Hi-Z rejection and the PSO's hardware slope-
// scaled depth bias — the stored depth is the NATURAL hyperbolic z of the 90-deg perspective, which the sampler
// reconstructs from the dominant-axis distance (more efficient than writing linear SV_Depth). Alpha-clip cutouts.
void PSCubeClip( VS_OUT i ) { clip( tx.Sample( smp, i.uv ).a - 0.5 ); }
)";
    UINT compileFlags = 0;
    if ( !CompileShaderD3D12( kPointShadowShaderSource, sizeof( kPointShadowShaderSource ) - 1, "PointShadow",
        nullptr, nullptr, "VSCube", Shadermodel_VS, compileFlags, 0, m_PointShadowVsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !CompileShaderD3D12( kPointShadowShaderSource, sizeof( kPointShadowShaderSource ) - 1, "PointShadow",
        nullptr, nullptr, "VSCubeVob", Shadermodel_VS, compileFlags, 0, m_PointShadowVobVsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !CompileShaderD3D12( kPointShadowShaderSource, sizeof( kPointShadowShaderSource ) - 1, "PointShadow",
        nullptr, nullptr, "VSCubeSkel", Shadermodel_VS, compileFlags, 0, m_PointShadowSkelVsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !CompileShaderD3D12( kPointShadowShaderSource, sizeof( kPointShadowShaderSource ) - 1, "PointShadow",
        nullptr, nullptr, "PSCubeClip", Shadermodel_PS, compileFlags, 0, m_PointShadowPsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    // --- Cube array resource: Texture2DArray with kMaxShadowCubes*6 R16 slices (interpreted as a TextureCubeArray
    // by the SRV). NORMAL-Z depth (clear 1.0, LESS_EQUAL). Born in DEPTH_WRITE (RenderPointShadows flips it to
    // PIXEL_SHADER_RESOURCE for the lit pass and back next frame).
    D3D12_HEAP_PROPERTIES heapDefault = {};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width  = kPointShadowCubeSize;
    dd.Height = kPointShadowCubeSize;
    dd.DepthOrArraySize = static_cast<UINT16>( kMaxShadowCubes * 6 );
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_R16_TYPELESS;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D16_UNORM;
    clear.DepthStencil.Depth = 1.0f;
    if ( FAILED( device->CreateCommittedResource( &heapDefault, D3D12_HEAP_FLAG_NONE, &dd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS( m_PointShadowCube.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_PointShadowCube->SetName( L"PointShadowCubeArray(D16)" );
    m_PointShadowInPixelState = false;

    // One DSV per cube slot: a 6-slice Texture2DArray view (FirstArraySlice = slot*6). SV_RenderTargetArrayIndex
    // 0..5 from the VS then selects the face within the bound slot.
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = kMaxShadowCubes;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if ( FAILED( device->CreateDescriptorHeap( &dsvHeapDesc, IID_PPV_ARGS( m_PointShadowDsvHeap.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_PointShadowDsvSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );
    D3D12_CPU_DESCRIPTOR_HANDLE dsvH = m_PointShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    for ( UINT s = 0; s < kMaxShadowCubes; ++s ) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D16_UNORM;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.FirstArraySlice = s * 6;
        dsv.Texture2DArray.ArraySize = 6;
        device->CreateDepthStencilView( m_PointShadowCube.Get(), &dsv, dsvH );
        dsvH.ptr += m_PointShadowDsvSize;
    }

    // --- Static-aside cube (P2.10g): a SECOND identical cube array holding static-caster depth only. Same desc
    // (so CopyResource into the active cube is legal), born in DEPTH_WRITE, with its own per-slot 6-slice DSV heap.
    // No SRV — it's never sampled; its depth is copied into the active cube each frame before the dynamic overlay.
    if ( FAILED( device->CreateCommittedResource( &heapDefault, D3D12_HEAP_FLAG_NONE, &dd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS( m_PointShadowStaticCube.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_PointShadowStaticCube->SetName( L"PointShadowStaticCubeArray(D16)" );
    m_PointShadowStaticState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    D3D12_DESCRIPTOR_HEAP_DESC staticDsvHeapDesc = {};
    staticDsvHeapDesc.NumDescriptors = kMaxShadowCubes;
    staticDsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if ( FAILED( device->CreateDescriptorHeap( &staticDsvHeapDesc, IID_PPV_ARGS( m_PointShadowStaticDsvHeap.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    D3D12_CPU_DESCRIPTOR_HANDLE sdsvH = m_PointShadowStaticDsvHeap->GetCPUDescriptorHandleForHeapStart();
    for ( UINT s = 0; s < kMaxShadowCubes; ++s ) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D16_UNORM;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.FirstArraySlice = s * 6;
        dsv.Texture2DArray.ArraySize = 6;
        device->CreateDepthStencilView( m_PointShadowStaticCube.Get(), &dsv, sdsvH );
        sdsvH.ptr += m_PointShadowDsvSize;
    }

    // TextureCubeArray SRV (R16_UNORM) over all cubes — sampled by the tiled point-light loop in P2.10d.
    m_PointShadowSrvSlot = AllocateSrvSlot();
    if ( m_PointShadowSrvSlot == UINT_MAX ) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R16_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCubeArray.MipLevels = 1;
    srv.TextureCubeArray.NumCubes = kMaxShadowCubes;
    device->CreateShaderResourceView( m_PointShadowCube.Get(), &srv, GetSrvCpuHandle( m_PointShadowSrvSlot ) );

    // Caster PSO. Single stream: Position + TexCoord0 from the packed 36-byte world vertex. Depth-only (no RTV),
    // NORMAL-Z LESS_EQUAL, CULL_NONE (D3D11 renders cubes with cullFront=false; bias is applied at sample time).
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_PointShadowRootSig.Get();
    pso.VS = { m_PointShadowVsBlob->GetBufferPointer(), m_PointShadowVsBlob->GetBufferSize() };
    pso.PS = { m_PointShadowPsBlob->GetBufferPointer(), m_PointShadowPsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 0;
    pso.DSVFormat = DXGI_FORMAT_D16_UNORM;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.DepthBias = 100;                 // hardware depth bias (hyperbolic depth) to fight acne
    pso.RasterizerState.SlopeScaledDepthBias = 2.0f;     // — free with early-Z, unlike an in-shader bias
    pso.RasterizerState.DepthBiasClamp = 0.0f;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;   // normal-Z
    pso.DepthStencilState.StencilEnable = FALSE;
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_PointShadowCasterWorldPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (point-shadow world caster).";
        return false;
    }

    // --- VOB caster PSO (P2.10e): same root sig (per-instance world rides the vertex stream, not the root) + the
    // same caster state, but VSCubeVob and a two-stream layout whose instance rows carry InstanceDataStepRate=6 —
    // so one real instance is fetched for 6 consecutive instanceIDs and each renders to one cube face. The instance
    // stream is a TIGHT 64-byte world matrix (packed by RenderPointShadows; not the full 144B VobInstanceInfo).
    {
        const D3D12_INPUT_ELEMENT_DESC vobLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 6 },
            { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 6 },
            { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 6 },
            { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 6 },
        };
        pso.pRootSignature = m_PointShadowRootSig.Get();
        pso.VS = { m_PointShadowVobVsBlob->GetBufferPointer(), m_PointShadowVobVsBlob->GetBufferSize() };
        pso.PS = { m_PointShadowPsBlob->GetBufferPointer(), m_PointShadowPsBlob->GetBufferSize() };
        pso.InputLayout = { vobLayout, _countof( vobLayout ) };
        if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_PointShadowCasterVobPSO.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: CreateGraphicsPipelineState failed (point-shadow VOB caster).";
            return false;
        }
    }

    // --- Skeletal caster: needs a dedicated root sig (b0 = 6 face view-projs CBV, b1 = instance, b2 = bones, all
    // VS; t0 diffuse table + s0 for the alpha cutout). Mirrors the sun path's skeletal binds but with the 6-matrix
    // face CBV at b0 instead of the single-matrix root const. Reuses the per-frame d.instCb/d.boneCb.
    {
        D3D12_DESCRIPTOR_RANGE skelSrvRange = {};
        skelSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        skelSrvRange.NumDescriptors = 1;
        skelSrvRange.BaseShaderRegister = 0;   // t0
        skelSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER skelParams[4] = {};
        skelParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        skelParams[0].Descriptor.ShaderRegister = 0;   // b0 PCR_ViewProj[6]
        skelParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        skelParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        skelParams[1].Descriptor.ShaderRegister = 1;   // b1 instance (M_World/Fatness)
        skelParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        skelParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        skelParams[2].Descriptor.ShaderRegister = 2;   // b2 bones
        skelParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        skelParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        skelParams[3].DescriptorTable.NumDescriptorRanges = 1;
        skelParams[3].DescriptorTable.pDescriptorRanges = &skelSrvRange;
        skelParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC skelDesc = {};
        skelDesc.NumParameters = _countof( skelParams );
        skelDesc.pParameters = skelParams;
        skelDesc.NumStaticSamplers = 1;
        skelDesc.pStaticSamplers = &sampler;
        skelDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> skelBlob, skelErr;
        if ( FAILED( serialize( &skelDesc, D3D_ROOT_SIGNATURE_VERSION_1, skelBlob.GetAddressOf(), skelErr.GetAddressOf() ) ) ) {
            if ( skelErr ) LogWarn() << "D3D12: point-shadow skeletal root sig error: " << static_cast<const char*>( skelErr->GetBufferPointer() );
            return false;
        }
        if ( FAILED( device->CreateRootSignature( 0, skelBlob->GetBufferPointer(), skelBlob->GetBufferSize(),
            IID_PPV_ARGS( m_PointShadowSkeletalRootSig.ReleaseAndGetAddressOf() ) ) ) )
            return false;

        const D3D12_INPUT_ELEMENT_DESC skelLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "POSITION", 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0,  8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "POSITION", 2, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "POSITION", 3, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BONEIDS",  0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 64, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "WEIGHTS",  0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 68, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        pso.pRootSignature = m_PointShadowSkeletalRootSig.Get();
        pso.VS = { m_PointShadowSkelVsBlob->GetBufferPointer(), m_PointShadowSkelVsBlob->GetBufferSize() };
        pso.PS = { m_PointShadowPsBlob->GetBufferPointer(), m_PointShadowPsBlob->GetBufferSize() };
        pso.InputLayout = { skelLayout, _countof( skelLayout ) };
        if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_PointShadowCasterSkeletalPSO.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: CreateGraphicsPipelineState failed (point-shadow skeletal caster).";
            return false;
        }
    }

    // Per-frame ring for the face-matrix CB: one 512-byte (256-aligned; 6 matrices = 384B) slot per shadowed
    // light, so each light's cube draw binds its own root CBV without clobbering earlier same-frame draws.
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width  = static_cast<UINT64>( kMaxShadowCubes ) * 512;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( m_PointShadowCB[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_PointShadowCB[i]->SetName( L"PointShadowFaceCB" );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( m_PointShadowCB[i]->Map( 0, &noRead, &mapped ) ) ) return false;
        m_PointShadowCBMapped[i] = static_cast<uint8_t*>( mapped );
        m_PointShadowCBGpu[i] = m_PointShadowCB[i]->GetGPUVirtualAddress();
    }

    // Per-frame TIGHT VOB-instance ring for the point-shadow VOB caster (P2.10e). RenderPointShadows range-culls
    // each visible VOB's instances against every shadowed light and packs the in-range ones' 64-byte world matrix
    // here (only the near casters, not the whole visible set) — so the cube pass draws proportional to actual
    // nearby geometry. Persistently mapped UPLOAD; offset reset at the top of RenderPointShadows; drop+log on
    // overflow (never reallocates — see the 32-bit per-frame-allocation rule).
    m_PointShadowVobInstCapacity = static_cast<UINT>( kPointShadowMaxVobInstances ) * sizeof( XMFLOAT4X4 );
    D3D12_RESOURCE_DESC viDesc = {};
    viDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    viDesc.Width  = m_PointShadowVobInstCapacity;
    viDesc.Height = 1;
    viDesc.DepthOrArraySize = 1;
    viDesc.MipLevels = 1;
    viDesc.Format = DXGI_FORMAT_UNKNOWN;
    viDesc.SampleDesc.Count = 1;
    viDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &viDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( m_PointShadowVobInst[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_PointShadowVobInst[i]->SetName( L"PointShadowVobInstRing" );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( m_PointShadowVobInst[i]->Map( 0, &noRead, &mapped ) ) ) return false;
        m_PointShadowVobInstPtr[i] = static_cast<uint8_t*>( mapped );
        m_PointShadowVobInstGpu[i] = m_PointShadowVobInst[i]->GetGPUVirtualAddress();
    }
    return true;
}

void D3D12GraphicsEngine::ComputeCascadeMatrices() {
    using namespace DirectX;
    // P2.9c-3a: stable, frustum-fit + texel-snapped cascades — mirrors D3D11 CalculateCascadeMatrices
    // (D3D11ShadowMap.cpp). Per cascade: fit a bounding SPHERE to the camera's frustum SLICE [splitNear,splitFar]
    // (rotation-invariant → no shimmer from turning), quantize the radius, snap the sphere centre to the shadow
    // texel grid anchored at the world origin (→ no crawl when translating), pull the light back, and derive the
    // ortho Z bounds from the slice + the scene BBox. Replaces the old camera-centred concentric boxes.
    Engine::GAPI->GetSky()->RenderSky(); // <-- does not render, but calculates atmosphere data like AC_LightPos

    float3 lp = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos;
    XMVECTOR rawToSun = XMVector3Normalize( XMVectorSet( lp.x, lp.y, lp.z, 0.0f ) );
    // Temporal smoothing (P2.9c-3c): lerp toward the live sun dir so the origin-anchored snap grid rotates
    // gradually instead of jittering per frame (the lever arm from origin to a distant player turns tiny
    // sun drift into visible texel crawl). alpha small = strong smoothing; the day cycle is minutes-long so
    // a ~1s time constant lags imperceptibly while killing per-frame jitter.
    XMVECTOR toSun;
    if ( !m_SunDirInitialized ) {
        toSun = rawToSun;
        m_SunDirInitialized = true;
    } else {
        constexpr float alpha = 0.03f;
        toSun = XMVector3Normalize( XMVectorLerp( XMLoadFloat3( &m_SmoothedSunDir ), rawToSun, alpha ) );
    }
    XMStoreFloat3( &m_SmoothedSunDir, toSun );
    XMStoreFloat3( &m_SunDirWS, toSun );   // world-space dir TOWARD the sun (for the lit-pass N.L term)
    const XMVECTOR lightDir = XMVectorNegate( toSun );   // sun -> scene (the caster's look direction)
    const XMVECTOR worldUp  = XMVectorSet( 0, 1, 0, 0 );
    const XMVECTOR up = ( fabsf( lp.y ) > 0.95f ) ? XMVectorSet( 0, 0, 1, 0 ) : worldUp;

    // Camera basis for reconstructing world-space frustum-slice corners: inverse(view) is camera->world, and the
    // projection diagonal gives the half-angle scales (_11 = 1/tan(fovX/2), _22 = 1/tan(fovY/2)). GothicAPI's
    // getters are column-major but the proj DIAGONAL is transpose-invariant, so we read _11/_22 straight off it.
    const XMMATRIX viewStd = XMMatrixTranspose( Engine::GAPI->GetViewMatrixXM() );   // row-vector standard view
    const XMMATRIX invView = XMMatrixInverse( nullptr, viewStd );
    const XMFLOAT4X4& projCM = Engine::GAPI->GetProjectionMatrix();
    const float projXScale = projCM._11;
    const float projYScale = projCM._22;

    // Practical split scheme (blend of uniform + logarithmic), from shadowNear..shadowFar in world units.
    const float shadowNear = 15.0f;
    const float shadowFar  = 20000.0f;
    const float lambda     = 0.85f;
    float splits[kShadowCascades + 1];
    splits[0] = shadowNear;
    splits[kShadowCascades] = shadowFar;
    for ( UINT i = 1; i < kShadowCascades; ++i ) {
        float p    = static_cast<float>( i ) / static_cast<float>( kShadowCascades );
        float logS = shadowNear * powf( shadowFar / shadowNear, p );
        float uniS = shadowNear + ( shadowFar - shadowNear ) * p;
        splits[i]  = uniS + lambda * ( logS - uniS );
    }

    // Scene-BBox light-space Z extent (tightens the ortho depth so casters between the light and the slice are
    // captured without shooting miles past the level). Recomputed per cascade against that cascade's lightView.
    zTBBox3D sceneBox = {};
    bool haveScene = false;
    if ( auto wi = Engine::GAPI->GetLoadedWorldInfo() )
        if ( wi->BspTree && wi->BspTree->GetRootNode() ) { sceneBox = wi->BspTree->GetRootNode()->BBox3D; haveScene = true; }

    const float lightDotUp = std::max( fabsf( XMVectorGetX( XMVector3Dot( lightDir, worldUp ) ) ), 0.05f );
    const float dynamicPullback = std::clamp( 4000.0f / lightDotUp, 2000.0f, 15000.0f );

    for ( UINT c = 0; c < kShadowCascades; ++c ) {
        // 8 world-space corners of the camera frustum slice [splits[c], splits[c+1]].
        XMFLOAT3 corners[8];
        int ci = 0;
        for ( int f = 0; f < 2; ++f ) {
            float d = splits[c + f];
            float xe = d / projXScale, ye = d / projYScale;
            for ( int sy = -1; sy <= 1; sy += 2 )
                for ( int sx = -1; sx <= 1; sx += 2 ) {
                    XMVECTOR vVS = XMVectorSet( sx * xe, sy * ye, d, 1.0f );
                    XMStoreFloat3( &corners[ci++], XMVector3TransformCoord( vVS, invView ) );
                }
        }

        // Minimal bounding sphere of the slice: slide the centre along the near->far axis so near/far radii equal.
        XMVECTOR nearC = XMVectorZero(), farC = XMVectorZero();
        for ( int i = 0; i < 4; ++i ) nearC += XMLoadFloat3( &corners[i] );
        for ( int i = 4; i < 8; ++i ) farC  += XMLoadFloat3( &corners[i] );
        nearC *= 0.25f; farC *= 0.25f;
        XMVECTOR axis = XMVectorSubtract( farC, nearC );
        float L = XMVectorGetX( XMVector3Length( axis ) );
        XMVECTOR viewDir = ( L > 1e-4f ) ? XMVectorScale( axis, 1.0f / L ) : lightDir;
        float nearRSq = XMVectorGetX( XMVector3LengthSq( XMVectorSubtract( XMLoadFloat3( &corners[0] ), nearC ) ) );
        float farRSq  = XMVectorGetX( XMVector3LengthSq( XMVectorSubtract( XMLoadFloat3( &corners[4] ), farC ) ) );
        float optimalX = std::clamp( ( L * L + farRSq - nearRSq ) / std::max( 2.0f * L, 1e-4f ), 0.0f, L );
        XMVECTOR frustumCenter = XMVectorAdd( nearC, XMVectorScale( viewDir, optimalX ) );

        float radius = 0.0f;
        for ( int i = 0; i < 8; ++i )
            radius = std::max( radius, XMVectorGetX( XMVector3Length( XMVectorSubtract( XMLoadFloat3( &corners[i] ), frustumCenter ) ) ) );
        radius = std::ceil( radius * 16.0f ) / 16.0f;   // quantize → no micro-scaling from FOV/aspect rounding
        const float cascadeSize = radius * 2.0f;
        const float texelSize   = cascadeSize / static_cast<float>( m_ShadowMapSize );
        m_CascadeTexelWorld[c]  = texelSize;   // world units/texel → the lit-pass normal bias

        // Texel-snap the centre on a GLOBAL light-space grid anchored at the world origin (unmoving as the player
        // translates), then transform back to world.
        XMMATRIX gridView = XMMatrixLookToLH( XMVectorZero(), lightDir, up );
        XMVECTOR cLS = XMVector3TransformCoord( frustumCenter, gridView );
        float snapX = std::floor( XMVectorGetX( cLS ) / texelSize ) * texelSize;
        float snapY = std::floor( XMVectorGetY( cLS ) / texelSize ) * texelSize;
        XMVECTOR snappedLS = XMVectorSet( snapX, snapY, XMVectorGetZ( cLS ), 1.0f );
        XMVECTOR snappedWS = XMVector3TransformCoord( snappedLS, XMMatrixInverse( nullptr, gridView ) );

        const float pullBack = std::max( 10000.0f, radius * 2.0f );
        XMVECTOR lightPos = XMVectorSubtract( snappedWS, XMVectorScale( lightDir, pullBack ) );
        XMMATRIX lightView = XMMatrixLookToLH( lightPos, lightDir, up );

        // Ortho Z from the slice corners' light-space depth, widened by the dynamic (sun-angle) pullback and the
        // scene BBox so occluders above/behind the slice still lie within the depth range.
        float minZ = FLT_MAX, maxZ = -FLT_MAX;
        for ( int i = 0; i < 8; ++i ) {
            float z = XMVectorGetZ( XMVector3TransformCoord( XMLoadFloat3( &corners[i] ), lightView ) );
            minZ = std::min( minZ, z ); maxZ = std::max( maxZ, z );
        }
        float orthoNear = std::max( 1.0f, minZ - dynamicPullback );
        float orthoFar  = maxZ + 5000.0f;
        if ( haveScene ) {
            const XMFLOAT3 sc[8] = {
                { sceneBox.Min.x, sceneBox.Min.y, sceneBox.Min.z }, { sceneBox.Max.x, sceneBox.Min.y, sceneBox.Min.z },
                { sceneBox.Min.x, sceneBox.Max.y, sceneBox.Min.z }, { sceneBox.Max.x, sceneBox.Max.y, sceneBox.Min.z },
                { sceneBox.Min.x, sceneBox.Min.y, sceneBox.Max.z }, { sceneBox.Max.x, sceneBox.Min.y, sceneBox.Max.z },
                { sceneBox.Min.x, sceneBox.Max.y, sceneBox.Max.z }, { sceneBox.Max.x, sceneBox.Max.y, sceneBox.Max.z } };
            float sMinZ = FLT_MAX, sMaxZ = -FLT_MAX;
            for ( int i = 0; i < 8; ++i ) {
                float z = XMVectorGetZ( XMVector3TransformCoord( XMLoadFloat3( &sc[i] ), lightView ) );
                sMinZ = std::min( sMinZ, z ); sMaxZ = std::max( sMaxZ, z );
            }
            orthoNear = std::min( orthoNear, sMinZ - 100.0f );
            orthoFar  = std::min( orthoFar,  sMaxZ + 500.0f );
        }
        orthoNear = std::max( 1.0f, orthoNear );
        if ( orthoFar <= orthoNear + 1.0f ) orthoFar = orthoNear + 1.0f;

        XMMATRIX proj = XMMatrixOrthographicLH( cascadeSize, cascadeSize, orthoNear, orthoFar );
        // Store (View*Proj)^T (see the c-1 convention note): our lightView/proj are standard row-vector matrices,
        // so we transpose the product to match the column-major bytes the caster VS + sampling PS read back.
        XMStoreFloat4x4( &m_CascadeViewProj[c], XMMatrixTranspose( XMMatrixMultiply( lightView, proj ) ) );
        
        m_CascadeFrustum[c].BuildOrthographic( lightView,
            cascadeSize,
            cascadeSize,
            orthoNear,
            orthoFar,
            Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendBack,
            Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendFront,
            Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendSide );
    }
}

void D3D12GraphicsEngine::RenderSunShadows() {
    // P2.9c-1/-2: render the opaque casters (world mesh + instanced VOBs + skinned skeletals + node attachments)
    // into each cascade slice from the sun's POV. Still PRODUCES the shadow map only — nothing samples it yet, so
    // the frame is visually unchanged; inspect the D32 Texture2DArray in RenderDoc (each slice should show the
    // scene depth from the sun angle, now including VOB/NPC silhouettes). Stable cascades + the lit-pass PCF
    // sampling are later increments. Casters reuse the shared per-frame records built before the cull
    // (g_FrameVobUploads / g_FrameSkelDraws / g_FrameAttachDraws) — no second upload or animation update.
    if ( !m_FrameOpen || !m_ShadowMap || !m_ShadowCasterWorldPSO || !m_ShadowDsvHeap || !m_WorldRootSig )
        return;

    DX_ZONE( m_CmdList, "Sun Shadows (cascades)" );

    // Return the map to DEPTH_WRITE if last frame's (future) lit sampling left it in PIXEL_SHADER_RESOURCE.
    if ( m_ShadowInPixelState ) {
        auto toDepth = TransitionBarrier( m_ShadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE );
        m_CmdList->ResourceBarrier( 1, &toDepth );
        m_ShadowInPixelState = false;
    }

    ComputeCascadeMatrices();

    // Sun below the horizon → clear each slice to far (1.0 = unshadowed) and skip ALL casting.
    const float3 lp = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos;
    const bool sunUp = ( lp.y > 0.0f );

    // Upload this frame's shadow-sampling CB (b3 for the lit passes): cascade view-projs + sun dir + darkening
    // strength + per-cascade texel size. Layout MUST match the HLSL ShadowCB (row-major matrices, HLSL packing).
    if ( m_ShadowCBMapped[m_FrameIndex] ) {
        // Layout MUST match the HLSL ShadowCB (256B, row-major matrices). Stage-2 PBR sun params come from the
        // shared RendererSettings (same knobs D3D11 feeds SQ_LightColor/SQ_ShadowStrength/SQ_*AOStrength from).
        struct ShadowCBData {
            XMFLOAT4X4 CascadeViewProj[kShadowCascades];
            XMFLOAT3   SunDirWS;          float ShadowMapSize;
            XMFLOAT3   SunColor;          float SunIntensity;
            XMFLOAT3   CascadeTexelWorld; float AmbientStrength;
            float ShadowAOStrength; float WorldAOStrength; float _pad0; float _pad1;
        } cb;
        const auto& set = Engine::GAPI->GetRendererState().RendererSettings;
        for ( UINT c = 0; c < kShadowCascades; ++c ) cb.CascadeViewProj[c] = m_CascadeViewProj[c];
        cb.SunDirWS = m_SunDirWS;
        cb.ShadowMapSize = static_cast<float>( m_ShadowMapSize );
        cb.CascadeTexelWorld = XMFLOAT3( m_CascadeTexelWorld[0], m_CascadeTexelWorld[1], m_CascadeTexelWorld[2] );

        // Rain dims the sun toward RainSunLightStrength (parity with D3D11's SQ_LightColor.a lerp).
        const float rain = Engine::GAPI->GetRainFXWeight();
        const float sunStrength = set.SunLightStrength
            + ( set.RainSunLightStrength - set.SunLightStrength ) * std::min( 1.0f, rain * 2.0f );

        // Ambient/sky strength (SQ_ShadowStrength). Night is a bit brighter than before (0.3 -> 0.5, per user)
        // so interiors aren't too dark after dusk; interiors also self-darken via baked vertLighting-as-AO.
        float ambient = sunUp ? set.ShadowStrength : set.ShadowStrength * 0.5f;

        // BSP-indoor override (parity with D3D11): interiors use a NEUTRAL white sun (no warm outdoor tint) at a
        // softened intensity (no hard raking sun through a cave), and worldAO fully tracks the baked light. We keep
        // a non-zero ambient (D3D11 zeroes it for G2 -> torch-only) so interiors that already look fine don't go dark.
        bool indoor = false;
        if ( auto* wi = Engine::GAPI->GetLoadedWorldInfo() )
            if ( wi->BspTree )
                indoor = ( wi->BspTree->GetBspTreeMode() == zBSP_MODE_INDOOR );

        if ( indoor ) {
            cb.SunColor = XMFLOAT3( 1.0f, 1.0f, 1.0f );
            cb.SunIntensity = sunUp ? sunStrength * 0.5f : 0.0f;
            cb.AmbientStrength = ambient;
            cb.WorldAOStrength = 1.0f;
        } else {
            cb.SunColor = XMFLOAT3( set.SunLightColor.x, set.SunLightColor.y, set.SunLightColor.z );
            cb.SunIntensity = sunUp ? sunStrength : 0.0f;   // no direct sun when it's below the horizon
            cb.AmbientStrength = ambient;
            cb.WorldAOStrength = set.WorldAOStrength;
        }
        cb.ShadowAOStrength = set.ShadowAOStrength;
        memcpy( m_ShadowCBMapped[m_FrameIndex], &cb, sizeof( cb ) );
    }

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    D3D12VertexBuffer* vb = wm ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
    D3D12VertexBuffer* ib = wm ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;
    const bool haveWorld = vb && ib && vb->GetResource() && ib->GetResource()
                        && ( ib->GetSizeInBytes() / sizeof( uint32_t ) ) > 0;
    const bool haveSkel   = m_ShadowCasterSkeletalPSO && m_SkeletalRootSig && !g_FrameSkelDraws.empty();
    const bool haveAttach = m_ShadowCasterVobPSO && !g_FrameAttachDraws.empty();

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_ShadowMapSize ), static_cast<float>( m_ShadowMapSize ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, static_cast<LONG>( m_ShadowMapSize ), static_cast<LONG>( m_ShadowMapSize ) };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    // Resolve + bind a material's diffuse to a root descriptor-table slot (white fallback) for the alpha cutout.
    auto bindDiffuse = [&]( zCTexture* tex, UINT rootParam ) {
        D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
        if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
            if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
                if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                    D3D12Texture* d12 = D3D12Texture::From( gfx );
                    if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                }
            }
        }
        m_CmdList->SetGraphicsRootDescriptorTable( rootParam, srv );
    };
    
    D3D12_CPU_DESCRIPTOR_HANDLE dsvBase = m_ShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();    
    for ( UINT c = 0; c < kShadowCascades; ++c ) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvBase;
        dsv.ptr += static_cast<SIZE_T>( c ) * m_ShadowDsvSize;
        m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );   // DSV stays bound across the PSO switches below
        m_CmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );   // normal-Z far
        if ( !sunUp ) continue;   // still leaves a valid (unshadowed) slice

        // --- World mesh (root sig: m_WorldRootSig; b0 = cascade view-proj; t0 diffuse table @1) ---
        if ( haveWorld ) {
            DX_ZONE(m_CmdList, "World Mesh");
            
            static std::vector<WorldMeshSectionInfo*> sections;
            sections.clear();
            Engine::GAPI->CollectVisibleSections( sections, &m_CascadeFrustum[c], false );
            
            m_CmdList->SetPipelineState( m_ShadowCasterWorldPSO.Get() );
            m_CmdList->SetGraphicsRootSignature( m_WorldRootSig.Get() );
            m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );

            const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
            const D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
            m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
            m_CmdList->IASetIndexBuffer( &ibv );

            zCTexture* boundTex = nullptr;
            for ( WorldMeshSectionInfo* section : sections ) {
                if ( !section ) continue;
                for ( auto const& [meshKey, mesh] : section->WorldMeshes ) {
                    if ( !mesh || mesh->Indices.empty() ) continue;
                    if ( meshKey.Info && meshKey.Info->MaterialType != MaterialInfo::MT_None ) continue;

                    // frustum cull meshes inside the large sections. drastically reduces draw calls.
                    if ( !Engine::GAPI->IsWorldMeshVisibleInFrustum( mesh, m_CascadeFrustum[c] ) ) {
                        continue;
                    }

                    // we don't draw alpha stuff into shadowmaps.
                    if ( (meshKey.Material->GetAlphaFunc() > zMAT_ALPHA_FUNC_NONE &&
                        meshKey.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST)
                            || (meshKey.Material->GetAlphaFunc() == 0 && zColor( meshKey.Material->GetColor() ).bgra.alpha < 255) ) {
                        continue;
                    }
                    
                    zCTexture* tex = meshKey.Material->GetAniTexture();
                    if ( tex != boundTex ) { bindDiffuse( tex, 1 ); boundTex = tex; }
                    m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 1, mesh->BaseIndexLocation, 0, 0 );
                }
            }
        }

        const auto& rs = Engine::GAPI->GetRendererState().RendererSettings;
                
        const float shadowDistance = 8000 + (12000.0f * std::max( 0.1f, rs.WorldShadowRangeScale ));

        thread_local std::vector<SkeletalVobInfo*> cascadeMobs;
        std::vector<TransparencyVobInfo> _nop;
        std::vector<VobLightInfo*> _nop2;

        g_ShadowPassVobs[c].Reset(); // TODO: maybe only at BeginFrame?

        D3D12RenderQueue queue(&g_ShadowPassVobs[c], &cascadeMobs, &_nop, &_nop2);
        RndCullContext ctx;
        ctx.queue = &queue;
        ctx.frustum = m_CascadeFrustum[c];
        ctx.cameraPosition = Engine::GAPI->GetCameraPosition();
        ctx.stage = RenderStage::STAGE_DRAW_SHADOWS;
        ctx.drawDistances.OutdoorVobs = std::max(20000.0f, shadowDistance);
        ctx.drawDistances.OutdoorVobsSmall = std::max(20000.0f, shadowDistance);
        ctx.drawDistances.IndoorVobs = std::max(20000.0f, shadowDistance);
        ctx.drawDistances.VisualFX = 0.0f;
        ctx.drawDistancesSq.OutdoorVobs = ctx.drawDistances.OutdoorVobs * ctx.drawDistances.OutdoorVobs;
        ctx.drawDistancesSq.OutdoorVobsSmall = ctx.drawDistances.OutdoorVobsSmall * ctx.drawDistances.OutdoorVobsSmall;
        ctx.drawDistancesSq.IndoorVobs = ctx.drawDistances.IndoorVobs * ctx.drawDistances.IndoorVobs;
        ctx.drawDistancesSq.VisualFX = 0.0f;

        ctx.drawFlags.DrawVOBs = rs.DrawVOBs;
        ctx.drawFlags.DrawMobs = rs.DrawMobs;
        ctx.drawFlags.EnableDynamicLighting = rs.EnableDynamicLighting;
        ctx.drawFlags.EnableOcclusionCulling = false; // shadows do not use the players view frustum for culling, so occlusion culling would be inaccurate and cause popping.
        ctx.drawFlags.CullVobs = rs.DebugSettings.Culling.CullVobs;
        ctx.drawFlags.CollectIndoorVobs = false;
        ctx.drawFlags.CollectMobs = false;
        ctx.drawFlags.CollectLights = false;

        Engine::GAPI->CollectVisibleVobs( ctx ); // uses rendercontext and does not mutate objects.
        
        // --- Instanced VOBs (same root sig; two streams: packed vertex slot 0 + per-instance world slot 1) ---
        thread_local std::vector<FrameVobUpload> cascadeUploads;
        cascadeUploads.clear();
        
        const auto haveVobs = UploadVobs(g_ShadowPassVobs[c].buckets, cascadeUploads);
        if ( haveVobs ) {
            DX_ZONE(m_CmdList, "Vobs");
            
            m_CmdList->SetPipelineState( m_ShadowCasterVobPSO.Get() );
            m_CmdList->SetGraphicsRootSignature( m_WorldRootSig.Get() );
            m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
            for ( const FrameVobUpload& up : cascadeUploads ) {
                MeshVisualInfo* visual = up.visual;
                if ( !visual ) continue;
                for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
                    bindDiffuse( meshKey.Material->GetAniTexture(), 1 );
                    for ( MeshInfo* mi : meshList ) {
                        if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() ) continue;
                        D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
                        D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
                        if ( !mvb->GetResource() || !mib->GetResource() ) continue;
                        const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
                        const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, up.instView };
                        m_CmdList->IASetVertexBuffers( 0, 2, views );
                        const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                        m_CmdList->IASetIndexBuffer( &ibv );
                        m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mi->Indices.size() ), up.numInstances, 0, 0, 0 );
                    }
                }
            }
        }

        // --- Skinned skeletals (root sig: m_SkeletalRootSig; b0 cascade view-proj, b1 instance, b2 bones) ---
        if ( haveSkel ) {
            DX_ZONE(m_CmdList, "Skeletals");
            
            m_CmdList->SetPipelineState( m_ShadowCasterSkeletalPSO.Get() );
            m_CmdList->SetGraphicsRootSignature( m_SkeletalRootSig.Get() );
            m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
            for ( const FrameSkelDraw& d : g_FrameSkelDraws ) {
                if ( !d.visual ) continue;
                // Shared per-MODEL texture slots: refresh THIS instance's textures right before reading its
                // materials (see [[skeletal-texani-shared-slots]]) — required in the shadow pass too (alpha-clip).
                zCModel* model = static_cast<zCModel*>( d.vobInfo->Vob->GetVisual() );
                model->UpdateMeshLibTexAniState();

                m_CmdList->SetGraphicsRootConstantBufferView( 1, d.instCb );
                m_CmdList->SetGraphicsRootConstantBufferView( 2, d.boneCb );
                for ( auto const& [mat, meshList] : d.visual->SkeletalMeshes ) {
                    bindDiffuse( mat ? mat->GetAniTexture() : nullptr, 3 );
                    for ( auto const& mesh : meshList ) {
                        if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer ) continue;
                        D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
                        D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
                        if ( !mvb->GetResource() || !mib->GetResource() ) continue;
                        const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExSkelVertexStruct ) };
                        m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
                        const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                        m_CmdList->IASetIndexBuffer( &ibv );
                        m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 1, 0, 0, 0 );
                    }
                }
            }
        }

        // --- Node attachments (weapons/heads) through the VOB caster PSO (packed vertex + single instance) ---
        if ( haveAttach ) {
            DX_ZONE(m_CmdList, "Skeletal Nodes");

            m_CmdList->SetPipelineState( m_ShadowCasterVobPSO.Get() );
            m_CmdList->SetGraphicsRootSignature( m_WorldRootSig.Get() );
            m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
            for ( const FrameAttachDraw& a : g_FrameAttachDraws ) {
                if ( !a.mesh || !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() ) continue;
                D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
                D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
                if ( !mvb->GetResource() || !mib->GetResource() ) continue;
                bindDiffuse( a.tex, 1 );
                const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
                const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, a.instView };
                m_CmdList->IASetVertexBuffers( 0, 2, views );
                const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                m_CmdList->IASetIndexBuffer( &ibv );
                m_CmdList->DrawIndexedInstanced( static_cast<UINT>( a.mesh->Indices.size() ), 1, 0, 0, 0 );
            }
        }
    }

    // Hand the whole array to PIXEL_SHADER_RESOURCE for the (future) lit-pass PCF sampling; reverted at the top
    // of next frame's shadow pass. Also re-binds the main RT/DSV so subsequent passes draw to the backbuffer.
    auto toSrv = TransitionBarrier( m_ShadowMap.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
    m_CmdList->ResourceBarrier( 1, &toSrv );
    m_ShadowInPixelState = true;

    // Restore the HDR scene-color RT (+ shared depth) for the lit passes that follow — the world pass renders
    // into the HDR target, not the swapchain (Phase 3); the tonemap resolve composites it at the end of the frame.
    D3D12_CPU_DESCRIPTOR_HANDLE mainDsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, m_DepthBuffer ? &mainDsv : nullptr );
}

void D3D12GraphicsEngine::RenderPointShadows() {
    // P2.10g — static/dynamic split (the D3D11 static-aside model). Per shadowed light, the active cube is built
    // each frame as (cached static-only depth) + (this frame's dynamic casters overlaid). Three phases:
    //   A) STATIC pass — for slots whose light is fresh / moved / resized (renderStatic), (re)render the STATIC
    //      casters (world mesh + instanced VOBs) into the static-aside cube slot. Amortized: usually a no-op.
    //   B) COPY — CopyResource the whole static-aside cube into the active cube (cheap ~6MB depth copy).
    //   C) DYNAMIC overlay — render the moving casters (skeletal NPCs) into the active cube over the copied
    //      static depth (LESS_EQUAL, no clear), every frame for every shadowed light.
    // So per-frame cost is one depth copy + the few near dynamic draws — the expensive static cull/draw is paid
    // once. Casters are range-culled to each light's sphere (360°, not the camera frustum). NORMAL-Z depth; the
    // active cube round-trips DEPTH_WRITE/COPY_DEST -> PIXEL_SHADER_RESOURCE for the lit pass and back next frame.
    if ( !m_FrameOpen || !m_PointShadowCube || !m_PointShadowStaticCube || !m_PointShadowCasterWorldPSO
        || !m_PointShadowDsvHeap || !m_PointShadowStaticDsvHeap || !m_PointShadowRootSig )
        return;
    if ( g_FramePointShadows.empty() ) return;

    DX_ZONE( m_CmdList, "Point Shadows (cubes)" );

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    D3D12VertexBuffer* vb = wm ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
    D3D12VertexBuffer* ib = wm ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;
    const bool haveWorld = vb && ib && vb->GetResource() && ib->GetResource();
    const bool haveVobs  = m_PointShadowCasterVobPSO && !g_FrameVobUploads.empty()
                        && m_PointShadowVobInstPtr[m_FrameIndex];
    const bool haveSkel  = m_PointShadowCasterSkeletalPSO && m_PointShadowSkeletalRootSig && !g_FrameSkelDraws.empty();

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( kPointShadowCubeSize ), static_cast<float>( kPointShadowCubeSize ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, static_cast<LONG>( kPointShadowCubeSize ), static_cast<LONG>( kPointShadowCubeSize ) };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    auto bindDiffuse = [&]( zCTexture* tex, UINT rootParam ) {
        D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
        if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN )
            if ( MyDirectDrawSurface7* surface = tex->GetSurface() )
                if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                    D3D12Texture* d12 = D3D12Texture::From( gfx );
                    if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                }
        m_CmdList->SetGraphicsRootDescriptorTable( rootParam, srv );
    };

    // Standard D3D cube face order: +X, -X, +Y, -Y, +Z, -Z, with the canonical per-face up vectors.
    static const XMVECTORF32 kFaceDir[6] = {
        { { {  1, 0, 0, 0 } } }, { { { -1, 0, 0, 0 } } }, { { { 0,  1, 0, 0 } } },
        { { { 0, -1, 0, 0 } } }, { { {  0, 0, 1, 0 } } }, { { { 0, 0, -1, 0 } } } };
    static const XMVECTORF32 kFaceUp[6] = {
        { { { 0, 1, 0, 0 } } }, { { { 0, 1, 0, 0 } } }, { { { 0, 0, -1, 0 } } },
        { { { 0, 0, 1, 0 } } }, { { { 0, 1, 0, 0 } } }, { { { 0, 1, 0, 0 } } } };

    const D3D12_CPU_DESCRIPTOR_HANDLE activeDsvBase = m_PointShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE staticDsvBase = m_PointShadowStaticDsvHeap->GetCPUDescriptorHandleForHeapStart();
    auto& worldSections = Engine::GAPI->GetWorldSections();

    // Precompute each winner's 6 face view-projs into its per-frame CB slot (transpose(view*proj) — same
    // column-major convention the world/CSM shaders read back). Both the static and dynamic passes bind this.
    for ( const FramePointShadow& ps : g_FramePointShadows ) {
        if ( ps.slot >= kMaxShadowCubes ) continue;
        const XMVECTOR eye = XMLoadFloat3( &ps.posWS );
        const XMMATRIX proj = XMMatrixPerspectiveFovLH( XM_PIDIV2, 1.0f, 15.0f, ps.range * 2.0f );
        XMFLOAT4X4* faceVP = reinterpret_cast<XMFLOAT4X4*>( m_PointShadowCBMapped[m_FrameIndex] + static_cast<size_t>( ps.slot ) * 512 );
        for ( int f = 0; f < 6; ++f ) {
            XMMATRIX vw = XMMatrixLookAtLH( eye, XMVectorAdd( eye, kFaceDir[f] ), kFaceUp[f] );
            XMStoreFloat4x4( &faceVP[f], XMMatrixTranspose( XMMatrixMultiply( vw, proj ) ) );
        }
    }
    auto faceCb = [&]( UINT slot ) { return m_PointShadowCBGpu[m_FrameIndex] + static_cast<UINT64>( slot ) * 512; };

    // ============================ Phase A — STATIC pass (into the static-aside cube) ============================
    bool anyStatic = false;
    for ( const FramePointShadow& ps : g_FramePointShadows ) if ( ps.renderStatic && ps.slot < kMaxShadowCubes ) { anyStatic = true; break; }

    if ( anyStatic ) {
        DX_ZONE( m_CmdList, "Static Pass" );
        if ( m_PointShadowStaticState != D3D12_RESOURCE_STATE_DEPTH_WRITE ) {
            auto b = TransitionBarrier( m_PointShadowStaticCube.Get(), m_PointShadowStaticState, D3D12_RESOURCE_STATE_DEPTH_WRITE );
            m_CmdList->ResourceBarrier( 1, &b );
            m_PointShadowStaticState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }

        // Reset the tight VOB-instance ring — only static-VOB gathers use it now (dynamic pass has no VOBs).
        m_PointShadowVobInstOffset = 0;
        uint8_t* const viBase = m_PointShadowVobInstPtr[m_FrameIndex];
        const D3D12_GPU_VIRTUAL_ADDRESS viGpu = m_PointShadowVobInstGpu[m_FrameIndex];

        for ( const FramePointShadow& ps : g_FramePointShadows ) {
            if ( ps.slot >= kMaxShadowCubes || !ps.renderStatic ) continue;
            m_PointShadowSlots[ps.slot].staticValid = true;   // static-aside now holds this slot's static depth

            D3D12_CPU_DESCRIPTOR_HANDLE dsv = staticDsvBase;
            dsv.ptr += static_cast<SIZE_T>( ps.slot ) * m_PointShadowDsvSize;
            m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );
            m_CmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );

            const float rangeSq = ps.range * ps.range;

            // --- World mesh: range-cull sections (AABB nearest-point), draw all 6 faces in one call. ---
            if ( haveWorld ) {
                DX_ZONE( m_CmdList, "World Mesh" );
                m_CmdList->SetPipelineState( m_PointShadowCasterWorldPSO.Get() );
                m_CmdList->SetGraphicsRootSignature( m_PointShadowRootSig.Get() );
                m_CmdList->SetGraphicsRootConstantBufferView( 0, faceCb( ps.slot ) );
                const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
                const D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
                m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
                m_CmdList->IASetIndexBuffer( &ibv );
                zCTexture* boundTex = nullptr;
                for ( auto& [sx, col] : worldSections ) {
                    for ( auto& [sy, section] : col ) {
                        const zTBBox3D& bb = section.BoundingBox;
                        float cx = std::min( std::max( ps.posWS.x, bb.Min.x ), bb.Max.x );
                        float cy = std::min( std::max( ps.posWS.y, bb.Min.y ), bb.Max.y );
                        float cz = std::min( std::max( ps.posWS.z, bb.Min.z ), bb.Max.z );
                        float dx = ps.posWS.x - cx, dy = ps.posWS.y - cy, dz = ps.posWS.z - cz;
                        if ( dx * dx + dy * dy + dz * dz >= rangeSq ) continue;   // section outside the light sphere
                        for ( auto const& [meshKey, mesh] : section.WorldMeshes ) {
                            if ( !mesh || mesh->Indices.empty() ) continue;
                            if ( meshKey.Info && meshKey.Info->MaterialType == MaterialInfo::MT_Water ) continue;
                            zCTexture* tex = meshKey.Material->GetAniTexture();
                            if ( tex != boundTex ) { bindDiffuse( tex, 1 ); boundTex = tex; }
                            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 6, mesh->BaseIndexLocation, 0, 0 );
                        }
                    }
                }
            }

            // --- Instanced VOBs (static decoration): range-cull instances, pack 64B world matrices into the tight
            // ring, draw count*6 (InstanceDataStepRate=6 → 6 faces per real instance). Same root sig as world. ---
            if ( haveVobs ) {
                DX_ZONE( m_CmdList, "Vobs" );
                m_CmdList->SetPipelineState( m_PointShadowCasterVobPSO.Get() );
                m_CmdList->SetGraphicsRootSignature( m_PointShadowRootSig.Get() );
                m_CmdList->SetGraphicsRootConstantBufferView( 0, faceCb( ps.slot ) );
                for ( const FrameVobUpload& up : g_FrameVobUploads ) {
                    MeshVisualInfo* visual = up.visual;
                    if ( !visual || visual->Instances.empty() ) continue;
                    const float cullR   = ps.range + visual->MeshSize * 0.5f;   // sphere test allows for VOB extent
                    const float cullRSq = cullR * cullR;

                    const UINT gatherStart = m_PointShadowVobInstOffset;
                    UINT count = 0;
                    bool overflow = false;
                    for ( const VobInstanceInfo& inst : visual->Instances ) {
                        float dx = inst.world._41 - ps.posWS.x, dy = inst.world._42 - ps.posWS.y, dz = inst.world._43 - ps.posWS.z;
                        if ( dx * dx + dy * dy + dz * dz >= cullRSq ) continue;
                        if ( m_PointShadowVobInstOffset + sizeof( XMFLOAT4X4 ) > m_PointShadowVobInstCapacity ) {
                            if ( !m_PointShadowVobInstOverflowLogged ) {
                                LogWarn() << "D3D12: point-shadow VOB instance ring overflow ("
                                    << m_PointShadowVobInstCapacity << " bytes/frame); some cube casters dropped.";
                                m_PointShadowVobInstOverflowLogged = true;
                            }
                            overflow = true;
                            break;
                        }
                        memcpy( viBase + m_PointShadowVobInstOffset, &inst.world, sizeof( XMFLOAT4X4 ) );
                        m_PointShadowVobInstOffset += sizeof( XMFLOAT4X4 );
                        ++count;
                    }
                    if ( count == 0 ) { if ( overflow ) break; continue; }

                    const D3D12_VERTEX_BUFFER_VIEW instView = { viGpu + gatherStart, count * static_cast<UINT>( sizeof( XMFLOAT4X4 ) ), static_cast<UINT>( sizeof( XMFLOAT4X4 ) ) };
                    for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
                        bindDiffuse( meshKey.Material->GetAniTexture(), 1 );
                        for ( MeshInfo* mi : meshList ) {
                            if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() ) continue;
                            D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
                            D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
                            if ( !mvb->GetResource() || !mib->GetResource() ) continue;
                            const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
                            const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, instView };
                            m_CmdList->IASetVertexBuffers( 0, 2, views );
                            const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                            m_CmdList->IASetIndexBuffer( &ibv );
                            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mi->Indices.size() ), count * 6, 0, 0, 0 );
                        }
                    }
                    if ( overflow ) break;
                }
            }
        }
    }

    // ============================ Phase B — COPY static-aside -> active cube ============================
    {
        DX_ZONE( m_CmdList, "Copy Static->Active" );
        D3D12_RESOURCE_BARRIER pre[2];
        UINT n = 0;
        if ( m_PointShadowStaticState != D3D12_RESOURCE_STATE_COPY_SOURCE ) {
            pre[n++] = TransitionBarrier( m_PointShadowStaticCube.Get(), m_PointShadowStaticState, D3D12_RESOURCE_STATE_COPY_SOURCE );
            m_PointShadowStaticState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }
        const D3D12_RESOURCE_STATES activeCur = m_PointShadowInPixelState
            ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_DEPTH_WRITE;
        pre[n++] = TransitionBarrier( m_PointShadowCube.Get(), activeCur, D3D12_RESOURCE_STATE_COPY_DEST );
        m_CmdList->ResourceBarrier( n, pre );

        m_CmdList->CopyResource( m_PointShadowCube.Get(), m_PointShadowStaticCube.Get() );

        auto toDepth = TransitionBarrier( m_PointShadowCube.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE );
        m_CmdList->ResourceBarrier( 1, &toDepth );
        m_PointShadowInPixelState = false;   // active cube now in DEPTH_WRITE for the dynamic overlay
    }

    // ============================ Phase C — DYNAMIC overlay (skeletal NPCs into active cube) ============================
    // Rendered over the copied static depth (LESS_EQUAL, NO clear) so moving casters composite with the cached
    // static occluders. Runs every frame for every shadowed light — this is the real-time part of the split.
    if ( haveSkel ) {
        DX_ZONE( m_CmdList, "Dynamic Overlay (skeletals)" );
        m_CmdList->SetPipelineState( m_PointShadowCasterSkeletalPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_PointShadowSkeletalRootSig.Get() );
        for ( const FramePointShadow& ps : g_FramePointShadows ) {
            if ( ps.slot >= kMaxShadowCubes ) continue;
            D3D12_CPU_DESCRIPTOR_HANDLE dsv = activeDsvBase;
            dsv.ptr += static_cast<SIZE_T>( ps.slot ) * m_PointShadowDsvSize;
            m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );   // no clear — keep the copied static depth
            m_CmdList->SetGraphicsRootConstantBufferView( 0, faceCb( ps.slot ) );

            for ( const FrameSkelDraw& d : g_FrameSkelDraws ) {
                if ( !d.visual || !d.vobInfo || !d.vobInfo->Vob ) continue;
                const XMFLOAT3 pos = d.vobInfo->Vob->GetPositionWorld();
                const float cullR = ps.range + d.visual->MeshSize * 0.5f;
                float dx = pos.x - ps.posWS.x, dy = pos.y - ps.posWS.y, dz = pos.z - ps.posWS.z;
                if ( dx * dx + dy * dy + dz * dz >= cullR * cullR ) continue;

                // Shared per-MODEL texture slots: refresh THIS instance's textures right before reading its
                // materials (see [[skeletal-texani-shared-slots]]) — required in the cube alpha-clip pass too.
                zCModel* model = static_cast<zCModel*>( d.vobInfo->Vob->GetVisual() );
                model->UpdateMeshLibTexAniState();

                m_CmdList->SetGraphicsRootConstantBufferView( 1, d.instCb );
                m_CmdList->SetGraphicsRootConstantBufferView( 2, d.boneCb );
                for ( auto const& [mat, meshList] : d.visual->SkeletalMeshes ) {
                    bindDiffuse( mat ? mat->GetAniTexture() : nullptr, 3 );
                    for ( auto const& mesh : meshList ) {
                        if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer ) continue;
                        D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
                        D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
                        if ( !mvb->GetResource() || !mib->GetResource() ) continue;
                        const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExSkelVertexStruct ) };
                        m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
                        const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                        m_CmdList->IASetIndexBuffer( &ibv );
                        m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 6, 0, 0, 0 );
                    }
                }
            }
        }
    }

    // ============================ Phase D — active cube -> PIXEL_SHADER_RESOURCE for the lit pass ============================
    auto toSrv = TransitionBarrier( m_PointShadowCube.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
    m_CmdList->ResourceBarrier( 1, &toSrv );
    m_PointShadowInPixelState = true;

    // Restore the HDR scene-color RT (+ shared depth) for the lit passes that follow — the world pass renders
    // into the HDR target, not the swapchain (Phase 3); the tonemap resolve composites it at the end of the frame.
    D3D12_CPU_DESCRIPTOR_HANDLE mainDsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, m_DepthBuffer ? &mainDsv : nullptr );
}

bool D3D12GraphicsEngine::CreateLightCullPipeline() {
    // Forward+ tiled light-culling compute pipeline (P2.9b-2). One GLOBAL compute root signature + PSO,
    // created once. b0 = cull constants (8 root 32-bit values); t0 = the point-light StructuredBuffer as a
    // root SRV (same UPLOAD buffer the world PS reads); u0/u1 = the light grid / index-list DEFAULT-heap UAVs
    // as root UAVs (RWStructuredBuffers are valid as root UAVs; stride comes from the HLSL declaration). t1 =
    // the depth buffer SRV, which (being a Texture2D) can't be a root SRV, so it rides a one-entry descriptor
    // table off the shared SRV heap — used to tighten each tile's far-Z bound (P2.9b-3 flicker fix).
    ID3D12Device* device = m_Device.GetDevice();

    D3D12_DESCRIPTOR_RANGE depthRange = {};
    depthRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depthRange.NumDescriptors = 1;
    depthRange.BaseShaderRegister = 1;        // t1 DepthTex
    depthRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[5] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0 CullCB
    params[0].Constants.Num32BitValues = 8;   // ProjScale(2) + ScreenDim(2) + TotalLights + NumTilesX + ProjA + ProjB
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;  // t0 SB_Lights
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;  // u0 RW_LightGrid
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[3].Descriptor.ShaderRegister = 1;  // u1 RW_LightIndexList
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &depthRange;   // t1 DepthTex (SRV heap)
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof( params );
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;   // compute: no IA input layout

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (light cull)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: light-cull root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( m_LightCullRootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    UINT compileFlags = 0;
    if ( !CompileShaderD3D12( kLightCullShaderSource, sizeof( kLightCullShaderSource ) - 1, "LightCull",
        nullptr, nullptr, "CSMain", Shadermodel_CS, compileFlags, 0, m_LightCullCsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_LightCullRootSig.Get();
    pso.CS = { m_LightCullCsBlob->GetBufferPointer(), m_LightCullCsBlob->GetBufferSize() };
    if ( FAILED( device->CreateComputePipelineState( &pso, IID_PPV_ARGS( m_LightCullPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateComputePipelineState failed (light cull).";
        return false;
    }
    return true;
}

bool D3D12GraphicsEngine::CreateLightCullBuffers( INT2 size ) {
    // Per-resolution Forward+ tile grid storage (P2.9b-2). Recreated on resize alongside the depth buffer.
    // RW_LightGrid: one {Offset,Count} (8 B) per 16x16 tile. RW_LightIndexList: a fixed MAX_LIGHTS_PER_TILE
    // (=32) uint slice per tile (no compaction/global counter — see the shader header). Both are DEFAULT-heap
    // UAV buffers created in UNORDERED_ACCESS; each frame DispatchLightCulling writes them (UAV) then transitions
    // them to PIXEL_SHADER_RESOURCE for the lit geometry passes to read, then back. ~1 MB total at 1080p.
    if ( size.x <= 0 || size.y <= 0 ) return false;
    ID3D12Device* device = m_Device.GetDevice();

    constexpr UINT kTileSize = 16;
    constexpr UINT kMaxLightsPerTile = 32;
    m_NumTilesX = ( static_cast<UINT>( size.x ) + kTileSize - 1 ) / kTileSize;
    m_NumTilesY = ( static_cast<UINT>( size.y ) + kTileSize - 1 ) / kTileSize;
    const UINT numTiles = m_NumTilesX * m_NumTilesY;
    if ( numTiles == 0 ) return false;

    D3D12_HEAP_PROPERTIES heapDefault = {};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto makeUavBuffer = [&]( UINT64 bytes, const wchar_t* name, ComPtr<ID3D12Resource>& out ) -> bool {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if ( FAILED( device->CreateCommittedResource( &heapDefault, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create a light-cull UAV buffer.";
            return false;
        }
        out->SetName( name );
        return true;
    };

    if ( !makeUavBuffer( static_cast<UINT64>( numTiles ) * 2u * sizeof( uint32_t ), L"LightGrid", m_LightGridBuffer ) )
        return false;
    if ( !makeUavBuffer( static_cast<UINT64>( numTiles ) * kMaxLightsPerTile * sizeof( uint32_t ), L"LightIndexList", m_LightIndexBuffer ) )
        return false;
    m_LightGridInPixelState = false;   // freshly created in UNORDERED_ACCESS (see DispatchLightCulling round-trip)
    return true;
}

bool D3D12GraphicsEngine::CreateVobPipeline() {
    ID3D12Device* device = m_Device.GetDevice();

    UINT compileFlags = 0;
    if ( !CompileShaderD3D12( kVobShaderSource, sizeof( kVobShaderSource ) - 1, "VobShader", nullptr, nullptr,
        "VSMain", Shadermodel_VS, compileFlags, 0, m_VobVsBlob.ReleaseAndGetAddressOf() ) ) {
            return false;
    }
    if ( !CompileShaderD3D12( kVobShaderSource, sizeof( kVobShaderSource ) - 1, "VobShader", nullptr, nullptr,
        "PSMain", Shadermodel_PS, compileFlags, 0, m_VobPsBlob.ReleaseAndGetAddressOf() ) ) {
            return false;
    }

    // Slot 0 = packed 36-byte ExVertexStructGPU (Position@0, TexCoord0@20); slot 1 = per-instance data
    // read from VobInstanceInfo (stride 144): world matrix rows @0/16/32/48, instance color @128.
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R16G16_SNORM,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },  // octahedral, object-space
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,   0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_COLOR",        0, DXGI_FORMAT_R8G8B8A8_UNORM,     1, 128, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };

    // Reuse the world root signature (b0 ViewProj + t0 SRV + static sampler s0 — identical needs).
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_WorldRootSig.Get();
    pso.VS = { m_VobVsBlob->GetBufferPointer(), m_VobVsBlob->GetBufferSize() };
    pso.PS = { m_VobPsBlob->GetBufferPointer(), m_VobPsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // first-light; VOB winding varies
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed-Z
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_VobPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB).";
        return false;
    }
    return CreateVobInstanceBuffers();
}

bool D3D12GraphicsEngine::CreateVobInstanceBuffers() {
    ID3D12Device* device = m_Device.GetDevice();
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kVobInstanceBufferBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( m_VobInstanceBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_VobInstanceBuffer[i]->SetName( i == 0 ? L"VobInstanceRing0" : L"VobInstanceRing1" );
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( m_VobInstanceBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_VobInstanceBufferPtr[i] ) ) ) )
            return false;
    }
    m_VobInstanceBufferCapacity = kVobInstanceBufferBytes;
    return true;
}

bool D3D12GraphicsEngine::CreateLightBuffer() {
    // Per-frame point-light StructuredBuffers (one per in-flight frame). The whole visible-light list is
    // rewritten from offset 0 each frame, so these are plain persistently-mapped UPLOAD snapshots, bound
    // as a root SRV. Sized kMaxFrameLights * sizeof(GPULight).
    ID3D12Device* device = m_Device.GetDevice();
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = static_cast<UINT64>(kMaxFrameLights) * sizeof( GPULight );
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( m_LightBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_LightBuffer[i]->SetName( i == 0 ? L"PointLightBuffer0" : L"PointLightBuffer1" );
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( m_LightBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_LightBufferPtr[i] ) ) ) )
            return false;
    }
    m_LightBufferCapacity = kMaxFrameLights;
    return true;
}

void D3D12GraphicsEngine::BuildFrameLightBuffer() {
    // Rebuild this frame's point-light buffer from the visible-light set collected in OnStartWorldRendering.
    // Mirrors D3D11 D3D11TiledDeferredShading::CullLights' CPU fill: skip disabled lights, unpack the color
    // DWORD (0xAARRGGBB), apply the fixed 1.2 lightFactor, store world-space position + range. View-space
    // position / shadow index are left defaulted (world-space shading in the MVP; tiling fills them later).
    m_FrameLightCount = 0;
    const UINT frame = m_FrameIndex;
    if ( !m_LightBuffer[frame] || !m_LightBufferPtr[frame] ) return;

    GPULight* dst = reinterpret_cast<GPULight*>(m_LightBufferPtr[frame]);
    UINT count = 0;
    constexpr float lightFactor = 1.2f;   // matches D3D11 CullLights RGB scale

    // Parallel to dst[]: the owning light Vob per GPULight index, so the shadow selection below can map a
    // chosen light back to its identity for STABLE per-light cube-slot ownership (static-aside cache, P2.10f).
    static std::vector<zCVobLight*> s_lightVobs;
    s_lightVobs.clear();

    // View-space transform for PositionView (consumed by the tiled light-culling CS). Mirrors D3D11
    // CullLights EXACTLY: transpose(GetViewMatrixXM()) then a row-vector transform of the world position,
    // so the view space this fills matches what the cull shader's frustum is built in.
    const XMMATRIX view = XMMatrixTranspose( Engine::GAPI->GetViewMatrixXM() );

    for ( VobLightInfo* li : g_FrameLights ) {
        if ( !li || !li->Vob ) continue;
        zCVobLight* vob = li->Vob;
        if ( !vob->IsEnabled() ) continue;
        if ( count >= m_LightBufferCapacity ) {
            if ( !m_LightOverflowLogged ) {
                LogWarn() << "D3D12: point-light buffer overflow (" << m_LightBufferCapacity
                    << " lights/frame). Excess lights dropped this frame.";
                m_LightOverflowLogged = true;
            }
            break;
        }
        const DWORD c = vob->GetLightColor();   // 0xAARRGGBB
        const float r = ((c >> 16) & 0xFF) / 255.0f;
        const float g = ((c >> 8) & 0xFF) / 255.0f;
        const float b = (c & 0xFF) / 255.0f;
        const XMFLOAT3 pw = vob->GetPositionWorld();

        GPULight& L = dst[count];
        XMStoreFloat3( &L.PositionView, XMVector3TransformCoord( XMLoadFloat3( &pw ), view ) );
        L.Range = vob->GetLightRange();
        L.Color = XMFLOAT4( r * lightFactor, g * lightFactor, b * lightFactor, vob->IsStatic() ? 0.0f : 1.0f );
        L.PositionWorld = pw;
        L.ShadowCubeIndex = -1;
        s_lightVobs.push_back( vob );
        ++count;
    }
    m_FrameLightCount = count;

    // Point-light shadow selection (P2.10c + static-aside/round-robin P2.10f). Pick the closest-to-camera
    // in-range lights (up to kMaxShadowCubes) as this frame's "winners", but assign each winner a STABLE cube
    // slot keyed by its light Vob identity (kept across frames, not reassigned by proximity every frame). A
    // slot's rendered content persists in the cube array, so a STATIC winner whose light didn't move can reuse
    // its cached cube (render=false) instead of re-culling + re-rendering all world/VOB/skeletal casters each
    // frame. Dynamic (moving) lights, newly-assigned slots, and moved/range-changed lights render (render=true).
    // Mirrors D3D11 DrawPointlightShadows' distance/range gating (it keys off the player; camera is close enough).
    g_FramePointShadows.clear();
    if ( m_PointShadowCube && count > 0 ) {
        const XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
        struct Cand { UINT dstIdx; zCVobLight* vob; float distSq; bool isStatic; };
        static std::vector<Cand> cands;
        cands.clear();
        for ( UINT i = 0; i < count; ++i ) {
            const GPULight& L = dst[i];
            const float range = L.Range;
            if ( range <= 0.0f ) continue;
            XMVECTOR d = XMVectorSubtract( XMLoadFloat3( &L.PositionWorld ), camPos );
            float distSq = XMVectorGetX( XMVector3LengthSq( d ) );
            const float maxSq = ( range * 9.0f ) * ( range * 9.0f );   // D3D11 distMaxShadowSq
            if ( distSq >= maxSq ) continue;
            cands.push_back( { i, s_lightVobs[i], distSq, L.Color.w == 0.0f } );   // Color.w: 0 = static light
        }
        std::sort( cands.begin(), cands.end(), []( const Cand& a, const Cand& b ) { return a.distSq < b.distSq; } );
        if ( cands.size() > kMaxShadowCubes ) cands.resize( kMaxShadowCubes );

        // Release slots whose owner is no longer a winner (frees them + invalidates their cached content).
        for ( UINT s = 0; s < kMaxShadowCubes; ++s ) {
            zCVobLight* o = m_PointShadowSlots[s].owner;
            if ( !o ) continue;
            bool stillWinner = false;
            for ( const Cand& c : cands ) if ( c.vob == o ) { stillWinner = true; break; }
            if ( !stillWinner ) m_PointShadowSlots[s] = PointShadowSlot{};
        }

        // Assign each winner a stable slot (keep its existing one, else grab a free one) and decide render vs cache.
        for ( const Cand& c : cands ) {
            int slot = -1;
            for ( UINT s = 0; s < kMaxShadowCubes; ++s )
                if ( m_PointShadowSlots[s].owner == c.vob ) { slot = static_cast<int>( s ); break; }
            if ( slot < 0 ) {
                for ( UINT s = 0; s < kMaxShadowCubes; ++s )
                    if ( !m_PointShadowSlots[s].owner ) { slot = static_cast<int>( s ); break; }
                if ( slot < 0 ) continue;   // no free slot (can't happen: winners <= kMaxShadowCubes)
                m_PointShadowSlots[slot].owner = c.vob;
                m_PointShadowSlots[slot].staticValid = false;   // fresh occupant → must render static
            }
            PointShadowSlot& ss = m_PointShadowSlots[slot];
            ss.isStatic = c.isStatic;

            GPULight& L = dst[c.dstIdx];
            const XMFLOAT3& np = L.PositionWorld;
            const float moveEps = 0.5f;   // Gothic world units; below this the light hasn't meaningfully moved
            bool moved = std::fabs( np.x - ss.pos.x ) > moveEps
                      || std::fabs( np.y - ss.pos.y ) > moveEps
                      || std::fabs( np.z - ss.pos.z ) > moveEps;
            bool rangeChanged = std::fabs( L.Range - ss.range ) > 1.0f;
            // Static-aside is re-rendered only when fresh / the light moved / range changed; otherwise reused.
            // The DYNAMIC (skeletal) overlay + the static->active copy run every frame regardless (see RenderPointShadows).
            bool renderStatic = !ss.staticValid || moved || rangeChanged;

            L.ShadowCubeIndex = static_cast<int32_t>( slot );
            g_FramePointShadows.push_back( { np, L.Range, static_cast<UINT>( slot ), renderStatic } );
            if ( renderStatic ) { ss.pos = np; ss.range = L.Range; }   // staticValid stamped once actually drawn
        }
    }
}

void D3D12GraphicsEngine::BindFrameLights( UINT srvParam, UINT countParam, UINT gridParam, UINT indexParam ) {
    // Bind the Forward+ tiled point-light root params: srvParam = the light StructuredBuffer as a root SRV
    // (t1), countParam = LightCB { LightCount, NumTilesX }, gridParam/indexParam = the per-tile grid (t2) and
    // light-index list (t3) root SRVs produced by DispatchLightCulling. EVERY draw whose bound PSO reads the
    // tiled light loop MUST call this after setting its root signature, or the loop bound (Count) and grid are
    // UNDEFINED root values and can run billions of iterations → GPU timeout/removal. Root args are cleared on
    // every SetGraphicsRootSignature. The param indices differ per root sig: m_WorldRootSig uses (3,4,5,6) —
    // the default — for the world mesh / instanced VOBs / node attachments; m_SkeletalRootSig uses (5,6,7,8).
    m_CmdList->SetGraphicsRootShaderResourceView( srvParam, m_LightBuffer[m_FrameIndex]->GetGPUVirtualAddress() );
    m_CmdList->SetGraphicsRoot32BitConstant( countParam, m_FrameLightCount, 0 );   // LightCount @ b*.x
    m_CmdList->SetGraphicsRoot32BitConstant( countParam, m_NumTilesX, 1 );         // NumTilesX  @ b*.y
    m_CmdList->SetGraphicsRootShaderResourceView( gridParam, m_LightGridBuffer->GetGPUVirtualAddress() );
    m_CmdList->SetGraphicsRootShaderResourceView( indexParam, m_LightIndexBuffer->GetGPUVirtualAddress() );
}

bool D3D12GraphicsEngine::CreateWaterPipeline() {
    ID3D12Device* device = m_Device.GetDevice();

    // Root signature = the world layout + one extra param: b0 ViewProj (16 consts, VS), t0 diffuse SRV
    // (PS), b1 fog (8 consts, ALL), b2 water { time, alpha } (4 consts, ALL — VS reads time, PS alpha).
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;         // t0
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0 ViewProj
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 1;   // b1 fog
    params[2].Constants.Num32BitValues = 8;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.ShaderRegister = 2;   // b2 water { time, alpha }
    params[3].Constants.Num32BitValues = 4;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;              // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof( params );
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (water)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: water root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( m_WaterRootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    UINT compileFlags = 0;
    if ( !CompileShaderD3D12( kWaterShaderSource, sizeof( kWaterShaderSource ) - 1, "WaterShader", nullptr, nullptr,
        "VSMain", Shadermodel_VS, compileFlags, 0, m_WaterVsBlob.ReleaseAndGetAddressOf() ) ) {
            return false;
    }
    if ( !CompileShaderD3D12( kWaterShaderSource, sizeof( kWaterShaderSource ) - 1, "WaterShader", nullptr, nullptr,
        "PSMain", Shadermodel_PS, compileFlags, 0, m_WaterPsBlob.ReleaseAndGetAddressOf() ) ) {
            return false;
    }

    // Same packed 36-byte ExVertexStructGPU as the world mesh; here TexCoord2 (@28, half2) is the water
    // UV-scroll delta (bound as TEXCOORD1), and DIFFUSE (@32) is the baked vertex tint.
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT,    0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "DIFFUSE",  0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_WaterRootSig.Get();
    pso.VS = { m_WaterVsBlob->GetBufferPointer(), m_WaterVsBlob->GetBufferSize() };
    pso.PS = { m_WaterPsBlob->GetBufferPointer(), m_WaterPsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    // Straight alpha blend over the opaque scene: src.rgb*a + dst.rgb*(1-a); keep dst alpha.
    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Reversed-Z: test GREATER_EQUAL, but DO NOT write depth — transparent water must not occlude, and
    // overlapping water blends painter-style over whatever opaque depth is already there.
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_WaterPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (water).";
        return false;
    }
    return true;
}

void D3D12GraphicsEngine::DrawWaterSurfaces() {
    if ( !m_FrameOpen || !m_WaterPSO || !m_WaterRootSig || !m_DepthBuffer || g_FrameWaterSurfaces.empty() )
        return;

    DX_ZONE( m_CmdList, "DrawWaterSurfaces" );

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    if ( !wm || !wm->GetMeshVertexBuffer() || !wm->GetMeshIndexBuffer() ) { g_FrameWaterSurfaces.clear(); return; }
    D3D12VertexBuffer* vb = D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() );
    D3D12VertexBuffer* ib = D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() );
    if ( !vb->GetResource() || !ib->GetResource() ) { g_FrameWaterSurfaces.clear(); return; }

    // ViewProj — identical derivation to DrawWorldMesh (water verts are already world-space).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    const FogConstants fog = MakeFogConstants();
    // b2: { totalTime (ms, drives UV scroll), water alpha (translucency), pad, pad }.
    const float water[4] = { Engine::GAPI->GetTotalTime(), 0.7f, 0.0f, 0.0f };

    m_CmdList->SetPipelineState( m_WaterPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_WaterRootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 3, 4, water, 0 );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
    D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    m_CmdList->IASetIndexBuffer( &ibv );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    unsigned int drawnIndices = 0;
    for ( auto const& [tex, meshes] : g_FrameWaterSurfaces ) {
        D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
        if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
            if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
                if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                    D3D12Texture* d12 = D3D12Texture::From( gfx );
                    if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                }
            }
        }
        m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );
        for ( MeshInfo* mesh : meshes ) {
            if ( !mesh || mesh->Indices.empty() ) continue;
            m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mesh->Indices.size()), 1,
                mesh->BaseIndexLocation, 0, 0 );
            drawnIndices += static_cast<unsigned int>(mesh->Indices.size());
        }
    }

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnIndices / 3;
    g_FrameWaterSurfaces.clear();
}

bool D3D12GraphicsEngine::CreateParticlePipeline() {
    ID3D12Device* device = m_Device.GetDevice();

    // Root signature: b0 = ViewProj (16 root consts, VS), b1 = camera world pos (4 consts, VS), t0 =
    // diffuse SRV table (PS), static linear-clamp sampler s0 (PS). Particles sample [0,1] UVs, so CLAMP
    // avoids the billboard edge bleeding into the opposite side of the atlas frame.
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;         // t0
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0 ViewProj
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;   // b1 camera pos
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srvRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;              // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof( params );
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (particles)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: particle root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( m_ParticleRootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    UINT compileFlags = 0;
    if ( !CompileShaderD3D12( kParticleShaderSource, sizeof( kParticleShaderSource ) - 1, "ParticleShader", nullptr, nullptr,
        "VSMain", Shadermodel_VS, compileFlags, 0, m_ParticleVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !CompileShaderD3D12( kParticleShaderSource, sizeof( kParticleShaderSource ) - 1, "ParticleShader", nullptr, nullptr,
        "PSMain", Shadermodel_PS, compileFlags, 0, m_ParticlePsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // PSOs are built per blend state on demand (GetOrCreateParticlePipeline). Warm the alpha-blend one so
    // the common case never stalls at first draw.
    GothicBlendStateInfo defaultBlend;
    defaultBlend.SetAlphaBlending();
    if ( !GetOrCreateParticlePipeline( defaultBlend ) ) {
        LogWarn() << "D3D12: failed to create the default particle pipeline.";
        return false;
    }

    return CreateParticleInstanceBuffers();
}

bool D3D12GraphicsEngine::CreateParticleInstanceBuffers() {
    ID3D12Device* device = m_Device.GetDevice();
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kParticleInstanceBufferBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( m_ParticleInstanceBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_ParticleInstanceBuffer[i]->SetName( i == 0 ? L"ParticleInstanceRing0" : L"ParticleInstanceRing1" );
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( m_ParticleInstanceBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_ParticleInstanceBufferPtr[i] ) ) ) )
            return false;
    }
    m_ParticleInstanceBufferCapacity = kParticleInstanceBufferBytes;
    return true;
}

ID3D12PipelineState* D3D12GraphicsEngine::GetOrCreateParticlePipeline( const GothicBlendStateInfo& blend ) {
    const uint32_t key = BlendKey( blend );
    auto it = m_ParticlePipelines.find( key );
    if ( it != m_ParticlePipelines.end() ) return it->second.Get();

    // Fully per-instance layout: one ParticleInstanceInfo (56B) per particle, the VS expands the quad from
    // SV_VertexID. DIFFUSE is a real float4 here (not a packed DWORD), so R32G32B32A32_FLOAT.
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "DIFFUSE",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "SIZE",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "TYPE",     0, DXGI_FORMAT_R32_UINT,           0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "VELOCITY", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_ParticleRootSig.Get();
    pso.VS = { m_ParticleVsBlob->GetBufferPointer(), m_ParticleVsBlob->GetBufferSize() };
    pso.PS = { m_ParticlePsBlob->GetBufferPointer(), m_ParticlePsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;   // strips still use the TRIANGLE type
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = blend.BlendEnabled ? TRUE : FALSE;
    rt.SrcBlend = static_cast<D3D12_BLEND>(blend.SrcBlend);
    rt.DestBlend = static_cast<D3D12_BLEND>(blend.DestBlend);
    rt.BlendOp = static_cast<D3D12_BLEND_OP>(blend.BlendOp);
    rt.SrcBlendAlpha = static_cast<D3D12_BLEND>(blend.SrcBlendAlpha);
    rt.DestBlendAlpha = static_cast<D3D12_BLEND>(blend.DestBlendAlpha);
    rt.BlendOpAlpha = static_cast<D3D12_BLEND_OP>(blend.BlendOpAlpha);
    rt.RenderTargetWriteMask = blend.ColorWritesEnabled ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;

    // Reversed-Z: test GREATER_EQUAL against the opaque scene depth, but DO NOT write — particles are
    // transparent, must not occlude, and blend painter-style over whatever depth is already there.
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;

    ComPtr<ID3D12PipelineState> state;
    if ( FAILED( m_Device.GetDevice()->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( state.GetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed for particle blend key 0x" << std::hex << key << ".";
        return nullptr;
    }
    ID3D12PipelineState* raw = state.Get();
    m_ParticlePipelines.emplace( key, std::move( state ) );
    return raw;
}

XRESULT D3D12GraphicsEngine::DrawParticleEffects() {
    if ( !m_FrameOpen || !m_ParticleRootSig || !m_DepthBuffer )
        return XR_SUCCESS;

    auto& particles = Engine::GAPI->GetFrameParticles();
    auto& info = Engine::GAPI->GetFrameParticleInfo();

    // Clear the per-frame buckets BEFORE collecting. On D3D11 this happens in DrawWorldMeshNaive
    // (GothicAPI.cpp:1334) — but the D3D12 world pass never routes through it, so we must clear here or
    // the buckets accumulate every frame (particles smear, then the instance ring overflows and nothing
    // draws). DrawParticlesSimple appends into these, mirroring D3D11's clear-then-fill contract.
    particles.clear();
    info.clear();

    // Collect this frame's visible particle effects (backend-neutral). Fills FrameParticles (instances
    // bucketed by texture) + FrameParticleInfo (blend mode per texture). The mesh-PFX sub-call inside
    // (DrawFrameParticleMeshes) is a no-op on D3D12 — mesh-shaped effects are a later step.
    Engine::GAPI->DrawParticlesSimple();
    if ( particles.empty() ) return XR_SUCCESS;

    // ViewProj — identical derivation to DrawWorldMesh (particle positions are world-space).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    XMFLOAT3 camPos;
    XMStoreFloat3( &camPos, Engine::GAPI->GetCameraPositionXM() );
    const float camConsts[4] = { camPos.x, camPos.y, camPos.z, 0.0f };

    m_CmdList->SetGraphicsRootSignature( m_ParticleRootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 1, 4, camConsts, 0 );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    const UINT frame = m_FrameIndex;
    ID3D12PipelineState* lastPso = nullptr;
    unsigned int drawnTris = 0;

    for ( auto& [tex, instances] : particles ) {
        if ( instances.empty() ) continue;

        // Blend mode for this texture bucket (falls back to alpha blend if somehow unlisted).
        auto infoIt = info.find( tex );
        ID3D12PipelineState* pso = infoIt != info.end()
            ? GetOrCreateParticlePipeline( infoIt->second.BlendState )
            : nullptr;
        if ( !pso ) {
            GothicBlendStateInfo alpha; alpha.SetAlphaBlending();
            pso = GetOrCreateParticlePipeline( alpha );
        }
        if ( !pso ) continue;
        if ( pso != lastPso ) { m_CmdList->SetPipelineState( pso ); lastPso = pso; }

        const UINT numInstances = static_cast<UINT>(instances.size());
        const UINT instBytes = numInstances * static_cast<UINT>(sizeof( ParticleInstanceInfo ));
        if ( m_ParticleInstanceBufferOffset + instBytes > m_ParticleInstanceBufferCapacity ) {
            if ( !m_ParticleInstanceOverflowLogged ) {
                LogWarn() << "D3D12: particle instance ring overflow (" << m_ParticleInstanceBufferCapacity
                    << " bytes/frame). Some particles dropped this frame.";
                m_ParticleInstanceOverflowLogged = true;
            }
            break;
        }

        const UINT instOffset = m_ParticleInstanceBufferOffset;
        memcpy( m_ParticleInstanceBufferPtr[frame] + instOffset, instances.data(), instBytes );
        m_ParticleInstanceBufferOffset += instBytes;
        const D3D12_VERTEX_BUFFER_VIEW instView = {
            m_ParticleInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( ParticleInstanceInfo ) };
        m_CmdList->IASetVertexBuffers( 0, 1, &instView );

        D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
        if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
            if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
                if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                    D3D12Texture* d12 = D3D12Texture::From( gfx );
                    if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                }
            }
        }
        m_CmdList->SetGraphicsRootDescriptorTable( 2, srv );

        // 4-vertex triangle-strip quad, one draw per particle instance.
        m_CmdList->DrawInstanced( 4, numInstances, 0, 0 );
        drawnTris += 2 * numInstances;
    }

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnTris;
    return XR_SUCCESS;
}

// The decal input layout is shared by the lit + every transparent PSO: slot 0 = the unit quad
// (POSITION @0, TEXCOORD0 @12, stride 20), slot 1 = per-instance DecalInstanceInfo (world rows
// @0/16/32/48, color @64, stride 80).
static const D3D12_INPUT_ELEMENT_DESC kDecalInputLayout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    { "INSTANCE_COLOR",        0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
};

bool D3D12GraphicsEngine::CreateDecalPipeline() {
    ID3D12Device* device = m_Device.GetDevice();

    // Root signature: b0 = ViewProj (16 root consts, VS), t0 = diffuse SRV table (PS), static linear-clamp
    // sampler s0 (PS). CLAMP because a decal is a single [0,1] sprite; wrap would bleed the opposite edge.
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;         // t0
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0 ViewProj
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;              // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof( params );
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (decals)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: decal root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( m_DecalRootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    UINT compileFlags = 0;
    if ( !CompileShaderD3D12( kDecalShaderSource, sizeof( kDecalShaderSource ) - 1, "DecalShader", nullptr, nullptr,
        "VSMain", Shadermodel_VS, compileFlags, 0, m_DecalVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !CompileShaderD3D12( kDecalShaderSource, sizeof( kDecalShaderSource ) - 1, "DecalShader", nullptr, nullptr,
        "PSMainLit", Shadermodel_PS, compileFlags, 0, m_DecalLitPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !CompileShaderD3D12( kDecalShaderSource, sizeof( kDecalShaderSource ) - 1, "DecalShader", nullptr, nullptr,
        "PSMainBlend", Shadermodel_PS, compileFlags, 0, m_DecalBlendPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // Shared unit quad (two triangles, corners +/-0.5, UV 0..1) — same 6 verts as D3D11's decal quad, so
    // the per-decal scale matrix's Y-flip (-DecalSize.y*2) lands the sprite the same way.
    const DecalQuadVertex quad[6] = {
        { -0.5f, -0.5f, 0.0f, 0.0f, 0.0f },
        {  0.5f, -0.5f, 0.0f, 1.0f, 0.0f },
        { -0.5f,  0.5f, 0.0f, 0.0f, 1.0f },
        {  0.5f, -0.5f, 0.0f, 1.0f, 0.0f },
        {  0.5f,  0.5f, 0.0f, 1.0f, 1.0f },
        { -0.5f,  0.5f, 0.0f, 0.0f, 1.0f },
    };
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = sizeof( quad );
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if ( FAILED( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( m_DecalQuadVB.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_DecalQuadVB->SetName( L"DecalQuadVB" );
    void* mapped = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    if ( FAILED( m_DecalQuadVB->Map( 0, &noRead, &mapped ) ) ) return false;
    memcpy( mapped, quad, sizeof( quad ) );
    m_DecalQuadVB->Unmap( 0, nullptr );
    m_DecalQuadVBV = { m_DecalQuadVB->GetGPUVirtualAddress(), sizeof( quad ), sizeof( DecalQuadVertex ) };

    // Lit / opaque PSO: alpha-test cutout, depth test GREATER_EQUAL + WRITE (draws with the opaque scene).
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_DecalRootSig.Get();
    pso.VS = { m_DecalVsBlob->GetBufferPointer(), m_DecalVsBlob->GetBufferSize() };
    pso.PS = { m_DecalLitPsBlob->GetBufferPointer(), m_DecalLitPsBlob->GetBufferSize() };
    pso.InputLayout = { kDecalInputLayout, _countof( kDecalInputLayout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // decals are double-sided
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;   // reversed-Z; coplanar decals win ties
    pso.DepthStencilState.StencilEnable = FALSE;
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_DecalLitPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (decal lit).";
        return false;
    }

    // Warm the common transparent (alpha) PSO so the first blended decal never stalls.
    GothicBlendStateInfo defaultBlend;
    defaultBlend.SetAlphaBlending();
    if ( !GetOrCreateDecalBlendPipeline( defaultBlend ) ) {
        LogWarn() << "D3D12: failed to create the default decal blend pipeline.";
        return false;
    }

    return CreateDecalInstanceBuffers();
}

bool D3D12GraphicsEngine::CreateDecalInstanceBuffers() {
    ID3D12Device* device = m_Device.GetDevice();
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kDecalInstanceBufferBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( m_DecalInstanceBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_DecalInstanceBuffer[i]->SetName( i == 0 ? L"DecalInstanceRing0" : L"DecalInstanceRing1" );
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( m_DecalInstanceBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_DecalInstanceBufferPtr[i] ) ) ) )
            return false;
    }
    m_DecalInstanceBufferCapacity = kDecalInstanceBufferBytes;
    return true;
}

ID3D12PipelineState* D3D12GraphicsEngine::GetOrCreateDecalBlendPipeline( const GothicBlendStateInfo& blend ) {
    const uint32_t key = BlendKey( blend );
    auto it = m_DecalBlendPipelines.find( key );
    if ( it != m_DecalBlendPipelines.end() ) return it->second.Get();

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_DecalRootSig.Get();
    pso.VS = { m_DecalVsBlob->GetBufferPointer(), m_DecalVsBlob->GetBufferSize() };
    pso.PS = { m_DecalBlendPsBlob->GetBufferPointer(), m_DecalBlendPsBlob->GetBufferSize() };
    pso.InputLayout = { kDecalInputLayout, _countof( kDecalInputLayout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    // Gothic blend enums are laid out for D3D11, whose _BLEND/_OP values equal D3D12's — cast directly.
    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = blend.BlendEnabled ? TRUE : FALSE;
    rt.SrcBlend = static_cast<D3D12_BLEND>(blend.SrcBlend);
    rt.DestBlend = static_cast<D3D12_BLEND>(blend.DestBlend);
    rt.BlendOp = static_cast<D3D12_BLEND_OP>(blend.BlendOp);
    rt.SrcBlendAlpha = static_cast<D3D12_BLEND>(blend.SrcBlendAlpha);
    rt.DestBlendAlpha = static_cast<D3D12_BLEND>(blend.DestBlendAlpha);
    rt.BlendOpAlpha = static_cast<D3D12_BLEND_OP>(blend.BlendOpAlpha);
    rt.RenderTargetWriteMask = blend.ColorWritesEnabled ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;

    // Reversed-Z: test GREATER_EQUAL against the opaque scene, DO NOT write depth (transparent overlay).
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;

    ComPtr<ID3D12PipelineState> state;
    if ( FAILED( m_Device.GetDevice()->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( state.GetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed for decal blend key 0x" << std::hex << key << ".";
        return nullptr;
    }
    ID3D12PipelineState* raw = state.Get();
    m_DecalBlendPipelines.emplace( key, std::move( state ) );
    return raw;
}

void D3D12GraphicsEngine::DrawDecalList( const std::vector<zCVob*>& decals, bool lighting ) {
    if ( !m_FrameOpen || !m_DecalRootSig || !m_DecalLitPSO || !m_DepthBuffer ) return;
    if ( decals.empty() ) return;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    XMFLOAT3 camPos;
    XMStoreFloat3( &camPos, Engine::GAPI->GetCameraPositionXM() );

    // Build the per-decal instance data on the CPU (filter + camera alignment) — a direct port of D3D11's
    // DrawDecalList, minus the baked-in view matrix (the VS applies the standard ViewProj). The received
    // list is already sorted back-to-front; we keep that order (painter's algorithm) and DON'T batch.
    struct DecalMeta { zCTexture* texture; int alphaFunc; };
    static std::vector<DecalInstanceInfo> gpu;
    static std::vector<DecalMeta> meta;
    gpu.clear(); meta.clear();
    gpu.reserve( decals.size() ); meta.reserve( decals.size() );

    for ( zCVob* vob : decals ) {
        zCDecal* d = static_cast<zCDecal*>(vob->GetVisual());
        if ( !d ) continue;
        zCMaterial* material = d->GetDecalSettings()->DecalMaterial;
        if ( !material ) continue;
        zCTexture* texture = material->GetTextureSingle();
        if ( !texture ) continue;

        int alphaFunc = material->GetAlphaFunc();
        if ( alphaFunc == zMAT_ALPHA_FUNC_MAT_DEFAULT ) {
            alphaFunc = zMAT_ALPHA_FUNC_BLEND;
            if ( !texture->HasAlphaChannel() ) alphaFunc = zMAT_ALPHA_FUNC_NONE;
        }

        if ( lighting ) {
            // Opaque pass: only decals with no alpha or alpha test (blood, arrows).
            if ( !(alphaFunc == zMAT_ALPHA_FUNC_NONE || alphaFunc == zMAT_ALPHA_FUNC_TEST) ) continue;
        } else {
            // Transparent pass: only the supported blend modes; skip fully-transparent decals.
            switch ( alphaFunc ) {
            case zMAT_ALPHA_FUNC_BLEND:
            case zMAT_ALPHA_FUNC_BLEND_TEST:
            case zMAT_ALPHA_FUNC_ADD:
            case zMAT_ALPHA_FUNC_MUL:
            case zMAT_ALPHA_FUNC_MUL2:
                break;
            default:
                continue;
            }
            if ( (material->GetColor() >> 24) == 0 ) continue;
        }

        // Camera-alignment / world matrix — verbatim from D3D11 DrawDecalList (view is NOT applied here).
        int alignment = vob->GetAlignment();
        XMMATRIX world = vob->GetWorldMatrixXM();
        XMMATRIX offset =
            XMMatrixTranslation( d->GetDecalSettings()->DecalOffset.x, -d->GetDecalSettings()->DecalOffset.y, 0 );
        XMMATRIX scale =
            XMMatrixTranspose( XMMatrixScaling( d->GetDecalSettings()->DecalSize.x * 2,
                -d->GetDecalSettings()->DecalSize.y * 2, 1 ) );

        if ( alignment == zVISUAL_CAM_ALIGN_YAW ) {
            XMFLOAT3 decalPos = vob->GetPositionWorld();
            XMVECTOR at = XMVectorSet( decalPos.x - camPos.x, 0.0f, decalPos.z - camPos.z, 0.0f );
            XMFLOAT4 atLengthSq = {};
            XMStoreFloat4( &atLengthSq, XMVector3LengthSq( at ) );

            if ( atLengthSq.x > 1e-6f ) {
                XMMATRIX worldObj = XMMatrixTranspose( world );
                XMVECTOR translation = worldObj.r[3];

                at = XMVector3Normalize( at );
                XMVECTOR up = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
                XMVECTOR right = XMVector3Normalize( XMVector3Cross( up, at ) );
                up = XMVector3Normalize( XMVector3Cross( at, right ) );

                XMFLOAT3 right3 = {}, up3 = {}, at3 = {}, translation3 = {};
                XMStoreFloat3( &right3, right );
                XMStoreFloat3( &up3, up );
                XMStoreFloat3( &at3, at );
                XMStoreFloat3( &translation3, translation );

                worldObj.r[0] = XMVectorSet( right3.x, right3.y, right3.z, 0.0f );
                worldObj.r[1] = XMVectorSet( up3.x, up3.y, up3.z, 0.0f );
                worldObj.r[2] = XMVectorSet( at3.x, at3.y, at3.z, 0.0f );
                worldObj.r[3] = XMVectorSet( translation3.x, translation3.y, translation3.z, 1.0f );

                world = XMMatrixTranspose( worldObj );
            }
        } else if ( alignment == zVISUAL_CAM_ALIGN_FULL ) {
            XMFLOAT3 decalPos = vob->GetPositionWorld();
            XMVECTOR at = XMVectorSet( decalPos.x - camPos.x, decalPos.y - camPos.y, decalPos.z - camPos.z, 0.0f );
            XMFLOAT4 atLengthSq = {};
            XMStoreFloat4( &atLengthSq, XMVector3LengthSq( at ) );

            if ( atLengthSq.x > 1e-6f ) {
                at = XMVector3Normalize( at );
                XMVECTOR upRef = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
                XMFLOAT4 upDot = {};
                XMStoreFloat4( &upDot, XMVector3Dot( at, upRef ) );
                if ( fabsf( upDot.x ) > 0.999f ) upRef = XMVectorSet( 0.0f, 0.0f, 1.0f, 0.0f );

                XMVECTOR right = XMVector3Normalize( XMVector3Cross( upRef, at ) );
                XMVECTOR up = XMVector3Normalize( XMVector3Cross( at, right ) );

                XMFLOAT3 right3 = {}, up3 = {}, at3 = {};
                XMStoreFloat3( &right3, right );
                XMStoreFloat3( &up3, up );
                XMStoreFloat3( &at3, at );

                XMMATRIX worldObj;
                worldObj.r[0] = XMVectorSet( right3.x, right3.y, right3.z, 0.0f );
                worldObj.r[1] = XMVectorSet( up3.x, up3.y, up3.z, 0.0f );
                worldObj.r[2] = XMVectorSet( at3.x, at3.y, at3.z, 0.0f );
                worldObj.r[3] = XMVectorSet( decalPos.x, decalPos.y, decalPos.z, 1.0f );
                world = XMMatrixTranspose( worldObj );
            } else {
                world = XMMatrixTranspose( XMMatrixTranslation( decalPos.x, decalPos.y, decalPos.z ) );
            }
        }

        DecalInstanceInfo inst;
        XMStoreFloat4x4( &inst.World, world * offset * scale );
        const float ghostAlpha = lighting ? 1.0f : ((material->GetColor() >> 24) * (1.0f / 255.0f));
        inst.Color = XMFLOAT4( 1.0f, 1.0f, 1.0f, ghostAlpha );
        gpu.push_back( inst );
        meta.push_back( { material->GetAniTexture(), alphaFunc } );
    }

    if ( gpu.empty() ) return;

    // Snapshot the instances into this frame's ring; bind as the slot-1 stream (StartInstanceLocation picks
    // each decal, so we keep the exact submission order without splitting the memcpy).
    const UINT frame = m_FrameIndex;
    const UINT instBytes = static_cast<UINT>(gpu.size() * sizeof( DecalInstanceInfo ));
    if ( m_DecalInstanceBufferOffset + instBytes > m_DecalInstanceBufferCapacity ) {
        if ( !m_DecalInstanceOverflowLogged ) {
            LogWarn() << "D3D12: decal instance ring overflow (" << m_DecalInstanceBufferCapacity
                << " bytes/frame). Some decals dropped this frame.";
            m_DecalInstanceOverflowLogged = true;
        }
        return;
    }
    const UINT instOffset = m_DecalInstanceBufferOffset;
    memcpy( m_DecalInstanceBufferPtr[frame] + instOffset, gpu.data(), instBytes );
    m_DecalInstanceBufferOffset += instBytes;

    // ViewProj — identical derivation to the opaque passes (decal instance matrices are model-space).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = rs.TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetGraphicsRootSignature( m_DecalRootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_VERTEX_BUFFER_VIEW instView = {
        m_DecalInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( DecalInstanceInfo ) };
    const D3D12_VERTEX_BUFFER_VIEW views[2] = { m_DecalQuadVBV, instView };
    m_CmdList->IASetVertexBuffers( 0, 2, views );

    ID3D12PipelineState* lastPso = nullptr;
    if ( lighting ) { m_CmdList->SetPipelineState( m_DecalLitPSO.Get() ); lastPso = m_DecalLitPSO.Get(); }

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    zCTexture* lastTex = reinterpret_cast<zCTexture*>(~static_cast<uintptr_t>(0));  // force first bind
    unsigned int drawnTris = 0;

    for ( size_t i = 0; i < gpu.size(); ++i ) {
        if ( !lighting ) {
            GothicBlendStateInfo blend;
            switch ( meta[i].alphaFunc ) {
            case zMAT_ALPHA_FUNC_ADD:  blend.SetAdditiveBlending();  break;
            case zMAT_ALPHA_FUNC_MUL:  blend.SetModulateBlending();  break;
            case zMAT_ALPHA_FUNC_MUL2: blend.SetModulate2Blending(); break;
            default:                   blend.SetAlphaBlending();     break;  // BLEND / BLEND_TEST
            }
            ID3D12PipelineState* pso = GetOrCreateDecalBlendPipeline( blend );
            if ( !pso ) continue;
            if ( pso != lastPso ) { m_CmdList->SetPipelineState( pso ); lastPso = pso; }
        }

        zCTexture* tex = meta[i].texture;
        if ( tex != lastTex ) {
            D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
            if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
                    if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                        D3D12Texture* d12 = D3D12Texture::From( gfx );
                        if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                    }
                }
            }
            m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );
            lastTex = tex;
        }

        m_CmdList->DrawInstanced( 6, 1, 0, static_cast<UINT>(i) );
        drawnTris += 2;
    }

    rs.RendererInfo.FrameDrawnTriangles += drawnTris;
}

bool D3D12GraphicsEngine::CreateSkeletalPipeline() {
    ID3D12Device* device = m_Device.GetDevice();

    // Root signature: b0 = ViewProj (16 root 32-bit constants, VS); b1 = per-instance CBV (VS);
    // b2 = bone-palette CBV (VS); t0 = diffuse SRV table (PS); static linear-wrap sampler s0 (PS).
    // b1/b2 are root CBVs (raw GPU VAs into the per-frame skeletal ring) rather than root constants —
    // the bone palette (up to 96 matrices = 6 KB) far exceeds the 64-DWORD root-constant budget.
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;         // t0
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // CSM sampling (P2.9c-4b): shadow-map array SRV at t4 (skeletal PS samples it like world/VOB).
    D3D12_DESCRIPTOR_RANGE shadowSrvRange = {};
    shadowSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowSrvRange.NumDescriptors = 1;
    shadowSrvRange.BaseShaderRegister = 4;    // t4
    shadowSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    // Point-light shadow cube array SRV at t5 (P2.10d).
    D3D12_DESCRIPTOR_RANGE cubeSrvRange = {};
    cubeSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    cubeSrvRange.NumDescriptors = 1;
    cubeSrvRange.BaseShaderRegister = 5;      // t5
    cubeSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[13] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0 ViewProj
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;  // b1 per-instance
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 2;  // b2 bone palette
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &srvRange;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].Constants.ShaderRegister = 3;   // b3 fog
    params[4].Constants.Num32BitValues = 8;   // FogConstants (8 DWORDs)
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;  // VS: CamPosWS; PS: color/near/far
    // Forward+ point lights (mirrors m_WorldRootSig params 3/4/5/6, here at 5..8 — see BindFrameLights). All
    // MUST be bound at every skeletal draw or the PS light-loop bound/grid is undefined → GPU hang.
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[5].Descriptor.ShaderRegister = 1;  // t1 light StructuredBuffer (root SRV, no descriptor slot)
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[6].Constants.ShaderRegister = 4;   // b4 { LightCount, NumTilesX, pad, pad }
    params[6].Constants.Num32BitValues = 4;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[7].Descriptor.ShaderRegister = 2;  // t2 per-tile LightGrid {Offset,Count}
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[8].Descriptor.ShaderRegister = 3;  // t3 per-tile light-index list
    params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[9].Descriptor.ShaderRegister = 5;  // b5 shadow-sampling CB (skeletal's b3/b4 are fog/light count)
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[10].DescriptorTable.NumDescriptorRanges = 1;
    params[10].DescriptorTable.pDescriptorRanges = &shadowSrvRange;
    params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[11].DescriptorTable.NumDescriptorRanges = 1;
    params[11].DescriptorTable.pDescriptorRanges = &cubeSrvRange;   // t5 point-shadow cube array
    params[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[12].Constants.ShaderRegister = 6;   // b6 MaterialCB { MatNormalIndex, MatOrmIndex } — bindless indices
    params[12].Constants.Num32BitValues = 2;
    params[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    // s0 diffuse: 16x anisotropic (matches D3D11's main texture sampler) — sharpens surfaces at grazing
    // angles and in the distance, which trilinear alone smears badly.
    samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
    samplers[0].MaxAnisotropy = 16;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;          // s0 diffuse
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;   // s2 PCF (see world root sig)
    samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 2;          // s2 shadow comparison
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof( params );
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = _countof( samplers );
    rsDesc.pStaticSamplers = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                 | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;   // SM6.6 bindless normal/ORM

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (skeletal)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: skeletal root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( m_SkeletalRootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    UINT compileFlags = 0;
    if ( !CompileShaderD3D12( kSkeletalShaderSource, sizeof( kSkeletalShaderSource ) - 1, "SkeletalShader", nullptr, nullptr,
        "VSMain", Shadermodel_VS, compileFlags, 0, m_SkeletalVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !CompileShaderD3D12( kSkeletalShaderSource, sizeof( kSkeletalShaderSource ) - 1, "SkeletalShader", nullptr, nullptr,
        "PSMain", Shadermodel_PS, compileFlags, 0, m_SkeletalPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // Input layout = D3D11's layout3, explicit offsets into the 76-byte ExSkelVertexStruct:
    //   Position[4]   4x half4  (R16G16B16A16_FLOAT) @0/8/16/24  — vertex baked into each bone's space
    //   Normal        float3    @32
    //   BindPoseNormal float3   @44 (TEXCOORD0)
    //   TexCoord      float2    @56 (TEXCOORD1)
    //   boneIndices   uint8x4   @64 (BONEIDS)
    //   weights       half4     @68 (WEIGHTS)
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "POSITION", 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0,  8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "POSITION", 2, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "POSITION", 3, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BONEIDS",  0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 64, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "WEIGHTS",  0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 68, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_SkeletalRootSig.Get();
    pso.VS = { m_SkeletalVsBlob->GetBufferPointer(), m_SkeletalVsBlob->GetBufferSize() };
    pso.PS = { m_SkeletalPsBlob->GetBufferPointer(), m_SkeletalPsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // first-light; skinned winding varies
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed-Z
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_SkeletalPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (skeletal).";
        return false;
    }

    // Skeletal depth-prepass PSO (P2.9b-4b): same root sig + skinned input layout + depth state, but VSDepth/
    // PSDepthClip and color writes masked off. Lays down NPC/monster depth so the light cull bounds tiles to
    // them (fixing the near-skeletal cutoff). Same layout as the color PSO (VSDepth reads the same VS_IN).
    if ( !CompileShaderD3D12( kSkeletalShaderSource, sizeof( kSkeletalShaderSource ) - 1, "SkeletalShader",
        nullptr, nullptr, "VSDepth", Shadermodel_VS, compileFlags, 0, m_DepthPrepassSkeletalVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !CompileShaderD3D12( kSkeletalShaderSource, sizeof( kSkeletalShaderSource ) - 1, "SkeletalShader",
        nullptr, nullptr, "PSDepthClip", Shadermodel_PS, compileFlags, 0, m_DepthPrepassSkeletalPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    pso.VS = { m_DepthPrepassSkeletalVsBlob->GetBufferPointer(), m_DepthPrepassSkeletalVsBlob->GetBufferSize() };
    pso.PS = { m_DepthPrepassSkeletalPsBlob->GetBufferPointer(), m_DepthPrepassSkeletalPsBlob->GetBufferSize() };
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;   // DEPTH ONLY — discard color
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_DepthPrepassSkeletalPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (skeletal depth prepass).";
        return false;
    }
    return CreateSkeletalConstantBuffers();
}

bool D3D12GraphicsEngine::CreateSkeletalConstantBuffers() {
    ID3D12Device* device = m_Device.GetDevice();
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kSkeletalConstantBufferBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( m_SkeletalCBBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_SkeletalCBBuffer[i]->SetName( i == 0 ? L"SkeletalCBRing0" : L"SkeletalCBRing1" );
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( m_SkeletalCBBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_SkeletalCBBufferPtr[i] ) ) ) )
            return false;
    }
    m_SkeletalCBBufferCapacity = kSkeletalConstantBufferBytes;
    return true;
}

XRESULT D3D12GraphicsEngine::SetViewport( const ViewportInfo& vp ) {
    m_CurrentViewport.TopLeftX = static_cast<float>(vp.TopLeftX);
    m_CurrentViewport.TopLeftY = static_cast<float>(vp.TopLeftY);
    m_CurrentViewport.Width = static_cast<float>(vp.Width);
    m_CurrentViewport.Height = static_cast<float>(vp.Height);
    m_CurrentViewport.MinDepth = vp.MinZ;
    m_CurrentViewport.MaxDepth = vp.MaxZ;
    m_CurrentScissor = {
        static_cast<LONG>(vp.TopLeftX), static_cast<LONG>(vp.TopLeftY),
        static_cast<LONG>(vp.TopLeftX + vp.Width), static_cast<LONG>(vp.TopLeftY + vp.Height) };
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
    if ( !m_SwapChainReady || !m_FrameOpen || !m_UIRootSig || numVertices == 0 || !vertices )
        return XR_SUCCESS;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();

    // Emulate Gothic's per-draw fixed-function blend mode by selecting the matching PSO.
    ID3D12PipelineState* pso = GetOrCreateUIPipeline( rs.BlendState, rs.DepthState );
    if ( !pso ) return XR_SUCCESS;

    const UINT frame = m_FrameIndex;
    const UINT bytes = stride * numVertices;
    if ( m_UIVertexBufferOffset + bytes > m_UIVertexBufferCapacity ) {
        if ( !m_UIOverflowLogged ) {
            LogWarn() << "D3D12: 2D vertex ring overflow (" << m_UIVertexBufferCapacity
                << " bytes/frame). Some UI geometry dropped this frame.";
            m_UIOverflowLogged = true;
        }
        return XR_SUCCESS;
    }

    memcpy( m_UIVertexBufferPtr[frame] + m_UIVertexBufferOffset, vertices, bytes );
    const D3D12_GPU_VIRTUAL_ADDRESS gpuVA = m_UIVertexBuffer[frame]->GetGPUVirtualAddress() + m_UIVertexBufferOffset;
    m_UIVertexBufferOffset += bytes;

    m_CmdList->SetPipelineState( pso );
    m_CmdList->SetGraphicsRootSignature( m_UIRootSig.Get() );

    // The 2D/UI path is inherently full-screen: D3D11 forces a full-backbuffer viewport in OnBeginFrame
    // ("otherwise Gothic can't render its initial menu UI") and its BindViewportInformation reads that
    // live viewport. Gothic can leave a tiny/degenerate D3D7 viewport set at the menu (observed 1x1),
    // which would collapse the whole UI into a single pixel. Fall back to the full backbuffer when the
    // tracked viewport is degenerate, so the transform + rasterizer cover the screen like on D3D11.
    D3D12_VIEWPORT vp = m_CurrentViewport;
    D3D12_RECT     sc = m_CurrentScissor;
    if ( vp.Width < 2.0f || vp.Height < 2.0f ) {
        vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
        sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    }

    // Viewport transform constants (pixels / GothicUIScale), mirroring D3D11 BindViewportInformation.
    const float scale = std::max<float>( 0.001f, rs.RendererSettings.GothicUIScale );
    const float vpConsts[4] = {
        vp.TopLeftX / scale, vp.TopLeftY / scale,
        vp.Width / scale,    vp.Height / scale };
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 4, vpConsts, 0 );

    // Fixed-function stage state (b1) — same struct D3D11 binds as FFPipelineConstantBuffer.
    m_CmdList->SetGraphicsRoot32BitConstants( 2, sizeof( GothicGraphicsState ) / 4, &rs.GraphicsState, 0 );

    // Diffuse texture (fall back to 1x1 white for untextured colored draws / failed uploads).
    D3D12_GPU_DESCRIPTOR_HANDLE srv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    if ( m_CurrentTexture ) {
        D3D12Texture* t = D3D12Texture::From( m_CurrentTexture );
        if ( t->HasSRV() ) srv = t->GetSrvGpuHandle();
    }
    m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );

    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    D3D12_VERTEX_BUFFER_VIEW vbv = { gpuVA, bytes, stride };
    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    m_CmdList->DrawInstanced( numVertices, 1, startVertex, 0 );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += numVertices / 3;
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::DrawVertexBufferFF( GfxVertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
    if ( !vb || numVertices == 0 ) return XR_SUCCESS;

    // The D3D7 fixed-function vertex-buffer path (MyDirect3DDevice7::DrawPrimitiveVB) only ever feeds
    // Gothic_XYZRHW_DIF_T1_Vertex (28 bytes) — the sky dome + a few HUD strips. Rather than a second
    // input layout + VS variant, snapshot the CPU-side verts (the buffer is a persistently-mapped
    // upload resource, never GPU-bound here), convert to ExVertexStruct, and reuse the validated 2D/UI
    // draw path — exactly what DrawPrimitive does for the immediate (non-VB) case.
    if ( stride != sizeof( Gothic_XYZRHW_DIF_T1_Vertex ) )
        return XR_SUCCESS; // unknown FF-VB format — nothing else is emitted through this path

    const uint8_t* base = static_cast<const uint8_t*>(D3D12VertexBuffer::From( vb )->GetMappedData());
    if ( !base ) return XR_SUCCESS;

    const Gothic_XYZRHW_DIF_T1_Vertex* src =
        reinterpret_cast<const Gothic_XYZRHW_DIF_T1_Vertex*>(base + static_cast<size_t>(startVertex) * stride);

    static std::vector<ExVertexStruct> exv; // reused; the render path is single-threaded (matches DrawPrimitive)
    exv.resize( numVertices );
    for ( unsigned int i = 0; i < numVertices; ++i ) {
        exv[i].Position = src[i].xyz;
        exv[i].Normal.x = src[i].rhw;
        exv[i].TexCoord = src[i].texCoord;
        exv[i].Color = src[i].color;
    }

    return DrawVertexArray( exv.data(), numVertices, 0, sizeof( ExVertexStruct ) );
}

XRESULT D3D12GraphicsEngine::OnStartWorldRendering() {

    // m_PresentPending prevents inventory-world from rendering the whole game scenery for every inventory tile.
    // The engine sadly works like that.
    // the first OnStartWorldRendering after a Present() will be the correct one to draw the world.
    if ( m_PresentPending ) return XR_SUCCESS;

    // zCBspNodeRender hook — Gothic's BSP traversal is replaced; we draw the world ourselves.
    // Order mirrors D3D11's DrawWorldMeshNaive: sky background, world mesh, skeletal (NPCs/monsters),
    // then instanced static VOBs. The sky is a fog-colored fill so the horizon dissolves into the
    // per-pixel distance fog of the geometry.

    // Collect the frame's visible vobs/lights/mobs ONCE (CollectVisibleVobs has side effects — it fills each
    // visual's Instances list — so it must run exactly once), then rebuild the per-frame point-light buffer so
    // every geometry pass (world, VOBs, skeletal) lights against the same visible-light set. Mirrors D3D11
    // filling m_FrameLights during collection.
    g_FrameVobs.clear(); g_FrameLights.clear(); g_FrameMobs.clear();
    Engine::GAPI->CollectVisibleVobs( g_FrameVobs, g_FrameLights, g_FrameMobs );
    BuildFrameLightBuffer();
    // Snapshot ALL opaque instanced geometry ONCE, before the depth prepass + cull, so every geometry pass draws
    // from one shared upload: VOB instances (g_FrameVobUploads), then skeletal base/attachment CBs + instances
    // (g_FrameSkelDraws/g_FrameAttachDraws — PrepareFrameSkeletals also runs the once/frame animation update, so
    // it MUST run exactly once). Both skeletal lists (animated + static mobs) are prepared here up front.
    UploadFrameVobInstances();
    g_FrameSkelDraws.clear(); g_FrameAttachDraws.clear();
    PrepareFrameSkeletals( Engine::GAPI->GetAnimatedSkeletalMeshVobs() );
    PrepareFrameSkeletals( g_FrameMobs );

    // Phase 3 HDR: redirect the whole 3D scene into the R16F scene-color target (OnBeginFrame bound the
    // swapchain for 2D-only/menu frames; here we switch to HDR so lighting can exceed 1.0). Depth is shared.
    BindSceneColorTarget();

    DrawSky();
    // CSM sun shadows (P2.9c): render the opaque casters into the cascade shadow map from the sun's POV. Runs
    // before the main geometry; rebinds the backbuffer RT/DSV when done. Nothing samples the map yet (later
    // increment), so this is visually a no-op — inspect the shadow Texture2DArray in RenderDoc.
    RenderSunShadows();
    // Point-light shadow cubes (P2.10): render each selected shadowed light's 6 faces into the shared cube array.
    // Runs before the lit passes that sample it; leaves the backbuffer RT/DSV rebound. Visually a no-op until the
    // point-light loop samples the cubes (P2.10d).
    RenderPointShadows();
    // Forward+ opaque depth prepass — lays down ALL opaque depth before the lit passes so the tiled light cull
    // bounds each tile to real geometry: world mesh, then instanced VOBs, then skeletal (NPCs/monsters) + node
    // attachments. Visually a no-op (the color passes re-pass on GREATER_EQUAL and rewrite the same depth).
    // Build the world-mesh ExecuteIndirect command set ONCE (P2.11) — the shared visible-section collection +
    // per-material bindless-index resolution + water peel-out. Both the depth prepass and the color pass draw
    // from it, so the BSP walk happens once (was per-pass) and neither pass issues per-material CPU draw calls.
    BuildWorldDrawCommands();
    DrawDepthPrepass();
    DrawVobDepthPrepass();
    DrawSkeletalDepthPrepass();
    // Forward+ tiled light cull: consume this frame's light buffer + the prepass depth to record which point
    // lights touch each 16x16 screen tile (bounded to real geometry on both the near and far side).
    DispatchLightCulling();
    DrawWorldMesh();
    {
        DX_ZONE( m_CmdList, "Draw skeletal (color)" );
        DrawSkeletalColor();   // base meshes + node attachments, lit through the tile grid (both lists)
    }
    DrawVobsInstanced();

    // Decals (blood, arrows, sprites): collect the visible, back-to-front-sorted list once, then draw the
    // opaque/alpha-test ones here (with the opaque scene, depth-write) and the transparent ones after water
    // (blended over the finished scene). Mirrors D3D11's two-pass DrawDecalList.
    static std::vector<zCVob*> decals;
    decals.clear();
    Engine::GAPI->GetVisibleDecalList( decals );
    {
        DX_ZONE( m_CmdList, "Draw decals (opaque)" );
        DrawDecalList( decals, true );
    }

    // Water: alpha-blended over the finished opaque scene (world + NPCs + VOBs + opaque decals).
    DrawWaterSurfaces();

    {
        DX_ZONE( m_CmdList, "Draw decals (transparent)" );
        DrawDecalList( decals, false );
    }

    // Particles last: billboarded PFX (fire, smoke, magic, dust) blended over everything, depth-tested
    // against the opaque scene but not writing depth. Mirrors D3D11's late DrawParticlesSimple pass.
    {
        DX_ZONE( m_CmdList, "Draw particles" );
        DrawParticleEffects();
    }

    // Clear the per-visual instance lists so next frame's CollectVisibleVobs starts fresh (mirrors D3D11).
    // Done here (not in DrawVobsInstanced) so it runs even when DrawVOBs is off and that pass early-outs.
    for ( auto const& [visualPtr, visual] : Engine::GAPI->GetStaticMeshVisuals() ) {
        if ( visual ) visual->Instances.clear();
    }

    // Phase 3 HDR: the 3D scene is complete — tonemap the HDR target into the swapchain and rebind the backbuffer
    // so Gothic's subsequent 2D UI/HUD draws (and the ImGui overlay in Present) composite on top in LDR.
    ResolveSceneToBackBuffer();

    // Do any remaining dx12 stuff BEFORE setting PresentPending

    m_PresentPending = true;

    // After this point, we hand over to Gothics UI rendering

    // TODO: the inventory rendering code requires DrawVobSingle which is currently not implemented in dx12

    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::DrawSky() {
    if ( !m_FrameOpen ) return XR_SUCCESS;
    DX_ZONE( m_CmdList, "Draw sky" );
    // Forward-renderer sky MVP: fill the backbuffer with Gothic's atmosphere (fog) color. Runs at the
    // start of the world pass — after OnBeginFrame's black clear, before any 3D geometry — so wherever
    // no geometry draws (above the horizon) the sky shows this color, and the geometry shaders' distance
    // fog fades into the same color so the horizon dissolves seamlessly. Depth (cleared to 0.0 = far in
    // OnBeginFrame) is left untouched, so geometry still depth-tests / occludes normally.
    XMFLOAT3 fc;
    XMStoreFloat3( &fc, Engine::GAPI->GetFogColor() );

    // The HDR scene target is LINEAR (the lit passes sRGB-decode their albedo + linearize FogColor), so the sky
    // fill must be linear too — otherwise the tonemap (ACES + sRGB-encode) would double-process the sky/fog and it
    // wouldn't match the geometry's distance-fog fade. sRGB->linear each channel on the CPU before clearing.
    auto srgbToLinear = []( float c ) { return c <= 0.04045f ? c / 12.92f : std::pow( ( c + 0.055f ) / 1.055f, 2.4f ); };
    const float clear[4] = { srgbToLinear( fc.x ), srgbToLinear( fc.y ), srgbToLinear( fc.z ), 1.0f };

    // Clear the HDR scene-color target (bound by BindSceneColorTarget just before this) to the fog color — the
    // geometry's distance fog fades into the same value so the horizon dissolves; the tonemap resolves it later.
    m_CmdList->ClearRenderTargetView( m_SceneColorRtv, clear, 0, nullptr );
    return XR_SUCCESS;
}

bool D3D12GraphicsEngine::CreateWorldIndirect() {
    // Command signature + per-frame UPLOAD arg ring for the GPU-driven world mesh (P2.11). One command sets the
    // b6 material bindless indices (3 root constants @ param 10 of m_WorldRootSig) then issues a DrawIndexed. Both
    // the depth prepass and the color pass ExecuteIndirect over the SAME per-frame buffer (identical opaque draw
    // set — water peeled at build time). The arg buffer is UPLOAD (permanently GENERIC_READ, which INCLUDES
    // INDIRECT_ARGUMENT), rebuilt each frame by BuildWorldDrawCommands — no DEFAULT-heap copy needed.
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_WorldRootSig ) return false;

    D3D12_INDIRECT_ARGUMENT_DESC args[2] = {};
    args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    args[0].Constant.RootParameterIndex = 10;                 // b6 MaterialCB (world root sig)
    args[0].Constant.DestOffsetIn32BitValues = 0;
    args[0].Constant.Num32BitValuesToSet = 3;                 // normal, orm, diffuse
    args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
    sigDesc.ByteStride = sizeof( WorldDrawCommand );          // 32 B; MUST match the struct + shader layout
    sigDesc.NumArgumentDescs = _countof( args );
    sigDesc.pArgumentDescs = args;
    // A command that sets root constants must carry the root signature its param index refers to.
    if ( FAILED( device->CreateCommandSignature( &sigDesc, m_WorldRootSig.Get(),
        IID_PPV_ARGS( m_WorldIndirectCmdSig.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: failed to create the world indirect command signature.";
        return false;
    }

    D3D12_HEAP_PROPERTIES upload = {};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>( kMaxWorldDrawCommands ) * sizeof( WorldDrawCommand );
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommittedResource( &upload, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( m_WorldDrawArgs[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_WorldDrawArgs[i]->SetName( L"WorldDrawArgsRing" );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( m_WorldDrawArgs[i]->Map( 0, &noRead, &mapped ) ) ) return false;
        m_WorldDrawArgsPtr[i] = static_cast<uint8_t*>( mapped );
        m_WorldDrawArgsGpu[i] = m_WorldDrawArgs[i]->GetGPUVirtualAddress();
    }
    return true;
}

void D3D12GraphicsEngine::BuildWorldDrawCommands() {
    // Build this frame's world-mesh ExecuteIndirect command set ONCE (P2.11): frustum-collect the visible sections,
    // then per non-water material append { bindless material indices, DrawIndexedArguments (its index range into
    // the shared world VB/IB) }. Water is peeled here into g_FrameWaterSurfaces (drawn later, alpha-blended). Both
    // world passes then consume the result, so the BSP walk + per-material CacheIn happen once/frame (was 2-3x).
    m_WorldDrawCount = 0;
    m_WorldDrawnIndices = 0;
    g_FrameWaterSurfaces.clear();
    if ( !m_FrameOpen || !m_WorldIndirectCmdSig || !m_WorldDrawArgsPtr[m_FrameIndex] ) return;

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    if ( !wm || !wm->GetMeshVertexBuffer() || !wm->GetMeshIndexBuffer() ) return;

    // Camera setup identical to the passes so CollectVisibleSections culls against the same frustum.
    Engine::GAPI->SetViewTransformXM( Engine::GAPI->GetViewMatrixXM() );
    Engine::GAPI->ResetWorldTransform();

    static std::vector<WorldMeshSectionInfo*> sections;
    sections.clear();
    Engine::GAPI->CollectVisibleSections( sections, nullptr, true );

    WorldDrawCommand* cmds = reinterpret_cast<WorldDrawCommand*>( m_WorldDrawArgsPtr[m_FrameIndex] );
    UINT count = 0;
    
    Frustum playerFrustum = Frustum::AlwaysContainingFrustum();
    if ( auto cam = (zCCamera*)oCGame::GetGame()->_zCSession_camera ) {
        const auto& view = cam->trafoView; // Column-Major, needs Transpose for DxMath
        const auto& proj = cam->trafoProjection; // Row-Major, does not need transpose.
        playerFrustum.BuildPerspective(
            XMMatrixTranspose( XMLoadFloat4x4( &view ) ),
            XMLoadFloat4x4( &proj )
        );
    }

    for ( WorldMeshSectionInfo* section : sections ) {
        if ( !section ) continue;
        for ( auto const& [meshKey, mesh] : section->WorldMeshes ) {
            if ( !mesh || mesh->Indices.empty() ) continue;
            
            if ( !Engine::GAPI->IsWorldMeshVisibleInFrustum( mesh, playerFrustum ) ) {
                continue;
            }            

            // Water is transparent — bucket it by texture for the later alpha-blended pass, skip the opaque command set.
            if ( meshKey.Info && meshKey.Info->MaterialType == MaterialInfo::MT_Water ) {
                g_FrameWaterSurfaces[meshKey.Material->GetAniTexture()].push_back( mesh );
                continue;
            }
            if ( count >= kMaxWorldDrawCommands ) {
                if ( !m_WorldDrawArgsOverflowLogged ) {
                    LogWarn() << "D3D12: world draw-command ring overflow (" << kMaxWorldDrawCommands
                        << " draws/frame); some world materials dropped this frame.";
                    m_WorldDrawArgsOverflowLogged = true;
                }
                break;
            }

            // Resolve this material's bindless SRV heap indices — diffuse (CacheIn triggers load + its normal/ORM
            // side-loads), normal (0xFFFFFFFF = none → PS skips perturb), ORM (1x1 default when the material has no _FX).
            zCTexture* tex = meshKey.Material->GetAniTexture();
            uint32_t diffuseIdx = m_BlackTexture->GetSrvSlot();
            uint32_t normalIdx  = 0xFFFFFFFFu;
            uint32_t ormIdx     = m_DefaultOrmTexture->GetSrvSlot();
            if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                if ( MyDirectDrawSurface7* s = tex->GetSurface() ) {
                    if ( GfxTexture* gfx = s->GetEngineTexture() ) {
                        D3D12Texture* d = D3D12Texture::From( gfx );
                        if ( d->HasSRV() ) diffuseIdx = d->GetSrvSlot();
                    }
                    if ( GfxTexture* n = s->GetNormalmap() ) {
                        D3D12Texture* d = D3D12Texture::From( n );
                        if ( d->HasSRV() ) normalIdx = d->GetSrvSlot();
                    }
                    if ( GfxTexture* o = s->GetFxMap() ) {
                        D3D12Texture* d = D3D12Texture::From( o );
                        if ( d->HasSRV() ) ormIdx = d->GetSrvSlot();
                    }
                }
            }

            WorldDrawCommand& c = cmds[count];
            c.MatNormalIndex  = normalIdx;
            c.MatOrmIndex     = ormIdx;
            c.MatDiffuseIndex = diffuseIdx;
            c.Draw.IndexCountPerInstance = static_cast<UINT>( mesh->Indices.size() );
            c.Draw.InstanceCount = 1;
            c.Draw.StartIndexLocation = mesh->BaseIndexLocation;
            c.Draw.BaseVertexLocation = 0;
            c.Draw.StartInstanceLocation = 0;
            ++count;
            m_WorldDrawnIndices += static_cast<unsigned int>( mesh->Indices.size() );
        }
    }
    m_WorldDrawCount = count;
}

void D3D12GraphicsEngine::DrawDepthPrepass() {
    // Forward+ opaque depth prepass (P2.9b-1). Lays down the opaque WORLD-MESH depth before the lit color
    // passes, so the tiled light-culling compute (P2.9b-2) can read a populated depth buffer to tighten each
    // tile's frustum. Writes depth only (color write mask 0). Runs after DrawSky (color cleared, depth still
    // at the OnBeginFrame clear of 0.0) and before DrawWorldMesh. The main opaque passes keep GREATER_EQUAL +
    // depth-write, so they re-pass on equal depth and rewrite the same value — the frame is visually identical.
    //
    // Scope: WORLD MESH ONLY this increment. Instanced VOBs + skeletal NPCs still get their depth from their
    // own (unchanged) color passes; they'll be added to the prepass alongside the cull consumer (P2.9b-2),
    // where the VOB instance-ring offset sharing gets designed together with the tile grid. Water is skipped
    // (transparent — it never writes depth, same as the opaque pass peels it out).
    if ( !m_FrameOpen || !m_DepthPrepassWorldPSO || !m_WorldRootSig || !m_DepthBuffer )
        return;

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    if ( !wm || !wm->GetMeshVertexBuffer() || !wm->GetMeshIndexBuffer() )
        return;

    D3D12VertexBuffer* vb = D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() );
    D3D12VertexBuffer* ib = D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() );
    if ( !vb->GetResource() || !ib->GetResource() ) return;
    if ( ib->GetSizeInBytes() / sizeof( uint32_t ) == 0 ) return;

    DX_ZONE( m_CmdList, "Depth Prepass (world)" );

    // ViewProj — identical derivation to DrawWorldMesh so the prepass depth matches the opaque pass exactly.
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetPipelineState( m_DepthPrepassWorldPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_WorldRootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );   // b0 ViewProj (fog/lights not referenced)

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
    D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    m_CmdList->IASetIndexBuffer( &ibv );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // GPU-driven submit (P2.11): one ExecuteIndirect over the shared per-frame command buffer that
    // BuildWorldDrawCommands filled (same opaque draw set as the color pass; water already peeled). Each command
    // sets the b6 diffuse index (PSClip alpha-clips bindless) then draws its material's index range. Replaces the
    // per-material descriptor-table binds + DrawIndexedInstanced calls (the CPU cost this optimization targets).
    if ( m_WorldDrawCount == 0 ) return;
    m_CmdList->ExecuteIndirect( m_WorldIndirectCmdSig.Get(), m_WorldDrawCount,
        m_WorldDrawArgs[m_FrameIndex].Get(), 0, nullptr, 0 );
}

void D3D12GraphicsEngine::DispatchLightCulling() {
    // Forward+ tiled light cull (P2.9b-2). Runs after the depth prepass, before the lit passes: one 16x16
    // thread group per screen tile writes {Offset,Count} into m_LightGridBuffer and the touching light
    // indices into m_LightIndexBuffer. This increment only PRODUCES the grid — nothing consumes it yet, so
    // the frame is visually unchanged; verify by inspecting the two buffers in RenderDoc (per-tile Count
    // should be non-zero where torches/spells are on screen, zero for empty sky). The lit pixel shaders read
    // this grid instead of looping all lights. The cull reads the prepass depth (transitioned to a compute SRV
    // and back below) to clamp each tile's FAR-Z to real geometry — see the shader header for why far-only.
    if ( !m_FrameOpen || !m_LightCullPSO || !m_LightCullRootSig || !m_LightGridBuffer || !m_LightIndexBuffer )
        return;
    if ( !m_LightBuffer[m_FrameIndex] || m_NumTilesX == 0 || m_NumTilesY == 0 )
        return;

    DX_ZONE( m_CmdList, "Light Culling (compute)" );

    // The lit geometry passes left the grid/index buffers in PIXEL_SHADER_RESOURCE last frame; transition them
    // back to UNORDERED_ACCESS so the cull CS can write them as root UAVs. Skipped on the first dispatch after
    // (re)creation, when they're already in UAV (see CreateLightCullBuffers / m_LightGridInPixelState).
    if ( m_LightGridInPixelState ) {
        D3D12_RESOURCE_BARRIER toUav[2] = {};
        for ( int i = 0; i < 2; ++i ) {
            toUav[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toUav[i].Transition.pResource = ( i == 0 ? m_LightGridBuffer : m_LightIndexBuffer ).Get();
            toUav[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toUav[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            toUav[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        m_CmdList->ResourceBarrier( 2, toUav );
        m_LightGridInPixelState = false;
    }

    // ProjScale = the projection's x/y view->clip scale (diagonal terms; layout-invariant so no transpose
    // worry). ProjA/ProjB = the z-row terms (_33, _43) the CS inverts to turn a reversed-Z depth sample into
    // a view-space Z (viewZ = ProjB / (depth - ProjA)); same scalars read straight off the CPU matrix, so no
    // transpose concern either. Same projection the geometry passes use.
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();

    struct LightCullConstants {
        float    ProjScaleX, ProjScaleY;
        uint32_t ScreenX, ScreenY;
        uint32_t TotalLights;
        uint32_t NumTilesX;
        float    ProjA, ProjB;   // projM._33, projM._43 (reversed-Z depth -> viewZ reconstruction)
    } cb{};
    cb.ProjScaleX = projM._11;
    cb.ProjScaleY = projM._22;
    cb.ScreenX = static_cast<uint32_t>( m_Resolution.x );
    cb.ScreenY = static_cast<uint32_t>( m_Resolution.y );
    cb.TotalLights = m_FrameLightCount;
    cb.NumTilesX = m_NumTilesX;
    cb.ProjA = projM._33;
    cb.ProjB = projM._43;

    // The depth prepass left the depth buffer in DEPTH_WRITE; make it readable by this compute pass, then hand
    // it back to DEPTH_WRITE below so the lit passes (GREATER_EQUAL depth-write) see it exactly as before.
    D3D12_RESOURCE_BARRIER depthToSrv = {};
    depthToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    depthToSrv.Transition.pResource = m_DepthBuffer.Get();
    depthToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    depthToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    depthToSrv.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_CmdList->ResourceBarrier( 1, &depthToSrv );

    m_CmdList->SetPipelineState( m_LightCullPSO.Get() );
    m_CmdList->SetComputeRootSignature( m_LightCullRootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 8, &cb, 0 );
    m_CmdList->SetComputeRootShaderResourceView( 1, m_LightBuffer[m_FrameIndex]->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootUnorderedAccessView( 2, m_LightGridBuffer->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootUnorderedAccessView( 3, m_LightIndexBuffer->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootDescriptorTable( 4, GetSrvGpuHandle( m_DepthSrvSlot ) );   // t1 DepthTex (SRV heap already bound)
    m_CmdList->Dispatch( m_NumTilesX, m_NumTilesY, 1 );

    D3D12_RESOURCE_BARRIER depthToWrite = depthToSrv;
    depthToWrite.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    depthToWrite.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    m_CmdList->ResourceBarrier( 1, &depthToWrite );

    // Transition both buffers UNORDERED_ACCESS -> PIXEL_SHADER_RESOURCE so the lit world/VOB/skeletal passes
    // can read them as root SRVs. The transition barrier also orders those reads after these UAV writes (no
    // separate UAV barrier needed). Reverted at the top of next frame's dispatch.
    D3D12_RESOURCE_BARRIER toSrv[2] = {};
    for ( int i = 0; i < 2; ++i ) {
        toSrv[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toSrv[i].Transition.pResource = ( i == 0 ? m_LightGridBuffer : m_LightIndexBuffer ).Get();
        toSrv[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toSrv[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toSrv[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    m_CmdList->ResourceBarrier( 2, toSrv );
    m_LightGridInPixelState = true;
}

void D3D12GraphicsEngine::BindMaterialMaps( zCTexture* tex, UINT matRootParam ) {
    // Set the per-material bindless indices (b6 root consts) the PBR PS reads via ResourceDescriptorHeap[...]:
    // this material's normal + ORM map SRV heap slots (loaded onto the surface by LoadAdditionalResources).
    // No normal map -> 0xFFFFFFFF (PS skips the perturb); no _FX/ORM map -> the 1x1 default ORM slot.
    UINT idx[2] = { 0xFFFFFFFFu, m_DefaultOrmTexture->GetSrvSlot() };
    if ( tex ) {
        if ( MyDirectDrawSurface7* s = tex->GetSurface() ) {
            if ( GfxTexture* n = s->GetNormalmap() ) {
                D3D12Texture* d = D3D12Texture::From( n );
                if ( d->HasSRV() ) idx[0] = d->GetSrvSlot();
            }
            if ( GfxTexture* o = s->GetFxMap() ) {
                D3D12Texture* d = D3D12Texture::From( o );
                if ( d->HasSRV() ) idx[1] = d->GetSrvSlot();
            }
        }
    }
    m_CmdList->SetGraphicsRoot32BitConstants( matRootParam, 2, idx, 0 );
}

XRESULT D3D12GraphicsEngine::DrawWorldMesh( bool /*noTextures*/ ) {
    if ( !m_FrameOpen || !m_WorldPSO || !m_WorldRootSig || !m_DepthBuffer )
        return XR_SUCCESS;

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    if ( !wm || !wm->GetMeshVertexBuffer() || !wm->GetMeshIndexBuffer() )
        return XR_SUCCESS;

    D3D12VertexBuffer* vb = D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() );
    D3D12VertexBuffer* ib = D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() );
    if ( !vb->GetResource() || !ib->GetResource() ) return XR_SUCCESS;

    // The wrapped world index buffer is a single merged 32-bit buffer (bound R32_UINT, like D3D11).
    const UINT numIndices = ib->GetSizeInBytes() / sizeof( uint32_t );
    if ( numIndices == 0 ) return XR_SUCCESS;

    DX_ZONE( m_CmdList, "Draw World Mesh" );

    // Camera matrices — replicate the D3D11 DrawWorldMesh setup exactly so ViewProj is byte-identical:
    // world verts are already world-space (identity world), transform is proj*view (reversed-Z).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    const FogConstants fog = MakeFogConstants();

    m_CmdList->SetPipelineState( m_WorldPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_WorldRootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );   // b1 fog
    BindFrameLights();   // param 3 = light SRV (t1), param 4 = light count (b2) — MUST set both or the
                         // shader's light loop reads a garbage count and runs away (GPU TDR hang).
    // CSM sampling: param 7 = shadow CB (b3), param 8 = shadow-map array SRV (t4). The map was left in
    // PIXEL_SHADER_RESOURCE by RenderSunShadows; the PS samples it to darken sun-occluded surfaces.
    m_CmdList->SetGraphicsRootConstantBufferView( 7, m_ShadowCBGpu[m_FrameIndex] );
    m_CmdList->SetGraphicsRootDescriptorTable( 8, GetSrvGpuHandle( m_ShadowSrvSlot ) );
    m_CmdList->SetGraphicsRootDescriptorTable( 9, GetSrvGpuHandle( m_PointShadowSrvSlot ) );   // t5 point-shadow cubes

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
    D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    m_CmdList->IASetIndexBuffer( &ibv );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // GPU-driven submit (P2.11): one ExecuteIndirect over the shared per-frame command buffer (built by
    // BuildWorldDrawCommands before the depth prepass; water already peeled into g_FrameWaterSurfaces). Each
    // command sets this material's b6 { normal, orm, diffuse } bindless indices then draws its index range — so
    // the whole opaque world is one API call with zero per-draw descriptor binds (the CPU cost this targets).
    // Frame-constant root args (b0 ViewProj, b1 fog, lights, CSM, point-shadow cubes) are already set above.
    if ( m_WorldDrawCount == 0 ) return XR_SUCCESS;
    m_CmdList->ExecuteIndirect( m_WorldIndirectCmdSig.Get(), m_WorldDrawCount,
        m_WorldDrawArgs[m_FrameIndex].Get(), 0, nullptr, 0 );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += m_WorldDrawnIndices / 3;
    return XR_SUCCESS;
}

bool D3D12GraphicsEngine::UploadVobs(
    const std::vector<RenderBucket>& vobs,
    std::vector<FrameVobUpload>& uploads) {
    if ( !m_FrameOpen || !m_VobInstanceBuffer[m_FrameIndex] || !m_VobInstanceBufferPtr[m_FrameIndex] )
        return false;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    if ( !rs.RendererSettings.DrawVOBs )
        return false;

    const UINT frame = m_FrameIndex;
    
    bool hasInstances = false;
    thread_local std::vector<VobInstanceInfo> instances;
    for ( size_t i = 0; i < vobs.size(); ++i) {
        auto& bucket = vobs[i];
        auto& instances = bucket.instances;
        if (instances.empty()) {
            continue;
        }

        const UINT numInstances = instances.size();
        const UINT instBytes = numInstances * sizeof( VobInstanceInfo );

        if ( m_VobInstanceBufferOffset + instBytes > m_VobInstanceBufferCapacity ) {
            if ( !m_VobInstanceOverflowLogged ) {
                LogWarn() << "D3D12: VOB instance ring overflow (" << m_VobInstanceBufferCapacity
                    << " bytes/frame). Some VOBs dropped this frame.";
                m_VobInstanceOverflowLogged = true;
            }
            break;
        }
        hasInstances = true;

        const UINT instOffset = m_VobInstanceBufferOffset;
        memcpy( m_VobInstanceBufferPtr[frame] + instOffset, instances.data(), instBytes );
        m_VobInstanceBufferOffset += instBytes;

        FrameVobUpload up;
        up.visual = reinterpret_cast<MeshVisualInfo*>(g_vobInfoVisualIndexToVisualInfo[i]);
        up.instView = { m_VobInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( VobInstanceInfo ) };
        up.numInstances = numInstances;
        uploads.push_back( up );
    }
    
    return hasInstances;
}

void D3D12GraphicsEngine::UploadFrameVobInstances() {
    // P2.9b-4a: snapshot every visible VOB visual's instances into the per-frame instance ring ONCE, before the
    // depth prepass and the light cull. DrawVobDepthPrepass and DrawVobsInstanced both draw from g_FrameVobUploads,
    // so the ring is filled a single time (the color pass adds no upload → its ring usage is unchanged). Gated on
    // DrawVOBs so that with VOBs disabled neither their depth nor their color is laid down (the cull must never
    // tighten a tile to geometry that isn't actually drawn). The per-frame ring offset was reset in OnBeginFrame.
    g_FrameVobUploads.clear();
    if ( !m_FrameOpen || !m_VobInstanceBuffer[m_FrameIndex] || !m_VobInstanceBufferPtr[m_FrameIndex] )
        return;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    if ( !rs.RendererSettings.DrawVOBs )
        return;

    const UINT frame = m_FrameIndex;
    for ( auto const& [visualPtr, visual] : Engine::GAPI->GetStaticMeshVisuals() ) {
        if ( !visual || visual->Instances.empty() ) continue;

        const UINT numInstances = static_cast<UINT>( visual->Instances.size() );
        const UINT instBytes = numInstances * static_cast<UINT>( sizeof( VobInstanceInfo ) );

        if ( m_VobInstanceBufferOffset + instBytes > m_VobInstanceBufferCapacity ) {
            if ( !m_VobInstanceOverflowLogged ) {
                LogWarn() << "D3D12: VOB instance ring overflow (" << m_VobInstanceBufferCapacity
                    << " bytes/frame). Some VOBs dropped this frame.";
                m_VobInstanceOverflowLogged = true;
            }
            break;
        }

        const UINT instOffset = m_VobInstanceBufferOffset;
        memcpy( m_VobInstanceBufferPtr[frame] + instOffset, visual->Instances.data(), instBytes );
        m_VobInstanceBufferOffset += instBytes;

        FrameVobUpload up;
        up.visual = visual;
        up.instView = { m_VobInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( VobInstanceInfo ) };
        up.numInstances = numInstances;
        g_FrameVobUploads.push_back( up );
    }
}

void D3D12GraphicsEngine::DrawVobDepthPrepass() {
    // P2.9b-4a: lay down instanced-VOB depth (alpha-clipped) into the Forward+ opaque prepass so the tiled light
    // cull sees VOB surfaces and bounds each tile's near plane to them — fixing the static 16x16 cutoff where a
    // light on an object in front of distant world geometry got dropped (the tile AABB used to sit at the far
    // world, missing the near VOB). Depth-only via m_DepthPrepassVobPSO; consumes the shared g_FrameVobUploads.
    // Same per-material diffuse bind as the color pass for the alpha cutout. Node attachments (weapons/heads) are
    // NOT here — they upload during the skeletal color pass, after the cull; they'll join once skeletal does.
    if ( !m_FrameOpen || !m_DepthPrepassVobPSO || !m_WorldRootSig || !m_DepthBuffer )
        return;
    if ( g_FrameVobUploads.empty() ) return;

    DX_ZONE( m_CmdList, "Depth Prepass (vobs)" );

    // ViewProj — identical derivation to DrawVobsInstanced so the prepass depth matches the color pass exactly.
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetPipelineState( m_DepthPrepassVobPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_WorldRootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );   // b0 ViewProj (fog/lights not referenced)

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    for ( const FrameVobUpload& up : g_FrameVobUploads ) {
        MeshVisualInfo* visual = up.visual;
        if ( !visual ) continue;
        const UINT numInstances = up.numInstances;

        for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
            zCTexture* tex = meshKey.Material->GetAniTexture();
            D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
            if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
                    if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                        D3D12Texture* d12 = D3D12Texture::From( gfx );
                        if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                    }
                }
            }
            m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );

            for ( MeshInfo* mi : meshList ) {
                if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() )
                    continue;
                D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
                D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
                if ( !mvb->GetResource() || !mib->GetResource() ) continue;

                const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
                const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, up.instView };
                m_CmdList->IASetVertexBuffers( 0, 2, views );

                const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                m_CmdList->IASetIndexBuffer( &ibv );

                m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mi->Indices.size()), numInstances, 0, 0, 0 );
            }
        }
    }
}

XRESULT D3D12GraphicsEngine::DrawVobsInstanced() {
    if ( !m_FrameOpen || !m_VobPSO || !m_WorldRootSig || !m_DepthBuffer )
        return XR_SUCCESS;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    if ( !rs.RendererSettings.DrawVOBs )
        return XR_SUCCESS;

    // Visible VOBs/lights/mobs were already collected once in OnStartWorldRendering (g_FrameVobs/Lights/Mobs);
    // this pass just consumes them (each visual's Instances list was filled by that collection).

    // Reversed-Z ViewProj (recomputed; identical derivation to DrawWorldMesh).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = rs.TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    const FogConstants fog = MakeFogConstants();

    m_CmdList->SetPipelineState( m_VobPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_WorldRootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );   // b1 fog
    BindFrameLights();   // param 3 = light SRV (t1), param 4 = light count (b2) — see DrawWorldMesh.
    m_CmdList->SetGraphicsRootConstantBufferView( 7, m_ShadowCBGpu[m_FrameIndex] );          // b3 shadow CB
    m_CmdList->SetGraphicsRootDescriptorTable( 8, GetSrvGpuHandle( m_ShadowSrvSlot ) );      // t4 shadow map
    m_CmdList->SetGraphicsRootDescriptorTable( 9, GetSrvGpuHandle( m_PointShadowSrvSlot ) ); // t5 point-shadow cubes

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    unsigned int drawnTris = 0;

    {
        DX_ZONE( m_CmdList, "Draw Vobs" );
        // Instances were snapshotted into the ring once by UploadFrameVobInstances (before the cull); draw from
        // those shared records so the color pass adds no second upload (its ring usage is unchanged).
        for ( const FrameVobUpload& up : g_FrameVobUploads ) {
            MeshVisualInfo* visual = up.visual;
            if ( !visual ) continue;
            const UINT numInstances = up.numInstances;

            for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
                zCTexture* tex = meshKey.Material->GetAniTexture();
                D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
                if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
                        if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                            D3D12Texture* d12 = D3D12Texture::From( gfx );
                            if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                        }
                    }
                }
                m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );
                BindMaterialMaps( tex, 10 );   // b6 bindless normal/ORM indices for this material

                for ( MeshInfo* mi : meshList ) {
                    if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() )
                        continue;
                    D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
                    D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
                    if ( !mvb->GetResource() || !mib->GetResource() ) continue;

                    const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
                    const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, up.instView };
                    m_CmdList->IASetVertexBuffers( 0, 2, views );

                    // VOB sub-mesh index buffers are 16-bit (VERTEX_INDEX), unlike the 32-bit wrapped world mesh.
                    const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                    m_CmdList->IASetIndexBuffer( &ibv );

                    m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mi->Indices.size()), numInstances, 0, 0, 0 );
                    drawnTris += (static_cast<unsigned int>(mi->Indices.size()) / 3) * numInstances;
                }
            }
        }
    }

    // Static skeletal MOBs (g_FrameMobs) are now prepared up front (PrepareFrameSkeletals) and drawn by
    // DrawSkeletalColor alongside the animated NPCs — no longer a nested call here (see OnStartWorldRendering).

    // NOTE: the per-visual Instances lists are cleared once at the end of OnStartWorldRendering (not here),
    // so they still get reset even when DrawVOBs is off and this pass early-outs above.

    rs.RendererInfo.FrameDrawnTriangles += drawnTris;
    return XR_SUCCESS;
}

void D3D12GraphicsEngine::PrepareFrameSkeletals( std::vector<SkeletalVobInfo*>& vobs ) {
    // P2.9b-4b (pre-cull): run each visible skeletal vob's once-per-frame animation update, upload its instance +
    // bone CBs (base meshes) and its node attachments' VOB-instance data into the per-frame rings, and RECORD
    // the GPU addresses in g_FrameSkelDraws / g_FrameAttachDraws. NO draws here — DrawSkeletalDepthPrepass
    // (pre-cull, depth) and DrawSkeletalColor (post-cull, lit) both draw from these records, so the animation
    // update never runs twice and nothing is uploaded twice. Appends (called once per list: animated + mobs).
    if ( !m_FrameOpen || !m_SkeletalCBBuffer[m_FrameIndex] || !m_SkeletalCBBufferPtr[m_FrameIndex] )
        return;
    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    if ( !rs.RendererSettings.DrawSkeletalMeshes )
        return;
    if ( vobs.empty() ) return;

    const float radius = rs.RendererSettings.SkeletalMeshDrawRadius;
    const XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
    const XMVECTOR radiusSq = XMVectorReplicate( radius * radius );
    const UINT frame = m_FrameIndex;
    const auto now = Engine::GAPI->GetTotalTimeDW();
    static std::vector<XMFLOAT4X4> boneCache;

    for ( SkeletalVobInfo* vi : vobs ) {
        if ( !vi || !vi->Vob || !vi->VisualInfo ) continue;
        if ( !vi->Vob->GetShowVisual() ) continue;

        SkeletalMeshVisualInfo* visual = static_cast<SkeletalMeshVisualInfo*>( vi->VisualInfo );
        zCModel* model = static_cast<zCModel*>( vi->Vob->GetVisual() );
        if ( !model ) continue;

        if ( XMVector3Greater( XMVector3LengthSq( camPos - vi->Vob->GetPositionWorldXM() ), radiusSq ) )
            continue;   // out of skeletal-draw range

        // Some skeletal vobs arrive with their base mesh not yet extracted (SkeletalMeshes empty but the model
        // does carry soft-skin geometry) — build it lazily. Interactive MOBs whose ONLY renderable content is a
        // node attachment (a lamp post's lamp, some doors) legitimately stay empty and fall through to the
        // attachment loop below — they must NOT be skipped (this was the "MOBs don't render" bug).
        if ( visual->SkeletalMeshes.empty() && model->GetMeshSoftSkinList()->NumInArray > 0 )
            WorldConverter::ExtractSkeletalMeshFromVob( model, visual );

        model->SetDistanceToCamera( 500 );
        if ( vi->LastAniUpdateFrame != now ) {
            vi->LastAniUpdateFrame = now;
            model->UpdateAttachedVobs();   // once/frame — this is why the pass can't just re-run in a prepass
        }
        // NOTE: UpdateMeshLibTexAniState is intentionally NOT called here. It mutates the model's SHARED texture
        // slots, so it's only meaningful immediately before drawing a specific instance's meshes (all instances
        // of a model share the slots). The draw paths (DrawSkeletalDepthPrepass / DrawSkeletalColor) call it
        // per-record right before reading the materials — which is why FrameSkelDraw carries vobInfo.

        // Bone palette (object-space matrices) for the model's current animation pose. Needed for BOTH the base
        // skinned mesh AND the node-attachment world matrices, so compute it (and xmWorld) before either.
        boneCache.clear();
        model->GetBoneTransforms( &boneCache );
        UINT numBones = static_cast<UINT>( boneCache.size() );
        if ( numBones == 0 ) continue;
        if ( numBones > kSkeletalMaxBones ) numBones = kSkeletalMaxBones;

        const XMMATRIX xmWorld = vi->Vob->GetWorldMatrixXM() * XMMatrixScalingFromVector( model->GetModelScaleXM() );

        // Base skinned mesh — skipped entirely for attachment-only MOBs (empty SkeletalMeshes). Mirrors D3D11
        // DrawSkeletalMeshVobs, which guards its base pass on !SkeletalMeshes.empty() but always runs attachments.
        if ( !visual->SkeletalMeshes.empty() ) {
            // Allocate the per-instance CB + bone CB from the per-frame ring (each 256-byte aligned so it can be
            // bound as a root CBV). Uploaded ONCE here; the prepass + color pass reuse these two GPU addresses.
            const UINT instSize = static_cast<UINT>( sizeof( SkeletalInstanceCB ) );
            const UINT boneSize = numBones * static_cast<UINT>( sizeof( XMFLOAT4X4 ) );
            const UINT instOff = AlignCB( m_SkeletalCBBufferOffset );
            const UINT boneOff = AlignCB( instOff + instSize );
            if ( boneOff + boneSize > m_SkeletalCBBufferCapacity ) {
                if ( !m_SkeletalCBOverflowLogged ) {
                    LogWarn() << "D3D12: skeletal CB ring overflow (" << m_SkeletalCBBufferCapacity
                              << " bytes/frame). Some skeletal meshes dropped this frame.";
                    m_SkeletalCBOverflowLogged = true;
                }
                break;
            }

            SkeletalInstanceCB inst = {};
            XMStoreFloat4x4( &inst.World, xmWorld );
            inst.ModelColor = XMFLOAT4( 1.0f, 1.0f, 1.0f, 1.0f );   // first-light: white; ground-light color is a later step
            inst.Fatness = model->GetModelFatness();

            uint8_t* ringBase = m_SkeletalCBBufferPtr[frame];
            memcpy( ringBase + instOff, &inst, instSize );
            memcpy( ringBase + boneOff, boneCache.data(), boneSize );
            m_SkeletalCBBufferOffset = boneOff + boneSize;

            const D3D12_GPU_VIRTUAL_ADDRESS ringGpu = m_SkeletalCBBuffer[frame]->GetGPUVirtualAddress();
            g_FrameSkelDraws.push_back( { vi, visual, ringGpu + instOff, ringGpu + boneOff } );
        }

        // Node attachments (weapons/heads/lamps/held items): world = modelWorld * boneMatrix[node]. Upload each
        // as a VOB instance into the VOB ring NOW (pre-cull) so it can be depth-prepassed AND color-drawn from
        // one snapshot. Lazily convert the node visual on first sight (or if the node's visual changed).
        gtl::flat_hash_map<int, std::vector<MeshVisualInfo*>>& nodeAttachments = vi->NodeAttachments;
        zCArray<zCModelNodeInst*>* nodeList = model->GetNodeList();
        const int nodeCount = nodeList ? std::min<int>( static_cast<int>( boneCache.size() ), nodeList->NumInArray ) : 0;
        for ( int n = 0; n < nodeCount; ++n ) {
            zCModelNodeInst* node = nodeList->Array[n];
            if ( !node || !node->NodeVisual ) continue;   // no attachment on this node (e.g. sheathed weapon)

            auto it = nodeAttachments.find( n );
            if ( it == nodeAttachments.end() ) {
                WorldConverter::ExtractNodeVisual( n, node, nodeAttachments );
                it = nodeAttachments.find( n );
            } else if ( !it->second.empty() && it->second[0] && it->second[0]->Visual != node->NodeVisual ) {
                WorldConverter::ExtractNodeVisual( n, node, nodeAttachments );  // visual changed
                it = nodeAttachments.find( n );
            }
            if ( it == nodeAttachments.end() ) continue;

            XMFLOAT4X4 attWorld;
            XMStoreFloat4x4( &attWorld, xmWorld * XMLoadFloat4x4( &boneCache[n] ) );
            for ( MeshVisualInfo* mvi : it->second ) {
                if ( !mvi ) continue;
                bool isMMS = strcmp( mvi->Visual->GetFileExtension( 0 ), ".MMS" ) == 0;
                node->TexAniState.UpdateTexList();
                if ( isMMS ) {
                    zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>(mvi->Visual);
                    mm->GetTexAniState()->UpdateTexList();
                }
                for ( auto const& [attMat, attMeshes] : mvi->Meshes ) {
                    zCTexture* attTex = attMat ? attMat->GetAniTexture() : nullptr;
                    for ( auto const& attMesh : attMeshes ) {
                        if ( !attMesh || attMesh->Indices.empty() ) continue;
                        if ( !attMesh->GetMeshVertexBuffer() || !attMesh->GetMeshIndexBuffer() ) continue;

                        const UINT instBytes = static_cast<UINT>( sizeof( VobInstanceInfo ) );
                        if ( m_VobInstanceBufferOffset + instBytes > m_VobInstanceBufferCapacity ) {
                            if ( !m_VobInstanceOverflowLogged ) {
                                LogWarn() << "D3D12: VOB instance ring overflow (skeletal attachments dropped this frame).";
                                m_VobInstanceOverflowLogged = true;
                            }
                            break;
                        }
                        VobInstanceInfo vii = {};
                        vii.world = attWorld;
                        vii.color = 0xFFFFFFFF;   // first-light: white (baked ground-light tint is a later step)
                        const UINT instOffset = m_VobInstanceBufferOffset;
                        memcpy( m_VobInstanceBufferPtr[frame] + instOffset, &vii, instBytes );
                        m_VobInstanceBufferOffset += instBytes;
                        const D3D12_VERTEX_BUFFER_VIEW attInstView = {
                            m_VobInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( VobInstanceInfo ) };
                        g_FrameAttachDraws.push_back( { attMesh.get(), attTex, attInstView } );
                    }
                }
            }
        }
    }
}

void D3D12GraphicsEngine::DrawSkeletalDepthPrepass() {
    // P2.9b-4b (pre-cull): lay down skeletal base-mesh + node-attachment depth into the Forward+ opaque prepass
    // so the tiled light cull bounds tiles to NPCs/monsters (fixing the static near-skeletal cutoff). Depth-only;
    // consumes the shared records. Base meshes via m_DepthPrepassSkeletalPSO (skinned), attachments via
    // m_DepthPrepassVobPSO (packed vertex + instance) — both read only b0 + t0/s0, so no BindFrameLights.
    if ( !m_FrameOpen || !m_DepthBuffer ) return;
    if ( g_FrameSkelDraws.empty() && g_FrameAttachDraws.empty() ) return;

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );

    // Base skinned meshes (depth only).
    if ( !g_FrameSkelDraws.empty() && m_DepthPrepassSkeletalPSO && m_SkeletalRootSig ) {
        DX_ZONE( m_CmdList, "Depth Prepass (skeletal)" );
        m_CmdList->SetPipelineState( m_DepthPrepassSkeletalPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_SkeletalRootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        for ( const FrameSkelDraw& d : g_FrameSkelDraws ) {
            if ( !d.visual ) continue;
            zCModel* model = static_cast<zCModel*>(d.vobInfo->Vob->GetVisual());
            model->UpdateMeshLibTexAniState(); // before drawing we NEED to TexAni, the models share the same textures, causing incorrect textures if not done correctly.

            m_CmdList->SetGraphicsRootConstantBufferView( 1, d.instCb );
            m_CmdList->SetGraphicsRootConstantBufferView( 2, d.boneCb );
            for ( auto const& [mat, meshList] : d.visual->SkeletalMeshes ) {
                D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
                zCTexture* tex = mat ? mat->GetAniTexture() : nullptr;
                if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
                        if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                            D3D12Texture* d12 = D3D12Texture::From( gfx );
                            if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                        }
                    }
                }
                m_CmdList->SetGraphicsRootDescriptorTable( 3, srv );
                BindMaterialMaps( tex, 12 );   // b6 bindless normal/ORM indices (no-op in the depth prepass)
                for ( auto const& mesh : meshList ) {
                    if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer )
                        continue;
                    D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
                    D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
                    if ( !mvb->GetResource() || !mib->GetResource() ) continue;
                    const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExSkelVertexStruct ) };
                    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
                    const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                    m_CmdList->IASetIndexBuffer( &ibv );
                    m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 1, 0, 0, 0 );
                }
            }
        }
    }

    // Node attachments (depth only) through the VOB depth PSO (packed vertex slot 0 + per-instance world slot 1).
    if ( !g_FrameAttachDraws.empty() && m_DepthPrepassVobPSO && m_WorldRootSig ) {
        DX_ZONE( m_CmdList, "Depth Prepass (attachments)" );
        m_CmdList->SetPipelineState( m_DepthPrepassVobPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_WorldRootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        for ( const FrameAttachDraw& a : g_FrameAttachDraws ) {
            if ( !a.mesh || !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() ) continue;
            D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
            D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
            if ( !mvb->GetResource() || !mib->GetResource() ) continue;

            D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
            if ( a.tex && a.tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                if ( MyDirectDrawSurface7* surface = a.tex->GetSurface() ) {
                    if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                        D3D12Texture* d12 = D3D12Texture::From( gfx );
                        if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                    }
                }
            }
            m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );
            BindMaterialMaps( a.tex, 10 );   // b6 bindless normal/ORM indices (no-op in the depth prepass)

            const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
            const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, a.instView };
            m_CmdList->IASetVertexBuffers( 0, 2, views );
            const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
            m_CmdList->IASetIndexBuffer( &ibv );
            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( a.mesh->Indices.size() ), 1, 0, 0, 0 );
        }
    }
}

void D3D12GraphicsEngine::DrawSkeletalColor() {
    // P2.9b-4b (post-cull): draw the skeletal base meshes + node attachments collected by PrepareFrameSkeletals,
    // lit through the tile grid. Base via m_SkeletalPSO, attachments via m_VobPSO — same PSOs/binds as before the
    // 4b split, just consuming the shared records (no re-upload, no re-run of the once/frame animation update).
    if ( !m_FrameOpen || !m_SkeletalPSO || !m_SkeletalRootSig || !m_DepthBuffer ) return;
    if ( g_FrameSkelDraws.empty() && g_FrameAttachDraws.empty() ) return;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();

    // Reversed-Z ViewProj (identical derivation to DrawWorldMesh / DrawVobsInstanced).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = rs.TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    const FogConstants fog = MakeFogConstants();
    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    unsigned int drawnTris = 0;

    // Base skinned meshes (lit).
    if ( !g_FrameSkelDraws.empty() ) {
        DX_ZONE( m_CmdList, "Draw skeletal" );
        m_CmdList->SetPipelineState( m_SkeletalPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_SkeletalRootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->SetGraphicsRoot32BitConstants( 4, 8, &fog, 0 );   // b3 fog
        BindFrameLights( 5, 6, 7, 8 );   // light SRV(t1)+count(b4)+grid(t2)+index(t3) — MUST set all (see BindFrameLights)
        m_CmdList->SetGraphicsRootConstantBufferView( 9, m_ShadowCBGpu[m_FrameIndex] );        // b5 shadow CB
        m_CmdList->SetGraphicsRootDescriptorTable( 10, GetSrvGpuHandle( m_ShadowSrvSlot ) );   // t4 shadow map
        m_CmdList->SetGraphicsRootDescriptorTable( 11, GetSrvGpuHandle( m_PointShadowSrvSlot ) ); // t5 point-shadow cubes
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        for ( const FrameSkelDraw& d : g_FrameSkelDraws ) {
            if ( !d.visual ) continue;
            zCModel* model = static_cast<zCModel*>(d.vobInfo->Vob->GetVisual());
            model->UpdateMeshLibTexAniState(); // before drawing we NEED to TexAni, the models share the same textures, causing incorrect textures if not done correctly.

            m_CmdList->SetGraphicsRootConstantBufferView( 1, d.instCb );
            m_CmdList->SetGraphicsRootConstantBufferView( 2, d.boneCb );
            for ( auto const& [mat, meshList] : d.visual->SkeletalMeshes ) {
                D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
                zCTexture* tex = mat ? mat->GetAniTexture() : nullptr;
                if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
                        if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                            D3D12Texture* d12 = D3D12Texture::From( gfx );
                            if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                        }
                    }
                }
                m_CmdList->SetGraphicsRootDescriptorTable( 3, srv );
                BindMaterialMaps( tex, 12 );   // b6 bindless normal/ORM indices (no-op in the depth prepass)
                for ( auto const& mesh : meshList ) {
                    if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer )
                        continue;
                    D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
                    D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
                    if ( !mvb->GetResource() || !mib->GetResource() ) continue;
                    const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExSkelVertexStruct ) };
                    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
                    const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                    m_CmdList->IASetIndexBuffer( &ibv );
                    m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 1, 0, 0, 0 );
                    drawnTris += static_cast<unsigned int>( mesh->Indices.size() ) / 3;
                }
            }
        }
    }

    // Node attachments (lit) through the VOB pipeline. BindFrameLights() is REQUIRED — the VOB PS reads the
    // light count/grid, so an unbound count would run the loop on garbage → GPU TDR hang.
    if ( !g_FrameAttachDraws.empty() && m_VobPSO && m_WorldRootSig ) {
        DX_ZONE( m_CmdList, "Draw attachments" );
        m_CmdList->SetPipelineState( m_VobPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_WorldRootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );   // b1 fog (VOB root sig)
        BindFrameLights();
        m_CmdList->SetGraphicsRootConstantBufferView( 7, m_ShadowCBGpu[m_FrameIndex] );        // b3 shadow CB
        m_CmdList->SetGraphicsRootDescriptorTable( 8, GetSrvGpuHandle( m_ShadowSrvSlot ) );    // t4 shadow map
        m_CmdList->SetGraphicsRootDescriptorTable( 9, GetSrvGpuHandle( m_PointShadowSrvSlot ) ); // t5 point-shadow cubes
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        for ( const FrameAttachDraw& a : g_FrameAttachDraws ) {
            if ( !a.mesh || !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() ) continue;
            D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
            D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
            if ( !mvb->GetResource() || !mib->GetResource() ) continue;

            D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
            if ( a.tex && a.tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                if ( MyDirectDrawSurface7* surface = a.tex->GetSurface() ) {
                    if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                        D3D12Texture* d12 = D3D12Texture::From( gfx );
                        if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                    }
                }
            }
            m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );
            BindMaterialMaps( a.tex, 10 );   // b6 bindless normal/ORM indices (no-op in the depth prepass)

            const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
            const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, a.instView };
            m_CmdList->IASetVertexBuffers( 0, 2, views );
            const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
            m_CmdList->IASetIndexBuffer( &ibv );
            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( a.mesh->Indices.size() ), 1, 0, 0, 0 );
            drawnTris += static_cast<unsigned int>( a.mesh->Indices.size() ) / 3;
        }
    }

    rs.RendererInfo.FrameDrawnTriangles += drawnTris;
}

XRESULT D3D12GraphicsEngine::SetWindow( HWND hWnd ) {
    LogInfo() << "D3D12: Creating swapchain";
    m_OutputWindow = hWnd;

    // Use the configured target resolution (NOT the current client rect — Gothic creates its window
    // tiny, so GetClientRect here would size the swapchain to a few pixels). Mirrors the D3D11 path,
    // which takes RendererSettings.LoadedResolution. OnResize sizes the OS window + builds the swapchain.
    INT2 size = Engine::GAPI->GetRendererState().RendererSettings.LoadedResolution;
    if ( size.x <= 0 || size.y <= 0 ) {
        RECT rc = {};
        GetClientRect( hWnd, &rc );
        size = INT2( std::max<int>( 800, rc.right - rc.left ), std::max<int>( 600, rc.bottom - rc.top ) );
    }

    return OnResize( size );
}

void D3D12GraphicsEngine::QueueSrvResourceForRelease( UINT slot, Microsoft::WRL::ComPtr<ID3D12Resource> resource )
{
    QueueCleanupJob( [this, slot, resource = std::move(resource)]() {
        // Recycle the descriptor slot safely
        this->FreeSrvSlot( slot );

        // The ComPtr 'resource' capture naturally dies here, releasing the ID3D12Resource;
    } );
}

void D3D12GraphicsEngine::QueueCleanupJob( std::move_only_function<void()> callback )
{
    if ( callback == nullptr ) return;
    // No slot to recycle — just hold a reference until this frame index comes back around (after its
    // fence is waited on in MoveToNextFrame), then drop it. The capture keeps the resource alive until
    // every command list that could reference it has finished on the GPU.
    m_PerFrameCleanupItems[m_FrameIndex].emplace_back( std::move(callback) );
}

void D3D12GraphicsEngine::QueueResourceForRelease( Microsoft::WRL::ComPtr<ID3D12Resource> resource )
{
    if ( !resource ) return;
    // No slot to recycle — just hold a reference until this frame index comes back around (after its
    // fence is waited on in MoveToNextFrame), then drop it. The capture keeps the resource alive until
    // every command list that could reference it has finished on the GPU.
    QueueCleanupJob( [resource = std::move(resource)]() {} );
}

void D3D12GraphicsEngine::QueueAllocationForRelease( Microsoft::WRL::ComPtr<D3D12MA::Allocation> value )
{
    if ( !value ) return;
    // No slot to recycle — just hold a reference until this frame index comes back around (after its
    // fence is waited on in MoveToNextFrame), then drop it. The capture keeps the resource alive until
    // every command list that could reference it has finished on the GPU.
    QueueCleanupJob( [resource = std::move(value)]() { } );
}

/** Sizes the actual OS window to the target resolution and tells Gothic about the mode so its 2D
    UI coordinate space matches. Mirrors the windowed / borderless branch of the D3D11 backend. */
void D3D12GraphicsEngine::ResizeOutputWindow( INT2 size ) {
    if ( !m_OutputWindow || size.x <= 0 || size.y <= 0 ) return;

#ifndef BUILD_SPACER
    RECT desktopRect = {};
    GetClientRect( GetDesktopWindow(), &desktopRect );
    const bool borderless = ( size.x >= desktopRect.right && size.y >= desktopRect.bottom );

    if ( borderless ) {
        // Fullscreen-borderless: strip the frame and cover the desktop.
        LONG style = GetWindowLong( m_OutputWindow, GWL_STYLE );
        style &= ~( WS_CAPTION | WS_THICKFRAME );
        SetWindowLong( m_OutputWindow, GWL_STYLE, style );
        LONG exStyle = GetWindowLong( m_OutputWindow, GWL_EXSTYLE );
        exStyle &= ~( WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE );
        SetWindowLong( m_OutputWindow, GWL_EXSTYLE, exStyle );
        SetWindowPos( m_OutputWindow, nullptr, 0, 0, desktopRect.right, desktopRect.bottom, SWP_SHOWWINDOW | SWP_FRAMECHANGED );
    } else {
        // Windowed: fixed-size window whose CLIENT area equals the target resolution.
        LONG style = ( WS_OVERLAPPEDWINDOW | WS_VISIBLE ) & ~( WS_MAXIMIZEBOX | WS_THICKFRAME );
        SetWindowLong( m_OutputWindow, GWL_STYLE, style );
        SetWindowLong( m_OutputWindow, GWL_EXSTYLE, WS_EX_APPWINDOW );

        RECT wr = { 0, 0, size.x, size.y };
        AdjustWindowRectEx( &wr, style, FALSE, WS_EX_APPWINDOW );

        RECT cur = {};
        int x = 0, y = 0;
        if ( GetWindowRect( m_OutputWindow, &cur ) ) { x = cur.left; y = cur.top; }
        SetWindowPos( m_OutputWindow, nullptr, x, y, wr.right - wr.left, wr.bottom - wr.top, SWP_SHOWWINDOW | SWP_FRAMECHANGED );
    }

    zCView::SetWindowMode( size.x, size.y, 32 );
    // Inform Gothic of the resolution (drives its virtual UI coordinate space).
    zCView::SetVirtualMode( size.x, size.y, 32 );
    POINT virtualSize = { 8192, 8192 };
    zCViewDraw::GetScreen().SetVirtualSize( virtualSize );
#endif
}

static bool CheckTearingSupport() {
    BOOL allowTearing = FALSE;
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory5)))) {
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    }
    return allowTearing == TRUE;
}

bool D3D12GraphicsEngine::CreateSwapChain( INT2 size ) {
    m_Resolution = size;

    m_TearingSupported = CheckTearingSupport();
    
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = static_cast<UINT>( size.x );
    scd.Height = static_cast<UINT>( size.y );
    scd.Format = kBackBufferFormat;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kBackBufferCount;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags = m_TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = m_Device.GetFactory()->CreateSwapChainForHwnd(
        m_Device.GetDirectQueue(), m_OutputWindow, &scd, nullptr, nullptr, swapChain1.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogWarn() << "CreateSwapChainForHwnd failed (0x" << std::hex << hr << ").";
        return false;
    }

    // GD3D11 manages fullscreen itself; disable DXGI's Alt+Enter handling.
    m_Device.GetFactory()->MakeWindowAssociation( m_OutputWindow, DXGI_MWA_NO_ALT_ENTER );

    if ( FAILED( swapChain1.As( &m_SwapChain ) ) ) {
        LogWarn() << "Swapchain does not support IDXGISwapChain3.";
        return false;
    }
    m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

    if ( !CreateFrameResources() ) return false;
    if ( !AcquireBackBufferRTVs() ) return false;
    if ( !CreateDepthBuffer( size ) ) return false;
    if ( !CreateSceneColorTarget( size ) ) return false;   // HDR scene RT (RTV heap now exists with the extra slot)

    m_SwapChainReady = true;

    // Bring up the ImGui overlay on the D3D12 backend (mirrors D3D11's OnResize-time init). ImGui
    // texture SRVs are allocated out of our shader-visible heap via callbacks; drawn each Present().
    if ( Engine::ImGuiHandle && !Engine::ImGuiHandle->Initiated && m_SrvHeap ) {
        Engine::ImGuiHandle->InitD3D12( m_OutputWindow, this, m_Device.GetDevice(),
            m_Device.GetDirectQueue(), kBackBufferCount, kBackBufferFormat, m_SrvHeap.Get() );
    }
    return true;
}

bool D3D12GraphicsEngine::CreateFrameResources() {
    ID3D12Device* device = m_Device.GetDevice();

    // RTV descriptor heap: one per backbuffer + 1 for the HDR scene-color target (slot kBackBufferCount).
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kBackBufferCount + 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if ( FAILED( device->CreateDescriptorHeap( &rtvHeapDesc, IID_PPV_ARGS( m_RtvHeap.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_RtvDescriptorSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_RTV );

    // Per-frame command allocators
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS( m_CmdAllocators[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
    }

    // A single command list (created recording, then closed — OnBeginFrame resets it each frame)
    if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_CmdAllocators[m_FrameIndex].Get(), nullptr, IID_PPV_ARGS( m_CmdList.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_CmdList->Close();

    // Frame-sync fence
    if ( FAILED( device->CreateFence( m_FenceValues[m_FrameIndex], D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS( m_Fence.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_FenceValues[m_FrameIndex]++;

    if ( !m_FenceEvent ) {
        m_FenceEvent = CreateEvent( nullptr, FALSE, FALSE, nullptr );
        if ( !m_FenceEvent ) return false;
    }
    return true;
}

bool D3D12GraphicsEngine::AcquireBackBufferRTVs() {
    ID3D12Device* device = m_Device.GetDevice();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_SwapChain->GetBuffer( i, IID_PPV_ARGS( m_BackBuffers[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_BackBuffers[i]->SetName( i == 0 ? L"BackBuffer0" : L"BackBuffer1" );
        device->CreateRenderTargetView( m_BackBuffers[i].Get(), nullptr, rtvHandle );
        rtvHandle.ptr += m_RtvDescriptorSize;
    }
    return true;
}

XRESULT D3D12GraphicsEngine::OnBeginFrame() {
    if ( !m_SwapChainReady ) return XR_SUCCESS;

    
    {
        std::lock_guard<std::mutex> lock( m_CopyQueueMutex );
        ReleaseCompletedCopyResources( m_CopyFence->GetCompletedValue() );
    }
    
    HRESULT hr = m_CmdAllocators[m_FrameIndex]->Reset();
    if ( FAILED( hr ) ) {
        WaitForGpuIdle();
        hr = m_CmdAllocators[m_FrameIndex]->Reset();
        if ( FAILED( hr ) ) return XR_FAILED;
    }
    hr = m_CmdList->Reset( m_CmdAllocators[m_FrameIndex].Get(), nullptr );
    if ( FAILED( hr ) ) return XR_FAILED;
    ResetCpuContextTracker();

    auto toRT = TransitionBarrier( m_BackBuffers[m_FrameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET );
    m_CmdList->ResourceBarrier( 1, &toRT );

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>( m_FrameIndex ) * m_RtvDescriptorSize;

    // Bind the backbuffer + depth target for the frame. The 3D world pass (OnStartWorldRendering) uses
    // the depth buffer; the 2D/UI PSO has depth disabled, so it draws over the result regardless.
    const bool haveDepth = m_DepthBuffer && m_DsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
    if ( haveDepth ) dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();

    m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, haveDepth ? &dsv : nullptr );
    m_CmdList->ClearRenderTargetView( rtv, m_ClearColor, 0, nullptr );
    if ( haveDepth )  // reversed-Z: clear to 0.0
        m_CmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr );

    // Bind the shader-visible SRV heap for this frame's 2D draws (descriptor tables reference it).
    ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
    m_CmdList->SetDescriptorHeaps( 1, heaps );

    // Reset the per-frame 2D vertex ring + VOB instance ring + default the viewport to the full backbuffer.
    m_UIVertexBufferOffset = 0;
    m_UIOverflowLogged = false;
    m_VobInstanceBufferOffset = 0;
    m_VobInstanceOverflowLogged = false;
    m_SkeletalCBBufferOffset = 0;
    m_SkeletalCBOverflowLogged = false;
    m_ParticleInstanceBufferOffset = 0;
    m_ParticleInstanceOverflowLogged = false;
    m_DecalInstanceBufferOffset = 0;
    m_DecalInstanceOverflowLogged = false;
    m_LightOverflowLogged = false;   // light buffer is rebuilt from 0 each frame in BuildFrameLightBuffer
    m_CurrentTexture = nullptr;
    m_CurrentViewport = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    m_CurrentScissor = { 0, 0, m_Resolution.x, m_Resolution.y };

    // this seems to do nothing on its own - CURRENTLY. But who knows what will happen when we do 3d.
    zCView::SetWindowMode(
        m_Resolution.x,
        m_Resolution.y,
        32 );

    // This ensures any 2D UI is rendered with the proper resolution.
    // needs to be per-frame or it won't do anything.
    zCView::SetVirtualMode(
        static_cast<int>(m_Resolution.x),
        static_cast<int>(m_Resolution.y),
        32 );

    //POINT virtualSize = { 8192, 8192 };
    //zCViewDraw::GetScreen().SetVirtualSize( virtualSize );

    m_FrameOpen = true;
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::OnEndFrame() {
    if ( !m_SwapChainReady || !m_FrameOpen ) return XR_SUCCESS;
    Present();
    m_FrameOpen = false;
    m_PresentPending = false;
    return XR_SUCCESS;
}

#ifdef DEBUG_D3D11

static const wchar_t* GetOpName( D3D12_AUTO_BREADCRUMB_OP op ) {
    switch ( op ) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return L"SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return L"BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return L"EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return L"DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return L"DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return L"ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return L"Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return L"CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return L"CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return L"CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return L"CopyTiles";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return L"ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return L"ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return L"ClearUnorderedAccessView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return L"ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return L"ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return L"ExecuteBundle";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return L"Present";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return L"BuildRaytracingAccelerationStructure";
    case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO: return L"EmitRaytracingAccelerationStructurePostBuildInfo";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return L"CopyRaytracingAccelerationStructure";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return L"DispatchRays";
    case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEMETACOMMAND: return L"InitializeMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND: return L"ExecuteMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_ESTIMATEMOTION: return L"EstimateMotion";
    case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return L"EnhancedBarrier";
    default: return L"Unknown D3D12 Command";
    }
}

static const wchar_t* SafeWideString( const wchar_t* str ) {
    return str ? str : L"[Unnamed Object]";
}

static const wchar_t* FindCpuRecordedContext( UINT crashIndex ) {
    const wchar_t* lastKnownContext = L"Unknown/Outside Scopes";

    // Look back through what the CPU logged during recording up to the crash point
    for ( UINT i = 0; i <= crashIndex; ++i ) {
        if ( i < g_CpuContextHistory.size() && g_CpuContextHistory[i].pContextText != nullptr ) {
            lastKnownContext = g_CpuContextHistory[i].pContextText;
        }
    }
    return lastKnownContext;
}

static void PrintNode( const D3D12_AUTO_BREADCRUMB_NODE1* node ) {
    if ( !node ) {
        return;
    }

    std::wstring builder{};
    builder.reserve(1024);

    builder.append( L"--- Outstanding Command List GPU Breadcrumbs ---\n" );
    builder.append( L"Command List Debug Name: " ).append( SafeWideString( node->pCommandListDebugNameW ) ).append( L"\n" );
    builder.append( L"Command Queue Debug Name: " ).append( SafeWideString( node->pCommandQueueDebugNameW ) ).append( L"\n" );
    OutputDebugStringW( builder.c_str() );

    // Log out the History of GPU Operations recorded
    // Note: pLastBreadcrumbValue points to the number of completed operations.
    // Operations *up to* (*node->pLastBreadcrumbValue) finished. Anything past failed or hung.
    UINT completedOps = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;

    builder.clear();
    builder.append( L"Completed Op Count: " ).append( std::to_wstring( completedOps ) ).append( L" / " ).append( std::to_wstring( node->BreadcrumbCount ) ).append( L"\n" );
    OutputDebugStringW( builder.c_str() );

    for ( UINT i = 0; i < node->BreadcrumbCount; ++i ) {
        builder.clear();

        if ( i < completedOps ) {
            builder.append( L" [ok] " );
        } else if ( i == completedOps ) {
            builder.append( L" [ERR] " );
        } else {
            builder.append( L" [ ] " );
        }

        builder.append( L"Op #" ).append( std::to_wstring( i ) ).append( L": " );
        builder.append( GetOpName( node->pCommandHistory[i] ) );

        if ( i == completedOps ) {
            builder.append( L"   <=== !!! HARDWARE HANG DETECTED AT THIS OPERATION !!!" );

            // Pull the exact recorded context step tied directly to this operation cluster!
            const wchar_t* contextAtCrash = FindCpuRecordedContext( completedOps );
            builder.append( L"\n   <=== !!! ACTIVE SCOPE AT TIME OF HARDWARE CRASH: \"" )
                .append( contextAtCrash ).append( L"\" !!!" );
        }

        builder.append( L"\n" );
        OutputDebugStringW( builder.c_str() );
    }

    if ( node->pNext ) {
        PrintNode( node->pNext );
    }
#undef PRINT_NODE_FIELD
}

static void DiagnoseErrors(ID3D12Device* device) {
    // Enable the debug layer before device creation when available (best-effort).
    if ( HMODULE d3d12 = GetModuleHandleA( "d3d12.dll" ) ) {
        auto getDebug = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress( d3d12, "D3D12GetDebugInterface" ));

        ComPtr<ID3D12DeviceRemovedExtendedData1> pRemovedExtendedData;
        if ( SUCCEEDED( device->QueryInterface( IID_PPV_ARGS( pRemovedExtendedData.ReleaseAndGetAddressOf() ) ) ) ) {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 output;
            if (SUCCEEDED( pRemovedExtendedData->GetAutoBreadcrumbsOutput1( &output ) )) {
                PrintNode( output.pHeadAutoBreadcrumbNode );
            }
        }
    }
}
#endif

XRESULT D3D12GraphicsEngine::Present() {
    if ( !m_SwapChainReady || !m_FrameOpen ) return XR_SUCCESS;

    // Draw the ImGui overlay last, on top of the 2D UI, while the backbuffer is still a render target.
    // The SRV heap is bound (OnBeginFrame); re-bind the RTV defensively in case a draw changed it.
    if ( Engine::ImGuiHandle && Engine::ImGuiHandle->Initiated ) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>( m_FrameIndex ) * m_RtvDescriptorSize;
        m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, nullptr );
        Engine::ImGuiHandle->RenderLoopD3D12( m_CmdList.Get() );
    }

    auto toPresent = TransitionBarrier( m_BackBuffers[m_FrameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT );
    m_CmdList->ResourceBarrier( 1, &toPresent );

    if ( FAILED( m_CmdList->Close() ) ) return XR_FAILED;

    ID3D12CommandList* lists[] = { m_CmdList.Get() };
    m_Device.GetDirectQueue()->ExecuteCommandLists( 1, lists );

    const bool vsync = Engine::GAPI->GetRendererState().RendererSettings.EnableVSync;
    const UINT syncInterval = vsync ? 1 : 0;

    UINT presentFlags = 0;
    if (!vsync && m_TearingSupported) {
        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    
    HRESULT hr = m_SwapChain->Present( syncInterval, presentFlags );
    if ( FAILED( hr ) ) {
        auto r = static_cast<uint32_t>(hr);
        if ( hr == DXGI_ERROR_DEVICE_REMOVED) {
#ifdef DEBUG_D3D11
            DiagnoseErrors( m_Device.GetDevice() );
#endif
            auto removedReason = m_Device.GetDevice()->GetDeviceRemovedReason();
            auto msg = std::format( "D3D12 Present failed (0x{:08X}, reason: 0x{:08X})", r, static_cast<uint32_t>(removedReason) );
            LogWarn() << "D3D12 Present failed (0x" << std::hex << r << ").";
            MessageBoxA( NULL, msg.c_str(), "GD3D11 (DX12): Error", MB_OK );
        } else {
            auto msg = std::format( "D3D12 Present failed (0x{:08X})", r );
            LogWarn() << "D3D12 Present failed (0x" << std::hex << r << ").";
            MessageBoxA( NULL, msg.c_str(), "GD3D11 (DX12): Error", MB_OK);
        }
        exit( hr );
        return XR_FAILED;
    }

    MoveToNextFrame();
    return XR_SUCCESS;
}

void D3D12GraphicsEngine::MoveToNextFrame() {
    const UINT64 currentFenceValue = m_FenceValues[m_FrameIndex];
    m_Device.GetDirectQueue()->Signal( m_Fence.Get(), currentFenceValue );

    m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

    if ( m_Fence->GetCompletedValue() < m_FenceValues[m_FrameIndex] ) {
        m_Fence->SetEventOnCompletion( m_FenceValues[m_FrameIndex], m_FenceEvent );
        WaitForSingleObject( m_FenceEvent, INFINITE );
    }
    m_FenceValues[m_FrameIndex] = currentFenceValue + 1;

    // Clean up all resources slated for deletion from its last pass.
    for ( auto& cleanupCallback : m_PerFrameCleanupItems[m_FrameIndex] ) {
        cleanupCallback(); // Calls FreeSrvSlot() and drops the captured ComPtrs
    }
    m_PerFrameCleanupItems[m_FrameIndex].clear(); // Empty the list for the new frame
}

void D3D12GraphicsEngine::WaitForGpuIdle() {
    if ( !m_Fence || !m_Device.GetDirectQueue() ) return;

    if ( m_CopyFence && m_CopyFenceEvent ) {
        const UINT64 copyFenceValue = m_CopyFenceValue;
        if ( copyFenceValue > 0 && m_CopyFence->GetCompletedValue() < copyFenceValue ) {
            m_CopyFence->SetEventOnCompletion( copyFenceValue, m_CopyFenceEvent );
            WaitForSingleObject( m_CopyFenceEvent, INFINITE );
        }
    }

    // Calculate a "one-off" future value beyond all active frames.
    // This avoids colliding with any m_FenceValues currently in flight.
    UINT64 completedValue = m_Fence->GetCompletedValue();
    UINT64 idleValue = completedValue + 1;

    // Scan all active frames to ensure we choose a value strictly greater 
    // than any pending fence signals.
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( m_FenceValues[i] >= idleValue ) {
            idleValue = m_FenceValues[i] + 1;
        }
    }

    // Queue the signal command on the GPU timeline.
    // Because GPU execution is sequential, this milestone is only reached 
    // when ALL work previously queued has finished.
    if ( FAILED( m_Device.GetDirectQueue()->Signal( m_Fence.Get(), idleValue ) ) ) return;

    // Perform a CPU wait using a transient local event.
    if ( m_Fence->GetCompletedValue() < idleValue ) {
        // Create an anonymous, auto-reset event
        HANDLE eventHandle = CreateEventEx( nullptr, nullptr, 0, EVENT_ALL_ACCESS );
        if ( eventHandle ) {
            m_Fence->SetEventOnCompletion( idleValue, eventHandle );
            WaitForSingleObject( eventHandle, INFINITE );
            CloseHandle( eventHandle );
        }
    }

    // Update our CPU-side trackers so they know the GPU is completely caught up.
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        m_FenceValues[i] = idleValue;
    }
}

bool D3D12GraphicsEngine::ResizeSwapChain( INT2 size ) {
    if ( !m_SwapChainReady ) return false;
    if ( size.x <= 0 || size.y <= 0 ) return false;
    if ( size.x == m_Resolution.x && size.y == m_Resolution.y ) return true;

    WaitForGpuIdle();
    for ( UINT i = 0; i < kBackBufferCount; ++i ) m_BackBuffers[i].Reset();

    HRESULT hr = m_SwapChain->ResizeBuffers( kBackBufferCount,
        static_cast<UINT>( size.x ), static_cast<UINT>( size.y ), kBackBufferFormat, 0 );
    if ( FAILED( hr ) ) {
        LogWarn() << "D3D12 ResizeBuffers failed (0x" << std::hex << hr << ").";
        return false;
    }

    m_Resolution = size;
    m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();
    if ( !AcquireBackBufferRTVs() ) return false;
    if ( !CreateDepthBuffer( size ) ) return false;   // GPU is idle (WaitForGpuIdle above), safe to recreate
    return CreateSceneColorTarget( size );            // HDR scene RT tracks the new resolution too
}

XRESULT D3D12GraphicsEngine::OnResize( INT2 newSize ) {
    if ( newSize.x <= 0 || newSize.y <= 0 ) return XR_SUCCESS;
    if ( m_SwapChainReady && newSize.x == m_Resolution.x && newSize.y == m_Resolution.y )
        return XR_SUCCESS; // nothing to do

    ResizeOutputWindow( newSize );

    if ( !m_SwapChainReady ) {
        if ( !CreateSwapChain( newSize ) ) {
            LogWarn() << "D3D12GraphicsEngine::OnResize: swapchain creation failed.";
            return XR_FAILED;
        }
        LogInfo() << "D3D12 swapchain created (" << newSize.x << "x" << newSize.y << ").";
    } else {
        ResizeSwapChain( newSize );
    }

    if ( Engine::ImGuiHandle && Engine::ImGuiHandle->Initiated ) {
        Engine::ImGuiHandle->OnResize( newSize );
    }
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::TriggerResize( INT2 resolution ) {
    m_PerFrameCleanupItems[m_FrameIndex].emplace_back( [this, resolution]() {
        OnResize( resolution );
    } );
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::Clear( const float4& /*color*/ ) {
    return XR_SUCCESS; // first-light clears to the sentinel color in OnBeginFrame
}

XRESULT D3D12GraphicsEngine::CreateVertexBuffer( std::unique_ptr<GfxVertexBuffer>& outBuffer ) {
    outBuffer = std::make_unique<D3D12VertexBuffer>();
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::CreateTexture( GfxTexture** outTexture ) {
    if ( outTexture ) *outTexture = new D3D12Texture();
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::CreateTexture( std::unique_ptr<GfxTexture>& outTexture ) {
    outTexture = std::make_unique<D3D12Texture>();
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::CreateTexture( std::unique_ptr<D3D12Texture>& outTexture ) {
    outTexture = std::make_unique<D3D12Texture>();
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool /*includeSuperSampling*/ ) {
    if ( !modeList ) return XR_SUCCESS;

    modeList->clear();
    if ( XR_SUCCESS != DXGI_GetDisplayModeList( m_Device.GetDevice()->GetAdapterLuid(), m_OutputWindow, modeList) ) {
        modeList->clear();
        modeList->push_back( DisplayModeInfo( std::max<int>( 1, m_Resolution.x ), std::max<int>( 1, m_Resolution.y ), 60, 1 ) );
    }
    return XR_SUCCESS;
}

BaseLineRenderer* D3D12GraphicsEngine::GetLineRenderer() {
    return m_LineRenderer.get();
}
