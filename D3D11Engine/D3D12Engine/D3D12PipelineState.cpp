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

D3D12RootLayout& D3D12PipelineState::Layout( const char* name ) {
    D3D12RootLayout& layout = m_Layouts[name];
    layout.Reset( name );
    return layout;
}

D3D12RootLayout* D3D12PipelineState::GetLayout( const char* name ) {
    auto it = m_Layouts.find( name );
    return ( it == m_Layouts.end() ) ? nullptr : &it->second;
}

bool D3D12PipelineState::CreateWorld() {
    ID3D12Device* device = m_Device->GetDevice();

    // Root signature: b0 = ViewProj (16 root 32-bit constants, VS); t0 = diffuse SRV table (PS);
    // b1 = fog (8 root 32-bit constants, VS reads CamPosWS, PS reads color/near/far); static sampler s0.
    D3D12RootLayout& rs = Layout( "World" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );                 // 0: b0 ViewProj (float4x4)
    rs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 1: t0 diffuse
    rs.AddConstants( 1, 8, D3D12_SHADER_VISIBILITY_ALL );                     // 2: b1 fog — VS: CamPosWS; PS: color/near/far

    // 3 = point-light StructuredBuffer as a ROOT SRV at t1 (no descriptor slot consumed — a GPU VA
    // straight in the root; aligns with the CPU-offload/bindless direction). 4 = b2 { light count,
    // NumTilesX }. 5/6 = the Forward+ per-tile grid + index-list root SRVs at t2/t3. All four MUST
    // be bound (BindFrameLights) by every draw using this root sig with a light-reading PSO (World.PSO/
    // World.VobPSO), else the count/grid are undefined root values and the shader loops away.
    rs.AddSRV( 1, D3D12_SHADER_VISIBILITY_PIXEL );        // 3: t1 light StructuredBuffer
    rs.AddConstants( 2, 4, D3D12_SHADER_VISIBILITY_PIXEL );  // 4: b2 { LightCount, NumTilesX, pad, pad }
    rs.AddSRV( 2, D3D12_SHADER_VISIBILITY_PIXEL );        // 5: t2 per-tile LightGrid {Offset,Count}
    rs.AddSRV( 3, D3D12_SHADER_VISIBILITY_PIXEL );        // 6: t3 per-tile light-index list

    // 7 = shadow-sampling CB (b3) as a ROOT CBV (cascade view-projs are too big for root constants).
    // 8 = the CSM shadow-map Texture2DArray SRV (t4) via a one-entry descriptor table off the shared
    // SRV heap. Both are read only by the lit world PS (PSMain); the depth-prepass/caster PSOs sharing
    // this root sig don't reference them, so those draws simply leave the slots unbound.
    // 9 = t5 point-light shadow cube array SRV (P2.10d), sampled by the tiled point-light loop.
    rs.AddCBV( 3, D3D12_SHADER_VISIBILITY_PIXEL );        // 7: b3 shadow CB (cascade view-projs + sun + strength)
    rs.AddTable( D3D12RootLayout::SRVRange( 4 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 8: t4 CSM array
    rs.AddTable( D3D12RootLayout::SRVRange( 5 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 9: t5 point-shadow cube array

    // 10 = per-material bindless indices { normalSrvIndex, ormSrvIndex } as root constants (b6). The PS
    // reads the normal/ORM maps via ResourceDescriptorHeap[...] (SM6.6 bindless) — no per-material descriptor
    // tables. normalIndex == 0xFFFFFFFF means "no normal map" (skip the TBN/perturb); ormIndex is always valid
    // (the default ORM slot when a material has no _FX map), so ORM is sampled branchlessly. The 3rd value is
    // the bindless diffuse index; the 4th the normal-perturb strength (World.hlsl's wet-ground path only —
    // Vob.hlsl/Skeletal.hlsl's own MaterialCB declarations just don't read it).
    rs.AddConstants( 6, 4, D3D12_SHADER_VISIBILITY_PIXEL );  // 10: b6 MaterialCB

    // 11 = wind sway CB (b4, VS only) — read by Vob.hlsl's VSMain (flags/foliage sway + hero-affects-
    // bushes push); World.hlsl/Skeletal.hlsl don't declare b4 so they simply never read it. Only
    // DrawVobsInstanced needs to bind it before its draws; other users of this root sig leave it unbound.
    rs.AddConstants( 4, 12, D3D12_SHADER_VISIBILITY_VERTEX );  // 11: b4 WindCB (VS_ExConstantBuffer_Wind, 48 bytes)

    // 12 = simple-SSAO mask bindless SRV-heap index (b7 AOCB, PS only), set ONCE per frame (not per
    // draw/ExecuteIndirect command) by DrawWorldMesh/DrawVobsInstanced/DrawSkeletalColor's attachment pass —
    // World.hlsl/Vob.hlsl's PSMain(Bindless) read it via ResourceDescriptorHeap[AoMaskIndex]. Points at the
    // white texture's SRV slot when SSAO is disabled/unavailable (mask = no occlusion, matches D3D11's default).
    rs.AddConstants( 7, 1, D3D12_SHADER_VISIBILITY_PIXEL );    // 12: b7 AOCB { AoMaskIndex }

    // s0 diffuse: 16x anisotropic (matches D3D11's main texture sampler) — sharpens surfaces at grazing
    // angles and in the distance, which trilinear alone smears badly.
    rs.AddStaticSampler( D3D12RootLayout::SamplerAniso( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );
    // s2: PCF comparison sampler for the CSM depth. Normal-Z map (LESS_EQUAL): SampleCmp returns 1 where the
    // fragment is closer-or-equal to the light than the stored occluder (lit), 0 where behind it (shadowed).
    // BORDER address + opaque-white border → taps past a cascade's edge read as far (lit), not spurious shadow.
    rs.AddStaticSampler( D3D12RootLayout::SamplerComparison( 2, D3D12_SHADER_VISIBILITY_PIXEL ) );
    // s1: point-clamp for the simple-SSAO mask fetch (World/Vob PSMain). MUST be Sample/SampleLevel with CLAMP
    // addressing, not Load — Load() with out-of-range integer texel coords (the 1x1 white "AO disabled"
    // fallback read at full-res screen coords) returns 0 per the HLSL spec, not the texel's actual value, which
    // was silently zeroing the ambient term (darker with AO "disabled" than with it on — the opposite of what
    // should happen). CLAMP addressing sidesteps this for both the 1x1 fallback and the real full-res mask.
    rs.AddStaticSampler( D3D12RootLayout::SamplerPoint( 1, D3D12_SHADER_VISIBILITY_PIXEL ) );

    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED enables SM6.6 ResourceDescriptorHeap[...] bindless sampling of the
    // per-material normal/ORM maps out of the shared SRV heap (tier-3; present on the target AMD GPU).
    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                          | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )
        return false;
    World.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "World.hlsl", "VSMain", Shadermodel_VS, World.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "World.hlsl", "PSMain", Shadermodel_PS, World.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    // Lit quad marks (D3D12Fx.cpp). Non-fatal: DrawQuadMarks falls back to the unlit Fx pipeline if this
    // blob is missing, so a shader edit that breaks it costs the lighting, not the blood splats.
    if ( !m_Shaders->CompileFromFile( "World.hlsl", "VSQuadMark", Shadermodel_VS, World.QuadMarkVsBlob.ReleaseAndGetAddressOf() ) ) {
        LogWarn() << "D3D12: World.hlsl VSQuadMark failed to compile — quad marks will draw unlit.";
        World.QuadMarkVsBlob.Reset();
    }

    rs.ValidateShaders( {
        { World.VsBlob.Get(),        "World.hlsl:VSMain",     D3D12_SHADER_VISIBILITY_VERTEX },
        { World.PsBlob.Get(),        "World.hlsl:PSMain",     D3D12_SHADER_VISIBILITY_PIXEL  },
        { World.QuadMarkVsBlob.Get(),"World.hlsl:VSQuadMark", D3D12_SHADER_VISIBILITY_VERTEX },
    } );

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

bool D3D12PipelineState::CreateWorldTransparency() {
    // Alpha-blended world-mesh surfaces (ice, glass, magic barriers) — port of D3D11's
    // DrawMeshInfoListAlphablended. These are NOT lit on either backend (D3D11 routes BLEND/ADD materials to
    // PS_Simple_FF), so this pipeline needs none of the Forward+ light/shadow/AO bindings the World root sig
    // carries. It gets its OWN small root sig rather than a 14th World.RootSig parameter, which keeps that
    // signature's root-DWORD budget (56 of 64 used) and its two ExecuteIndirect command signatures untouched.
    //   b0 = ViewProj (16 consts, VS) — same value DrawWorldMesh uploads
    //   b5 = TransparencyCB (8 consts, PS) — the per-material FF tint + the sun height (AC_LightPos.y) the
    //        portal/foam variants darken by; passing that one scalar avoids binding the whole Atmosphere CB
    //   b6 = MaterialCB (4 consts, PS) — reuses World.hlsl's declaration; only MatDiffuseIndex is read
    //   b4 = TransparencyViewCB (16 consts, VS) — world->view, read ONLY by VSTransparentPortal
    // Register numbers match World.hlsl's existing file-scope cbuffers, so the shared VS_IN / includes there
    // need no renumbering for these extra entry points.
    ID3D12Device* device = m_Device->GetDevice();

    D3D12RootLayout& rs = Layout( "WorldTransparency" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );  // 0: b0 ViewProj
    // 1: b5 TransparencyCB { float4 TextureFactor; float SunHeight; float3 pad }
    rs.AddConstants( 5, 8, D3D12_SHADER_VISIBILITY_PIXEL );
    rs.AddConstants( 6, 4, D3D12_SHADER_VISIBILITY_PIXEL );    // 2: b6 MaterialCB { normal, orm, diffuse, normalStrength }
    // 3: b4 TransparencyViewCB { float4x4 View } — portal VS only
    rs.AddConstants( 4, 16, D3D12_SHADER_VISIBILITY_VERTEX );

    // s0 diffuse: same 16x anisotropic wrap sampler the opaque world pass uses.
    rs.AddStaticSampler( D3D12RootLayout::SamplerAniso( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );

    // The diffuse texture is fetched bindlessly (ResourceDescriptorHeap[MatDiffuseIndex]), like the opaque
    // world pass — no per-material descriptor table.
    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                          | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )
        return false;
    WorldTransparency.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "World.hlsl", "VSTransparent", Shadermodel_VS, WorldTransparency.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "World.hlsl", "PSTransparent", Shadermodel_PS, WorldTransparency.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    // MT_WaterfallFoam + MT_Portal variants (D3D11's PS_WaterfallFoam / PS_PortalDiffuse). Non-fatal: the
    // pass falls back to the plain PS_Simple_FF-equivalent for a kind whose blob is missing, which is what
    // those surfaces got before they were peeled out at all.
    if ( !m_Shaders->CompileFromFile( "World.hlsl", "PSTransparentFoam", Shadermodel_PS, WorldTransparency.FoamPsBlob.ReleaseAndGetAddressOf() ) ) {
        LogWarn() << "D3D12: PSTransparentFoam failed to compile — waterfall foam falls back to the plain transparent shader.";
        WorldTransparency.FoamPsBlob.Reset();
    }
    if ( !m_Shaders->CompileFromFile( "World.hlsl", "VSTransparentPortal", Shadermodel_VS, WorldTransparency.PortalVsBlob.ReleaseAndGetAddressOf() )
      || !m_Shaders->CompileFromFile( "World.hlsl", "PSTransparentPortal", Shadermodel_PS, WorldTransparency.PortalPsBlob.ReleaseAndGetAddressOf() ) ) {
        LogWarn() << "D3D12: the portal transparency shaders failed to compile — G1 forest portals will not be drawn.";
        WorldTransparency.PortalVsBlob.Reset();
        WorldTransparency.PortalPsBlob.Reset();
    }

    rs.ValidateShaders( {
        { WorldTransparency.VsBlob.Get(),       "World.hlsl:VSTransparent",       D3D12_SHADER_VISIBILITY_VERTEX },
        { WorldTransparency.PsBlob.Get(),       "World.hlsl:PSTransparent",       D3D12_SHADER_VISIBILITY_PIXEL  },
        { WorldTransparency.FoamPsBlob.Get(),   "World.hlsl:PSTransparentFoam",   D3D12_SHADER_VISIBILITY_PIXEL  },
        { WorldTransparency.PortalVsBlob.Get(), "World.hlsl:VSTransparentPortal", D3D12_SHADER_VISIBILITY_VERTEX },
        { WorldTransparency.PortalPsBlob.Get(), "World.hlsl:PSTransparentPortal", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

    // Warm the two states every transparent world frame needs: plain alpha blending (by far the most common
    // Gothic alpha func on these materials) and the depth-fill re-draw.
    GothicBlendStateInfo alpha;
    alpha.SetAlphaBlending();
    if ( !GetOrCreateWorldTransparencyPipeline( alpha, false ) ) {
        LogWarn() << "D3D12: failed to create the default world-transparency blend pipeline.";
        return false;
    }

    GothicBlendStateInfo depthOnly;
    depthOnly.SetDefault();
    depthOnly.ColorWritesEnabled = false;   // D3D11's second loop: color writes off, depth-write back on
    WorldTransparency.DepthFillPSO = GetOrCreateWorldTransparencyPipeline( depthOnly, true );
    if ( !WorldTransparency.DepthFillPSO ) {
        LogWarn() << "D3D12: failed to create the world-transparency depth-fill pipeline.";
        return false;
    }
    return true;
}

ID3D12PipelineState* D3D12PipelineState::GetOrCreateWorldTransparencyPipeline( const GothicBlendStateInfo& blend, bool depthWrite,
    WorldTransparencyPipeline::EKind kind ) {
    // BlendKey occupies bits 0..28, so the material kind takes 29..30 and the depth-write flag the top bit.
    const uint32_t key = BlendKey( blend ) | (static_cast<uint32_t>(kind) << 29) | (depthWrite ? (1u << 31) : 0u);
    auto it = WorldTransparency.BlendPipelines.find( key );
    if ( it != WorldTransparency.BlendPipelines.end() ) return it->second.Get();
    if ( !WorldTransparency.RootSig || !WorldTransparency.VsBlob || !WorldTransparency.PsBlob ) return nullptr;

    // Per-kind shader selection; a kind whose blobs failed to compile degrades to the plain variant rather
    // than dropping the geometry (the portal caller checks the blobs itself and skips instead).
    ID3DBlob* vs = WorldTransparency.VsBlob.Get();
    ID3DBlob* ps = WorldTransparency.PsBlob.Get();
    if ( kind == WorldTransparencyPipeline::EKind::Foam && WorldTransparency.FoamPsBlob ) {
        ps = WorldTransparency.FoamPsBlob.Get();
    } else if ( kind == WorldTransparencyPipeline::EKind::Portal
        && WorldTransparency.PortalVsBlob && WorldTransparency.PortalPsBlob ) {
        vs = WorldTransparency.PortalVsBlob.Get();
        ps = WorldTransparency.PortalPsBlob.Get();
    }

    // Same packed 36-byte world vertex the opaque pass consumes; VSTransparent just doesn't read NORMAL.
    // (Kept as a superset of what the VS declares so the color and depth-fill PSOs are byte-identical apart
    // from blend/depth state — see the water Z-prepass lesson about ULP-divergent depth.)
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R16G16_SNORM,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "DIFFUSE",  0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = WorldTransparency.RootSig.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // CULL_BACK: D3D11's pass explicitly forces CM_CULL_BACK before DrawMeshInfoListAlphablended.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
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

    // Reversed-Z, tested against the finished opaque scene. Depth-write follows D3D11's state machine:
    // on until the first alpha-func switch turns it off, then on again for the trailing depth-fill loop.
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;

    ComPtr<ID3D12PipelineState> state;
    if ( FAILED( m_Device->GetDevice()->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( state.GetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed for world-transparency key 0x" << std::hex << key << ".";
        return nullptr;
    }
    ID3D12PipelineState* raw = state.Get();
    WorldTransparency.BlendPipelines.emplace( key, std::move( state ) );
    return raw;
}

bool D3D12PipelineState::CreatePreview() {
    // Single-VOB inventory-item preview (GInventory::DrawVobSingle): drawn straight onto the swapchain
    // backbuffer + its depth buffer from Gothic's own UI-phase zCWorld::Render hook, not through the
    // Forward+ scene passes. Mirrors D3D11's VS_Ex + PS_Preview_Textured (RENDERMODE==1: plain textured,
    // alpha-clip, no lighting/fog) — its own minimal root sig, no Forward+ light/shadow bindings needed.
    ID3D12Device* device = m_Device->GetDevice();

    D3D12RootLayout& rs = Layout( "Preview" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );  // 0: b0 ViewProj
    // 1: b1 World (per-instance, single draw — no instance buffer needed)
    rs.AddConstants( 1, 16, D3D12_SHADER_VISIBILITY_VERTEX );
    rs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );   // 2: t0 diffuse
    // s0 diffuse: 16x anisotropic wrap, matches D3D11's DefaultSamplerState used for this draw.
    rs.AddStaticSampler( D3D12RootLayout::SamplerAniso( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ) )
        return false;
    Preview.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Preview.hlsl", "VSMain", Shadermodel_VS, Preview.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Preview.hlsl", "PSMain", Shadermodel_PS, Preview.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    rs.ValidateShaders( {
        { Preview.VsBlob.Get(), "Preview.hlsl:VSMain", D3D12_SHADER_VISIBILITY_VERTEX },
        { Preview.PsBlob.Get(), "Preview.hlsl:PSMain", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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

    D3D12RootLayout& rs = Layout( "Ghost" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );  // 0: b0 ViewProj
    // 1: b1 World (per-instance, single draw — no instance buffer needed)
    rs.AddConstants( 1, 16, D3D12_SHADER_VISIBILITY_VERTEX );
    rs.AddConstants( 2, 1, D3D12_SHADER_VISIBILITY_PIXEL );    // 2: b2 GhostAlpha
    rs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 3: t0 diffuse
    // s0 diffuse: matches Preview's sampler (16x anisotropic wrap).
    rs.AddStaticSampler( D3D12RootLayout::SamplerAniso( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ) )
        return false;
    Ghost.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Preview.hlsl", "VSMain", Shadermodel_VS, Ghost.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Preview.hlsl", "PSGhost", Shadermodel_PS, Ghost.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    rs.ValidateShaders( {
        { Ghost.VsBlob.Get(), "Preview.hlsl:VSMain",  D3D12_SHADER_VISIBILITY_VERTEX },
        { Ghost.PsBlob.Get(), "Preview.hlsl:PSGhost", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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

    D3D12RootLayout& rs = Layout( "GhostSkeletal" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );  // 0: b0 ViewProj
    rs.AddCBV( 1, D3D12_SHADER_VISIBILITY_VERTEX );            // 1: b1 per-instance (World/ModelColor/Fatness)
    rs.AddCBV( 2, D3D12_SHADER_VISIBILITY_VERTEX );            // 2: b2 bone palette
    // 3: b7 GhostAlpha (Skeletal.hlsl's b0..b6 are all spoken for)
    rs.AddConstants( 7, 1, D3D12_SHADER_VISIBILITY_PIXEL );
    rs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 4: t0 diffuse
    rs.AddStaticSampler( D3D12RootLayout::SamplerAniso( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ) )
        return false;
    GhostSkeletal.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Skeletal.hlsl", "VSDepth", Shadermodel_VS, GhostSkeletal.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Skeletal.hlsl", "PSGhost", Shadermodel_PS, GhostSkeletal.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    rs.ValidateShaders( {
        { GhostSkeletal.VsBlob.Get(), "Skeletal.hlsl:VSDepth", D3D12_SHADER_VISIBILITY_VERTEX },
        { GhostSkeletal.PsBlob.Get(), "Skeletal.hlsl:PSGhost", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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

    D3D12RootLayout& rs = Layout( "Grass" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );  // 0: b0 ViewProj
    rs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 1: t0 grass blade texture
    rs.AddTable( D3D12RootLayout::SRVRange( 1 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 2: t1 ground/undercoat texture
    // 3: b1 GrassCB { Time, WindStrength, HeroAffectStrength, pad, PlayerPosWS, pad }
    // VS: all of it; PS: none currently, but cheap to keep visible.
    rs.AddConstants( 1, 8, D3D12_SHADER_VISIBILITY_ALL );
    rs.AddConstants( 2, 8, D3D12_SHADER_VISIBILITY_ALL );      // 4: b2 fog — VS: CamPosWS; PS: color/near/far
    rs.AddSRV( 2, D3D12_SHADER_VISIBILITY_PIXEL );             // 5: t2 light StructuredBuffer (root SRV)
    // 6: b3 { LightCount, NumTilesX, LimitLightIntensity, pad }
    rs.AddConstants( 3, 4, D3D12_SHADER_VISIBILITY_PIXEL );
    rs.AddSRV( 3, D3D12_SHADER_VISIBILITY_PIXEL );             // 7: t3 per-tile LightGrid
    rs.AddSRV( 4, D3D12_SHADER_VISIBILITY_PIXEL );             // 8: t4 per-tile light-index list
    rs.AddCBV( 4, D3D12_SHADER_VISIBILITY_PIXEL );             // 9: b4 shadow-sampling CB (root CBV)
    rs.AddTable( D3D12RootLayout::SRVRange( 5 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 10: t5 CSM shadow-map array
    // 11: t6 point-shadow cube array (PBRLighting.hlsl requires this symbol)
    rs.AddTable( D3D12RootLayout::SRVRange( 6 ), D3D12_SHADER_VISIBILITY_PIXEL );
    // 12 = simple-SSAO mask bindless SRV-heap index (b5 AOCB, PS only), set once per frame by
    // DrawVegetation. Grass gets real AO now that RenderSSAO builds the mask from the PREVIOUS frame's COMPLETE
    // depth rather than the depth prepass grass never joins — see Vegetation.hlsl's PSMain. The grass shadow
    // CASTER (m_ShadowCasterGrassPSO, CreateGrassShadowCaster) shares this root sig but reads none of this.
    rs.AddConstants( 5, 1, D3D12_SHADER_VISIBILITY_PIXEL );    // 12: b5 AOCB { AoMaskIndex }

    rs.AddStaticSampler( D3D12RootLayout::SamplerAniso( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );      // s0 diffuse
    // s2 PCF, matches World/Vob/Skeletal.
    rs.AddStaticSampler( D3D12RootLayout::SamplerComparison( 2, D3D12_SHADER_VISIBILITY_PIXEL ) );
    // s1: point-clamp for the AO mask + the reprojection depth fetch — identical to World/Vob/Skeletal's s1.
    rs.AddStaticSampler( D3D12RootLayout::SamplerPoint( 1, D3D12_SHADER_VISIBILITY_PIXEL ) );

    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED: the PS reaches the AO mask and the previous-frame depth through
    // SM6.6 ResourceDescriptorHeap[...] (see ScreenSpaceAO.hlsl), same as the World/Vob/Skeletal sigs.
    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                          | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )
        return false;
    Grass.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Vegetation.hlsl", "VSMain", Shadermodel_VS, Grass.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Vegetation.hlsl", "PSMain", Shadermodel_PS, Grass.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    rs.ValidateShaders( {
        { Grass.VsBlob.Get(), "Vegetation.hlsl:VSMain", D3D12_SHADER_VISIBILITY_VERTEX },
        { Grass.PsBlob.Get(), "Vegetation.hlsl:PSMain", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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
    D3D12RootLayout& rs = Layout( "Video" );
    rs.AddConstants( 0, 4, D3D12_SHADER_VISIBILITY_VERTEX );   // 0: b0 viewport (float2 pos + float2 size)
    for ( UINT i = 0; i < 3; ++i )                             // 1..3: t0 Y, t1 U, t2 V
        rs.AddTable( D3D12RootLayout::SRVRange( i ), D3D12_SHADER_VISIBILITY_PIXEL );
    rs.AddStaticSampler( D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );   // s0

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ) )
        return false;
    Video.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Video.hlsl", "VSMain", Shadermodel_VS, Video.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Video.hlsl", "PSMain", Shadermodel_PS, Video.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    rs.ValidateShaders( {
        { Video.VsBlob.Get(), "Video.hlsl:VSMain", D3D12_SHADER_VISIBILITY_VERTEX },
        { Video.PsBlob.Get(), "Video.hlsl:PSMain", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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
    // FFPipelineConstantBuffer (b1) is fed Gothic's GraphicsState each draw as root constants — the
    // struct layout matches the HLSL cbuffer 1:1 (same reason D3D11 memcpy's it into the CB).
    static_assert( sizeof( GothicGraphicsState ) == 144, "FF constant layout must match FFPipelineConstantBuffer" );

    D3D12RootLayout& rs = Layout( "UI" );
    rs.AddConstants( 0, 4, D3D12_SHADER_VISIBILITY_VERTEX );   // 0: b0 float2 pos + float2 size
    rs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );   // 1: t0
    // 2: b1 FF state (36 DWORDs)
    rs.AddConstants( 1, sizeof( GothicGraphicsState ) / 4, D3D12_SHADER_VISIBILITY_PIXEL );
    rs.AddStaticSampler( D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_PIXEL,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP ) );   // s0

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ) )
        return false;
    UI.RootSig = rs.RootSig();

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

    rs.ValidateShaders( {
        { UI.VsBlob.Get(),     "UI.hlsl:VSMain",                D3D12_SHADER_VISIBILITY_VERTEX },
        { UI.PsBlob.Get(),     "UI.hlsl:PSMain",                D3D12_SHADER_VISIBILITY_PIXEL  },
        { UI.VsBlobMaxZ.Get(), "UI.hlsl:VSMain[FORCE_MAX_Z]",   D3D12_SHADER_VISIBILITY_VERTEX },
        { UI.PsBlobHdr.Get(),  "UI.hlsl:PSMain[LINEARIZE_OUTPUT]", D3D12_SHADER_VISIBILITY_PIXEL },
    } );

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
    D3D12RootLayout& rs = Layout( "Particle" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );  // 0: b0 ViewProj
    rs.AddConstants( 1, 4, D3D12_SHADER_VISIBILITY_VERTEX );   // 1: b1 camera pos
    rs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 2: t0 diffuse
    rs.AddStaticSampler( D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );  // s0

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ) )
        return false;
    Particle.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Particle.hlsl", "VSMain", Shadermodel_VS, Particle.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Particle.hlsl", "PSMain", Shadermodel_PS, Particle.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    rs.ValidateShaders( {
        { Particle.VsBlob.Get(), "Particle.hlsl:VSMain", D3D12_SHADER_VISIBILITY_VERTEX },
        { Particle.PsBlob.Get(), "Particle.hlsl:PSMain", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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
    D3D12RootLayout& rs = Layout( "Decal" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );  // 0: b0 ViewProj
    rs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 1: t0 diffuse
    rs.AddStaticSampler( D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );  // s0

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ) )
        return false;
    Decal.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Decal.hlsl", "VSMain", Shadermodel_VS, Decal.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Decal.hlsl", "PSMainLit", Shadermodel_PS, Decal.LitPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Decal.hlsl", "PSMainBlend", Shadermodel_PS, Decal.BlendPsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    rs.ValidateShaders( {
        { Decal.VsBlob.Get(),      "Decal.hlsl:VSMain",      D3D12_SHADER_VISIBILITY_VERTEX },
        { Decal.LitPsBlob.Get(),   "Decal.hlsl:PSMainLit",   D3D12_SHADER_VISIBILITY_PIXEL  },
        { Decal.BlendPsBlob.Get(), "Decal.hlsl:PSMainBlend", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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
    D3D12RootLayout& rs = Layout( "Skeletal" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );  // 0: b0 ViewProj
    rs.AddCBV( 1, D3D12_SHADER_VISIBILITY_VERTEX );            // 1: b1 per-instance
    rs.AddCBV( 2, D3D12_SHADER_VISIBILITY_VERTEX );            // 2: b2 bone palette
    rs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 3: t0 diffuse
    // 4: b3 fog — FogConstants (8 DWORDs); VS: CamPosWS; PS: color/near/far
    rs.AddConstants( 3, 8, D3D12_SHADER_VISIBILITY_ALL );
    // Forward+ point lights (mirrors World.RootSig params 3/4/5/6, here at 5..8 — see BindFrameLights). All
    // MUST be bound at every skeletal draw or the PS light-loop bound/grid is undefined → GPU hang.
    rs.AddSRV( 1, D3D12_SHADER_VISIBILITY_PIXEL );             // 5: t1 light StructuredBuffer (root SRV)
    rs.AddConstants( 4, 4, D3D12_SHADER_VISIBILITY_PIXEL );    // 6: b4 { LightCount, NumTilesX, pad, pad }
    rs.AddSRV( 2, D3D12_SHADER_VISIBILITY_PIXEL );             // 7: t2 per-tile LightGrid {Offset,Count}
    rs.AddSRV( 3, D3D12_SHADER_VISIBILITY_PIXEL );             // 8: t3 per-tile light-index list
    // 9: b5 shadow-sampling CB (skeletal's b3/b4 are fog/light count)
    rs.AddCBV( 5, D3D12_SHADER_VISIBILITY_PIXEL );
    // CSM sampling (P2.9c-4b): shadow-map array SRV at t4 (skeletal PS samples it like world/VOB);
    // point-light shadow cube array SRV at t5 (P2.10d).
    rs.AddTable( D3D12RootLayout::SRVRange( 4 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 10: t4 CSM array
    rs.AddTable( D3D12RootLayout::SRVRange( 5 ), D3D12_SHADER_VISIBILITY_PIXEL );  // 11: t5 point-shadow cube array
    // 12: b6 MaterialCB { MatNormalIndex, MatOrmIndex } — bindless indices
    rs.AddConstants( 6, 2, D3D12_SHADER_VISIBILITY_PIXEL );
    // 13 = simple-SSAO mask bindless SRV-heap index (b8 AOCB — b7 is GhostCB, used only by the separate
    // GhostSkeletal root sig/PSO, not this one). Set once per frame by DrawSkeletalColor before the base-mesh
    // draws; Skeletal.hlsl's PSMain reads it via ResourceDescriptorHeap[AoMaskIndex].
    rs.AddConstants( 8, 1, D3D12_SHADER_VISIBILITY_PIXEL );    // 13: b8 AOCB { AoMaskIndex }

    // s0 diffuse: 16x anisotropic (matches D3D11's main texture sampler) — sharpens surfaces at grazing
    // angles and in the distance, which trilinear alone smears badly.
    rs.AddStaticSampler( D3D12RootLayout::SamplerAniso( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );
    // s2 PCF (see world root sig).
    rs.AddStaticSampler( D3D12RootLayout::SamplerComparison( 2, D3D12_SHADER_VISIBILITY_PIXEL ) );
    // s1: point-clamp for the simple-SSAO mask fetch — see World.RootSig's identical s1 for why Load() (raw
    // texel coords) is wrong for the 1x1 "AO disabled" fallback and CLAMP-addressed Sample must be used instead.
    rs.AddStaticSampler( D3D12RootLayout::SamplerPoint( 1, D3D12_SHADER_VISIBILITY_PIXEL ) );

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                          | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )   // SM6.6 bindless normal/ORM
        return false;
    Skeletal.RootSig = rs.RootSig();

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

    rs.ValidateShaders( {
        { Skeletal.VsBlob.Get(),             "Skeletal.hlsl:VSMain",      D3D12_SHADER_VISIBILITY_VERTEX },
        { Skeletal.PsBlob.Get(),             "Skeletal.hlsl:PSMain",      D3D12_SHADER_VISIBILITY_PIXEL  },
        { Skeletal.DepthPrepassVsBlob.Get(), "Skeletal.hlsl:VSDepth",     D3D12_SHADER_VISIBILITY_VERTEX },
        { Skeletal.DepthPrepassPsBlob.Get(), "Skeletal.hlsl:PSDepthClip", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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
    D3D12RootLayout& rs = Layout( "PointShadow" );
    rs.AddCBV( 0, D3D12_SHADER_VISIBILITY_VERTEX );   // 0: b0 PCR_ViewProj[6]
    rs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );   // 1: t0 diffuse
    rs.AddStaticSampler( D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_PIXEL,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP ) );   // s0

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ) )
        return false;
    PointShadow.RootSig = rs.RootSig();

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

    rs.ValidateShaders( {
        { PointShadow.VsBlob.Get(),    "PointShadow.hlsl:VSCube",     D3D12_SHADER_VISIBILITY_VERTEX },
        { PointShadow.VobVsBlob.Get(), "PointShadow.hlsl:VSCubeVob",  D3D12_SHADER_VISIBILITY_VERTEX },
        { PointShadow.PsBlob.Get(),    "PointShadow.hlsl:PSCubeClip", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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
        D3D12RootLayout& skelRs = Layout( "PointShadowSkeletal" );
        skelRs.AddCBV( 0, D3D12_SHADER_VISIBILITY_VERTEX );   // 0: b0 PCR_ViewProj[6]
        skelRs.AddCBV( 1, D3D12_SHADER_VISIBILITY_VERTEX );   // 1: b1 instance (M_World/Fatness)
        skelRs.AddCBV( 2, D3D12_SHADER_VISIBILITY_VERTEX );   // 2: b2 bones
        skelRs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );   // 3: t0 diffuse
        skelRs.AddStaticSampler( D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_PIXEL,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP ) );   // s0 — same as the world/VOB caster sig above

        if ( !skelRs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ) )
            return false;
        PointShadow.SkeletalRootSig = skelRs.RootSig();

        skelRs.ValidateShaders( {
            { PointShadow.SkelVsBlob.Get(), "PointShadow.hlsl:VSCubeSkel", D3D12_SHADER_VISIBILITY_VERTEX },
            { PointShadow.PsBlob.Get(),     "PointShadow.hlsl:PSCubeClip", D3D12_SHADER_VISIBILITY_PIXEL  },
        } );

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

    D3D12RootLayout& rs = Layout( "Tonemap" );
    rs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );   // 0: t0 scene HDR
    // 1: b0 { Exposure, LumWhite, ToneMapMode, pad }
    rs.AddConstants( 0, 4, D3D12_SHADER_VISIBILITY_PIXEL );
    rs.AddSRV( 1, D3D12_SHADER_VISIBILITY_PIXEL );   // 2: t1 AdaptedLum
    rs.AddStaticSampler( D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );   // s0

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ) )
        return false;
    Tonemap.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Tonemap.hlsl", "VSFullscreen", Shadermodel_VS, Tonemap.VsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "Tonemap.hlsl", "PSTonemap", Shadermodel_PS, Tonemap.PsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    rs.ValidateShaders( {
        { Tonemap.VsBlob.Get(), "Tonemap.hlsl:VSFullscreen", D3D12_SHADER_VISIBILITY_VERTEX },
        { Tonemap.PsBlob.Get(), "Tonemap.hlsl:PSTonemap",    D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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

    // --- LumReduce root sig: b0 4x32-bit consts, t0 SRV table (scene color), u0 UAV root descriptor ---
    D3D12RootLayout& reduceRs = Layout( "LumReduce" );
    reduceRs.AddConstants( 0, 4, D3D12_SHADER_VISIBILITY_ALL );   // 0: b0 LumReduceCB
    reduceRs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_ALL );   // 1: t0 SceneHDR
    reduceRs.AddUAV( 0, D3D12_SHADER_VISIBILITY_ALL );            // 2: u0 PartialSums
    if ( !reduceRs.Build( device ) )
        return false;
    LumReduce.RootSig = reduceRs.RootSig();

    if ( !m_Shaders->CompileFromFile( "CS_LumReduce.hlsl", "CSMain", Shadermodel_CS, LumReduce.CsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    reduceRs.ValidateShaders( { { LumReduce.CsBlob.Get(), "CS_LumReduce.hlsl:CSMain", D3D12_SHADER_VISIBILITY_ALL } } );
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
    D3D12RootLayout& adaptRs = Layout( "LumAdapt" );
    adaptRs.AddConstants( 0, 4, D3D12_SHADER_VISIBILITY_ALL );   // 0: b0 LumAdaptCB
    adaptRs.AddSRV( 0, D3D12_SHADER_VISIBILITY_ALL );            // 1: t0 PartialSums
    adaptRs.AddUAV( 0, D3D12_SHADER_VISIBILITY_ALL );            // 2: u0 AdaptedLum
    if ( !adaptRs.Build( device ) )
        return false;
    LumAdapt.RootSig = adaptRs.RootSig();

    if ( !m_Shaders->CompileFromFile( "CS_LumAdapt.hlsl", "CSMain", Shadermodel_CS, LumAdapt.CsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    adaptRs.ValidateShaders( { { LumAdapt.CsBlob.Get(), "CS_LumAdapt.hlsl:CSMain", D3D12_SHADER_VISIBILITY_ALL } } );
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

    // Root signature: b0 ViewProj (16 consts, VS), t0 diffuse SRV table (PS — rebound per texture batch),
    // b2 the water/refraction CB (root CBV; VS reads the scroll time + view matrix, PS reads everything
    // else), b1 the atmosphere CB (root CBV, PS — ApplyAtmosphericScatteringGround).
    //
    // The refraction/reflection inputs (scene copy, depth copy, distortion, reflection cube) are NOT in the
    // table: they are fetched bindlessly out of the shared SRV heap by index from the water CB, hence the
    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED flag. That keeps this root signature at 4 params while the D3D11
    // equivalent needs five fixed t-slots, and avoids having to build a heap-contiguous descriptor run for
    // resources that live in unrelated slots.
    D3D12RootLayout& rs = Layout( "Water" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );   // 0: b0 ViewProj
    rs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );   // 1: t0 diffuse
    // 2: b2 WaterCB (projection/view/params/bindless indices)
    rs.AddCBV( 2, D3D12_SHADER_VISIBILITY_ALL );
    rs.AddCBV( 1, D3D12_SHADER_VISIBILITY_PIXEL );              // 3: b1 AtmosphereConstantBuffer

    // s0 — diffuse + the world-space distortion lookups.
    rs.AddStaticSampler( D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_PIXEL,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP ) );
    // s1: screen-space fetches (scene copy / depth copy). CLAMP, not WRAP — a distorted UV that walks a
    // texel past the viewport edge must repeat the border pixel, not wrap around to the opposite side of
    // the screen (which shows up as a bright seam along the water's screen-edge silhouette).
    rs.AddStaticSampler( D3D12RootLayout::SamplerLinear( 1, D3D12_SHADER_VISIBILITY_PIXEL ) );

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                          | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )
        return false;
    Water.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Water.hlsl", "VSMain", Shadermodel_VS, Water.VsBlob.ReleaseAndGetAddressOf() ) ) {
            return false;
    }
    if ( !m_Shaders->CompileFromFile( "Water.hlsl", "PSMain", Shadermodel_PS, Water.PsBlob.ReleaseAndGetAddressOf() ) ) {
            return false;
    }

    rs.ValidateShaders( {
        { Water.VsBlob.Get(), "Water.hlsl:VSMain", D3D12_SHADER_VISIBILITY_VERTEX },
        { Water.PsBlob.Get(), "Water.hlsl:PSMain", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

    // Same packed 36-byte ExVertexStructGPU as the world mesh; here TexCoord2 (@28, half2) is the water
    // UV-scroll delta (bound as TEXCOORD1), and DIFFUSE (@32) is the baked vertex tint. NORMAL (@12,
    // octahedral) feeds the PS's waterfall test — near-vertical water surfaces suppress their reflection.
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R16G16_SNORM,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },  // octahedral, world-space
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

    // NO blending — this is an OPAQUE draw, matching D3D11: DrawWaterSurfaces calls SetDefaultStates() and
    // GothicBlendStateInfo::SetDefault() leaves BlendEnabled = false. The water's see-through appearance is
    // composited inside the pixel shader from a copy of the finished scene (refraction), not by the blender.
    // The old SRC_ALPHA/INV_SRC_ALPHA blend with a constant 0.7 alpha is exactly what made D3D12 water read
    // as a solid tinted sheet: a flat texture lerped over the scene can't darken with depth, refract, or
    // reflect.
    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = FALSE;
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
    // the world/VOB depth prepass) but mask off every color write.
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

    D3D12RootLayout& rs = Layout( "LightCull" );
    // 0: b0 CullCB — ProjScale(2) + ScreenDim(2) + TotalLights + NumTilesX + ProjA + ProjB
    rs.AddConstants( 0, 8, D3D12_SHADER_VISIBILITY_ALL );
    rs.AddSRV( 0, D3D12_SHADER_VISIBILITY_ALL );   // 1: t0 SB_Lights
    rs.AddUAV( 0, D3D12_SHADER_VISIBILITY_ALL );   // 2: u0 RW_LightGrid
    rs.AddUAV( 1, D3D12_SHADER_VISIBILITY_ALL );   // 3: u1 RW_LightIndexList
    rs.AddTable( D3D12RootLayout::SRVRange( 1 ), D3D12_SHADER_VISIBILITY_ALL );   // 4: t1 DepthTex (SRV heap)

    if ( !rs.Build( device ) )   // compute: no IA input layout
        return false;
    LightCull.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "LightCull.hlsl", "CSMain", Shadermodel_CS, LightCull.CsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    rs.ValidateShaders( { { LightCull.CsBlob.Get(), "LightCull.hlsl:CSMain", D3D12_SHADER_VISIBILITY_ALL } } );

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

    D3D12RootLayout& rs = Layout( "AdvanceRain" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_ALL );   // 0: b0 AdvanceRainCB
    rs.AddSRV( 0, D3D12_SHADER_VISIBILITY_ALL );             // 1: t0 StaticData
    rs.AddUAV( 0, D3D12_SHADER_VISIBILITY_ALL );             // 2: u0 DynamicData

    if ( !rs.Build( device ) )   // compute: no IA input layout
        return false;
    AdvanceRain.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "AdvanceRain.hlsl", "CSMain", Shadermodel_CS, AdvanceRain.CsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    rs.ValidateShaders( { { AdvanceRain.CsBlob.Get(), "AdvanceRain.hlsl:CSMain", D3D12_SHADER_VISIBILITY_ALL } } );

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

    D3D12RootLayout& rs = Layout( "RainDraw" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );   // 0: b0 ViewProjCB
    // 1: b1 RainInfoCB — read by both VS (billboard construction) and PS (rainResponse eye/light vectors)
    rs.AddConstants( 1, 10, D3D12_SHADER_VISIBILITY_ALL );
    rs.AddSRV( 0, D3D12_SHADER_VISIBILITY_VERTEX );             // 2: t0 DynamicData
    rs.AddSRV( 1, D3D12_SHADER_VISIBILITY_VERTEX );             // 3: t1 StaticData
    rs.AddConstants( 2, 2, D3D12_SHADER_VISIBILITY_PIXEL );     // 4: b2 RainTexCB
    // 5: b3 RainShadowCB — VS-only (IsWet is called from the VS); 16 (float4x4) + 1 (heap slot index)
    rs.AddConstants( 3, 17, D3D12_SHADER_VISIBILITY_VERTEX );

    rs.AddStaticSampler( D3D12RootLayout::SamplerAniso( 0, D3D12_SHADER_VISIBILITY_PIXEL, 4 ) );   // s0 rain/snow texture array
    // s1: rain shadowmap comparison sampler — matches D3D11's m_RainDropShadowSamplerState (WRAP, LESS_EQUAL).
    D3D12_STATIC_SAMPLER_DESC rainShadowSampler =
        D3D12RootLayout::SamplerComparison( 1, D3D12_SHADER_VISIBILITY_VERTEX );
    rainShadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    rainShadowSampler.AddressU = rainShadowSampler.AddressV = rainShadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    rs.AddStaticSampler( rainShadowSampler );

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )
        return false;
    RainDraw.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Rain.hlsl", "VSMain", Shadermodel_VS, RainDraw.VsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }
    if ( !m_Shaders->CompileFromFile( "Rain.hlsl", "PSMain", Shadermodel_PS, RainDraw.PsBlob.ReleaseAndGetAddressOf() ) ) {
        return false;
    }

    rs.ValidateShaders( {
        { RainDraw.VsBlob.Get(), "Rain.hlsl:VSMain", D3D12_SHADER_VISIBILITY_VERTEX },
        { RainDraw.PsBlob.Get(), "Rain.hlsl:PSMain", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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

    // s0, shared by all three compute passes + the composite.
    const D3D12_STATIC_SAMPLER_DESC sampler =
        D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_ALL );

    // --- Down root sig (prefilter + downsample): b0 8x32-bit consts, t0 SRV table, u0 UAV table ---
    D3D12RootLayout& downRs = Layout( "BloomDown" );
    downRs.AddConstants( 0, 8, D3D12_SHADER_VISIBILITY_ALL );   // 0: b0 BloomConstantBuffer
    downRs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_ALL );   // 1: t0
    downRs.AddTable( D3D12RootLayout::UAVRange( 0 ), D3D12_SHADER_VISIBILITY_ALL );   // 2: u0
    downRs.AddStaticSampler( sampler );
    if ( !downRs.Build( device ) )
        return false;
    Bloom.DownRootSig = downRs.RootSig();

    // --- Up root sig (upsample): b0 8x32-bit consts, t0+t1 SRV table (2 contiguous descriptors), u0 UAV table ---
    D3D12RootLayout& upRs = Layout( "BloomUp" );
    upRs.AddConstants( 0, 8, D3D12_SHADER_VISIBILITY_ALL );     // 0: b0 BloomConstantBuffer
    upRs.AddTable( D3D12RootLayout::SRVRange( 0, 2 ), D3D12_SHADER_VISIBILITY_ALL );  // 1: t0, t1
    upRs.AddTable( D3D12RootLayout::UAVRange( 0 ), D3D12_SHADER_VISIBILITY_ALL );     // 2: u0
    upRs.AddStaticSampler( sampler );
    if ( !upRs.Build( device ) )
        return false;
    Bloom.UpRootSig = upRs.RootSig();

    const D3D_SHADER_MACRO prefilterMacro[] = { { "BLOOM_PREFILTER", "1" }, { nullptr, nullptr } };
    if ( !m_Shaders->CompileFromFile( "CS_Bloom_Downsample.hlsl", "CSMain", Shadermodel_CS, Bloom.PrefilterCsBlob.ReleaseAndGetAddressOf(), prefilterMacro ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "CS_Bloom_Downsample.hlsl", "CSMain", Shadermodel_CS, Bloom.DownsampleCsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "CS_Bloom_Upsample.hlsl", "CSMain", Shadermodel_CS, Bloom.UpsampleCsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    downRs.ValidateShaders( {
        { Bloom.PrefilterCsBlob.Get(),  "CS_Bloom_Downsample.hlsl:CSMain[BLOOM_PREFILTER]", D3D12_SHADER_VISIBILITY_ALL },
        { Bloom.DownsampleCsBlob.Get(), "CS_Bloom_Downsample.hlsl:CSMain",                  D3D12_SHADER_VISIBILITY_ALL },
    } );
    upRs.ValidateShaders( { { Bloom.UpsampleCsBlob.Get(), "CS_Bloom_Upsample.hlsl:CSMain", D3D12_SHADER_VISIBILITY_ALL } } );

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
        D3D12RootLayout& compositeRs = Layout( "BloomComposite" );
        compositeRs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_PIXEL );   // 0: t0
        compositeRs.AddConstants( 0, 1, D3D12_SHADER_VISIBILITY_PIXEL );   // 1: b0 { Intensity }
        compositeRs.AddStaticSampler( sampler );
        if ( !compositeRs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ) )
            return false;
        Bloom.CompositeRootSig = compositeRs.RootSig();

        if ( !m_Shaders->CompileFromFile( "Bloom_Composite.hlsl", "VSFullscreen", Shadermodel_VS, Bloom.CompositeVsBlob.ReleaseAndGetAddressOf() ) )
            return false;
        if ( !m_Shaders->CompileFromFile( "Bloom_Composite.hlsl", "PSComposite", Shadermodel_PS, Bloom.CompositePsBlob.ReleaseAndGetAddressOf() ) )
            return false;

        compositeRs.ValidateShaders( {
            { Bloom.CompositeVsBlob.Get(), "Bloom_Composite.hlsl:VSFullscreen", D3D12_SHADER_VISIBILITY_VERTEX },
            { Bloom.CompositePsBlob.Get(), "Bloom_Composite.hlsl:PSComposite",  D3D12_SHADER_VISIBILITY_PIXEL  },
        } );

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

    D3D12RootLayout& rs = Layout( "SMAA" );
    // 0: b0 cbSMAA — 9 root constants { float4 RT_METRICS; uint Color/Edges/Blend/Area/Search index }.
    // ALL because RT_METRICS is read by both the VS and PS.
    rs.AddConstants( 0, 9, D3D12_SHADER_VISIBILITY_ALL );
    // s0 linear-clamp, s1 point-clamp (matches D3D11SMAA::Init's two sampler states).
    D3D12_STATIC_SAMPLER_DESC linearClamp = D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_PIXEL );
    linearClamp.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    rs.AddStaticSampler( linearClamp );
    rs.AddStaticSampler( D3D12RootLayout::SamplerPoint( 1, D3D12_SHADER_VISIBILITY_PIXEL ) );

    // DIRECTLY_INDEXED enables SM6.6 ResourceDescriptorHeap[...] bindless fetch of the SMAA textures.
    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                          | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )
        return false;
    Smaa.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "SMAA.hlsl", "EdgeDetectionVS", Shadermodel_VS, Smaa.EdgeVsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "SMAA.hlsl", "LumaEdgeDetectionPS", Shadermodel_PS, Smaa.EdgePsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "SMAA.hlsl", "BlendingWeightCalculationVS", Shadermodel_VS, Smaa.BlendVsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "SMAA.hlsl", "BlendingWeightCalculationPS", Shadermodel_PS, Smaa.BlendPsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "SMAA.hlsl", "NeighborhoodBlendingVS", Shadermodel_VS, Smaa.NeighborVsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "SMAA.hlsl", "NeighborhoodBlendingPS", Shadermodel_PS, Smaa.NeighborPsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    rs.ValidateShaders( {
        { Smaa.EdgeVsBlob.Get(),     "SMAA.hlsl:EdgeDetectionVS",             D3D12_SHADER_VISIBILITY_VERTEX },
        { Smaa.EdgePsBlob.Get(),     "SMAA.hlsl:LumaEdgeDetectionPS",         D3D12_SHADER_VISIBILITY_PIXEL  },
        { Smaa.BlendVsBlob.Get(),    "SMAA.hlsl:BlendingWeightCalculationVS", D3D12_SHADER_VISIBILITY_VERTEX },
        { Smaa.BlendPsBlob.Get(),    "SMAA.hlsl:BlendingWeightCalculationPS", D3D12_SHADER_VISIBILITY_PIXEL  },
        { Smaa.NeighborVsBlob.Get(), "SMAA.hlsl:NeighborhoodBlendingVS",      D3D12_SHADER_VISIBILITY_VERTEX },
        { Smaa.NeighborPsBlob.Get(), "SMAA.hlsl:NeighborhoodBlendingPS",      D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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

ID3D12PipelineState* D3D12PipelineState::GetOrCreateQuadMarkPipeline( const GothicBlendStateInfo& blend, bool depthWrite ) {
    // Lit quad marks: World.RootSig + World.hlsl's VSQuadMark/PSMain. Reusing the world PIXEL shader verbatim
    // is the whole point — it is what gives the marks the same SrgbToLinear -> DelightDiffuse albedo handling,
    // CSM shadows, tiled point lights, SSAO, wetness and sky IBL the world mesh gets, with no duplicated
    // lighting code to drift. Only the VS + input layout differ (see VSQuadMark), plus the blend/depth state,
    // which is Gothic's per-material alpha func, hence the same blend-keyed cache shape the FX pass uses.
    const uint32_t key = BlendKey( blend ) | (depthWrite ? (1u << 31) : 0u);
    auto it = World.QuadMarkPipelines.find( key );
    if ( it != World.QuadMarkPipelines.end() ) return it->second.Get();
    if ( !World.RootSig || !World.QuadMarkVsBlob || !World.PsBlob ) return nullptr;

    // CPU-side ExVertexStruct (stride 60): Position @0, full-precision float3 Normal @12, TexCoord0 @24,
    // Color @40. Unlike the world mesh's packed 36-byte vertex, nothing here is quantized — quad marks are
    // built per-mark on the CPU (WorldConverter::UpdateQuadMarkInfo) and never go through the packer.
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "DIFFUSE",  0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = World.RootSig.Get();
    pso.VS = { World.QuadMarkVsBlob->GetBufferPointer(), World.QuadMarkVsBlob->GetBufferSize() };
    pso.PS = { World.PsBlob->GetBufferPointer(), World.PsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // D3D11's DrawQuadMarks forces CM_CULL_NONE
    pso.RasterizerState.DepthClipEnable = TRUE;
    // Marks lie flat ON the surface they were projected onto, so they z-fight it without a bias. The world
    // mesh writes that depth in the prepass; a small slope-scaled pull toward the camera (reversed-Z: POSITIVE
    // depth bias moves toward the near plane) keeps them on top without visibly detaching them.
    pso.RasterizerState.DepthBias = 200;
    pso.RasterizerState.SlopeScaledDepthBias = 1.0f;

    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = blend.BlendEnabled ? TRUE : FALSE;
    rt.SrcBlend = static_cast<D3D12_BLEND>(blend.SrcBlend);
    rt.DestBlend = static_cast<D3D12_BLEND>(blend.DestBlend);
    rt.BlendOp = static_cast<D3D12_BLEND_OP>(blend.BlendOp);
    rt.SrcBlendAlpha = static_cast<D3D12_BLEND>(blend.SrcBlendAlpha);
    rt.DestBlendAlpha = static_cast<D3D12_BLEND>(blend.DestBlendAlpha);
    rt.BlendOpAlpha = static_cast<D3D12_BLEND_OP>(blend.BlendOpAlpha);
    rt.RenderTargetWriteMask = blend.ColorWritesEnabled ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;   // reversed-Z
    pso.DepthStencilState.StencilEnable = FALSE;

    ComPtr<ID3D12PipelineState> state;
    if ( FAILED( m_Device->GetDevice()->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( state.GetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed for lit quad-mark key 0x" << std::hex << key << ".";
        return nullptr;
    }
    ID3D12PipelineState* raw = state.Get();
    World.QuadMarkPipelines.emplace( key, std::move( state ) );
    return raw;
}

bool D3D12PipelineState::CreateFx() {
    // Quad marks (zCQuadMark) + poly strips (weapon/spell trails, lightning flashes) — see D3D12Fx.cpp.
    // Both feed CPU-built ExVertexStruct triangle lists through one unlit shader with a per-draw blend mode,
    // so one root signature and one blend-keyed PSO cache serve both. Non-fatal — the draw paths guard on
    // the root sig existing.
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    D3D12RootLayout& rs = Layout( "Fx" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );   // 0: b0 ViewProj
    // 1: b1 World (identity for poly strips, per-mark for quad marks)
    rs.AddConstants( 1, 16, D3D12_SHADER_VISIBILITY_VERTEX );
    // 2: b2 { bindless diffuse index, alpha-test flag }
    rs.AddConstants( 2, 4, D3D12_SHADER_VISIBILITY_PIXEL );
    // s0: same 16x aniso wrap the world/VOB passes use for diffuse maps.
    rs.AddStaticSampler( D3D12RootLayout::SamplerAniso( 0, D3D12_SHADER_VISIBILITY_PIXEL ) );

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                          | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )
        return false;
    Fx.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Fx.hlsl", "VSMain", Shadermodel_VS, Fx.VsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "Fx.hlsl", "PSMain", Shadermodel_PS, Fx.PsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    rs.ValidateShaders( {
        { Fx.VsBlob.Get(), "Fx.hlsl:VSMain", D3D12_SHADER_VISIBILITY_VERTEX },
        { Fx.PsBlob.Get(), "Fx.hlsl:PSMain", D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

    // Warm the state both passes start from (D3D11's SetDefaultStates: no blending, depth-write on).
    GothicBlendStateInfo defaultBlend;
    defaultBlend.SetDefault();
    if ( !GetOrCreateFxPipeline( defaultBlend, true ) ) {
        LogWarn() << "D3D12: failed to create the default FX pipeline.";
        return false;
    }
    return true;
}

ID3D12PipelineState* D3D12PipelineState::GetOrCreateFxPipeline( const GothicBlendStateInfo& blend, bool depthWrite ) {
    const uint32_t key = BlendKey( blend ) | (depthWrite ? (1u << 31) : 0u);
    auto it = Fx.BlendPipelines.find( key );
    if ( it != Fx.BlendPipelines.end() ) return it->second.Get();
    if ( !Fx.RootSig || !Fx.VsBlob || !Fx.PsBlob ) return nullptr;

    // CPU-side ExVertexStruct (stride 60): Position @0, TexCoord0 @24, Color @40. Normal/TexCoord2/Tangent
    // are unread by this unlit shader and left out of the layout.
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "DIFFUSE",  0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = Fx.RootSig.Get();
    pso.VS = { Fx.VsBlob->GetBufferPointer(), Fx.VsBlob->GetBufferSize() };
    pso.PS = { Fx.PsBlob->GetBufferPointer(), Fx.PsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;   // both passes run inside world rendering, on the HDR scene target
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // CULL_NONE: both D3D11 passes explicitly force CM_CULL_NONE (quad marks lie flat on arbitrary geometry,
    // poly strips are camera-facing double-sided ribbons).
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

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;   // reversed-Z
    pso.DepthStencilState.StencilEnable = FALSE;

    ComPtr<ID3D12PipelineState> state;
    if ( FAILED( m_Device->GetDevice()->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( state.GetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed for FX key 0x" << std::hex << key << ".";
        return nullptr;
    }
    ID3D12PipelineState* raw = state.Get();
    Fx.BlendPipelines.emplace( key, std::move( state ) );
    return raw;
}

bool D3D12PipelineState::CreateSharpen() {
    // Post-tonemap sharpening — port of D3D11's two SHARPEN_* modes (D3D11PFX_CAS::Apply and
    // D3D11PfxRenderer::RenderSimpleSharpen). Both modes are fullscreen-triangle passes reading the LDR copy
    // of the swapchain bindlessly and writing the swapchain, so they share one root signature and differ only
    // in the pixel shader. Non-fatal on failure — RenderSharpen() guards on the PSO for the selected mode.
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;

    D3D12RootLayout& rs = Layout( "Sharpen" );
    // 0: b0 SharpenCB — 12 root constants { uint4 CasConst0; uint4 CasConst1; uint SrcIndex;
    // float SharpenStrength; float2 TextureSize }. CAS reads only the first 9, the simple mode only the
    // last 4 — one layout keeps the two PSOs on the same root signature (no rebind between modes; only
    // one ever runs per frame).
    rs.AddConstants( 0, 12, D3D12_SHADER_VISIBILITY_PIXEL );
    // s0 linear-clamp: the simple mode's 3x3 box blur samples off-pixel-center UVs and must repeat the border
    // at the screen edge rather than wrap. CAS uses Load() and ignores the sampler entirely.
    D3D12_STATIC_SAMPLER_DESC linearClamp = D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_PIXEL );
    linearClamp.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    rs.AddStaticSampler( linearClamp );

    // DIRECTLY_INDEXED enables the SM6.6 ResourceDescriptorHeap[SrcIndex] fetch of the LDR copy.
    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                          | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )
        return false;
    Sharpen.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Sharpen.hlsl", "VSFullscreen", Shadermodel_VS, Sharpen.VsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "Sharpen.hlsl", "PSSimple", Shadermodel_PS, Sharpen.SimplePsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "Sharpen.hlsl", "PSCas", Shadermodel_PS, Sharpen.CasPsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    rs.ValidateShaders( {
        { Sharpen.VsBlob.Get(),       "Sharpen.hlsl:VSFullscreen", D3D12_SHADER_VISIBILITY_VERTEX },
        { Sharpen.SimplePsBlob.Get(), "Sharpen.hlsl:PSSimple",     D3D12_SHADER_VISIBILITY_PIXEL  },
        { Sharpen.CasPsBlob.Get(),    "Sharpen.hlsl:PSCas",        D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

    auto makePso = [&]( ID3DBlob* ps, ID3D12PipelineState** out, const char* name ) -> bool {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = Sharpen.RootSig.Get();
        pso.VS = { Sharpen.VsBlob->GetBufferPointer(), Sharpen.VsBlob->GetBufferSize() };
        pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.InputLayout = { nullptr, 0 };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kBackBufferFormat;
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
            LogWarn() << "D3D12: CreateGraphicsPipelineState failed (Sharpen " << name << ").";
            return false;
        }
        return true;
        };

    if ( !makePso( Sharpen.SimplePsBlob.Get(), Sharpen.SimplePSO.ReleaseAndGetAddressOf(), "simple" ) ) return false;
    if ( !makePso( Sharpen.CasPsBlob.Get(), Sharpen.CasPSO.ReleaseAndGetAddressOf(), "cas" ) ) return false;

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
    const D3D12_STATIC_SAMPLER_DESC sampler =
        D3D12RootLayout::SamplerPoint( 0, D3D12_SHADER_VISIBILITY_ALL );

    // --- Main pass root sig: b0 12x32-bit SSAOCB, t0 depth SRV table, u0 AO-output UAV table ---
    D3D12RootLayout& mainRs = Layout( "AOMain" );
    mainRs.AddConstants( 0, 12, D3D12_SHADER_VISIBILITY_ALL );   // 0: b0 SSAOCB
    mainRs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_ALL );   // 1: t0 DepthTex
    mainRs.AddTable( D3D12RootLayout::UAVRange( 0 ), D3D12_SHADER_VISIBILITY_ALL );   // 2: u0 OutputAO
    mainRs.AddStaticSampler( sampler );
    if ( !mainRs.Build( device ) )
        return false;
    AO.MainRootSig = mainRs.RootSig();

    // --- Blur pass root sig: b0 8x32-bit BlurCB, t0+t1 SRV table (AO input + depth), u0 UAV table ---
    D3D12RootLayout& blurRs = Layout( "AOBlur" );
    blurRs.AddConstants( 0, 8, D3D12_SHADER_VISIBILITY_ALL );    // 0: b0 BlurCB
    // 1: t0 BlurAOTex, t1 BlurDepthTex
    blurRs.AddTable( D3D12RootLayout::SRVRange( 0, 2 ), D3D12_SHADER_VISIBILITY_ALL );
    blurRs.AddTable( D3D12RootLayout::UAVRange( 0 ), D3D12_SHADER_VISIBILITY_ALL );   // 2: u0 OutputAO
    blurRs.AddStaticSampler( sampler );
    if ( !blurRs.Build( device ) )
        return false;
    AO.BlurRootSig = blurRs.RootSig();

    if ( !m_Shaders->CompileFromFile( "SSAO.hlsl", "CSMain", Shadermodel_CS, AO.MainCsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "SSAO.hlsl", "CSBlur", Shadermodel_CS, AO.BlurCsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    mainRs.ValidateShaders( { { AO.MainCsBlob.Get(), "SSAO.hlsl:CSMain", D3D12_SHADER_VISIBILITY_ALL } } );
    blurRs.ValidateShaders( { { AO.BlurCsBlob.Get(), "SSAO.hlsl:CSBlur", D3D12_SHADER_VISIBILITY_ALL } } );

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

bool D3D12PipelineState::CreateSkyIbl() {
    // Sky image-based lighting (Shaders/D3D12/SkyIbl.hlsl) — the indirect-light source that replaces the flat
    // greyscale ambient in PBRLighting.hlsl's ComputeSunLightingPBR. Non-fatal: RenderSkyIBL() guards on every
    // PSO below, and the lit shaders fall back to the old flat ambient whenever the cube indices are invalid.
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;

    // s0: linear WRAP is wrong for cube faces (it would bleed across the seam); CLAMP is what cube sampling
    // wants, and the hardware handles the cross-face filtering itself.
    const D3D12_STATIC_SAMPLER_DESC sampler =
        D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_ALL );

    // --- Radiance root sig: b0 20x32-bit SkyRadianceCB, u0 UAV table (no SRV — it is purely analytic) ---
    D3D12RootLayout& radianceRs = Layout( "SkyIblRadiance" );
    radianceRs.AddConstants( 0, 20, D3D12_SHADER_VISIBILITY_ALL );   // 0: b0 SkyRadianceCB
    radianceRs.AddTable( D3D12RootLayout::UAVRange( 0 ), D3D12_SHADER_VISIBILITY_ALL );   // 1: u0 OutputCube
    if ( !radianceRs.Build( device ) )
        return false;
    SkyIbl.RadianceRootSig = radianceRs.RootSig();

    // --- Filter root sig (prefilter + irradiance): b1 4x32-bit PrefilterCB, t0 mip-0 SRV table, u0 UAV table ---
    D3D12RootLayout& filterRs = Layout( "SkyIblFilter" );
    filterRs.AddConstants( 1, 4, D3D12_SHADER_VISIBILITY_ALL );      // 0: b1 PrefilterCB
    // 1: t0 TX_SkySource (mip-0-only cube view)
    filterRs.AddTable( D3D12RootLayout::SRVRange( 0 ), D3D12_SHADER_VISIBILITY_ALL );
    filterRs.AddTable( D3D12RootLayout::UAVRange( 0 ), D3D12_SHADER_VISIBILITY_ALL );     // 2: u0 OutputCube
    filterRs.AddStaticSampler( sampler );
    if ( !filterRs.Build( device ) )
        return false;
    SkyIbl.FilterRootSig = filterRs.RootSig();

    if ( !m_Shaders->CompileFromFile( "SkyIbl.hlsl", "CSSkyRadiance", Shadermodel_CS, SkyIbl.RadianceCsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "SkyIbl.hlsl", "CSPrefilter", Shadermodel_CS, SkyIbl.PrefilterCsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "SkyIbl.hlsl", "CSIrradiance", Shadermodel_CS, SkyIbl.IrradianceCsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    radianceRs.ValidateShaders( { { SkyIbl.RadianceCsBlob.Get(), "SkyIbl.hlsl:CSSkyRadiance", D3D12_SHADER_VISIBILITY_ALL } } );
    filterRs.ValidateShaders( {
        { SkyIbl.PrefilterCsBlob.Get(),  "SkyIbl.hlsl:CSPrefilter",  D3D12_SHADER_VISIBILITY_ALL },
        { SkyIbl.IrradianceCsBlob.Get(), "SkyIbl.hlsl:CSIrradiance", D3D12_SHADER_VISIBILITY_ALL },
    } );

    struct { ID3D12RootSignature* rs; ID3DBlob* cs; ID3D12PipelineState** pso; const char* name; } psos[] = {
        { SkyIbl.RadianceRootSig.Get(), SkyIbl.RadianceCsBlob.Get(),   SkyIbl.RadiancePSO.ReleaseAndGetAddressOf(),   "SkyIbl radiance" },
        { SkyIbl.FilterRootSig.Get(),   SkyIbl.PrefilterCsBlob.Get(),  SkyIbl.PrefilterPSO.ReleaseAndGetAddressOf(),  "SkyIbl prefilter" },
        { SkyIbl.FilterRootSig.Get(),   SkyIbl.IrradianceCsBlob.Get(), SkyIbl.IrradiancePSO.ReleaseAndGetAddressOf(), "SkyIbl irradiance" },
    };
    for ( const auto& p : psos ) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = p.rs;
        pd.CS = { p.cs->GetBufferPointer(), p.cs->GetBufferSize() };
        if ( FAILED( device->CreateComputePipelineState( &pd, IID_PPV_ARGS( p.pso ) ) ) ) {
            LogWarn() << "D3D12: CreateComputePipelineState failed (" << p.name << ").";
            return false;
        }
    }
    return true;
}

bool D3D12PipelineState::CreateFog() {
    // Height fog + god rays (plan item #5) — mirrors D3D11's PostFX composition (D3D11PfxRenderer::
    // RenderPostFXComposition + D3D11PFX_GodRays::RenderToTextureCS). Non-fatal on failure:
    // RenderFogAndGodRays() guards on every PSO below.
    ID3D12Device* device = m_Device->GetDevice();
    if ( !device ) return false;

    // --- God-ray compute root sig (shared by CSMask + CSZoom) ---
    // b0: 12 root constants. CSMask reads the first 4 (3 heap indices + pad), CSZoom all 12 (blur params +
    // 2 heap indices) — two cbuffers declared at b0 in the same file, only the one an entry point references
    // survives into that PSO (same pattern as SSAO.hlsl's SSAOCB/BlurCB). Everything else is bindless.
    D3D12RootLayout& godRayRs = Layout( "FogGodRay" );
    godRayRs.AddConstants( 0, 12, D3D12_SHADER_VISIBILITY_ALL );   // 0: b0
    D3D12_STATIC_SAMPLER_DESC godRaySampler = D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_ALL );
    godRaySampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;   // s0 SS_LinearClamp
    godRayRs.AddStaticSampler( godRaySampler );
    if ( !godRayRs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )   // SM6.6 ResourceDescriptorHeap
        return false;
    Fog.GodRayRootSig = godRayRs.RootSig();

    // --- Composition graphics root sig: b0 height-fog CBV, b1 atmosphere CBV, b2 4 root consts ---
    D3D12RootLayout& compositeRs = Layout( "FogComposite" );
    compositeRs.AddCBV( 0, D3D12_SHADER_VISIBILITY_PIXEL );   // 0: b0 PFXBuffer (HeightfogConstantBuffer)
    compositeRs.AddCBV( 1, D3D12_SHADER_VISIBILITY_PIXEL );   // 1: b1 Atmosphere (AtmosphereConstantBuffer)
    // 2: b2 FogCompositeCB { depthIdx, godRaysIdx, flags, pad }
    compositeRs.AddConstants( 2, 4, D3D12_SHADER_VISIBILITY_PIXEL );
    // s0: god-ray upscale (quarter -> full res); s1: depth (1:1, never filtered).
    D3D12_STATIC_SAMPLER_DESC upscaleSampler = D3D12RootLayout::SamplerLinear( 0, D3D12_SHADER_VISIBILITY_PIXEL );
    upscaleSampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    compositeRs.AddStaticSampler( upscaleSampler );
    compositeRs.AddStaticSampler( D3D12RootLayout::SamplerPoint( 1, D3D12_SHADER_VISIBILITY_PIXEL ) );
    if ( !compositeRs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )
        return false;
    Fog.CompositeRootSig = compositeRs.RootSig();

    if ( !m_Shaders->CompileFromFile( "GodRays.hlsl", "CSMask", Shadermodel_CS, Fog.MaskCsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "GodRays.hlsl", "CSZoom", Shadermodel_CS, Fog.ZoomCsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "HeightFog.hlsl", "VSFullscreen", Shadermodel_VS, Fog.CompositeVsBlob.ReleaseAndGetAddressOf() )
        || !m_Shaders->CompileFromFile( "HeightFog.hlsl", "PSComposite", Shadermodel_PS, Fog.CompositePsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    godRayRs.ValidateShaders( {
        { Fog.MaskCsBlob.Get(), "GodRays.hlsl:CSMask", D3D12_SHADER_VISIBILITY_ALL },
        { Fog.ZoomCsBlob.Get(), "GodRays.hlsl:CSZoom", D3D12_SHADER_VISIBILITY_ALL },
    } );
    compositeRs.ValidateShaders( {
        { Fog.CompositeVsBlob.Get(), "HeightFog.hlsl:VSFullscreen", D3D12_SHADER_VISIBILITY_VERTEX },
        { Fog.CompositePsBlob.Get(), "HeightFog.hlsl:PSComposite",  D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

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

    auto makeComputePSO = [&]( const char* file, const char* entry, const D3D12RootLayout& rs,
        ID3DBlob** blob, ID3D12PipelineState** pso ) -> bool {
        if ( !m_Shaders->CompileFromFile( file, entry, Shadermodel_CS, blob ) ) return false;
        rs.ValidateShaders( { { *blob, entry, D3D12_SHADER_VISIBILITY_ALL } } );
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = rs.Get();
        desc.CS = { (*blob)->GetBufferPointer(), (*blob)->GetBufferSize() };
        if ( FAILED( device->CreateComputePipelineState( &desc, IID_PPV_ARGS( pso ) ) ) ) {
            LogWarn() << "D3D12: CreateComputePipelineState failed (" << entry << ").";
            return false;
        }
        return true;
        };

    // --- Hi-Z build root sig: b0 4 root consts (src/dst heap indices), fully bindless, no tables ---
    D3D12RootLayout& hiZRs = Layout( "CullHiZ" );
    hiZRs.AddConstants( 0, 4, D3D12_SHADER_VISIBILITY_ALL );   // 0: b0 HiZCopyCB / HiZReduceCB
    if ( !hiZRs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )   // SM6.6 ResourceDescriptorHeap
        return false;
    Cull.HiZRootSig = hiZRs.RootSig();

    if ( !makeComputePSO( "HiZ.hlsl", "CSCopyDepth", hiZRs,
        Cull.HiZCopyCsBlob.ReleaseAndGetAddressOf(), Cull.HiZCopyPSO.ReleaseAndGetAddressOf() ) ) return false;
    if ( !makeComputePSO( "HiZ.hlsl", "CSReduce", hiZRs,
        Cull.HiZReduceCsBlob.ReleaseAndGetAddressOf(), Cull.HiZReducePSO.ReleaseAndGetAddressOf() ) ) return false;

    // --- VOB cull root sig: b0 24 consts (ViewProj + Hi-Z params), t0/t1 root SRVs, u0/u1 root UAVs ---
    // The visual records + instance streams are plain structured buffers, so they ride as root descriptors
    // (no heap slots). Only the Hi-Z pyramid is a texture and it comes in bindlessly by heap index.
    D3D12RootLayout& vobCullRs = Layout( "CullVob" );
    vobCullRs.AddConstants( 0, 24, D3D12_SHADER_VISIBILITY_ALL );   // 0: b0 VobCullCB — float4x4 ViewProj + 8 uints
    vobCullRs.AddSRV( 0, D3D12_SHADER_VISIBILITY_ALL );             // 1: t0 Visuals
    vobCullRs.AddSRV( 1, D3D12_SHADER_VISIBILITY_ALL );             // 2: t1 InInstances
    vobCullRs.AddUAV( 0, D3D12_SHADER_VISIBILITY_ALL );             // 3: u0 OutInstances (compacted)
    vobCullRs.AddUAV( 1, D3D12_SHADER_VISIBILITY_ALL );             // 4: u1 VisibleCounts
    if ( !vobCullRs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED ) )
        return false;
    Cull.VobCullRootSig = vobCullRs.RootSig();

    if ( !makeComputePSO( "VobCull.hlsl", "CSCull", vobCullRs,
        Cull.VobCullCsBlob.ReleaseAndGetAddressOf(), Cull.VobCullPSO.ReleaseAndGetAddressOf() ) ) return false;

    // --- Indirect-arg patch root sig: b0 4 consts (count/stride/offsets), t0 counts SRV, u0 raw arg UAV ---
    D3D12RootLayout& patchRs = Layout( "CullPatch" );
    patchRs.AddConstants( 0, 4, D3D12_SHADER_VISIBILITY_ALL );   // 0: b0 VobPatchCB
    patchRs.AddSRV( 0, D3D12_SHADER_VISIBILITY_ALL );            // 1: t0 PatchCounts
    patchRs.AddUAV( 0, D3D12_SHADER_VISIBILITY_ALL );            // 2: u0 PatchArgs (RWByteAddressBuffer)
    if ( !patchRs.Build( device ) )
        return false;
    Cull.PatchRootSig = patchRs.RootSig();

    if ( !makeComputePSO( "VobCull.hlsl", "CSPatchArgs", patchRs,
        Cull.PatchCsBlob.ReleaseAndGetAddressOf(), Cull.PatchPSO.ReleaseAndGetAddressOf() ) ) return false;

    return true;
}


bool D3D12PipelineState::CreateLines() {
    // Debug/editor line lists (D3D12LineRenderer) — port of D3D11's PS_Lines + VS_Lines / VS_Lines_XYZRHW.
    // Drawn after the tonemap resolve, straight onto the swapchain backbuffer, so the vertex colors land
    // in the final LDR image unmodified (no HDR scene target, no tonemap, no fog).
    ID3D12Device* device = m_Device->GetDevice();

    // b0 ViewProj (world-space VS) + b1 viewport pos/size (screen-space VS). One root signature for both
    // PSOs; the draw path sets BOTH parameters every time so neither is ever left stale.
    D3D12RootLayout& rs = Layout( "Lines" );
    rs.AddConstants( 0, 16, D3D12_SHADER_VISIBILITY_VERTEX );   // 0: b0 ViewProj
    // 1: b1 { ViewportPos, ViewportSize } (+ padding to a float4x2)
    rs.AddConstants( 1, 8, D3D12_SHADER_VISIBILITY_VERTEX );

    if ( !rs.Build( device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ) )
        return false;
    Lines.RootSig = rs.RootSig();

    if ( !m_Shaders->CompileFromFile( "Lines.hlsl", "VSMain", Shadermodel_VS, Lines.VsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "Lines.hlsl", "VSScreen", Shadermodel_VS, Lines.ScreenVsBlob.ReleaseAndGetAddressOf() ) )
        return false;
    if ( !m_Shaders->CompileFromFile( "Lines.hlsl", "PSMain", Shadermodel_PS, Lines.PsBlob.ReleaseAndGetAddressOf() ) )
        return false;

    rs.ValidateShaders( {
        { Lines.VsBlob.Get(),       "Lines.hlsl:VSMain",   D3D12_SHADER_VISIBILITY_VERTEX },
        { Lines.ScreenVsBlob.Get(), "Lines.hlsl:VSScreen", D3D12_SHADER_VISIBILITY_VERTEX },
        { Lines.PsBlob.Get(),       "Lines.hlsl:PSMain",   D3D12_SHADER_VISIBILITY_PIXEL  },
    } );

    // BaseLineRenderer's LineVertex: float4 Position @0, float4 Color @16 (32 bytes, #pragma pack(1)).
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = Lines.RootSig.Get();
    pso.VS = { Lines.VsBlob->GetBufferPointer(), Lines.VsBlob->GetBufferSize() };
    pso.PS = { Lines.PsBlob->GetBufferPointer(), Lines.PsBlob->GetBufferSize() };
    pso.InputLayout = { layout, _countof( layout ) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kBackBufferFormat;   // drawn onto the tonemapped swapchain, not the HDR scene target
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // lines have no facing
    pso.RasterizerState.DepthClipEnable = TRUE;

    // Alpha blending — D3D11's line flush forces BlendState.SetAlphaBlending() for both lists.
    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Depth-tested against the finished scene depth (reversed-Z), but never written: the lines are a debug
    // overlay composited after the scene resolve, and nothing samples depth afterwards this frame.
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pso.DepthStencilState.StencilEnable = FALSE;

    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Lines.WorldPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (world-space lines).";
        return false;
    }

    // Screen-space variant: pre-transformed xyzrhw vertices, no depth target at all (D3D11 doesn't bind one
    // for VS_Lines_XYZRHW either — these are 2D overlays from zCRndD3D's hooked DrawLine/DrawLineZ).
    pso.VS = { Lines.ScreenVsBlob->GetBufferPointer(), Lines.ScreenVsBlob->GetBufferSize() };
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.DepthStencilState.DepthEnable = FALSE;
    if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( Lines.ScreenPSO.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: CreateGraphicsPipelineState failed (screen-space lines).";
        return false;
    }
    return true;
}
