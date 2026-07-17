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

#include <dxcapi.h>

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

namespace {
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
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
cbuffer LightCB : register(b2) { uint LightCount; uint3 _lpad; };   // Forward+ MVP: brute-force point-light count

// Per-frame visible point light (torches/campfires/spells). Byte-identical to D3D11 TiledPointLight (48 B);
// this brute-force MVP only reads PositionWorld/Range/Color — PositionView/ShadowCubeIndex land with tiling.
// Bound as a ROOT descriptor SRV (no descriptor-table slot). LightCount comes from a root constant that the
// draw MUST bind (BindFrameLights) — the loop is additionally clamped to LIGHT_CAP as a hard safety net so a
// stray unbound/garbage count can never run the loop away (that reads as a GPU timeout, not a clean error).
struct GPULight { float3 PositionView; float Range; float4 Color; float3 PositionWorld; int ShadowCubeIndex; };
StructuredBuffer<GPULight> Lights : register(t1);   // bound as a root SRV (no descriptor slot consumed)
#define LIGHT_CAP 400u

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

// Octahedral normal decode — matches Shaders/VertexPacking.h DecodeOctNormal (the packed 36-byte vertex
// stores the normal as R16G16_SNORM at offset 12; world-mesh normals are already world-space).
float3 DecodeOctNormal( float2 e )
{
    float3 n = float3( e.xy, 1.0 - abs( e.x ) - abs( e.y ) );
    float t = saturate( -n.z );
    n.xy += select(n.xy >= 0., -t, t);
    return normalize( n );
}

// Accumulate dynamic point lights at a world-space surface point. Ports D3D11's FP_ComputePointLighting /
// PLS_ComputePointLightLighting (world-space here instead of view-space): range cull, N.L, the exact
// falloff = nd*(nd*0.2+0.8), per-light saturate, additive accumulate. Specular is deferred (needs material
// spec params); the point term modulates the *texture* color (not the baked vertex color) like D3D11.
float3 AccumPointLights( float3 wpos, float3 N, float3 diffuse )
{
    float3 total = 0;
    uint n = min( LightCount, LIGHT_CAP );   // hard clamp: never trust the count enough to loop away
    for ( uint k = 0; k < n; k++ )
    {
        GPULight L = Lights[k];
        float3 dir = L.PositionWorld - wpos;
        float dist = length( dir );
        if ( dist >= L.Range ) continue;
        dir /= dist;
        float ndl = max( 0.0, dot( dir, N ) );
        float nd  = saturate( 1.0 - dist / L.Range );
        float falloff = nd * (nd * 0.2 + 0.8);
        float3 c = saturate( falloff * ndl * L.Color.rgb );
        total += saturate( c * diffuse );
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
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );                    // fixed alpha-test cutout (opaque textures have a==1 -> kept)
    float3 baseLit = t.rgb * i.col.bgr;   // baked vertex lighting (ambient/sun/GI); .bgr recovers Gothic's RGB
    float3 rgb = baseLit + AccumPointLights( i.wpos, normalize( i.wnrm ), t.rgb );
    // Linear distance fog toward the atmosphere color (matches Gothic's FFFog / the sky-clear color).
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    rgb = lerp( rgb, FogColor, f );
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
cbuffer LightCB : register(b2) { uint LightCount; uint3 _lpad; };

// Root-descriptor SRV point-light buffer + hard loop clamp — see the world shader for the full rationale.
struct GPULight { float3 PositionView; float Range; float4 Color; float3 PositionWorld; int ShadowCubeIndex; };
StructuredBuffer<GPULight> Lights : register(t1);
#define LIGHT_CAP 400u

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

float3 DecodeOctNormal( float2 e )
{
    float3 n = float3( e.xy, 1.0 - abs( e.x ) - abs( e.y ) );
    float t = saturate( -n.z );
    n.xy += select(n.xy >= 0., -t, t);
    return normalize( n );
}

float3 AccumPointLights( float3 wpos, float3 N, float3 diffuse )
{
    float3 total = 0;
    uint n = min( LightCount, LIGHT_CAP );
    for ( uint k = 0; k < n; k++ )
    {
        GPULight L = Lights[k];
        float3 dir = L.PositionWorld - wpos;
        float dist = length( dir );
        if ( dist >= L.Range ) continue;
        dir /= dist;
        float ndl = max( 0.0, dot( dir, N ) );
        float nd  = saturate( 1.0 - dist / L.Range );
        float falloff = nd * (nd * 0.2 + 0.8);
        float3 c = saturate( falloff * ndl * L.Color.rgb );
        total += saturate( c * diffuse );
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
    float3 baseLit = t.rgb * i.col.bgr;
    float3 rgb = baseLit + AccumPointLights( i.wpos, normalize( i.wnrm ), t.rgb );
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, FogColor, f ), 1.0 );
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
Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

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
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );          // same cutout as the opaque world PS so gaps don't lay down depth
    return float4( 0, 0, 0, 1 );   // discarded: the PSO's color write mask is 0 (depth-only pass)
}
)";

    // Forward+ tiled light-culling COMPUTE shader (P2.9b-2). One thread group per 16x16 screen tile; each
    // group builds its tile's view-space frustum AABB and records which point lights touch it into a
    // per-tile slice of RW_LightIndexList (fixed MAX_LIGHTS_PER_TILE stride — no global atomic counter, so
    // no counter buffer/clear), with the per-tile {Offset,Count} landing in RW_LightGrid. Adapted from the
    // D3D11 reference Shaders/CS_LightCulling.hlsl with two deliberate divergences for this backend:
    //   1. DEPTH-INDEPENDENT frustum. The reference derives each tile's near/far view-Z by inverting the
    //      depth buffer through the projection (z_view = Proj[3][2]/(depth-Proj[2][2])). That inverse is
    //      only valid for a finite standard-Z projection; our camera is reversed-Z infinite-far, so the
    //      formula does not transfer. Instead we build the tile AABB between FIXED conservative view-Z
    //      bounds [zNear, zFar] — the cull then includes every light whose sphere intersects the tile's
    //      screen cone at ANY depth. This is conservative (never drops a light that should light a pixel),
    //      which is exactly what a Forward+ consumer needs for correctness; depth-buffer tightening (fewer
    //      lights/tile) is a later optimization that will read the prepass depth.
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

