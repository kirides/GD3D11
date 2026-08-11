// D3D12GraphicsEngine — the frame's sorted transparency queue and the alpha-blended world-mesh surfaces
// (ice, glass, magic barriers, waterfall foam). Port of D3D11's DrawWorldTransparencyRun + the queue.
//
// Three lists feed the WorldMesh kind, selected by the item's SubKind:
//   * g_FrameWorldTransparency       — real blend alpha func (ice, glass)      -> PS_Simple_FF
//   * g_FrameWorldTransparencyPortal — MT_Portal, gated on DrawG1ForestPortals -> PS_PortalDiffuse
//   * g_FrameWorldTransparencyFoam   — MT_WaterfallFoam                        -> PS_WaterfallFoam
// The latter two are collected by material TYPE regardless of alpha func, which is what keeps
// ResolveAlphaFunc's "0 -> derive from the material color's alpha" fixup live.
//
// Deliberate divergences from D3D11, documented at their site: the vertex color is swizzled .bgra and the
// sampled texel is linearized (D3D12's scene target is linear HDR).
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12Texture.h"
#include "D3D12VertexBuffer.h"
#include "D3D12PipelineState.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../GSky.h"
#include "../WorldObjects.h"
#include "../zCMaterial.h"
#include "../zCTexture.h"
#include "../zCVob.h"          // CollectTransparencyQueue: decal world positions
#include "../zCQuadMark.h"     // CollectTransparencyQueue: quad-mark connected vob
#include "../D3D7/MyDirectDrawSurface7.h"

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

// Declared in D3D12EngineCommon.h; filled by BuildWorldDrawCommands (D3D12Scene.cpp), drained here.
std::vector<WorldTransparencyMesh> g_FrameWorldTransparency;
std::vector<WorldTransparencyMesh> g_FrameWorldTransparencyPortal;
std::vector<WorldTransparencyMesh> g_FrameWorldTransparencyFoam;

// Declared in D3D12EngineCommon.h; filled by BuildVobDrawCommands (D3D12Scene.cpp), drained by
// DrawVobAlphaRun below.
std::vector<VobAlphaMesh> g_FrameVobAlpha;

namespace {
    // alphaFunc 0 (MAT_DEFAULT) means "whatever the material color says": translucent -> blend.
    int ResolveAlphaFunc( zCMaterial* mat ) {
        int alphaFunc = mat->GetAlphaFunc();
        if ( alphaFunc == zMAT_ALPHA_FUNC_MAT_DEFAULT ) {
            alphaFunc = zColor( mat->GetColor() ).bgra.alpha < 255
                ? zMAT_ALPHA_FUNC_BLEND
                : zMAT_ALPHA_FUNC_MAT_DEFAULT;
        }
        return alphaFunc;
    }

    // Material color contributes ALPHA only: ZenGin's base stage is rgbGen=VERTEX / alphaGen=FACTOR
    // (zRenderManager.cpp:601-610), and its RGB is for untextured polys, which never get here. Multiplying
    // the RGB in made dark-tinted additive surfaces (the magic barrier) vanish.
    float4 ComputeTextureFactor( zCMaterial* mat ) {
        // RGB carries the day/night factor: unlit surfaces over a baked-daylight vertex color would
        // otherwise stay noon-bright at midnight. 1.0 in daylight. See GothicAPI::GetSkyDayFactor.
        const float skyLight = Engine::GAPI->GetSkyDayFactor();
        return float4( skyLight, skyLight, skyLight,
            zColor( mat->GetColor() ).bgra.alpha * (1.0f / 255.0f) );
    }

    // False on the `default:` arm, where D3D11 leaves the current blend state alone.
    bool BlendStateForAlphaFunc( int alphaFunc, GothicBlendStateInfo& blend ) {
        switch ( alphaFunc ) {
        case zMAT_ALPHA_FUNC_BLEND:
        case zMAT_ALPHA_FUNC_BLEND_TEST:
            blend.SetAlphaBlending();
            return true;
        case zMAT_ALPHA_FUNC_ADD:
            blend.SetAdditiveBlending();
            return true;
        case zMAT_ALPHA_FUNC_MUL:
            blend.SetModulateBlending();
            return true;
        case zMAT_ALPHA_FUNC_MUL2:
            blend.SetModulate2Blending();
            return true;
        default:
            return false;
        }
    }

