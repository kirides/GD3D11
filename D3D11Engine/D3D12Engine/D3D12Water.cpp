// D3D12GraphicsEngine — water surfaces (refraction, sky reflection, screen-space reflections).
//
// The D3D11 spec is D3D11GraphicsEngine::DrawWaterSurfaces (D3D11GraphicsEngine.cpp:5384) plus the shader
// pair VS_ExWater.hlsl / PS_Water.hlsl. Shape of the pass, mirrored here one-to-one:
//
//   1. copy the finished opaque HDR scene into a temp texture     (D3D11: PfxRenderer->CopyTextureToRTV)
//   2. copy the depth buffer                                       (D3D11: CopyDepthStencil)
//   3. Z-prepass: water depth only, color writes off               (D3D11: "DrawWaterSurfaces::ZPrepass")
//   4. color pass: depth-read-only, OPAQUE, per-texture batches    (D3D11: "DrawWaterSurfaces::Refraction")
//
// The critical thing the earlier D3D12 MVP got wrong is step 1/2 and the blend mode. D3D11's water is NOT
// alpha-blended (GothicBlendStateInfo::SetDefault leaves BlendEnabled = false) — the pixel shader composites
// the see-through result itself out of the scene copy, sampled through a distorted UV and darkened by how
// deep the water is at that pixel. Faking that with a constant-alpha blend over a flat texture is what made
// D3D12 water read as a solid sheet with no reflections. See Shaders/D3D12/Water.hlsl.
//
// Divergences from D3D11, both deliberate:
//   * the refraction/reflection inputs are bound BINDLESSLY (SM6.6 ResourceDescriptorHeap) by heap index
//     from the water CB, instead of D3D11's fixed t2..t5 slots.
//   * SSR quality (RendererSettings.WaterSSRQuality) is a runtime uniform loop bound, not D3D11's
//     SSR_QUALITY shader permutation — D3D12 bakes its DXIL at Init() and has no live reload yet, so a
//     permutation would need a game restart to take effect.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12Texture.h"
#include "D3D12VertexBuffer.h"
#include "D3D12PipelineState.h"
#include "D3D12RenderGraph.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../GSky.h"
#include "../ConstantBufferStructs.h"
#include "../DDSFormat.h"
#include "../WorldObjects.h"
#include "../zCTexture.h"
#include "../WaterProfile.h"
#include "../D3D7/MyDirectDrawSurface7.h"
#include "D3D12RayTracing.h"

#include <fstream>
#include <filesystem>

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

// Declared in D3D12EngineCommon.h; filled by BuildWorldDrawCommands (D3D12Scene.cpp), drained here.
std::unordered_map<zCTexture*, std::vector<MeshInfo*>> g_FrameWaterSurfaces;

namespace {
    // b2 of Shaders/D3D12/Water.hlsl. Every row is 16-byte aligned, so the HLSL packing rules place these
    // exactly as declared — no implicit padding on either side.
    struct WaterCBData {
        XMFLOAT4X4 Projection;        // Gothic's projection matrix, verbatim (mul(v,M) == M*v — see CLAUDE.md)
        XMFLOAT4X4 View;              // world->view, verbatim; D3D11 uploads the same matrix untransposed

        float ViewportSize[2];
        float Time;                   // seconds  — distortion scroll   (D3D11: RI_Time / GetTimeSeconds)
        float TotalTime;              // millisec — material UV scroll  (D3D11: M_TotalTime / GetTotalTime)

        XMFLOAT3 CameraPosition;
        float ProjA;                  // HLSL RI_Projection._33 == the CPU matrix's _33
        float ProjB;                  // HLSL RI_Projection._43 == the CPU matrix's _34

        UINT DepthIndex;
        UINT SceneIndex;
        UINT DistortionIndex;
        UINT ReflectionCubeIndex;     // 0xFFFFFFFF => shader skips the static cube
        UINT SsrMaxSteps;             // 0 => SSR off
        UINT SsrRefineSteps;
        UINT UseAtmosphere;           // 0 => skip ApplyAtmosphericScatteringGround (no GSky data)

        UINT CameraUnderwater;        // 1 => no water-body absorption (the column above is air)
        UINT SurfaceDepthIndex;       // depth after the water prepass (shore probes); 0xFFFFFFFF => unavailable
        float OceanClimate;
        float OceanTintStrength;

        XMFLOAT3 MoonDir;             // world space, toward the moon
        float MoonGlint;
        XMFLOAT3 OceanTint;
        float MoonDisc;

