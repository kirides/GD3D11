// Procedural atmospheric-scattering sky dome (D3D12) — the port of D3D11GraphicsEngine::DrawSky's
// AtmosphericScattering branch, and the replacement for Gothic's fixed-function sky.
//
// WHY THIS EXISTS
// ZenGin draws its sky as a sequence of separate zCRenderer calls: zCSkyLayer::RenderSkyLayer twice (each a
// zCMesh::Render of a dome/horizon layer), an optional colour dome, a background zrenderer->DrawTile, an
// underwater screen-blend zrenderer->DrawPoly, plus the planets. In this mod every one of those arrives at
// MyDirect3DDevice7::DrawPrimitive as a D3DFVF_XYZRHW_DIF_T1 batch and is replayed through the fixed-function
// emulation one draw at a time — each paying a render-state resolve, an FF constant-buffer update and a
// vertex-buffer refill. That is measurably expensive (~0.5 ms for the dome alone) for something that is a
// single analytic function of time of day.
//
// This pass replaces the whole sequence with ONE DrawIndexedInstanced over GSky's static unit sphere. The
// day/night cycle, sun position, weather tint and wavelength (world-specific: G1 and G2 use different
// AC_Wavelength) all come from the AtmosphereConstantBuffer that GSky::RenderSky() already fills every frame
// regardless of who draws the sky — so nothing new has to be computed on the CPU, and the cloud + star layers
// come from GSky's own two textures (which scripts can override via SetCustomSkyTexture_ZenGin).
//
// FIXED-FUNCTION FALLBACK
// DrawAtmosphereSkyDome() returns false without drawing if anything it needs is missing, and DrawSky() then
// calls RenderSkyPre() exactly as before. The user-facing RendererSettings.AtmosphericScattering toggle gates
// it the same way it gates D3D11's, so the two backends respond identically to the same setting.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12Texture.h"
#include "D3D12VertexBuffer.h"
#include "D3D12PipelineState.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../GSky.h"
#include "../GMesh.h"
#include "../WorldObjects.h"
#include "../VertexTypes.h"
#include "../zCWorld.h"
#include "../zCTexture.h"
#include "../D3D7/MyDirectDrawSurface7.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    // b3 payload — must mirror Sky.hlsl's SkyMaterialCB exactly, including the 16-byte-boundary padding.
    // kNoSkyTexture matches its 0xFFFFFFFF sentinel: the layer is skipped entirely rather than sampled from
    // a garbage descriptor.
    constexpr UINT kNoSkyTexture = 0xFFFFFFFFu;
    struct SkyMaterialConsts {
        UINT  CloudIndex;
        UINT  NightIndex;
        UINT  MoonIndex;
        UINT  Pad0;
        float MoonCenterPx[2];
        float MoonHalfSizePx[2];
        float MoonColor[4];
    };
    static_assert( sizeof( SkyMaterialConsts ) == 12 * sizeof( UINT ), "SkyMaterialCB is 12 root constants" );

    // GfxTexture -> bindless SRV heap slot, or kNoSkyTexture. GSky's own DDS textures are loaded through the
    // backend-neutral CreateTexture path (so on D3D12 they really are D3D12Textures); a script-supplied
    // ZenGin override arrives already cached-in from GSky::Get*TextureGfx.
    UINT SkyTextureSlot( GfxTexture* tex ) {
        if ( !tex ) return kNoSkyTexture;
        D3D12Texture* d = D3D12Texture::From( tex );
        return d->HasSRV() ? d->GetSrvSlot() : kNoSkyTexture;
    }

    // Resolve a zCTexture (a ZenGin material's texture) to a bindless SRV slot, caching it in on the way —
    // the same chain every other D3D12 pass uses. RenderPlanets does the equivalent CacheIn(-1) itself.
    UINT ZenTextureSlot( zCTexture* tex ) {
        if ( !tex || tex->CacheIn( -1 ) != zRES_CACHED_IN ) return kNoSkyTexture;
        MyDirectDrawSurface7* surface = tex->GetSurface();
        if ( !surface ) return kNoSkyTexture;
        return SkyTextureSlot( surface->GetEngineTexture() );
    }

}


