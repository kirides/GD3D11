// D3D12GraphicsEngine — Gothic FX geometry: quad marks and poly strips.
//
// Ports three D3D11 passes that had no D3D12 counterpart at all (the base-class DrawPolyStrips is a silent
// `return XR_SUCCESS`, and nothing on D3D12 ever looked at GothicAPI's quad-mark list), so blood splatter,
// spell ground marks, weapon/spell trails and lightning flashes were simply missing from the frame:
//   * D3D11GraphicsEngine::DrawQuadMarks   (:8741) — zCQuadMark geometry, opaque/add/blend modes
//   * D3D11GraphicsEngine::DrawMQuadMarks  (:8817) — the MUL/MUL2 marks the first pass defers
//   * D3D11GraphicsEngine::DrawPolyStrips  (:7912) — zCPolyStrip trails + lightning flashes
//
// All three are CPU-built ExVertexStruct triangle lists drawn unlit with a per-material blend mode, so they
// share one root signature, one shader (Shaders/D3D12/Fx.hlsl) and one blend-keyed PSO cache
// (D3D12PipelineState::CreateFx / GetOrCreateFxPipeline).
//
// Geometry sources differ, and that is the only real difference between the passes:
//   * quad marks own a per-mark static GfxVertexBuffer, filled by WorldConverter::UpdateQuadMarkInfo when
//     Gothic creates the mark (so it is already a D3D12VertexBuffer — bind it directly)
//   * poly-strip meshes are rebuilt on the CPU every frame, so they stream through a per-frame upload ring
//     (m_FxVertexBuffer). D3D11 instead grows one shared TempPolysVertexBuffer on demand.
//
// Deliberate divergence, also documented in Fx.hlsl: D3D11's DrawQuadMarks binds the LIT world pixel shader
// (PS_World) while its MUL/MUL2 marks and all poly strips use the unlit PS_Simple. D3D12 draws every one of
// them unlit — the vertices already carry Gothic's baked static lighting in their vertex color, and plumbing
// the Forward+ tile grid/cascades into a pass that draws a handful of blood splats is not worth it.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12Texture.h"
#include "D3D12VertexBuffer.h"
#include "D3D12PipelineState.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"
#include "../VertexTypes.h"
#include "../zCMaterial.h"
#include "../zCTexture.h"
#include "../zCPolygon.h"
#include "../zCMesh.h"
#include "../zCQuadMark.h"
#include "../zCVob.h"
#include "../D3D7/MyDirectDrawSurface7.h"

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

namespace {
    // Per-frame poly-strip vertex ring. 2 MB / frame-in-flight is ~34k ExVertexStruct verts — well above what
    // Gothic's trails and lightning flashes produce (a few hundred verts per strip), and overflow drops the
    // rest of the frame's strips with a one-shot warning rather than reallocating on the frame path.
    constexpr UINT kFxVertexBufferBytes = 2 * 1024 * 1024;

    // D3D11 spec: DrawQuadMarks defers MUL/MUL2 marks into the MulQuadMarks member and DrawMQuadMarks drains
    // it in the later transparent pass. Same lifetime here: filled by DrawQuadMarks, cleared by DrawMQuadMarks.
    struct MulQuadMark { zCQuadMark* Mark; const QuadMarkInfo* Info; };
    std::vector<MulQuadMark> g_MulQuadMarks;

    // b2 Flags — must match Fx.hlsl's FX_* defines. Each D3D11 pass binds a different pixel shader; these
    // pick the matching behaviour. Never combined: PS_World alpha-tests and ignores the vertex color for RGB,
    // PS_Simple modulates by the vertex color and does not alpha-test.
    constexpr uint32_t kFxAlphaTest = 1;     // PS_World's unconditional DoAlphaTest(color.a)
    constexpr uint32_t kFxVertexColor = 2;   // PS_Simple's `color *= Input.vDiffuse`

    // b2 payload: { diffuse SRV slot, flags, alpha ref, pad }.
    struct FxMaterialConsts { uint32_t DiffuseIndex; uint32_t Flags; float AlphaRef; float Pad; };
    static_assert( sizeof( FxMaterialConsts ) == 4 * sizeof( uint32_t ), "FxMaterialCB is 4 root constants" );

