#include "../pch.h"
#include "D3D12PipelineState.h"
#include "D3D12ShaderBackend.h"
#include <wrl/client.h>
#include "../Logger.h"

using Microsoft::WRL::ComPtr;

namespace {
    // Duplicated from the engine TU (namespace-scope constants have internal linkage, so each TU
    // keeps its own copy — no ODR concern). Kept in sync with D3D12GraphicsEngine.cpp.
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr const char* Shadermodel_PS = "ps_6_6";
    constexpr const char* Shadermodel_VS = "vs_6_6";
    constexpr const char* Shadermodel_CS = "cs_6_6";

    // D3D12SerializeRootSignature is exported from the already-loaded d3d12.dll (we don't link d3d12.lib).
    typedef HRESULT( WINAPI* PFN_SERIALIZE_ROOT_SIG )( const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob** );
    PFN_SERIALIZE_ROOT_SIG LoadSerializeRootSignature() {
        HMODULE d3d12 = LoadLibraryA( "d3d12.dll" );
        if ( !d3d12 ) return nullptr;
        return reinterpret_cast<PFN_SERIALIZE_ROOT_SIG>( GetProcAddress( d3d12, "D3D12SerializeRootSignature" ) );
    }
}

bool D3D12PipelineState::Init( D3D12Device* device, D3D12ShaderBackend* shaders ) {
    m_Device = device;
    m_Shaders = shaders;
    return m_Device != nullptr && m_Shaders != nullptr;
}