    // The three lists the queue's WorldMesh SubKind selects between.
    std::vector<WorldTransparencyMesh>& WorldTransparencyListFor( EWorldTransparencyVariant variant ) {
        switch ( variant ) {
        case EWorldTransparencyVariant::Portal:    return g_FrameWorldTransparencyPortal;
        case EWorldTransparencyVariant::Waterfall: return g_FrameWorldTransparencyFoam;
        default:                                   return g_FrameWorldTransparency;
        }
    }
}


bool D3D12GraphicsEngine::IsWorldMeshAlphaBlended( zCMaterial* mat ) {
    // Any real blend mode is peeled out of the opaque set. TEST is a cutout, not a blend; MAT_DEFAULT (0)
    // only blends for the types collected by MaterialType (portals, foam), which have their own branches.
    if ( !mat ) return false;
    const int alphaFunc = mat->GetAlphaFunc();
    return alphaFunc > zMAT_ALPHA_FUNC_NONE && alphaFunc != zMAT_ALPHA_FUNC_TEST;
}


/** One run of alpha-blended world mesh sections. The three lists are just the payload store now; SubKind
    picks between them and runs interleave with every other blended kind. */
void D3D12GraphicsEngine::DrawWorldTransparencyRun( std::span<const TransparentItem> items,
    EWorldTransparencyVariant variant ) {
    using EKind = D3D12PipelineState::WorldTransparencyPipeline::EKind;

    if ( items.empty() ) return;
    if ( !m_FrameOpen || !m_Pipelines.WorldTransparency.RootSig || !m_DepthBuffer ) return;

    const EKind kind =
        variant == EWorldTransparencyVariant::Portal    ? EKind::Portal :
        variant == EWorldTransparencyVariant::Waterfall ? EKind::Foam   : EKind::Simple;

    if ( variant == EWorldTransparencyVariant::Portal ) {
        // Gated the same way D3D11 gates its forest portals; the blob check keeps a failed shader compile
        // from dropping into the wrong pixel shader.
        if ( !Engine::GAPI->GetRendererState().RendererSettings.DrawG1ForestPortals
            || !m_Pipelines.WorldTransparency.PortalVsBlob || !m_Pipelines.WorldTransparency.PortalPsBlob ) {
            return;
        }
    }

    if ( !BindWorldTransparencyFrameState() ) return;

    const TransparencyQueue& queue = Engine::GAPI->GetTransparencyQueue();
    const std::vector<WorldTransparencyMesh>& list = WorldTransparencyListFor( variant );
    const bool readsTextureFactor = ( kind == EKind::Simple );   // PS_PortalDiffuse/PS_WaterfallFoam ignore it

    // D3D11's state machine, reproduced exactly: each list starts from SetDefaultStates() (no blending,
    // depth-write ON) and the FIRST alpha-func change both selects a blend mode and turns depth-write off
    // for the rest of the list. Materials whose effective alpha func is 0 therefore draw opaque + depth-
    // writing when they come first — order-dependent, but faithful. That case is live for portals/foam,
    // which are collected by TYPE and so may well carry alpha func 0.
    // Env-map overlay stage (see ComputeEnvMapAlpha). Simple variant only — portals and foam are their own
    // effects, and ZenGin never env-maps a portal. Both blobs are required because
    // GetOrCreateWorldTransparencyPipeline degrades a missing blob to the plain shader, which here would
    // re-draw the base surface.
    const bool envOverlayAvailable = kind == EKind::Simple
        && m_Pipelines.WorldTransparency.EnvVsBlob && m_Pipelines.WorldTransparency.EnvPsBlob
        && m_ReflectionCubeSrvSlot != UINT_MAX;
    const XMFLOAT3 camPosWS = Engine::GAPI->GetCameraPosition();

    GothicBlendStateInfo blend;
    blend.SetDefault();
    bool depthWrite = true;
    int lastAlphaFunc = zMAT_ALPHA_FUNC_MAT_DEFAULT;

    ID3D12PipelineState* pso = m_Pipelines.GetOrCreateWorldTransparencyPipeline( blend, depthWrite, kind );
    if ( !pso ) return;
    m_CmdList->SetPipelineState( pso );

    unsigned int drawnIndices = 0;
    zCMaterial* lastMat = nullptr;
    for ( const TransparentItem& item : items ) {
        const uint32_t entryIndex = queue.GetIndices( item ).BatchIndex;
        if ( entryIndex >= list.size() ) continue;
        const WorldTransparencyMesh& entry = list[entryIndex];

        zCMaterial* mat = entry.Material;
        if ( !mat || !entry.Mesh || entry.Mesh->Indices.empty() ) continue;

        zCTexture* tex = mat->GetAniTexture();
        if ( !tex || tex->CacheIn( 0.6f ) != zRES_CACHED_IN ) continue;   // "Draw what? black? :)"

        // Bindless diffuse (b6.MatDiffuseIndex). No usable texture -> skip rather than draw a black
        // slab over the scene; the other three material indices are unread by these pixel shaders.
        uint32_t mat6[4] = { 0xFFFFFFFFu, 0u, 0u, 0u };
        bool haveDiffuse = false;
        if ( MyDirectDrawSurface7* s = tex->GetSurface() ) {
            if ( GfxTexture* gfx = s->GetEngineTexture() ) {
                D3D12Texture* d = D3D12Texture::From( gfx );
                if ( d->HasSRV() ) { mat6[2] = d->GetSrvSlot(); haveDiffuse = true; }
            }
        }
        if ( !haveDiffuse ) continue;

        const int alphaFunc = ResolveAlphaFunc( mat );
        if ( lastAlphaFunc != alphaFunc ) {
            BlendStateForAlphaFunc( alphaFunc, blend );   // the `default:` arm keeps the current blend state
            depthWrite = false;
            lastAlphaFunc = alphaFunc;
            ID3D12PipelineState* next = m_Pipelines.GetOrCreateWorldTransparencyPipeline( blend, depthWrite, kind );
            if ( !next ) continue;
            m_CmdList->SetPipelineState( next );
        }

        if ( readsTextureFactor && lastMat != mat ) {
            // Writes b5.xyzw only — the sun height in the 5th DWORD was set once by the caller and must survive.
            const float4 factor = ComputeTextureFactor( mat );
            m_CmdList->SetGraphicsRoot32BitConstants( 1, 4, &factor, 0 );
            lastMat = mat;
        }
        m_CmdList->SetGraphicsRoot32BitConstants( 2, 4, mat6, 0 );          // b6 MaterialCB

        m_CmdList->DrawIndexedInstanced( static_cast<UINT>( entry.Mesh->Indices.size() ), 1,
            entry.Mesh->BaseIndexLocation, 0, 0 );
        drawnIndices += static_cast<unsigned int>( entry.Mesh->Indices.size() );

        // ZenGin appends the env-map stage to the SAME zCShader (zRenderManager.cpp:671), so the overlay
        // goes inline here rather than as a second sweep, keeping the painter's order intact.
        if ( envOverlayAvailable && mat->GetEnvMapEnabled() ) {
            GothicBlendStateInfo envBlend;
            // Water gets an additive stage in ZenGin (zRenderManager.cpp:709); everything else blends.
            if ( mat->GetMatGroup() == zMAT_GROUP_WATER ) envBlend.SetAdditiveBlending();
            else                                         envBlend.SetAlphaBlending();

            ID3D12PipelineState* envPso = m_Pipelines.GetOrCreateWorldTransparencyPipeline(
                envBlend, false, EKind::Env );
            if ( envPso ) {
                m_CmdList->SetPipelineState( envPso );

                // b5 tail: [4] SunHeight is left alone (PSTransparentEnv doesn't read it), [5..7] camera
                // world position, [8] the bindless cube slot. TextureFactor.a carries the stage alpha.
                const float envAlpha = Engine::GAPI->GetEnvMapStageAlpha( mat );
                const float4 envFactor( 1.0f, 1.0f, 1.0f, envAlpha );
                m_CmdList->SetGraphicsRoot32BitConstants( 1, 4, &envFactor, 0 );
                m_CmdList->SetGraphicsRoot32BitConstants( 1, 3, &camPosWS, 5 );
                m_CmdList->SetGraphicsRoot32BitConstants( 1, 1, &m_ReflectionCubeSrvSlot, 8 );

                m_CmdList->DrawIndexedInstanced( static_cast<UINT>( entry.Mesh->Indices.size() ), 1,
                    entry.Mesh->BaseIndexLocation, 0, 0 );
                drawnIndices += static_cast<unsigned int>( entry.Mesh->Indices.size() );

                // The overlay replaced the PSO and overwrote b5's TextureFactor, so the next item has to
                // re-establish both instead of hitting these caches.
                ID3D12PipelineState* restore = m_Pipelines.GetOrCreateWorldTransparencyPipeline( blend, depthWrite, kind );
                if ( restore ) m_CmdList->SetPipelineState( restore );
                lastMat = nullptr;
            }
        }
    }

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnIndices / 3;
}


