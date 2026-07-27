// D3D12GraphicsEngine — alpha-blended world-mesh surfaces (ice, glass, magic barriers, water-fall style
// overlays). This is the port of D3D11GraphicsEngine::DrawMeshInfoListAlphablended
// (D3D11GraphicsEngine.cpp:4840) plus the peel-out half of its DrawWorldMesh mesh-list build
// (D3D11GraphicsEngine.cpp:5138).
//
// Why the surfaces need their own pass at all: they were previously swept into D3D12's opaque world
// ExecuteIndirect command set, which renders every material fully opaque with a fixed alpha-test cutout —
// so Gothic's ice slabs read as solid rock. D3D11 instead peels any material whose alpha func is a real
// blend mode out of the opaque set entirely, sorts it back-to-front, and draws it after the opaque scene
// (and after water) with the material's own blend mode and depth-write off.
//
// Shape of the pass, mirrored one-to-one from D3D11:
//   1. per-material: derive the effective alpha func (0 == "material default" -> BLEND when the material
//      color's alpha is < 255, else stay opaque)
//   2. compute the FF `textureFactor` tint — the material color when translucent, or the env-map strength
//      ramped by the sun height for env-mapped materials
//   3. draw with the blend mode that alpha func maps to, depth-tested but NOT depth-writing
//   4. re-draw the whole list depth-only (color writes off, depth-write on) so the depth-reading post
//      passes (height fog, god rays) see these surfaces instead of whatever is behind them
//
// Deliberate divergences from D3D11, both documented at their site: the vertex color is swizzled .bgra
// (D3D11's PS_Simple reads the BGRA DWORD unswizzled, i.e. with R/B transposed), and the sampled texel is
// linearized because D3D12's scene target is linear HDR.
//
// NOT ported here (still the same gap D3D12 had before): MT_Portal surfaces (D3D11's
// FrameTransparencyMeshesPortal / PS_PortalDiffuse, gated on DrawG1ForestPortals) are still skipped
// entirely, and MT_WaterfallFoam (PS_WaterfallFoam) still renders through the opaque path. Both reuse this
// exact machinery — they need one extra pixel shader each plus AC_LightPos on the CB.
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
#include "../D3D7/MyDirectDrawSurface7.h"

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

// Declared in D3D12EngineCommon.h; filled by BuildWorldDrawCommands (D3D12Scene.cpp), drained here.
std::vector<WorldTransparencyMesh> g_FrameWorldTransparency;

namespace {
    // D3D11 spec: DrawMeshInfoListAlphablended's `alphaFunc == 0` fixup. zMAT_ALPHA_FUNC_MAT_DEFAULT (0)
    // means "whatever the material color says": translucent color -> blend, opaque color -> leave it alone.
    int ResolveAlphaFunc( zCMaterial* mat ) {
        int alphaFunc = mat->GetAlphaFunc();
        if ( alphaFunc == zMAT_ALPHA_FUNC_MAT_DEFAULT ) {
            alphaFunc = zColor( mat->GetColor() ).bgra.alpha < 255
                ? zMAT_ALPHA_FUNC_BLEND
                : zMAT_ALPHA_FUNC_MAT_DEFAULT;
        }
        return alphaFunc;
    }

    // D3D11 spec: the `ffdata.textureFactor` block of DrawMeshInfoListAlphablended, verbatim. Env-mapped
    // materials get a white tint whose ALPHA is the env-map strength ramped by how high the sun stands
    // (that is what makes ice/glass reflective at noon and near-invisible at night); everything else uses
    // the material color when it is translucent, and stays untinted otherwise.
    float4 ComputeTextureFactor( zCMaterial* mat ) {
        if ( mat->GetEnvMapEnabled() ) {
            GSky* sky = Engine::GAPI->GetSky();
            const float sunHeight = sky ? sky->GetAtmosphereCB().AC_LightPos.y : 0.0f;
            if ( sunHeight > 0 ) {
                // sun is up
                const float maxSunHeight = 1.0f;
                const float lerpFactor = std::clamp( sunHeight / maxSunHeight, 0.0f, 1.0f );

                const float minIntensity = 0.1f;
                const float maxIntensity = 0.7f;
                const float currentIntensity = std::clamp(
                    mat->GetEnvMapStrength() * std::lerp( minIntensity, maxIntensity, lerpFactor ), 0.0f, 1.0f );
                return zColor( 255, 255, 255, static_cast<uint8_t>( 255.0f * currentIntensity ) ).ToFloat4();
            }
            return zColor( 255, 255, 255,
                static_cast<uint8_t>( 255.0f * mat->GetEnvMapStrength() * 0.1f ) ).ToFloat4();
        }
        if ( zColor( mat->GetColor() ).bgra.alpha < 255 ) {
            return zColor( mat->GetColor() ).ToFloat4();
        }
        return float4( 1.0f, 1.0f, 1.0f, 1.0f );
    }

