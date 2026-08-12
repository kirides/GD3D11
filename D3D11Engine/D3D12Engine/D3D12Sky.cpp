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
#include "../zCSkyController_Outdoor.h"
#include "../zCWorld.h"
#include "../zCMesh.h"
#include "../zCPolygon.h"
#include "../zCMaterial.h"
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

    // Everything the pixel shader needs to composite the moon, or MoonIndex == kNoSkyTexture to skip it.
    struct MoonSprite {
        UINT  SrvSlot = kNoSkyTexture;
        float CenterPx[2] = { 0.0f, 0.0f };
        float HalfSizePx[2] = { 0.0f, 0.0f };
        float Color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    // Port of the planets[1] half of zCSkyControler_Outdoor::RenderPlanets (ZenGin/_dieter/zSky_Outdoor.cpp
    // :1724). The FF sky draws each planet as a camera-facing 100x100-unit quad (zCMesh::CreateQuadMesh),
    // screen-projected from the planet's DIRECTION and back-projected onto that ray at view depth
    // `planet.size` (zCMesh::RenderDecal) — so `size` is a depth, and bigger `size` means a SMALLER moon.
    // Reproduced here as a screen-space sprite the sky dome's pixel shader composites, which costs no extra
    // draw at all. Every "moon not visible" branch RenderPlanets takes (below the horizon, behind the camera,
    // faded to nothing) returns the default, i.e. skip.
    //
    // Deliberately moon-only: the scattering dome already renders the sun as the Mie forward-scattering lobe,
    // so also stamping the FF sun sprite on top would double it.
    MoonSprite ResolveMoonSprite( const INT2& resolution ) {
        MoonSprite out;

        auto* worldInfo = Engine::GAPI->GetLoadedWorldInfo();
        if ( !worldInfo || !worldInfo->MainWorld ) return out;
        zCSkyController_Outdoor* sc = worldInfo->MainWorld->GetSkyControllerOutdoor();
        if ( !sc ) return out;

        zCSkyPlanet* moon = sc->GetPlanet( 1 );
        if ( !moon || !moon->mesh || moon->size <= 0.0f ) return out;

        // Direction: same time remap + rotation the FF sky uses, via this mod's own patched angle math
        // (GetMoonWorldPosition mirrors GetSunWorldPosition / FixMoonWorldPosition), so the moon stays in
        // step with the sun and with GSky's SkyTimeScale setting.
        XMFLOAT3 dirWS = sc->GetMoonWorldPosition( Engine::GAPI->GetSky()->GetAtmoshpereSettings().SkyTimeScale );
        const float dot = dirWS.y;      // RenderPlanets' `dot`: the planet's height over the horizon

        // RenderPlanets: `if (dot < -0.5) { if (i==1) continue; ... }` — the moon is simply not drawn.
        if ( dot < -0.5f ) return out;

        // Tint: color0 near the horizon -> color1 high up, both RGBA in 0..255. RenderPlanets' d0/d1 window.
        constexpr float d1 = 0.2f;
        constexpr float d0 = -0.3f;
        XMFLOAT4 res;
        if ( dot < d0 ) {
            return out;                 // `else continue;` for the moon
        } else if ( dot > d1 ) {
            res = moon->color1;
        } else {
            const float t = 1.0f - ( ( d1 - dot ) / ( d1 - d0 ) );
            res.x = moon->color0.x + t * ( moon->color1.x - moon->color0.x );
            res.y = moon->color0.y + t * ( moon->color1.y - moon->color0.y );
            res.z = moon->color0.z + t * ( moon->color1.z - moon->color0.z );
            res.w = moon->color0.w + t * ( moon->color1.w - moon->color0.w );
        }

        // Foggy days fade the planets out, and rain hides them entirely. Both verbatim from RenderPlanets.
        res.w -= ( 1.0f - dot ) * sc->GetResultFogScale() * 255.0f;
        res.w *= 1.0f - sc->GetRainFXWeight();
        if ( res.w <= 0.0f ) return out;

        // Texture: whatever frame the material currently holds (GetAniTexture, so a texture-animated moon
        // varies night to night on its own). Poly 0 carries the material — CreateQuadMesh builds exactly one.
        zCMesh* mesh = moon->mesh;
        if ( mesh->GetNumPolygons() <= 0 ) return out;
        zCPolygon** polys = mesh->GetPolygons();
        if ( !polys || !polys[0] ) return out;
        zCMaterial* mat = polys[0]->GetMaterial();
        if ( !mat ) return out;
        const UINT slot = ZenTextureSlot( mat->GetAniTexture() );
        if ( slot == kNoSkyTexture ) return out;

        // Placement. Uses the same transform convention as the god-ray sun projection in D3D12Fog.cpp
        // (transpose, then XMVector3TransformCoord) — that one is GPU-verified, so don't re-derive it.
        // The sprite's screen rect is computed by projecting the quad's CENTRE and its two EDGE midpoints
        // rather than by deriving a focal length from the projection matrix: convention-agnostic, and it
        // reproduces RenderDecal's geometry (half-extent 50 at view depth `size`) exactly.
        XMMATRIX view = XMMatrixTranspose( Engine::GAPI->GetViewMatrixXM() );
        XMMATRIX proj = XMMatrixTranspose( XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() ) );

        XMVECTOR worldPos = XMLoadFloat3( &dirWS ) * Engine::GAPI->GetSky()->GetAtmosphereCB().AC_OuterRadius;
        worldPos += Engine::GAPI->GetCameraPositionXM();

        XMFLOAT3 viewPos;
        XMStoreFloat3( &viewPos, XMVector3TransformCoord( worldPos, view ) );
        if ( viewPos.z <= 0.0f ) return out;      // RenderPlanets' `if (posCS[VZ] > 0)` guard

        // Put the quad where RenderDecal puts it: on the same view ray, at view depth `size`.
        const float scale = moon->size / viewPos.z;
        const XMVECTOR centreView = XMVectorSet( viewPos.x * scale, viewPos.y * scale, moon->size, 1.0f );
        constexpr float kQuadHalfExtent = 50.0f;   // CreateQuadMesh spans -50..+50 on both axes
        const XMVECTOR edgeXView = XMVectorAdd( centreView, XMVectorSet( kQuadHalfExtent, 0.0f, 0.0f, 0.0f ) );
        const XMVECTOR edgeYView = XMVectorAdd( centreView, XMVectorSet( 0.0f, kQuadHalfExtent, 0.0f, 0.0f ) );

        XMFLOAT3 c, ex, ey;
        XMStoreFloat3( &c,  XMVector3TransformCoord( centreView, proj ) );
        XMStoreFloat3( &ex, XMVector3TransformCoord( edgeXView, proj ) );
        XMStoreFloat3( &ey, XMVector3TransformCoord( edgeYView, proj ) );

        const float w = static_cast<float>( resolution.x );
        const float h = static_cast<float>( resolution.y );
        out.CenterPx[0] = ( c.x * 0.5f + 0.5f ) * w;
        out.CenterPx[1] = ( c.y * -0.5f + 0.5f ) * h;
        out.HalfSizePx[0] = std::abs( ex.x - c.x ) * 0.5f * w;
        out.HalfSizePx[1] = std::abs( ey.y - c.y ) * 0.5f * h;
        if ( out.HalfSizePx[0] < 0.5f || out.HalfSizePx[1] < 0.5f ) return out;   // sub-pixel: nothing to draw

        out.SrvSlot = slot;
        out.Color[0] = res.x / 255.0f;
        out.Color[1] = res.y / 255.0f;
        out.Color[2] = res.z / 255.0f;
        out.Color[3] = std::min( res.w / 255.0f, 1.0f );
        return out;
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
    // not. Composited by the dome's own pixel shader, so it costs no additional draw call.
    const MoonSprite moon = ResolveMoonSprite( m_Resolution );

    SkyMaterialConsts matCb = {};
    matCb.CloudIndex = SkyTextureSlot( sky->GetCloudTextureGfx() );
    matCb.NightIndex = SkyTextureSlot( sky->GetNightTextureGfx() );
    matCb.MoonIndex = moon.SrvSlot;
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