/** Depth of this frame's world transparency surfaces, color writes off. ONCE after the whole replay, or a
    nearer surface would depth-reject a farther one drawn later. The fog/god-ray passes reconstruct world
    positions from this. MT_Portal stays out - a fade-in curtain must not push fog depth to itself. */
void D3D12GraphicsEngine::DrawWorldTransparencyDepthOnly() {
    if ( !m_FrameOpen || !m_Pipelines.WorldTransparency.DepthFillPSO || !m_DepthBuffer ) return;
    if ( g_FrameWorldTransparency.empty() && g_FrameWorldTransparencyFoam.empty() ) return;
    if ( !BindWorldTransparencyFrameState() ) return;

    DX_ZONE( m_CmdList.Get(), "World transparency (depth re-lay)" );

    m_CmdList->SetPipelineState( m_Pipelines.WorldTransparency.DepthFillPSO.Get() );
    // The depth-fill PSO reuses PSTransparent (color writes masked off) rather than a null PS, so the
    // shader still runs its bindless ResourceDescriptorHeap[MatDiffuseIndex] fetch even though the
    // result is discarded. Stamp a known-good slot: the loop below is less strict than the color loop
    // (it accepts materials whose texture failed to cache in), so b6 could otherwise still hold
    // uninitialized root constants if the color loop drew nothing at all — an out-of-range descriptor
    // index, i.e. a GPU fault, not just a wrong pixel.
    const uint32_t safeMat6[4] = { 0xFFFFFFFFu, 0u, m_BlackTexture->GetSrvSlot(), 0u };
    m_CmdList->SetGraphicsRoot32BitConstants( 2, 4, safeMat6, 0 );

    for ( const std::vector<WorldTransparencyMesh>* list :
        { &g_FrameWorldTransparency, &g_FrameWorldTransparencyFoam } ) {
        for ( const WorldTransparencyMesh& entry : *list ) {
            if ( !entry.Material || !entry.Mesh || entry.Mesh->Indices.empty() ) continue;
            if ( !entry.Material->GetAniTexture() ) continue;
            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( entry.Mesh->Indices.size() ), 1,
                entry.Mesh->BaseIndexLocation, 0, 0 );
        }
    }
}


