#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12LineRenderer.h"
#include "D3D12VertexBuffer.h"
#include "D3D12Texture.h"
#include "../Engine.h"
#include "../GothicAPI.h"
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

    // D3D12SerializeRootSignature is exported from the already-loaded d3d12.dll (we don't link d3d12.lib).
    typedef HRESULT( WINAPI* PFN_SERIALIZE_ROOT_SIG )( const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob** );
    PFN_SERIALIZE_ROOT_SIG LoadSerializeRootSignature() {
        HMODULE d3d12 = LoadLibraryA( "d3d12.dll" );
        if ( !d3d12 ) return nullptr;
        return reinterpret_cast<PFN_SERIALIZE_ROOT_SIG>( GetProcAddress( d3d12, "D3D12SerializeRootSignature" ) );
    }

    // Inline HLSL for the 2D UI path. VS mirrors VS_TransformedEx (screen-space xyzrhw -> clip space,
    // rhw packed in Normal.x); PS does the dominant MODULATE case (texture * diffuse.bgra). The full
    // fixed-function stage/blend emulation lands with the D3D12 shader-backend increment.
    constexpr char kUIShaderSource[] = R"(
cbuffer Viewport : register(b0) { float2 V_ViewportPos; float2 V_ViewportSize; };
Texture2D    tx  : register(t0);
SamplerState smp : register(s0);
struct VS_IN  { float3 pos:POSITION; float3 nrm:NORMAL; float2 t0:TEXCOORD0; float2 t1:TEXCOORD1; float4 dif:DIFFUSE; };
struct VS_OUT { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float4 dif:TEXCOORD1; };
VS_OUT VSMain( VS_IN i ) {
    VS_OUT o;
    float rhw = i.nrm.x;                 // rhw stored in Normal.x
    float2 ndc;
    ndc.x = ((2.0 * (i.pos.x - V_ViewportPos.x)) / V_ViewportSize.x) - 1.0;
    ndc.y = 1.0 - ((2.0 * (i.pos.y - V_ViewportPos.y)) / V_ViewportSize.y);
    float actualW = 1.0 / rhw;
    o.pos = float4(float3(ndc, i.pos.z) * actualW, actualW);
    o.uv  = i.t0;
    o.dif = i.dif;
    return o;
}
float4 PSMain( VS_OUT i ) : SV_TARGET {
    return tx.Sample(smp, i.uv) * i.dif.bgra;
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
    LogInfo() << "D3D12GraphicsEngine initialized (device + 2D pipeline up). Swapchain is created once the game window is set.";
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

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;  // b0
    params[0].Constants.Num32BitValues = 4;  // float2 pos + float2 size
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
#ifdef _DEBUG
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> vs, ps, err;
    if ( FAILED( D3DCompile( kUIShaderSource, sizeof( kUIShaderSource ) - 1, "UIShader", nullptr, nullptr,
        "VSMain", "vs_5_0", compileFlags, 0, vs.GetAddressOf(), err.GetAddressOf() ) ) ) {
        if ( err ) LogWarn() << "D3D12: UI VS compile error: " << static_cast<const char*>( err->GetBufferPointer() );
        return false;
    }
    if ( FAILED( D3DCompile( kUIShaderSource, sizeof( kUIShaderSource ) - 1, "UIShader", nullptr, nullptr,
        "PSMain", "ps_5_0", compileFlags, 0, ps.GetAddressOf(), err.ReleaseAndGetAddressOf() ) ) ) {
        if ( err ) LogWarn() << "D3D12: UI PS compile error: " << static_cast<const char*>( err->GetBufferPointer() );
        return false;
    }

    // --- Input layout: mirrors layout1 (the ExVertexStruct HUD layout; tangent treated as padding) ---
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "DIFFUSE",  0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // --- PSO: alpha-blended, no depth, cull-none, triangle list ---
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_UIRootSig.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kBackBufferFormat;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_UIPipeline.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    return CreateUIVertexBuffers();
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
    if ( !m_SwapChainReady || !m_FrameOpen || !m_UIPipeline || numVertices == 0 || !vertices )
        return XR_SUCCESS;

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

    m_CmdList->SetPipelineState( m_UIPipeline.Get() );
    m_CmdList->SetGraphicsRootSignature( m_UIRootSig.Get() );

    // Viewport transform constants (pixels / GothicUIScale), mirroring D3D11 BindViewportInformation.
    const float scale = std::max<float>( 0.001f, Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale );
    const float vpConsts[4] = {
        m_CurrentViewport.TopLeftX / scale, m_CurrentViewport.TopLeftY / scale,
        m_CurrentViewport.Width / scale,    m_CurrentViewport.Height / scale };
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 4, vpConsts, 0 );

    // Diffuse texture (fall back to 1x1 white for untextured colored draws / failed uploads).
    D3D12_GPU_DESCRIPTOR_HANDLE srv = GetSrvGpuHandle( m_WhiteSrvSlot );
    if ( m_CurrentTexture ) {
        D3D12Texture* t = D3D12Texture::From( m_CurrentTexture );
        if ( t->HasSRV() ) srv = t->GetSrvGpuHandle();
    }
    m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );

    m_CmdList->RSSetViewports( 1, &m_CurrentViewport );
    m_CmdList->RSSetScissorRects( 1, &m_CurrentScissor );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    D3D12_VERTEX_BUFFER_VIEW vbv = { gpuVA, bytes, stride };
    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    m_CmdList->DrawInstanced( numVertices, 1, startVertex, 0 );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += numVertices / 3;
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

    m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, nullptr );
    m_CmdList->ClearRenderTargetView( rtv, m_ClearColor, 0, nullptr );

    // Bind the shader-visible SRV heap for this frame's 2D draws (descriptor tables reference it).
    ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
    m_CmdList->SetDescriptorHeaps( 1, heaps );

    // Reset the per-frame 2D vertex ring + default the viewport to the full backbuffer.
    m_UIVertexBufferOffset = 0;
    m_UIOverflowLogged = false;
    m_CurrentTexture = nullptr;
    m_CurrentViewport = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    m_CurrentScissor = { 0, 0, m_Resolution.x, m_Resolution.y };

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
    return AcquireBackBufferRTVs();
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