cbuffer CullCB : register(b0) {
    float2 ProjScale;    // (Proj._11, Proj._22): view->clip x/y scale (diagonal terms, layout-invariant)
    uint2  ScreenDim;    // render-target pixel size
    uint   TotalLights;  // valid light count in SB_Lights (<= light buffer capacity)
    uint   NumTilesX;    // ceil(ScreenDim.x / TILE_SIZE)
    uint2  _pad;
};

groupshared uint gs_Count;
groupshared uint gs_Indices[MAX_LIGHTS_PER_TILE];

// View-space position of a screen pixel at a chosen view depth. Reversed-Z agnostic: we pick zView directly
// rather than inverting a depth-buffer sample (see header note). ndc.y flipped (screen +y is downward).
float3 ViewCornerAtZ( float2 pixel, float zView ) {
    float2 ndc;
    ndc.x = pixel.x / (float)ScreenDim.x * 2.0 - 1.0;
    ndc.y = -(pixel.y / (float)ScreenDim.y * 2.0 - 1.0);
    return float3( ndc.x / ProjScale.x * zView, ndc.y / ProjScale.y * zView, zView );
}

bool SphereInsideAABB( float3 c, float r, float3 mn, float3 mx ) {
    float3 closest = clamp( c, mn, mx );
    float3 d = closest - c;
    return dot( d, d ) <= r * r;
}