        UINT LowCloudIndex;           // premultiplied low cloud layer; 0xFFFFFFFF => none
        float SkyReflection;          // 1 => march the reflected sky in screen space
        UINT SkyAverageIndex;         // 4x1 average on-screen sky; 0xFFFFFFFF => none
        float OceanTexture;           // 0 = pure water body, 1 = legacy-strength texture blend

        UINT RtColorIndex;            // ray-traced reflection result; 0xFFFFFFFF => screen-space reflections
        UINT RtDistanceIndex;
        UINT WaveAnimation;           // 1 => Gerstner swell (D3D11: SHD_WATERANI)
        float RtPad;
    };
    static_assert( sizeof( WaterCBData ) == 272, "WaterCBData must match Water.hlsl's b2 layout" );

    // Resting state of the water copies: the water PS and the sky-average compute pass both read them.
    constexpr D3D12_RESOURCE_STATES kWaterCopyReadState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // b0 of Shaders/D3D12/WaterSkyAverage.hlsl
    struct WaterSkyAverageConsts {
        UINT Size[2];
        float Blend;
        UINT Reset;
        UINT SceneIndex;
        UINT DepthIndex;
        UINT HistoryIndex;
        UINT Pad;
    };
    static_assert( sizeof( WaterSkyAverageConsts ) == 8 * sizeof( UINT ), "WaterSkyAverage root constants must be 8 DWORDs" );
    constexpr float kWaterSkyAverageSeconds = 1.0f;   // history time constant
    constexpr UINT kWaterCbBytes = 768;

    // D3D11's SSR_QUALITY permutation table (PS_Water.hlsl lines 72-81), as runtime loop bounds. Shared
    // with opaque-surface SSR (D3D12AO.cpp) via D3D12EngineCommon.h's SsrStepsForQuality.

    // DDS_HEADER.dwCaps2 bits (the cubemap flags live outside DDSFormat.h's pixel-format tables).
    constexpr uint32_t kDdsCaps2Cubemap = 0x00000200;
    constexpr uint32_t kDdsCaps2CubemapAllFaces = 0x0000FC00;
}


bool D3D12GraphicsEngine::CreateWaterConstantBuffers() {
    // One persistently-mapped UPLOAD buffer per frame-in-flight, 768 B: [0,512) WaterCBData (b2),
    // [512,768) the AtmosphereConstantBuffer (b1). Both root CBV addresses must be 256-byte aligned, hence
    // the split rather than one packed struct. Same pattern as CreateFogConstantBuffers.
    static_assert( sizeof( WaterCBData ) <= kWaterAtmosphereCbOffset,
        "WaterCBData must fit in the first 256-byte block of the water CB" );
    static_assert( sizeof( AtmosphereConstantBuffer ) <= kWaterCbBytes - kWaterAtmosphereCbOffset,
        "AtmosphereConstantBuffer must fit in the second 256-byte block of the water CB" );

    D3D12MA::ALLOCATION_DESC uploadAlloc = {};
    uploadAlloc.HeapType = DefaultUploadHeapType;

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = kWaterCbBytes;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_Rhi->CreateResource( uploadAlloc.HeapType, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_WaterCB[i].ReleaseAndGetAddressOf() ) ) ) {
            Logging::Wrn( "D3D12: failed to create the water constant buffer." );
            return false;
        }
        m_WaterCB[i]->SetName( L"WaterCB" );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( m_WaterCB[i]->Map( 0, &noRead, &mapped ) ) ) return false;
        m_WaterCBMapped[i] = static_cast<uint8_t*>( mapped );
        m_WaterCBGpu[i] = m_WaterCB[i]->GetGPUVirtualAddress();
    }
    return true;
}


bool D3D12GraphicsEngine::CreateWaterSkyAverage() {
    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = 4;
    dd.Height = 1;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_R32_FLOAT;   // typed UAV loads of R32 need no optional format support
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if ( FAILED( m_Rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &dd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        m_WaterSkyAverage.ReleaseAndGetAddressOf(), Rhi::RESOURCE_FLAG_TRACK_LAYOUT ) ) ) {
        return false;
    }
    m_WaterSkyAverage->SetName( L"WaterSkyAverage" );
    m_WaterSkyAverageState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_WaterSkyAverageHistoryValid = false;

    if ( m_WaterSkyAverageSrvSlot == UINT_MAX ) m_WaterSkyAverageSrvSlot = AllocateSrvSlot();
    if ( m_WaterSkyAverageUavSlot == UINT_MAX ) m_WaterSkyAverageUavSlot = AllocateSrvSlot();
    if ( m_WaterSkyAverageSrvSlot == UINT_MAX || m_WaterSkyAverageUavSlot == UINT_MAX ) {
        m_WaterSkyAverage.Reset();
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_Rhi->CreateShaderResourceView( m_WaterSkyAverage.Get(), &srv, GetSrvCpuHandle( m_WaterSkyAverageSrvSlot ) );

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_Rhi->CreateUnorderedAccessView( m_WaterSkyAverage.Get(), nullptr, &uav, GetSrvCpuHandle( m_WaterSkyAverageUavSlot ) );
    return true;
}