    // D3D11 spec: the alpha-func -> blend-state switch in DrawMeshInfoListAlphablended. Returns false for
    // the `default:` arm, which in D3D11 leaves the previously-set blend state alone (it only turns
    // depth-write off) — the caller reproduces that by keeping its current blend state.
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
}


bool D3D12GraphicsEngine::IsWorldMeshAlphaBlended( zCMaterial* mat ) {
    // D3D11 spec: the "Check for alphablending" test in DrawWorldMesh's BuildMeshList — any real blend mode
    // (BLEND/ADD/SUB/MUL/MUL2/BLEND_TEST) is peeled out of the opaque set. zMAT_ALPHA_FUNC_TEST is a cutout,
    // not a blend, and stays opaque; zMAT_ALPHA_FUNC_MAT_DEFAULT (0) does too, because the material color's
    // alpha only becomes a blend factor for the material types D3D11 collects by MaterialType (portals,
    // waterfall foam) — neither of which reaches this pass yet.
    if ( !mat ) return false;
    const int alphaFunc = mat->GetAlphaFunc();
    return alphaFunc > zMAT_ALPHA_FUNC_NONE && alphaFunc != zMAT_ALPHA_FUNC_TEST;
}


void D3D12GraphicsEngine::SortWorldTransparencyMeshes() {
    // D3D11 spec: sortAndAppendTransparencyMeshes. Painter's order — farthest first — with a stable
    // material/texture/index tiebreak so same-distance surfaces don't shuffle between frames (which would
    // flicker, since these are blended).
    if ( g_FrameWorldTransparency.empty() ) return;
    std::sort( g_FrameWorldTransparency.begin(), g_FrameWorldTransparency.end(),
        []( const WorldTransparencyMesh& a, const WorldTransparencyMesh& b ) {
            if ( a.DistanceSq > b.DistanceSq ) return true;
            if ( a.DistanceSq < b.DistanceSq ) return false;
            if ( a.Material != b.Material ) return a.Material < b.Material;
            const unsigned int aBase = a.Mesh ? a.Mesh->BaseIndexLocation : 0u;
            const unsigned int bBase = b.Mesh ? b.Mesh->BaseIndexLocation : 0u;
            return aBase < bBase;
        } );
}