[numthreads( TILE_SIZE, TILE_SIZE, 1 )]
void CSMain( uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID ) {
    uint ti = threadID.y * TILE_SIZE + threadID.x;
    if ( ti == 0 ) gs_Count = 0;
    GroupMemoryBarrierWithGroupSync();

    // Tile frustum AABB in view space between fixed conservative view-Z bounds (depth-independent cull).
    const float zNear = 1.0;
    const float zFar  = 100000.0;
    float2 tileMin = float2( groupID.xy ) * TILE_SIZE;
    float2 tileMax = float2( groupID.xy + uint2( 1, 1 ) ) * TILE_SIZE;
    float3 corners[8];
    corners[0] = ViewCornerAtZ( float2( tileMin.x, tileMin.y ), zNear );
    corners[1] = ViewCornerAtZ( float2( tileMax.x, tileMin.y ), zNear );
    corners[2] = ViewCornerAtZ( float2( tileMin.x, tileMax.y ), zNear );
    corners[3] = ViewCornerAtZ( float2( tileMax.x, tileMax.y ), zNear );
    corners[4] = ViewCornerAtZ( float2( tileMin.x, tileMin.y ), zFar );
    corners[5] = ViewCornerAtZ( float2( tileMax.x, tileMin.y ), zFar );
    corners[6] = ViewCornerAtZ( float2( tileMin.x, tileMax.y ), zFar );
    corners[7] = ViewCornerAtZ( float2( tileMax.x, tileMax.y ), zFar );
    float3 aabbMin = corners[0];
    float3 aabbMax = corners[0];
    [unroll]
    for ( uint c = 1; c < 8; ++c ) { aabbMin = min( aabbMin, corners[c] ); aabbMax = max( aabbMax, corners[c] ); }

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
    float3 rgb = t.rgb * i.col.bgr;
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    rgb = lerp( rgb, FogColor, f );
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
cbuffer LightCB    : register(b4) { uint LightCount; uint3 _lpad; };   // Forward+ MVP brute-force point-light count

// Root-descriptor SRV point-light buffer + hard loop clamp — see the world shader for the full rationale.
struct GPULight { float3 PositionView; float Range; float4 Color; float3 PositionWorld; int ShadowCubeIndex; };
StructuredBuffer<GPULight> Lights : register(t1);
#define LIGHT_CAP 400u

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

// Accumulate dynamic point lights at a world-space surface point (identical math to the world/VOB shaders:
// range cull, N.L, falloff = nd*(nd*0.2+0.8), per-light saturate, additive; specular/shadows are later).
float3 AccumPointLights( float3 wpos, float3 N, float3 diffuse )
{
    float3 total = 0;
    uint n = min( LightCount, LIGHT_CAP );
    for ( uint k = 0; k < n; k++ )
    {
        GPULight L = Lights[k];
        float3 dir = L.PositionWorld - wpos;
        float dist = length( dir );
        if ( dist >= L.Range ) continue;
        dir /= dist;
        float ndl = max( 0.0, dot( dir, N ) );
        float nd  = saturate( 1.0 - dist / L.Range );
        float falloff = nd * (nd * 0.2 + 0.8);
        float3 c = saturate( falloff * ndl * L.Color.rgb );
        total += saturate( c * diffuse );
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
    float3 rgb = t.rgb * i.col.rgb;            // ModelColor is an RGBA float (white for first-light)
    rgb += AccumPointLights( i.wpos, normalize( i.wnrm ), t.rgb );   // dynamic point lights on top
    float f = saturate( ( i.fogDist - FogNear ) / max( 1.0, FogFar - FogNear ) );
    return float4( lerp( rgb, FogColor, f ), 1.0 );
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
    return tx.Sample( smp, i.uv ) * i.dif;   // color = texture * particle diffuse (blend picks add/alpha/mul)
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
    return float4( t.rgb, 1.0 );
}

float4 PSMainBlend( VS_OUT i ) : SV_TARGET // transparent — the PSO blend state picks add/alpha/modulate
{
    float4 t = tx.Sample( smp, i.uv );
    return float4( t.rgb, t.a * i.alpha );
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
    if ( !CreateUploadObjects() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create upload objects.";
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
    LogInfo() << "D3D12GraphicsEngine initialized (device + 2D + world + VOB + skeletal + water + particle + decal pipelines up). Swapchain is created once the game window is set.";
    return XR_SUCCESS;
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

bool D3D12GraphicsEngine::UploadTextureSubresources( ID3D12Resource* dst, const D3D12_SUBRESOURCE_DATA* subresources, UINT numSubresources ) {
    if ( !dst || !subresources || numSubresources == 0 ) return false;
    ID3D12Device* device = m_Device.GetDevice();

    D3D12_RESOURCE_DESC desc = dst->GetDesc();

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts( numSubresources );
    std::vector<UINT>    numRows( numSubresources );
    std::vector<UINT64>  rowSizes( numSubresources );
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints( &desc, 0, numSubresources, 0, layouts.data(), numRows.data(), rowSizes.data(), &totalBytes );

    // Upload (staging) buffer sized to hold all subresources' aligned footprints.
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = totalBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    if ( FAILED( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS( upload.ReleaseAndGetAddressOf() ) ) ) )
        return false;

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

    // Record the copies + transition to a shader-readable state.
    m_UploadAllocator->Reset();
    m_UploadCmdList->Reset( m_UploadAllocator.Get(), nullptr );

    for ( UINT i = 0; i < numSubresources; ++i ) {
        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = dst;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = i;

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = upload.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = layouts[i];

        m_UploadCmdList->CopyTextureRegion( &dstLoc, 0, 0, 0, &srcLoc, nullptr );
    }

    auto toSRV = TransitionBarrier( dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
    m_UploadCmdList->ResourceBarrier( 1, &toSRV );
    m_UploadCmdList->Close();

    ID3D12CommandList* lists[] = { m_UploadCmdList.Get() };
    m_Device.GetDirectQueue()->ExecuteCommandLists( 1, lists );

    const UINT64 waitValue = ++m_UploadFenceValue;
    if ( FAILED( m_Device.GetDirectQueue()->Signal( m_UploadFence.Get(), waitValue ) ) )
        return false;
    if ( m_UploadFence->GetCompletedValue() < waitValue ) {
        m_UploadFence->SetEventOnCompletion( waitValue, m_UploadEvent );
        WaitForSingleObject( m_UploadEvent, INFINITE ); // upload buffer stays alive until here
    }
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
    if ( slot == UINT_MAX || slot == m_WhiteSrvSlot ) return;

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
    device->CreateShaderResourceView( m_WhiteTexture.Get(), &nullDesc, cpuHandle);

    m_FreeSrvSlots.push_back( slot );
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvCpuHandle( UINT slot ) const {
    if ( std::ranges::contains( m_FreeSrvSlots, slot ) ) {
        // Ensure invalid slots provide some texture instead of breaking
        return GetSrvCpuHandle( m_WhiteSrvSlot );
    }

    D3D12_CPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>( slot ) * m_SrvDescriptorSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvGpuHandle( UINT slot ) const {
    if ( std::ranges::contains( m_FreeSrvSlots, slot ) ) {
        // Ensure invalid slots provide some texture instead of breaking
        return GetSrvGpuHandle( m_WhiteSrvSlot );
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
    pso.RTVFormats[0] = kBackBufferFormat;
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
    ID3D12Device* device = m_Device.GetDevice();

    D3D12_HEAP_PROPERTIES heapDefault = {};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 1;
    td.Height = 1;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;

    if ( FAILED( device->CreateCommittedResource( &heapDefault, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS( m_WhiteTexture.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_WhiteTexture->SetName( L"WhiteFallbackTexture" );

    const uint32_t white = 0xFFFFFFFFu;
    D3D12_SUBRESOURCE_DATA sub = {};
    sub.pData = &white;
    sub.RowPitch = 4;
    sub.SlicePitch = 4;
    if ( !UploadTextureSubresources( m_WhiteTexture.Get(), &sub, 1 ) )
        return false;

    m_WhiteSrvSlot = AllocateSrvSlot();
    if ( m_WhiteSrvSlot == UINT_MAX ) return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView( m_WhiteTexture.Get(), &srvd, GetSrvCpuHandle( m_WhiteSrvSlot ) );
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
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // Reversed-Z: the world clears depth to 0.0, so make that the optimized clear value.
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 0.0f;

    // Lives in DEPTH_WRITE for its whole lifetime (only ever written/tested; no SRV read yet).
    if ( FAILED( device->CreateCommittedResource( &heapDefault, D3D12_HEAP_FLAG_NONE, &dd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS( m_DepthBuffer.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: failed to create the depth buffer (" << size.x << "x" << size.y << ").";
        return false;
    }
    m_DepthBuffer->SetName( L"DepthBuffer(D32)" );

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView( m_DepthBuffer.Get(), &dsv, m_DsvHeap->GetCPUDescriptorHandleForHeapStart() );

    // Forward+ tile grid storage is resolution-dependent too — (re)build it here so it always tracks the
    // depth buffer's size (called from both init and the resize path). GPU is idle at both call sites.
    if ( !CreateLightCullBuffers( size ) ) return false;
    return true;
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
    // straight in the root; aligns with the CPU-offload/bindless direction). params[4] = b2 light count.
    // Both params MUST be bound (BindFrameLights) by every draw that uses this root sig with a light-reading
    // PSO (m_WorldPSO/m_VobPSO), else LightCount is an undefined root value and the shader loops away.
    D3D12_ROOT_PARAMETER params[5] = {};
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
    params[4].Constants.ShaderRegister = 2;   // b2 light count
    params[4].Constants.Num32BitValues = 4;   // { LightCount, pad, pad, pad }
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
    pso.RTVFormats[0] = kBackBufferFormat;
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
    // Keep NumRenderTargets=1 with the backbuffer format so the PSO matches the RTV bound in OnBeginFrame
    // (no OMSetRenderTargets change needed) — but mask off all color writes so only depth is touched.
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kBackBufferFormat;
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
    return true;
}

bool D3D12GraphicsEngine::CreateLightCullPipeline() {
    // Forward+ tiled light-culling compute pipeline (P2.9b-2). One GLOBAL compute root signature + PSO,
    // created once. All four bindings are ROOT descriptors (no descriptor tables → no shader-visible-heap
    // plumbing for this pass): b0 = cull constants (8 root 32-bit values); t0 = the point-light
    // StructuredBuffer as a root SRV (same UPLOAD buffer the world PS reads); u0/u1 = the light grid /
    // index-list DEFAULT-heap UAVs as root UAVs. RWStructuredBuffers are valid as root UAVs (stride comes
    // from the HLSL declaration). Mirrors the world path's "structured buffers straight in the root" style.
    ID3D12Device* device = m_Device.GetDevice();

    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0 CullCB
    params[0].Constants.Num32BitValues = 8;   // ProjScale(2) + ScreenDim(2) + TotalLights + NumTilesX + pad(2)
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
    // UAV buffers that live permanently in UNORDERED_ACCESS (only the cull CS writes them; the lit PS will
    // read them next increment), so no state transitions are ever needed. ~1 MB total at 1080p.
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
    pso.RTVFormats[0] = kBackBufferFormat;
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
        ++count;
    }
    m_FrameLightCount = count;
}

void D3D12GraphicsEngine::BindFrameLights( UINT srvParam, UINT countParam ) {
    // Bind the Forward+ point-light root params: srvParam = the light StructuredBuffer as a root SRV (t1),
    // countParam = the light count (LightCB). EVERY draw whose bound PSO reads the light loop MUST call this
    // after setting its root signature, or the pixel shader's loop bound (LightCount) is an UNDEFINED root
    // value and can run billions of iterations → GPU timeout/removal. Root args are cleared on every
    // SetGraphicsRootSignature. The param indices differ per root sig: m_WorldRootSig uses (3,4) — the default —
    // for the world mesh / instanced VOBs / node attachments; m_SkeletalRootSig uses (5,6).
    m_CmdList->SetGraphicsRootShaderResourceView( srvParam, m_LightBuffer[m_FrameIndex]->GetGPUVirtualAddress() );
    m_CmdList->SetGraphicsRoot32BitConstant( countParam, m_FrameLightCount, 0 );
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
    pso.RTVFormats[0] = kBackBufferFormat;
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

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_WhiteSrvSlot );
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
    pso.RTVFormats[0] = kBackBufferFormat;
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

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_WhiteSrvSlot );
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
    pso.RTVFormats[0] = kBackBufferFormat;
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
    pso.RTVFormats[0] = kBackBufferFormat;
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

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_WhiteSrvSlot );
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

    D3D12_ROOT_PARAMETER params[7] = {};
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
    // Forward+ point lights (mirrors m_WorldRootSig params 3/4, here at 5/6 — see BindFrameLights). Both MUST
    // be bound at every skeletal draw or the PS light-loop bound is undefined → GPU hang.
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[5].Descriptor.ShaderRegister = 1;  // t1 light StructuredBuffer (root SRV, no descriptor slot)
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[6].Constants.ShaderRegister = 4;   // b4 light count
    params[6].Constants.Num32BitValues = 4;   // LightCB { uint LightCount; uint3 _lpad; }
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
    pso.RTVFormats[0] = kBackBufferFormat;
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
    D3D12_GPU_DESCRIPTOR_HANDLE srv = GetSrvGpuHandle( m_WhiteSrvSlot );
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

    DrawSky();
    // Forward+ opaque depth prepass (world mesh) — lays down depth before the lit passes so the upcoming
    // tiled light-culling compute has a populated depth buffer. Visually a no-op (the color passes re-pass
    // on GREATER_EQUAL and rewrite the same depth).
    DrawDepthPrepass();
    // Forward+ tiled light cull: consume this frame's light buffer + the tile grid geometry to record which
    // point lights touch each 16x16 screen tile. Produces the grid only (no lit-output change yet).
    DispatchLightCulling();
    DrawWorldMesh();
    {
        DX_ZONE( m_CmdList, "Draw animated skeletal" );
        DrawSkeletalMeshes( Engine::GAPI->GetAnimatedSkeletalMeshVobs(), false );
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
    const float clear[4] = { fc.x, fc.y, fc.z, 1.0f };

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(m_FrameIndex) * m_RtvDescriptorSize;
    m_CmdList->ClearRenderTargetView( rtv, clear, 0, nullptr );
    return XR_SUCCESS;
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

    // Own frustum collection (DrawWorldMesh collects again for its color pass; a redundant BSP walk for now —
    // hoisting to a shared per-frame collection is a follow-up when the passes get factored). Bind each
    // material's diffuse for the alpha-test clip; skip water (transparent, peeled to DrawWaterSurfaces).
    static std::vector<WorldMeshSectionInfo*> sections;
    sections.clear();
    Engine::GAPI->CollectVisibleSections( sections, nullptr, true );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_WhiteSrvSlot );
    zCTexture* boundTex = nullptr;

    for ( WorldMeshSectionInfo* section : sections ) {
        if ( !section ) continue;
        for ( auto const& [meshKey, mesh] : section->WorldMeshes ) {
            if ( !mesh || mesh->Indices.empty() ) continue;
            if ( meshKey.Info && meshKey.Info->MaterialType == MaterialInfo::MT_Water )
                continue;   // transparent — never writes depth (matches the opaque pass peel-out)

            zCTexture* tex = meshKey.Material->GetAniTexture();
            if ( tex != boundTex ) {
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
                boundTex = tex;
            }

            m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mesh->Indices.size()), 1,
                mesh->BaseIndexLocation, 0, 0 );
        }
    }
}

void D3D12GraphicsEngine::DispatchLightCulling() {
    // Forward+ tiled light cull (P2.9b-2). Runs after the depth prepass, before the lit passes: one 16x16
    // thread group per screen tile writes {Offset,Count} into m_LightGridBuffer and the touching light
    // indices into m_LightIndexBuffer. This increment only PRODUCES the grid — nothing consumes it yet, so
    // the frame is visually unchanged; verify by inspecting the two buffers in RenderDoc (per-tile Count
    // should be non-zero where torches/spells are on screen, zero for empty sky). The lit pixel shaders swap
    // their brute-force light loop for this grid in the next increment. The cull is depth-independent
    // (reversed-Z agnostic — see the shader header), so it does NOT sample the prepass depth here.
    if ( !m_FrameOpen || !m_LightCullPSO || !m_LightCullRootSig || !m_LightGridBuffer || !m_LightIndexBuffer )
        return;
    if ( !m_LightBuffer[m_FrameIndex] || m_NumTilesX == 0 || m_NumTilesY == 0 )
        return;

    DX_ZONE( m_CmdList, "Light Culling (compute)" );

    // ProjScale = the projection's x/y view->clip scale (diagonal terms; layout-invariant so no transpose
    // worry). Same projection the geometry passes use.
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();

    struct LightCullConstants {
        float    ProjScaleX, ProjScaleY;
        uint32_t ScreenX, ScreenY;
        uint32_t TotalLights;
        uint32_t NumTilesX;
        uint32_t Pad0, Pad1;
    } cb{};
    cb.ProjScaleX = projM._11;
    cb.ProjScaleY = projM._22;
    cb.ScreenX = static_cast<uint32_t>( m_Resolution.x );
    cb.ScreenY = static_cast<uint32_t>( m_Resolution.y );
    cb.TotalLights = m_FrameLightCount;
    cb.NumTilesX = m_NumTilesX;

    m_CmdList->SetPipelineState( m_LightCullPSO.Get() );
    m_CmdList->SetComputeRootSignature( m_LightCullRootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 8, &cb, 0 );
    m_CmdList->SetComputeRootShaderResourceView( 1, m_LightBuffer[m_FrameIndex]->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootUnorderedAccessView( 2, m_LightGridBuffer->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootUnorderedAccessView( 3, m_LightIndexBuffer->GetGPUVirtualAddress() );
    m_CmdList->Dispatch( m_NumTilesX, m_NumTilesY, 1 );

    // No UAV barrier yet — nothing reads these buffers this increment. It'll be added when the lit pixel
    // shaders consume the grid (P2.9b-3), so their reads are ordered after these writes.
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

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
    D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    m_CmdList->IASetIndexBuffer( &ibv );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // Per-material submission: frustum-cull to the visible sections, then draw each material's index
    // range into the shared buffer (BaseIndexLocation), binding its diffuse texture. Mirrors D3D11's
    // opaque loop minus the water/portal/transparency bucketing + material-CB pooling (later steps).
    static std::vector<WorldMeshSectionInfo*> sections;
    sections.clear();
    Engine::GAPI->CollectVisibleSections( sections, nullptr, true );

    // Start the water collection fresh; water meshes are peeled out below and drawn in DrawWaterSurfaces
    // after all opaque geometry (they share this same VB/IB, so no separate binding is needed there).
    g_FrameWaterSurfaces.clear();

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_WhiteSrvSlot );
    zCTexture* boundTex = nullptr;
    unsigned int drawnIndices = 0;

    for ( WorldMeshSectionInfo* section : sections ) {
        if ( !section ) continue;
        for ( auto const& [meshKey, mesh] : section->WorldMeshes ) {
            if ( !mesh || mesh->Indices.empty() ) continue;

            // Water is transparent — bucket it by texture for the later alpha-blended pass, skip here.
            if ( meshKey.Info && meshKey.Info->MaterialType == MaterialInfo::MT_Water ) {
                g_FrameWaterSurfaces[meshKey.Material->GetAniTexture()].push_back( mesh );
                continue;
            }

            zCTexture* tex = meshKey.Material->GetAniTexture();
            if ( tex != boundTex ) {
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
                boundTex = tex;
            }

            m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mesh->Indices.size()), 1,
                mesh->BaseIndexLocation, 0, 0 );
            drawnIndices += static_cast<unsigned int>(mesh->Indices.size());
        }
    }

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnIndices / 3;
    return XR_SUCCESS;
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

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_WhiteSrvSlot );
    const UINT frame = m_FrameIndex;
    unsigned int drawnTris = 0;

    {
        DX_ZONE( m_CmdList, "Draw Vobs" );
        for ( auto const& [visualPtr, visual] : Engine::GAPI->GetStaticMeshVisuals() ) {
            if ( !visual || visual->Instances.empty() ) continue;

            const UINT numInstances = static_cast<UINT>(visual->Instances.size());
            const UINT instBytes = numInstances * static_cast<UINT>(sizeof( VobInstanceInfo ));

            if ( m_VobInstanceBufferOffset + instBytes > m_VobInstanceBufferCapacity ) {
                if ( !m_VobInstanceOverflowLogged ) {
                    LogWarn() << "D3D12: VOB instance ring overflow (" << m_VobInstanceBufferCapacity
                        << " bytes/frame). Some VOBs dropped this frame.";
                    m_VobInstanceOverflowLogged = true;
                }
                break;
            }

            // Snapshot this visual's instances into the per-frame ring; bind as the slot-1 stream.
            const UINT instOffset = m_VobInstanceBufferOffset;
            memcpy( m_VobInstanceBufferPtr[frame] + instOffset, visual->Instances.data(), instBytes );
            m_VobInstanceBufferOffset += instBytes;
            const D3D12_VERTEX_BUFFER_VIEW instView = {
                m_VobInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( VobInstanceInfo ) };

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
                    const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, instView };
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

    {
        DX_ZONE( m_CmdList, "Draw static skeletal" );
        DrawSkeletalMeshes( g_FrameMobs, false );
    }

    // NOTE: the per-visual Instances lists are cleared once at the end of OnStartWorldRendering (not here),
    // so they still get reset even when DrawVOBs is off and this pass early-outs above.

    rs.RendererInfo.FrameDrawnTriangles += drawnTris;
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::DrawSkeletalMeshes( std::vector<SkeletalVobInfo*>& vobs, bool asMorphMeshes ) {
    if ( !m_FrameOpen || !m_SkeletalPSO || !m_SkeletalRootSig || !m_DepthBuffer )
        return XR_SUCCESS;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    if ( !rs.RendererSettings.DrawSkeletalMeshes )
        return XR_SUCCESS;

    if ( vobs.empty() ) return XR_SUCCESS;

    // Reversed-Z ViewProj (identical derivation to DrawWorldMesh / DrawVobsInstanced).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = rs.TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    const FogConstants fog = MakeFogConstants();

    m_CmdList->SetPipelineState( m_SkeletalPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_SkeletalRootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 4, 8, &fog, 0 );   // b3 fog
    BindFrameLights( 5, 6 );   // param 5 = light SRV (t1), param 6 = light count (b4) — MUST set both (see BindFrameLights)

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const float radius = rs.RendererSettings.SkeletalMeshDrawRadius;
    const XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
    const XMVECTOR radiusSq = XMVectorReplicate( radius * radius );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_WhiteSrvSlot );
    const UINT frame = m_FrameIndex;
    unsigned int drawnTris = 0;
    static std::vector<XMFLOAT4X4> boneCache;

    // Node attachments (weapons, heads, lamps, held items) are static packed meshes placed at a bone
    // node's object-space transform. They're collected here (with the bone pose live) and drawn in a
    // second pass via the VOB pipeline, whose packed-vertex + per-instance-world-matrix format matches.
    struct AttachmentDraw { MeshInfo* mesh; zCTexture* tex; XMFLOAT4X4 world; };
    static std::vector<AttachmentDraw> attachmentDraws;
    attachmentDraws.clear();

    for ( SkeletalVobInfo* vi : vobs ) {
        if ( !vi || !vi->Vob || !vi->VisualInfo ) continue;
        if ( !vi->Vob->GetShowVisual() ) continue;

        SkeletalMeshVisualInfo* visual = static_cast<SkeletalMeshVisualInfo*>( vi->VisualInfo );
        if ( visual->SkeletalMeshes.empty() ) continue;   // interactive MOBs (no base skeletal mesh): skip

        zCModel* model = static_cast<zCModel*>( vi->Vob->GetVisual() );
        if ( !model ) continue;

        if ( XMVector3Greater( XMVector3LengthSq( camPos - vi->Vob->GetPositionWorldXM() ), radiusSq ) )
            continue;   // out of skeletal-draw range

        model->SetDistanceToCamera( 500 );
        model->UpdateAttachedVobs();
        model->UpdateMeshLibTexAniState();

        // Bone palette (object-space matrices) for the model's current animation pose.
        boneCache.clear();
        model->GetBoneTransforms( &boneCache );
        UINT numBones = static_cast<UINT>( boneCache.size() );
        if ( numBones == 0 ) continue;
        if ( numBones > kSkeletalMaxBones ) numBones = kSkeletalMaxBones;

        // Allocate the per-instance CB + bone CB from the per-frame ring (each 256-byte aligned so it can
        // be bound as a root CBV). Both live in one committed upload resource, so any offset is valid GPU
        // memory — the shader only indexes Bones[idx] for idx < numBones, so numBones matrices suffice.
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
        XMMATRIX xmWorld = vi->Vob->GetWorldMatrixXM() * XMMatrixScalingFromVector( model->GetModelScaleXM() );
        XMStoreFloat4x4( &inst.World, xmWorld );
        inst.ModelColor = XMFLOAT4( 1.0f, 1.0f, 1.0f, 1.0f );   // first-light: white; ground-light color is a later step
        inst.Fatness = model->GetModelFatness();

        uint8_t* ringBase = m_SkeletalCBBufferPtr[frame];
        memcpy( ringBase + instOff, &inst, instSize );
        memcpy( ringBase + boneOff, boneCache.data(), boneSize );
        m_SkeletalCBBufferOffset = boneOff + boneSize;

        const D3D12_GPU_VIRTUAL_ADDRESS ringGpu = m_SkeletalCBBuffer[frame]->GetGPUVirtualAddress();
        m_CmdList->SetGraphicsRootConstantBufferView( 1, ringGpu + instOff );
        m_CmdList->SetGraphicsRootConstantBufferView( 2, ringGpu + boneOff );

        for ( auto const& [mat, meshList] : visual->SkeletalMeshes ) {
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

            for ( auto const& mesh : meshList ) {
                if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer )
                    continue;
                D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
                D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
                if ( !mvb->GetResource() || !mib->GetResource() ) continue;

                const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExSkelVertexStruct ) };
                m_CmdList->IASetVertexBuffers( 0, 1, &vbv );

                // Skeletal sub-mesh index buffers are 16-bit (VERTEX_INDEX = unsigned short).
                const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                m_CmdList->IASetIndexBuffer( &ibv );

                m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 1, 0, 0, 0 );
                drawnTris += static_cast<unsigned int>( mesh->Indices.size() ) / 3;
            }
        }

        // Collect this vob's node attachments while its bone pose (boneCache) is live. Each attachment's
        // world = modelWorld * boneMatrix[node] (mirrors D3D11 `world * curTransform`). Lazily convert the
        // node visual on first sight (and re-convert if the node's visual changed, e.g. a weapon drawn).
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
                if ( true ) {
                    node->TexAniState.UpdateTexList();
                    if ( isMMS ) {
                        zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>(mvi->Visual);
                        mm->GetTexAniState()->UpdateTexList();
                    }
                }
                for ( auto const& [attMat, attMeshes] : mvi->Meshes ) {
                    zCTexture* attTex = attMat ? attMat->GetAniTexture() : nullptr;
                    for ( auto const& attMesh : attMeshes ) {
                        if ( !attMesh || attMesh->Indices.empty() ) continue;
                        attachmentDraws.push_back( { attMesh.get(), attTex, attWorld } );
                    }
                }
            }
        }
    }

    // Second pass: draw node attachments through the VOB pipeline (packed 36-byte vertex slot 0 +
    // per-instance world matrix slot 1). One instance per attachment; reuses the VOB instance ring,
    // which DrawVobsInstanced (running right after) continues to fill — the offset advances across both.
    if ( !attachmentDraws.empty() && m_VobPSO && m_WorldRootSig ) {
        m_CmdList->SetPipelineState( m_VobPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_WorldRootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );   // b1 fog (VOB root sig)
        BindFrameLights();   // param 3 = light SRV (t1), param 4 = light count (b2) — the VOB PSO's shader
                             // reads both; omitting them here (as this attachment pass previously did) makes
                             // the light loop run on a garbage count → GPU TDR hang.
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        for ( const AttachmentDraw& a : attachmentDraws ) {
            if ( !a.mesh || !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() ) continue;
            D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
            D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
            if ( !mvb->GetResource() || !mib->GetResource() ) continue;

            const UINT instBytes = static_cast<UINT>( sizeof( VobInstanceInfo ) );
            if ( m_VobInstanceBufferOffset + instBytes > m_VobInstanceBufferCapacity ) {
                if ( !m_VobInstanceOverflowLogged ) {
                    LogWarn() << "D3D12: VOB instance ring overflow (skeletal attachments dropped this frame).";
                    m_VobInstanceOverflowLogged = true;
                }
                break;
            }

            VobInstanceInfo vii = {};
            vii.world = a.world;
            vii.color = 0xFFFFFFFF;   // first-light: white (baked ground-light tint is a later step)
            const UINT instOffset = m_VobInstanceBufferOffset;
            memcpy( m_VobInstanceBufferPtr[frame] + instOffset, &vii, instBytes );
            m_VobInstanceBufferOffset += instBytes;

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

            const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
            const D3D12_VERTEX_BUFFER_VIEW instView = {
                m_VobInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( VobInstanceInfo ) };
            const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, instView };
            m_CmdList->IASetVertexBuffers( 0, 2, views );

            const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
            m_CmdList->IASetIndexBuffer( &ibv );

            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( a.mesh->Indices.size() ), 1, 0, 0, 0 );
            drawnTris += static_cast<unsigned int>( a.mesh->Indices.size() ) / 3;
        }
    }

    rs.RendererInfo.FrameDrawnTriangles += drawnTris;
    return XR_SUCCESS;
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
    m_PerFrameCleanupItems[m_FrameIndex].emplace_back( [this, slot, resource = std::move(resource)]() {
        // Recycle the descriptor slot safely
        this->FreeSrvSlot( slot );

        // The ComPtr 'resource' capture naturally dies here, releasing the ID3D12Resource;
    } );
}

