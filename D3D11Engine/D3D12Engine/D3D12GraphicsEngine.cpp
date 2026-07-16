#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12LineRenderer.h"
#include "D3D12VertexBuffer.h"
#include "D3D12Texture.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"
#include "../zCTexture.h"
#include "../D3D7/MyDirectDrawSurface7.h"
#include "../zCView.h"
#include "../VertexTypes.h"
#include "../ImGuiShim.h"

#include <d3dcompiler.h>

// imgui_impl_dx12 calls CreateDXGIFactory1 directly (for tearing detection). dxgi.dll is present on
// every Windows 7+ and the D3D11 fallback swapchain already needs it at runtime, so a load-time link
// here is safe — it does NOT reintroduce the D3D12 soft-dependency that lets old systems fall back.
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr UINT kSrvHeapCapacity = 65536;                  // texture SRVs (tier-1 max is 1M; bump-allocated)
    constexpr UINT kUIVertexBufferBytes = 16 * 1024 * 1024;   // per-frame 2D vertex ring (~280k ExVertex)
    constexpr UINT kVobInstanceBufferBytes = 8 * 1024 * 1024; // per-frame VOB instance ring (~58k instances @144B)

    // D3D12SerializeRootSignature is exported from the already-loaded d3d12.dll (we don't link d3d12.lib).
    typedef HRESULT( WINAPI* PFN_SERIALIZE_ROOT_SIG )( const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob** );
    PFN_SERIALIZE_ROOT_SIG LoadSerializeRootSignature() {
        HMODULE d3d12 = LoadLibraryA( "d3d12.dll" );
        if ( !d3d12 ) return nullptr;
        return reinterpret_cast<PFN_SERIALIZE_ROOT_SIG>( GetProcAddress( d3d12, "D3D12SerializeRootSignature" ) );
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

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

struct VS_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 col : DIFFUSE; };
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    o.clip = mul( float4( i.pos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = i.col;
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );                    // fixed alpha-test cutout (opaque textures have a==1 -> kept)
    float3 rgb = t.rgb * i.col.bgr;       // baked vertex lighting; .bgr recovers Gothic's RGB
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

Texture2D    tx  : register(t0);
SamplerState smp : register(s0);

struct VS_IN
{
    float3   pos     : POSITION;
    float2   uv      : TEXCOORD0;
    float4x4 iworld  : INSTANCE_WORLD_MATRIX;   // 4 per-instance rows (semantic index 0..3)
    float4   icolor  : INSTANCE_COLOR;
};
struct VS_OUT { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; };

VS_OUT VSMain( VS_IN i )
{
    VS_OUT o;
    float3 worldPos = mul( float4( i.pos, 1.0 ), i.iworld ).xyz;
    o.clip = mul( float4( worldPos, 1.0 ), ViewProj );
    o.uv  = i.uv;
    o.col = i.icolor;
    return o;
}

float4 PSMain( VS_OUT i ) : SV_TARGET
{
    float4 t = tx.Sample( smp, i.uv );
    clip( t.a - 0.5 );
    return float4( t.rgb * i.col.bgr, 1.0 );
}
)";

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
    if ( m_SwapChainReady ) WaitForGpuIdle();
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
    if ( !CreateVobPipeline() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the VOB pipeline.";
        return XR_FAILED;
    }
    LogInfo() << "D3D12GraphicsEngine initialized (device + 2D + world + VOB pipelines up). Swapchain is created once the game window is set.";
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
    if ( m_SrvAllocated >= m_SrvHeapCapacity ) {
        LogWarn() << "D3D12: SRV heap exhausted (" << m_SrvHeapCapacity << " descriptors). Further textures won't bind.";
        return UINT_MAX;
    }
    return m_SrvAllocated++;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvCpuHandle( UINT slot ) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>( slot ) * m_SrvDescriptorSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvGpuHandle( UINT slot ) const {
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
#ifdef DEBUG_D3D11
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> err;
    if ( FAILED( D3DCompile( kUIShaderSource, sizeof( kUIShaderSource ) - 1, "UIShader", nullptr, nullptr,
        "VSMain", "vs_5_0", compileFlags, 0, m_UIVsBlob.ReleaseAndGetAddressOf(), err.GetAddressOf() ) ) ) {
        if ( err ) LogWarn() << "D3D12: UI VS compile error: " << static_cast<const char*>( err->GetBufferPointer() );
        return false;
    }
    if ( FAILED( D3DCompile( kUIShaderSource, sizeof( kUIShaderSource ) - 1, "UIShader", nullptr, nullptr,
        "PSMain", "ps_5_0", compileFlags, 0, m_UIPsBlob.ReleaseAndGetAddressOf(), err.ReleaseAndGetAddressOf() ) ) ) {
        if ( err ) LogWarn() << "D3D12: UI PS compile error: " << static_cast<const char*>( err->GetBufferPointer() );
        return false;
    }

    // PSOs are built per blend state on demand (GetOrCreateUIPipeline). Warm the default (opaque) one so
    // any Init-time failure surfaces here rather than mid-frame.
    GothicBlendStateInfo defaultBlend;
    defaultBlend.SetDefault();
    if ( !GetOrCreateUIPipeline( defaultBlend ) ) {
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
        k |= ( b.BlendEnabled ? 1u : 0u );
        k |= ( b.ColorWritesEnabled ? 1u : 0u ) << 1;
        k |= ( b.AlphaToCoverage ? 1u : 0u ) << 2;
        k |= ( static_cast<uint32_t>( b.SrcBlend )      & 0x1F ) << 3;
        k |= ( static_cast<uint32_t>( b.DestBlend )     & 0x1F ) << 8;
        k |= ( static_cast<uint32_t>( b.BlendOp )       & 0x07 ) << 13;
        k |= ( static_cast<uint32_t>( b.SrcBlendAlpha ) & 0x1F ) << 16;
        k |= ( static_cast<uint32_t>( b.DestBlendAlpha )& 0x1F ) << 21;
        k |= ( static_cast<uint32_t>( b.BlendOpAlpha )  & 0x07 ) << 26;
        return k;
    }
}