    // The material a quad mark actually draws with: the first polygon's, falling back to the mark's own.
    // Verbatim from both D3D11 passes (and from WorldConverter::UpdateQuadMarkInfo, which builds the verts).
    zCMaterial* QuadMarkMaterial( zCQuadMark* mark ) {
        zCMesh* mesh = mark->GetQuadMesh();
        if ( !mesh ) return mark->GetMaterial();
        const int numPolys = mesh->GetNumPolygons();
        zCPolygon** polys = mesh->GetPolygons();
        return ( numPolys > 0 && polys ) ? polys[0]->GetMaterial() : mark->GetMaterial();
    }

    // Resolve a material's diffuse texture to a bindless SRV heap slot, caching it in on the way (mirrors the
    // CacheIn -> GetSurface -> GetEngineTexture -> D3D12Texture::From chain every other D3D12 pass uses).
    // Returns UINT_MAX when there is nothing to draw with — the caller skips rather than drawing black.
    UINT ResolveDiffuseSlot( zCTexture* tex ) {
        if ( !tex || tex->CacheIn( 0.6f ) != zRES_CACHED_IN ) return UINT_MAX;
        MyDirectDrawSurface7* surface = tex->GetSurface();
        if ( !surface ) return UINT_MAX;
        GfxTexture* gfx = surface->GetEngineTexture();
        if ( !gfx ) return UINT_MAX;
        D3D12Texture* d = D3D12Texture::From( gfx );
        return d->HasSRV() ? d->GetSrvSlot() : UINT_MAX;
    }

    UINT ResolveDiffuseSlot( zCMaterial* mat ) {
        return mat ? ResolveDiffuseSlot( mat->GetAniTexture() ) : UINT_MAX;
    }
}


bool D3D12GraphicsEngine::CreateFxVertexBuffers() {
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = DefaultUploadHeapType;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kFxVertexBufferBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_Allocator->CreateResource( &allocDesc, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_FxVertexBufferAlloc[i].ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_FxVertexBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_FxVertexBuffer[i]->SetName( i == 0 ? L"FxVertexRing0" : L"FxVertexRing1" );
        D3D12_RANGE noRead = { 0, 0 };
        if ( FAILED( m_FxVertexBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_FxVertexBufferPtr[i] ) ) ) )
            return false;
    }
    m_FxVertexBufferCapacity = kFxVertexBufferBytes;
    return true;
}


