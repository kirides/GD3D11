// D3D12GraphicsEngine — rain/snow particle system (D3D12 rain parity port, step 1: GPU particle
// buffers + the compute advance pass only; no visible draw yet, see the plan's staged increments).
// Mirrors D3D11Effect::DrawRain_CS's data model (RainParticleDynamic/Static, WorldObjects.h) but binds
// both buffers via ROOT SRV/UAV (see D3D12PipelineState::AdvanceRain) instead of D3D11's dual VBV+SRV
// bind — a StructuredBuffer read/write never needs a descriptor-heap slot, only Texture2D-shaped
// resources do.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"
#include "../WorldConverter.h"
#include "../oCGame.h"
#include "../zCSkyController_Outdoor.h"
#include "../DDSArrayLoader.h"
#include "../Toolbox.h"
#include "../zCMaterial.h"
#include "../zCTexture.h"
#include "../D3D7/MyDirectDrawSurface7.h"
#include "D3D12Texture.h"
#include "D3D12VertexBuffer.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    const float2 snowScale( 3.0f, 3.0f );
    const float2 rainScale( 30.0f / 10.0f, 30.0f / 2.0f );
    constexpr float kSnowSpeedFactor = 0.15f;

    bool IsSnowWeather() {
        return oCGame::GetGame()
            && oCGame::GetGame()->_zCSession_world
            && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()
            && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetWeatherType() == zTWEATHER_SNOW;
    }

    // Same generation as D3D11Effect::FillRandomRaindropData (D3D11Effect.cpp:53-96) — duplicated rather
    // than shared, since D3D11Effect is a D3D11-only class and the D3D12 backend must not depend on it.
    void FillRandomRaindropData( std::vector<RainParticleDynamic>& dynamicData, std::vector<RainParticleStatic>& staticData ) {
        const float radius = Engine::GAPI->GetRendererState().RendererSettings.RainRadiusRange;
        const float height = Engine::GAPI->GetRendererState().RendererSettings.RainHeightRange;
        const XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();

        for ( size_t i = 0; i < dynamicData.size(); i++ ) {
            float seedX, seedZ;
            bool inside = false;
            while ( !inside ) {
                seedX = Toolbox::frand() - 0.5f;
                seedZ = Toolbox::frand() - 0.5f;
                if ( sqrt( seedX * seedX + seedZ * seedZ ) <= 0.5f ) inside = true;
            }
            seedX *= radius;
            seedZ *= radius;
            const float seedY = Toolbox::frand() * height;

            const float speedX = 40.0f * (Toolbox::frand() / 20.0f);
            const float speedZ = 40.0f * (Toolbox::frand() / 20.0f);
            const float speedY = 40.0f * (Toolbox::frand() / 10.0f);

            RainParticleDynamic& dyn = dynamicData[i];
            dyn.position = float3( seedX + camPos.x, seedY + camPos.y, seedZ + camPos.z );
            dyn.velocity = XMFLOAT3( speedX, speedY, speedZ );

            RainParticleStatic& stat = staticData[i];
            stat.seed = float3( seedX, seedY, seedZ );
            stat.randomBrightness = Toolbox::frand();

            short* s = reinterpret_cast<short*>(&stat.drawMode);
            s[0] = static_cast<short>( floor( Toolbox::frand() * 8 + 1 ) );
            s[1] = static_cast<short>( floor( Toolbox::frand() * 0xFFFF ) );
        }
    }
}

