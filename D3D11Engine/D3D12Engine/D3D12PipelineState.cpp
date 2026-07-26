#include "../pch.h"
#include "D3D12PipelineState.h"
#include "D3D12ShaderBackend.h"
#include <wrl/client.h>
#include "../Logger.h"
#include "../GothicGraphicsState.h"   // GothicBlendStateInfo / GothicDepthBufferStateInfo (full defs for BlendKey/DepthKey)

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

    // The decal input layout is shared by the lit + every transparent PSO: slot 0 = the unit quad
    // (POSITION @0, TEXCOORD0 @12, stride 20), slot 1 = per-instance DecalInstanceInfo (world rows
    // @0/16/32/48, color @64, stride 80). Duplicated from the engine TU (internal linkage, no ODR concern).
    const D3D12_INPUT_ELEMENT_DESC kDecalInputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_COLOR",        0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };
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

    D3D12_ROOT_PARAMETER params[13] = {};
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
    params[10].Constants.ShaderRegister = 6;   // b6 MaterialCB { MatNormalIndex, MatOrmIndex, MatDiffuseIndex, MatNormalStrength }
    params[10].Constants.Num32BitValues = 4;   // 3rd = bindless diffuse index; 4th = normal-perturb strength (World.hlsl's wet-ground
                                                // path only — Vob.hlsl/Skeletal.hlsl's own MaterialCB declarations just don't read it)
    params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // params[11] = wind sway CB (b4, VS only) — read by Vob.hlsl's VSMain (flags/foliage sway + hero-affects-
    // bushes push); World.hlsl/Skeletal.hlsl don't declare b4 so they simply never read it. Only DrawVobsInstanced
    // needs to actually bind this root param before its draws; other users of this root sig leave it unbound.
    params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[11].Constants.ShaderRegister = 4;   // b4 WindCB (VS_ExConstantBuffer_Wind, 48 bytes)
    params[11].Constants.Num32BitValues = 12;
    params[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    // params[12] = simple-SSAO mask bindless SRV-heap index (b7 AOCB, PS only), set ONCE per frame (not per
    // draw/ExecuteIndirect command) by DrawWorldMesh/DrawVobsInstanced/DrawSkeletalColor's attachment pass —
    // World.hlsl/Vob.hlsl's PSMain(Bindless) read it via ResourceDescriptorHeap[AoMaskIndex]. Points at the
    // white texture's SRV slot when SSAO is disabled/unavailable (mask = no occlusion, matches D3D11's default).
    params[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[12].Constants.ShaderRegister = 7;   // b7 AOCB { AoMaskIndex }
    params[12].Constants.Num32BitValues = 1;
    params[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[3] = {};
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
    // s1: point-clamp for the simple-SSAO mask fetch (World/Vob PSMain). MUST be Sample/SampleLevel with CLAMP
    // addressing, not Load — Load() with out-of-range integer texel coords (the 1x1 white "AO disabled"
    // fallback read at full-res screen coords) returns 0 per the HLSL spec, not the texel's actual value, which
    // was silently zeroing the ambient term (darker with AO "disabled" than with it on — the opposite of what
    // should happen). CLAMP addressing sidesteps this for both the 1x1 fallback and the real full-res mask.
    samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[2].ShaderRegister = 1;           // s1 AO mask point-clamp
    samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
    // CULL_BACK matches D3D11's GothicRasterizerStateInfo::SetDefault() (CM_CULL_BACK), the standard cull
    // mode for opaque world/VOB/skeletal geometry there. CULL_NONE (as this used to be) draws BOTH faces of
    // every triangle; for THIN geometry (leaf cards, ice slabs, etc. — often two nearly-coincident surfaces)
    // that turns a normal single-sided draw into two near-identical depths fighting every frame, which reads
    // as flicker. D3D11 never disables culling for ordinary world/VOB/skeletal draws — only the dedicated
    // GVegetationBox grass path (not yet ported to D3D12) opts into CM_CULL_NONE for actual double-sided cards.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
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

bool D3D12PipelineState::CreatePreview() {
    // Single-VOB inventory-item preview (GInventory::DrawVobSingle): drawn straight onto the swapchain
    // backbuffer + its depth buffer from Gothic's own UI-phase zCWorld::Render hook, not through the
    // Forward+ scene passes. Mirrors D3D11's VS_Ex + PS_Preview_Textured (RENDERMODE==1: plain textured,
    // alpha-clip, no lighting/fog) — its own minimal root sig, no Forward+ light/shadow bindings needed.
    ID3D12Device* device = m_Device->GetDevice();

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;   // t0
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0 ViewProj
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;   // b1 World (per-instance, single draw — no instance buffer needed)
    params[1].Constants.Num32BitValues = 16;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srvRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s0 diffuse: 16x anisotropic wrap, matches D3D11's DefaultSamplerState used for this draw.
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.MaxAnisotropy = 16;
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
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (preview)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: preview root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( Preview.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "Preview.hlsl", "VSMain", Shadermodel_VS, Preview.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Preview.hlsl", "PSMain", Shadermodel_PS, Preview.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // MeshInfo (VOB submesh) vertex buffers are laid out as the CPU-side ExVertexStruct (stride 60 bytes,
    // NOT the packed 36-byte ExVertexStructGPU world format) — Position @0, TexCoord0 @24 (matches Vob.hlsl's
    // VSDepth/DepthPrepassVobPSO layout above). Normal/TexCoord2/Color/Tangent unused (unlit preview).
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = Preview.RootSig.Get();
    pso.VS = { Preview.VsBlob->GetBufferPointer(), Preview.VsBlob->GetBufferSize() };
    pso.PS = { Preview.PsBlob->GetBufferPointer(), Preview.PsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kBackBufferFormat;   // drawn directly onto the swapchain backbuffer, not the HDR scene target
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;   // matches D3D11's explicit CM_CULL_BACK for this draw
    pso.RasterizerState.DepthClipEnable = TRUE;

    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Reversed-Z: test + write against the shared swapchain depth buffer (cleared to 0.0, GREATER_EQUAL) —
    // matches D3D11's comment that the swapchain-sized depth buffer must be bound or the preview looks wrong.
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Preview.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (preview).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateGhost() {
    // Ghost/transparency VOBs (GothicAPI::TransparencyVobs — invisible-potion/fade-out items, GetVisualAlpha()):
    // single-object, non-instanced, alpha-blended draw. Mirrors D3D11's PS_Transparency (unlit: sample diffuse,
    // alpha *= per-vob GhostAlpha) — reuses Preview.hlsl's VSMain (identical single-object World/ViewProj layout)
    // plus a new PSGhost entry point, with its own root sig (adds the GhostAlpha root constant Preview lacks).
    // Simplification vs. D3D11: D3D11 does a same-mesh Z-prepass first so a ghost's own back faces don't double-
    // blend through its front faces; this single-pass version skips that (rare/minor artifact on chunky ghost
    // meshes, acceptable for a niche effect) — no depth WRITE either, so multiple overlapping ghosts all show.
    ID3D12Device* device = m_Device->GetDevice();

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;   // t0
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0 ViewProj
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;   // b1 World (per-instance, single draw — no instance buffer needed)
    params[1].Constants.Num32BitValues = 16;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 2;   // b2 GhostAlpha
    params[2].Constants.Num32BitValues = 1;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &srvRange;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s0 diffuse: matches Preview's sampler (16x anisotropic wrap).
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.MaxAnisotropy = 16;
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
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (ghost)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: ghost root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( Ghost.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "Preview.hlsl", "VSMain", Shadermodel_VS, Ghost.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Preview.hlsl", "PSGhost", Shadermodel_PS, Ghost.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // Position (@0) + TexCoord0 (@24) from ExVertexStruct — identical to Preview's layout (same CPU-side mesh format).
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = Ghost.RootSig.Get();
    pso.VS = { Ghost.VsBlob->GetBufferPointer(), Ghost.VsBlob->GetBufferSize() };
    pso.PS = { Ghost.PsBlob->GetBufferPointer(), Ghost.PsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;   // ghosts draw into the HDR scene color, before the tonemap resolve
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;   // matches D3D11's RasterizerState.SetDefault() for ghosts
    pso.RasterizerState.DepthClipEnable = TRUE;

    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;    // don't occlude other ghosts/opaque geo
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; // reversed-Z, tested against opaque depth
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Ghost.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (ghost).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateGhostSkeletal() {
    // Skeletal ghost VOBs (GothicAPI::TransparencyVobs.skeletalVob — invisible/fading NPCs): reuses Skeletal.hlsl's
    // VSDepth (identical matrix-palette skinning pose to the color/prepass/shadow draws — same b0/b1/b2 cbuffers
    // declared once at the top of that file) plus a new PSGhost entry point there. Own root sig, same shape as
    // the non-skeletal Ghost pipeline but with the skinned b1 (per-instance)/b2 (bone-palette) root CBVs instead
    // of a single World root-constant, since the bone palette (up to 96 matrices) far exceeds the root-constant
    // budget — same reasoning as Skeletal.RootSig itself. No same-mesh Z-prepass (matches the non-skeletal
    // Ghost's simplification — rare/minor artifact on chunky ghost meshes, acceptable for a niche effect).
    ID3D12Device* device = m_Device->GetDevice();

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;   // t0
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[5] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0 ViewProj
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;  // b1 per-instance (World/ModelColor/Fatness)
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 2;  // b2 bone palette
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.ShaderRegister = 7;   // b7 GhostAlpha (Skeletal.hlsl's b0..b6 are all spoken for)
    params[3].Constants.Num32BitValues = 1;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &srvRange;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.MaxAnisotropy = 16;
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
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (ghost skeletal)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: ghost skeletal root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( GhostSkeletal.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "Skeletal.hlsl", "VSDepth", Shadermodel_VS, GhostSkeletal.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Skeletal.hlsl", "PSGhost", Shadermodel_PS, GhostSkeletal.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // Same skinned-vertex layout as Skeletal.RootSig's PSOs — VSDepth (VS_IN) needs BONEIDS/WEIGHTS too.
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
    pso.pRootSignature = GhostSkeletal.RootSig.Get();
    pso.VS = { GhostSkeletal.VsBlob->GetBufferPointer(), GhostSkeletal.VsBlob->GetBufferSize() };
    pso.PS = { GhostSkeletal.PsBlob->GetBufferPointer(), GhostSkeletal.PsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.RasterizerState.DepthClipEnable = TRUE;

    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( GhostSkeletal.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (ghost skeletal).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateGrass() {
    // GVegetationBox instanced grass cards (P2.12) — own root sig rather than reusing World.RootSig: grass
    // needs a SECOND texture (the ground/undercoat tint sampled beneath the blade, GVegetationBox::MeshTexture)
    // and its own small per-frame CB (wind/hero-push/time), neither of which cleanly fits World.RootSig's shape.
    // Same Forward+ tiled-light + CSM-shadow param shapes as World/Vob/Skeletal, just renumbered for this sig
    // (including the point-shadow cube table — PBRLighting.hlsl's AccumTiledPointLights hard-requires that
    // symbol to be declared and bound, even though grass rarely sits in a torch's small shadow radius).
    ID3D12Device* device = m_Device->GetDevice();

    D3D12_DESCRIPTOR_RANGE grassSrvRange = {};
    grassSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    grassSrvRange.NumDescriptors = 1;
    grassSrvRange.BaseShaderRegister = 0;   // t0 grass blade texture
    grassSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE groundSrvRange = {};
    groundSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    groundSrvRange.NumDescriptors = 1;
    groundSrvRange.BaseShaderRegister = 1;   // t1 ground/undercoat texture
    groundSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE shadowSrvRange = {};
    shadowSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowSrvRange.NumDescriptors = 1;
    shadowSrvRange.BaseShaderRegister = 5;   // t5 CSM shadow-map array
    shadowSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE cubeSrvRange = {};
    cubeSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    cubeSrvRange.NumDescriptors = 1;
    cubeSrvRange.BaseShaderRegister = 6;   // t6 point-shadow cube array (PBRLighting.hlsl requires this symbol)
    cubeSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[13] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0 ViewProj
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &grassSrvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &groundSrvRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.ShaderRegister = 1;   // b1 GrassCB { Time, WindStrength, HeroAffectStrength, pad, PlayerPosWS, pad }
    params[3].Constants.Num32BitValues = 8;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;   // VS: all of it; PS: none currently, but cheap to keep visible
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].Constants.ShaderRegister = 2;   // b2 fog
    params[4].Constants.Num32BitValues = 8;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;   // VS: CamPosWS; PS: color/near/far
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[5].Descriptor.ShaderRegister = 2;  // t2 light StructuredBuffer (root SRV)
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[6].Constants.ShaderRegister = 3;   // b3 { LightCount, NumTilesX, LimitLightIntensity, pad }
    params[6].Constants.Num32BitValues = 4;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[7].Descriptor.ShaderRegister = 3;  // t3 per-tile LightGrid
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[8].Descriptor.ShaderRegister = 4;  // t4 per-tile light-index list
    params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[9].Descriptor.ShaderRegister = 4;  // b4 shadow-sampling CB (root CBV)
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[10].DescriptorTable.NumDescriptorRanges = 1;
    params[10].DescriptorTable.pDescriptorRanges = &shadowSrvRange;
    params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[11].DescriptorTable.NumDescriptorRanges = 1;
    params[11].DescriptorTable.pDescriptorRanges = &cubeSrvRange;
    params[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // params[12] = simple-SSAO mask bindless SRV-heap index (b5 AOCB, PS only), set once per frame by
    // DrawVegetation. Grass gets real AO now that RenderSSAO builds the mask from the PREVIOUS frame's COMPLETE
    // depth rather than the depth prepass grass never joins — see Vegetation.hlsl's PSMain. The grass shadow
    // CASTER (m_ShadowCasterGrassPSO, CreateGrassShadowCaster) shares this root sig but reads none of this.
    params[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[12].Constants.ShaderRegister = 5;   // b5 AOCB { AoMaskIndex }
    params[12].Constants.Num32BitValues = 1;
    params[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[3] = {};
    samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
    samplers[0].MaxAnisotropy = 16;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;          // s0 diffuse
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;   // s2 PCF, matches World/Vob/Skeletal
    samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 2;          // s2 shadow comparison
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s1: point-clamp for the AO mask + the reprojection depth fetch — identical to World/Vob/Skeletal's s1.
    samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[2].ShaderRegister = 1;          // s1 AO mask point-clamp
    samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof( params );
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = _countof( samplers );
    rsDesc.pStaticSamplers = samplers;
    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED: the PS reaches the AO mask and the previous-frame depth through
    // SM6.6 ResourceDescriptorHeap[...] (see ScreenSpaceAO.hlsl), same as the World/Vob/Skeletal sigs.
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                 | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (grass)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: grass root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( Grass.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "Vegetation.hlsl", "VSMain", Shadermodel_VS, Grass.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Vegetation.hlsl", "PSMain", Shadermodel_PS, Grass.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // Slot 0 = SimpleObjectVertexStruct (Position@0, TexCoord@12, stride 20); slot 1 = per-instance world matrix
    // (GVegetationBox::VegetationSpots, stride 64, already transposed on upload).
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = Grass.RootSig.Get();
    pso.VS = { Grass.VsBlob->GetBufferPointer(), Grass.VsBlob->GetBufferSize() };
    pso.PS = { Grass.PsBlob->GetBufferPointer(), Grass.PsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // CULL_NONE: grass cards are thin single-sided planes crossed at angles to fake volume — D3D11 uses the
    // same CM_CULL_NONE for this exact reason (see D3D11 GVegetationBox::PrepareRenderGeometryPipeline).
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;   // reversed-Z
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Grass.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (grass).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateVideo() {
    // Bink cutscene playback (zBinkPlayer.cpp): a single pre-transformed (XYZRHW) fullscreen-ish quad,
    // sampling three R8 planes (Y/U/V) and converting to RGB in the PS. Mirrors D3D11's VS_TransformedEx +
    // PS_Video. No blend/depth variants — zBinkPlayer always disables alpha blend/z-write/z-test/culling/fog
    // itself via the D3D7 zRenderer state before drawing, same as the opaque default below.
    ID3D12Device* device = m_Device->GetDevice();

    // Three SEPARATE single-descriptor tables (not one 3-wide range): the Y/U/V planes are independent
    // GfxTextures, each with its own persistent slot in the engine's SRV heap allocated at texture-Init
    // time — they are not contiguous, so a single multi-descriptor range/table wouldn't be valid here.
    D3D12_DESCRIPTOR_RANGE srvRanges[3] = {};
    for ( UINT i = 0; i < 3; ++i ) {
        srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors = 1;
        srvRanges[i].BaseShaderRegister = i;   // t0 Y, t1 U, t2 V
        srvRanges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }

    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;  // b0 viewport (float2 pos + float2 size)
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    for ( UINT i = 0; i < 3; ++i ) {
        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[1 + i].DescriptorTable.pDescriptorRanges = &srvRanges[i];
        params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;  // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof( params );
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (video)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: video root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( Video.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "Video.hlsl", "VSMain", Shadermodel_VS, Video.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Video.hlsl", "PSMain", Shadermodel_PS, Video.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // Same ExVertexStruct HUD layout as the 2D/UI pipeline (rhw packed into Normal.x).
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "DIFFUSE",  0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = Video.RootSig.Get();
    pso.VS = { Video.VsBlob->GetBufferPointer(), Video.VsBlob->GetBufferSize() };
    pso.PS = { Video.PsBlob->GetBufferPointer(), Video.PsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kBackBufferFormat;   // drawn straight onto the swapchain backbuffer, like the 2D/UI path
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;    // zBinkPlayer disables culling before drawing
    pso.RasterizerState.DepthClipEnable = FALSE;

    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // No depth test/write, no DSV bound — zBinkPlayer sets the D3D7 z-compare to "always pass"/z-write off
    // before drawing, and the frame is otherwise a plain 2D overlay (matches the 2D/UI depth-disabled path).
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Video.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (video).";
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
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;   // match the world/VOB color PSOs' cull mode
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

    // Position (@0) + TexCoord0 (@24) from ExVertexStruct (stride 60 bytes). VSDepth now applies the same wind
    // sway as VSMain (ApplyVobWind, Vob.hlsl) so the prepass depth matches the color pass exactly for swaying
    // VOBs — it unconditionally reads INSTANCE_WINDFLUENCE, so every PSO built from this VS blob needs the
    // element (node attachments/non-wind VOBs just carry zeroes there, a no-op per ApplyVobWind's iwind>0 gate).
    const D3D12_INPUT_ELEMENT_DESC vobLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WINDFLUENCE",  0, DXGI_FORMAT_R32G32_FLOAT,       1, 132, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };

    // pso still carries the depth-only state (color mask 0, GREATER_EQUAL depth-write) — only swap VS/PS/layout.
    pso.VS = { World.DepthPrepassVobVsBlob->GetBufferPointer(), World.DepthPrepassVobVsBlob->GetBufferSize() };
    pso.PS = { World.DepthPrepassVobPsBlob->GetBufferPointer(), World.DepthPrepassVobPsBlob->GetBufferSize() };
    pso.InputLayout = { vobLayout, _countof( vobLayout ) };
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( World.DepthPrepassVobPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB depth prepass).";
        return false;
    }

    // Node-attachment depth-prepass variant (VSDepthAttach: Fatness/Scaling inflate-along-normal instead of
    // wind — see Vob.hlsl). Needs NORMAL in the layout (the plain VOB depth prepass doesn't read it at all),
    // so this is its own input layout, not a reuse of vobLayout above. Reuses PSDepthClip (DepthPrepassVobPsBlob)
    // unchanged — alpha-cutout doesn't depend on the fatness inflate.
    if ( !m_Shaders->CompileFromFile( "Vob.hlsl", "VSDepthAttach", Shadermodel_VS, World.DepthPrepassVobAttachVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    const D3D12_INPUT_ELEMENT_DESC vobAttachLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_COLOR",        0, DXGI_FORMAT_R8G8B8A8_UNORM,     1, 128, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WINDFLUENCE",  0, DXGI_FORMAT_R32G32_FLOAT,       1, 132, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };
    pso.VS = { World.DepthPrepassVobAttachVsBlob->GetBufferPointer(), World.DepthPrepassVobAttachVsBlob->GetBufferSize() };
    pso.PS = { World.DepthPrepassVobPsBlob->GetBufferPointer(), World.DepthPrepassVobPsBlob->GetBufferSize() };
    pso.InputLayout = { vobAttachLayout, _countof( vobAttachLayout ) };
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( World.DepthPrepassVobAttachPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB attachment depth prepass).";
        return false;
    }

    // Bindless-diffuse VOB depth-prepass PSO (ExecuteIndirect, P2.12): same VSDepth blob + wind-only vobLayout +
    // depth-only state as DepthPrepassVobPSO, only the PS swapped to PSDepthClipBindless (diffuse alpha-clip from
    // the SRV heap by b6.MatDiffuseIndex). Consumed by the one ExecuteIndirect the VOB depth prepass now issues.
    if ( !m_Shaders->CompileFromFile( "Vob.hlsl", "PSDepthClipBindless", Shadermodel_PS, World.DepthPrepassVobIndirectPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    pso.VS = { World.DepthPrepassVobVsBlob->GetBufferPointer(), World.DepthPrepassVobVsBlob->GetBufferSize() };
    pso.PS = { World.DepthPrepassVobIndirectPsBlob->GetBufferPointer(), World.DepthPrepassVobIndirectPsBlob->GetBufferSize() };
    pso.InputLayout = { vobLayout, _countof( vobLayout ) };
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( World.DepthPrepassVobIndirectPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB depth prepass, indirect).";
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
    // read from VobInstanceInfo (stride 144): world matrix rows @0/16/32/48, instance color @128,
    // {windStrenth, canBeAffectedByPlayer} @132 (see Vob.hlsl's wind sway — VSMain only, not the depth prepass).
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,   0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_COLOR",        0, DXGI_FORMAT_R8G8B8A8_UNORM,     1, 128, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WINDFLUENCE",  0, DXGI_FORMAT_R32G32_FLOAT,       1, 132, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
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
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;   // matches D3D11's CM_CULL_BACK default for VOBs
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

    // Node-attachment color variant (VSMainAttach: Fatness/Scaling instead of wind — see Vob.hlsl). Reuses this
    // same input `layout` (already has NORMAL, needed for the fatness-along-normal inflate) and PSMain unchanged.
    if ( !m_Shaders->CompileFromFile( "Vob.hlsl", "VSMainAttach", Shadermodel_VS, World.VobAttachVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    pso.VS = { World.VobAttachVsBlob->GetBufferPointer(), World.VobAttachVsBlob->GetBufferSize() };
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( World.VobAttachPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB attachment).";
        return false;
    }

    // Bindless-diffuse instanced-VOB color PSO (ExecuteIndirect, P2.12): identical to VobPSO (VSMain + the
    // wind-carrying `layout` + lit reversed-Z state) except the PS is PSMainBindless (diffuse sampled from the
    // SRV heap by b6.MatDiffuseIndex instead of the t0 table). Lets the whole instanced-VOB color pass submit as
    // one ExecuteIndirect. Restore pso.VS/PS/layout to the plain VOB set first (the attach block moved pso.VS).
    if ( !m_Shaders->CompileFromFile( "Vob.hlsl", "PSMainBindless", Shadermodel_PS, World.VobIndirectPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    pso.VS = { World.VobVsBlob->GetBufferPointer(), World.VobVsBlob->GetBufferSize() };
    pso.PS = { World.VobIndirectPsBlob->GetBufferPointer(), World.VobIndirectPsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( World.VobIndirectPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB, indirect).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateUI() {
    ID3D12Device* device = m_Device->GetDevice();

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
        IID_PPV_ARGS( UI.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    // --- Compile shaders ---
    if ( !m_Shaders->CompileFromFile( "UI.hlsl", "VSMain", Shadermodel_VS, UI.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "UI.hlsl", "PSMain", Shadermodel_PS, UI.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    // Sky pass variant (D3D11 VS_TransformedEx_MAX_Z equivalent) — same VS, z pinned to the reversed-Z far plane.
    const D3D_SHADER_MACRO maxZDefines[] = { { "FORCE_MAX_Z", "1" }, { nullptr, nullptr } };
    if ( !m_Shaders->CompileFromFile( "UI.hlsl", "VSMain", Shadermodel_VS, UI.VsBlobMaxZ.ReleaseAndGetAddressOf(), maxZDefines ) ) {
        return false;
    }
    // Sky pass PS variant: linearize the sRGB-encoded FF output before it lands in the linear HDR scene
    // target (plain 2D UI writes straight to the swapchain post-tonemap and must NOT linearize).
    const D3D_SHADER_MACRO linearizeDefines[] = { { "LINEARIZE_OUTPUT", "1" }, { nullptr, nullptr } };
    if ( !m_Shaders->CompileFromFile( "UI.hlsl", "PSMain", Shadermodel_PS, UI.PsBlobHdr.ReleaseAndGetAddressOf(), linearizeDefines ) ) {
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
    return true;
}

ID3D12PipelineState* D3D12PipelineState::GetOrCreateUIPipeline(
    const GothicBlendStateInfo& blend,
    const GothicDepthBufferStateInfo& depth,
    D3D12_CULL_MODE cullMode, bool rtvIsHdr, bool forceMaxZ, bool frontCCW ) {
    const uint64_t key = static_cast<uint64_t>(BlendKey( blend )) | (static_cast<uint64_t>(DepthKey( depth )) << 32)
        | (static_cast<uint64_t>(cullMode) << 34) | (static_cast<uint64_t>(rtvIsHdr) << 36) | (static_cast<uint64_t>(forceMaxZ) << 37)
        | (static_cast<uint64_t>(frontCCW) << 38);
    auto it = UI.Pipelines.find( key );
    if ( it != UI.Pipelines.end() ) return it->second.Get();

    // --- Input layout: mirrors layout1 (the ExVertexStruct HUD layout; tangent treated as padding) ---
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "DIFFUSE",  0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // --- PSO: blend emulates Gothic's per-draw state; RTV/cull/VS vary by caller (plain 2D vs sky pass) ---
    ID3DBlob* vsBlob = forceMaxZ ? UI.VsBlobMaxZ.Get() : UI.VsBlob.Get();
    // The HDR scene-color target holds linear values; the plain-2D swapchain target is already tonemapped/
    // sRGB. Only the former needs the FF output's sRGB encoding undone (see UI.hlsl's LINEARIZE_OUTPUT).
    ID3DBlob* psBlob = rtvIsHdr ? UI.PsBlobHdr.Get() : UI.PsBlob.Get();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = UI.RootSig.Get();
    pso.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    pso.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    // Plain 2D UI draws straight to the swapchain (after the tonemap resolve); the sky pass (rtvIsHdr) runs
    // during OnStartWorldRendering with the R16F HDR scene-color target bound.
    pso.RTVFormats[0] = rtvIsHdr ? kSceneColorFormat : kBackBufferFormat;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = cullMode;
    pso.RasterizerState.FrontCounterClockwise = frontCCW ? TRUE : FALSE;
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
        pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    }
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    ComPtr<ID3D12PipelineState> state;
    if ( FAILED( m_Device->GetDevice()->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( state.GetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed for UI pipeline key 0x" << std::hex << key << ".";
        return nullptr;
    }
    ID3D12PipelineState* raw = state.Get();
    UI.Pipelines.emplace( key, std::move( state ) );
    return raw;
}

bool D3D12PipelineState::CreateParticle() {
    ID3D12Device* device = m_Device->GetDevice();

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
        IID_PPV_ARGS( Particle.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "Particle.hlsl", "VSMain", Shadermodel_VS, Particle.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Particle.hlsl", "PSMain", Shadermodel_PS, Particle.PsBlob.ReleaseAndGetAddressOf() ) ) {
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
    return true;
}

ID3D12PipelineState* D3D12PipelineState::GetOrCreateParticlePipeline( const GothicBlendStateInfo& blend ) {
    const uint32_t key = BlendKey( blend );
    auto it = Particle.Pipelines.find( key );
    if ( it != Particle.Pipelines.end() ) return it->second.Get();

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
    pso.pRootSignature = Particle.RootSig.Get();
    pso.VS = { Particle.VsBlob->GetBufferPointer(), Particle.VsBlob->GetBufferSize() };
    pso.PS = { Particle.PsBlob->GetBufferPointer(), Particle.PsBlob->GetBufferSize() };
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
    if ( FAILED( m_Device->GetDevice()->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( state.GetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed for particle blend key 0x" << std::hex << key << ".";
        return nullptr;
    }
    ID3D12PipelineState* raw = state.Get();
    Particle.Pipelines.emplace( key, std::move( state ) );
    return raw;
}

bool D3D12PipelineState::CreateDecal() {
    ID3D12Device* device = m_Device->GetDevice();

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
        IID_PPV_ARGS( Decal.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "Decal.hlsl", "VSMain", Shadermodel_VS, Decal.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Decal.hlsl", "PSMainLit", Shadermodel_PS, Decal.LitPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Decal.hlsl", "PSMainBlend", Shadermodel_PS, Decal.BlendPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    // Lit / opaque PSO: alpha-test cutout, depth test GREATER_EQUAL + WRITE (draws with the opaque scene).
    // The shared unit-quad VB + instance ring buffers are created by the engine (GPU resources).
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = Decal.RootSig.Get();
    pso.VS = { Decal.VsBlob->GetBufferPointer(), Decal.VsBlob->GetBufferSize() };
    pso.PS = { Decal.LitPsBlob->GetBufferPointer(), Decal.LitPsBlob->GetBufferSize() };
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
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Decal.LitPSO.ReleaseAndGetAddressOf() ) ) ) ) {
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
    return true;
}

ID3D12PipelineState* D3D12PipelineState::GetOrCreateDecalBlendPipeline( const GothicBlendStateInfo& blend ) {
    const uint32_t key = BlendKey( blend );
    auto it = Decal.BlendPipelines.find( key );
    if ( it != Decal.BlendPipelines.end() ) return it->second.Get();

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = Decal.RootSig.Get();
    pso.VS = { Decal.VsBlob->GetBufferPointer(), Decal.VsBlob->GetBufferSize() };
    pso.PS = { Decal.BlendPsBlob->GetBufferPointer(), Decal.BlendPsBlob->GetBufferSize() };
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
    if ( FAILED( m_Device->GetDevice()->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( state.GetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed for decal blend key 0x" << std::hex << key << ".";
        return nullptr;
    }
    ID3D12PipelineState* raw = state.Get();
    Decal.BlendPipelines.emplace( key, std::move( state ) );
    return raw;
}

bool D3D12PipelineState::CreateSkeletal() {
    ID3D12Device* device = m_Device->GetDevice();

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

    D3D12_ROOT_PARAMETER params[14] = {};
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
    // Forward+ point lights (mirrors World.RootSig params 3/4/5/6, here at 5..8 — see BindFrameLights). All
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
    // params[13] = simple-SSAO mask bindless SRV-heap index (b8 AOCB — b7 is GhostCB, used only by the separate
    // GhostSkeletal root sig/PSO, not this one). Set once per frame by DrawSkeletalColor before the base-mesh
    // draws; Skeletal.hlsl's PSMain reads it via ResourceDescriptorHeap[AoMaskIndex].
    params[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[13].Constants.ShaderRegister = 8;   // b8 AOCB { AoMaskIndex }
    params[13].Constants.Num32BitValues = 1;
    params[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[3] = {};
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
    // s1: point-clamp for the simple-SSAO mask fetch — see World.RootSig's identical s1 for why Load() (raw
    // texel coords) is wrong for the 1x1 "AO disabled" fallback and CLAMP-addressed Sample must be used instead.
    samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[2].ShaderRegister = 1;           // s1 AO mask point-clamp
    samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
        IID_PPV_ARGS( Skeletal.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "Skeletal.hlsl", "VSMain", Shadermodel_VS, Skeletal.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Skeletal.hlsl", "PSMain", Shadermodel_PS, Skeletal.PsBlob.ReleaseAndGetAddressOf() ) ) {
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
    pso.pRootSignature = Skeletal.RootSig.Get();
    pso.VS = { Skeletal.VsBlob->GetBufferPointer(), Skeletal.VsBlob->GetBufferSize() };
    pso.PS = { Skeletal.PsBlob->GetBufferPointer(), Skeletal.PsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;   // matches D3D11's CM_CULL_BACK default for skinned meshes
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed-Z
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Skeletal.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (skeletal).";
        return false;
    }

    // Skeletal depth-prepass PSO (P2.9b-4b): same root sig + skinned input layout + depth state, but VSDepth/
    // PSDepthClip and color writes masked off. Lays down NPC/monster depth so the light cull bounds tiles to
    // them (fixing the near-skeletal cutoff). Same layout as the color PSO (VSDepth reads the same VS_IN).
    if ( !m_Shaders->CompileFromFile( "Skeletal.hlsl", "VSDepth", Shadermodel_VS, Skeletal.DepthPrepassVsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Skeletal.hlsl", "PSDepthClip", Shadermodel_PS, Skeletal.DepthPrepassPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    pso.VS = { Skeletal.DepthPrepassVsBlob->GetBufferPointer(), Skeletal.DepthPrepassVsBlob->GetBufferSize() };
    pso.PS = { Skeletal.DepthPrepassPsBlob->GetBufferPointer(), Skeletal.DepthPrepassPsBlob->GetBufferSize() };
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;   // DEPTH ONLY — discard color
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Skeletal.DepthPrepassPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (skeletal depth prepass).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreatePointShadow() {
    // P2.10a: the point-light shadow cube caster PIPELINES only (the cube array textures, per-slot DSVs, array
    // SRV, and per-frame CB/instance rings are GPU resources built in the engine's CreatePointShadowResources).
    // Two root sigs (world/VOB share one; skeletal has its own with the 6-face CBV at b0), four VS/PS blobs, and
    // three single-pass-6-face caster PSOs. Non-fatal at init: on failure the point lights simply stay unshadowed.
    ID3D12Device* device = m_Device->GetDevice();
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
        IID_PPV_ARGS( PointShadow.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    // --- Caster shader: single-pass 6-face via instancing. instanceID (0..5) picks the face view-proj AND is
    // written to SV_RenderTargetArrayIndex to route the primitive to that cube face slice (no geometry shader —
    // needs VS-stage RT-array-index support, present on the target AMD GPU). World verts are already world-space.
    if ( !m_Shaders->CompileFromFile( "PointShadow.hlsl", "VSCube", Shadermodel_VS, PointShadow.VsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "PointShadow.hlsl", "VSCubeVob", Shadermodel_VS, PointShadow.VobVsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "PointShadow.hlsl", "VSCubeSkel", Shadermodel_VS, PointShadow.SkelVsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "PointShadow.hlsl", "PSCubeClip", Shadermodel_PS, PointShadow.PsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    // Caster PSO. Single stream: Position + TexCoord0 from the packed 36-byte world vertex. Depth-only (no RTV),
    // NORMAL-Z LESS_EQUAL, CULL_NONE (D3D11 renders cubes with cullFront=false; bias is applied at sample time).
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = PointShadow.RootSig.Get();
    pso.VS = { PointShadow.VsBlob->GetBufferPointer(), PointShadow.VsBlob->GetBufferSize() };
    pso.PS = { PointShadow.PsBlob->GetBufferPointer(), PointShadow.PsBlob->GetBufferSize() };
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
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( PointShadow.CasterWorldPSO.ReleaseAndGetAddressOf() ) ) ) ) {
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
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            // InstanceDataStepRate=6: one real instance's matrix is fetched for 6 consecutive instanceIDs (the 6
            // cube faces). Must match VSCubeVob (face = iid % 6) and the count-matrices / count*6-instances draw —
            // step rate 1 here would demand count*6 matrices (slot-1-too-small spam) and fetch matrix[iid] per face.
            { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 6 },
            { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 6 },
            { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 6 },
            { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 6 },
        };
        pso.pRootSignature = PointShadow.RootSig.Get();
        pso.VS = { PointShadow.VobVsBlob->GetBufferPointer(), PointShadow.VobVsBlob->GetBufferSize() };
        pso.PS = { PointShadow.PsBlob->GetBufferPointer(), PointShadow.PsBlob->GetBufferSize() };
        pso.InputLayout = { vobLayout, _countof( vobLayout ) };
        if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( PointShadow.CasterVobPSO.ReleaseAndGetAddressOf() ) ) ) ) {
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
            IID_PPV_ARGS( PointShadow.SkeletalRootSig.ReleaseAndGetAddressOf() ) ) ) )
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
        pso.pRootSignature = PointShadow.SkeletalRootSig.Get();
        pso.VS = { PointShadow.SkelVsBlob->GetBufferPointer(), PointShadow.SkelVsBlob->GetBufferSize() };
        pso.PS = { PointShadow.PsBlob->GetBufferPointer(), PointShadow.PsBlob->GetBufferSize() };
        pso.InputLayout = { skelLayout, _countof( skelLayout ) };
        if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( PointShadow.CasterSkeletalPSO.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: CreateGraphicsPipelineState failed (point-shadow skeletal caster).";
            return false;
        }
    }
    return true;
}

bool D3D12PipelineState::CreateTonemap() {
    // Fullscreen HDR->swapchain resolve (Phase 3). Dynamic exposure (Exposure * MiddleGray / AdaptedLum) *
    // scene HDR -> a user-selectable tonemap curve (RendererSettings.HDRToneMap, same 6 operators as D3D11's
    // ImGui combo — Tonemap.hlsl branches on it at runtime, no PSO variants needed) -> R10G10B10A2. Runs once
    // per world frame after all 3D. No vertex buffer
    // (SV_VertexID fullscreen triangle), no depth. Created once. AdaptedLum (t1) is a root SRV (not a table —
    // it's a raw StructuredBuffer), fed every frame by CS_LumReduce/CS_LumAdapt in RenderLuminanceAdapt(); the
    // PS reads it UNCONDITIONALLY, so m_LumAdaptedBuffer's creation is a fatal Init failure, same as this PSO.
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;   // t0 scene HDR
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;   // b0 { Exposure, LumWhite, ToneMapMode, pad }
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 1;   // t1 AdaptedLum
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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

bool D3D12PipelineState::CreateLumAdapt() {
    // Dynamic exposure: two independent compute pipelines. LumReduce (t0 scene-color descriptor table, u0
    // PartialSums root UAV) writes one {sum,count} per 16x16 group; LumAdapt (t0 PartialSums root SRV, u0
    // AdaptedLum root UAV) finishes the reduction and temporally adapts it. Both buffers are raw
    // StructuredBuffers, so — mirroring CreateLightCull's SB_Lights/RW_LightGrid pattern — they ride root
    // descriptors, not descriptor-table heap slots; only the scene-color Texture2D needs a table (it's not a
    // buffer, so it can't be a root SRV).
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;
    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (lum-adapt)."; return false; }

    // --- LumReduce root sig: b0 4x32-bit consts, t0 SRV table (scene color), u0 UAV root descriptor ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;   // t0 SceneHDR
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0 LumReduceCB
        params[0].Constants.Num32BitValues = 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].Descriptor.ShaderRegister = 0;   // u0 PartialSums
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof( params );
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
            if ( rsErr ) LogWarn() << "D3D12: lum-reduce root sig error: " << static_cast<const char*>(rsErr->GetBufferPointer());
            return false;
        }
        if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_PPV_ARGS( LumReduce.RootSig.ReleaseAndGetAddressOf() ) ) ) )
            return false;
    }
    if ( !m_Shaders->CompileFromFile( "CS_LumReduce.hlsl", "CSMain", Shadermodel_CS, LumReduce.CsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = LumReduce.RootSig.Get();
        pso.CS = { LumReduce.CsBlob->GetBufferPointer(), LumReduce.CsBlob->GetBufferSize() };
        if ( FAILED( device->CreateComputePipelineState( &pso, IID_PPV_ARGS( LumReduce.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: CreateComputePipelineState failed (lum-reduce).";
            return false;
        }
    }

    // --- LumAdapt root sig: b0 4x32-bit consts, t0 SRV root descriptor (PartialSums), u0 UAV root descriptor ---
    {
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0 LumAdaptCB
        params[0].Constants.Num32BitValues = 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0;   // t0 PartialSums
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].Descriptor.ShaderRegister = 0;   // u0 AdaptedLum
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof( params );
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
            if ( rsErr ) LogWarn() << "D3D12: lum-adapt root sig error: " << static_cast<const char*>(rsErr->GetBufferPointer());
            return false;
        }
        if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_PPV_ARGS( LumAdapt.RootSig.ReleaseAndGetAddressOf() ) ) ) )
            return false;
    }
    if ( !m_Shaders->CompileFromFile( "CS_LumAdapt.hlsl", "CSMain", Shadermodel_CS, LumAdapt.CsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = LumAdapt.RootSig.Get();
        pso.CS = { LumAdapt.CsBlob->GetBufferPointer(), LumAdapt.CsBlob->GetBufferSize() };
        if ( FAILED( device->CreateComputePipelineState( &pso, IID_PPV_ARGS( LumAdapt.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: CreateComputePipelineState failed (lum-adapt).";
            return false;
        }
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
    // Back-face culling, matching D3D11: DrawWaterSurfaces calls SetDefaultStates(), and the rasterizer
    // default is CM_CULL_BACK (GothicRasterizerStateInfo::SetDefault) — same cull the opaque world pass uses,
    // and water lives in the same wrapped world mesh with the same winding. This was CULL_NONE, which drew
    // both sides of every water polygon: harmless-looking while water never wrote depth (the extra layer just
    // blended twice), but once the Z-prepass below writes depth the duplicate/back-facing layers fight with
    // the front ones and read as flickering stacked water textures.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
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

    // Water depth-only prepass PSO (mirrors D3D11's DrawWaterSurfaces Z-prepass). Everything about the color
    // PSO above is kept — same root sig, same input layout, and critically the **same VS blob** — so the depth
    // it lays down is bit-identical to what the color pass rasterizes (see Water.hlsl's PSDepth comment: a
    // separate position-only VS is only algebraically equal and ULP disagreements show up as z-fighting on
    // Gothic's coplanar water surfaces). Only the PS (writes nothing), the color write mask and the depth-write
    // mask differ. Cull mode is inherited from the color PSO, so the depth silhouette is exactly the shape that
    // later blends on top.
    if ( !m_Shaders->CompileFromFile( "Water.hlsl", "PSDepth", Shadermodel_PS, Water.DepthPrepassPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    pso.PS = { Water.DepthPrepassPsBlob->GetBufferPointer(), Water.DepthPrepassPsBlob->GetBufferSize() };
    // Keep NumRenderTargets=1/kSceneColorFormat (the scene-color RTV stays bound through this pass, same as
    // the world/VOB depth prepass) but mask off every color write, and drop the blend the color pass needs.
    pso.BlendState.RenderTarget[0] = {};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;   // DEPTH ONLY
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Water.DepthPrepassPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (water depth prepass).";
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

bool D3D12PipelineState::CreateAdvanceRain() {
    // Rain/snow particle advance compute (D3D12 rain parity, step 1). Same shape as CreateLightCull:
    // b0 = AdvanceRainConstantBuffer as 16 root 32-bit values (3+1 + 3+1 + 1+3 + 1+3 floats), t0 = the
    // immutable per-particle StructuredBuffer as a root SRV, u0 = the dynamic {position,velocity}
    // StructuredBuffer as a root UAV — both plain/structured buffers, so neither needs a descriptor-heap
    // slot (only Texture2D-shaped resources need the heap-table path CreateLightCull's t1 depth SRV uses).
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0 AdvanceRainCB
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;  // t0 StaticData
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;  // u0 DynamicData
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof( params );
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;   // compute: no IA input layout

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (advance rain)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: advance-rain root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( AdvanceRain.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "AdvanceRain.hlsl", "CSMain", Shadermodel_CS, AdvanceRain.CsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = AdvanceRain.RootSig.Get();
    pso.CS = { AdvanceRain.CsBlob->GetBufferPointer(), AdvanceRain.CsBlob->GetBufferSize() };
    if ( FAILED( device->CreateComputePipelineState( &pso, IID_PPV_ARGS( AdvanceRain.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateComputePipelineState failed (advance rain).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateRainDraw() {
    // Rain/snow billboard draw. No input-assembler layout at all — the VS reads both particle buffers
    // as StructuredBuffers via root SRV, indexed by SV_InstanceID (see Shaders/D3D12/Rain.hlsl), so the
    // root sig omits ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT (mirrors the compute root sigs' shape, just with
    // VS/PS stages instead of CS). CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED is set because the PS reads the
    // active rain/snow Texture2DArray bindlessly (ResourceDescriptorHeap[TexArrayIndex]) rather than via
    // a descriptor table — the array choice is a per-draw root const, not a shader permutation.
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MaxAnisotropy = 4;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;   // s0 rain/snow texture array
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s1: rain shadowmap comparison sampler — matches D3D11's m_RainDropShadowSamplerState (WRAP, LESS_EQUAL).
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 1;   // s1
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_PARAMETER params[6] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;   // b0 ViewProjCB
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;   // b1 RainInfoCB — read by both VS (billboard construction) and PS (rainResponse eye/light vectors)
    params[1].Constants.Num32BitValues = 10;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 0;  // t0 DynamicData
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 1;  // t1 StaticData
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].Constants.ShaderRegister = 2;   // b2 RainTexCB
    params[4].Constants.Num32BitValues = 2;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[5].Constants.ShaderRegister = 3;   // b3 RainShadowCB — VS-only (IsWet is called from the VS)
    params[5].Constants.Num32BitValues = 17;  // 16 (float4x4) + 1 (heap slot index)
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof( params );
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = _countof( samplers );
    rsDesc.pStaticSamplers = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (rain draw)."; return false; }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: rain-draw root signature serialize error: " << static_cast<const char*>(rsErr->GetBufferPointer());
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( RainDraw.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "Rain.hlsl", "VSMain", Shadermodel_VS, RainDraw.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Rain.hlsl", "PSMain", Shadermodel_PS, RainDraw.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = RainDraw.RootSig.Get();
    pso.VS = { RainDraw.VsBlob->GetBufferPointer(), RainDraw.VsBlob->GetBufferSize() };
    pso.PS = { RainDraw.PsBlob->GetBufferPointer(), RainDraw.PsBlob->GetBufferSize() };
    pso.InputLayout = { nullptr, 0 };   // no IA — VS pulls everything from root SRVs by SV_VertexID/InstanceID
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;   // strips still use the TRIANGLE type
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    // Alpha blend, matching D3D11's rain state block (SetAlphaBlending + DepthWriteEnabled=false).
    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Reversed-Z: test GREATER_EQUAL against the opaque scene depth, no write — rain is transparent and
    // must not occlude, same reasoning as the particle PSOs.
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( RainDraw.PSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (rain draw).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateBloom() {
    // Bloom pyramid (P2.11): mirrors D3D11PFX_Bloom's compute pipeline exactly (same HLSL math, ported
    // verbatim into Shaders/D3D12/CS_Bloom_*.hlsl since D3D12ShaderBackend only looks under shaders/D3D12/).
    // Prefilter/downsample share one descriptor-table layout (t0 SRV, u0 UAV); upsample needs a second SRV
    // (t1, the same-size downsampled mip) so it gets its own root sig. Composite is a fullscreen-triangle
    // graphics pass (additive blend, no depth) mirroring Tonemap's structure. Pyramid TEXTURES stay in the
    // engine (resolution-dependent, recreated on resize) — this only builds pipeline state.
    ID3D12Device* device = m_Device->GetDevice();
    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (bloom)."; return false; }

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;   // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // --- Down root sig (prefilter + downsample): b0 8x32-bit consts, t0 SRV table, u0 UAV table ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;   // t0
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;   // u0
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0 BloomConstantBuffer
        params[0].Constants.Num32BitValues = 8;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uavRange;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof( params );
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
            if ( rsErr ) LogWarn() << "D3D12: bloom-down root sig error: " << static_cast<const char*>(rsErr->GetBufferPointer());
            return false;
        }
        if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_PPV_ARGS( Bloom.DownRootSig.ReleaseAndGetAddressOf() ) ) ) )
            return false;
    }

    // --- Up root sig (upsample): b0 8x32-bit consts, t0+t1 SRV table (2 contiguous descriptors), u0 UAV table ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 2;
        srvRange.BaseShaderRegister = 0;   // t0, t1
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;   // u0
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 8;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uavRange;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof( params );
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
            if ( rsErr ) LogWarn() << "D3D12: bloom-up root sig error: " << static_cast<const char*>(rsErr->GetBufferPointer());
            return false;
        }
        if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_PPV_ARGS( Bloom.UpRootSig.ReleaseAndGetAddressOf() ) ) ) )
            return false;
    }

    const D3D_SHADER_MACRO prefilterMacro[] = { { "BLOOM_PREFILTER", "1" }, { nullptr, nullptr } };
    if ( !m_Shaders->CompileFromFile( "CS_Bloom_Downsample.hlsl", "CSMain", Shadermodel_CS, Bloom.PrefilterCsBlob.ReleaseAndGetAddressOf(), prefilterMacro ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "CS_Bloom_Downsample.hlsl", "CSMain", Shadermodel_CS, Bloom.DownsampleCsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "CS_Bloom_Upsample.hlsl", "CSMain", Shadermodel_CS, Bloom.UpsampleCsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    auto makeComputePSO = [&]( ID3D12RootSignature* rootSig, ID3DBlob* cs, ComPtr<ID3D12PipelineState>& out, const char* name ) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = rootSig;
        pso.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        if ( FAILED( device->CreateComputePipelineState( &pso, IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: CreateComputePipelineState failed (bloom " << name << ").";
            return false;
        }
        return true;
    };
    if ( !makeComputePSO( Bloom.DownRootSig.Get(), Bloom.PrefilterCsBlob.Get(), Bloom.PrefilterPSO, "prefilter" ) ) return false;
    if ( !makeComputePSO( Bloom.DownRootSig.Get(), Bloom.DownsampleCsBlob.Get(), Bloom.DownsamplePSO, "downsample" ) ) return false;
    if ( !makeComputePSO( Bloom.UpRootSig.Get(), Bloom.UpsampleCsBlob.Get(), Bloom.UpsamplePSO, "upsample" ) ) return false;

    // --- Composite (graphics): fullscreen triangle, additive blend, writes into the HDR scene-color target ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;   // t0
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &srvRange;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;   // b0 { Intensity }
        params[1].Constants.Num32BitValues = 1;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof( params );
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
            if ( rsErr ) LogWarn() << "D3D12: bloom-composite root sig error: " << static_cast<const char*>(rsErr->GetBufferPointer());
            return false;
        }
        if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_PPV_ARGS( Bloom.CompositeRootSig.ReleaseAndGetAddressOf() ) ) ) )
            return false;

        if ( !m_Shaders->CompileFromFile( "Bloom_Composite.hlsl", "VSFullscreen", Shadermodel_VS, Bloom.CompositeVsBlob.ReleaseAndGetAddressOf() ) )
            return false;
        if ( !m_Shaders->CompileFromFile( "Bloom_Composite.hlsl", "PSComposite", Shadermodel_PS, Bloom.CompositePsBlob.ReleaseAndGetAddressOf() ) )
            return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = Bloom.CompositeRootSig.Get();
        pso.VS = { Bloom.CompositeVsBlob->GetBufferPointer(), Bloom.CompositeVsBlob->GetBufferSize() };
        pso.PS = { Bloom.CompositePsBlob->GetBufferPointer(), Bloom.CompositePsBlob->GetBufferSize() };
        pso.InputLayout = { nullptr, 0 };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kSceneColorFormat;   // composites additively onto the HDR scene, before tonemap
        pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc.Count = 1;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        // Additive: dst + src*1 (Intensity already baked into the PS output), no destination read needed.
        pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Bloom.CompositePSO.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: CreateGraphicsPipelineState failed (bloom composite).";
            return false;
        }
    }
    return true;
}


bool D3D12PipelineState::CreateSmaa() {
    // SMAA (Subpixel Morphological Anti-Aliasing) — runtime toggle (RendererSettings.AntiAliasingMode ==
    // AA_SMAA). Mirrors the D3D11 3-pass structure (D3D11SMAA::Render): edge detection, blend-weight
    // calculation, neighborhood blending. All three passes share one bindless root signature; the pixel
    // shaders fetch color/edges/blend/area/search from the shader-visible heap by index (root consts), so no
    // per-pass descriptor tables are needed. Non-fatal on failure — RenderSMAA() guards on the PSOs existing.
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;

    // b0: 9 root constants { float4 RT_METRICS; uint Color/Edges/Blend/Area/Search index }.
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;   // b0 cbSMAA
    param.Constants.Num32BitValues = 9;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;   // RT_METRICS is read by both the VS and PS

    // s0 linear-clamp, s1 point-clamp (matches D3D11SMAA::Init's two sampler states).
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;   // s0 LinearSampler
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 1;   // s1 PointSampler
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &param;
    rsDesc.NumStaticSamplers = _countof( samplers );
    rsDesc.pStaticSamplers = samplers;
    // DIRECTLY_INDEXED enables SM6.6 ResourceDescriptorHeap[...] bindless fetch of the SMAA textures.
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                 | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (SMAA)."; return false; }
    ComPtr<ID3DBlob> rsBlob, rsErr;
    if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
        if ( rsErr ) LogWarn() << "D3D12: SMAA root sig error: " << static_cast<const char*>( rsErr->GetBufferPointer() );
        return false;
    }
    if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS( Smaa.RootSig.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( !m_Shaders->CompileFromFile( "SMAA.hlsl", "EdgeDetectionVS", Shadermodel_VS, Smaa.EdgeVsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "SMAA.hlsl", "LumaEdgeDetectionPS", Shadermodel_PS, Smaa.EdgePsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "SMAA.hlsl", "BlendingWeightCalculationVS", Shadermodel_VS, Smaa.BlendVsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "SMAA.hlsl", "BlendingWeightCalculationPS", Shadermodel_PS, Smaa.BlendPsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "SMAA.hlsl", "NeighborhoodBlendingVS", Shadermodel_VS, Smaa.NeighborVsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "SMAA.hlsl", "NeighborhoodBlendingPS", Shadermodel_PS, Smaa.NeighborPsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    // Shared PSO skeleton: fullscreen triangle (no VB / input layout), no depth, no blend (each pass fully
    // overwrites its target). Only VS/PS and the RTV format differ per pass.
    auto makePso = [&]( ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv, ID3D12PipelineState** out, const char* name ) -> bool {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = Smaa.RootSig.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.InputLayout = { nullptr, 0 };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = rtv;
        pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc.Count = 1;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( out ) ) ) ) {
            LogWarn() << "D3D12: CreateGraphicsPipelineState failed (SMAA " << name << ").";
            return false;
        }
        return true;
        };

    // Edge + blend passes render to the R8G8B8A8 intermediates; the final neighborhood pass writes the swapchain.
    if ( !makePso( Smaa.EdgeVsBlob.Get(), Smaa.EdgePsBlob.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, Smaa.EdgePSO.ReleaseAndGetAddressOf(), "edge" ) )
        return false;
    if ( !makePso( Smaa.BlendVsBlob.Get(), Smaa.BlendPsBlob.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, Smaa.BlendPSO.ReleaseAndGetAddressOf(), "blend" ) )
        return false;
    if ( !makePso( Smaa.NeighborVsBlob.Get(), Smaa.NeighborPsBlob.Get(), kBackBufferFormat, Smaa.NeighborPSO.ReleaseAndGetAddressOf(), "neighbor" ) )
        return false;

    return true;
}

bool D3D12PipelineState::CreateAO() {
    // Simple screen-space AO (plan item #4, "SAO"). Two compute root sigs sharing one shape (b0 32-bit
    // consts, one SRV descriptor table, one UAV descriptor table): the main pass (Shaders/D3D12/SSAO.hlsl
    // CSMain) reads only the depth SRV; the blur pass (CSBlur, run horizontal then vertical) additionally
    // reads the AO estimate, so its SRV table is 2-wide. Non-fatal — RenderSSAO() guards on both PSOs.
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;

    // s0: point-clamp — depth/AO are sampled at the source pixel grid; no filtering wanted across depth
    // discontinuities (bilinear would blend unrelated surfaces' AO/depth together at edges).
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;   // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (AO)."; return false; }

    // --- Main pass root sig: b0 12x32-bit SSAOCB, t0 depth SRV table, u0 AO-output UAV table ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;   // t0 DepthTex
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;   // u0 OutputAO
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0 SSAOCB
        params[0].Constants.Num32BitValues = 12;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uavRange;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof( params );
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
            if ( rsErr ) LogWarn() << "D3D12: AO main root sig error: " << static_cast<const char*>(rsErr->GetBufferPointer());
            return false;
        }
        if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_PPV_ARGS( AO.MainRootSig.ReleaseAndGetAddressOf() ) ) ) )
            return false;
    }

    // --- Blur pass root sig: b0 8x32-bit BlurCB, t0+t1 SRV table (AO input + depth), u0 UAV table ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 2;
        srvRange.BaseShaderRegister = 0;   // t0 BlurAOTex, t1 BlurDepthTex
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_DESCRIPTOR_RANGE uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;   // u0 OutputAO
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0 BlurCB
        params[0].Constants.Num32BitValues = 8;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uavRange;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof( params );
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        if ( FAILED( serialize( &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
            if ( rsErr ) LogWarn() << "D3D12: AO blur root sig error: " << static_cast<const char*>(rsErr->GetBufferPointer());
            return false;
        }
        if ( FAILED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_PPV_ARGS( AO.BlurRootSig.ReleaseAndGetAddressOf() ) ) ) )
            return false;
    }

    if ( !m_Shaders->CompileFromFile( "SSAO.hlsl", "CSMain", Shadermodel_CS, AO.MainCsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "SSAO.hlsl", "CSBlur", Shadermodel_CS, AO.BlurCsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC mainPso = {};
    mainPso.pRootSignature = AO.MainRootSig.Get();
    mainPso.CS = { AO.MainCsBlob->GetBufferPointer(), AO.MainCsBlob->GetBufferSize() };
    if ( FAILED( device->CreateComputePipelineState( &mainPso, IID_PPV_ARGS( AO.MainPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateComputePipelineState failed (AO main).";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC blurPso = {};
    blurPso.pRootSignature = AO.BlurRootSig.Get();
    blurPso.CS = { AO.BlurCsBlob->GetBufferPointer(), AO.BlurCsBlob->GetBufferSize() };
    if ( FAILED( device->CreateComputePipelineState( &blurPso, IID_PPV_ARGS( AO.BlurPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateComputePipelineState failed (AO blur).";
        return false;
    }
    return true;
}

bool D3D12PipelineState::CreateFog() {
    // Height fog + god rays (plan item #5) — mirrors D3D11's PostFX composition (D3D11PfxRenderer::
    // RenderPostFXComposition + D3D11PFX_GodRays::RenderToTextureCS). Non-fatal on failure:
    // RenderFogAndGodRays() guards on every PSO below.
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (Fog)."; return false; }

    auto makeRootSig = [&]( const D3D12_ROOT_SIGNATURE_DESC& desc, ID3D12RootSignature** out, const char* name ) -> bool {
        ComPtr<ID3DBlob> rsBlob, rsErr;
        if ( FAILED( serialize( &desc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
            if ( rsErr ) LogWarn() << "D3D12: Fog " << name << " root sig error: " << static_cast<const char*>(rsErr->GetBufferPointer());
            return false;
        }
        return SUCCEEDED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS( out ) ) );
        };

    // --- God-ray compute root sig (shared by CSMask + CSZoom) ---
    // b0: 12 root constants. CSMask reads the first 4 (3 heap indices + pad), CSZoom all 12 (blur params +
    // 2 heap indices) — two cbuffers declared at b0 in the same file, only the one an entry point references
    // survives into that PSO (same pattern as SSAO.hlsl's SSAOCB/BlurCB). Everything else is bindless.
    {
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;   // s0 SS_LinearClamp
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = 0;   // b0
        param.Constants.Num32BitValues = 12;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = 1;
        rsDesc.pParameters = &param;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;   // SM6.6 ResourceDescriptorHeap

        if ( !makeRootSig( rsDesc, Fog.GodRayRootSig.ReleaseAndGetAddressOf(), "god-ray" ) ) return false;
    }

    // --- Composition graphics root sig: b0 height-fog CBV, b1 atmosphere CBV, b2 4 root consts ---
    {
        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;   // s0: god-ray upscale (quarter -> full res)
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[0].ShaderRegister = 0;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;          // s1: depth (1:1, never filtered)
        samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[1].ShaderRegister = 1;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;   // b0 PFXBuffer (HeightfogConstantBuffer)
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 1;   // b1 Atmosphere (AtmosphereConstantBuffer)
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants.ShaderRegister = 2;   // b2 FogCompositeCB { depthIdx, godRaysIdx, flags, pad }
        params[2].Constants.Num32BitValues = 4;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof( params );
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = _countof( samplers );
        rsDesc.pStaticSamplers = samplers;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

        if ( !makeRootSig( rsDesc, Fog.CompositeRootSig.ReleaseAndGetAddressOf(), "composition" ) ) return false;
    }

    if ( !m_Shaders->CompileFromFile( "GodRays.hlsl", "CSMask", Shadermodel_CS, Fog.MaskCsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "GodRays.hlsl", "CSZoom", Shadermodel_CS, Fog.ZoomCsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "HeightFog.hlsl", "VSFullscreen", Shadermodel_VS, Fog.CompositeVsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "HeightFog.hlsl", "PSComposite", Shadermodel_PS, Fog.CompositePsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC maskPso = {};
    maskPso.pRootSignature = Fog.GodRayRootSig.Get();
    maskPso.CS = { Fog.MaskCsBlob->GetBufferPointer(), Fog.MaskCsBlob->GetBufferSize() };
    if ( FAILED( device->CreateComputePipelineState( &maskPso, IID_PPV_ARGS( Fog.MaskPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateComputePipelineState failed (god-ray mask).";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC zoomPso = {};
    zoomPso.pRootSignature = Fog.GodRayRootSig.Get();
    zoomPso.CS = { Fog.ZoomCsBlob->GetBufferPointer(), Fog.ZoomCsBlob->GetBufferSize() };
    if ( FAILED( device->CreateComputePipelineState( &zoomPso, IID_PPV_ARGS( Fog.ZoomPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateComputePipelineState failed (god-ray zoom).";
        return false;
    }

    // Composition: fullscreen triangle (no IA), no depth, PREMULTIPLIED-alpha blend onto the HDR scene color.
    // src = (fogColor*fogAlpha + godRays, fogAlpha), so ONE / INV_SRC_ALPHA reproduces D3D11's
    // `lerp(dst, fog.rgb, fog.a) + godrays` without ever reading the destination. Alpha is masked out of the
    // write so the scene target's alpha channel stays exactly as the geometry passes left it.
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = Fog.CompositeRootSig.Get();
        pso.VS = { Fog.CompositeVsBlob->GetBufferPointer(), Fog.CompositeVsBlob->GetBufferSize() };
        pso.PS = { Fog.CompositePsBlob->GetBufferPointer(), Fog.CompositePsBlob->GetBufferSize() };
        pso.InputLayout = { nullptr, 0 };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kSceneColorFormat;
        pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc.Count = 1;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        auto& rt = pso.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Fog.CompositePSO.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: CreateGraphicsPipelineState failed (fog composition).";
            return false;
        }
    }

    return true;
}


bool D3D12PipelineState::CreateCull() {
    // GPU-driven VOB culling (Shaders/D3D12/HiZ.hlsl + Shaders/D3D12/VobCull.hlsl). Non-fatal on failure:
    // BuildHiZ()/CullVobsGPU() guard on every PSO below and the engine falls back to the CPU frustum cull
    // (RendererSettings.GpuVobCulling is evaluated together with these — see EvaluateGpuVobCulling()).
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;

    PFN_SERIALIZE_ROOT_SIG serialize = LoadSerializeRootSignature();
    if ( !serialize ) { LogWarn() << "D3D12: D3D12SerializeRootSignature unavailable (cull)."; return false; }

    auto makeRootSig = [&]( const D3D12_ROOT_SIGNATURE_DESC& desc, ID3D12RootSignature** out, const char* name ) -> bool {
        ComPtr<ID3DBlob> rsBlob, rsErr;
        if ( FAILED( serialize( &desc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf() ) ) ) {
            if ( rsErr ) LogWarn() << "D3D12: cull " << name << " root sig error: " << static_cast<const char*>(rsErr->GetBufferPointer());
            return false;
        }
        return SUCCEEDED( device->CreateRootSignature( 0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS( out ) ) );
        };

    auto makeComputePSO = [&]( const char* file, const char* entry, ID3D12RootSignature* rs,
        ID3DBlob** blob, ID3D12PipelineState** pso ) -> bool {
        if ( !m_Shaders->CompileFromFile( file, entry, Shadermodel_CS, blob ) ) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = rs;
        desc.CS = { (*blob)->GetBufferPointer(), (*blob)->GetBufferSize() };
        if ( FAILED( device->CreateComputePipelineState( &desc, IID_PPV_ARGS( pso ) ) ) ) {
            LogWarn() << "D3D12: CreateComputePipelineState failed (" << entry << ").";
            return false;
        }
        return true;
        };

    // --- Hi-Z build root sig: b0 4 root consts (src/dst heap indices), fully bindless, no tables ---
    {
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = 0;   // b0 HiZCopyCB / HiZReduceCB
        param.Constants.Num32BitValues = 4;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = 1;
        rsDesc.pParameters = &param;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;   // SM6.6 ResourceDescriptorHeap

        if ( !makeRootSig( rsDesc, Cull.HiZRootSig.ReleaseAndGetAddressOf(), "Hi-Z" ) ) return false;
    }
    if ( !makeComputePSO( "HiZ.hlsl", "CSCopyDepth", Cull.HiZRootSig.Get(),
        Cull.HiZCopyCsBlob.ReleaseAndGetAddressOf(), Cull.HiZCopyPSO.ReleaseAndGetAddressOf() ) ) return false;
    if ( !makeComputePSO( "HiZ.hlsl", "CSReduce", Cull.HiZRootSig.Get(),
        Cull.HiZReduceCsBlob.ReleaseAndGetAddressOf(), Cull.HiZReducePSO.ReleaseAndGetAddressOf() ) ) return false;

    // --- VOB cull root sig: b0 24 consts (ViewProj + Hi-Z params), t0/t1 root SRVs, u0/u1 root UAVs ---
    // The visual records + instance streams are plain structured buffers, so they ride as root descriptors
    // (no heap slots). Only the Hi-Z pyramid is a texture and it comes in bindlessly by heap index.
    {
        D3D12_ROOT_PARAMETER params[5] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0 VobCullCB
        params[0].Constants.Num32BitValues = 24;  // float4x4 ViewProj + 8 uints
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0;  // t0 Visuals
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor.ShaderRegister = 1;  // t1 InInstances
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[3].Descriptor.ShaderRegister = 0;  // u0 OutInstances (compacted)
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[4].Descriptor.ShaderRegister = 1;  // u1 VisibleCounts
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof( params );
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

        if ( !makeRootSig( rsDesc, Cull.VobCullRootSig.ReleaseAndGetAddressOf(), "VOB cull" ) ) return false;
    }
    if ( !makeComputePSO( "VobCull.hlsl", "CSCull", Cull.VobCullRootSig.Get(),
        Cull.VobCullCsBlob.ReleaseAndGetAddressOf(), Cull.VobCullPSO.ReleaseAndGetAddressOf() ) ) return false;

    // --- Indirect-arg patch root sig: b0 4 consts (count/stride/offsets), t0 counts SRV, u0 raw arg UAV ---
    {
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0 VobPatchCB
        params[0].Constants.Num32BitValues = 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0;  // t0 PatchCounts
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].Descriptor.ShaderRegister = 0;  // u0 PatchArgs (RWByteAddressBuffer)
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof( params );
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        if ( !makeRootSig( rsDesc, Cull.PatchRootSig.ReleaseAndGetAddressOf(), "arg patch" ) ) return false;
    }
    if ( !makeComputePSO( "VobCull.hlsl", "CSPatchArgs", Cull.PatchRootSig.Get(),
        Cull.PatchCsBlob.ReleaseAndGetAddressOf(), Cull.PatchPSO.ReleaseAndGetAddressOf() ) ) return false;

    return true;
}