/** Root signature, b0/b3/b5, viewport and the shared world VB/IB. Re-done per run, since the queue
    interleaves kinds that bind their own root signature. False = cannot draw. */
bool D3D12GraphicsEngine::BindWorldTransparencyFrameState() {
    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    if ( !wm || !wm->GetMeshVertexBuffer() || !wm->GetMeshIndexBuffer() ) return false;
    D3D12VertexBuffer* vb = D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() );
    D3D12VertexBuffer* ib = D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() );
    if ( !vb->GetResource() || !ib->GetResource() ) return false;

    // ViewProj — identical derivation to DrawWorldMesh/DrawWaterSurfaces (world verts are world-space).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetGraphicsRootSignature( m_Pipelines.WorldTransparency.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );   // b0 ViewProj
    // b4 world->view: only VSTransparentPortal reads it, but binding it unconditionally keeps the root
    // parameter initialized for every PSO on this signature. Uploaded verbatim like every other matrix here
    // (row-major stored, read column-major in HLSL — see CLAUDE.md), matching D3D11's VS_Ex M_View.
    m_CmdList->SetGraphicsRoot32BitConstants( 3, 16, &viewM, 0 );

    // b5 in full, once: the sun height must be valid even for the lists that never rewrite the texture
    // factor (portals/foam). GSky::RenderSky refreshes AC_LightPos every frame and DrawSky has already run
    // by now — same reasoning as RenderFogAndGodRays/DrawWaterSurfaces. The texture-factor half is then
    // overwritten per material by the Simple list.
    GSky* sky = Engine::GAPI->GetSky();
    struct TransparencyCBData { float4 TextureFactor; float SunHeight; float Pad[3]; } tcb = {};
    tcb.TextureFactor = float4( 1.0f, 1.0f, 1.0f, 1.0f );
    tcb.SunHeight = sky ? sky->GetAtmosphereCB().AC_LightPos.y : 0.0f;
    m_CmdList->SetGraphicsRoot32BitConstants( 1, 8, &tcb, 0 );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
    D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    m_CmdList->IASetIndexBuffer( &ibv );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    return true;
}