void D3D12GraphicsEngine::QueueResourceForRelease( Microsoft::WRL::ComPtr<ID3D12Resource> resource )
{
    if ( !resource ) return;
    // No slot to recycle — just hold a reference until this frame index comes back around (after its
    // fence is waited on in MoveToNextFrame), then drop it. The capture keeps the resource alive until
    // every command list that could reference it has finished on the GPU.
    m_PerFrameCleanupItems[m_FrameIndex].emplace_back( [resource = std::move(resource)]() {} );
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

bool D3D12GraphicsEngine::CreateSwapChain( INT2 size ) {
    m_Resolution = size;

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = static_cast<UINT>( size.x );
    scd.Height = static_cast<UINT>( size.y );
    scd.Format = kBackBufferFormat;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kBackBufferCount;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
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

    // RTV descriptor heap (one per backbuffer)
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kBackBufferCount;
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

    m_CmdAllocators[m_FrameIndex]->Reset();
    m_CmdList->Reset( m_CmdAllocators[m_FrameIndex].Get(), nullptr );
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
    HRESULT hr = m_SwapChain->Present( vsync ? 1 : 0, 0 );
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
    return CreateDepthBuffer( size );  // GPU is idle (WaitForGpuIdle above), safe to recreate
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
        return XR_SUCCESS;
    }

    ResizeSwapChain( newSize );
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::TriggerResize( INT2 resolution ) {
    return OnResize( resolution );
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

XRESULT D3D12GraphicsEngine::GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool /*includeSuperSampling*/ ) {
    if ( !modeList ) return XR_SUCCESS;
    // Minimal: report the current backbuffer resolution. Full enumeration lands with the
    // shared DXGIHelper / settings-UI work.
    modeList->clear();
    modeList->push_back( DisplayModeInfo( std::max<int>( 1, m_Resolution.x ), std::max<int>( 1, m_Resolution.y ) ) );
    return XR_SUCCESS;
}

BaseLineRenderer* D3D12GraphicsEngine::GetLineRenderer() {
    return m_LineRenderer.get();
}