ID3D12PipelineState* D3D12GraphicsEngine::GetOrCreateUIPipeline( const GothicBlendStateInfo& blend ) {
    const uint32_t key = BlendKey( blend );
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
    pso.RasterizerState.DepthClipEnable = TRUE;

    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = blend.BlendEnabled ? TRUE : FALSE;
    rt.SrcBlend       = static_cast<D3D12_BLEND>( blend.SrcBlend );
    rt.DestBlend      = static_cast<D3D12_BLEND>( blend.DestBlend );
    rt.BlendOp        = static_cast<D3D12_BLEND_OP>( blend.BlendOp );
    rt.SrcBlendAlpha  = static_cast<D3D12_BLEND>( blend.SrcBlendAlpha );
    rt.DestBlendAlpha = static_cast<D3D12_BLEND>( blend.DestBlendAlpha );
    rt.BlendOpAlpha   = static_cast<D3D12_BLEND_OP>( blend.BlendOpAlpha );
    rt.RenderTargetWriteMask = blend.ColorWritesEnabled ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
    pso.BlendState.AlphaToCoverageEnable = blend.AlphaToCoverage ? TRUE : FALSE;

    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;

    ComPtr<ID3D12PipelineState> state;
    if ( FAILED( m_Device.GetDevice()->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( state.GetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed for UI blend key 0x" << std::hex << key << ".";
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
    dd.Width = static_cast<UINT64>( size.x );
    dd.Height = static_cast<UINT>( size.y );
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

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView( m_DepthBuffer.Get(), &dsv, m_DsvHeap->GetCPUDescriptorHandleForHeapStart() );
    return true;
}

bool D3D12GraphicsEngine::CreateWorldPipeline() {
    ID3D12Device* device = m_Device.GetDevice();

    // Root signature: b0 = ViewProj (16 root 32-bit constants, VS); t0 = diffuse SRV table (PS);
    // static linear-wrap sampler s0 (PS).
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;         // t0
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0
    params[0].Constants.Num32BitValues = 16;  // float4x4
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;              // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (world)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: world root signature serialize error: " << static_cast<const char*>( rsErr->GetBufferPointer() );
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( m_WorldRootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    UINT compileFlags = 0;
#ifdef DEBUG_D3D11
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> err;
    if ( FAILED( D3DCompile( kWorldShaderSource, sizeof( kWorldShaderSource ) - 1, "WorldShader", nullptr, nullptr,
        "VSMain", "vs_5_0", compileFlags, 0, m_WorldVsBlob.ReleaseAndGetAddressOf(), err.GetAddressOf() ) ) ) {
        if ( err ) LogWarn() << "D3D12: world VS compile error: " << static_cast<const char*>( err->GetBufferPointer() );
        return false;
    }
    if ( FAILED( D3DCompile( kWorldShaderSource, sizeof( kWorldShaderSource ) - 1, "WorldShader", nullptr, nullptr,
        "PSMain", "ps_5_0", compileFlags, 0, m_WorldPsBlob.ReleaseAndGetAddressOf(), err.ReleaseAndGetAddressOf() ) ) ) {
        if ( err ) LogWarn() << "D3D12: world PS compile error: " << static_cast<const char*>( err->GetBufferPointer() );
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

bool D3D12GraphicsEngine::CreateVobPipeline() {
    ID3D12Device* device = m_Device.GetDevice();

    UINT compileFlags = 0;
#ifdef DEBUG_D3D11
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> err;
    if ( FAILED( D3DCompile( kVobShaderSource, sizeof( kVobShaderSource ) - 1, "VobShader", nullptr, nullptr,
        "VSMain", "vs_5_0", compileFlags, 0, m_VobVsBlob.ReleaseAndGetAddressOf(), err.GetAddressOf() ) ) ) {
        if ( err ) LogWarn() << "D3D12: VOB VS compile error: " << static_cast<const char*>( err->GetBufferPointer() );
        return false;
    }
    if ( FAILED( D3DCompile( kVobShaderSource, sizeof( kVobShaderSource ) - 1, "VobShader", nullptr, nullptr,
        "PSMain", "ps_5_0", compileFlags, 0, m_VobPsBlob.ReleaseAndGetAddressOf(), err.ReleaseAndGetAddressOf() ) ) ) {
        if ( err ) LogWarn() << "D3D12: VOB PS compile error: " << static_cast<const char*>( err->GetBufferPointer() );
        return false;
    }

    // Slot 0 = packed 36-byte ExVertexStructGPU (Position@0, TexCoord0@20); slot 1 = per-instance data
    // read from VobInstanceInfo (stride 144): world matrix rows @0/16/32/48, instance color @128.
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( m_VobInstanceBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_VobInstanceBufferPtr[i] ) ) ) )
            return false;
    }
    m_VobInstanceBufferCapacity = kVobInstanceBufferBytes;
    return true;
}

XRESULT D3D12GraphicsEngine::SetViewport( const ViewportInfo& vp ) {
    m_CurrentViewport.TopLeftX = static_cast<float>( vp.TopLeftX );
    m_CurrentViewport.TopLeftY = static_cast<float>( vp.TopLeftY );
    m_CurrentViewport.Width    = static_cast<float>( vp.Width );
    m_CurrentViewport.Height   = static_cast<float>( vp.Height );
    m_CurrentViewport.MinDepth = vp.MinZ;
    m_CurrentViewport.MaxDepth = vp.MaxZ;
    m_CurrentScissor = {
        static_cast<LONG>( vp.TopLeftX ), static_cast<LONG>( vp.TopLeftY ),
        static_cast<LONG>( vp.TopLeftX + vp.Width ), static_cast<LONG>( vp.TopLeftY + vp.Height ) };
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
    if ( !m_SwapChainReady || !m_FrameOpen || !m_UIRootSig || numVertices == 0 || !vertices )
        return XR_SUCCESS;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();

    // Emulate Gothic's per-draw fixed-function blend mode by selecting the matching PSO.
    ID3D12PipelineState* pso = GetOrCreateUIPipeline( rs.BlendState );
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

    const uint8_t* base = static_cast<const uint8_t*>( D3D12VertexBuffer::From( vb )->GetMappedData() );
    if ( !base ) return XR_SUCCESS;

    const Gothic_XYZRHW_DIF_T1_Vertex* src =
        reinterpret_cast<const Gothic_XYZRHW_DIF_T1_Vertex*>( base + static_cast<size_t>( startVertex ) * stride );

    static std::vector<ExVertexStruct> exv; // reused; the render path is single-threaded (matches DrawPrimitive)
    exv.resize( numVertices );
    for ( unsigned int i = 0; i < numVertices; ++i ) {
        exv[i].Position = src[i].xyz;
        exv[i].Normal.x = src[i].rhw;
        exv[i].TexCoord = src[i].texCoord;
        exv[i].Color    = src[i].color;
    }

    return DrawVertexArray( exv.data(), numVertices, 0, sizeof( ExVertexStruct ) );
}

XRESULT D3D12GraphicsEngine::OnStartWorldRendering() {
    // zCBspNodeRender hook — Gothic's BSP traversal is replaced; we draw the world ourselves.
    DrawWorldMesh();
    DrawVobsInstanced();
    return XR_SUCCESS;
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

    // Camera matrices — replicate the D3D11 DrawWorldMesh setup exactly so ViewProj is byte-identical:
    // world verts are already world-space (identity world), transform is proj*view (reversed-Z).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetPipelineState( m_WorldPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_WorldRootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
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

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_WhiteSrvSlot );
    zCTexture* boundTex = nullptr;
    unsigned int drawnIndices = 0;

    for ( WorldMeshSectionInfo* section : sections ) {
        if ( !section ) continue;
        for ( auto const& [meshKey, mesh] : section->WorldMeshes ) {
            if ( !mesh || mesh->Indices.empty() ) continue;

            zCTexture* tex = meshKey.Texture;
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

            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 1,
                mesh->BaseIndexLocation, 0, 0 );
            drawnIndices += static_cast<unsigned int>( mesh->Indices.size() );
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

    // Collect visible VOBs — this fills each visual's Instances list (world matrices grouped by
    // visual). COLLECT_ALL_MUTATE + CullAll are the defaults; mirrors D3D11's DrawVOBsInstanced.
    static std::vector<VobInfo*> vobs;
    static std::vector<VobLightInfo*> lights;
    static std::vector<SkeletalVobInfo*> mobs;
    vobs.clear(); lights.clear(); mobs.clear();
    Engine::GAPI->CollectVisibleVobs( vobs, lights, mobs );

    // Reversed-Z ViewProj (recomputed; identical derivation to DrawWorldMesh).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = rs.TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetPipelineState( m_VobPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_WorldRootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_WhiteSrvSlot );
    const UINT frame = m_FrameIndex;
    unsigned int drawnTris = 0;

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

        // Snapshot this visual's instances into the per-frame ring; bind as the slot-1 stream.
        const UINT instOffset = m_VobInstanceBufferOffset;
        memcpy( m_VobInstanceBufferPtr[frame] + instOffset, visual->Instances.data(), instBytes );
        m_VobInstanceBufferOffset += instBytes;
        const D3D12_VERTEX_BUFFER_VIEW instView = {
            m_VobInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( VobInstanceInfo ) };

        for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
            zCTexture* tex = meshKey.Texture;
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

                m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mi->Indices.size() ), numInstances, 0, 0, 0 );
                drawnTris += ( static_cast<unsigned int>( mi->Indices.size() ) / 3 ) * numInstances;
            }
        }
    }

    // Clear the per-visual instance lists so next frame's CollectVisibleVobs starts fresh (mirrors D3D11).
    for ( auto const& [visualPtr, visual] : Engine::GAPI->GetStaticMeshVisuals() ) {
        if ( visual ) visual->Instances.clear();
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
        device->CreateRenderTargetView( m_BackBuffers[i].Get(), nullptr, rtvHandle );
        rtvHandle.ptr += m_RtvDescriptorSize;
    }
    return true;
}

XRESULT D3D12GraphicsEngine::OnBeginFrame() {
    if ( !m_SwapChainReady ) return XR_SUCCESS;

    m_CmdAllocators[m_FrameIndex]->Reset();
    m_CmdList->Reset( m_CmdAllocators[m_FrameIndex].Get(), nullptr );

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
        LogWarn() << "D3D12 Present failed (0x" << std::hex << hr << ").";
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
}

void D3D12GraphicsEngine::WaitForGpuIdle() {
    if ( !m_Fence || !m_Device.GetDirectQueue() ) return;

    const UINT64 value = m_FenceValues[m_FrameIndex];
    if ( FAILED( m_Device.GetDirectQueue()->Signal( m_Fence.Get(), value ) ) ) return;

    if ( m_Fence->GetCompletedValue() < value ) {
        m_Fence->SetEventOnCompletion( value, m_FenceEvent );
        WaitForSingleObject( m_FenceEvent, INFINITE );
    }
    m_FenceValues[m_FrameIndex]++;
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
