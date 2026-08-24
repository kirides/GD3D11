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
#include "../D3D7/MyDirectDrawSurface7.h"

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

        // --- Raytraced reflection PoC (see EnsureWaterReflectionAS) ---
        UINT TlasIndex;               // RAYTRACING_ACCELERATION_STRUCTURE SRV; 0xFFFFFFFF => RT path off
        UINT WorldVbIndex;            // raw ByteAddressBuffer SRV over the same world VB the BLAS was built from
        UINT WorldIbIndex;            // raw ByteAddressBuffer SRV over the same world IB (R32_UINT indices)
        UINT _Pad0;                   // keeps the struct a multiple of 16 bytes
    };
    static_assert( sizeof( WaterCBData ) == 208, "WaterCBData must match Water.hlsl's b2 layout" );

    // Resting state of both water copies. PIXEL_SHADER_RESOURCE (not the combined NON_PIXEL|PIXEL the fog
    // pass uses) because only the water PS ever reads them.
    constexpr D3D12_RESOURCE_STATES kWaterCopyReadState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // D3D11's SSR_QUALITY permutation table (PS_Water.hlsl lines 72-81), as runtime loop bounds.
    void SsrStepsForQuality( int quality, UINT& maxSteps, UINT& refineSteps ) {
        switch ( quality ) {
        case GothicRendererSettings::WATER_SSR_LOW:    maxSteps = 12; refineSteps = 4; break;
        case GothicRendererSettings::WATER_SSR_MEDIUM: maxSteps = 24; refineSteps = 5; break;
        case GothicRendererSettings::WATER_SSR_HIGH:   maxSteps = 48; refineSteps = 6; break;
        default:                                       maxSteps = 0;  refineSteps = 0; break;
        }
    }

    // DDS_HEADER.dwCaps2 bits (the cubemap flags live outside DDSFormat.h's pixel-format tables).
    constexpr uint32_t kDdsCaps2Cubemap = 0x00000200;
    constexpr uint32_t kDdsCaps2CubemapAllFaces = 0x0000FC00;
}