void D3D12GraphicsEngine::DrawVobAlphaRun( std::span<const TransparentItem> items ) {
    // Port of D3D11GraphicsEngine::DrawFrameAlphaMeshes for the instanced-VOB path: cobwebs, hanging cloth and
    // magic sheets whose material carries a BLEND/ADD alpha func.
    //
    // They need their own pass because the opaque VOB set is one ExecuteIndirect through a PSO that alpha-
    // CLIPS at 0.5, writes depth and returns alpha 1 — which turns a spider web's half-transparent texel into
    // an opaque white one. Drawn here blended, unlit and depth-tested but NOT depth-writing.
    // BuildVobDrawCommands does the peeling (so they miss the VOB depth prepass too, as a blended surface
    // wants) and resolves every buffer view, leaving this a short CPU draw loop.
    //
    // Ordered by the frame's transparency queue now, at batch granularity (see VobAlphaMesh::DistanceSq for
    // why this backend cannot go per instance the way D3D11 does).
    if ( items.empty() ) return;
    if ( !m_FrameOpen || !m_Pipelines.World.RootSig || !m_Pipelines.World.VobAlphaBlendPSO || !m_DepthBuffer ) {
        return;
    }

    const TransparencyQueue& queue = Engine::GAPI->GetTransparencyQueue();

    DX_ZONE( m_CmdList.Get(), "DrawVobAlphaRun" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw Vobs (blended)" );

    // ViewProj — identical derivation to DrawVobsInstanced, so a peeled mesh lands on exactly the pixels it
    // would have landed on in the opaque pass.
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );   // b0 ViewProj
    // The full Forward+ set: PSAlphaBlendBindless shades exactly like the opaque PSMainBindless (see Vob.hlsl
    // for why these surfaces have to be lit rather than emitted at full albedo), so it needs every root
    // parameter DrawVobsInstanced binds. Fog is bound for the VS's CamPosWS even though the PS drops the term.
    const FogConstants fog = MakeSceneFogConstants();
    m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );                                       // b1 fog
    BindFrameLights();                                                                               // 3..5
    m_CmdList->SetGraphicsRootConstantBufferView( 7, m_ShadowCBGpu[m_FrameIndex] );                  // b3 shadow CB
    m_CmdList->SetGraphicsRootDescriptorTable( 8, GetSrvGpuHandle( m_ShadowMap.GetSrvSlot() ) );     // t4 CSM
    m_CmdList->SetGraphicsRootDescriptorTable( 9, GetSrvGpuHandle( m_PointShadows.GetSrvSlot() ) );  // t5 cubes
    // b4 WindCB: the frame-global half (dir/time/playerPos); min/maxHeight (@4,5) are stamped per entry below,
    // exactly as the indirect commands do. Must be bound before the first draw — VSMain reads b4 for the sway.
    m_CmdList->SetGraphicsRoot32BitConstants( 11, 12, &m_WindBuffer, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 12, 1, &m_ActiveAOMaskSrvSlot, 0 );                    // b7 AOCB

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // ADD falls back to the plain blend PSO when its own failed to create (see CreateVob) — a slightly wrong
    // blend beats dropping the geometry.
    ID3D12PipelineState* const blendPso = m_Pipelines.World.VobAlphaBlendPSO.Get();
    ID3D12PipelineState* const addPso = m_Pipelines.World.VobAlphaAddPSO
        ? m_Pipelines.World.VobAlphaAddPSO.Get() : blendPso;

    ID3D12PipelineState* current = nullptr;
    unsigned int drawnTriangles = 0;
    for ( const TransparentItem& item : items ) {
        const uint32_t entryIndex = queue.GetIndices( item ).BatchIndex;
        if ( entryIndex >= g_FrameVobAlpha.size() ) continue;
        const VobAlphaMesh& e = g_FrameVobAlpha[entryIndex];
        if ( e.NumInstances == 0 || e.IndexCount == 0 ) continue;

        ID3D12PipelineState* want = e.Additive ? addPso : blendPso;
        if ( want != current ) {
            m_CmdList->SetPipelineState( want );
            current = want;
        }

        const float windHeights[2] = { e.WindMinHeight, e.WindMaxHeight };
        m_CmdList->SetGraphicsRoot32BitConstants( 10, 3, e.MatIndices, 0 );              // b6 MaterialCB
        m_CmdList->SetGraphicsRoot32BitConstants( 11, 2, windHeights, 4 );               // b4[4..5] per visual

        const D3D12_VERTEX_BUFFER_VIEW vbs[2] = { e.MeshVBV, e.InstVBV };
        m_CmdList->IASetVertexBuffers( 0, 2, vbs );
        m_CmdList->IASetIndexBuffer( &e.IBV );
        m_CmdList->DrawIndexedInstanced( e.IndexCount, e.NumInstances, 0, 0, 0 );
        drawnTriangles += ( e.IndexCount / 3 ) * e.NumInstances;
    }

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnTriangles;
}