void D3D12GraphicsEngine::DrawWorldTransparencyMeshes() {
    if ( g_FrameWorldTransparency.empty() ) return;
    if ( !m_FrameOpen || !m_Pipelines.WorldTransparency.RootSig || !m_DepthBuffer ) {
        g_FrameWorldTransparency.clear();
        return;
    }

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    if ( !wm || !wm->GetMeshVertexBuffer() || !wm->GetMeshIndexBuffer() ) { g_FrameWorldTransparency.clear(); return; }
    D3D12VertexBuffer* vb = D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() );
    D3D12VertexBuffer* ib = D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() );
    if ( !vb->GetResource() || !ib->GetResource() ) { g_FrameWorldTransparency.clear(); return; }

    DX_ZONE( m_CmdList, "DrawWorldTransparencyMeshes" );

    // ViewProj — identical derivation to DrawWorldMesh/DrawWaterSurfaces (world verts are world-space).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetGraphicsRootSignature( m_Pipelines.WorldTransparency.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
    D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    m_CmdList->IASetIndexBuffer( &ibv );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // D3D11's state machine, reproduced exactly: the pass starts from SetDefaultStates() (no blending,
    // depth-write ON) and the FIRST alpha-func change both selects a blend mode and turns depth-write off
    // for the rest of the list. Materials whose effective alpha func is 0 therefore draw opaque + depth-
    // writing when they come first, which is what D3D11 does — order-dependent, but faithful.
    GothicBlendStateInfo blend;
    blend.SetDefault();
    bool depthWrite = true;
    int lastAlphaFunc = zMAT_ALPHA_FUNC_MAT_DEFAULT;

    ID3D12PipelineState* pso = m_Pipelines.GetOrCreateWorldTransparencyPipeline( blend, depthWrite );
    if ( !pso ) { g_FrameWorldTransparency.clear(); return; }
    m_CmdList->SetPipelineState( pso );

    unsigned int drawnIndices = 0;
    zCMaterial* lastMat = nullptr;
    {
        DX_ZONE( m_CmdList, "World transparency (blended)" );
        for ( const WorldTransparencyMesh& entry : g_FrameWorldTransparency ) {
            zCMaterial* mat = entry.Material;
            if ( !mat || !entry.Mesh || entry.Mesh->Indices.empty() ) continue;

            zCTexture* tex = mat->GetAniTexture();
            if ( !tex || tex->CacheIn( 0.6f ) != zRES_CACHED_IN ) continue;   // "Draw what? black? :)"

            // Bindless diffuse (b6.MatDiffuseIndex). No usable texture -> skip rather than draw a black
            // slab over the scene; the other three material indices are unread by PSTransparent.
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
                BlendStateForAlphaFunc( alphaFunc, blend );   // `default:` keeps the current blend state
                depthWrite = false;
                lastAlphaFunc = alphaFunc;
                ID3D12PipelineState* next = m_Pipelines.GetOrCreateWorldTransparencyPipeline( blend, depthWrite );
                if ( !next ) continue;
                m_CmdList->SetPipelineState( next );
            }

            if ( lastMat != mat ) {
                const float4 factor = ComputeTextureFactor( mat );
                m_CmdList->SetGraphicsRoot32BitConstants( 1, 4, &factor, 0 );   // b5 TextureFactor
                lastMat = mat;
            }
            m_CmdList->SetGraphicsRoot32BitConstants( 2, 4, mat6, 0 );          // b6 MaterialCB

            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( entry.Mesh->Indices.size() ), 1,
                entry.Mesh->BaseIndexLocation, 0, 0 );
            drawnIndices += static_cast<unsigned int>( entry.Mesh->Indices.size() );
        }
    }

    // Second loop: depth only. D3D11 does exactly this ("Draw again, but only to depthbuffer this time to
    // make them work with fogging") — without it the height-fog / god-ray passes reconstruct world
    // positions from whatever lies BEHIND the glass. Same VB/IB and the same VS blob, so the depth written
    // here is bit-identical to what the blended loop tested against.
    if ( m_Pipelines.WorldTransparency.DepthFillPSO ) {
        DX_ZONE( m_CmdList, "World transparency (depth fill)" );
        m_CmdList->SetPipelineState( m_Pipelines.WorldTransparency.DepthFillPSO.Get() );
        // The depth-fill PSO reuses PSTransparent (color writes masked off) rather than a null PS, so the
        // shader still runs its bindless ResourceDescriptorHeap[MatDiffuseIndex] fetch even though the
        // result is discarded. Stamp a known-good slot: the loop below is less strict than the color loop
        // (it accepts materials whose texture failed to cache in), so b6 could otherwise still hold
        // uninitialized root constants if the color loop drew nothing at all — an out-of-range descriptor
        // index, i.e. a GPU fault, not just a wrong pixel.
        const uint32_t safeMat6[4] = { 0xFFFFFFFFu, 0u, m_BlackTexture->GetSrvSlot(), 0u };
        m_CmdList->SetGraphicsRoot32BitConstants( 2, 4, safeMat6, 0 );
        for ( const WorldTransparencyMesh& entry : g_FrameWorldTransparency ) {
            if ( !entry.Material || !entry.Mesh || entry.Mesh->Indices.empty() ) continue;
            if ( !entry.Material->GetAniTexture() ) continue;
            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( entry.Mesh->Indices.size() ), 1,
                entry.Mesh->BaseIndexLocation, 0, 0 );
        }
    }

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnIndices / 3;
    g_FrameWorldTransparency.clear();
}