bool D3D12GraphicsEngine::CreateWaterConstantBuffers() {
    // One persistently-mapped UPLOAD buffer per frame-in-flight, 512 B: [0,256) WaterCBData (b2),
    // [256,512) the AtmosphereConstantBuffer (b1). Both root CBV addresses must be 256-byte aligned, hence
    // the split rather than one packed struct. Same pattern as CreateFogConstantBuffers.
    static_assert( sizeof( WaterCBData ) <= kWaterAtmosphereCbOffset,
        "WaterCBData must fit in the first 256-byte block of the water CB" );
    static_assert( sizeof( AtmosphereConstantBuffer ) <= 512 - kWaterAtmosphereCbOffset,
        "AtmosphereConstantBuffer must fit in the second 256-byte block of the water CB" );

    D3D12MA::ALLOCATION_DESC uploadAlloc = {};
    uploadAlloc.HeapType = DefaultUploadHeapType;

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = 512;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_Allocator->CreateResource( &uploadAlloc, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_WaterCBAlloc[i].ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_WaterCB[i].ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the water constant buffer.";
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


bool D3D12GraphicsEngine::LoadReflectionCube() {
    // Same file D3D11 loads at D3D11GraphicsEngine.cpp:848 (via DirectXTK's CreateDDSTextureFromFile, which
    // recognises the cubemap caps bits). D3D12Texture is Texture2D-only, so the 6-face DDS is parsed here
    // and given a real D3D12_SRV_DIMENSION_TEXTURECUBE view. Non-fatal: on failure the water shader gets
    // 0xFFFFFFFF for ReflectionCubeIndex and simply renders with SSR/refraction only.
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    const std::string path = Engine::GAPI->GetStartDirectory() + "\\system\\GD3D11\\Textures\\reflect_cube.dds";
    std::vector<uint8_t> bytes;
    {
        std::ifstream f( path, std::ios::binary | std::ios::ate );
        if ( !f ) {
            LogWarn() << "D3D12: reflection cube not found (" << path << ") — water will reflect only on-screen geometry (SSR).";
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
        LogWarn() << "D3D12: reflect_cube.dds is not a DDS file — water sky reflection disabled.";
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
        LogWarn() << "D3D12: unsupported pixel format in reflect_cube.dds — water sky reflection disabled.";
        return false;
    }
    // The DX10-extended header can also flag the cube via miscFlag (0x4); accept either signalling.
    const bool isCube = ( ( caps2 & kDdsCaps2Cubemap ) && ( caps2 & kDdsCaps2CubemapAllFaces ) == kDdsCaps2CubemapAllFaces )
        || ( dataOffset == 148 && ( rd( 136 ) & 0x4 ) != 0 );
    if ( !isCube || width == 0 || height == 0 ) {
        LogWarn() << "D3D12: reflect_cube.dds is not a complete 6-face cubemap — water sky reflection disabled.";
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
                LogWarn() << "D3D12: reflect_cube.dds is truncated (face " << face << " mip " << m << ") — water sky reflection disabled.";
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
    if ( FAILED( m_Allocator->CreateResource( &heapDefault, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
        m_ReflectionCubeAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_ReflectionCube.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: failed to create the reflection cube resource.";
        return false;
    }
    m_ReflectionCube->SetName( L"WaterReflectionCube" );

    if ( !UploadTextureSubresources( m_ReflectionCube.Get(), subs.data(), static_cast<UINT>( subs.size() ) ) ) {
        LogWarn() << "D3D12: failed to upload the reflection cube — water sky reflection disabled.";
        m_ReflectionCube.Reset();
        m_ReflectionCubeAlloc.Reset();
        return false;
    }

    m_ReflectionCubeSrvSlot = AllocateSrvSlot();
    if ( m_ReflectionCubeSrvSlot == UINT_MAX ) {
        LogWarn() << "D3D12: SRV heap exhausted allocating a slot for the reflection cube.";
        m_ReflectionCube.Reset();
        m_ReflectionCubeAlloc.Reset();
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = fmt;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCube.MipLevels = mips;
    m_Device.GetDevice()->CreateShaderResourceView( m_ReflectionCube.Get(), &srv, GetSrvCpuHandle( m_ReflectionCubeSrvSlot ) );
    return true;
}


namespace {
    // One-time buffer alloc for the AS/scratch resources below: DEFAULT heap, UAV-capable (both the
    // acceleration-structure result and its scratch buffer are written by the GPU build). AS result
    // buffers must be created directly in RAYTRACING_ACCELERATION_STRUCTURE state per the DXR spec; that
    // holds regardless of enhanced-barrier support, so this always goes through the legacy CreateResource
    // (same as m_WaterCB above) rather than D3D12ResourceCreate::CreateTexture.
    bool CreateAsBuffer( D3D12MA::Allocator* allocator, UINT64 sizeBytes, D3D12_RESOURCE_STATES state,
        ComPtr<ID3D12Resource>& outRes, ComPtr<D3D12MA::Allocation>& outAlloc, const wchar_t* name ) {
        D3D12MA::ALLOCATION_DESC allocDesc = {};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if ( FAILED( allocator->CreateResource( &allocDesc, &desc, state, nullptr,
            outAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( outRes.ReleaseAndGetAddressOf() ) ) ) )
            return false;
        outRes->SetName( name );
        return true;
    }

    // Raw ByteAddressBuffer SRV over an existing static geometry buffer (world VB or IB), for the hit
    // shader's vertex/index fetch in Water.hlsl. NumElements is in 4-byte units per D3D12_BUFFER_SRV_FLAG_RAW.
    D3D12_SHADER_RESOURCE_VIEW_DESC RawBufferSrvDesc( UINT sizeInBytes ) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = sizeInBytes / 4;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        return srv;
    }
}


bool D3D12GraphicsEngine::EnsureWaterReflectionAS() {
    if ( !m_WaterRaytracingSupported || !m_FrameOpen || !m_CmdList )
        return false;

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    if ( !wm || !wm->GetMeshVertexBuffer() || !wm->GetMeshIndexBuffer() )
        return false;

    D3D12VertexBuffer* vb = D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() );
    D3D12VertexBuffer* ib = D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() );
    if ( !vb->GetResource() || !ib->GetResource() )
        return false;

    // Already built for this exact world mesh (the common case — every frame after the first).
    if ( m_WaterAsBuilt && m_WaterAsBuiltFromVb == wm->GetMeshVertexBuffer() && m_WaterAsBuiltFromIb == wm->GetMeshIndexBuffer() )
        return true;

    ID3D12GraphicsCommandList4* cmdList4 = m_CmdList.List4();
    ComPtr<ID3D12Device5> device5;
    if ( !cmdList4 || FAILED( m_Device.GetDevice()->QueryInterface( IID_PPV_ARGS( device5.GetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: device reported raytracing Tier 1.1 but ID3D12GraphicsCommandList4/ID3D12Device5 "
                      "are unavailable — water RT reflections disabled for this session.";
        m_WaterRaytracingSupported = false;
        return false;
    }

    const UINT vertexCount = vb->GetSizeInBytes() / sizeof( ExVertexStructGPU );
    const UINT indexCount = ib->GetSizeInBytes() / sizeof( uint32_t );
    if ( vertexCount == 0 || indexCount == 0 )
        return false;

    D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    // OPAQUE: alpha-tested/foliage world materials aren't in the wrapped mesh's water-relevant surfaces
    // anyway, and skipping any-hit shading keeps this PoC to inline ray tracing's simplest path.
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.Transform3x4 = 0;
    geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;   // ExVertexStructGPU::Position, offset 0
    geometry.Triangles.IndexCount = indexCount;
    geometry.Triangles.VertexCount = vertexCount;
    geometry.Triangles.IndexBuffer = ib->GetGpuVirtualAddress();
    geometry.Triangles.VertexBuffer.StartAddress = vb->GetGpuVirtualAddress();
    geometry.Triangles.VertexBuffer.StrideInBytes = sizeof( ExVertexStructGPU );

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs = {};
    blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    blasInputs.NumDescs = 1;
    blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blasInputs.pGeometryDescs = &geometry;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasPrebuild = {};
    device5->GetRaytracingAccelerationStructurePrebuildInfo( &blasInputs, &blasPrebuild );
    if ( blasPrebuild.ResultDataMaxSizeInBytes == 0 ) {
        LogWarn() << "D3D12: GetRaytracingAccelerationStructurePrebuildInfo returned an empty BLAS — water RT reflections disabled.";
        return false;
    }

    if ( !CreateAsBuffer( m_Allocator.Get(), blasPrebuild.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            m_WaterBlas, m_WaterBlasAlloc, L"WaterReflectionBLAS" )
        || !CreateAsBuffer( m_Allocator.Get(), blasPrebuild.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            m_WaterBlasScratch, m_WaterBlasScratchAlloc, L"WaterReflectionBLASScratch" ) ) {
        LogWarn() << "D3D12: failed to allocate the water reflection BLAS buffers.";
        return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasBuild = {};
    blasBuild.Inputs = blasInputs;
    blasBuild.DestAccelerationStructureData = m_WaterBlas->GetGPUVirtualAddress();
    blasBuild.ScratchAccelerationStructureData = m_WaterBlasScratch->GetGPUVirtualAddress();
    cmdList4->BuildRaytracingAccelerationStructure( &blasBuild, 0, nullptr );
    m_CmdList->UAVBarrier( m_WaterBlas.Get() );   // the TLAS build below reads the BLAS the line above just wrote

    // --- TLAS: a single identity instance wrapping the BLAS above -------------------------------------
    D3D12_RAYTRACING_INSTANCE_DESC instance = {};
    instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1.0f;
    instance.InstanceMask = 0xFF;
    instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
    instance.AccelerationStructure = m_WaterBlas->GetGPUVirtualAddress();

    {
        D3D12MA::ALLOCATION_DESC uploadAlloc = {};
        uploadAlloc.HeapType = DefaultUploadHeapType;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeof( D3D12_RAYTRACING_INSTANCE_DESC );
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if ( FAILED( m_Allocator->CreateResource( &uploadAlloc, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            m_WaterTlasInstanceAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_WaterTlasInstanceBuffer.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to allocate the water reflection TLAS instance buffer.";
            return false;
        }
        m_WaterTlasInstanceBuffer->SetName( L"WaterReflectionTLASInstances" );
        void* mapped = nullptr;
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( m_WaterTlasInstanceBuffer->Map( 0, &noRead, &mapped ) ) ) return false;
        memcpy( mapped, &instance, sizeof( instance ) );
        m_WaterTlasInstanceBuffer->Unmap( 0, nullptr );
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = {};
    tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlasInputs.NumDescs = 1;
    tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasInputs.InstanceDescs = m_WaterTlasInstanceBuffer->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuild = {};
    device5->GetRaytracingAccelerationStructurePrebuildInfo( &tlasInputs, &tlasPrebuild );
    if ( tlasPrebuild.ResultDataMaxSizeInBytes == 0 ) {
        LogWarn() << "D3D12: GetRaytracingAccelerationStructurePrebuildInfo returned an empty TLAS — water RT reflections disabled.";
        return false;
    }

    if ( !CreateAsBuffer( m_Allocator.Get(), tlasPrebuild.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            m_WaterTlas, m_WaterTlasAlloc, L"WaterReflectionTLAS" )
        || !CreateAsBuffer( m_Allocator.Get(), tlasPrebuild.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            m_WaterTlasScratch, m_WaterTlasScratchAlloc, L"WaterReflectionTLASScratch" ) ) {
        LogWarn() << "D3D12: failed to allocate the water reflection TLAS buffers.";
        return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasBuild = {};
    tlasBuild.Inputs = tlasInputs;
    tlasBuild.DestAccelerationStructureData = m_WaterTlas->GetGPUVirtualAddress();
    tlasBuild.ScratchAccelerationStructureData = m_WaterTlasScratch->GetGPUVirtualAddress();
    cmdList4->BuildRaytracingAccelerationStructure( &tlasBuild, 0, nullptr );
    m_CmdList->UAVBarrier( m_WaterTlas.Get() );   // the water PS below must not TraceRayInline before this completes

    // --- Bindless views: TLAS (as a raw SRV — resource must be null for this view dimension), plus the
    // world VB/IB as raw ByteAddressBuffers so the PS can fetch a hit triangle's position/UV/color. ------
    if ( m_WaterTlasSrvSlot == UINT_MAX ) m_WaterTlasSrvSlot = AllocateSrvSlot();
    if ( m_WaterWorldVbSrvSlot == UINT_MAX ) m_WaterWorldVbSrvSlot = AllocateSrvSlot();
    if ( m_WaterWorldIbSrvSlot == UINT_MAX ) m_WaterWorldIbSrvSlot = AllocateSrvSlot();
    if ( m_WaterTlasSrvSlot == UINT_MAX || m_WaterWorldVbSrvSlot == UINT_MAX || m_WaterWorldIbSrvSlot == UINT_MAX ) {
        LogWarn() << "D3D12: SRV heap exhausted allocating water RT reflection views.";
        return false;
    }

    ID3D12Device* device = m_Device.GetDevice();
    D3D12_SHADER_RESOURCE_VIEW_DESC tlasSrv = {};
    tlasSrv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    tlasSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    tlasSrv.RaytracingAccelerationStructure.Location = m_WaterTlas->GetGPUVirtualAddress();
    device->CreateShaderResourceView( nullptr, &tlasSrv, GetSrvCpuHandle( m_WaterTlasSrvSlot ) );   // resource MUST be null here

    D3D12_SHADER_RESOURCE_VIEW_DESC vbSrv = RawBufferSrvDesc( vb->GetSizeInBytes() );
    device->CreateShaderResourceView( vb->GetResource(), &vbSrv, GetSrvCpuHandle( m_WaterWorldVbSrvSlot ) );
    D3D12_SHADER_RESOURCE_VIEW_DESC ibSrv = RawBufferSrvDesc( ib->GetSizeInBytes() );
    device->CreateShaderResourceView( ib->GetResource(), &ibSrv, GetSrvCpuHandle( m_WaterWorldIbSrvSlot ) );

    m_WaterAsBuiltFromVb = wm->GetMeshVertexBuffer();
    m_WaterAsBuiltFromIb = wm->GetMeshIndexBuffer();
    m_WaterAsBuilt = true;
    LogInfo() << "D3D12: built the water reflection acceleration structure (" << vertexCount << " verts, "
              << indexCount / 3 << " tris).";
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

    // Raytraced reflection PoC: only built at all once the player opts into
    // GothicRendererSettings::WATER_REFLECTION_RAYTRACED (players who never touch the setting pay no
    // VRAM/build cost for it). Once opted in, lazily (re)builds the world BLAS/TLAS on the first water
    // draw of a newly loaded world and no-ops on every later frame. Non-fatal — a build failure (or the
    // player being on SCREENSPACE mode) just leaves rtReflectionsReady false, and the CB fill below falls
    // back to the screen-space march for this frame.
    const bool wantsRaytracedReflections =
        Engine::GAPI->GetRendererState().RendererSettings.WaterReflectionMode == GothicRendererSettings::WATER_REFLECTION_RAYTRACED;
    const bool rtReflectionsReady = wantsRaytracedReflections && EnsureWaterReflectionAS();

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

        // Reflection mode: the two are DELIBERATELY exclusive, never blended. Combining SSR with the RT
        // fallback (the previous behavior — RT only on an SSR miss) still let SSR's own grazing-angle
        // artifacts through on every pixel it *did* resolve, since the screen-space march's silhouette/
        // thickness heuristics get worse exactly at the shallow view angles reflections are most visible
        // at. RAYTRACED mode now skips the march entirely (SsrMaxSteps=0) so every reflected pixel goes
        // through the ray query instead, at the cost of RT's own limitations (single BLAS instance, no
        // textures, one bounce — see TraceWaterReflectionRT's comment in Water.hlsl).
        if ( rtReflectionsReady ) {
            cb.SsrMaxSteps = 0;
            cb.SsrRefineSteps = 0;
            cb.TlasIndex = m_WaterTlasSrvSlot;
        } else {
            // Also the automatic fallback while RAYTRACED is selected but the AS hasn't finished building
            // yet (first frame of a new world) or failed to build at all — cheap SSR beats a flat cube.
            SsrStepsForQuality( settings.WaterSSRQuality, cb.SsrMaxSteps, cb.SsrRefineSteps );
            cb.TlasIndex = 0xFFFFFFFFu;
        }
        // Underwater neither trace is valid — both assume the eye sits above the surface, so from below
        // they march/cast up through the water body and mirror the shoreline over the underwater view.
        // 0 SSR steps is the shader's "SSR off" path (Water.hlsl), same effect as D3D11's RI_SSREnabled=0.
        if ( Engine::GAPI->IsUnderWater() ) {
            cb.SsrMaxSteps = 0;
            cb.SsrRefineSteps = 0;
            cb.TlasIndex = 0xFFFFFFFFu;
        }
        cb.WorldVbIndex = m_WaterWorldVbSrvSlot;
        cb.WorldIbIndex = m_WaterWorldIbSrvSlot;

        // GSky::RenderSky() refreshes the AC_* constants every frame (DrawSky runs before this), even though
        // D3D12 renders Gothic's fixed-function sky — same reasoning as RenderFogAndGodRays. Without them the
        // scattering math would divide by a zeroed wavelength/radius set, so the shader skips it instead.
        GSky* sky = Engine::GAPI->GetSky();
        if ( sky ) {
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
        if ( !warned ) { warned = true; LogWarn() << "D3D12: water refraction resources unavailable — water surfaces will not be shaded."; }
        g_FrameWaterSurfaces.clear();
        return;
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