void D3D12GraphicsEngine::DrawQuadMarks() {
    // Port of D3D11GraphicsEngine::DrawQuadMarks. Runs with the opaque decals (D3D11's "Draw ParticleFX #1"
    // pass calls the two back to back), i.e. on the HDR scene target with the opaque scene already laid down.
    const auto& quadMarks = Engine::GAPI->GetQuadMarks();
    g_MulQuadMarks.clear();
    if ( quadMarks.empty() ) return;
    if ( !m_FrameOpen || !m_Pipelines.World.RootSig || !m_DepthBuffer ) return;

    DX_ZONE( m_CmdList, "Draw quad marks" );

    // LIT pass: World.RootSig + World.hlsl's VSQuadMark/PSMain, i.e. the exact same lighting the world mesh
    // gets — sRGB->linear albedo, DelightDiffuse, CSM shadows, tiled point lights, SSAO, wetness, sky IBL.
    // D3D11 does the same thing structurally (DrawQuadMarks binds PS_World, its lit world shader). Each mark
    // draws straight out of its own static VB with its world matrix in b4; nothing is streamed or re-uploaded.
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    BindWorldFrameRootState( viewProj );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    // D3D11's state machine: SetDefaultStates() (no blending, depth-write ON), then the blend state follows
    // whatever the CURRENT material's alpha func says — but only when it CHANGES, so a run of same-func marks
    // costs one PSO switch. Depth-write stays on for this pass (only DrawMQuadMarks turns it off).
    GothicBlendStateInfo blend;
    blend.SetDefault();
    ID3D12PipelineState* pso = m_Pipelines.GetOrCreateQuadMarkPipeline( blend, true );
    if ( !pso ) return;
    m_CmdList->SetPipelineState( pso );

    int alphaFunc = zMAT_ALPHA_FUNC_NONE;

    const float vfxRadius = Engine::GAPI->GetRendererState().RendererSettings.VisualFXDrawRadius;
    const XMVECTOR vVfxRadiusSq = XMVectorReplicate( vfxRadius * vfxRadius );
    const XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();

    unsigned int drawnVertices = 0;
    for ( auto const& it : quadMarks ) {
        zCQuadMark* mark = it.first;
        const QuadMarkInfo& info = it.second;
        if ( !mark->GetConnectedVob() ) continue;
        if ( !info.Mesh || info.NumVertices <= 0 ) continue;

        if ( XMVector3Greater( XMVector3LengthSq( camPos - XMLoadFloat3( &info.Position ) ), vVfxRadiusSq ) )
            continue;

        zCMaterial* mat = QuadMarkMaterial( mark );
        if ( !mat ) continue;

        if ( alphaFunc != mat->GetAlphaFunc() ) {
            switch ( mat->GetAlphaFunc() ) {
            case zMAT_ALPHA_FUNC_ADD:
                blend.SetAdditiveBlending();
                break;
            case zMAT_ALPHA_FUNC_BLEND:
                blend.SetAlphaBlending();
                break;
            case zMAT_ALPHA_FUNC_NONE:
            case zMAT_ALPHA_FUNC_TEST:
                blend.SetDefault();
                break;
            case zMAT_ALPHA_FUNC_MUL:
            case zMAT_ALPHA_FUNC_MUL2:
                // Deferred to DrawMQuadMarks — modulate blending has to happen over the finished scene, and
                // those stay UNLIT (D3D11's DrawMQuadMarks binds PS_Simple, not PS_World).
                g_MulQuadMarks.push_back( { mark, &info } );
                continue;
            default:
                continue;
            }
            alphaFunc = mat->GetAlphaFunc();
            ID3D12PipelineState* next = m_Pipelines.GetOrCreateQuadMarkPipeline( blend, true );
            if ( !next ) continue;
            m_CmdList->SetPipelineState( next );
        }

        const UINT diffuseSlot = ResolveDiffuseSlot( mat );
        if ( diffuseSlot == UINT_MAX ) continue;

        D3D12VertexBuffer* vb = D3D12VertexBuffer::From( info.Mesh.get() );
        if ( !vb->GetResource() ) continue;

        // b4: rows 0-2 of the mark's world matrix (see World.hlsl's QuadMarkCB for why only three, and why
        // this borrows the wind slot). Uploaded verbatim like every other matrix here — the vob matrix is
        // column-vector form, translation at _14/_24/_34.
        const XMFLOAT4X4 world = *mark->GetConnectedVob()->GetWorldMatrixPtr();
        m_CmdList->SetGraphicsRoot32BitConstants( 11, 12, &world, 0 );

        // b6 MaterialCB, the same four root constants the world ExecuteIndirect commands push per draw:
        // no normal map (0xFFFFFFFF -> PSMain skips the perturb), the 1x1 default ORM (AO 1 / rough .5 /
        // metal 0, channel layout 0 = full RGB ORM), the mark's diffuse, and a zero normal strength.
        const uint32_t matCb[4] = { 0xFFFFFFFFu, m_DefaultOrmTexture->GetSrvSlot(), diffuseSlot, 0u };
        m_CmdList->SetGraphicsRoot32BitConstants( 10, 4, matCb, 0 );

        const UINT numVerts = static_cast<UINT>( info.NumVertices );
        const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
        m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
        m_CmdList->DrawInstanced( numVerts, 1, 0, 0 );
        drawnVertices += numVerts;
    }

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnVertices / 3;
}