bool D3D12GraphicsEngine::CreateSkyConstantBuffers() {
    // One persistently-mapped 256-byte UPLOAD buffer per frame-in-flight, holding just the
    // AtmosphereConstantBuffer the pixel shader reads at b1. 256 B because that is the root-CBV alignment
    // floor anyway; the struct itself is 96 B. Same pattern as CreateFogConstantBuffers /
    // CreateWaterConstantBuffers, minus their second block (this pass has no other CB).
    static_assert( sizeof( AtmosphereConstantBuffer ) <= 256,
        "AtmosphereConstantBuffer must fit in one 256-byte root-CBV block" );

    D3D12MA::ALLOCATION_DESC uploadAlloc = {};
    uploadAlloc.HeapType = DefaultUploadHeapType;

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = 256;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_Allocator->CreateResource( &uploadAlloc, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, m_SkyCBAlloc[i].ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_SkyCB[i].ReleaseAndGetAddressOf() ) ) ) ) {
            LogWarn() << "D3D12: failed to create the sky atmosphere constant buffer.";
            return false;
        }
        m_SkyCB[i]->SetName( L"SkyAtmosphereCB" );

        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( m_SkyCB[i]->Map( 0, &noRead, &mapped ) ) ) {
            LogWarn() << "D3D12: failed to map the sky atmosphere constant buffer.";
            return false;
        }
        m_SkyCBMapped[i] = static_cast<uint8_t*>( mapped );
        m_SkyCBGpu[i] = m_SkyCB[i]->GetGPUVirtualAddress();
    }
    return true;
}