/** Fills the queue from this backend's per-kind lists and sorts it. World meshes and alpha VOB batches were
    collected during the command build; the rest is gathered here. Payloads are plain indices. */
void D3D12GraphicsEngine::CollectTransparencyQueue() {
    TransparencyQueue& queue = Engine::GAPI->GetTransparencyQueue();
    queue.BeginFrame();

    auto& renderSettings = Engine::GAPI->GetRendererState().RendererSettings;
    const XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();

    // One item per entry of each of the three lists
    const std::pair<const std::vector<WorldTransparencyMesh>*, EWorldTransparencyVariant> worldLists[] = {
        { &g_FrameWorldTransparency,       EWorldTransparencyVariant::Normal },
        { &g_FrameWorldTransparencyPortal, EWorldTransparencyVariant::Portal },
        { &g_FrameWorldTransparencyFoam,   EWorldTransparencyVariant::Waterfall },
    };
    for ( auto const& [list, variant] : worldLists ) {
        for ( size_t i = 0; i < list->size(); ++i ) {
            const WorldTransparencyMesh& entry = ( *list )[i];
            queue.AddIndexed( entry.DistanceSq, ETransparentKind::WorldMesh, static_cast<uint8_t>( variant ),
                static_cast<uint32_t>( i ), 0, TransparencyQueue::MakeBatchKey( entry.Material ) );
        }
    }

    // One item per batch (see VobAlphaMesh::DistanceSq)
    for ( size_t i = 0; i < g_FrameVobAlpha.size(); ++i ) {
        queue.AddIndexed( g_FrameVobAlpha[i].DistanceSq, ETransparentKind::AlphaVob, 0,
            static_cast<uint32_t>( i ), 0, g_FrameVobAlpha[i].MatIndices[2] );
    }

    // TransparencyVobInfo::distance is linear; the queue sorts on squared distances.
    auto& transparencyVobs = Engine::GAPI->GetTransparencyVobs();
    for ( size_t i = 0; i < transparencyVobs.size(); ++i ) {
        const float distance = transparencyVobs[i].distance;
        queue.AddGhost( distance * distance, static_cast<uint32_t>( i ) );
    }

    if ( renderSettings.DrawParticleEffects ) {
        // Blended decals only; the opaque/alpha-test ones drew with the opaque scene.
        static std::vector<zCVob*> decals;   // static to get around reallocations
        decals.clear();
        Engine::GAPI->GetVisibleDecalList( decals );
        for ( zCVob* decal : decals ) {
            float distanceSq;
            XMStoreFloat( &distanceSq, XMVector3LengthSq( decal->GetPositionWorldXM() - camPos ) );
            queue.AddDecal( distanceSq, decal );
        }

        const float vfxRadiusSq = renderSettings.VisualFXDrawRadius * renderSettings.VisualFXDrawRadius;
        for ( auto const& it : Engine::GAPI->GetQuadMarks() ) {
            if ( !it.first->GetConnectedVob() ) continue;

            float distanceSq;
            XMStoreFloat( &distanceSq, XMVector3LengthSq( camPos - XMLoadFloat3( &it.second.Position ) ) );
            if ( distanceSq > vfxRadiusSq ) continue;

            queue.AddQuadMark( distanceSq, it.first, &it.second );
        }
    }

#if (defined BUILD_GOTHIC_2_6_fix || defined BUILD_GOTHIC_1_08k)
    // Mesh data has to exist before we can take a depth for it.
    Engine::GAPI->CalcPolyStripMeshes();   // weapon/effect trails
    Engine::GAPI->CalcFlashMeshes();       // lightning flashes

    for ( auto const& it : Engine::GAPI->GetPolyStripInfos() ) {
        const std::vector<ExVertexStruct>& vertices = it.second.vertices;
        if ( vertices.empty() || !it.second.material || !it.first ) continue;

        // One group per texture -> centroid depth
        XMVECTOR center = XMVectorZero();
        for ( auto const& vertex : vertices ) {
            center = XMVectorAdd( center, XMLoadFloat3( &vertex.Position ) );
        }
        center = XMVectorScale( center, 1.0f / static_cast<float>( vertices.size() ) );

        float distanceSq;
        XMStoreFloat( &distanceSq, XMVector3LengthSq( center - camPos ) );
        queue.AddPolyStrip( distanceSq, it.first, &it.second );
    }
#endif

    // categoryMajor reproduces the old per-category pass order without a second set of emitters
    queue.Sort( !renderSettings.SortedTransparency );
}