void D3D12GraphicsEngine::DrawMQuadMarks() {
    // Port of D3D11GraphicsEngine::DrawMQuadMarks — the MUL/MUL2 marks DrawQuadMarks deferred, drawn with the
    // transparent decals over the finished scene. Depth-write OFF here (D3D11 sets it explicitly).
    if ( g_MulQuadMarks.empty() ) return;
    if ( !m_FrameOpen || !m_Pipelines.Fx.RootSig ) { g_MulQuadMarks.clear(); return; }

    DX_ZONE( m_CmdList, "Draw quad marks (modulate)" );

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetGraphicsRootSignature( m_Pipelines.Fx.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    GothicBlendStateInfo blend;
    blend.SetDefault();
    int alphaFunc = 0;   // D3D11 starts at 0 here, so the first mark always switches (0 is never MUL/MUL2)
    ID3D12PipelineState* pso = m_Pipelines.GetOrCreateFxPipeline( blend, false );
    if ( !pso ) { g_MulQuadMarks.clear(); return; }
    m_CmdList->SetPipelineState( pso );

    unsigned int drawnVertices = 0;
    for ( auto const& entry : g_MulQuadMarks ) {
        zCMaterial* mat = QuadMarkMaterial( entry.Mark );
        if ( !mat || !entry.Mark->GetConnectedVob() ) continue;
        if ( !entry.Info->Mesh || entry.Info->NumVertices <= 0 ) continue;

        if ( alphaFunc != mat->GetAlphaFunc() ) {
            switch ( mat->GetAlphaFunc() ) {
            case zMAT_ALPHA_FUNC_MUL:
                blend.SetModulateBlending();
                break;
            case zMAT_ALPHA_FUNC_MUL2:
                blend.SetModulate2Blending();
                break;
            default:
                continue;
            }
            alphaFunc = mat->GetAlphaFunc();
            ID3D12PipelineState* next = m_Pipelines.GetOrCreateFxPipeline( blend, false );
            if ( !next ) continue;
            m_CmdList->SetPipelineState( next );
        }

        const UINT diffuseSlot = ResolveDiffuseSlot( mat );
        if ( diffuseSlot == UINT_MAX ) continue;

        D3D12VertexBuffer* vb = D3D12VertexBuffer::From( entry.Info->Mesh.get() );
        if ( !vb->GetResource() ) continue;

        XMFLOAT4X4 world;
        XMStoreFloat4x4( &world, entry.Mark->GetConnectedVob()->GetWorldMatrixXM() );
        m_CmdList->SetGraphicsRoot32BitConstants( 1, 16, &world, 0 );

        // PS_Simple semantics (what D3D11's DrawMQuadMarks binds): modulate by the vertex color, no alpha
        // test. UpdateQuadMarkInfo forces the vertex color to white for MUL/MUL2 materials anyway, so this
        // is a no-op multiply in practice — kept because it is what the D3D11 shader does.
        const FxMaterialConsts matCb = { diffuseSlot, kFxVertexColor, 0.0f, 0.0f };
        m_CmdList->SetGraphicsRoot32BitConstants( 2, 4, &matCb, 0 );

        const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
        m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
        m_CmdList->DrawInstanced( static_cast<UINT>( entry.Info->NumVertices ), 1, 0, 0 );
        drawnVertices += static_cast<unsigned int>( entry.Info->NumVertices );
    }

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnVertices / 3;
    g_MulQuadMarks.clear();
}


XRESULT D3D12GraphicsEngine::DrawPolyStrips( bool noTextures ) {
    // Port of D3D11GraphicsEngine::DrawPolyStrips — weapon/spell trails and lightning flashes. D3D11 rebuilds
    // both mesh sets at the top of its own "Draw PolyStrips" render-graph pass; that CPU work is backend-
    // neutral GothicAPI code, so it happens here for the same reason (nothing else calls it).
#if (defined BUILD_GOTHIC_2_6_fix || defined BUILD_GOTHIC_1_08k)
    Engine::GAPI->CalcPolyStripMeshes();   // weapon/effect trails
    Engine::GAPI->CalcFlashMeshes();       // lightning flashes
#endif

    const std::map<zCTexture*, PolyStripInfo>& polyStripInfos = Engine::GAPI->GetPolyStripInfos();
    if ( polyStripInfos.empty() ) return XR_SUCCESS;
    if ( !m_FrameOpen || !m_Pipelines.Fx.RootSig || !m_FxVertexBuffer[m_FrameIndex] ) return XR_SUCCESS;

    DX_ZONE( m_CmdList, "Draw poly strips" );

    // Strip vertices are already in world space (GothicAPI builds them that way), so the world matrix is
    // identity — D3D11 uploads an identity Matrices_PerInstances for exactly this reason.
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj, identity;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );
    XMStoreFloat4x4( &identity, XMMatrixIdentity() );

    m_CmdList->SetGraphicsRootSignature( m_Pipelines.Fx.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 1, 16, &identity, 0 );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    // D3D11's state machine: the FIRST blended material turns blending on and depth-write off, and every
    // later material keeps that state (the test is `(blendAdd||blendBlend) && !BlendState.BlendEnabled`) —
    // so a pass that starts with an ADD strip stays additive even across a later BLEND strip. Faithful,
    // order-dependent, and cheap: at most one PSO switch for the whole pass.
    GothicBlendStateInfo blend;
    blend.SetDefault();
    bool blendEnabled = false;
    bool depthWrite = true;
    ID3D12PipelineState* pso = m_Pipelines.GetOrCreateFxPipeline( blend, depthWrite );
    if ( !pso ) return XR_SUCCESS;
    m_CmdList->SetPipelineState( pso );

    const UINT frame = m_FrameIndex;
    unsigned int drawnVertices = 0;
    for ( auto const& it : polyStripInfos ) {
        zCMaterial* mat = it.second.material;
        const std::vector<ExVertexStruct>& vertices = it.second.vertices;
        if ( !mat || vertices.empty() ) continue;

        // The MAP KEY is the texture to draw with, not mat->GetAniTexture() — CalcPolyStripMeshes falls back
        // to GetTextureSingle() when a strip material has no animated texture, and keys the map on the result.
        // D3D11 does the same (`zCTexture* tx = it->first`). It also skips strips whose texture is not cached
        // in yet ("Don't draw if texture is not yet cached").
        const UINT diffuseSlot = noTextures ? m_WhiteTexture->GetSrvSlot() : ResolveDiffuseSlot( it.first );
        if ( diffuseSlot == UINT_MAX ) continue;

        const int matAlphaFunc = mat->GetAlphaFunc();
        const bool blendAdd = matAlphaFunc == zMAT_ALPHA_FUNC_ADD;
        const bool blendBlend = matAlphaFunc == zMAT_ALPHA_FUNC_BLEND;
        if ( ( blendAdd || blendBlend ) && !blendEnabled ) {
            if ( blendAdd ) blend.SetAdditiveBlending();
            else            blend.SetAlphaBlending();
            blendEnabled = true;
            depthWrite = false;
            ID3D12PipelineState* next = m_Pipelines.GetOrCreateFxPipeline( blend, depthWrite );
            if ( !next ) continue;
            m_CmdList->SetPipelineState( next );
        }

        const UINT bytes = static_cast<UINT>( vertices.size() * sizeof( ExVertexStruct ) );
        if ( m_FxVertexBufferOffset + bytes > m_FxVertexBufferCapacity ) {
            if ( !m_FxOverflowLogged ) {
                LogWarn() << "D3D12: poly-strip vertex ring overflow (" << m_FxVertexBufferCapacity
                    << " bytes/frame). Some trails/flashes dropped this frame.";
                m_FxOverflowLogged = true;
            }
            break;
        }
        memcpy( m_FxVertexBufferPtr[frame] + m_FxVertexBufferOffset, vertices.data(), bytes );
        const D3D12_GPU_VIRTUAL_ADDRESS gpuVA = m_FxVertexBuffer[frame]->GetGPUVirtualAddress() + m_FxVertexBufferOffset;
        m_FxVertexBufferOffset += bytes;

        // PS_Simple semantics (BindShaderForTexture resolves to it for every strip): modulate by the vertex
        // color, no alpha test — the trails' translucency comes from their blend mode, not a cutout.
        const FxMaterialConsts matCb = { diffuseSlot, kFxVertexColor, 0.0f, 0.0f };
        m_CmdList->SetGraphicsRoot32BitConstants( 2, 4, &matCb, 0 );

        const D3D12_VERTEX_BUFFER_VIEW vbv = { gpuVA, bytes, sizeof( ExVertexStruct ) };
        m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
        m_CmdList->DrawInstanced( static_cast<UINT>( vertices.size() ), 1, 0, 0 );
        drawnVertices += static_cast<unsigned int>( vertices.size() );
    }

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnVertices / 3;
    return XR_SUCCESS;
}