/** Folds this frame's on-screen sky into the water's sky-fill history; false when Water.hlsl should not read it. */
bool D3D12GraphicsEngine::UpdateWaterSkyAverage( UINT sceneSrvSlot, UINT depthSrvSlot ) {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( !m_WaterSkyAverage || !m_Pipelines.WaterSkyAverage.PSO
        || settings.WaterSSRQuality == GothicRendererSettings::WATER_SSR_DISABLED
        || settings.WaterReflectionMode != GothicRendererSettings::WATER_REFLECTION_GEOMETRY_SKY ) {
        return false;
    }

    DX_ZONE( m_CmdList.Get(), "Water Sky Average" );
    if ( m_WaterSkyAverageState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) {
        m_CmdList->TransitionBarriers( { { m_WaterSkyAverage.Get(), m_WaterSkyAverageState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } } );
    }

    WaterSkyAverageConsts c = {};
    c.Size[0] = static_cast<UINT>( m_Resolution.x );
    c.Size[1] = static_cast<UINT>( m_Resolution.y );
    c.Blend = 1.0f - std::exp( -Engine::GAPI->GetDeltaTime() / kWaterSkyAverageSeconds );
    c.Reset = m_WaterSkyAverageHistoryValid ? 0u : 1u;
    c.SceneIndex = sceneSrvSlot;
    c.DepthIndex = depthSrvSlot;
    c.HistoryIndex = m_WaterSkyAverageUavSlot;
    m_CmdList->SetPipelineState( m_Pipelines.WaterSkyAverage.PSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.WaterSkyAverage.RootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 8, &c, 0 );
    m_CmdList->Dispatch( 1, 1, 1 );

    m_CmdList->TransitionBarriers( { { m_WaterSkyAverage.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE } } );
    m_WaterSkyAverageState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_WaterSkyAverageHistoryValid = true;
    return true;
}