bool D3D12PipelineState::CreateWorld() {
    ID3D12Device* device = m_Device->GetDevice();

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
    // be bound (BindFrameLights) by every draw using this root sig with a light-reading PSO (World.PSO/
    // World.VobPSO), else the count/grid are undefined root values and the shader loops away.
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
        IID_PPV_ARGS( World.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "World.hlsl", "VSMain", Shadermodel_VS, World.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "World.hlsl", "PSMain", Shadermodel_PS, World.PsBlob.ReleaseAndGetAddressOf() ) ) {
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
    pso.pRootSignature = World.RootSig.Get();
    pso.VS = { World.VsBlob->GetBufferPointer(), World.VsBlob->GetBufferSize() };
    pso.PS = { World.PsBlob->GetBufferPointer(), World.PsBlob->GetBufferSize() };
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

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( World.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (world).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateDepthPrepass() {
    // Forward+ opaque depth prepass (P2.9b-1): a depth-only variant of the world-mesh pass. Reuses
    // World.RootSig (only b0 ViewProj + t0/s0 are referenced by the prepass shaders) and the world's
    // packed 36-byte vertex, but binds just Position + TexCoord0, writes NO color (write mask 0), and
    // keeps the exact reversed-Z GREATER_EQUAL depth-write state so the depth it lays down is bit-identical
    // to what the opaque world pass would write. Must run AFTER CreateWorld (needs World.RootSig).
    ID3D12Device* device = m_Device->GetDevice();
    if ( !World.RootSig ) { LogWarn() << "D3D12: depth prepass needs the world root sig."; return false; }

    if ( !m_Shaders->CompileFromFile( "DepthPrepass.hlsl", "VSWorld", Shadermodel_VS, World.DepthPrepassVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "DepthPrepass.hlsl", "PSClip", Shadermodel_PS, World.DepthPrepassPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // Only Position (@0) + TexCoord0 (@20) from the packed 36-byte ExVertexStructGPU (stride comes from the VBV).
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = World.RootSig.Get();
    pso.VS = { World.DepthPrepassVsBlob->GetBufferPointer(), World.DepthPrepassVsBlob->GetBufferSize() };
    pso.PS = { World.DepthPrepassPsBlob->GetBufferPointer(), World.DepthPrepassPsBlob->GetBufferSize() };
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

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( World.DepthPrepassPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (depth prepass).";
        return false;
    }

    // Instanced-VOB depth prepass PSO (P2.9b-4a): same depth-only state, but the VOB two-stream input layout
    // (packed vertex slot 0 + per-instance world matrix slot 1) and the VOB shader's VSDepth/PSDepthClip.
    if ( !m_Shaders->CompileFromFile( "Vob.hlsl", "VSDepth", Shadermodel_VS, World.DepthPrepassVobVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Vob.hlsl", "PSDepthClip", Shadermodel_PS, World.DepthPrepassVobPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // Position (@0) + TexCoord0 (@24) from ExVertexStruct (stride 60 bytes)
    const D3D12_INPUT_ELEMENT_DESC vobLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };

    // pso still carries the depth-only state (color mask 0, GREATER_EQUAL depth-write) — only swap VS/PS/layout.
    pso.VS = { World.DepthPrepassVobVsBlob->GetBufferPointer(), World.DepthPrepassVobVsBlob->GetBufferSize() };
    pso.PS = { World.DepthPrepassVobPsBlob->GetBufferPointer(), World.DepthPrepassVobPsBlob->GetBufferSize() };
    pso.InputLayout = { vobLayout, _countof( vobLayout ) };
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( World.DepthPrepassVobPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB depth prepass).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateVob() {
    // Lit instanced static VOBs. Reuses World.RootSig (b0 ViewProj + t0 SRV + static sampler s0 — identical
    // needs). GPU RESOURCE creation (the per-instance upload ring) stays in the engine, called after this.
    ID3D12Device* device = m_Device->GetDevice();

    if ( !m_Shaders->CompileFromFile( "Vob.hlsl", "VSMain", Shadermodel_VS, World.VobVsBlob.ReleaseAndGetAddressOf() ) ) {
            return false;
    }
    if ( !m_Shaders->CompileFromFile( "Vob.hlsl", "PSMain", Shadermodel_PS, World.VobPsBlob.ReleaseAndGetAddressOf() ) ) {
            return false;
    }

    // Slot 0 = ExVertexStruct (Position@0, Normal@12, TexCoord0@24); slot 1 = per-instance data
    // read from VobInstanceInfo (stride 144): world matrix rows @0/16/32/48, instance color @128.
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,   0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_COLOR",        0, DXGI_FORMAT_R8G8B8A8_UNORM,     1, 128, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };

    // Reuse the world root signature (b0 ViewProj + t0 SRV + static sampler s0 — identical needs).
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = World.RootSig.Get();
    pso.VS = { World.VobVsBlob->GetBufferPointer(), World.VobVsBlob->GetBufferSize() };
    pso.PS = { World.VobPsBlob->GetBufferPointer(), World.VobPsBlob->GetBufferSize() };
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

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( World.VobPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateTonemap() {
    // Fullscreen HDR->swapchain resolve (Phase 3). Exposure * scene HDR -> ACES filmic curve -> R10G10B10A2. Runs
    // once per world frame after all 3D. No vertex buffer (SV_VertexID fullscreen triangle), no depth. Created once.
    ID3D12Device* device = m_Device->GetDevice();
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
        IID_PPV_ARGS( Tonemap.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "Tonemap.hlsl", "VSFullscreen", Shadermodel_VS, Tonemap.VsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "Tonemap.hlsl", "PSTonemap", Shadermodel_PS, Tonemap.PsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = Tonemap.RootSig.Get();
    pso.VS = { Tonemap.VsBlob->GetBufferPointer(), Tonemap.VsBlob->GetBufferSize() };
    pso.PS = { Tonemap.PsBlob->GetBufferPointer(), Tonemap.PsBlob->GetBufferSize() };
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
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Tonemap.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (tonemap).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateWater() {
    ID3D12Device* device = m_Device->GetDevice();

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
        IID_PPV_ARGS( Water.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "Water.hlsl", "VSMain", Shadermodel_VS, Water.VsBlob.ReleaseAndGetAddressOf() ) ) {
            return false;
    }
    if ( !m_Shaders->CompileFromFile( "Water.hlsl", "PSMain", Shadermodel_PS, Water.PsBlob.ReleaseAndGetAddressOf() ) ) {
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
    pso.pRootSignature = Water.RootSig.Get();
    pso.VS = { Water.VsBlob->GetBufferPointer(), Water.VsBlob->GetBufferSize() };
    pso.PS = { Water.PsBlob->GetBufferPointer(), Water.PsBlob->GetBufferSize() };
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

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Water.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (water).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateLightCull() {
    // Forward+ tiled light-culling compute pipeline (P2.9b-2). One GLOBAL compute root signature + PSO,
    // created once. b0 = cull constants (8 root 32-bit values); t0 = the point-light StructuredBuffer as a
    // root SRV (same UPLOAD buffer the world PS reads); u0/u1 = the light grid / index-list DEFAULT-heap UAVs
    // as root UAVs (RWStructuredBuffers are valid as root UAVs; stride comes from the HLSL declaration). t1 =
    // the depth buffer SRV, which (being a Texture2D) can't be a root SRV, so it rides a one-entry descriptor
    // table off the shared SRV heap — used to tighten each tile's far-Z bound (P2.9b-3 flicker fix).
    ID3D12Device* device = m_Device->GetDevice();

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
        IID_PPV_ARGS( LightCull.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "LightCull.hlsl", "CSMain", Shadermodel_CS, LightCull.CsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = LightCull.RootSig.Get();
    pso.CS = { LightCull.CsBlob->GetBufferPointer(), LightCull.CsBlob->GetBufferSize() };
    if ( FAILED( device->CreateComputePipelineState( &pso, IID_PPV_ARGS( LightCull.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateComputePipelineState failed (light cull).";
        return false;
    }
    return true;
}