bool D3D12GraphicsEngine::DrawAtmosphereSkyDome() {
    GSky* sky = Engine::GAPI->GetSky();
    if ( !sky ) return false;

    // GSky::RenderSky() (called by DrawSky just above) lazily loads the dome on first use, so this is only
    // null before the first world frame.
    GMesh* dome = sky->GetSkyDome();
    if ( !dome || dome->GetMeshes().empty() ) return false;

    if ( !m_Pipelines.Sky.RootSig || !m_Pipelines.Sky.PSO || !m_Pipelines.Sky.OuterPSO ) return false;
    if ( !m_SkyCBMapped[m_FrameIndex] ) return false;

    // Own marker inside DrawSky's — this is the pass whose cost is being compared against the
    // fixed-function sky it replaces, so it needs to be separable in a capture.
    DX_ZONE( m_CmdList.Get(), "Sky dome (scattering)" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "Sky dome (scattering)" );

    const AtmosphereConstantBuffer& atmo = sky->GetAtmosphereCB();
    const AtmosphereSettings& settings = sky->GetAtmoshpereSettings();

    // World matrix, verbatim from D3D11's DrawSky (:8085-8096): scale the unit sphere up to OuterRadius and
    // centre it on the camera, offset down by SphereOffsetY so the camera sits inside the atmosphere shell.
    // The transpose puts the translation at _14/_24/_34, which is the form Sky.hlsl's
    // `mul( float4( pos, 1 ), World )` needs (HLSL reads a cbuffer matrix column-major, so that memory layout
    // lands the translation in the row a row-vector multiply picks up — same convention as Fx.hlsl and the
    // Gothic world matrices, see CLAUDE.md).
    //
    // Why the translation being applied is load-bearing: GSky sets AC_SpherePosition to the SAME
    // camera + SphereOffsetY point, so the shader's `vPos = worldPosition - AC_SpherePosition` cancels it back
    // out to pos * OuterRadius — a pure direction on the shell, independent of where the player is standing.
    // Drop the translation here and vPos would drift with the camera and the sky would swim.
    const XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();
    XMMATRIX scale = XMMatrixScaling( settings.OuterRadius, settings.OuterRadius, settings.OuterRadius );
    XMMATRIX translate = XMMatrixTranslation( camPos.x, camPos.y + settings.SphereOffsetY, camPos.z );
    XMFLOAT4X4 world;
    XMStoreFloat4x4( &world, XMMatrixTranspose( scale * translate ) );

    // ViewProj in the codebase's established order: XMMatrixMultiply( proj, view ) with the row-major-stored /
    // column-major-read HLSL convention (see CLAUDE.md and DrawParticleEffects) — the other order silently
    // produces garbage with `mul( vec, StoredMatrix )`.
    Engine::GAPI->SetViewTransformXM( Engine::GAPI->GetViewMatrixXM() );
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    memcpy( m_SkyCBMapped[m_FrameIndex], &atmo, sizeof( atmo ) );

    // Which side of the atmosphere shell the camera is on picks the pixel shader, exactly as D3D11 chooses
    // between PS_Atmosphere and PS_AtmosphereOuter. In normal play this is always the inner one.
    const bool outside = atmo.AC_CameraHeight > atmo.AC_OuterRadius;
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.Sky.RootSig.Get() );
    m_CmdList->SetPipelineState( outside ? m_Pipelines.Sky.OuterPSO.Get() : m_Pipelines.Sky.PSO.Get() );

    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );                  // b0 ViewProj
    m_CmdList->SetGraphicsRootConstantBufferView( 1, m_SkyCBGpu[m_FrameIndex] );      // b1 Atmosphere
    m_CmdList->SetGraphicsRoot32BitConstants( 2, 16, &world, 0 );                     // b2 World

    // The moon (planets[1]) — the one thing the fixed-function sky drew that the bare scattering dome does
    // not. GSky resolves the placement (shared with D3D11's sky, which composites the same sprite); all this
    // backend adds is the bindless slot. Composited by the dome's own pixel shader, so it costs no extra draw.
    // EnsureInit() forces planets[1].mesh to exist, since this path never calls RenderSkyPre() itself.
    if ( auto loadedWorld = Engine::GAPI->GetLoadedWorldInfo(); loadedWorld && loadedWorld->MainWorld ) {
        if ( auto skyCtrl = loadedWorld->MainWorld->GetSkyControllerOutdoor(); skyCtrl ) {
            skyCtrl->EnsureInit();
        }
    }

    const MoonSpriteInfo moon = sky->ResolveMoonSprite( m_Resolution );

    SkyMaterialConsts matCb = {};
    matCb.CloudIndex = SkyTextureSlot( sky->GetCloudTextureGfx() );
    matCb.NightIndex = SkyTextureSlot( sky->GetNightTextureGfx() );
    matCb.MoonIndex = ZenTextureSlot( moon.Texture );
    matCb.MoonCenterPx[0] = moon.CenterPx[0];
    matCb.MoonCenterPx[1] = moon.CenterPx[1];
    matCb.MoonHalfSizePx[0] = moon.HalfSizePx[0];
    matCb.MoonHalfSizePx[1] = moon.HalfSizePx[1];
    matCb.MoonColor[0] = moon.Color[0];
    matCb.MoonColor[1] = moon.Color[1];
    matCb.MoonColor[2] = moon.Color[2];
    matCb.MoonColor[3] = moon.Color[3];
    m_CmdList->SetGraphicsRoot32BitConstants( 3, 12, &matCb, 0 );                     // b3 SkyMaterialCB

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // The dome is one sub-mesh in practice (unitSphere.obj), but GMesh splits at 0xFFFF vertices so loop.
    // Raw ExVertexStruct (stride 60) + 16-bit indices, like the particle prog-mesh path in D3D12Fx.cpp — the
    // buffers were created by MeshInfo::Create through the backend-neutral CreateVertexBuffer, so on D3D12
    // they already are D3D12VertexBuffers.
    unsigned int drawnIndices = 0;
    for ( MeshInfo* mesh : dome->GetMeshes() ) {
        if ( !mesh || mesh->Indices.empty() || !mesh->GetMeshVertexBuffer() || !mesh->GetMeshIndexBuffer() )
            continue;
        D3D12VertexBuffer* vb = D3D12VertexBuffer::From( mesh->GetMeshVertexBuffer() );
        D3D12VertexBuffer* ib = D3D12VertexBuffer::From( mesh->GetMeshIndexBuffer() );
        if ( !vb->GetResource() || !ib->GetResource() ) continue;

        const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
        const D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
        m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
        m_CmdList->IASetIndexBuffer( &ibv );

        const UINT numIndices = static_cast<UINT>( mesh->Indices.size() );
        m_CmdList->DrawIndexedInstanced( numIndices, 1, 0, 0, 0 );
        drawnIndices += numIndices;
    }
    if ( !drawnIndices ) return false;   // nothing was actually submitted — let the caller fall back

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnIndices / 3;
    return true;
}