bool D3D12GraphicsEngine::LoadReflectionCube() {
    // Same file D3D11 loads at D3D11GraphicsEngine.cpp:848 (via DirectXTK's CreateDDSTextureFromFile, which
    // recognises the cubemap caps bits). D3D12Texture is Texture2D-only, so the 6-face DDS is parsed here
    // and given a real D3D12_SRV_DIMENSION_TEXTURECUBE view. Non-fatal: on failure the water shader gets
    // 0xFFFFFFFF for ReflectionCubeIndex and simply renders with SSR/refraction only.
    Rhi::Device* device = m_Rhi.Get();
    if ( !device ) return false;

    const std::string path = Engine::GAPI->GetStartDirectory() + "\\system\\GD3D11\\Textures\\reflect_cube.dds";
    std::vector<uint8_t> bytes;
    {
        std::ifstream f( path, std::ios::binary | std::ios::ate );
        if ( !f ) {
            Logging::Wrn( "D3D12: reflection cube not found ({}) — water will reflect only on-screen geometry (SSR).", path );
            return false;
        }
        const std::streamsize sz = f.tellg();
        if ( sz < 128 ) return false;
        f.seekg( 0, std::ios::beg );
        bytes.resize( static_cast<size_t>( sz ) );
        if ( !f.read( reinterpret_cast<char*>( bytes.data() ), sz ) ) return false;
    }

    auto rd = [&]( size_t off ) -> uint32_t { uint32_t v; memcpy( &v, bytes.data() + off, 4 ); return v; };
    if ( rd( 0 ) != DDS::Magic ) {
        Logging::Wrn( "D3D12: reflect_cube.dds is not a DDS file — water sky reflection disabled." );
        return false;
    }

    const uint32_t height = rd( 12 );
    const uint32_t width = rd( 16 );
    uint32_t mips = rd( 28 );
    if ( mips == 0 ) mips = 1;
    const uint32_t caps2 = rd( 112 );

    const uint32_t pfFlags = rd( 80 );
    const uint32_t fourCC = rd( 84 );
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    size_t dataOffset = 128;
    if ( pfFlags & DDS::FlagFourCC ) {
        if ( fourCC == DDS::Dx10 ) {
            if ( bytes.size() < 148 ) return false;
            fmt = static_cast<DXGI_FORMAT>( rd( 128 ) );   // DDS_HEADER_DXT10.dxgiFormat
            dataOffset = 148;
        } else {
            fmt = DDS::FromFourCC( fourCC );
        }
    } else {
        fmt = DDS::FromPixelFormat( pfFlags, rd( 88 ), rd( 92 ), rd( 96 ), rd( 100 ), rd( 104 ) );
    }
    if ( fmt == DXGI_FORMAT_UNKNOWN || ( DDS::BCBlockBytes( fmt ) == 0 && DDS::BitsPerPixel( fmt ) == 0 ) ) {
        Logging::Wrn( "D3D12: unsupported pixel format in reflect_cube.dds — water sky reflection disabled." );
        return false;
    }
    // The DX10-extended header can also flag the cube via miscFlag (0x4); accept either signalling.
    const bool isCube = ( ( caps2 & kDdsCaps2Cubemap ) && ( caps2 & kDdsCaps2CubemapAllFaces ) == kDdsCaps2CubemapAllFaces )
        || ( dataOffset == 148 && ( rd( 136 ) & 0x4 ) != 0 );
    if ( !isCube || width == 0 || height == 0 ) {
        Logging::Wrn( "D3D12: reflect_cube.dds is not a complete 6-face cubemap — water sky reflection disabled." );
        return false;
    }

    // DDS cubemaps store the faces in +X,-X,+Y,-Y,+Z,-Z order, each face carrying its full mip chain —
    // exactly D3D12's subresource ordering (arraySlice * mipLevels + mip), so the payload maps 1:1.
    std::vector<D3D12_SUBRESOURCE_DATA> subs;
    subs.reserve( static_cast<size_t>( 6 ) * mips );
    size_t offset = dataOffset;
    for ( uint32_t face = 0; face < 6; ++face ) {
        for ( uint32_t m = 0; m < mips; ++m ) {
            const uint32_t w = std::max( 1u, width >> m );
            const uint32_t h = std::max( 1u, height >> m );
            const uint32_t rowPitch = DDS::RowPitch( fmt, w );
            const uint32_t surfaceBytes = DDS::SurfaceBytes( fmt, w, h );
            if ( offset + surfaceBytes > bytes.size() ) {
                Logging::Wrn( "D3D12: reflect_cube.dds is truncated (face {} mip {}) — water sky reflection disabled.", face, m );
                return false;
            }
            subs.push_back( { bytes.data() + offset, static_cast<LONG_PTR>( rowPitch ), static_cast<LONG_PTR>( surfaceBytes ) } );
            offset += surfaceBytes;
        }
    }

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 6;
    td.MipLevels = static_cast<UINT16>( mips );
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    // COMMON, like the rain texture arrays: the copy-queue upload promotes to COPY_DEST implicitly and the
    // later SRV read promotes back, so no explicit barrier is needed on either side.
    if ( FAILED( m_Rhi->CreateResource( heapDefault.HeapType, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, m_ReflectionCube.ReleaseAndGetAddressOf() ) ) ) {
        Logging::Wrn( "D3D12: failed to create the reflection cube resource." );
        return false;
    }
    m_ReflectionCube->SetName( L"WaterReflectionCube" );

    if ( !UploadTextureSubresources( m_ReflectionCube.Get(), subs.data(), static_cast<UINT>( subs.size() ) ) ) {
        Logging::Wrn( "D3D12: failed to upload the reflection cube — water sky reflection disabled." );
        m_ReflectionCube.Reset();
        return false;
    }

    m_ReflectionCubeSrvSlot = AllocateSrvSlot();
    if ( m_ReflectionCubeSrvSlot == UINT_MAX ) {
        Logging::Wrn( "D3D12: SRV heap exhausted allocating a slot for the reflection cube." );
        m_ReflectionCube.Reset();
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = fmt;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCube.MipLevels = mips;
    m_Rhi->CreateShaderResourceView( m_ReflectionCube.Get(), &srv, GetSrvCpuHandle( m_ReflectionCubeSrvSlot ) );
    return true;
}


void D3D12GraphicsEngine::DrawWaterSurfaces() {
    if ( !m_FrameOpen || !m_Pipelines.Water.PSO || !m_Pipelines.Water.RootSig || !m_DepthBuffer || g_FrameWaterSurfaces.empty() )
        return;

    DX_ZONE( m_CmdList.Get(), "DrawWaterSurfaces" );

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

    const D3D12_CPU_DESCRIPTOR_HANDLE mainDsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();

    // ---------------------------------------------------------------------------------------------------
    // Steps 1+2: snapshot the finished opaque scene and its depth, BEFORE the Z-prepass below writes the
    // water surface's own depth. Getting the order wrong would have the refraction read water-vs-water
    // (shallowDepth collapses to 0 everywhere and the surface goes flat/black). Both copies are graph-managed
    // transients (see the header comment on the old m_WaterSceneCopy/m_WaterDepthCopy members) — this
    // function is a BaseGraphicsEngine override with a fixed signature, so it builds its own small LOCAL
    // D3D12RenderGraph rather than taking a shared one (safe: D3D12AliasedTextureArena::ReserveNamedRange
    // dedups by name across the whole arena, not per-graph-instance).
    // ---------------------------------------------------------------------------------------------------
    UINT waterSceneSrvSlot = UINT_MAX;
    UINT waterDepthSrvSlot = UINT_MAX;
    bool copiesReady = false;
    bool skyAverageReady = false;
    if ( m_WaterCBMapped[m_FrameIndex] ) {
        D3D12RenderGraph waterGraph( &m_AliasArena );
        RGResourceHandle sceneHandle = RG_INVALID_HANDLE;
        RGResourceHandle depthHandle = RG_INVALID_HANDLE;

        waterGraph.AddPass( RG_PASS_NAME( "Water Copy" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
            // CreateTexture()'s state param gets both into COPY_DEST automatically before this callback
            // runs, so the callback itself never needs to check/transition scene->State or depth->State on
            // entry — only m_SceneColor/m_DepthBuffer (not graph-tracked) still need a manual transition.
            sceneHandle = builder.CreateTexture( { static_cast<uint32_t>( m_Resolution.x ), static_cast<uint32_t>( m_Resolution.y ),
                static_cast<int>( kSceneColorFormat ), L"WaterSceneCopy", 0u }, D3D12_RESOURCE_STATE_COPY_DEST );
            // Plain R32_FLOAT, not R32_TYPELESS+ALLOW_DEPTH_STENCIL: CopyResource only needs format-FAMILY
            // compatibility (R32_TYPELESS and R32_FLOAT share one) and this is never bound as a real depth
            // target — see the header comment.
            depthHandle = builder.CreateTexture( { static_cast<uint32_t>( m_Resolution.x ), static_cast<uint32_t>( m_Resolution.y ),
                static_cast<int>( DXGI_FORMAT_R32_FLOAT ), L"WaterDepthCopy", 0u }, D3D12_RESOURCE_STATE_COPY_DEST );
            // Nothing ever Read()s either handle (both leave the graph via waterSceneSrvSlot/
            // waterDepthSrvSlot, plain locals read back further down) — mark the side effect explicitly.
            builder.MarkExternalEffect();

            pass.m_executeCallback = [this, sceneHandle, depthHandle]( const D3D12RenderGraph& g, D3D12CmdList& cmdList ) {
                D3D12RenderTarget* scene = g.GetPhysicalTexture( sceneHandle );
                D3D12RenderTarget* depth = g.GetPhysicalTexture( depthHandle );
                if ( !scene || !depth ) return;

                // Both sources must leave their bound states, so drop the render targets first.
                cmdList.OMSetRenderTargets( 0, nullptr, FALSE, nullptr );

                const D3D12_RESOURCE_STATES sceneFrom = m_SceneColorInPixelState
                    ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET;
                cmdList.TransitionBarriers( {
                    { m_SceneColor.Get(), sceneFrom, D3D12_RESOURCE_STATE_COPY_SOURCE },
                    { m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE },
                    } );

                cmdList.CopyResource( scene->GetResource(), m_SceneColor.Get() );
                cmdList.CopyResource( depth->GetResource(), m_DepthBuffer.Get() );

                cmdList.TransitionBarriers( {
                    { m_SceneColor.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET },
                    { m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE },
                    { scene->GetResource(), D3D12_RESOURCE_STATE_COPY_DEST, kWaterCopyReadState },
                    { depth->GetResource(), D3D12_RESOURCE_STATE_COPY_DEST, kWaterCopyReadState },
                    } );
                scene->State = kWaterCopyReadState;
                depth->State = kWaterCopyReadState;
                m_SceneColorInPixelState = false;   // the scene target is back to RENDER_TARGET regardless of before
                };
            } );

        waterGraph.Compile();
        waterGraph.Execute( m_CmdList );

        // Execute() has already run by this point (this is a local, synchronous graph — see the header
        // comment), so the physical textures it placed are valid to query immediately, unlike the deferred
        // postFxGraph pattern DoF/SMAA/etc. use.
        if ( D3D12RenderTarget* scene = waterGraph.GetPhysicalTexture( sceneHandle ) ) {
            if ( D3D12RenderTarget* depth = waterGraph.GetPhysicalTexture( depthHandle ) ) {
                waterSceneSrvSlot = scene->GetSrvSlot();
                waterDepthSrvSlot = depth->GetSrvSlot();
                copiesReady = true;
            }
        }

        // Re-bind what the geometry passes had: HDR scene RTV + the main DSV.
        m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, &mainDsv );
        if ( copiesReady ) skyAverageReady = UpdateWaterSkyAverage( waterSceneSrvSlot, waterDepthSrvSlot );
    }

    // --- Water constant buffer (b2) + the atmosphere block (b1) -----------------------------------------
    if ( copiesReady ) {
        auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

        WaterCBData cb = {};
        cb.Projection = projM;
        cb.View = viewM;
        cb.ViewportSize[0] = static_cast<float>( m_Resolution.x );
        cb.ViewportSize[1] = static_cast<float>( m_Resolution.y );
        cb.Time = Engine::GAPI->GetTimeSeconds();
        cb.TotalTime = Engine::GAPI->GetTotalTime();
        cb.CameraPosition = Engine::GAPI->GetCameraPosition();
        // HLSL reads the verbatim-uploaded row-major matrix column-major, so its _33/_43 are the CPU
        // matrix's _33/_34 — pass them explicitly rather than relying on that transposition being obvious.
        cb.ProjA = projM._33;
        cb.ProjB = projM._34;
        cb.DepthIndex = waterDepthSrvSlot;
        cb.SceneIndex = waterSceneSrvSlot;
        // The distortion texture drives every wave normal in the shader. If it failed to load, fall back to
        // the 1x1 black texture: the distortion decode (x*2-1) then yields a constant vector, so the water
        // renders with static (unanimated) waves instead of not at all.
        cb.DistortionIndex = ( m_DistortionTexture && m_DistortionTexture->HasSRV() )
            ? m_DistortionTexture->GetSrvSlot() : m_BlackTexture->GetSrvSlot();
        cb.ReflectionCubeIndex = m_ReflectionCubeSrvSlot;   // UINT_MAX => shader skips the cube
        SsrStepsForQuality( settings.WaterSSRQuality, cb.SsrMaxSteps, cb.SsrRefineSteps );
        // Underwater the trace is invalid — it assumes the eye sits above the surface, so from below it
        // marches up through the water body and mirrors the shoreline over the underwater view. 0 steps
        // is the shader's "SSR off" path (Water.hlsl line 323), same effect as D3D11's RI_SSREnabled=0.
        if ( Engine::GAPI->IsUnderWater() ) {
            cb.SsrMaxSteps = 0;
            cb.SsrRefineSteps = 0;
            cb.CameraUnderwater = 1;
        }
        cb.SurfaceDepthIndex = UINT_MAX;   // patched after the prepass copy below
        cb.RtColorIndex = UINT_MAX;        // patched after the ray-traced reflections below
        cb.RtDistanceIndex = UINT_MAX;
        cb.LowCloudIndex = m_LowCloudLayerSrvSlot;   // set by GenerateLowClouds this frame, UINT_MAX without clouds

        const OceanProfile ocean = GetOceanProfile();
        cb.OceanClimate = ocean.Climate;
        cb.OceanTintStrength = ocean.TintStrength;
        cb.OceanTint = ocean.Tint;
        cb.OceanTexture = ocean.TextureStrength;
        cb.SkyReflection = WaterSkyReflectionEnabled();
        cb.SkyAverageIndex = skyAverageReady ? m_WaterSkyAverageSrvSlot : UINT_MAX;
        // Same build gate as D3D11's SHD_WATERANI. The Z-prepass shares the VS, so its depth moves with the swell.
#ifdef BUILD_GOTHIC_2_6_fix
        cb.WaveAnimation = settings.EnableWaterAnimation ? 1u : 0u;
#endif

        // GSky::RenderSky() refreshes the AC_* constants every frame (DrawSky runs before this), even though
        // D3D12 renders Gothic's fixed-function sky — same reasoning as RenderFogAndGodRays. Without them the
        // scattering math would divide by a zeroed wavelength/radius set, so the shader skips it instead.
        GSky* sky = Engine::GAPI->GetSky();
        if ( sky ) {
            const MoonLightInfo moon = sky->GetMoonLight();
            cb.MoonDir = moon.Direction;
            cb.MoonGlint = moon.GlintVisibility;
            cb.MoonDisc = moon.DiscVisibility;
            const auto& atmo = sky->GetAtmosphereCB();
            memcpy( m_WaterCBMapped[m_FrameIndex] + kWaterAtmosphereCbOffset, &atmo, sizeof( atmo ) );
            cb.UseAtmosphere = 1;
        } else {
            memset( m_WaterCBMapped[m_FrameIndex] + kWaterAtmosphereCbOffset, 0, sizeof( AtmosphereConstantBuffer ) );
        }

        memcpy( m_WaterCBMapped[m_FrameIndex], &cb, sizeof( cb ) );
    }

    m_CmdList->SetGraphicsRootSignature( m_Pipelines.Water.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    if ( copiesReady ) {
        m_CmdList->SetGraphicsRootConstantBufferView( 2, m_WaterCBGpu[m_FrameIndex] );                                  // b2 water
        m_CmdList->SetGraphicsRootConstantBufferView( 3, m_WaterCBGpu[m_FrameIndex] + kWaterAtmosphereCbOffset );       // b1 atmosphere
    }

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
    D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    m_CmdList->IASetIndexBuffer( &ibv );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_GPU_DESCRIPTOR_HANDLE blackSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    // Bind a dummy diffuse for the whole call: the depth prepass' PS reads nothing, but root parameter 1
    // must still be initialized before any draw on this root signature. The color loop below rebinds it.
    m_CmdList->SetGraphicsRootDescriptorTable( 1, blackSrv );
    m_CmdList->SetGraphicsRoot32BitConstant( 4, 0u, 0 );   // b3 IsOcean, set per batch in the color loop

    // === Z-Prepass === (mirrors D3D11's DrawWaterSurfaces::ZPrepass)
    // The color pass below is depth-read-only, so without this the main depth buffer would still hold the
    // geometry BEHIND the water surface (or the reversed-Z far plane over open ocean) at every water pixel.
    // Everything downstream that reconstructs a world position from depth — height fog and the god-ray mask
    // (RenderFogAndGodRays) — would then fog the sea floor / sky rather than the water surface, which is why
    // the fog visibly breaks at the ocean without this pass. Same VB/IB/root constants; only the PSO differs
    // (color writes masked, depth-write on).
    if ( m_Pipelines.Water.DepthPrepassPSO ) {
        DX_ZONE( m_CmdList.Get(), "Water Z-Prepass" );
        m_CmdList->SetPipelineState( m_Pipelines.Water.DepthPrepassPSO.Get() );
        for ( auto const& [tex, meshes] : g_FrameWaterSurfaces ) {
            for ( MeshInfo* mesh : meshes ) {
                if ( !mesh || mesh->Indices.empty() ) continue;
                m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 1,
                    mesh->BaseIndexLocation, 0, 0 );
            }
        }
    }

    // Without the scene/depth copies the refraction PS would read unbound descriptors, so skip the color
    // pass entirely (allocation failure only). The Z-prepass above still ran, so height fog stays correct
    // and the water pixels simply show the opaque scene underneath — a degradation, not a corruption.
    if ( !copiesReady ) {
        static bool warned = false;
        if ( !warned ) { warned = true; Logging::Wrn( "D3D12: water refraction resources unavailable — water surfaces will not be shaded." ); }
        g_FrameWaterSurfaces.clear();
        return;
    }

    UINT surfaceDepthSrvSlot = UINT_MAX;
    // Depth with the water surfaces in it, for the shore probes' coverage test. The color pass keeps the
    // writable DSV bound, so it reads a copy rather than the live buffer.
    if ( m_Pipelines.Water.DepthPrepassPSO ) {
        D3D12RenderGraph surfaceGraph( &m_AliasArena );
        RGResourceHandle surfaceHandle = RG_INVALID_HANDLE;
        surfaceGraph.AddPass( RG_PASS_NAME( "Water Surface Depth Copy" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
            surfaceHandle = builder.CreateTexture( { static_cast<uint32_t>( m_Resolution.x ), static_cast<uint32_t>( m_Resolution.y ),
                static_cast<int>( DXGI_FORMAT_R32_FLOAT ), L"WaterSurfaceDepthCopy", 0u }, D3D12_RESOURCE_STATE_COPY_DEST );
            builder.MarkExternalEffect();

            pass.m_executeCallback = [this, surfaceHandle]( const D3D12RenderGraph& g, D3D12CmdList& cmdList ) {
                D3D12RenderTarget* surface = g.GetPhysicalTexture( surfaceHandle );
                if ( !surface ) return;
                cmdList.OMSetRenderTargets( 0, nullptr, FALSE, nullptr );
                cmdList.TransitionBarriers( {
                    { m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE },
                    } );
                cmdList.CopyResource( surface->GetResource(), m_DepthBuffer.Get() );
                cmdList.TransitionBarriers( {
                    { m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE },
                    { surface->GetResource(), D3D12_RESOURCE_STATE_COPY_DEST, kWaterCopyReadState },
                    } );
                surface->State = kWaterCopyReadState;
                };
            } );
        surfaceGraph.Compile();
        surfaceGraph.Execute( m_CmdList );

        // The CB is plain upload memory the GPU reads at execution, so patching it after recording the prepass is safe.
        if ( D3D12RenderTarget* surface = surfaceGraph.GetPhysicalTexture( surfaceHandle ) ) {
            surfaceDepthSrvSlot = surface->GetSrvSlot();
            reinterpret_cast<WaterCBData*>( m_WaterCBMapped[m_FrameIndex] )->SurfaceDepthIndex = surfaceDepthSrvSlot;
        }
        m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, &mainDsv );
    }

    // Ray-traced reflections replace the screen-space march; the pixel shader falls back to SSR without them.
    const int rtQuality = Engine::GAPI->GetRendererState().RendererSettings.WaterRayTracing;
    if ( m_RayTracing && rtQuality != GothicRendererSettings::WATER_RT_OFF && surfaceDepthSrvSlot != UINT_MAX
        && !Engine::GAPI->IsUnderWater() && m_RayTracing->EnsureScene( RtSceneVobRadius(), m_RtPointShadowsActive ) ) {
        D3D12RayTracing::Inputs in = {};
        in.SurfaceDepthSlot = surfaceDepthSrvSlot;
        in.SceneDepthSlot = waterDepthSrvSlot;
        in.SceneColorSlot = waterSceneSrvSlot;
        in.DistortionSlot = ( m_DistortionTexture && m_DistortionTexture->HasSRV() )
            ? m_DistortionTexture->GetSrvSlot() : m_BlackTexture->GetSrvSlot();   // same choice as the water CB
        in.View = viewM;
        in.Projection = projM;
        in.CameraPosition = Engine::GAPI->GetCameraPosition();
        in.Time = Engine::GAPI->GetTimeSeconds();
        in.Quality = rtQuality;
        in.ScreenSpace = Engine::GAPI->GetRendererState().RendererSettings.WaterRayTracingScreenSpace;
        UINT colorSlot = UINT_MAX, distanceSlot = UINT_MAX;
        if ( m_RayTracing->TraceWater( in, colorSlot, distanceSlot ) ) {
            WaterCBData* patch = reinterpret_cast<WaterCBData*>( m_WaterCBMapped[m_FrameIndex] );
            patch->RtColorIndex = colorSlot;
            patch->RtDistanceIndex = distanceSlot;
        }
        // The trace left a compute root signature and PSO bound; the color pass below rebinds its graphics state.
        m_CmdList->SetGraphicsRootSignature( m_Pipelines.Water.RootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->SetGraphicsRootConstantBufferView( 2, m_WaterCBGpu[m_FrameIndex] );
        m_CmdList->SetGraphicsRootConstantBufferView( 3, m_WaterCBGpu[m_FrameIndex] + kWaterAtmosphereCbOffset );
    }

    m_CmdList->SetPipelineState( m_Pipelines.Water.PSO.Get() );
    unsigned int drawnIndices = 0;
    for ( auto const& [tex, meshes] : g_FrameWaterSurfaces ) {
        D3D12_GPU_DESCRIPTOR_HANDLE srv = blackSrv;
        if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
            if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
                if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                    D3D12Texture* d12 = D3D12Texture::From( gfx );
                    if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                }
            }
        }
        m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );
        m_CmdList->SetGraphicsRoot32BitConstant( 4, IsOceanWaterTexture( tex ) ? 1u : 0u, 0 );
        for ( MeshInfo* mesh : meshes ) {
            if ( !mesh || mesh->Indices.empty() ) continue;
            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 1,
                mesh->BaseIndexLocation, 0, 0 );
            drawnIndices += static_cast<unsigned int>( mesh->Indices.size() );
        }
    }

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnIndices / 3;
    g_FrameWaterSurfaces.clear();
}