/** Replays the sorted queue, one call per maximal same-kind run. */
void D3D12GraphicsEngine::DrawTransparencyQueue() {
    const TransparencyQueue& queue = Engine::GAPI->GetTransparencyQueue();

    if ( m_FrameOpen && !queue.Empty() ) {
        DX_ZONE( m_CmdList.Get(), "DrawTransparencyQueue" );
        TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw transparency (sorted)" );

        queue.ForEachRun( [&]( ETransparentKind kind, uint8_t subKind,
            std::span<const TransparentItem> items ) {
                switch ( kind ) {
                case ETransparentKind::WorldMesh:
                    DrawWorldTransparencyRun( items, static_cast<EWorldTransparencyVariant>( subKind ) );
                    break;
                case ETransparentKind::AlphaVob:
                    DrawVobAlphaRun( items );
                    break;
                case ETransparentKind::Ghost:
                    DrawGhostRun( items );
                    break;
                case ETransparentKind::Decal:
                    DrawDecalRun( items );
                    break;
                case ETransparentKind::QuadMark:
                    DrawQuadMarkRun( items );
                    break;
                case ETransparentKind::PolyStrip:
                    DrawPolyStripRun( items );
                    break;
                default:
                    break;
                }
            } );

        // Depth of the world transparency surfaces, once, after everything blended has been drawn
        DrawWorldTransparencyDepthOnly();
    }

    // Unconditional: single-frame lists nothing else drains, so an early-out must still empty them.
    g_FrameWorldTransparency.clear();
    g_FrameWorldTransparencyPortal.clear();
    g_FrameWorldTransparencyFoam.clear();
    g_FrameVobAlpha.clear();
    Engine::GAPI->GetTransparencyVobs().clear();
}


/** One run of blended decals; DrawDecalList already batches same-material runs without reordering. */
void D3D12GraphicsEngine::DrawDecalRun( std::span<const TransparentItem> items ) {
    if ( items.empty() ) return;

    const TransparencyQueue& queue = Engine::GAPI->GetTransparencyQueue();

    static std::vector<zCVob*> decalRun;   // static to get around reallocations
    decalRun.clear();
    decalRun.reserve( items.size() );
    for ( const TransparentItem& item : items ) {
        decalRun.push_back( queue.GetDecal( item ) );
    }

    DrawDecalList( decalRun, false );
}