bool D3D12GraphicsEngine::CreateRainBuffers( UINT numParticles ) {
    m_RainBuffersReady = false;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || numParticles == 0 ) return false;

    // 128-thread groups, like D3D11Effect::DrawRain_CS (D3D11Effect.cpp:323).
    const UINT alignedCount = ((numParticles + 127) / 128) * 128;

    std::vector<RainParticleDynamic> dynamicParticles( alignedCount );
    std::vector<RainParticleStatic> staticParticles( alignedCount );
    FillRandomRaindropData( dynamicParticles, staticParticles );

    // --- Static (immutable) buffer: persistently-mapped UPLOAD heap, written once, read via root SRV.
    // Mirrors CreateLightBuffer's per-frame StructuredBuffer pattern, minus the per-frame rewrite. ---
    {
        D3D12MA::ALLOCATION_DESC uploadHeap = {};
        uploadHeap.HeapType = DefaultUploadHeapType;

        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<UINT64>(staticParticles.size()) * sizeof( RainParticleStatic );
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if ( FAILED( m_Allocator->CreateResource( &uploadHeap, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            m_RainBufferStaticAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_RainBufferStatic.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the rain static-particle buffer.";
            return false;
        }
        m_RainBufferStatic->SetName( L"RainBufferStatic" );

        void* mapped = nullptr;
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( m_RainBufferStatic->Map( 0, &noRead, &mapped ) ) ) return false;
        memcpy( mapped, staticParticles.data(), bd.Width );
        m_RainBufferStatic->Unmap( 0, nullptr );
    }

    // --- Dynamic (mutable) buffer: DEFAULT-heap UAV, initial data copied in via UploadBufferData (same
    // idiom D3D12VertexBuffer's static path uses — created in COMMON so the copy-queue's CopyBufferRegion
    // can target it via buffer state promotion; flips to UNORDERED_ACCESS once, in AdvanceRain()). ---
    {
        D3D12MA::ALLOCATION_DESC heapDefault = {};
        heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<UINT64>(dynamicParticles.size()) * sizeof( RainParticleDynamic );
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if ( FAILED( m_Allocator->CreateResource( &heapDefault, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
            m_RainBufferDynamicAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_RainBufferDynamic.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the rain dynamic-particle buffer.";
            return false;
        }
        m_RainBufferDynamic->SetName( L"RainBufferDynamic" );

        if ( !UploadBufferData( m_RainBufferDynamic.Get(), dynamicParticles.data(), bd.Width ) ) {
            LogWarn() << "D3D12: failed to upload initial rain dynamic-particle data.";
            return false;
        }
        m_RainDynamicNeedsInitialBarrier = true;
    }

    // Non-fatal: DrawRainParticles falls back to the step-2 flat-white placeholder (m_RainTexturesLoaded
    // guard) if this fails, e.g. missing/unreadable DDS files.
    if ( !m_RainTexturesLoaded ) LoadRainTextures();

    m_RainParticleCount = alignedCount;
    m_RainBuffersReady = true;
    return true;
}

void D3D12GraphicsEngine::AdvanceRain() {
    if ( !m_FrameOpen || !m_Pipelines.AdvanceRain.PSO || !m_Pipelines.AdvanceRain.RootSig ) return;

    auto& state = Engine::GAPI->GetRendererState();
    // Matches D3D11's actual gate exactly (D3D11GraphicsEngine.cpp:4274): the whole rain pass is keyed
    // off RainFXWeight alone. RendererSettings.EnableRain is a DIFFERENT, Gothic-engine-side toggle
    // (GothicAPI.cpp:601, native rain spawn/sound suppression) — not the GD3D11 GPU rain draw's switch.
    if ( Engine::GAPI->GetRainFXWeight() <= 0.0f ) return;

    const UINT numParticles = state.RendererSettings.RainNumParticles;
    if ( numParticles == 0 ) return;

    // Dirty-check + (re)build, mirroring D3D11Effect::DrawRain_CS (D3D11Effect.cpp:314-316): rebuild
    // whenever radius/height change, or the 128-aligned particle count changes.
    const float radius = state.RendererSettings.RainRadiusRange;
    const float height = state.RendererSettings.RainHeightRange;
    const UINT alignedCount = ((numParticles + 127) / 128) * 128;
    if ( !m_RainBuffersReady || m_RainLastRadius != radius || m_RainLastHeight != height
        || m_RainParticleCount != alignedCount ) {
        if ( !CreateRainBuffers( numParticles ) ) return;
        m_RainLastRadius = radius;
        m_RainLastHeight = height;
        m_RainLastNumParticles = numParticles;
    }
    if ( !m_RainBuffersReady ) return;

    DX_ZONE( m_CmdList, "AdvanceRain" );

    // Establish the invariant DrawRainParticles relies on — by the time AdvanceRain returns (buffers
    // ready), m_RainBufferDynamic is always in UNORDERED_ACCESS, whether or not the CS actually dispatched
    // this frame (RainMoveParticles/pause only skip the dispatch below, never this state fixup) —
    // otherwise a paused/stationary first frame would leave the buffer in COMMON/NON_PIXEL_SHADER_RESOURCE
    // with no dispatch ever running to flip it, and the draw's SRV read would be reading stale state.
    if ( m_RainDynamicNeedsInitialBarrier ) {
        auto b = TransitionBarrier( m_RainBufferDynamic.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        m_CmdList->ResourceBarrier( 1, &b );
        m_RainDynamicNeedsInitialBarrier = false;
    } else if ( m_RainDynamicInReadState ) {
        // DrawRainParticles left it in NON_PIXEL_SHADER_RESOURCE last frame — flip back before this CS writes it.
        auto b = TransitionBarrier( m_RainBufferDynamic.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        m_CmdList->ResourceBarrier( 1, &b );
        m_RainDynamicInReadState = false;
    }

    if ( !(state.RendererSettings.RainMoveParticles && !Engine::GAPI->IsGamePaused()) ) return;

    auto velocity = state.RendererSettings.RainGlobalVelocity;
    if ( IsSnowWeather() ) {
        velocity = XMFLOAT3( velocity.x * kSnowSpeedFactor, velocity.y * kSnowSpeedFactor, velocity.z * kSnowSpeedFactor );
    }

    struct AdvanceRainCB {
        float LightX, LightY, LightZ, FPS;
        float CamX, CamY, CamZ, Radius;
        float Height, VelX, VelY, VelZ;
        int   MoveCount; float Pad0, Pad1, Pad2;
    };
    const XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();
    AdvanceRainCB cb = {
        0.0f, 0.0f, 0.0f, static_cast<float>(state.RendererInfo.FPS),
        camPos.x, camPos.y, camPos.z, radius,
        height, velocity.x, velocity.y, velocity.z,
        static_cast<int>(numParticles), 0.0f, 0.0f, 0.0f
    };

    m_CmdList->SetPipelineState( m_Pipelines.AdvanceRain.PSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.AdvanceRain.RootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 16, &cb, 0 );
    m_CmdList->SetComputeRootShaderResourceView( 1, m_RainBufferStatic->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootUnorderedAccessView( 2, m_RainBufferDynamic->GetGPUVirtualAddress() );
    m_CmdList->Dispatch( (numParticles + 127) / 128, 1, 1 );
}

void D3D12GraphicsEngine::DrawRainParticles() {
    // Rain/snow billboard draw. Slots where D3D11 draws rain: after the frame's regular PFX particles,
    // before ghost VOBs/post-FX — alpha-blended, depth-tested, no depth-write.
    if ( !m_FrameOpen || !m_Pipelines.RainDraw.PSO || !m_Pipelines.RainDraw.RootSig || !m_DepthBuffer ) return;

    auto& state = Engine::GAPI->GetRendererState();
    if ( !m_RainBuffersReady || !m_RainTexturesLoaded ) return;
    if ( !m_RainShadowResourcesReady ) return;   // VSMain unconditionally samples the rain shadowmap now
    if ( Engine::GAPI->GetRainFXWeight() <= 0.0f ) return;   // matches D3D11's actual gate (D3D11GraphicsEngine.cpp:4274) — RendererSettings.EnableRain is a different, Gothic-engine-side toggle (see AdvanceRain's comment)

    const UINT numParticles = state.RendererSettings.RainNumParticles;
    if ( numParticles == 0 ) return;

    const bool isSnow = IsSnowWeather();
    const UINT texArraySlot = isSnow ? m_SnowTextureArraySrvSlot : m_RainTextureArraySrvSlot;
    if ( texArraySlot == UINT_MAX ) return;

    DX_ZONE( m_CmdList, "DrawRainParticles" );

    // Flip the dynamic buffer UAV -> NON_PIXEL_SHADER_RESOURCE for the VS's root-SRV read (AdvanceRain
    // guarantees it's in UNORDERED_ACCESS by the time it returns, whether or not it actually dispatched).
    {
        auto b = TransitionBarrier( m_RainBufferDynamic.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        m_CmdList->ResourceBarrier( 1, &b );
        m_RainDynamicInReadState = true;
    }

    // ViewProj — same plain (non-jittered) derivation DrawParticleEffects uses (particle positions are
    // world-space; TAA jitter isn't threaded through this pass, matching the D3D11 rain draw either).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    // Matches DrawParticleEffects's exact construction order (proj, view) — this codebase's row-major-
    // stored/column-major-read HLSL convention (CLAUDE.md) makes that order, not (view, proj), the one
    // that composes correctly with Rain.hlsl's mul(vec, ViewProj).
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    auto velocity = state.RendererSettings.RainGlobalVelocity;
    XMFLOAT3 rainDir( 0.0f, -1.0f, 0.0f );
    XMVECTOR velVec = XMLoadFloat3( &velocity );
    if ( XMVectorGetX( XMVector3LengthSq( velVec ) ) > 0.0001f ) {
        XMStoreFloat3( &rainDir, XMVector3Normalize( velVec ) );
    }

    const float2& scale = isSnow ? snowScale : rainScale;
    const XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();
    struct RainInfoCB {
        float CamX, CamY, CamZ, RainFxWeight;
        float RainHeight, DirX, DirY, DirZ;
        float ScaleX, ScaleY;
    } infoCb = {
        camPos.x, camPos.y, camPos.z, Engine::GAPI->GetRainFXWeight(),
        state.RendererSettings.RainHeightRange, rainDir.x, rainDir.y, rainDir.z,
        scale.x, scale.y
    };
    struct RainTexCB { UINT TexArrayIndex; UINT IsSnow; } texCb = { texArraySlot, isSnow ? 1u : 0u };

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );

    m_CmdList->SetPipelineState( m_Pipelines.RainDraw.PSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.RainDraw.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 1, 10, &infoCb, 0 );
    m_CmdList->SetGraphicsRootShaderResourceView( 2, m_RainBufferDynamic->GetGPUVirtualAddress() );
    m_CmdList->SetGraphicsRootShaderResourceView( 3, m_RainBufferStatic->GetGPUVirtualAddress() );
    m_CmdList->SetGraphicsRoot32BitConstants( 4, 2, &texCb, 0 );

    struct RainShadowCB { XMFLOAT4X4 ViewProj; UINT SrvIndex; } shadowCb = { m_RainShadowViewProj, m_RainShadowSrvSlot };
    m_CmdList->SetGraphicsRoot32BitConstants( 5, 17, &shadowCb, 0 );

    // No IA vertex/index buffers — the VS pulls everything from the two root SRVs above by SV_VertexID/
    // SV_InstanceID (see Shaders/D3D12/Rain.hlsl).
    m_CmdList->DrawInstanced( 4, numParticles, 0, 0 );
}

bool D3D12GraphicsEngine::LoadRainTextureArray( const char* prefix, int count, ComPtr<ID3D12Resource>& outTex,
    ComPtr<D3D12MA::Allocation>& outAlloc, UINT& outSrvSlot ) {
    ParsedTextureArray parsed;
    if ( FAILED( ParseTextureArrayDDS( prefix, count, zVdfsReadFile, parsed ) ) ) {
        LogWarn() << "D3D12: failed to parse rain/snow texture array at prefix " << prefix;
        return false;
    }

    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = parsed.width;
    td.Height = parsed.height;
    td.DepthOrArraySize = static_cast<UINT16>(count);
    td.MipLevels = static_cast<UINT16>(parsed.mipCount);
    td.Format = parsed.format;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12MA::ALLOCATION_DESC heapDefault = {};
    heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    // COMMON, not UNORDERED_ACCESS/PIXEL_SHADER_RESOURCE — same idiom D3D12Texture uses: the upload
    // below is a copy-queue CopyTextureRegion (buffers/textures implicitly promote COMMON->COPY_DEST for
    // that), and reading it afterward as an SRV also implicitly promotes from COMMON, so no explicit
    // barrier is needed either side of the upload for a plain (non-simultaneous-access) texture.
    if ( FAILED( m_Allocator->CreateResource( &heapDefault, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
        outAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( outTex.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: failed to create rain/snow texture array resource (prefix " << prefix << ").";
        return false;
    }
    outTex->SetName( L"RainSnowTextureArray" );

    std::vector<D3D12_SUBRESOURCE_DATA> subs;
    subs.reserve( static_cast<size_t>(count) * parsed.mipCount );
    for ( const auto& slice : parsed.slices ) {
        for ( const auto& mip : slice.mips ) {
            subs.push_back( { mip.data, static_cast<LONG_PTR>(mip.rowPitch), static_cast<LONG_PTR>(mip.sizeBytes) } );
        }
    }
    if ( !UploadTextureSubresources( outTex.Get(), subs.data(), static_cast<UINT>(subs.size()) ) ) {
        LogWarn() << "D3D12: failed to upload rain/snow texture array data (prefix " << prefix << ").";
        return false;
    }

    outSrvSlot = AllocateSrvSlot();
    if ( outSrvSlot == UINT_MAX ) {
        LogWarn() << "D3D12: SRV heap exhausted allocating a slot for the rain/snow texture array.";
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = parsed.format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2DArray.MipLevels = parsed.mipCount;
    srv.Texture2DArray.ArraySize = count;
    device->CreateShaderResourceView( outTex.Get(), &srv, GetSrvCpuHandle( outSrvSlot ) );
    return true;
}

bool D3D12GraphicsEngine::LoadRainTextures() {
    if ( m_RainTexturesLoaded ) return true;

    // Same VFS paths as D3D11Effect::LoadRainResources; parsed via the DDSArrayLoader.h helper both
    // backends share.
    LogInfo() << "D3D12: loading rain-drop textures";
    const bool rainOk = LoadRainTextureArray( R"(\System\GD3D11\Textures\Raindrops\cv0_vPositive_)", 370,
        m_RainTextureArray, m_RainTextureArrayAlloc, m_RainTextureArraySrvSlot );

    LogInfo() << "D3D12: loading snow-flake textures";
    const bool snowOk = LoadRainTextureArray( R"(\System\GD3D11\Textures\Snowflakes\Snow_)", 256,
        m_SnowTextureArray, m_SnowTextureArrayAlloc, m_SnowTextureArraySrvSlot );

    m_RainTexturesLoaded = rainOk && snowOk;
    return m_RainTexturesLoaded;
}

bool D3D12GraphicsEngine::CreateRainShadowResources() {
    if ( m_RainShadowResourcesReady ) return true;
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    if ( !m_RainShadowDsvHeap ) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        if ( FAILED( device->CreateDescriptorHeap( &hd, IID_PPV_ARGS( m_RainShadowDsvHeap.ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the rain shadowmap DSV heap.";
            return false;
        }
        m_RainShadowDsv = m_RainShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    }
    if ( m_RainShadowSrvSlot == UINT_MAX ) {
        m_RainShadowSrvSlot = AllocateSrvSlot();
        if ( m_RainShadowSrvSlot == UINT_MAX ) {
            LogWarn() << "D3D12: SRV heap exhausted allocating the rain shadowmap slot.";
            return false;
        }
    }

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = kRainShadowMapSize;
    dd.Height = kRainShadowMapSize;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_R32_TYPELESS;   // D32 DSV + R32_FLOAT SRV, same split as the CSM sun shadow map
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;   // normal-Z: 1.0 == far (not reversed-Z, matches the CSM sun map)

    if ( FAILED( m_Allocator->CreateResource( &allocDesc, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
        m_RainShadowMapAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( m_RainShadowMap.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: failed to create the rain shadow map resource.";
        return false;
    }
    m_RainShadowMap->SetName( L"RainShadowMap" );
    m_RainShadowInReadState = false;   // freshly created in DEPTH_WRITE

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView( m_RainShadowMap.Get(), &dsv, m_RainShadowDsv );

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView( m_RainShadowMap.Get(), &srv, GetSrvCpuHandle( m_RainShadowSrvSlot ) );

    m_RainShadowResourcesReady = true;
    return true;
}

void D3D12GraphicsEngine::RenderRainShadowmap() {
    // World-mesh-only rain shadowmap (see the header comment for the VOB/grass scope cut). Reuses
    // m_ShadowCasterWorldPSO/m_Pipelines.World.RootSig with PER-DRAW (non-indirect) binds — the shared
    // root sig's b6 MaterialCB (params[10] in CreateWorld) is a plain root-constants parameter, so
    // ExecuteIndirect is purely an optimization the main CSM/color passes use, not a requirement; a
    // single low-frequency pass like this one is fine issuing one SetGraphicsRoot32BitConstants +
    // DrawIndexedInstanced per mesh, same as the pre-P2.11 per-draw pattern.
    if ( !m_FrameOpen || !m_ShadowCasterWorldPSO || !m_Pipelines.World.RootSig || !m_BlackTexture || !m_DefaultOrmTexture )
        return;

    auto& state = Engine::GAPI->GetRendererState();
    if ( Engine::GAPI->GetRainFXWeight() <= 0.0f ) return;   // matches D3D11's actual gate — see AdvanceRain's comment on RendererSettings.EnableRain
    if ( !CreateRainShadowResources() ) return;

    DX_ZONE( m_CmdList, "RainShadowmap" );

    // Return the map to DEPTH_WRITE if last frame's rain draw left it in NON_PIXEL_SHADER_RESOURCE.
    if ( m_RainShadowInReadState ) {
        auto toDepth = TransitionBarrier( m_RainShadowMap.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE );
        m_CmdList->ResourceBarrier( 1, &toDepth );
        m_RainShadowInReadState = false;
    }

    m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, &m_RainShadowDsv );
    m_CmdList->ClearDepthStencilView( m_RainShadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );

    // --- Camera: orthographic, looking along the (inverted) rain-velocity direction — mirrors
    const float scaleFactor = Toolbox::GetRecommendedWorldShadowRangeScaleForSize( static_cast<int>(kRainShadowMapSize) );
    const float span = static_cast<float>(kRainShadowMapSize) * scaleFactor;

    XMVECTOR rainVel = XMLoadFloat3( &state.RendererSettings.RainGlobalVelocity );
    if ( XMVectorGetX( XMVector3LengthSq( rainVel ) ) < 0.0001f ) rainVel = XMVectorSet( 0.0f, -1.0f, 0.0f, 0.0f );

    XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();

    XMVECTOR forward = XMVector3Normalize( rainVel ); // Light forward direction

    // 2. Compute Right and Up vectors
    XMVECTOR up = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
    if ( fabsf( XMVectorGetX( XMVector3Dot( forward, up ) ) ) > 0.95f )
        up = XMVectorSet( 1.0f, 0.0f, 0.0f, 0.0f );

    XMVECTOR right = XMVector3Normalize( XMVector3Cross( up, forward ) );
    up = XMVector3Cross( forward, right );

    // Pull the eye back along -forward by halfRange (same idea as ComputeCascadeMatrices' pullBack,
    // D3D12Scene.cpp:1088-1089) so the [0, 2*halfRange] depth range brackets halfRange ABOVE camPos
    // (catching roofs overhead) through halfRange BELOW it (catching terrain) — placing the eye AT
    // camPos with NearZ=1 clipped everything above the player (roofs) out of the depth map entirely,
    // leaving those texels at the cleared far-depth ("no occluder"), so rain fell through roofs.
    const float halfRange = 4000.0f; // 4000 units above, 4000 units below
    XMVECTOR eyePos = XMVectorSubtract( camPos, XMVectorScale( forward, halfRange ) );

    // 3. Construct the Light's World Matrix (Inverse View) directly!
    XMMATRIX lightWorldMatrix;
    lightWorldMatrix.r[0] = right;
    lightWorldMatrix.r[1] = up;
    lightWorldMatrix.r[2] = forward;
    lightWorldMatrix.r[3] = eyePos;
    lightWorldMatrix.r[0] = XMVectorSetW( lightWorldMatrix.r[0], 0.0f );
    lightWorldMatrix.r[1] = XMVectorSetW( lightWorldMatrix.r[1], 0.0f );
    lightWorldMatrix.r[2] = XMVectorSetW( lightWorldMatrix.r[2], 0.0f );
    lightWorldMatrix.r[3] = XMVectorSetW( lightWorldMatrix.r[3], 1.0f );

    XMMATRIX view = XMMatrixInverse( nullptr, lightWorldMatrix );
    XMMATRIX proj = XMMatrixOrthographicLH( span, span, 1.0f, halfRange * 2.0f );

    m_RainShadowFrustum.BuildOrthographic( view, span, span, 1.0f, halfRange * 2.0f, 0, 0, 0 );

    if ( !m_RainShadowFrustum.IsValid() ) {
        auto toRead = TransitionBarrier( m_RainShadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        m_CmdList->ResourceBarrier( 1, &toRead );
        m_RainShadowInReadState = true;
        // Re-bind the HDR scene-color RT (+ shared depth) — mirrors RenderSunShadows/RenderPointShadows'
        // end-of-pass rebind. Without this the rain-shadow DSV stays the "currently bound" render target
        // as far as the API is concerned, and the very next Draw call (anywhere) fails GPU validation
        // ("resource state ... invalid for use as depth-read") once this pass transitions it away from
        // DEPTH_WRITE.
        D3D12_CPU_DESCRIPTOR_HANDLE mainDsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, m_DepthBuffer ? &mainDsv : nullptr );
        return;
    }

    // Matches ComputeCascadeMatrices' CascadeViewProj construction EXACTLY (D3D12Scene.cpp) — both feed the
    // SAME shader (DepthPrepassVsBlob / m_Pipelines.World.RootSig's b0, "default column-major packing", plain
    // mul(pos, ViewProj)) and Rain.hlsl's mul(wsPos, RainShadowViewProj) sampling, which need the identical
    // convention: transpose(view*proj), NOT the (proj,view)-without-transpose this used before. That mismatch
    // (missing transpose + swapped multiply order) produced a degenerate/garbage clip transform — visible as a
    // thin wedge radiating from a point instead of a proper top-down depth map of the terrain.
    XMStoreFloat4x4( &m_RainShadowViewProj, XMMatrixTranspose( XMMatrixMultiply( view, proj ) ) );

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    D3D12VertexBuffer* vb = wm ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
    D3D12VertexBuffer* ib = wm ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;
    const bool haveWorld = vb && ib && vb->GetResource() && ib->GetResource()
        && (ib->GetSizeInBytes() / sizeof( uint32_t )) > 0;

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(kRainShadowMapSize), static_cast<float>(kRainShadowMapSize), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, static_cast<LONG>(kRainShadowMapSize), static_cast<LONG>(kRainShadowMapSize) };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    if ( haveWorld ) {
        static std::vector<WorldMeshSectionInfo*> rainShadowSections;
        rainShadowSections.clear();

        // culling currently broken with BVH but only for rain shadow map ???
        auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
        const auto oldUseWorldSectionBVH = settings.DebugSettings.FeatureSet.UseWorldSectionBVH;
        settings.DebugSettings.FeatureSet.UseWorldSectionBVH = false;
        Engine::GAPI->CollectVisibleSections( rainShadowSections, &m_RainShadowFrustum, false );
        settings.DebugSettings.FeatureSet.UseWorldSectionBVH = oldUseWorldSectionBVH;

        if ( rainShadowSections.empty() ) {
            auto _ = "wtf";
        }

        m_CmdList->SetPipelineState( m_ShadowCasterWorldPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_RainShadowViewProj, 0 );

        const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
        const D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
        m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
        m_CmdList->IASetIndexBuffer( &ibv );

        for ( WorldMeshSectionInfo* section : rainShadowSections ) {
            if ( !section ) continue;
            for ( auto const& [meshKey, mesh] : section->WorldMeshes ) {
                if ( !mesh || mesh->Indices.empty() ) continue;
                if ( meshKey.Info && meshKey.Info->MaterialType != MaterialInfo::MT_None ) continue;
               // if ( !Engine::GAPI->IsWorldMeshVisibleInFrustum( mesh, m_RainShadowFrustum ) ) continue;

                // Skip translucent/blended geometry, matching the CSM caster loop.
                if ( (meshKey.Material->GetAlphaFunc() > zMAT_ALPHA_FUNC_NONE &&
                    meshKey.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST)
                    || (meshKey.Material->GetAlphaFunc() == 0 && zColor( meshKey.Material->GetColor() ).bgra.alpha < 255) ) {
                    continue;
                }

                uint32_t diffuseIdx = m_BlackTexture->GetSrvSlot();
                if ( zCTexture* tex = meshKey.Material->GetAniTexture() ) {
                    if ( tex->GetCacheState() == zRES_CACHED_IN ) {
                        if ( MyDirectDrawSurface7* s = tex->GetSurface() ) {
                            if ( GfxTexture* gfx = s->GetEngineTexture() ) {
                                D3D12Texture* d = D3D12Texture::From( gfx );
                                if ( d->HasSRV() ) diffuseIdx = d->GetSrvSlot();
                            }
                        }
                    }
                }

                struct { UINT NormalIdx, OrmIdx, DiffuseIdx; } matCb = { 0xFFFFFFFFu, m_DefaultOrmTexture->GetSrvSlot(), diffuseIdx };
                m_CmdList->SetGraphicsRoot32BitConstants( 10, 3, &matCb, 0 );
                m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mesh->Indices.size()), 1, mesh->BaseIndexLocation, 0, 0 );
            }
        }
    }

    auto toRead = TransitionBarrier( m_RainShadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
    m_CmdList->ResourceBarrier( 1, &toRead );
    m_RainShadowInReadState = true;

    // Re-bind the HDR scene-color RT (+ shared depth) — mirrors RenderSunShadows/RenderPointShadows' end-
    // of-pass rebind. Without this the rain-shadow DSV stays the "currently bound" render target as far as
    // the command list is concerned, and the next Draw call anywhere in the frame fails GPU validation
    // ("resource state ... invalid for use as depth-read/depth-write") once this pass moves the resource
    // out of DEPTH_WRITE.
    D3D12_CPU_DESCRIPTOR_HANDLE mainDsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, m_DepthBuffer ? &mainDsv : nullptr );
}
