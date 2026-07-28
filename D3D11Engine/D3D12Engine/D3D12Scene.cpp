// D3D12GraphicsEngine — scene renderer: world/vob/skeletal/water/particles/decals/ghost/veg/sky/shadows/lightcull.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12LineRenderer.h"
#include "D3D12VertexBuffer.h"
#include "D3D12Texture.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../WorldObjects.h"
#include "../zCMorphMesh.h"
#include "../zCTexture.h"
#include "../D3D7/MyDirectDrawSurface7.h"
#include "../zCView.h"
#include "../zCModel.h"
#include "../zCMaterial.h"
#include "../zCVob.h"
#include "../zCVobLight.h"
#include "../zCDecal.h"
#include "../zCWorld.h"
#include "../WorldConverter.h"
#include "../VertexTypes.h"
#include "../ImGuiShim.h"
#include "../zFont.h"
#include "../zCCamera.h"
#include "../oCGame.h"
#include "../oCVisFX.h"
#include "../DXGIHelpers.h"
#include "../WindAnimation.h"
#include "../GVegetationBox.h"
#include "../GMeshSimple.h"
#include "../Toolbox.h"

#include "D3D12RenderQueue.h"
#include "InstancingUtils.h"
#include "../ThreadPool.h"   // Engine::RenderingThreadPool — MT shadow-cascade cull/record fan-out

#include <array>
#include <future>
#include <algorithm>   // std::ranges::sort — DrawGhostVobs' back-to-front ordering

// imgui_impl_dx12 calls CreateDXGIFactory1 directly (for tearing detection). dxgi.dll is present on
// every Windows 7+ and the D3D11 fallback swapchain already needs it at runtime, so a load-time link
// here is safe — it does NOT reintroduce the D3D12 soft-dependency that lets old systems fall back.
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"


// ---- The two per-frame record sets the shadow modules share with this TU (declared in D3D12EngineCommon.h) ----
// UploadFrameVobInstances memcpys each visible visual's instances into the VOB ring ONCE (before the light cull)
// and records the resulting stream view + count here. The depth prepass (DrawVobDepthPrepass), the color pass
// (DrawVobsInstanced) and the point-shadow static-VOB gather all draw from these — no second upload.
std::vector<FrameVobUpload> g_FrameVobUploads;
// Per-vob snapshot of the diffuse SRV HEAP SLOT for each entry of visual->SkeletalMeshes, in that map's
// (stable, unmutated-within-a-frame) iteration order. Taken by PrepareFrameSkeletals on the main thread
// immediately after that vob's UpdateMeshLibTexAniState(), which is the only moment the shared per-MODEL texture
// slots actually describe this instance. Grown monotonically and reused: only the live prefix
// [0, g_SkelMatSrvCount) is valid each frame and the inner vectors keep their capacity, so this settles into
// zero per-frame allocations. Indexed (never pointed into) by FrameSkelDraw::matSrvIndex so a rehash of
// g_SkelUploadCache or a growth of this vector can't dangle a record.
std::vector<std::vector<UINT>> g_SkelMatSrvs;
size_t g_SkelMatSrvCount = 0;

namespace {
    // Swapchain / final-output format. R10G10B10A2 (10-bit) instead of R8: the tonemapped output has much
    // finer gradients (kills banding in sky/fog/soft shadows) at the same 32bpp. Also the format the 2D UI +
    // ImGui + the tonemap resolve write to. Flip-model swapchains support R10G10B10A2_UNORM natively.
    // HDR scene-color format: the 3D world/VOB/skeletal/water/decal/particle passes accumulate lighting here in
    // linear-ish FLOAT (values may exceed 1.0 — bright sun + additive point lights no longer clip to white), then
    // a fullscreen tonemap resolves it into the swapchain. R16F 4-channel = 64bpp intermediate (recreated on resize).
    constexpr UINT kVobInstanceBufferBytes = 8 * 1024 * 1024; // per-frame VOB instance ring (~58k instances @144B)
    constexpr UINT kParticleInstanceBufferBytes = 8 * 1024 * 1024; // per-frame particle instance ring (~150k @56B)
    constexpr UINT kDecalInstanceBufferBytes = 4 * 1024 * 1024; // per-frame decal instance ring (~52k decals @80B)

    // D3D12SerializeRootSignature is exported from the already-loaded d3d12.dll (we don't link d3d12.lib).
    typedef HRESULT( WINAPI* PFN_SERIALIZE_ROOT_SIG )( const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob** );
    PFN_SERIALIZE_ROOT_SIG LoadSerializeRootSignature() {
        HMODULE d3d12 = LoadLibraryA( "d3d12.dll" );
        if ( !d3d12 ) return nullptr;
        return reinterpret_cast<PFN_SERIALIZE_ROOT_SIG>( GetProcAddress( d3d12, "D3D12SerializeRootSignature" ) );
    }

    // TODO: in the future make this depend on device capabilities.
    constexpr const char* Shadermodel_PS = "ps_6_6";
    constexpr const char* Shadermodel_VS = "vs_6_6";
    constexpr const char* Shadermodel_CS = "cs_6_6";



    // Inline HLSL for the 2D UI path. VS mirrors VS_TransformedEx (screen-space xyzrhw -> clip space,
    // rhw packed in Normal.x). PS emulates the fixed-function texture-stage pipeline (mirrors
    // Shaders/FixedFunctionPipeline.h): FF_Stages[0/1] color ops + args + diffuse-alpha test, driven by
    // the FFPipelineConstantBuffer (b1) which receives Gothic's GraphicsState each draw. Per-draw BLEND
    // modes (opaque/alpha/additive/modulate/...) are handled by selecting a matching PSO, not here.
    // Limitation: only texture0 is bound, so a 2nd stage samples texture0 (menus use 1 stage -> exact;
    // the world's 2-texture lightmap path is a Phase-2 concern).

    // Phase-2 world shader (textured). The wrapped world mesh is the packed 36-byte ExVertexStructGPU;
    // we bind Position (float3 @0), TexCoord0 (float2 @20) and Color (R8G8B8A8 @32), ignoring the packed
    // normal/tangent/uv2 for now. World-mesh verts are already in world space, so the transform is just
    // ViewProj (identity world) — matches D3D11 VS_ExPacked: mul(float4(pos,1), ViewProj), reversed-Z.
    // PS samples the diffuse texture, modulates by the baked vertex color (Gothic packs a DWORD read as
    // R8G8B8A8 -> .bgr recovers RGB), and does a fixed alpha-test cutout (foliage/fences). No G-buffer /
    // no deferred lighting yet: the baked vertex color stands in for lighting.

    // Phase-2 instanced VOB shader. Slot 0 = ExVertexStruct (Position@0, Normal@12, TexCoord0@24);
    // slot 1 = per-instance data (world matrix as 4 rows + instance color) from VobInstanceInfo.

    // Forward+ opaque DEPTH PREPASS shader (P2.9b-1). Lays down the opaque world-mesh depth before the
    // lit color passes so the later tiled light-culling compute (P2.9b-2) has a per-pixel depth to tighten
    // each tile's frustum. Writes DEPTH ONLY — the PSO sets the color write mask to 0, so the float4 the PS
    // returns is discarded (it exists solely to run the alpha-test clip). The clip cutoff matches the opaque
    // world PS's clip( t.a - 0.5 ) exactly, so cutout foliage/fence gaps do NOT write depth (otherwise the
    // main pass would see background occluded through the gaps). Reuses m_Pipelines.World.RootSig: only b0 (ViewProj)
    // and t0/s0 are referenced — fog/light params are NOT bound (no light loop here, so no hang risk).

    // Forward+ tiled light-culling COMPUTE shader (P2.9b-2). One thread group per 16x16 screen tile; each
    // group builds its tile's view-space AABB from the prepass depth and records which point lights touch it
    // into a per-tile slice of RW_LightIndexList (fixed MAX_LIGHTS_PER_TILE stride — no global atomic counter,
    // so no counter buffer/clear), with the per-tile {Offset,Count} landing in RW_LightGrid. This is the D3D11
    // reference Shaders/CS_LightCulling.hlsl (min/max-depth AABB + SphereInsideAABB) with two divergences:
    //   1. Reversed-Z + world-only prepass. The reference's ScreenToView z-inverse is written for finite
    //      standard-Z; we feed it the ACTUAL projection z-row terms (viewZ = Proj._43/(depth-Proj._33)), which
    //      is correct for our reversed-Z infinite-far camera. And because our depth prepass lays down WORLD
    //      MESH ONLY, an "empty" tile (no world depth — sky, or a VOB/NPC in front of nothing) falls back to an
    //      all-encompassing AABB (stay conservatively lit) rather than the reference's collapse-to-near-plane,
    //      which would unlight characters against the sky. The bounded near+far AABB is the whole point: it
    //      pulls in only lights whose sphere actually reaches the tile's geometry, so a wall at 15m no longer
    //      overflows the 32-light cap the way the earlier camera-origin cone (near fixed at 1) did. NOTE prior
    //      retired versions here: an unbounded [1,100000] AABB (far corners blew up off-axis), then side-planes
    //      with a fixed [1,100000] slab and then a far-only slab — all still over-included and overflowed.
    //      Residual: a short-range light on a VOB/NPC that pokes in front of distant world geometry and can't
    //      reach that world can still be culled; completing the prepass (VOB+skeletal depth) removes it.
    //   2. Fixed per-tile index slice instead of a compacted global list, dropping RW_IndexCounter and its
    //      per-frame clear. At 1080p this is ~1 MB (8160 tiles * 32 * 4 B) — negligible, and simpler/safer.
    // PositionView is filled CPU-side in BuildFrameLightBuffer using the same transpose(view) transform the
    // D3D11 CullLights uses, so this shader's view space matches. SM6.6: no ternary (use min()/select()).

    // Water: the surfaces peeled out of the opaque world pass live in g_FrameWaterSurfaces (declared in
    // D3D12EngineCommon.h, defined in D3D12Water.cpp, which owns the whole pass).

    // Per-frame visible-vob/light/mob collection, hoisted out of DrawVobsInstanced so ALL geometry passes
    // (world, VOBs, skeletal) light against the same set. CollectVisibleVobs has side effects (fills each
    // visual's Instances list) and must run EXACTLY ONCE per frame. Single-threaded within OnStartWorldRendering.
    std::vector<VobInfo*>         g_FrameVobs;
    std::vector<VobLightInfo*>    g_FrameLights;
    std::vector<SkeletalVobInfo*> g_FrameMobs;


    // g_FrameVobUploads (the per-frame VOB instance-ring snapshot) sits at file scope above this namespace —
    // the point-shadow static-VOB gather reads it too, so it is declared in D3D12EngineCommon.h.

    // Per-frame skeletal shared-upload records (P2.9b-4b). PrepareFrameSkeletals runs the once-per-frame
    // animation update and uploads each vob's bone/instance CBs (base meshes) and its node attachments'
    // VOB-instance data (into the VOB ring) BEFORE the cull, recording GPU addresses here. Then
    // DrawSkeletalDepthPrepass (pre-cull, depth-only) and DrawSkeletalColor (post-cull, lit) both draw from
    // these — so the animation update is never run twice and nothing is uploaded twice. Rebuilt each frame.
    // FrameSkelDraw / FrameAttachDraw themselves live in D3D12EngineCommon.h — the CSM cascades and the
    // point-light cubes consume the very same records (see D3D12ShadowMap::SkelDraws / AttachDraws and
    // D3D12PointShadows::SkelScratch / AttachScratch, which PrepareFrameSkeletals routes into).
    std::vector<FrameSkelDraw>   g_FrameSkelDraws;
    std::vector<FrameAttachDraw> g_FrameAttachDraws;

    // Per-vob-per-frame upload cache: the pose/instance CB data and node-attachment ring uploads are
    // view-independent (same pose regardless of who's culling), so PrepareFrameSkeletals uploads them ONCE per
    // vob per frame here and every cull pass (main view + each shadow cascade) just appends the cached GPU
    // addresses / attachment records into its own destination list. Cleared each frame alongside g_FrameSkelDraws.
    struct SkelUploadCache {
        D3D12_GPU_VIRTUAL_ADDRESS instCb = 0;
        D3D12_GPU_VIRTUAL_ADDRESS boneCb = 0;
        bool hasBaseMesh = false;
        uint32_t matSrvIndex = 0xFFFFFFFFu;   // -> g_SkelMatSrvs, see FrameSkelDraw::matSrvIndex
        std::vector<FrameAttachDraw> attachments;
    };
    gtl::flat_hash_map<SkeletalVobInfo*, SkelUploadCache> g_SkelUploadCache;

    // Per-vob snapshot of the diffuse descriptor handle for each entry of visual->SkeletalMeshes, in that map's
    // (stable, unmutated-within-a-frame) iteration order. Taken by PrepareFrameSkeletals on the main thread
    // immediately after that vob's UpdateMeshLibTexAniState(), which is the only moment the shared per-MODEL
    // texture slots actually describe this instance. Grown monotonically and reused: only the live prefix
    // [0, g_SkelMatSrvCount) is valid each frame and the inner vectors keep their capacity, so this settles into
    // zero per-frame allocations. Indexed (never pointed into) by FrameSkelDraw so a rehash of g_SkelUploadCache
    // or a growth of this vector can't dangle a record.
    // Definitions sit at file scope above this namespace (the CSM cascade recorder reads them).


    // Forward+ MVP light buffer (P2.9a): the whole visible-light list is rebuilt from offset 0 each frame,
    // so the ring is just kBackBufferCount snapshots (no per-draw offset). Cap matches D3D11 MAX_TILED_LIGHTS.
    constexpr UINT kMaxFrameLights = 400;


    constexpr UINT kSkeletalConstantBufferBytes = 8 * 1024 * 1024; // per-frame skeletal CB ring (instance + bone palettes)
    constexpr UINT kSkeletalMaxBones = 96;                         // NUM_MAX_BONES — matches every skeletal HLSL

    // Per-instance skeletal constant buffer (register b1). A minimal subset of the D3D11
    // VS_ExConstantBuffer_PerInstanceSkeletal — just what the first-light skeletal shader reads
    // (world matrix + model color + fatness). PrevWorld / motion vectors are a later step.
    struct SkeletalInstanceCB {
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT4   ModelColor;
        float               Fatness;
        float               Pad[3];
    };
    static_assert( sizeof( SkeletalInstanceCB ) == 96, "SkeletalInstanceCB must stay 16-byte-aligned" );

    // Phase-2 skeletal (animated) mesh shader — NPCs, monsters, animated MOBs. Matrix-palette skinning:
    // each vertex stores its position baked into up to 4 influencing bones' local spaces (half4 each),
    // plus per-influence bone index + weight, so skinnedPos = sum_i weight_i * mul(pos_i, Bones[idx_i]).
    // Mirrors VS_ExSkeletal.hlsl's ApplySkinningCurrent core (minus motion vectors / view-space normal /
    // prev-frame bones). b0 = ViewProj (root consts), b1 = per-instance (world + color + fatness), b2 =
    // bone-matrix palette (<=96). Default column-major packing: matrices are uploaded as row-major
    // XMFLOAT4X4 and read the same way the D3D11 skeletal VS does, so mul() is byte-for-byte identical.

    // Phase-2 particle (PFX) shader — instanced camera-facing billboards. One instance per live particle
    // (ParticleInstanceInfo, 56B, all PER_INSTANCE); the VS expands a 4-vertex triangle strip from
    // SV_VertexID, doing all billboard orientation itself (mirrors VS_ParticlePoint.hlsl). `type` encodes
    // the alignment: >=10 => quad-poly (half size); the low digit selects camera / y-locked / plane /
    // velocity-aligned. DIFFUSE is a full float4 here (unlike the packed DWORD paths) so no swizzle. b0 =
    // ViewProj (root consts, default column-major packing, same as the world shader); b1 = camera world pos.

    // Per-decal instance data (per-instance vertex stream, slot 1). World = world*offset*scale (view NOT
    // baked in — unlike D3D11, the D3D12 decal VS applies the standard ViewProj, so the CPU only needs the
    // model matrix). Color.a = the material's ghost alpha ((GetColor()>>24)/255); rgb is unused. 80 bytes.
    struct DecalInstanceInfo {
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT4   Color;
    };
    static_assert( sizeof( DecalInstanceInfo ) == 80, "DecalInstanceInfo layout must match the decal input layout" );

    // Shared unit-quad vertex (per-vertex stream, slot 0). Matches D3D11's decal QuadVertexBuffer verts.
    struct DecalQuadVertex { float px, py, pz; float u, v; };
    static_assert( sizeof( DecalQuadVertex ) == 20, "DecalQuadVertex must be tightly packed (stride 20)" );

    // Decal shader. The quad is expanded by the per-instance world matrix (built on the CPU from the vob's
    // world matrix + DecalOffset/DecalSize + camera-alignment, exactly like D3D11's DrawDecalList), then
    // transformed by the standard ViewProj. Two pixel shaders: PSMainLit (opaque/alpha-test cutout — blood,
    // arrows) and PSMainBlend (texture * material alpha; the PSO blend state does add/alpha/modulate).

    // Round a ring offset up so the next allocation starts on a 256-byte boundary (D3D12 requires root
    // CBV addresses to be 256-byte aligned).
    UINT AlignCB( UINT offset ) { return ( offset + 255u ) & ~255u; }

    // Per-frame linear-fog parameters, bound to the 3D shaders as 8 root 32-bit constants. Field order
    // MUST match the HLSL `cbuffer FogCB { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; }`
    // (root constants map by DWORD offset). The VS computes distance(worldPos, CamPosWS) (== view-space
    // distance for a rigid view transform), the PS lerps toward FogColor over [FogNear, FogFar].
    struct FogConstants {
        float FogColor[3];
        float FogNear;
        float CamPos[3];
        float FogFar;
    };
    static_assert( sizeof( FogConstants ) == 32, "FogConstants must be 8 DWORDs to match the fog root constants" );

    // The color to clear/fill the sky and per-pixel distance-fog with, mirroring D3D11's background-clear
    // formula (D3D11GraphicsEngine::OnStartWorldRendering, ~line 4180): GetFogColor() (== FogColorMod, a
    // fixed user-configurable tint, weather-override aware) is only correct while AtmosphericScattering is
    // on — that's the color the real scattering shader takes as its base input. With scattering OFF (the FF
    // sky path), D3D11 instead uses GraphicsState.FF_FogColor, which GSky.cpp refreshes every frame from
    // Gothic's own zCSkyController_Outdoor::GetMasterState()->FogColor — i.e. it actually tracks time of day/
    // weather. D3D12 previously called GetFogColor() unconditionally here, so with scattering disabled the
    // sky/fog stayed pinned to the static FogColorMod tint (default a light lavender-blue) regardless of time
    // of day — the sky never darkened at night, matching the reported "always looks like fog color" bug.
    DirectX::XMVECTOR GetSceneFogColorXM() {
        const auto& rs = Engine::GAPI->GetRendererState();
        XMFLOAT3 fogColorBase = rs.RendererSettings.AtmosphericScattering
            ? rs.RendererSettings.FogColorMod
            : rs.GraphicsState.FF_FogColor;

        if ( !rs.RendererSettings.DrawFog ) {
            return XMLoadFloat3( &rs.GraphicsState.FF_FogColor );
        }

        zCSkyController_Outdoor* sc = oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor();

        XMVECTOR FogColorMod = XMLoadFloat3( &fogColorBase );

        // Only give the overridden color out if the flag is set
        if ( !sc || !sc->GetOverrideFlag() )

            return FogColorMod;

        const XMFLOAT3 overrideColor = sc->GetOverrideColor();
        XMVECTOR color = XMLoadFloat3( &overrideColor );

        // Clamp to length of 0.5f. Gothic does it at an intensity of 120 / 255.
        float len;
        XMStoreFloat( &len, XMVector3Length( color ) );
        if ( len > 0.5f ) {
            color = XMVector3Normalize( color ) * 0.5f;
            len = 0.5f;
        }

        // Mix these, so the fog won't get black at transitions
        color = XMVectorLerpV( FogColorMod, color, XMVectorSet( len * 2.0f, len * 2.0f, len * 2.0f, 0 ) );

        return color;
    }

    // Builds this frame's fog constants from Gothic's sky state. FogColor = GetSceneFogColorXM() (0..1,
    // weather / sky-override / AtmosphericScattering-mode correct — the same color used to clear the sky);
    // FogNear/FogFar = GraphicsState.FF_FogNear/FF_FogFar, the same values D3D11's ComputeFog() uses (set
    // once per frame in GSky.cpp from sky->GetMasterState()->FogDist, with Gothic's hardcoded 0.3 near
    // factor) — NOT GetFarZ(), which is an unrelated atmospheric-perspective far plane the height-fog PFX
    // uses for its density falloff and is typically much smaller than FogDist, which was making the fog
    // ramp in far too aggressively.
    // Mirror of D3D12GraphicsEngine::m_HeightFogActive, refreshed from it once per frame at the top of
    // OnStartWorldRendering — MakeFogConstants is a free function and can't reach the member. When the
    // post-pass height fog runs (RenderFogAndGodRays), this cheap linear fog must NOT also be applied or the
    // scene ends up fogged twice; D3D11's lit shaders never apply distance fog, the composition pass is the
    // only fog there is.
    bool g_HeightFogActive = false;

    FogConstants MakeFogConstants() {
        FogConstants fog = {};
        DirectX::XMFLOAT3 fc;
        DirectX::XMStoreFloat3( &fc, GetSceneFogColorXM() );
        fog.FogColor[0] = fc.x; fog.FogColor[1] = fc.y; fog.FogColor[2] = fc.z;

        if ( g_HeightFogActive ) {
            // Push the ramp past any reachable view distance instead of adding a shader permutation: the PS
            // lerp weight saturate((d - near)/(far - near)) is then 0 everywhere, i.e. no fog, and the height
            // fog composition owns the look. (Never near == far — that would divide by zero.)
            fog.FogNear = 1.0e9f;
            fog.FogFar = 2.0e9f;
            DirectX::XMFLOAT3 camPos;
            DirectX::XMStoreFloat3( &camPos, Engine::GAPI->GetCameraPositionXM() );
            fog.CamPos[0] = camPos.x; fog.CamPos[1] = camPos.y; fog.CamPos[2] = camPos.z;
            return fog;
        }

        const auto& gs = Engine::GAPI->GetRendererState().GraphicsState;
        float fogFar = gs.FF_FogFar;
        if ( !( fogFar > 1.0f ) ) fogFar = 40000.0f;   // fallback if the controller reports 0 / invalid
        fogFar *= Engine::GAPI->GetRendererState().RendererSettings.FogRange;
        fog.FogFar = fogFar;
        fog.FogNear = gs.FF_FogNear > 0.0f ? gs.FF_FogNear : 0.3f * fogFar;

        DirectX::XMFLOAT3 cp;
        DirectX::XMStoreFloat3( &cp, Engine::GAPI->GetCameraPositionXM() );
        fog.CamPos[0] = cp.x; fog.CamPos[1] = cp.y; fog.CamPos[2] = cp.z;
        return fog;
    }


    gtl::flat_hash_map<BaseVisualInfo*, int16_t> g_vobInfoVisualToBucket;
    std::vector<BaseVisualInfo*> g_vobInfoVisualIndexToVisualInfo;
    RenderView g_GeometryPassVobs;
}


void D3D12GraphicsEngine::OnAddVob(VobInfo* vi) {
    auto [it, inserted] = g_vobInfoVisualToBucket.try_emplace(vi->VisualInfo);
    if (inserted) {
        // newly seen visual, add it to our vob instancing helpers
        it->second = static_cast<int16_t>(g_vobInfoVisualIndexToVisualInfo.size());
        g_vobInfoVisualIndexToVisualInfo.push_back(it->first);
        g_GeometryPassVobs.buckets.push_back({});

        for (auto& v : m_ShadowMap.PassVobs) {
            v.buckets.push_back({});
        }
        m_RainShadowVobs.buckets.push_back({});
    }
    vi->VisualIndex = it->second;

    // A VOB added after a nearby point light already cached its static-aside shadow cube would otherwise cast no
    // point-light shadow: the static cube is only re-rendered when the light is fresh / moved / resized (the
    // renderStatic gate in BuildFramePointShadows), not when world geometry around it changes. Walk the active
    // shadow slots and invalidate any whose light range the new VOB reaches, forcing a one-time static re-render
    // next frame (staticValid=false → renderStatic). Slots are empty during world load (owner==nullptr) so this is
    // a no-op then; the margin mirrors the static-VOB gather's cull (ps.range + visual->MeshSize * 0.5f).
    if ( vi->Vob && vi->VisualInfo )
        m_PointShadows.InvalidateStaticForVobAdded( vi->Vob->GetPositionWorld(), vi->VisualInfo->MeshSize * 0.5f );
}


XRESULT D3D12GraphicsEngine::OnVobRemovedFromWorld( zCVob* vob ) {
    // Symmetric to OnAddVob's static-cube invalidation: a VOB removed from the world must stop casting into any
    // point light's cached static-aside shadow cube. D3D11 keys this off each light's per-vob VobCache
    // (D3D11PointLight::OnVobRemovedFromWorld); D3D12 keeps no per-slot vob list, so test the removed vob's world
    // AABB against each active slot's light sphere (the same closest-point AABB test the static world-section cull
    // uses) and force a one-time static re-render (staticValid=false) for any slot it intersects. Over-invalidation
    // is harmless (one extra static pass); under-invalidation would leave the removed vob's shadow frozen in the
    // cache. Slots are empty during world load (owner==nullptr) so this is a no-op then.
    if ( vob ) m_PointShadows.InvalidateStaticForVobRemoved( vob->GetBBox() );
    return XR_SUCCESS;
}


void D3D12GraphicsEngine::OnLoadWorld()
{
    g_vobInfoVisualToBucket.clear();
    g_vobInfoVisualIndexToVisualInfo.clear();
    g_GeometryPassVobs.Reset();
    for (auto& v : m_ShadowMap.PassVobs) {
        v.Reset();
    }
    m_RainShadowVobs.Reset();
    // The AO depth snapshot still holds the OLD world seen through the old camera — reprojecting the new
    // world's geometry into it would produce garbage occlusion for one frame. RenderSSAO falls back to the
    // white "no occlusion" mask until CopyDepthForAO refills it at the end of the first frame here.
    m_PrevDepthValid = false;
}


// Inventory item preview (GInventory::DrawVobSingle): called directly from Gothic's own zCWorld::Render
// hook during the UI phase (thisptr != the main world — an inventory pseudo-world), so it draws straight
// onto the swapchain backbuffer + its depth buffer instead of going through the Forward+ scene passes.
// Mirrors D3D11GraphicsEngine::DrawVobSingle: back-face-culled, alpha-clipped, unlit/unfogged, via the
// dedicated Preview pipeline (own minimal root sig — no Forward+ light/shadow bindings needed).
void D3D12GraphicsEngine::DrawVobSingle( VobInfo* vob, zCCamera& camera ) {
    if ( !vob || !vob->VisualInfo || !vob->Vob || !m_FrameOpen
        || !m_Pipelines.Preview.PSO || !m_Pipelines.Preview.RootSig || !m_DepthBuffer || !m_DsvHeap )
        return;

    Engine::GAPI->SetViewTransformXM( XMLoadFloat4x4( &camera.GetTransformDX( zCCamera::ETransformType::TT_VIEW ) ) );

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    const XMFLOAT4X4& viewM = rs.TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    // Important: needs the swapchain-sized depth buffer bound, otherwise the inventory VOB renders without
    // depth testing and looks very bad (mirrors the D3D11 comment on why it rebinds the DSV for this draw).
    // Unlike D3D11 (which has a dedicated, always-cleared m_SwapchainDepthStencilBuffer for this), D3D12 reuses
    // the main scene depth buffer, which by now still holds THIS frame's 3D-world depth values at whatever
    // screen pixels this inventory slot's viewport happens to cover — comparing against those stale values
    // would randomly reject preview pixels and let the resolved 3D scene bleed through. Clear the depth back
    // to reversed-Z far (0.0), scoped to just this viewport's rect, before drawing.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>( m_FrameIndex ) * m_RtvDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_VIEWPORT vp = m_CurrentViewport;
    D3D12_RECT     sc = m_CurrentScissor;
    const D3D12_RECT clearRect = {
        static_cast<LONG>( vp.TopLeftX ), static_cast<LONG>( vp.TopLeftY ),
        static_cast<LONG>( vp.TopLeftX + vp.Width ), static_cast<LONG>( vp.TopLeftY + vp.Height ) };
    m_CmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 1, &clearRect );

    m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, &dsv );

    m_CmdList->SetPipelineState( m_Pipelines.Preview.PSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.Preview.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );   // b0 ViewProj

    XMFLOAT4X4 world = *vob->Vob->GetWorldMatrixPtr();
    m_CmdList->SetGraphicsRoot32BitConstants( 1, 16, &world, 0 );      // b1 World

    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );

    for ( auto const& [material, meshes] : vob->VisualInfo->Meshes ) {
        zCTexture* texture = material ? material->GetAniTexture() : nullptr;
        if ( !texture || texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) continue;

        D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
        if ( MyDirectDrawSurface7* surface = texture->GetSurface() ) {
            if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                D3D12Texture* d12 = D3D12Texture::From( gfx );
                if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
            }
        }
        m_CmdList->SetGraphicsRootDescriptorTable( 2, srv );   // t0 diffuse

        for ( auto const& mesh : meshes ) {
            if ( !mesh || mesh->Indices.empty() || !mesh->GetMeshVertexBuffer() || !mesh->GetMeshIndexBuffer() ) continue;
            D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->GetMeshVertexBuffer() );
            D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->GetMeshIndexBuffer() );
            if ( !mvb->GetResource() || !mib->GetResource() ) continue;

            const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
            m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
            // VOB sub-mesh index buffers are 16-bit (VERTEX_INDEX), same as DrawVobsInstanced.
            const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
            m_CmdList->IASetIndexBuffer( &ibv );

            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 1, 0, 0, 0 );
            rs.RendererInfo.FrameDrawnTriangles += static_cast<unsigned int>( mesh->Indices.size() ) / 3;
        }
    }

    // Rebind the backbuffer alone (no depth) — matches D3D11 disabling depth again after this draw so the
    // following 2D UI draws aren't depth-tested.
    m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, nullptr );
}




namespace {
	// Futures for the in-flight point-cube / rain-map recorders, live only between BeginShadowRecording and FinishShadowPasses.
	// Kept file-static (not a member) so D3D12GraphicsEngine.h doesn't have to pull in <future>; cleared rather
	// than reconstructed each frame so the vector keeps its capacity.
	std::vector<std::future<void>> g_ShadowRecordJobs;
}


ID3D12GraphicsCommandList* D3D12GraphicsEngine::BeginShadowList( UINT slot ) {
	// Resets one deferred-shadow (slot x frame-in-flight) allocator/list pair for recording, and returns the
	// open list (nullptr on failure — the caller then leaves that slot unrecorded and FinishShadowPasses
	// re-issues the pass inline). Safe without a GPU wait: this pair was last used kBackBufferCount frames ago
	// and Present() already fenced on that frame.
	ID3D12CommandAllocator*    alloc = m_ShadowCmdAllocators[slot][m_FrameIndex].Get();
	ID3D12GraphicsCommandList* cl    = m_ShadowCmdLists[slot][m_FrameIndex].Get();
	if ( !alloc || !cl ) return nullptr;
	if ( FAILED( alloc->Reset() ) ) return nullptr;
	if ( FAILED( cl->Reset( alloc, nullptr ) ) ) return nullptr;
	ResetCpuContextTracker();   // per-thread breadcrumb ring — see D3D12EngineCommon.h
	return cl;
}


void D3D12GraphicsEngine::PrepareShadowPasses() {
	// MAIN THREAD ONLY: every Gothic read/mutation the point-cube and rain-shadow passes need — the range/sphere
	// culls, zCTexture::CacheIn, zCModel animation + texani state, the shared upload rings — resolved into
	// records that reference nothing but D3D12 handles, so D3D12PointShadows::Record and RecordRainShadowmap can
	// run on the pool. Runs AFTER D3D12ShadowMap::Prepare launched the cascade culls, so all of this overlaps them.
	//
	// The CSM cascades are deliberately not here: their prepare is split around the concurrent cull
	// (D3D12ShadowMap::Prepare launches it, FinishPrepare joins it and does the dependent build).
	if ( !m_FrameOpen ) return;
	ZoneScopedN( "Prepare shadow passes" );

	m_PointShadows.Prepare();
	PrepareRainShadowmap();
}


void D3D12GraphicsEngine::BeginShadowRecording() {
	// Step 2. The three shadow passes write resources that NOTHING between here and the lit geometry passes
	// reads, so their command recording has no business sitting on the main thread's critical path: fan it out
	// and return immediately. The caller then records the depth prepass, the GPU VOB cull, the tiled light cull
	// and SSAO into m_CmdList while the pool records shadows into its own lists.
	//
	// Queue ordering: the finished lists must land BETWEEN what is already recorded this frame (OnBeginFrame's
	// clears, DrawSky, AdvanceRain, the indirect-arg builds) and everything recorded after this point. So
	// close+submit m_CmdList here and reopen it on the SAME frame allocator; FinishShadowPasses then executes
	// the shadow lists while the reopened list is still open and unsubmitted, which gives the GPU
	// [part A][cascades][point cubes][rain map][part B] even though the CPU recorded part B first.
	m_ShadowRecordingPending = false;
	m_ShadowThreadedRecord = false;
	for ( bool& r : m_ShadowListRecorded ) r = false;
	if ( !m_FrameOpen ) return;
	if ( !m_ShadowMap.IsPassReady() && !m_PointShadows.IsPassReady() && !m_RainShadowPassReady ) return;

	// Gated on the same ThreadedShadowCulling toggle the cascade cull uses (one ImGui checkbox / ini key for
	// both backends) plus the per-slot command lists actually existing.
	const bool threadedRecord = m_ShadowCmdListsReady
		&& Engine::GAPI->GetRendererState().RendererSettings.ThreadedShadowCulling
		&& Engine::RenderingThreadPool != nullptr;

	m_ShadowThreadedRecord = threadedRecord;
	if ( !threadedRecord ) {
		// Degrade to the original single-threaded driver: record inline, right here. Same output, same queue
		// order — just no overlap with the prepass. (The cascades still record in FinishShadowPasses, since
		// their caster data does not exist until the concurrent cull is joined there.)
		m_PointShadows.Record( m_CmdList.Get() );
		RecordRainShadowmap( m_CmdList.Get() );
		// Those passes leave no render target bound (their DSVs have just left DEPTH_WRITE) and the depth
		// prepass the caller records next does not bind its own — re-establish the scene-color RT + depth.
		BindSceneColorTarget();
		return;
	}

	SubmitRecordedCommandsAndReopen();
	// The reopen Resets m_CmdList, which drops its render targets/viewport along with everything else.
	BindSceneColorTarget();

	// NOTE: only the point-cube and rain passes are fanned out here. The CSM cascades cannot be recorded yet —
	// their per-cascade caster sets are still being culled on the worker pool, and the Phase-C build that turns
	// those cull results into instance uploads + indirect args has to run on the main thread first. They are
	// recorded in FinishShadowPasses, right after that join.
	g_ShadowRecordJobs.clear();
	if ( m_PointShadows.IsPassReady() ) {
		g_ShadowRecordJobs.push_back( Engine::RenderingThreadPool->enqueue(
			[]( const std::stop_token& token, D3D12GraphicsEngine* self ) {
				if ( token.stop_requested() ) return;
				ZoneScopedN( "Record point shadows" );
				ID3D12GraphicsCommandList* cl = self->BeginShadowList( kPointShadowListIndex );
				if ( !cl ) return;
				self->m_PointShadows.Record( cl );
				self->m_ShadowListRecorded[kPointShadowListIndex] = SUCCEEDED( cl->Close() );
			}, this ).future );
	}
	if ( m_RainShadowPassReady ) {
		g_ShadowRecordJobs.push_back( Engine::RenderingThreadPool->enqueue(
			[]( const std::stop_token& token, D3D12GraphicsEngine* self ) {
				if ( token.stop_requested() ) return;
				ZoneScopedN( "Record rain shadowmap" );
				ID3D12GraphicsCommandList* cl = self->BeginShadowList( kRainShadowListIndex );
				if ( !cl ) return;
				self->RecordRainShadowmap( cl );
				self->m_ShadowListRecorded[kRainShadowListIndex] = SUCCEEDED( cl->Close() );
			}, this ).future );
	}
	m_ShadowRecordingPending = !g_ShadowRecordJobs.empty();
}


void D3D12GraphicsEngine::FinishShadowPasses() {
	// Step 3, run immediately before the lit geometry pass. Three things happen here, in order:
	//   1. Join the concurrent cascade culls and do the serial Phase-C build that depends on them
	//      (D3D12ShadowMap::FinishPrepare). This is the "wait as late as possible" point — the culls have had
	//      DrawSky,
	//      the point/rain prepares, the indirect-arg builds and the whole prepass/cull/SSAO recording block to
	//      run in.
	//   2. Record the cascades (now that their data exists) and join the point/rain recorders launched back in
	//      BeginShadowRecording.
	//   3. Execute every finished list. m_CmdList part B (prepass, culls, SSAO) is still OPEN and unsubmitted,
	//      so the GPU order stays [part A][shadows][part B] even though the CPU recorded part B first.
	// Anything that failed to record is re-issued inline rather than dropped: skipping a pass would desync its
	// cross-frame resource-state tracking (D3D12PointShadows' per-slot cube states, m_RainShadowInReadState).
	if ( !m_FrameOpen ) return;

	// --- 1. the late join + the build that depends on it ---
	m_ShadowMap.FinishPrepare();

	// --- 2a. cascade recording. Unlike the point/rain passes this cannot overlap the prepass (the data did not
	// exist until a moment ago), but it is pure draw emission from pre-resolved records — a fraction of the cull
	// it displaced off the critical path. Fanned out across the cascades when threading is on.
	if ( m_ShadowMap.IsPassReady() ) {
		if ( m_ShadowThreadedRecord ) {
			std::array<std::future<void>, kShadowCascades> jobs;
			for ( UINT c = 0; c < kShadowCascades; ++c ) {
				jobs[c] = Engine::RenderingThreadPool->enqueue(
					[]( const std::stop_token& token, D3D12GraphicsEngine* self, UINT cascade, bool sunUp ) {
						if ( token.stop_requested() ) return;
						ZoneScopedN( "Record shadow cascade" );
						ID3D12GraphicsCommandList* cl = self->BeginShadowList( cascade );
						if ( !cl ) return;
						self->m_ShadowMap.RecordCascade( cascade, cl, sunUp );
						// Only a successfully closed list may be executed; a failed Close leaves it unusable.
						self->m_ShadowListRecorded[cascade] = SUCCEEDED( cl->Close() );
					}, this, c, m_ShadowMap.IsSunUp() ).future;
			}
			{
				ZoneScopedN( "Join shadow cascade recording" );
				for ( auto& j : jobs ) if ( j.valid() ) j.get();
			}
			m_ShadowRecordingPending = true;   // there is now at least one own-list batch to execute below
		} else {
			for ( UINT c = 0; c < kShadowCascades; ++c )
				m_ShadowMap.RecordCascade( c, m_CmdList.Get(), m_ShadowMap.IsSunUp() );
		}
	}

	// --- 2b/3. join the point/rain recorders and execute everything that landed in its own list ---
	if ( m_ShadowRecordingPending ) {
		{
			ZoneScopedN( "Join shadow recording" );
			for ( auto& j : g_ShadowRecordJobs ) if ( j.valid() ) j.get();
		}
		g_ShadowRecordJobs.clear();
		m_ShadowRecordingPending = false;

		ID3D12CommandList* lists[kShadowRecordSlots] = {};
		UINT numLists = 0;
		for ( UINT s = 0; s < kShadowRecordSlots; ++s )
			if ( m_ShadowListRecorded[s] ) lists[numLists++] = m_ShadowCmdLists[s][m_FrameIndex].Get();
		if ( numLists > 0 )
			m_Device.GetDirectQueue()->ExecuteCommandLists( numLists, lists );

		bool anyFailed = false;
		if ( m_ShadowMap.IsPassReady() ) {
			for ( UINT c = 0; c < kShadowCascades; ++c )
				if ( !m_ShadowListRecorded[c] ) { m_ShadowMap.RecordCascade( c, m_CmdList.Get(), m_ShadowMap.IsSunUp() ); anyFailed = true; }
		}
		if ( m_PointShadows.IsPassReady() && !m_ShadowListRecorded[kPointShadowListIndex] ) {
			m_PointShadows.Record( m_CmdList.Get() );
			anyFailed = true;
		}
		if ( m_RainShadowPassReady && !m_ShadowListRecorded[kRainShadowListIndex] ) {
			RecordRainShadowmap( m_CmdList.Get() );
			anyFailed = true;
		}
		if ( anyFailed && !m_ShadowRecordFailureLogged ) {
			m_ShadowRecordFailureLogged = true;   // log once, not once per frame
			LogWarn() << "D3D12: a shadow pass failed to record into its own command list — re-issued inline "
				<< "on the main command list (slower, but correct).";
		}
	}

	// Hand the cascade array to PIXEL_SHADER_RESOURCE for the lit-pass PCF sampling; reverted at the top of next
	// frame's D3D12ShadowMap::Prepare. (The point-shadow cube and the rain map do their own transition inside
	// their own pass, which is self-contained in one list.)
	m_ShadowMap.TransitionToReadState( m_CmdList.Get() );

	// Every shadow list for this frame is now recorded and submitted (or was re-issued inline above), so the
	// point-shadow static cubes this frame (re)rendered have actually reached the GPU — only now may their slots
	// be marked cached. Deliberately after the early-out above: a frame that never got here must NOT validate a
	// static render it never issued, or that light loses its static shadow permanently (see CommitStaticCache).
	m_PointShadows.CommitStaticCache();

	// Restore the HDR scene-color RT (+ shared depth) for the lit passes that follow — the world pass renders
	// into the HDR target, not the swapchain (Phase 3); the tonemap resolve composites it at the end of the frame.
	BindSceneColorTarget();
}


UINT D3D12GraphicsEngine::ResolveShadowDiffuseSlot( zCTexture* tex ) const {
	// "Already cached in, never CacheIn from here" contract (see [[skeletal-texani-shared-slots]]) — a shadow
	// caster must not pull textures in — returning the SRV heap index the bindless b6 MaterialCB wants. Used by
	// every shadow-recorded skeletal mesh and node attachment; a pool thread can read the result safely because
	// the resolve itself always happens on the main thread in PrepareFrameSkeletals.
	if ( tex && tex->GetCacheState() == zRES_CACHED_IN ) {
		if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
			if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
				D3D12Texture* d12 = D3D12Texture::From( gfx );
				if ( d12->HasSRV() ) return d12->GetSrvSlot();
			}
		}
	}
	return m_BlackTexture->GetSrvSlot();
}


UINT D3D12GraphicsEngine::ResolveDiffuseSlotCacheIn( zCTexture* tex ) {
	// The main-view variant: caches the texture in (the shadow paths deliberately don't) and returns its SRV
	// heap slot, falling back to the 1x1 black texture — the same fallback the old per-material descriptor-table
	// binds used, so an uncached material still draws exactly as before.
	if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
		if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
			if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
				D3D12Texture* d12 = D3D12Texture::From( gfx );
				if ( d12->HasSRV() ) return d12->GetSrvSlot();
			}
		}
	}
	return m_BlackTexture->GetSrvSlot();
}


bool D3D12GraphicsEngine::CreateLightCullBuffers( INT2 size ) {
	// Per-resolution Forward+ tile grid storage (P2.9b-2). Recreated on resize alongside the depth buffer.
	// RW_LightGrid: one {Offset,Count} (8 B) per 16x16 tile. RW_LightIndexList: a fixed MAX_LIGHTS_PER_TILE
	// (=32) uint slice per tile (no compaction/global counter — see the shader header). Both are DEFAULT-heap
	// UAV buffers created in UNORDERED_ACCESS; each frame DispatchLightCulling writes them (UAV) then transitions
	// them to PIXEL_SHADER_RESOURCE for the lit geometry passes to read, then back. ~1 MB total at 1080p.
	if ( size.x <= 0 || size.y <= 0 ) return false;
	ID3D12Device* device = m_Device.GetDevice();

	constexpr UINT kTileSize = 16;
	constexpr UINT kMaxLightsPerTile = 32;
	m_NumTilesX = (static_cast<UINT>(size.x) + kTileSize - 1) / kTileSize;
	m_NumTilesY = (static_cast<UINT>(size.y) + kTileSize - 1) / kTileSize;
	const UINT numTiles = m_NumTilesX * m_NumTilesY;
	if ( numTiles == 0 ) return false;

	D3D12MA::ALLOCATION_DESC heapDefault = {};
	heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	auto makeUavBuffer = [&]( UINT64 bytes, const wchar_t* name, ComPtr<ID3D12Resource>& out, ComPtr<D3D12MA::Allocation>& outAlloc ) -> bool {
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = bytes;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if ( FAILED( m_Allocator->CreateResource( &heapDefault, &bd,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, outAlloc.ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
			LogWarn() << "D3D12: failed to create a light-cull UAV buffer.";
			return false;
		}
		out->SetName( name );
		outAlloc->SetName( name );
		return true;
		};

	if ( !makeUavBuffer( static_cast<UINT64>(numTiles) * 2u * sizeof( uint32_t ), L"LightGrid", m_LightGridBuffer, m_LightGridBufferAlloc ) )
		return false;
	if ( !makeUavBuffer( static_cast<UINT64>(numTiles) * kMaxLightsPerTile * sizeof( uint32_t ), L"LightIndexList", m_LightIndexBuffer, m_LightIndexBufferAlloc ) )
		return false;
	m_LightGridInPixelState = false;   // freshly created in UNORDERED_ACCESS (see DispatchLightCulling round-trip)
	return true;
}



bool D3D12GraphicsEngine::CreateVobInstanceBuffers() {
	ID3D12Device* device = m_Device.GetDevice();
	D3D12MA::ALLOCATION_DESC uploadHeap = {};
	uploadHeap.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = kVobInstanceBufferBytes;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_Allocator->CreateResource( &uploadHeap, &bufDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_VobInstanceBufferAlloc[i].ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( m_VobInstanceBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_VobInstanceBuffer[i]->SetName( i == 0 ? L"VobInstanceRing0" : L"VobInstanceRing1" );
		m_VobInstanceBufferAlloc[i]->SetName( i == 0 ? L"AllocVobInstanceRing0" : L"AllocVobInstanceRing1" );
		D3D12_RANGE noRead = { 0, 0 };
		if ( FAILED( m_VobInstanceBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_VobInstanceBufferPtr[i] ) ) ) )
			return false;
	}
	m_VobInstanceBufferCapacity = kVobInstanceBufferBytes;
	return true;
}


bool D3D12GraphicsEngine::CreateLightBuffer() {
	// Per-frame point-light StructuredBuffers (one per in-flight frame). The whole visible-light list is
	// rewritten from offset 0 each frame, so these are plain persistently-mapped UPLOAD snapshots, bound
	// as a root SRV. Sized kMaxFrameLights * sizeof(GPULight).
	ID3D12Device* device = m_Device.GetDevice();
	D3D12MA::ALLOCATION_DESC uploadHeap = {};
	uploadHeap.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = static_cast<UINT64>(kMaxFrameLights) * sizeof( GPULight );
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_Allocator->CreateResource( &uploadHeap, &bufDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_LightBufferAlloc[i].ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( m_LightBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_LightBuffer[i]->SetName( i == 0 ? L"PointLightBuffer0" : L"PointLightBuffer1" );
		m_LightBufferAlloc[i]->SetName( i == 0 ? L"AllocPointLightBuffer0" : L"AllocPointLightBuffer1" );
		D3D12_RANGE noRead = { 0, 0 };
		if ( FAILED( m_LightBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_LightBufferPtr[i] ) ) ) )
			return false;
	}
	m_LightBufferCapacity = kMaxFrameLights;
	return true;
}


void D3D12GraphicsEngine::BuildFrameLightBuffer() {
	// Rebuild this frame's point-light buffer from the visible-light set collected in OnStartWorldRendering.
	// Mirrors D3D11 D3D11TiledDeferredShading::CullLights' CPU fill: skip disabled lights, unpack the color
	// DWORD (0xAARRGGBB), apply the fixed 1.2 lightFactor, store world-space position + range. View-space
	// position / shadow index are left defaulted (world-space shading in the MVP; tiling fills them later).
	m_FrameLightCount = 0;
	const UINT frame = m_FrameIndex;
	if ( !m_LightBuffer[frame] || !m_LightBufferPtr[frame] ) return;

	GPULight* dst = reinterpret_cast<GPULight*>(m_LightBufferPtr[frame]);
	UINT count = 0;
	constexpr float lightFactor = 1.2f;   // matches D3D11 CullLights RGB scale

	// Parallel to dst[]: the owning light Vob per GPULight index, so the shadow selection below can map a
	// chosen light back to its identity for STABLE per-light cube-slot ownership (static-aside cache, P2.10f).
	static std::vector<zCVobLight*> s_lightVobs;
	s_lightVobs.clear();

	// View-space transform for PositionView (consumed by the tiled light-culling CS). Mirrors D3D11
	// CullLights EXACTLY: transpose(GetViewMatrixXM()) then a row-vector transform of the world position,
	// so the view space this fills matches what the cull shader's frustum is built in.
	const XMMATRIX view = XMMatrixTranspose( Engine::GAPI->GetViewMatrixXM() );

	for ( VobLightInfo* li : g_FrameLights ) {
		if ( !li || !li->Vob ) continue;
		zCVobLight* vob = li->Vob;
		if ( !vob->IsEnabled() ) continue;
		if ( count >= m_LightBufferCapacity ) {
			if ( !m_LightOverflowLogged ) {
				LogWarn() << "D3D12: point-light buffer overflow (" << m_LightBufferCapacity
					<< " lights/frame). Excess lights dropped this frame.";
				m_LightOverflowLogged = true;
			}
			break;
		}
		const DWORD c = vob->GetLightColor();   // 0xAARRGGBB
		const float r = ((c >> 16) & 0xFF) / 255.0f;
		const float g = ((c >> 8) & 0xFF) / 255.0f;
		const float b = (c & 0xFF) / 255.0f;
		const XMFLOAT3 pw = vob->GetPositionWorld();

		GPULight& L = dst[count];
		XMStoreFloat3( &L.PositionView, XMVector3TransformCoord( XMLoadFloat3( &pw ), view ) );
		L.Range = vob->GetLightRange();
		L.Color = XMFLOAT4( r * lightFactor, g * lightFactor, b * lightFactor, vob->IsStatic() ? 0.0f : 1.0f );
		L.PositionWorld = pw;
		L.ShadowCubeIndex = -1;
		s_lightVobs.push_back( vob );
		++count;
	}
	m_FrameLightCount = count;

	// Point-light shadow selection: hand the filled buffer to the point-shadow module, which picks this
	// frame's shadowed lights (stable per-light cube slots, static-cache + round-robin scheduling) and writes
	// each winner's ShadowCubeIndex back into the GPU light struct. See D3D12PointShadows.h.
	m_PointShadows.SelectShadowedLights( dst, count, s_lightVobs );
}


void D3D12GraphicsEngine::BindFrameLights( UINT srvParam, UINT countParam, UINT gridParam, UINT indexParam ) {
	// Bind the Forward+ tiled point-light root params: srvParam = the light StructuredBuffer as a root SRV
	// (t1), countParam = LightCB { LightCount, NumTilesX, LimitLightIntensity }, gridParam/indexParam = the
	// per-tile grid (t2) and light-index list (t3) root SRVs produced by DispatchLightCulling. EVERY draw
	// whose bound PSO reads the tiled light loop MUST call this after setting its root signature, or the loop
	// bound (Count) and grid are UNDEFINED root values and can run billions of iterations → GPU timeout/
	// removal. Root args are cleared on every SetGraphicsRootSignature. The param indices differ per root
	// sig: m_Pipelines.World.RootSig uses (3,4,5,6) — the default — for the world mesh / instanced VOBs /
	// node attachments; m_Pipelines.Skeletal.RootSig uses (4,5,6,7); m_Pipelines.Grass.RootSig uses (5,6,7,8).
	m_CmdList->SetGraphicsRootShaderResourceView( srvParam, m_LightBuffer[m_FrameIndex]->GetGPUVirtualAddress() );
	m_CmdList->SetGraphicsRoot32BitConstant( countParam, m_FrameLightCount, 0 );   // LightCount @ b*.x
	m_CmdList->SetGraphicsRoot32BitConstant( countParam, m_NumTilesX, 1 );         // NumTilesX  @ b*.y
	// LimitLightIntensity @ b*.z — mirrors D3D11's ForwardPlusLighting.hlsl MAX-blend mode (swap "sum of
	// every overlapping point light" for "brightest single light" to avoid overexposure).
	const UINT limitLightIntensity = Engine::GAPI->GetRendererState().RendererSettings.LimitLightIntesity ? 1u : 0u;
	m_CmdList->SetGraphicsRoot32BitConstant( countParam, limitLightIntensity, 2 );
	m_CmdList->SetGraphicsRootShaderResourceView( gridParam, m_LightGridBuffer->GetGPUVirtualAddress() );
	m_CmdList->SetGraphicsRootShaderResourceView( indexParam, m_LightIndexBuffer->GetGPUVirtualAddress() );
}



bool D3D12GraphicsEngine::CreateParticleInstanceBuffers() {
	ID3D12Device* device = m_Device.GetDevice();
	D3D12MA::ALLOCATION_DESC uploadHeap = {};
	uploadHeap.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = kParticleInstanceBufferBytes;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_Allocator->CreateResource( &uploadHeap, &bufDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_ParticleInstanceBufferAlloc[i].ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( m_ParticleInstanceBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_ParticleInstanceBuffer[i]->SetName( i == 0 ? L"ParticleInstanceRing0" : L"ParticleInstanceRing1" );
		D3D12_RANGE noRead = { 0, 0 };
		if ( FAILED( m_ParticleInstanceBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_ParticleInstanceBufferPtr[i] ) ) ) )
			return false;
	}
	m_ParticleInstanceBufferCapacity = kParticleInstanceBufferBytes;
	return true;
}



XRESULT D3D12GraphicsEngine::DrawParticleEffects() {
	if ( !m_FrameOpen || !m_Pipelines.Particle.RootSig || !m_DepthBuffer )
		return XR_SUCCESS;

	auto& particles = Engine::GAPI->GetFrameParticles();
	auto& info = Engine::GAPI->GetFrameParticleInfo();

	// Clear the per-frame buckets BEFORE collecting. On D3D11 this happens in DrawWorldMeshNaive
	// (GothicAPI.cpp:1334) — but the D3D12 world pass never routes through it, so we must clear here or
	// the buckets accumulate every frame (particles smear, then the instance ring overflows and nothing
	// draws). DrawParticlesSimple appends into these, mirroring D3D11's clear-then-fill contract.
	particles.clear();
	info.clear();

	// Collect this frame's visible particle effects (backend-neutral). Fills FrameParticles (instances
	// bucketed by texture) + FrameParticleInfo (blend mode per texture). It also calls back into
	// DrawFrameParticleMeshes (D3D12Fx.cpp) for the mesh-shaped emitters, which draw right there —
	// before this pass binds the Particle root signature below, so no state has to be restored.
	Engine::GAPI->DrawParticlesSimple();
	if ( particles.empty() ) return XR_SUCCESS;

	// ViewProj — identical derivation to DrawWorldMesh (particle positions are world-space).
	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM( view );
	Engine::GAPI->ResetWorldTransform();
	const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
	const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
	XMFLOAT4X4 viewProj;
	XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

	XMFLOAT3 camPos;
	XMStoreFloat3( &camPos, Engine::GAPI->GetCameraPositionXM() );
	const float camConsts[4] = { camPos.x, camPos.y, camPos.z, 0.0f };

	m_CmdList->SetGraphicsRootSignature( m_Pipelines.Particle.RootSig.Get() );
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
	m_CmdList->SetGraphicsRoot32BitConstants( 1, 4, camConsts, 0 );

	D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
	D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );

	const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
	const UINT frame = m_FrameIndex;
	ID3D12PipelineState* lastPso = nullptr;
	unsigned int drawnTris = 0;

	for ( auto& [tex, instances] : particles ) {
		if ( instances.empty() ) continue;

		// Blend mode for this texture bucket (falls back to alpha blend if somehow unlisted).
		auto infoIt = info.find( tex );
		ID3D12PipelineState* pso = infoIt != info.end()
			? m_Pipelines.GetOrCreateParticlePipeline( infoIt->second.BlendState )
			: nullptr;
		if ( !pso ) {
			GothicBlendStateInfo alpha; alpha.SetAlphaBlending();
			pso = m_Pipelines.GetOrCreateParticlePipeline( alpha );
		}
		if ( !pso ) continue;
		if ( pso != lastPso ) { m_CmdList->SetPipelineState( pso ); lastPso = pso; }

		const UINT numInstances = static_cast<UINT>(instances.size());
		const UINT instBytes = numInstances * static_cast<UINT>(sizeof( ParticleInstanceInfo ));
		if ( m_ParticleInstanceBufferOffset + instBytes > m_ParticleInstanceBufferCapacity ) {
			if ( !m_ParticleInstanceOverflowLogged ) {
				LogWarn() << "D3D12: particle instance ring overflow (" << m_ParticleInstanceBufferCapacity
					<< " bytes/frame). Some particles dropped this frame.";
				m_ParticleInstanceOverflowLogged = true;
			}
			break;
		}

		const UINT instOffset = m_ParticleInstanceBufferOffset;
		memcpy( m_ParticleInstanceBufferPtr[frame] + instOffset, instances.data(), instBytes );
		m_ParticleInstanceBufferOffset += instBytes;
		const D3D12_VERTEX_BUFFER_VIEW instView = {
			m_ParticleInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( ParticleInstanceInfo ) };
		m_CmdList->IASetVertexBuffers( 0, 1, &instView );

		D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
		if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
			if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
				if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
					D3D12Texture* d12 = D3D12Texture::From( gfx );
					if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
				}
			}
		}
		m_CmdList->SetGraphicsRootDescriptorTable( 2, srv );

		// 4-vertex triangle-strip quad, one draw per particle instance.
		m_CmdList->DrawInstanced( 4, numInstances, 0, 0 );
		drawnTris += 2 * numInstances;
	}

	Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnTris;
	return XR_SUCCESS;
}




bool D3D12GraphicsEngine::CreateDecalInstanceBuffers() {
	ID3D12Device* device = m_Device.GetDevice();
	D3D12MA::ALLOCATION_DESC uploadHeap = {};
	uploadHeap.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = kDecalInstanceBufferBytes;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_Allocator->CreateResource( &uploadHeap, &bufDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_DecalInstanceBufferAlloc[i].ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( m_DecalInstanceBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_DecalInstanceBuffer[i]->SetName( i == 0 ? L"DecalInstanceRing0" : L"DecalInstanceRing1" );
		D3D12_RANGE noRead = { 0, 0 };
		if ( FAILED( m_DecalInstanceBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_DecalInstanceBufferPtr[i] ) ) ) )
			return false;
	}
	m_DecalInstanceBufferCapacity = kDecalInstanceBufferBytes;
	return true;
}


bool D3D12GraphicsEngine::CreateDecalQuadVB() {
	ID3D12Device* device = m_Device.GetDevice();

	// Shared unit quad (two triangles, corners +/-0.5, UV 0..1) — same 6 verts as D3D11's decal quad, so
	// the per-decal scale matrix's Y-flip (-DecalSize.y*2) lands the sprite the same way. GPU resource, so
	// it stays in the engine; the decal PSOs (in m_Pipelines.Decal) bind kDecalInputLayout to consume it.
	const DecalQuadVertex quad[6] = {
		{ -0.5f, -0.5f, 0.0f, 0.0f, 0.0f },
		{  0.5f, -0.5f, 0.0f, 1.0f, 0.0f },
		{ -0.5f,  0.5f, 0.0f, 0.0f, 1.0f },
		{  0.5f, -0.5f, 0.0f, 1.0f, 0.0f },
		{  0.5f,  0.5f, 0.0f, 1.0f, 1.0f },
		{ -0.5f,  0.5f, 0.0f, 0.0f, 1.0f },
	};
	D3D12MA::ALLOCATION_DESC uploadAlloc = {};
	uploadAlloc.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = sizeof( quad );
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if ( FAILED( m_Allocator->CreateResource( &uploadAlloc, &bufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_DecalQuadVBAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_DecalQuadVB.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_DecalQuadVB->SetName( L"DecalQuadVB" );
	void* mapped = nullptr;
	D3D12_RANGE noRead = { 0, 0 };
	if ( FAILED( m_DecalQuadVB->Map( 0, &noRead, &mapped ) ) ) return false;
	memcpy( mapped, quad, sizeof( quad ) );
	m_DecalQuadVB->Unmap( 0, nullptr );
	m_DecalQuadVBV = { m_DecalQuadVB->GetGPUVirtualAddress(), sizeof( quad ), sizeof( DecalQuadVertex ) };
	return true;
}



void D3D12GraphicsEngine::DrawDecalList( const std::vector<zCVob*>& decals, bool lighting ) {
	if ( !m_FrameOpen || !m_Pipelines.Decal.RootSig || !m_Pipelines.Decal.LitPSO || !m_DepthBuffer ) return;
	if ( decals.empty() ) return;

	GothicRendererState& rs = Engine::GAPI->GetRendererState();
	XMFLOAT3 camPos;
	XMStoreFloat3( &camPos, Engine::GAPI->GetCameraPositionXM() );

	// Build the per-decal instance data on the CPU (filter + camera alignment) — a direct port of D3D11's
	// DrawDecalList, minus the baked-in view matrix (the VS applies the standard ViewProj). The received
	// list is already sorted back-to-front; we keep that order (painter's algorithm) and DON'T batch.
	struct DecalMeta { zCTexture* texture; int alphaFunc; };
	static std::vector<DecalInstanceInfo> gpu;
	static std::vector<DecalMeta> meta;
	gpu.clear(); meta.clear();
	gpu.reserve( decals.size() ); meta.reserve( decals.size() );

	for ( zCVob* vob : decals ) {
		zCDecal* d = static_cast<zCDecal*>(vob->GetVisual());
		if ( !d ) continue;
		zCMaterial* material = d->GetDecalSettings()->DecalMaterial;
		if ( !material ) continue;
		zCTexture* texture = material->GetTextureSingle();
		if ( !texture ) continue;

		int alphaFunc = material->GetAlphaFunc();
		if ( alphaFunc == zMAT_ALPHA_FUNC_MAT_DEFAULT ) {
			alphaFunc = zMAT_ALPHA_FUNC_BLEND;
			if ( !texture->HasAlphaChannel() ) alphaFunc = zMAT_ALPHA_FUNC_NONE;
		}

		if ( lighting ) {
			// Opaque pass: only decals with no alpha or alpha test (blood, arrows).
			if ( !(alphaFunc == zMAT_ALPHA_FUNC_NONE || alphaFunc == zMAT_ALPHA_FUNC_TEST) ) continue;
		} else {
			// Transparent pass: only the supported blend modes; skip fully-transparent decals.
			switch ( alphaFunc ) {
			case zMAT_ALPHA_FUNC_BLEND:
			case zMAT_ALPHA_FUNC_BLEND_TEST:
			case zMAT_ALPHA_FUNC_ADD:
			case zMAT_ALPHA_FUNC_MUL:
			case zMAT_ALPHA_FUNC_MUL2:
				break;
			default:
				continue;
			}
			if ( (material->GetColor() >> 24) == 0 ) continue;
		}

		// Camera-alignment / world matrix — verbatim from D3D11 DrawDecalList (view is NOT applied here).
		int alignment = vob->GetAlignment();
		XMMATRIX world = vob->GetWorldMatrixXM();
		XMMATRIX offset =
			XMMatrixTranslation( d->GetDecalSettings()->DecalOffset.x, -d->GetDecalSettings()->DecalOffset.y, 0 );
		XMMATRIX scale =
			XMMatrixTranspose( XMMatrixScaling( d->GetDecalSettings()->DecalSize.x * 2,
				-d->GetDecalSettings()->DecalSize.y * 2, 1 ) );

		if ( alignment == zVISUAL_CAM_ALIGN_YAW ) {
			XMFLOAT3 decalPos = vob->GetPositionWorld();
			XMVECTOR at = XMVectorSet( decalPos.x - camPos.x, 0.0f, decalPos.z - camPos.z, 0.0f );
			XMFLOAT4 atLengthSq = {};
			XMStoreFloat4( &atLengthSq, XMVector3LengthSq( at ) );

			if ( atLengthSq.x > 1e-6f ) {
				XMMATRIX worldObj = XMMatrixTranspose( world );
				XMVECTOR translation = worldObj.r[3];

				at = XMVector3Normalize( at );
				XMVECTOR up = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
				XMVECTOR right = XMVector3Normalize( XMVector3Cross( up, at ) );
				up = XMVector3Normalize( XMVector3Cross( at, right ) );

				XMFLOAT3 right3 = {}, up3 = {}, at3 = {}, translation3 = {};
				XMStoreFloat3( &right3, right );
				XMStoreFloat3( &up3, up );
				XMStoreFloat3( &at3, at );
				XMStoreFloat3( &translation3, translation );

				worldObj.r[0] = XMVectorSet( right3.x, right3.y, right3.z, 0.0f );
				worldObj.r[1] = XMVectorSet( up3.x, up3.y, up3.z, 0.0f );
				worldObj.r[2] = XMVectorSet( at3.x, at3.y, at3.z, 0.0f );
				worldObj.r[3] = XMVectorSet( translation3.x, translation3.y, translation3.z, 1.0f );

				world = XMMatrixTranspose( worldObj );
			}
		} else if ( alignment == zVISUAL_CAM_ALIGN_FULL ) {
			XMFLOAT3 decalPos = vob->GetPositionWorld();
			XMVECTOR at = XMVectorSet( decalPos.x - camPos.x, decalPos.y - camPos.y, decalPos.z - camPos.z, 0.0f );
			XMFLOAT4 atLengthSq = {};
			XMStoreFloat4( &atLengthSq, XMVector3LengthSq( at ) );

			if ( atLengthSq.x > 1e-6f ) {
				at = XMVector3Normalize( at );
				XMVECTOR upRef = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
				XMFLOAT4 upDot = {};
				XMStoreFloat4( &upDot, XMVector3Dot( at, upRef ) );
				if ( fabsf( upDot.x ) > 0.999f ) upRef = XMVectorSet( 0.0f, 0.0f, 1.0f, 0.0f );

				XMVECTOR right = XMVector3Normalize( XMVector3Cross( upRef, at ) );
				XMVECTOR up = XMVector3Normalize( XMVector3Cross( at, right ) );

				XMFLOAT3 right3 = {}, up3 = {}, at3 = {};
				XMStoreFloat3( &right3, right );
				XMStoreFloat3( &up3, up );
				XMStoreFloat3( &at3, at );

				XMMATRIX worldObj;
				worldObj.r[0] = XMVectorSet( right3.x, right3.y, right3.z, 0.0f );
				worldObj.r[1] = XMVectorSet( up3.x, up3.y, up3.z, 0.0f );
				worldObj.r[2] = XMVectorSet( at3.x, at3.y, at3.z, 0.0f );
				worldObj.r[3] = XMVectorSet( decalPos.x, decalPos.y, decalPos.z, 1.0f );
				world = XMMatrixTranspose( worldObj );
			} else {
				world = XMMatrixTranspose( XMMatrixTranslation( decalPos.x, decalPos.y, decalPos.z ) );
			}
		}

		DecalInstanceInfo inst;
		XMStoreFloat4x4( &inst.World, world * offset * scale );
		const float ghostAlpha = lighting ? 1.0f : ((material->GetColor() >> 24) * (1.0f / 255.0f));
		inst.Color = XMFLOAT4( 1.0f, 1.0f, 1.0f, ghostAlpha );
		gpu.push_back( inst );
		meta.push_back( { material->GetAniTexture(), alphaFunc } );
	}

	if ( gpu.empty() ) return;

	// Snapshot the instances into this frame's ring; bind as the slot-1 stream (StartInstanceLocation picks
	// each decal, so we keep the exact submission order without splitting the memcpy).
	const UINT frame = m_FrameIndex;
	const UINT instBytes = static_cast<UINT>(gpu.size() * sizeof( DecalInstanceInfo ));
	if ( m_DecalInstanceBufferOffset + instBytes > m_DecalInstanceBufferCapacity ) {
		if ( !m_DecalInstanceOverflowLogged ) {
			LogWarn() << "D3D12: decal instance ring overflow (" << m_DecalInstanceBufferCapacity
				<< " bytes/frame). Some decals dropped this frame.";
			m_DecalInstanceOverflowLogged = true;
		}
		return;
	}
	const UINT instOffset = m_DecalInstanceBufferOffset;
	memcpy( m_DecalInstanceBufferPtr[frame] + instOffset, gpu.data(), instBytes );
	m_DecalInstanceBufferOffset += instBytes;

	// ViewProj — identical derivation to the opaque passes (decal instance matrices are model-space).
	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM( view );
	Engine::GAPI->ResetWorldTransform();
	const XMFLOAT4X4& viewM = rs.TransformState.TransformView;
	const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
	XMFLOAT4X4 viewProj;
	XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

	m_CmdList->SetGraphicsRootSignature( m_Pipelines.Decal.RootSig.Get() );
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );

	D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
	D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	const D3D12_VERTEX_BUFFER_VIEW instView = {
		m_DecalInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( DecalInstanceInfo ) };
	const D3D12_VERTEX_BUFFER_VIEW views[2] = { m_DecalQuadVBV, instView };
	m_CmdList->IASetVertexBuffers( 0, 2, views );

	ID3D12PipelineState* lastPso = nullptr;
	if ( lighting ) { m_CmdList->SetPipelineState( m_Pipelines.Decal.LitPSO.Get() ); lastPso = m_Pipelines.Decal.LitPSO.Get(); }

	const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
	zCTexture* lastTex = reinterpret_cast<zCTexture*>(~static_cast<uintptr_t>(0));  // force first bind
	unsigned int drawnTris = 0;

	for ( size_t i = 0; i < gpu.size(); ++i ) {
		if ( !lighting ) {
			GothicBlendStateInfo blend;
			switch ( meta[i].alphaFunc ) {
			case zMAT_ALPHA_FUNC_ADD:  blend.SetAdditiveBlending();  break;
			case zMAT_ALPHA_FUNC_MUL:  blend.SetModulateBlending();  break;
			case zMAT_ALPHA_FUNC_MUL2: blend.SetModulate2Blending(); break;
			default:                   blend.SetAlphaBlending();     break;  // BLEND / BLEND_TEST
			}
			ID3D12PipelineState* pso = m_Pipelines.GetOrCreateDecalBlendPipeline( blend );
			if ( !pso ) continue;
			if ( pso != lastPso ) { m_CmdList->SetPipelineState( pso ); lastPso = pso; }
		}

		zCTexture* tex = meta[i].texture;
		if ( tex != lastTex ) {
			D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
			if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
				if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
					if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
						D3D12Texture* d12 = D3D12Texture::From( gfx );
						if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
					}
				}
			}
			m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );
			lastTex = tex;
		}

		m_CmdList->DrawInstanced( 6, 1, 0, static_cast<UINT>(i) );
		drawnTris += 2;
	}

	rs.RendererInfo.FrameDrawnTriangles += drawnTris;
}


void D3D12GraphicsEngine::DrawGhostVobs() {
	// GothicAPI::TransparencyVobs is populated every frame by CollectVisibleVobs' GetVisualAlpha() branch
	// (invisible-potion/fade-out items) and is otherwise ONLY drained by D3D11's GothicAPI::DrawTransparencyVobs
	// (hard-wired to D3D11GraphicsEngine — never called on this backend). Without a consumer here the list grows
	// unbounded, one entry per ghost vob per frame, forever — so this function MUST run every frame regardless
	// of whether the Ghost PSO exists.
	auto& transparencyVobs = Engine::GAPI->GetTransparencyVobs();
	if ( transparencyVobs.empty() ) return;

	if ( !m_FrameOpen || ( !m_Pipelines.Ghost.PSO && !m_Pipelines.GhostSkeletal.PSO ) ) {
		transparencyVobs.clear();   // drop this frame's ghosts rather than leak; drawing is unavailable right now
		return;
	}

	// Reversed-Z ViewProj — identical derivation to DrawVobsInstanced/DrawWorldMesh.
	GothicRendererState& rs = Engine::GAPI->GetRendererState();
	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM( view );
	Engine::GAPI->ResetWorldTransform();
	const XMFLOAT4X4& viewM = rs.TransformState.TransformView;
	const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
	XMFLOAT4X4 viewProj;
	XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

	D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
	D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
	unsigned int drawnTris = 0;
	const UINT frame = m_FrameIndex;
	const auto now = Engine::GAPI->GetTotalTimeDW();
	static std::vector<XMFLOAT4X4> ghostBoneCache;

	// D3D11 draws these back-to-front (painter's algorithm) via a min/max-heap drain; the legacy
	// CollectVisibleVobs path (used by this backend, GothicAPI.cpp's std::ranges::sort(TransparencyVobs,
	// CompareGhostDistance)) instead leaves the vector plain-sorted NEAREST-first, so iterate it in reverse.
	//
	// That sort only covers the STATIC ghosts CollectVisibleVobs pushed; PrepareFrameSkeletals appends the
	// skeletal ghosts afterwards, leaving an unsorted tail. Re-sort here (nearest-first, matching
	// GothicAPI's CompareGhostDistance) so the reverse walk below is a correct far-to-near order across both
	// sources — otherwise overlapping ghosts blend in collection order rather than depth order.
	std::ranges::sort( transparencyVobs,
		[]( const TransparencyVobInfo& a, const TransparencyVobInfo& b ) { return a.distance < b.distance; } );

	for ( auto it = transparencyVobs.rbegin(); it != transparencyVobs.rend(); ++it ) {
		const TransparencyVobInfo& info = *it;

		// Skeletal ghosts (invisible/fading NPCs) — mirrors D3D11's DrawTransparencyVobs skeletalVob branch, minus
		// its same-mesh Z-prepass (same simplification the non-skeletal ghost path already made). Bone transforms
		// are computed here directly rather than via PrepareFrameSkeletals/g_SkelUploadCache: that pass explicitly
		// excludes ghost vobs (they never enter the normal skinned-color draw), so there is no cached pose to reuse.
		if ( info.skeletalVob ) {
			SkeletalVobInfo* skel = info.skeletalVob;
			if ( !skel->Vob || !skel->VisualInfo ) continue;
			// NOTE: the GhostSkeletal pipeline / skeletal CB-ring guards deliberately live further down, around
			// the base-mesh block only — a missing skinned pipeline must not also cost the ghost its node
			// attachments, which draw through the separate m_Pipelines.Ghost.

			zCModel* model = static_cast<zCModel*>( skel->Vob->GetVisual() );
			if ( !model ) continue;
			SkeletalMeshVisualInfo* visual = static_cast<SkeletalMeshVisualInfo*>( skel->VisualInfo );
			// An empty SkeletalMeshes list is NOT a reason to skip: attachment-only ghosts (and every NPC's
			// head, which is a .MMS node attachment rather than part of the soft-skin body) still have geometry
			// to draw below. Mirrors D3D11's DrawSkeletalMeshVobs, which guards only its BASE pass on
			// !SkeletalMeshes.empty() and always runs the attachment pass.

			if ( skel->LastAniUpdateFrame != now ) {
				skel->LastAniUpdateFrame = now;
				model->UpdateAttachedVobs();
			}
			model->UpdateMeshLibTexAniState();

			ghostBoneCache.clear();
			model->GetBoneTransforms( &ghostBoneCache );
			UINT numBones = static_cast<UINT>( ghostBoneCache.size() );
			if ( numBones == 0 ) continue;
			if ( numBones > kSkeletalMaxBones ) numBones = kSkeletalMaxBones;

			const XMMATRIX xmWorld = skel->Vob->GetWorldMatrixXM() * XMMatrixScalingFromVector( model->GetModelScaleXM() );

			// --- Base skinned mesh (the body). Skipped for attachment-only ghosts; the node attachments below
			// run either way. Needs the GhostSkeletal pipeline + the skeletal CB ring, neither of which the
			// attachment pass uses, so both guards live here rather than at the top of the branch.
			if ( !visual->SkeletalMeshes.empty() && m_Pipelines.GhostSkeletal.PSO && m_Pipelines.GhostSkeletal.RootSig
				&& m_SkeletalCBBuffer[frame] && m_SkeletalCBBufferPtr[frame] ) {
			SkeletalInstanceCB inst = {};
			XMStoreFloat4x4( &inst.World, xmWorld );
			inst.ModelColor = XMFLOAT4( 1, 1, 1, 1 );   // unused by VSDepth/PSGhost (unlit) — kept for struct-layout parity
			inst.Fatness = model->GetModelFatness();

			const UINT instSize = static_cast<UINT>( sizeof( SkeletalInstanceCB ) );
			const UINT boneSize = numBones * static_cast<UINT>( sizeof( XMFLOAT4X4 ) );
			const UINT instOff = AlignCB( m_SkeletalCBBufferOffset );
			const UINT boneOff = AlignCB( instOff + instSize );
			if ( boneOff + boneSize > m_SkeletalCBBufferCapacity ) {
				if ( !m_SkeletalCBOverflowLogged ) {
					LogWarn() << "D3D12: skeletal CB ring overflow (" << m_SkeletalCBBufferCapacity
						<< " bytes/frame). Some skeletal meshes (including ghosts) dropped this frame.";
					m_SkeletalCBOverflowLogged = true;
				}
				continue;
			}
			uint8_t* ringBase = m_SkeletalCBBufferPtr[frame];
			memcpy( ringBase + instOff, &inst, instSize );
			memcpy( ringBase + boneOff, ghostBoneCache.data(), boneSize );
			m_SkeletalCBBufferOffset = boneOff + boneSize;
			const D3D12_GPU_VIRTUAL_ADDRESS ringGpu = m_SkeletalCBBuffer[frame]->GetGPUVirtualAddress();

			m_CmdList->SetPipelineState( m_Pipelines.GhostSkeletal.PSO.Get() );
			m_CmdList->SetGraphicsRootSignature( m_Pipelines.GhostSkeletal.RootSig.Get() );
			m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
			m_CmdList->SetGraphicsRootConstantBufferView( 1, ringGpu + instOff );
			m_CmdList->SetGraphicsRootConstantBufferView( 2, ringGpu + boneOff );
			m_CmdList->SetGraphicsRoot32BitConstants( 3, 1, &info.alpha, 0 );

			for ( auto const& [mat, meshList] : visual->SkeletalMeshes ) {
				zCTexture* tex = mat ? mat->GetAniTexture() : nullptr;
				// b6 { normal, ORM, diffuse } — same bindless material block as the lit skeletal passes; PSGhost
				// reads only the diffuse index, but sharing the block keeps every skeletal entry point on one
				// SampleSkelDiffuse and off descriptor tables entirely.
				BindMaterialMaps( tex, 4, ResolveDiffuseSlotCacheIn( tex ) );
				for ( auto const& mesh : meshList ) {
					if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer ) continue;
					D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
					D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
					if ( !mvb->GetResource() || !mib->GetResource() ) continue;
					const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExSkelVertexStruct ) };
					m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
					const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
					m_CmdList->IASetIndexBuffer( &ibv );
					m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 1, 0, 0, 0 );
					drawnTris += static_cast<unsigned int>( mesh->Indices.size() ) / 3;
				}
			}
			}   // end base skinned mesh

			// --- Node attachments (HEADS, weapons, held items, lamps). An NPC's head is a .MMS node attachment,
			// not part of the soft-skin body, so without this a ghost NPC renders headless and unarmed.
			//
			// Drawn through the NON-skeletal m_Pipelines.Ghost, which fits exactly: attachments are RIGID
			// ExVertexStruct meshes (POSITION@0 + TEXCOORD@24 — its input layout) whose world matrix is
			// modelWorld * boneMatrix[node], and that pipeline takes World as a b1 root constant. So no new PSO,
			// no new shader and no VOB instance-ring traffic — unlike the lit path (DrawSkeletalColor), which
			// must use the instanced VobAttach PSO because it batches. Ghosts are a handful of objects; a root
			// constant per attachment is cheaper than a ring allocation.
			//
			// Fatness/Scaling (the VobAttach PSO's morph inflate) are deliberately NOT applied: Preview.hlsl's
			// VSMain has no such input, and D3D11's ghost path goes through DrawSkeletalMeshVob the same way.
			if ( m_Pipelines.Ghost.PSO && m_Pipelines.Ghost.RootSig ) {
				zCArray<zCModelNodeInst*>* nodeList = model->GetNodeList();
				const int nodeCount = nodeList
					? std::min<int>( static_cast<int>( ghostBoneCache.size() ), nodeList->NumInArray ) : 0;
				bool ghostPipelineBound = false;

				for ( int n = 0; n < nodeCount; ++n ) {
					zCModelNodeInst* node = nodeList->Array[n];
					if ( !node || !node->NodeVisual ) continue;   // nothing attached to this node

					// Ghosts never pass through PrepareFrameSkeletals' attachment block (they are rerouted out
					// of it), so this is the ONLY place that kicks their extraction — without it a permanently
					// ghosted NPC would never get its attachments built at all. Same async contract as the lit
					// path: the attachment simply isn't drawn until the worker lands, gated by Ready below.
					auto it = skel->NodeAttachments.find( n );
					if ( it == skel->NodeAttachments.end() ) {
						WorldConverter::ExtractNodeVisualAsync( n, node, skel->NodeAttachments );
						it = skel->NodeAttachments.find( n );
					} else if ( !it->second.empty() && it->second[0]
						&& it->second[0]->Ready.load( std::memory_order_acquire )
						&& it->second[0]->Visual != node->NodeVisual ) {
						WorldConverter::ExtractNodeVisualAsync( n, node, skel->NodeAttachments );  // visual changed
						it = skel->NodeAttachments.find( n );
					}
					if ( it == skel->NodeAttachments.end() ) continue;

					XMFLOAT4X4 attWorld;
					XMStoreFloat4x4( &attWorld, xmWorld * XMLoadFloat4x4( &ghostBoneCache[n] ) );

					for ( MeshVisualInfo* mvi : it->second ) {
						if ( !mvi ) continue;
						// Still being extracted on a worker — Meshes is being written right now, don't race it.
						if ( !mvi->Ready.load( std::memory_order_acquire ) || !mvi->Visual ) continue;

						// Texture animation + facial/bow morphing, same calls the lit attachment path makes.
						// A ghost NPC's head is exactly this case, so skipping it would freeze its face.
						node->TexAniState.UpdateTexList();
						if ( strcmp( mvi->Visual->GetFileExtension( 0 ), ".MMS" ) == 0 )
							WorldConverter::UpdateMorphMeshVisual( mvi->Visual, mvi );

						if ( !ghostPipelineBound ) {
							m_CmdList->SetPipelineState( m_Pipelines.Ghost.PSO.Get() );
							m_CmdList->SetGraphicsRootSignature( m_Pipelines.Ghost.RootSig.Get() );
							m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
							m_CmdList->SetGraphicsRoot32BitConstants( 2, 1, &info.alpha, 0 );
							ghostPipelineBound = true;
						}
						m_CmdList->SetGraphicsRoot32BitConstants( 1, 16, &attWorld, 0 );

						for ( auto const& [attMat, attMeshes] : mvi->Meshes ) {
							D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
							zCTexture* attTex = attMat ? attMat->GetAniTexture() : nullptr;
							if ( attTex && attTex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
								if ( MyDirectDrawSurface7* surface = attTex->GetSurface() ) {
									if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
										D3D12Texture* d12 = D3D12Texture::From( gfx );
										if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
									}
								}
							}
							m_CmdList->SetGraphicsRootDescriptorTable( 3, srv );

							for ( auto const& attMesh : attMeshes ) {
								if ( !attMesh || attMesh->Indices.empty() ) continue;
								if ( !attMesh->GetMeshVertexBuffer() || !attMesh->GetMeshIndexBuffer() ) continue;
								D3D12VertexBuffer* avb = D3D12VertexBuffer::From( attMesh->GetMeshVertexBuffer() );
								D3D12VertexBuffer* aib = D3D12VertexBuffer::From( attMesh->GetMeshIndexBuffer() );
								if ( !avb->GetResource() || !aib->GetResource() ) continue;
								const D3D12_VERTEX_BUFFER_VIEW vbv = {
									avb->GetGpuVirtualAddress(), avb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
								m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
								const D3D12_INDEX_BUFFER_VIEW ibv = {
									aib->GetGpuVirtualAddress(), aib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
								m_CmdList->IASetIndexBuffer( &ibv );
								m_CmdList->DrawIndexedInstanced( static_cast<UINT>( attMesh->Indices.size() ), 1, 0, 0, 0 );
								drawnTris += static_cast<unsigned int>( attMesh->Indices.size() ) / 3;
							}
						}
					}
				}
			}
			continue;
		}

		if ( !info.normalVob || !info.normalVob->VisualInfo || !m_Pipelines.Ghost.PSO || !m_Pipelines.Ghost.RootSig ) continue;

		VobInfo* vi = info.normalVob;
		m_CmdList->SetPipelineState( m_Pipelines.Ghost.PSO.Get() );
		m_CmdList->SetGraphicsRootSignature( m_Pipelines.Ghost.RootSig.Get() );
		m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
		XMFLOAT4X4 world = vi->WorldMatrix;
		m_CmdList->SetGraphicsRoot32BitConstants( 1, 16, &world, 0 );
		m_CmdList->SetGraphicsRoot32BitConstants( 2, 1, &info.alpha, 0 );

		MeshVisualInfo* visual = static_cast<MeshVisualInfo*>( vi->VisualInfo );
		for ( auto const& materialMesh : visual->Meshes ) {
			D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
			if ( materialMesh.first ) {
				if ( zCTexture* aniTex = materialMesh.first->GetAniTexture() ) {
					if ( aniTex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
						if ( MyDirectDrawSurface7* surface = aniTex->GetSurface() ) {
							if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
								D3D12Texture* d12 = D3D12Texture::From( gfx );
								if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
							}
						}
					}
				}
			}
			m_CmdList->SetGraphicsRootDescriptorTable( 3, srv );

			for ( auto const& meshInfo : materialMesh.second ) {
				if ( !meshInfo || meshInfo->Indices.empty() || !meshInfo->GetMeshVertexBuffer() || !meshInfo->GetMeshIndexBuffer() )
					continue;
				D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( meshInfo->GetMeshVertexBuffer() );
				D3D12VertexBuffer* mib = D3D12VertexBuffer::From( meshInfo->GetMeshIndexBuffer() );
				if ( !mvb->GetResource() || !mib->GetResource() ) continue;

				const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
				m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
				const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
				m_CmdList->IASetIndexBuffer( &ibv );

				m_CmdList->DrawIndexedInstanced( static_cast<UINT>(meshInfo->Indices.size()), 1, 0, 0, 0 );
				drawnTris += static_cast<unsigned int>(meshInfo->Indices.size()) / 3;
			}
		}
	}

	transparencyVobs.clear();
	rs.RendererInfo.FrameDrawnTriangles += drawnTris;
}


void D3D12GraphicsEngine::DrawVegetation() {
	// GVegetationBox instanced grass cards (P2.12). Bypasses GVegetationBox's own D3D11-only
	// PrepareRenderGeometryPipeline()/RenderVegetation() (a SetActiveVertexShader/SetupVS_Ex* state-machine
	// path with no D3D12 equivalent) — this backend reads the box's data (spot count, instancing buffer,
	// mesh, textures, bounding box) through the plain getters added alongside this feature and draws it
	// directly with its own PSO. Mirrors D3D11's DrawVegetationGeometryPass: distance + frustum cull per box,
	// (re)bind pipeline state once per frame the first time a box is actually in view, draw each visible box.
	if ( !m_FrameOpen || !m_Pipelines.Grass.PSO || !m_Pipelines.Grass.RootSig || !m_DepthBuffer ) return;
	const auto& vegetationBoxes = Engine::GAPI->GetVegetationBoxes();
	if ( vegetationBoxes.empty() ) return;

	DX_ZONE( m_CmdList, "Draw vegetation" );
	TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw vegetation" );

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM( view );
	Engine::GAPI->ResetWorldTransform();
	const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
	const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
	XMFLOAT4X4 viewProj;
	XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

	// Same frustum construction as BuildWorldDrawCommands — the player's real camera, not the cascade/light
	// frustums used elsewhere in this file.
	Frustum playerFrustum = Frustum::AlwaysContainingFrustum();
	if ( auto cam = (zCCamera*)oCGame::GetGame()->_zCSession_camera ) {
		const auto& camView = cam->trafoView;
		const auto& camProj = cam->trafoProjection;
		playerFrustum.BuildPerspective( XMMatrixTranspose( XMLoadFloat4x4( &camView ) ), XMLoadFloat4x4( &camProj ) );
	}

	const XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();
	auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
	const float drawRadius = settings.OutdoorSmallVobDrawRadius;
	const FogConstants fog = MakeFogConstants();

	// Mirrors GVegetationBox::PopulateConstantBuffer, minus G_NormalVS (see Vegetation.hlsl) — 8 32-bit values
	// to match GrassCB's HLSL layout exactly (root constants map by DWORD offset).
	struct GrassCBData { float Time; float WindStrength; float HeroAffectStrength; float _pad0; XMFLOAT3 PlayerPosWS; float _pad1; };
	static_assert( sizeof( GrassCBData ) == 32, "GrassCBData must be 8 DWORDs to match Vegetation.hlsl's GrassCB" );
	GrassCBData gcb = {};
	gcb.Time = Engine::GAPI->GetTimeSeconds();
	gcb.WindStrength = settings.WindQuality > 0 ? settings.GlobalWindStrength : 0.0f;
	if ( settings.HeroAffectsObjects ) {
		gcb.PlayerPosWS = Engine::GAPI->GetPlayerVob() ? Engine::GAPI->GetPlayerVob()->GetPositionWorld() : XMFLOAT3( 0, 0, 0 );
		gcb.HeroAffectStrength = 1.0f;
	} else {
		gcb.PlayerPosWS = XMFLOAT3( 0, 0, 0 );
		gcb.HeroAffectStrength = 0.0f;
	}

	const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };

	bool bound = false;
	unsigned int drawnTris = 0;
	GothicRendererState& rs = Engine::GAPI->GetRendererState();

	for ( GVegetationBox* box : vegetationBoxes ) {
		if ( !box || box->GetSpotCount() == 0 ) continue;

		XMFLOAT3 bbMin, bbMax;
		box->GetBoundingBox( &bbMin, &bbMax );

		if ( Toolbox::ComputePointAABBDistance( camPos, bbMin, bbMax ) > drawRadius ) continue;
		if ( !playerFrustum.Intersects( zTBBox3D{ bbMin, bbMax } ) ) continue;

		GMeshSimple* mesh = box->GetVegetationMesh();
		GfxVertexBuffer* instBuf = box->GetInstancingBuffer();
		if ( !mesh || !instBuf ) continue;
		D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->GetVertexBuffer() );
		D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->GetIndexBuffer() );
		D3D12VertexBuffer* mib_inst = D3D12VertexBuffer::From( instBuf );
		if ( !mvb || !mib || !mib_inst || !mvb->GetResource() || !mib->GetResource() || !mib_inst->GetResource() ) continue;

		if ( !bound ) {
			bound = true;
			m_CmdList->SetPipelineState( m_Pipelines.Grass.PSO.Get() );
			m_CmdList->SetGraphicsRootSignature( m_Pipelines.Grass.RootSig.Get() );
			m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
			m_CmdList->SetGraphicsRoot32BitConstants( 3, 8, &gcb, 0 );
			m_CmdList->SetGraphicsRoot32BitConstants( 4, 8, &fog, 0 );
			BindFrameLights( 5, 6, 7, 8 );
			m_CmdList->SetGraphicsRootConstantBufferView( 9, m_ShadowCBGpu[m_FrameIndex] );
			m_CmdList->SetGraphicsRootDescriptorTable( 10, GetSrvGpuHandle( m_ShadowMap.GetSrvSlot() ) );
			m_CmdList->SetGraphicsRootDescriptorTable( 11, GetSrvGpuHandle( m_PointShadows.GetSrvSlot() ) );
			m_CmdList->SetGraphicsRoot32BitConstants( 12, 1, &m_ActiveAOMaskSrvSlot, 0 );   // b5 AOCB (simple SSAO mask)
			m_CmdList->RSSetViewports( 1, &vp );
			m_CmdList->RSSetScissorRects( 1, &sc );
			m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
		}

		D3D12_GPU_DESCRIPTOR_HANDLE grassSrv = whiteSrv;
		if ( GfxTexture* grassTex = box->GetVegetationTexture() ) {
			D3D12Texture* d12 = D3D12Texture::From( grassTex );
			if ( d12 && d12->HasSRV() ) grassSrv = d12->GetSrvGpuHandle();
		}
		m_CmdList->SetGraphicsRootDescriptorTable( 1, grassSrv );

		D3D12_GPU_DESCRIPTOR_HANDLE groundSrv = whiteSrv;
		if ( zCTexture* groundTex = box->GetMeshTexture() ) {
			if ( groundTex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
				if ( MyDirectDrawSurface7* surface = groundTex->GetSurface() ) {
					if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
						D3D12Texture* d12 = D3D12Texture::From( gfx );
						if ( d12 && d12->HasSRV() ) groundSrv = d12->GetSrvGpuHandle(); 
					}
				}
			} else {
				continue;   // ground texture not cached in yet — matches D3D11's RenderVegetation early-out
			}
		}
		m_CmdList->SetGraphicsRootDescriptorTable( 2, groundSrv );

		const UINT numIndices = mesh->GetNumIndices();
		const UINT numInstances = static_cast<UINT>( box->GetSpotCount() );
		const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( SimpleObjectVertexStruct ) };
		const D3D12_VERTEX_BUFFER_VIEW instVbv = { mib_inst->GetGpuVirtualAddress(), mib_inst->GetSizeInBytes(), sizeof( XMFLOAT4X4 ) };
		const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, instVbv };
		m_CmdList->IASetVertexBuffers( 0, 2, views );
		const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
		m_CmdList->IASetIndexBuffer( &ibv );
		m_CmdList->DrawIndexedInstanced( numIndices, numInstances, 0, 0, 0 );
		drawnTris += ( numIndices / 3 ) * numInstances;
	}

	rs.RendererInfo.FrameDrawnTriangles += drawnTris;
}


bool D3D12GraphicsEngine::CreateSkeletalConstantBuffers() {
	ID3D12Device* device = m_Device.GetDevice();
	D3D12MA::ALLOCATION_DESC uploadAlloc = {};
	uploadAlloc.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = kSkeletalConstantBufferBytes;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_Allocator->CreateResource( &uploadAlloc, &bufDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_SkeletalCBBufferAlloc[i].ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( m_SkeletalCBBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_SkeletalCBBuffer[i]->SetName( i == 0 ? L"SkeletalCBRing0" : L"SkeletalCBRing1" );
		D3D12_RANGE noRead = { 0, 0 };
		if ( FAILED( m_SkeletalCBBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_SkeletalCBBufferPtr[i] ) ) ) )
			return false;
	}
	m_SkeletalCBBufferCapacity = kSkeletalConstantBufferBytes;
	return true;
}


XRESULT D3D12GraphicsEngine::OnStartWorldRendering() {

	// m_PresentPending prevents inventory-world from rendering the whole game scenery for every inventory tile.
	// The engine sadly works like that.
	// the first OnStartWorldRendering after a Present() will be the correct one to draw the world.
	if ( m_PresentPending ) return XR_SUCCESS;

    TracyD3D12ZoneCGX(m_CmdList.Get(), "OnStartWorldRendering");
    
    Engine::GAPI->SetFarPlane( Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE);

	// Decide ONCE, before anything draws, whether this frame's post-pass height fog runs (RenderFogAndGodRays,
	// near the bottom of this function). Every lit geometry pass reads the result via MakeFogConstants to
	// suppress its own cheap linear distance fog — that fog is D3D12's stand-in for this pass, and running
	// both would fog the scene twice (D3D11's world/VOB/skeletal shaders apply no distance fog at all).
	m_HeightFogActive = EvaluateHeightFogActive();
	g_HeightFogActive = m_HeightFogActive;

	// Decide ONCE, before anything collects, whether this frame's static VOBs are culled on the GPU. Every stage
	// downstream has to agree: the CollectVisibleVobs call below switches to distance-only, UploadFrameVobInstances
	// emits the per-visual cull records, BuildVobDrawCommands points each command's instance stream at the
	// compacted buffer, and both VOB passes ExecuteIndirect from the patched arg buffer. Re-evaluating any of that
	// mid-frame would mix culled and unculled state. See D3D12Cull.cpp.
	m_GpuVobCullActive = EvaluateGpuVobCulling();

	// zCBspNodeRender hook — Gothic's BSP traversal is replaced; we draw the world ourselves.
	// Order mirrors D3D11's DrawWorldMeshNaive: sky background, world mesh, skeletal (NPCs/monsters),
	// then instanced static VOBs. The sky is a fog-colored fill so the horizon dissolves into the
	// per-pixel distance fog of the geometry.

	// Collect the frame's visible vobs/lights/mobs ONCE (CollectVisibleVobs has side effects — it fills each
	// visual's Instances list — so it must run exactly once), then rebuild the per-frame point-light buffer so
	// every geometry pass (world, VOBs, skeletal) lights against the same visible-light set. Mirrors D3D11
	// filling m_FrameLights during collection.
	// skipVobFrustumCull (last arg): with GPU culling on, static VOBs are collected DISTANCE-ONLY — no per-VOB
	// frustum test and no BSP-node frustum rejection, so everything in range (including behind the camera) reaches
	// the GPU cull below. Lights and skeletal MOBs keep their frustum tests either way (the light buffer is capped,
	// and the per-draw skeletal path has no GPU cull yet).
	g_FrameVobs.clear(); g_FrameLights.clear(); g_FrameMobs.clear();
	Engine::GAPI->CollectVisibleVobs( g_FrameVobs, g_FrameLights, g_FrameMobs,
		EGothicCullFlags::CullAll, EBspTreeCollectFlags::COLLECT_ALL_MUTATE, m_GpuVobCullActive );
	BuildFrameLightBuffer();
	// Snapshot ALL opaque instanced geometry ONCE, before the depth prepass + cull, so every geometry pass draws
	// from one shared upload: VOB instances (g_FrameVobUploads), then skeletal base/attachment CBs + instances
	// (g_FrameSkelDraws/g_FrameAttachDraws — PrepareFrameSkeletals also runs the once/frame animation update, so
	// it MUST run exactly once). Both skeletal lists (animated + static mobs) are prepared here up front.
	UploadFrameVobInstances();
	g_FrameSkelDraws.clear(); g_FrameAttachDraws.clear();
	g_SkelUploadCache.clear();   // per-vob CB/attachment upload cache — rebuilt fresh each frame
	g_SkelMatSrvCount = 0;       // ...and its parallel per-material diffuse-handle snapshots (capacity retained)
	// collectGhosts=true ONLY here: this is the list D3D11's GothicAPI::DrawWorldMeshNaive walks, and the
	// reroute of ghost NPCs into TransparencyVobs is that function's job. Static MOBs (g_FrameMobs) keep the
	// plain drop — D3D11 does not reroute them either.
	PrepareFrameSkeletals( Engine::GAPI->GetAnimatedSkeletalMeshVobs(), nullptr, -1, nullptr, 0.0f, 1, true );
	PrepareFrameSkeletals( g_FrameMobs );

	// Refresh the wind CB's player position ONCE here (before shadows/prepass/color all run this frame) — windDir/
	// globalTime were already advanced once in OnBeginFrame; minHeight/maxHeight are refreshed per-visual right
	// before each pass's VOB draws (DrawVobDepthPrepass / the CSM cascades / DrawVobsInstanced).
	if ( zCVob* player = Engine::GAPI->GetPlayerVob() ) {
		m_WindBuffer.playerPos = player->GetPositionWorld();
	}

	// Phase 3 HDR: redirect the whole 3D scene into the R16F scene-color target (OnBeginFrame bound the
	// swapchain for 2D-only/menu frames; here we switch to HDR so lighting can exceed 1.0). Depth is shared.
	BindSceneColorTarget();

	// CSM cascades (P2.9c): compute this frame's cascade matrices + sampling CB and LAUNCH the per-cascade
	// caster culls on the worker pool. This returns immediately — the BSP walks run concurrently with everything
	// below, all the way down to FinishShadowPasses, which joins them right before the lit geometry pass. Same
	// split D3D11ShadowMap uses (PrepareRender enqueues, DrawWorldShadow's WaitShadowCullingComplete joins).
	m_ShadowMap.Prepare();

	DrawSky();
	// The other two shadow passes resolve their Gothic-side state here, on the main thread, WITHOUT recording any
	// draws: the point-light shadow cubes (P2.10 — each selected light's 6 faces into the shared cube array) and
	// the rain shadowmap (world-mesh + instanced-VOB depth along the rain-velocity direction, so DrawRainParticles'
	// VS can zero out raindrops under roofs and tree canopies, and the lit passes can stop wetting the ground
	// under them). Both overlap the cascade culls launched above; BeginShadowRecording then
	// fans their command recording out to the pool so it also overlaps the depth prepass / GPU cull / light cull
	// / SSAO the main thread records next.
	PrepareShadowPasses();
	// Scene wetness ("wet ground"): publish this frame's rain camera + wetness/time constants into the tail
	// of the shared shadow CB so the lit World/Vob/Skeletal pixel shaders can darken, ripple and gloss up
	// the surfaces the rain actually reaches. Unconditional (it also has to publish the "not wet" state) and
	// necessarily AFTER PrepareRainShadowmap, which is what computes m_RainShadowViewProj.
	UploadWetnessConstants();
	// Rain/snow particle advance: ping-pongs the GPU particle buffer in place. Independent of the geometry
	// passes below (only touches its own buffer); DrawRainParticles (later, alongside the PFX particles)
	// reads the result.
	AdvanceRain();
	// Forward+ opaque depth prepass — lays down ALL opaque depth before the lit passes so the tiled light cull
	// bounds each tile to real geometry: world mesh, then instanced VOBs, then skeletal (NPCs/monsters) + node
	// attachments. Visually a no-op (the color passes re-pass on GREATER_EQUAL and rewrite the same depth).
	// Build the world-mesh ExecuteIndirect command set ONCE (P2.11) — the shared visible-section collection +
	// per-material bindless-index resolution + water peel-out. Both the depth prepass and the color pass draw
	// from it, so the BSP walk happens once (was per-pass) and neither pass issues per-material CPU draw calls.
	BuildWorldDrawCommands();
	// Build the instanced-VOB ExecuteIndirect command set ONCE (P2.12) from the shared g_FrameVobUploads snapshot,
	// resolving each material's full PBR bindless indices (diffuse+normal+ORM) — the depth prepass ignores the extra
	// two, so both the prepass and the color pass ExecuteIndirect over this same buffer (the per-material CacheIn +
	// vertex/index binds happen once/frame instead of per-pass, and neither pass issues per-mesh CPU draw calls).
	if ( Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs && m_VobDrawArgsPtr[m_FrameIndex] ) {
		m_VobDrawCount = BuildVobDrawCommands( g_FrameVobUploads, m_VobDrawArgsPtr[m_FrameIndex], true,
			kMaxVobDrawCommands, m_GpuVobCullActive );
	} else {
		m_VobDrawCount = 0;
	}
	// Build the skeletal + node-attachment ExecuteIndirect command sets ONCE (T9) from g_FrameSkelDraws/
	// g_FrameAttachDraws, resolving each material's full bindless index set — the depth prepass ignores the
	// extra two, so both skeletal passes submit over these same two buffers. MUST stay ahead of
	// BeginShadowRecording: this is the last Gothic-touching skeletal work (UpdateMeshLibTexAniState mutates
	// the model's SHARED texani slots, zCTexture::CacheIn touches the resource manager) and the cascade
	// recorders below run on pool threads that must never see either mid-flight.
	BuildSkeletalDrawCommands();
	// Everything the shadow passes need is resolved and every indirect-arg buffer for this frame is written, so
	// hand the shadow recording to the pool and carry on: the prepass/cull/SSAO block below is recorded on the
	// main thread WHILE the cascades, the point-light cubes and the rain map record into their own lists.
	BeginShadowRecording();
    {
        DX_ZONE( m_CmdList, "Depth Prepass" );
        TracyD3D12ZoneCGX( m_CmdList.Get(), "Depth Prepass" );
        DrawDepthPrepass();
		// GPU VOB culling (D3D12Cull.cpp) sits between the WORLD-MESH depth prepass and the VOB one, which is the
		// only spot where the depth buffer holds world geometry and nothing else: BuildHiZ min-reduces it into the
		// occlusion pyramid, then CullVobsGPU frustum+occlusion-tests every uploaded instance, compacts the
		// survivors and rewrites the instance counts in the arg buffer DrawVobDepthPrepass/DrawVobsInstanced
		// submit. No-ops (and both passes fall back to the CPU-written arg ring) when m_GpuVobCullActive is false.
		if ( m_GpuVobCullActive ) {
			BuildHiZ();
			CullVobsGPU();
		}
	    DrawVobDepthPrepass();
	    DrawSkeletalDepthPrepass();
    }
	// Forward+ tiled light cull: consume this frame's light buffer + the prepass depth to record which point
	// lights touch each 16x16 screen tile (bounded to real geometry on both the near and far side).
	DispatchLightCulling();
	// Simple screen-space AO (plan item #4): reconstructs normals from the prepass depth (no GBuffer normals
	// pre-lighting in Forward+) and writes the blurred AO mask the lit passes below sample bindlessly via
	// m_ActiveAOMaskSrvSlot. No-op (mask stays white) when AoMode == AO_NONE or resources are unavailable.
	RenderSSAO();
	// Sky image-based lighting: rebuilds the indirect-light cubes (specular chain + irradiance) when Gothic's
	// sky state has moved, and publishes their bindless indices into the shadow CB the lit passes bind. Must
	// run after D3D12ShadowMap::Prepare (it reads its sun direction) and before the lit passes below. No-op on an unchanged
	// sky; the shaders fall back to the old flat ambient whenever the indices are the 0xFFFFFFFF sentinel.
	RenderSkyIBL();
	// Join the shadow recorders and slot their lists into the queue ahead of everything recorded above — the lit
	// passes below are the first thing this frame that samples the cascade map / point-shadow cubes. Also
	// re-establishes the scene-color RT + depth for them.
	FinishShadowPasses();
    {
        DX_ZONE( m_CmdList, "Lit Geometry Pass" );
        TracyD3D12ZoneCGX( m_CmdList.Get(), "Lit Geometry Pass" );
	    DrawWorldMesh();
	    {
		    DX_ZONE( m_CmdList, "Draw skeletal (color)" );
		    TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw skeletal (color)" );
		    DrawSkeletalColor();   // base meshes + node attachments, lit through the tile grid (both lists)
	    }
	    DrawVobsInstanced();
	    DrawVegetation();
    }

	// Decals (blood, arrows, sprites): collect the visible, back-to-front-sorted list once, then draw the
	// opaque/alpha-test ones here (with the opaque scene, depth-write) and the transparent ones after water
	// (blended over the finished scene). Mirrors D3D11's two-pass DrawDecalList.
	static std::vector<zCVob*> decals;
	decals.clear();
	Engine::GAPI->GetVisibleDecalList( decals );
	{
		DX_ZONE( m_CmdList, "Draw decals (opaque)" );
		TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw decals (opaque)" );
		DrawDecalList( decals, true );
	}

	// Quad marks (blood splatter, spell ground marks) — D3D11's "Draw ParticleFX #1" pass calls DrawQuadMarks
	// immediately after the lit DrawDecalList, same slot. MUL/MUL2 marks are deferred to DrawMQuadMarks below.
	DrawQuadMarks();

	// Water: alpha-blended over the finished opaque scene (world + NPCs + VOBs + opaque decals).
	DrawWaterSurfaces();

	// Alpha-blended world-mesh surfaces (ice, glass, magic barriers) peeled out of the opaque command set by
	// BuildWorldDrawCommands: blended back-to-front over the finished scene, then re-laid into depth so the
	// height fog/god rays see them. Same slot D3D11 draws FrameTransparencyMeshes in (right after water,
	// before the transparent decals). See D3D12Transparency.cpp.
	DrawWorldTransparencyMeshes();

	{
		DX_ZONE( m_CmdList, "Draw decals (transparent)" );
		TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw decals (transparent)" );
		DrawDecalList( decals, false );
	}

	// The modulate-blended quad marks DrawQuadMarks deferred — D3D11's "Draw ParticleFX #2" pass draws them
	// right after the unlit decals, for the same reason (MUL/MUL2 must land on the finished scene).
	DrawMQuadMarks();

	// Particles last: billboarded PFX (fire, smoke, magic, dust) blended over everything, depth-tested
	// against the opaque scene but not writing depth. Mirrors D3D11's late DrawParticlesSimple pass.
	{
		DX_ZONE( m_CmdList, "Draw particles" );
		TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw particles" );
		DrawParticleEffects();
	}

	// Rain/snow (D3D12 rain parity, step 2): unlit placeholder billboards, always "wet" — see
	// DrawRainParticles. Same late-transparency slot D3D11 draws rain in.
	{
		DX_ZONE( m_CmdList, "Draw rain" );
		TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw rain" );
		DrawRainParticles();
	}

	// Weapon/spell trails + lightning flashes. D3D11 draws its "Draw PolyStrips" pass right after the
	// particle pass and before the debug lines; on D3D12 the rain billboards sit in between, which is
	// immaterial (both are late alpha content that doesn't write depth).
	DrawPolyStrips();

	// Ghosts (invisible-potion/fade-out items): drawn last of the alpha content, mirrors D3D11's "Draw ghosts"
	// pass placement (after the transparency waterfall/decals, before post-FX). MUST run every frame even if
	// EnableBloom/etc. are off — it also drains GothicAPI::TransparencyVobs, which nothing else consumes.
	{
		DX_ZONE( m_CmdList, "Draw ghosts" );
		TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw ghosts" );
		DrawGhostVobs();
	}

	// Clear the per-visual instance lists so next frame's CollectVisibleVobs starts fresh (mirrors D3D11).
	// Done here (not in DrawVobsInstanced) so it runs even when DrawVOBs is off and that pass early-outs.
	for ( auto const& [visualPtr, visual] : Engine::GAPI->GetStaticMeshVisuals() ) {
		if ( visual ) visual->Instances.clear();
	}

	// Snapshot the now-COMPLETE depth buffer for the next frame's SSAO (see D3D12AO.cpp). Deliberately here:
	// every depth-writing pass (world, VOBs, skeletal, grass, decals, water, particles) has run, and
	// RenderFogAndGodRays below is the first thing that moves the depth buffer out of DEPTH_WRITE.
	CopyDepthForAO();

	// Height fog + god rays (parity item #5): the last thing to touch the scene before the post-FX chain, same
	// slot D3D11's PostFX composition occupies (after the ghosts/particle passes, before bloom+tonemap). Both
	// halves are outdoor-only and individually gated (DrawFog / EnableGodRays); no-ops otherwise.
	RenderFogAndGodRays();

	// Bloom (P2.11, opt-in via EnableBloom): must run before the tonemap resolve below, while the scene is still
	// linear HDR — additively blending a mip pyramid of the scene's own bright pixels back onto itself.
	RenderBloom();
	RenderLuminanceAdapt();

	// Phase 3 HDR: the 3D scene is complete — tonemap the HDR target into the swapchain and rebind the backbuffer
	// so Gothic's subsequent 2D UI/HUD draws (and the ImGui overlay in Present) composite on top in LDR.
	ResolveSceneToBackBuffer();

	// SMAA anti-aliasing (opt-in, RendererSettings.AntiAliasingMode == AA_SMAA): runs on the tonemapped LDR
	// swapchain image, before Gothic's 2D UI/HUD composites on top so the HUD stays crisp. No-ops if disabled
	// or resources unavailable. Mirrors D3D11's SMAA placement (post-tonemap, pre-sharpen/UI).
	RenderSMAA();

	// Post-tonemap sharpening (SHARPEN_CAS by default — this one is ON for a stock config, unlike SMAA).
	// D3D11's "Sharpen" pass sits in the same place: after AA, on the LDR backbuffer, before the 2D UI.
	RenderSharpen();

	// Debug/editor lines last, on the finished LDR image — same slot as D3D11's "Draw Debug Lines" render-graph
	// pass (after post-FX, before Gothic's 2D UI composites on top). Both calls also CLEAR their cache, so this
	// is what keeps the line lists from growing unbounded across frames.
	{
		DX_ZONE( m_CmdList, "Draw debug lines" );
		TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw debug lines" );
		m_LineRenderer->Flush();
		m_LineRenderer->FlushScreenSpace();
	}

	// Do any remaining dx12 stuff BEFORE setting PresentPending

	m_PresentPending = true;

	// After this point, we hand over to Gothics UI rendering (inventory item previews render via
	// DrawVobSingle, called straight from Gothic's own zCWorld::Render hook during this phase).

	return XR_SUCCESS;
}


XRESULT D3D12GraphicsEngine::DrawSky() {
	if ( !m_FrameOpen ) return XR_SUCCESS;
	DX_ZONE( m_CmdList, "Draw sky" );
	TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw sky" );

	// Base fallback fill: Gothic's current sky/fog color (see GetSceneFogColorXM — tracks time of day when
	// AtmosphericScattering is off, matching D3D11's own background-clear formula). Runs first so a gap in
	// the real sky draw below still leaves the horizon dissolving into the geometry's per-pixel distance fog
	// instead of showing the frame's black clear.
	XMFLOAT3 fc;
	XMStoreFloat3( &fc, GetSceneFogColorXM() );

	// The HDR scene target is LINEAR (the lit passes sRGB-decode their albedo + linearize FogColor), so the sky
	// fill must be linear too — otherwise the tonemap (ACES + sRGB-encode) would double-process the sky/fog and it
	// wouldn't match the geometry's distance-fog fade. sRGB->linear each channel on the CPU before clearing.
	auto srgbToLinear = []( float c ) { return c <= 0.04045f ? c / 12.92f : std::pow( (c + 0.055f) / 1.055f, 2.4f ); };
	const float clear[4] = { srgbToLinear( fc.x ), srgbToLinear( fc.y ), srgbToLinear( fc.z ), 1.0f };
	m_CmdList->ClearRenderTargetView( m_SceneColorRtv, clear, 0, nullptr );

	Engine::GAPI->GetSky()->RenderSky(); // does not render, but calculates atmosphere data (AC_LightPos etc.)

	GothicRendererState& rs = Engine::GAPI->GetRendererState();
	// D3D11's real atmospheric-scattering sky dome (PS_Atmosphere/PS_AtmosphereOuter + VS_ExWS, gated on
	// AtmosphericScattering==true, the project default — see D3D11GraphicsEngine::DrawSky) is NOT ported to
	// D3D12 yet. This used to `return` here when AtmosphericScattering was true, leaving ONLY the flat,
	// un-modulated FogColorMod fill above as the entire sky — day or night, since that fill has no time-of-day
	// term at all. That's what produced the "sky is shining light-blue at night" bug: the fixed-function sky
	// below (which Gothic itself renders with real per-time-of-day textures/colors, sun, moon and stars) was
	// being skipped entirely whenever AtmosphericScattering was on, which is the default. Until the real
	// scattering shader is ported, always fall through to the fixed-function sky instead — it isn't identical
	// to D3D11's procedural scattering, but it does correctly vary with time of day/weather, unlike the flat
	// fill, and is a strict improvement over showing a static color regardless of AtmosphericScattering.

	// Fixed-function sky path: hand off to Gothic's own zCSkyController_Outdoor::RenderSkyPre(), same as
	// D3D11GraphicsEngine::DrawSky. Its D3DFVF_XYZRHW_DIF_T1 draws come back through MyDirect3DDevice7 ->
	// DrawPrimitive/DrawPrimitiveVB -> DrawVertexArray/DrawVertexBufferFF (backend-neutral already), which
	// reads RenderStage == STAGE_DRAW_SKY to pick the HDR RTV format, real backface culling, and the
	// FORCE_MAX_Z vertex-shader variant that pins the sky to the reversed-Z far plane.
	const RenderStage oldStage = rs.RendererInfo.RenderStage;
	rs.RendererInfo.RenderStage = STAGE_DRAW_SKY;

	rs.DepthState.DepthBufferEnabled = true;
	rs.DepthState.DepthWriteEnabled = false;   // sky never occludes; only shows where nothing else drew
	rs.DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_GREATER_EQUAL;
	rs.DepthState.SetDirty();

	rs.RasterizerState.SetDefault();
	rs.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
	rs.RasterizerState.SetDirty();

	Engine::GAPI->GetLoadedWorldInfo()->MainWorld->GetSkyControllerOutdoor()->RenderSkyPre();

    // Fix FarPlane after engine breaks it.
    Engine::GAPI->SetFarPlane(
            Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
            WORLD_SECTION_SIZE );

	rs.RendererInfo.RenderStage = oldStage;
	return XR_SUCCESS;
}


bool D3D12GraphicsEngine::CreateWorldIndirect() {
	// Command signature + per-frame UPLOAD arg ring for the GPU-driven world mesh (P2.11). One command sets the
	// b6 material bindless indices (3 root constants @ param 10 of m_Pipelines.World.RootSig) then issues a DrawIndexed. Both
	// the depth prepass and the color pass ExecuteIndirect over the SAME per-frame buffer (identical opaque draw
	// set — water peeled at build time). The arg buffer is UPLOAD (permanently GENERIC_READ, which INCLUDES
	// INDIRECT_ARGUMENT), rebuilt each frame by BuildWorldDrawCommands — no DEFAULT-heap copy needed.
	ID3D12Device* device = m_Device.GetDevice();
	if ( !device || !m_Pipelines.World.RootSig ) return false;

	D3D12_INDIRECT_ARGUMENT_DESC args[2] = {};
	args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
	args[0].Constant.RootParameterIndex = 10;                 // b6 MaterialCB (world root sig)
	args[0].Constant.DestOffsetIn32BitValues = 0;
	args[0].Constant.Num32BitValuesToSet = 4;                 // normal, orm, diffuse, normal-perturb strength
	args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

	D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
	sigDesc.ByteStride = sizeof( WorldDrawCommand );          // 36 B; MUST match the struct + shader layout
	sigDesc.NumArgumentDescs = _countof( args );
	sigDesc.pArgumentDescs = args;
	// A command that sets root constants must carry the root signature its param index refers to.
	if ( FAILED( device->CreateCommandSignature( &sigDesc, m_Pipelines.World.RootSig.Get(),
		IID_PPV_ARGS( m_WorldIndirectCmdSig.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: failed to create the world indirect command signature.";
		return false;
	}

	D3D12MA::ALLOCATION_DESC upload = {};
	upload.HeapType = DefaultUploadHeapType;

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>( kMaxWorldDrawCommands ) * sizeof( WorldDrawCommand );
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_Allocator->CreateResource( &upload, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_WorldDrawArgsAlloc[i].ReleaseAndGetAddressOf(),
            IID_PPV_ARGS( m_WorldDrawArgs[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_WorldDrawArgs[i]->SetName( L"WorldDrawArgsRing" );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( m_WorldDrawArgs[i]->Map( 0, &noRead, &mapped ) ) ) return false;
        m_WorldDrawArgsPtr[i] = static_cast<uint8_t*>( mapped );
        m_WorldDrawArgsGpu[i] = m_WorldDrawArgs[i]->GetGPUVirtualAddress();
    }
    
    
    // Same command signature + same buffer layout serves the CSM cascades, one arg ring per (cascade x frame).
    return m_ShadowMap.CreateWorldArgRings( bd );
}


namespace {
    // Matches D3D11GraphicsEngine::DEFAULT_NOISE_NORMALMAP_STRENGTH — the weak perturb strength used when the
    // rain-distortion texture stands in for a missing normalmap (see BuildWorldDrawCommands' wet-ground fallback).
    constexpr float kWetDistortionNormalStrength = 0.10f;

    // The bindless ORM SRV index (MatOrmIndex, b6) has room to spare — the SRV heap is sized for tens of
    // thousands of descriptors (doctrine: ~64k), far under the 30 bits reserved here. The top 2 bits piggyback
    // the FxMap's channel layout (MyDirectDrawSurface7::AvailableMaterials) so the PS knows which channels of
    // the single bound texture are meaningful, instead of always assuming a full _ORM.DDS (R=AO,G=Rough,B=Metal).
    // Mirrored by SampleOrm() in Shaders/D3D12/include/PBRLighting.hlsl — keep the two in sync.
    constexpr uint32_t kOrmFormatShift = 30;
    constexpr uint32_t kOrmIndexMask   = 0x3FFFFFFFu;   // 1 billion+ slots — nowhere near the real heap size

    // Packs an SRV heap slot with the FxMap's channel layout for the D3D12 PS to unpack (see kOrmFormatShift).
    // EAdditionalMaterial::None/Specular (no _FX loaded, or the legacy D3D11-only _FX.dds) fall through to format
    // 0 (ORM) — correct because m_DefaultOrmTexture is itself laid out as full AO/Rough/Metal in that case.
    uint32_t EncodeOrmSlot( uint32_t srvSlot, EAdditionalMaterial availableMat ) {
        uint32_t fmt = 0;   // ORM: r=AO g=Roughness b=Metallic (also the default-texture / no-_FX case)
        switch ( availableMat ) {
            case EAdditionalMaterial::AoRoughness: fmt = 1; break;   // _OR.DDS:    r=AO g=Roughness, metal defaults to 0
            case EAdditionalMaterial::Roughness:   fmt = 2; break;   // _R.DDS:     r=Roughness only, AO defaults to 1, metal to 0
            default: break;
        }
        return ( fmt << kOrmFormatShift ) | ( srvSlot & kOrmIndexMask );
    }
}

void D3D12GraphicsEngine::BuildWorldDrawCommands() {
    // Build this frame's world-mesh ExecuteIndirect command set ONCE (P2.11): frustum-collect the visible sections,
    // then per non-water material append { bindless material indices, DrawIndexedArguments (its index range into
    // the shared world VB/IB) }. Water is peeled here into g_FrameWaterSurfaces (drawn later, alpha-blended). Both
    // world passes then consume the result, so the BSP walk + per-material CacheIn happen once/frame (was 2-3x).
    m_WorldDrawCount = 0;
    m_WorldDrawnIndices = 0;
    g_FrameWaterSurfaces.clear();
    g_FrameWorldTransparency.clear();
    g_FrameWorldTransparencyPortal.clear();
    g_FrameWorldTransparencyFoam.clear();
    if ( !m_FrameOpen || !m_WorldIndirectCmdSig || !m_WorldDrawArgsPtr[m_FrameIndex] ) return;

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    if ( !wm || !wm->GetMeshVertexBuffer() || !wm->GetMeshIndexBuffer() ) return;

    // Camera setup identical to the passes so CollectVisibleSections culls against the same frustum.
    Engine::GAPI->SetViewTransformXM( Engine::GAPI->GetViewMatrixXM() );
    Engine::GAPI->ResetWorldTransform();

    static std::vector<WorldMeshSectionInfo*> sections;
    sections.clear();
    Engine::GAPI->CollectVisibleSections( sections, nullptr, true );

    WorldDrawCommand* cmds = reinterpret_cast<WorldDrawCommand*>( m_WorldDrawArgsPtr[m_FrameIndex] );
    UINT count = 0;

    // Camera position for the alpha-blended peel's painter-order sort key (see the branch below).
    const XMVECTOR transparencyCamPos = Engine::GAPI->GetCameraPositionXM();
    
    Frustum playerFrustum = Frustum::AlwaysContainingFrustum();
    if ( auto cam = (zCCamera*)oCGame::GetGame()->_zCSession_camera ) {
        const auto& view = cam->trafoView; // Column-Major, needs Transpose for DxMath
        const auto& proj = cam->trafoProjection; // Row-Major, does not need transpose.
        playerFrustum.BuildPerspective(
            XMMatrixTranspose( XMLoadFloat4x4( &view ) ),
            XMLoadFloat4x4( &proj )
        );
    }

    for ( WorldMeshSectionInfo* section : sections ) {
        if ( !section ) continue;
        for ( auto const& [meshKey, mesh] : section->WorldMeshes ) {
            if ( !mesh || mesh->Indices.empty() ) continue;
            
            if ( !Engine::GAPI->IsWorldMeshVisibleInFrustum( mesh, playerFrustum ) ) {
                continue;
            }            

            // Sort key for every transparency bucket below: distance to the mesh's own bbox center, falling
            // back to the section's (ComputeWorldMeshDistanceSqFromCamera in D3D11GraphicsEngine.cpp).
            auto transparencyDistanceSq = [&]() -> float {
                const zTBBox3D* bounds = mesh->HasBoundingBox ? &mesh->BoundingBox : &section->BoundingBox;
                const XMFLOAT3 center( ( bounds->Min.x + bounds->Max.x ) * 0.5f,
                                       ( bounds->Min.y + bounds->Max.y ) * 0.5f,
                                       ( bounds->Min.z + bounds->Max.z ) * 0.5f );
                float distanceSq = 0.0f;
                XMStoreFloat( &distanceSq, XMVector3LengthSq( XMLoadFloat3( &center ) - transparencyCamPos ) );
                return distanceSq;
            };

            // Water is transparent — bucket it by texture for the later alpha-blended pass, skip the opaque
            // command set. Forest portals and waterfall foam get their own sorted lists for the same reason,
            // each drawn with its own pixel shader (D3D11: FrameTransparencyMeshesPortal / ...Waterfall).
            if ( meshKey.Info) {
                if ( meshKey.Info->MaterialType == MaterialInfo::MT_Water ) {
                    g_FrameWaterSurfaces[meshKey.Material->GetAniTexture()].push_back( mesh );
                    continue;
                } else if ( meshKey.Info->MaterialType == MaterialInfo::MT_Portal ) {
                    g_FrameWorldTransparencyPortal.push_back( { meshKey.Material, mesh, transparencyDistanceSq() } );
                    continue;
                } else if ( meshKey.Info->MaterialType == MaterialInfo::MT_WaterfallFoam ) {
                    g_FrameWorldTransparencyFoam.push_back( { meshKey.Material, mesh, transparencyDistanceSq() } );
                    continue;
                }
            }

            // Alpha-blended materials (ice, glass, magic barriers) are peeled out of the opaque set the same
            // way water is — they need the material's own blend mode, back-to-front order and no depth write,
            // which one ExecuteIndirect over the opaque PSO cannot express. Mirrors D3D11's DrawWorldMesh
            // "Check for alphablending" branch feeding FrameTransparencyMeshes; drawn by
            // DrawWorldTransparencyMeshes (D3D12Transparency.cpp). Peeled from the DEPTH PREPASS too (both
            // passes share this command set) — same as D3D11, whose prepass `isSkipped` filter drops them.
            if ( IsWorldMeshAlphaBlended( meshKey.Material ) ) {
                g_FrameWorldTransparency.push_back( { meshKey.Material, mesh, transparencyDistanceSq() } );
                continue;
            }
            if ( count >= kMaxWorldDrawCommands ) {
                if ( !m_WorldDrawArgsOverflowLogged ) {
                    LogWarn() << "D3D12: world draw-command ring overflow (" << kMaxWorldDrawCommands
                        << " draws/frame); some world materials dropped this frame.";
                    m_WorldDrawArgsOverflowLogged = true;
                }
                break;
            }

            // Resolve this material's bindless SRV heap indices — diffuse (CacheIn triggers load + its normal/ORM
            // side-loads), normal (0xFFFFFFFF = none → PS skips perturb), ORM (1x1 default when the material has no _FX).
            zCTexture* tex = meshKey.Material->GetAniTexture();
            uint32_t diffuseIdx = m_BlackTexture->GetSrvSlot();
            uint32_t normalIdx  = 0xFFFFFFFFu;
            uint32_t ormIdx     = m_DefaultOrmTexture->GetSrvSlot();
            float normalStrength = 1.0f;
            if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                if ( MyDirectDrawSurface7* s = tex->GetSurface() ) {
                    if ( GfxTexture* gfx = s->GetEngineTexture() ) {
                        D3D12Texture* d = D3D12Texture::From( gfx );
                        if ( d->HasSRV() ) diffuseIdx = d->GetSrvSlot();
                    }
                    if ( GfxTexture* n = s->GetNormalmap() ) {
                        D3D12Texture* d = D3D12Texture::From( n );
                        if ( d->HasSRV() ) normalIdx = d->GetSrvSlot();
                    }
                    if ( GfxTexture* o = s->GetFxMap() ) {
                        D3D12Texture* d = D3D12Texture::From( o );
                        if ( d->HasSRV() ) ormIdx = EncodeOrmSlot( d->GetSrvSlot(), s->GetAvailableMaterials() );
                    }
                }
            }
            // Wet-ground fallback (mirrors D3D11GraphicsEngine::BindTextureNRFX): a material with no normalmap
            // still gets a wet/specular look while it's raining, by perturbing with the same distortion noise
            // texture the D3D11 backend uses, at a much weaker strength than a real normalmap.
            if ( normalIdx == 0xFFFFFFFFu && m_DistortionTexture && m_DistortionTexture->HasSRV()
                && Engine::GAPI->GetSceneWetness() > 1e-6f ) {
                normalIdx = m_DistortionTexture->GetSrvSlot();
                normalStrength = kWetDistortionNormalStrength;
            }

            WorldDrawCommand& c = cmds[count];
            c.MatNormalIndex     = normalIdx;
            c.MatOrmIndex        = ormIdx;
            c.MatDiffuseIndex    = diffuseIdx;
            c.MatNormalStrength  = normalStrength;
            c.Draw.IndexCountPerInstance = static_cast<UINT>( mesh->Indices.size() );
            c.Draw.InstanceCount = 1;
            c.Draw.StartIndexLocation = mesh->BaseIndexLocation;
            c.Draw.BaseVertexLocation = 0;
            c.Draw.StartInstanceLocation = 0;
            ++count;
            m_WorldDrawnIndices += static_cast<unsigned int>( mesh->Indices.size() );
        }
    }
    m_WorldDrawCount = count;
    // Painter's order for the peeled alpha-blended surfaces (far -> near), once per frame.
    SortWorldTransparencyMeshes();
}


bool D3D12GraphicsEngine::CreateVobIndirect() {
    // Command signature + per-frame UPLOAD arg rings for the GPU-driven instanced VOBs (P2.12). Unlike the world
    // mesh's signature (material consts + DrawIndexed over one shared VB/IB), each VOB command ALSO sets its own
    // two vertex-buffer views (packed mesh @slot0, per-instance @slot1) + index-buffer view — VOB visuals don't
    // share a buffer. Arg order MUST match VobDrawCommand's member layout. The rings stay UPLOAD/GENERIC_READ
    // (which includes INDIRECT_ARGUMENT), rebuilt each frame by BuildVobDrawCommands — no DEFAULT-heap copy.
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Pipelines.World.RootSig ) return false;

    // The GPU reads each command as tightly-packed native argument structs in pArgumentDescs order; the two VBVs +
    // IBV lead so their UINT64 GPUVAs stay 8-aligned. 96 is a multiple of 8 → the next command's VBV is aligned too.
    // The final 8 bytes (VisualIndex + pad) sit PAST the arguments: ByteStride only has to cover them, so those
    // bytes are ours (VobCull.hlsl's CSPatchArgs reads VisualIndex out of the same buffer it patches).
    static_assert( sizeof( VobDrawCommand ) == 96, "VobDrawCommand must match the command signature arg layout (96 B stride)" );
    static_assert( offsetof( VobDrawCommand, MatNormalIndex ) == 48, "VobDrawCommand b6 consts must follow the 3 buffer views" );
    static_assert( offsetof( VobDrawCommand, Draw ) == 68, "VobDrawCommand draw args must be last of the indirect arguments" );
    static_assert( offsetof( VobDrawCommand, VisualIndex ) == 88, "VisualIndex must follow the indirect arguments" );

    D3D12_INDIRECT_ARGUMENT_DESC args[6] = {};
    args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    args[0].VertexBuffer.Slot = 0;                            // packed ExVertexStruct
    args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    args[1].VertexBuffer.Slot = 1;                            // per-instance VobInstanceInfo
    args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    args[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    args[3].Constant.RootParameterIndex = 10;                 // b6 MaterialCB { normal, orm, diffuse }
    args[3].Constant.DestOffsetIn32BitValues = 0;
    args[3].Constant.Num32BitValuesToSet = 3;
    args[4].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    args[4].Constant.RootParameterIndex = 11;                 // b4 WindCB: overwrite only minHeight/maxHeight (@4,5)
    args[4].Constant.DestOffsetIn32BitValues = 4;
    args[4].Constant.Num32BitValuesToSet = 2;
    args[5].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
    sigDesc.ByteStride = sizeof( VobDrawCommand );            // 96 B; MUST match the struct + arg layout
    sigDesc.NumArgumentDescs = _countof( args );
    sigDesc.pArgumentDescs = args;
    // Commands set root constants (b6/b4), so the signature must carry the root signature those param indices refer to.
    if ( FAILED( device->CreateCommandSignature( &sigDesc, m_Pipelines.World.RootSig.Get(),
        IID_PPV_ARGS( m_VobIndirectCmdSig.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: failed to create the VOB indirect command signature.";
        return false;
    }

    D3D12MA::ALLOCATION_DESC upload = {};
    upload.HeapType = DefaultUploadHeapType;

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    auto makeRing = [&]( UINT maxCommands, Microsoft::WRL::ComPtr<ID3D12Resource>& res, Microsoft::WRL::ComPtr<D3D12MA::Allocation>& alloc, uint8_t*& ptr, const wchar_t* name ) -> bool {
        bd.Width = static_cast<UINT64>( maxCommands ) * sizeof( VobDrawCommand );
        if ( FAILED( m_Allocator->CreateResource( &upload, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            alloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( res.ReleaseAndGetAddressOf() ) ) ) )
            return false;
        res->SetName( name );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( res->Map( 0, &noRead, &mapped ) ) ) return false;
        ptr = static_cast<uint8_t*>( mapped );
        return true;
        };

    for ( UINT i = 0; i < kBackBufferCount; ++i )
        if ( !makeRing( kMaxVobDrawCommands, m_VobDrawArgs[i], m_VobDrawArgsAlloc[i], m_VobDrawArgsPtr[i], L"VobDrawArgsRing" ) )
            return false;
    // The CSM cascades and the rain shadowmap submit through the same signature; their (smaller-capped) rings
    // are owned by the shadow module / the rain pass respectively.
    if ( !m_ShadowMap.CreateVobArgRings( sizeof( VobDrawCommand ) ) ) return false;
    return CreateRainVobArgRings( sizeof( VobDrawCommand ) );
}


UINT D3D12GraphicsEngine::BuildVobDrawCommands( const std::vector<FrameVobUpload>& uploads, uint8_t* argPtr, bool resolveMaps,
    UINT maxCommands, bool culled ) {
    // Fill an arg buffer with one command per (visual x material x sub-mesh): resolve the material's bindless
    // indices (diffuse always; normal/ORM only when resolveMaps — the depth/shadow passes just alpha-clip on
    // diffuse), pack the mesh + instance VB views + IB view, the per-visual wind min/max, and DrawIndexedInstanced.
    // Same CacheIn/From resolution the per-draw path did — but done ONCE per built buffer instead of per pass.
    if ( !argPtr ) return 0;
    VobDrawCommand* cmds = reinterpret_cast<VobDrawCommand*>( argPtr );
    UINT count = 0;
    const uint32_t whiteSlot   = m_BlackTexture->GetSrvSlot();
    const uint32_t defaultOrm  = m_DefaultOrmTexture->GetSrvSlot();
    // Attribute the triangle stat to the main-view build only (resolveMaps): the shadow cascades build the same
    // geometry and would double-count. Reset here; the color pass adds it to FrameDrawnTriangles once.
    // NOTE: with GPU culling (culled=true) this is a pre-cull UPPER BOUND — the real instance counts only exist
    // on the GPU after CSPatchArgs, and reading them back would cost a stall for a statistic.
    if ( resolveMaps ) m_VobDrawnTriangles = 0;

    for ( const FrameVobUpload& up : uploads ) {
        MeshVisualInfo* visual = up.visual;
        if ( !visual ) continue;
        const float minH = visual->BBox.Min.y;
        const float maxH = visual->BBox.Max.y;

        for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
            zCTexture* tex = meshKey.Material->GetAniTexture();
            uint32_t diffuseIdx = whiteSlot;
            uint32_t normalIdx  = 0xFFFFFFFFu;
            uint32_t ormIdx     = defaultOrm;
            if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                if ( MyDirectDrawSurface7* s = tex->GetSurface() ) {
                    if ( GfxTexture* gfx = s->GetEngineTexture() ) {
                        D3D12Texture* d = D3D12Texture::From( gfx );
                        if ( d->HasSRV() ) diffuseIdx = d->GetSrvSlot();
                    }
                    if ( resolveMaps ) {
                        if ( GfxTexture* n = s->GetNormalmap() ) { D3D12Texture* d = D3D12Texture::From( n ); if ( d->HasSRV() ) normalIdx = d->GetSrvSlot(); }
                        if ( GfxTexture* o = s->GetFxMap() )     { D3D12Texture* d = D3D12Texture::From( o ); if ( d->HasSRV() ) ormIdx    = EncodeOrmSlot( d->GetSrvSlot(), s->GetAvailableMaterials() ); }
                    }
                }
            }

            for ( MeshInfo* mi : meshList ) {
                if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() )
                    continue;
                D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
                D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
                if ( !mvb->GetResource() || !mib->GetResource() ) continue;

                if ( count >= maxCommands ) {
                    if ( !m_VobDrawArgsOverflowLogged ) {
                        LogWarn() << "D3D12: VOB draw-command ring overflow (" << maxCommands
                            << " draws/frame); some VOBs dropped this frame.";
                        m_VobDrawArgsOverflowLogged = true;
                    }
                    return count;
                }

                VobDrawCommand& c = cmds[count++];
                c.MeshVBV = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
                // GPU-culled: draw from the compacted buffer CSCull writes (same byte range, different base)
                // and let CSPatchArgs replace InstanceCount below with the surviving count for this visual.
                c.InstVBV = culled ? up.culledInstView : up.instView;
                c.VisualIndex = culled ? up.cullVisualIndex : 0xFFFFFFFFu;
                c._cmdPad = 0;
                c.IBV     = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                c.MatNormalIndex  = normalIdx;
                c.MatOrmIndex     = ormIdx;
                c.MatDiffuseIndex = diffuseIdx;
                c.WindMinHeight   = minH;
                c.WindMaxHeight   = maxH;
                c.Draw.IndexCountPerInstance = static_cast<UINT>( mi->Indices.size() );
                c.Draw.InstanceCount         = up.numInstances;
                c.Draw.StartIndexLocation    = 0;
                c.Draw.BaseVertexLocation    = 0;
                c.Draw.StartInstanceLocation = 0;
                if ( resolveMaps )
                    m_VobDrawnTriangles += (static_cast<unsigned int>( mi->Indices.size() ) / 3) * up.numInstances;
            }
        }
    }
    return count;
}


bool D3D12GraphicsEngine::CreateSkeletalIndirect() {
    // T9: command signature + per-frame UPLOAD arg rings for the GPU-driven skeletal base meshes, plus the
    // (signature-less) arg rings for the node attachments, which submit through the existing VOB signature.
    // Must run AFTER CreateVobIndirect — the attachment rings are sized on VobDrawCommand and the attachment
    // passes reuse m_VobIndirectCmdSig itself.
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device || !m_Pipelines.Skeletal.RootSig || !m_Pipelines.World.RootSig ) return false;

    // The GPU reads each command as tightly-packed native argument structs in pArgumentDescs order. The two root
    // CBVs are bare 8-byte GPU VAs and lead, so everything after them (both buffer views) stays 8-aligned; 80 is a
    // multiple of 8 so the next command's InstCB is aligned too. Unlike VobDrawCommand there is no trailing
    // payload here — nothing GPU-side patches skeletal commands (no GPU cull for NPCs yet).
    static_assert( sizeof( SkeletalDrawCommand ) == 80, "SkeletalDrawCommand must match the command signature arg layout (80 B stride)" );
    static_assert( offsetof( SkeletalDrawCommand, MeshVBV ) == 16, "SkeletalDrawCommand VBV must follow the two root CBVs" );
    static_assert( offsetof( SkeletalDrawCommand, MatNormalIndex ) == 48, "SkeletalDrawCommand b6 consts must follow the buffer views" );
    static_assert( offsetof( SkeletalDrawCommand, Draw ) == 60, "SkeletalDrawCommand draw args must be last" );

    D3D12_INDIRECT_ARGUMENT_DESC args[6] = {};
    args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    args[0].ConstantBufferView.RootParameterIndex = 1;         // b1 per-instance CB (root CBV, VS)
    args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    args[1].ConstantBufferView.RootParameterIndex = 2;         // b2 bone palette CB (root CBV, VS)
    args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    args[2].VertexBuffer.Slot = 0;                             // ExSkelVertexStruct (skinned, no instance stream)
    args[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    args[4].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    args[4].Constant.RootParameterIndex = 11;                  // b6 MaterialCB { normal, orm, diffuse }
    args[4].Constant.DestOffsetIn32BitValues = 0;
    args[4].Constant.Num32BitValuesToSet = 3;
    args[5].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
    sigDesc.ByteStride = sizeof( SkeletalDrawCommand );        // 80 B; MUST match the struct + arg layout
    sigDesc.NumArgumentDescs = _countof( args );
    sigDesc.pArgumentDescs = args;
    // Commands set root descriptors (b1/b2) and root constants (b6), so the signature must carry the root
    // signature those parameter indices refer to.
    if ( FAILED( device->CreateCommandSignature( &sigDesc, m_Pipelines.Skeletal.RootSig.Get(),
        IID_PPV_ARGS( m_SkeletalIndirectCmdSig.ReleaseAndGetAddressOf() ) ) ) ) {
        LogWarn() << "D3D12: failed to create the skeletal indirect command signature.";
        return false;
    }

    D3D12MA::ALLOCATION_DESC upload = {};
    upload.HeapType = DefaultUploadHeapType;

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    auto makeRing = [&]( UINT64 bytes, Microsoft::WRL::ComPtr<ID3D12Resource>& res, Microsoft::WRL::ComPtr<D3D12MA::Allocation>& alloc, uint8_t*& ptr, const wchar_t* name ) -> bool {
        bd.Width = bytes;
        if ( FAILED( m_Allocator->CreateResource( &upload, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            alloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( res.ReleaseAndGetAddressOf() ) ) ) )
            return false;
        res->SetName( name );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( res->Map( 0, &noRead, &mapped ) ) ) return false;
        ptr = static_cast<uint8_t*>( mapped );
        return true;
        };

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( !makeRing( static_cast<UINT64>( kMaxSkeletalDrawCommands ) * sizeof( SkeletalDrawCommand ),
            m_SkeletalDrawArgs[i], m_SkeletalDrawArgsAlloc[i], m_SkeletalDrawArgsPtr[i], L"SkeletalDrawArgsRing" ) )
            return false;
        if ( !makeRing( static_cast<UINT64>( kMaxAttachDrawCommands ) * sizeof( VobDrawCommand ),
            m_AttachDrawArgs[i], m_AttachDrawArgsAlloc[i], m_AttachDrawArgsPtr[i], L"AttachDrawArgsRing" ) )
            return false;
    }
    return true;
}


void D3D12GraphicsEngine::BuildSkeletalDrawCommands() {
    // T9: fold the whole per-mesh CPU draw path of BOTH skeletal passes into one build. Per base-mesh record:
    // run the instance's texani update ONCE (it mutates the model's SHARED texture slots, so it has to happen
    // while we read that instance's materials — see [[skeletal-texani-shared-slots]]), resolve the material's
    // bindless normal/ORM/diffuse indices, and emit one command per sub-mesh carrying the two root CBVs, the
    // skinned VB, the IB and DrawIndexed. Node attachments do the same into a VobDrawCommand buffer.
    //
    // Runs on the main thread from OnStartWorldRendering BEFORE BeginShadowRecording — every Gothic-touching
    // call here (UpdateMeshLibTexAniState, zCTexture::CacheIn) must be finished before the cascade recorders
    // start reading Gothic state on pool threads. That is also why the passes themselves are now pure GPU
    // submits: they used to do this work after the fan-out.
    m_SkeletalDrawCount = 0;
    m_AttachDrawCount = 0;
    m_SkeletalDrawnTriangles = 0;
    if ( !m_FrameOpen ) return;
    const UINT frame = m_FrameIndex;

    auto logOverflow = [this]( const char* what, UINT cap ) {
        if ( !m_SkeletalDrawArgsOverflowLogged ) {
            LogWarn() << "D3D12: skeletal " << what << " draw-command ring overflow (" << cap
                << " draws/frame); some skeletal geometry dropped this frame.";
            m_SkeletalDrawArgsOverflowLogged = true;
        }
        };

    // --- Base skinned meshes ---------------------------------------------------------------------------
    if ( !g_FrameSkelDraws.empty() && m_SkeletalDrawArgsPtr[frame] ) {
        SkeletalDrawCommand* cmds = reinterpret_cast<SkeletalDrawCommand*>( m_SkeletalDrawArgsPtr[frame] );
        UINT count = 0;
        for ( const FrameSkelDraw& d : g_FrameSkelDraws ) {
            if ( !d.visual || !d.vobInfo || !d.vobInfo->Vob ) continue;
            zCModel* model = static_cast<zCModel*>( d.vobInfo->Vob->GetVisual() );
            if ( !model ) continue;
            // before reading the materials we NEED to TexAni, the models share the same textures, causing
            // incorrect textures if not done correctly.
            model->UpdateMeshLibTexAniState();

            for ( auto const& [mat, meshList] : d.visual->SkeletalMeshes ) {
                zCTexture* tex = mat ? mat->GetAniTexture() : nullptr;
                // b6 { normal, ORM, DIFFUSE } — fully bindless, no descriptor table on this root sig. The
                // depth-prepass PS reads only the diffuse index (alpha clip) and ignores the other two, so one
                // resolved material set serves both passes.
                UINT mats[3];
                ResolveMaterialMapSlots( tex, mats );
                mats[2] = ResolveDiffuseSlotCacheIn( tex );

                for ( auto const& mesh : meshList ) {
                    if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer )
                        continue;
                    D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
                    D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
                    if ( !mvb->GetResource() || !mib->GetResource() ) continue;
                    if ( count >= kMaxSkeletalDrawCommands ) { logOverflow( "base-mesh", kMaxSkeletalDrawCommands ); break; }

                    SkeletalDrawCommand& c = cmds[count++];
                    c.InstCB  = d.instCb;
                    c.BoneCB  = d.boneCb;
                    c.MeshVBV = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExSkelVertexStruct ) };
                    c.IBV     = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                    c.MatNormalIndex  = mats[0];
                    c.MatOrmIndex     = mats[1];
                    c.MatDiffuseIndex = mats[2];
                    c.Draw.IndexCountPerInstance = static_cast<UINT>( mesh->Indices.size() );
                    c.Draw.InstanceCount         = 1;
                    c.Draw.StartIndexLocation    = 0;
                    c.Draw.BaseVertexLocation    = 0;
                    c.Draw.StartInstanceLocation = 0;
                    m_SkeletalDrawnTriangles += static_cast<unsigned int>( mesh->Indices.size() ) / 3;
                }
                if ( count >= kMaxSkeletalDrawCommands ) break;
            }
            if ( count >= kMaxSkeletalDrawCommands ) break;
        }
        m_SkeletalDrawCount = count;
    }

    // --- Node attachments (weapons/heads/held items) ----------------------------------------------------
    // Same VobDrawCommand/m_VobIndirectCmdSig the instanced VOBs use — an attachment IS a one-instance VOB
    // draw, down to the packed ExVertexStruct + VobInstanceInfo stream. WindMinHeight/MaxHeight go out as 0:
    // VSMainAttach/VSDepthAttach never read b4 (the instance stream's wind fields carry Fatness/Scaling here).
    if ( !g_FrameAttachDraws.empty() && m_AttachDrawArgsPtr[frame] ) {
        VobDrawCommand* cmds = reinterpret_cast<VobDrawCommand*>( m_AttachDrawArgsPtr[frame] );
        UINT count = 0;
        for ( const FrameAttachDraw& a : g_FrameAttachDraws ) {
            if ( !a.mesh || a.mesh->Indices.empty() || !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() )
                continue;
            D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
            D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
            if ( !mvb->GetResource() || !mib->GetResource() ) continue;
            if ( count >= kMaxAttachDrawCommands ) { logOverflow( "attachment", kMaxAttachDrawCommands ); break; }

            UINT mats[3];
            ResolveMaterialMapSlots( a.tex, mats );
            mats[2] = ResolveDiffuseSlotCacheIn( a.tex );

            VobDrawCommand& c = cmds[count++];
            c.MeshVBV = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
            c.InstVBV = a.instView;
            c.IBV     = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
            c.MatNormalIndex  = mats[0];
            c.MatOrmIndex     = mats[1];
            c.MatDiffuseIndex = mats[2];
            c.WindMinHeight   = 0.0f;
            c.WindMaxHeight   = 0.0f;
            c.Draw.IndexCountPerInstance = static_cast<UINT>( a.mesh->Indices.size() );
            c.Draw.InstanceCount         = 1;
            c.Draw.StartIndexLocation    = 0;
            c.Draw.BaseVertexLocation    = 0;
            c.Draw.StartInstanceLocation = 0;
            c.VisualIndex = 0xFFFFFFFFu;   // never GPU-culled: "leave the CPU's instance count alone"
            c._cmdPad     = 0;
            m_SkeletalDrawnTriangles += static_cast<unsigned int>( a.mesh->Indices.size() ) / 3;
        }
        m_AttachDrawCount = count;
    }
}


void D3D12GraphicsEngine::DrawDepthPrepass() {
    // Forward+ opaque depth prepass (P2.9b-1). Lays down the opaque WORLD-MESH depth before the lit color
    // passes, so the tiled light-culling compute (P2.9b-2) can read a populated depth buffer to tighten each
    // tile's frustum. Writes depth only (color write mask 0). Runs after DrawSky (color cleared, depth still
    // at the OnBeginFrame clear of 0.0) and before DrawWorldMesh. The main opaque passes keep GREATER_EQUAL +
    // depth-write, so they re-pass on equal depth and rewrite the same value — the frame is visually identical.
    //
    // Scope: WORLD MESH ONLY this increment. Instanced VOBs + skeletal NPCs still get their depth from their
    // own (unchanged) color passes; they'll be added to the prepass alongside the cull consumer (P2.9b-2),
    // where the VOB instance-ring offset sharing gets designed together with the tile grid. Water is skipped
    // (transparent — it never writes depth, same as the opaque pass peels it out).
    if ( !m_FrameOpen || !m_Pipelines.World.DepthPrepassPSO || !m_Pipelines.World.RootSig || !m_DepthBuffer )
        return;

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    if ( !wm || !wm->GetMeshVertexBuffer() || !wm->GetMeshIndexBuffer() )
        return;

    D3D12VertexBuffer* vb = D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() );
    D3D12VertexBuffer* ib = D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() );
    if ( !vb->GetResource() || !ib->GetResource() ) return;
    if ( ib->GetSizeInBytes() / sizeof( uint32_t ) == 0 ) return;

    DX_ZONE( m_CmdList, "Depth Prepass (world)" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "Depth Prepass (world)" );

    // ViewProj — identical derivation to DrawWorldMesh so the prepass depth matches the opaque pass exactly.
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetPipelineState( m_Pipelines.World.DepthPrepassPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );   // b0 ViewProj (fog/lights not referenced)

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
    D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    m_CmdList->IASetIndexBuffer( &ibv );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // GPU-driven submit (P2.11): one ExecuteIndirect over the shared per-frame command buffer that
    // BuildWorldDrawCommands filled (same opaque draw set as the color pass; water already peeled). Each command
    // sets the b6 diffuse index (PSClip alpha-clips bindless) then draws its material's index range. Replaces the
    // per-material descriptor-table binds + DrawIndexedInstanced calls (the CPU cost this optimization targets).
    if ( m_WorldDrawCount == 0 ) return;
    m_CmdList->ExecuteIndirect( m_WorldIndirectCmdSig.Get(), m_WorldDrawCount,
        m_WorldDrawArgs[m_FrameIndex].Get(), 0, nullptr, 0 );
}


void D3D12GraphicsEngine::DispatchLightCulling() {
    // Forward+ tiled light cull (P2.9b-2). Runs after the depth prepass, before the lit passes: one 16x16
    // thread group per screen tile writes {Offset,Count} into m_LightGridBuffer and the touching light
    // indices into m_LightIndexBuffer. This increment only PRODUCES the grid — nothing consumes it yet, so
    // the frame is visually unchanged; verify by inspecting the two buffers in RenderDoc (per-tile Count
    // should be non-zero where torches/spells are on screen, zero for empty sky). The lit pixel shaders read
    // this grid instead of looping all lights. The cull reads the prepass depth (transitioned to a compute SRV
    // and back below) to clamp each tile's FAR-Z to real geometry — see the shader header for why far-only.
    if ( !m_FrameOpen || !m_Pipelines.LightCull.PSO || !m_Pipelines.LightCull.RootSig || !m_LightGridBuffer || !m_LightIndexBuffer )
        return;
    if ( !m_LightBuffer[m_FrameIndex] || m_NumTilesX == 0 || m_NumTilesY == 0 )
        return;

    DX_ZONE( m_CmdList, "Light Culling (compute)" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "Light Culling (compute)" );

    // The lit geometry passes left the grid/index buffers in PIXEL_SHADER_RESOURCE last frame; transition them
    // back to UNORDERED_ACCESS so the cull CS can write them as root UAVs. Skipped on the first dispatch after
    // (re)creation, when they're already in UAV (see CreateLightCullBuffers / m_LightGridInPixelState).
    D3D12_RESOURCE_BARRIER batchPre[3] = {};
    UINT preCount = 0;

    if ( m_LightGridInPixelState ) {
        batchPre[preCount++] = TransitionBarrier( m_LightGridBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        batchPre[preCount++] = TransitionBarrier( m_LightIndexBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        m_LightGridInPixelState = false;
    }

    batchPre[preCount++] = TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
    m_CmdList->ResourceBarrier( preCount, batchPre );

    // ProjScale = the projection's x/y view->clip scale (diagonal terms; layout-invariant so no transpose
    // worry). ProjA/ProjB = the z-row terms (_33, _43) the CS inverts to turn a reversed-Z depth sample into
    // a view-space Z (viewZ = ProjB / (depth - ProjA)); same scalars read straight off the CPU matrix, so no
    // transpose concern either. Same projection the geometry passes use.
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();

    struct LightCullConstants {
        float    ProjScaleX, ProjScaleY;
        uint32_t ScreenX, ScreenY;
        uint32_t TotalLights;
        uint32_t NumTilesX;
        float    ProjA, ProjB;   // projM._33, projM._43 (reversed-Z depth -> viewZ reconstruction)
    } cb{};
    cb.ProjScaleX = projM._11;
    cb.ProjScaleY = projM._22;
    cb.ScreenX = static_cast<uint32_t>( m_Resolution.x );
    cb.ScreenY = static_cast<uint32_t>( m_Resolution.y );
    cb.TotalLights = m_FrameLightCount;
    cb.NumTilesX = m_NumTilesX;
    cb.ProjA = projM._33;
    cb.ProjB = projM._43;

    // The depth prepass left the depth buffer in DEPTH_WRITE; make it readable by this compute pass, then hand
    // it back to DEPTH_WRITE below so the lit passes (GREATER_EQUAL depth-write) see it exactly as before.
    D3D12_RESOURCE_BARRIER depthToSrv = {};
    depthToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    depthToSrv.Transition.pResource = m_DepthBuffer.Get();
    depthToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    depthToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    depthToSrv.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    m_CmdList->SetPipelineState( m_Pipelines.LightCull.PSO.Get() );
    m_CmdList->SetComputeRootSignature( m_Pipelines.LightCull.RootSig.Get() );
    m_CmdList->SetComputeRoot32BitConstants( 0, 8, &cb, 0 );
    m_CmdList->SetComputeRootShaderResourceView( 1, m_LightBuffer[m_FrameIndex]->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootUnorderedAccessView( 2, m_LightGridBuffer->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootUnorderedAccessView( 3, m_LightIndexBuffer->GetGPUVirtualAddress() );
    m_CmdList->SetComputeRootDescriptorTable( 4, GetSrvGpuHandle( m_DepthSrvSlot ) );   // t1 DepthTex (SRV heap already bound)
    m_CmdList->Dispatch( m_NumTilesX, m_NumTilesY, 1 );

    D3D12_RESOURCE_BARRIER batchPost[3] = {
        TransitionBarrier( m_DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE ),
        TransitionBarrier( m_LightGridBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ),
        TransitionBarrier( m_LightIndexBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE )
    };

    m_CmdList->ResourceBarrier( 3, batchPost );
    m_LightGridInPixelState = true;
}


void D3D12GraphicsEngine::BindMaterialMaps( zCTexture* tex, UINT matRootParam ) {
    // Set the per-material bindless indices (b6 root consts) the PBR PS reads via ResourceDescriptorHeap[...]:
    // this material's normal + ORM map SRV heap slots (loaded onto the surface by LoadAdditionalResources).
    // No normal map -> 0xFFFFFFFF (PS skips the perturb); no _FX/ORM map -> the 1x1 default ORM slot.
    UINT idx[2];
    ResolveMaterialMapSlots( tex, idx );
    m_CmdList->SetGraphicsRoot32BitConstants( matRootParam, 2, idx, 0 );
}


void D3D12GraphicsEngine::BindMaterialMaps( zCTexture* tex, UINT matRootParam, UINT diffuseSlot ) {
    // Fully-bindless variant: the same normal/ORM indices plus the DIFFUSE heap slot as the third b6 constant.
    // Used by every pass on Skeletal.RootSig (which has no SRV table at all) — one 3-DWORD root-constant push
    // per material instead of a descriptor-table bind. The b6 layout matches World.RootSig's, so the world/VOB
    // bindless PS variants read the identical struct.
    UINT idx[3];
    ResolveMaterialMapSlots( tex, idx );
    idx[2] = diffuseSlot;
    m_CmdList->SetGraphicsRoot32BitConstants( matRootParam, 3, idx, 0 );
}


void D3D12GraphicsEngine::ResolveMaterialMapSlots( zCTexture* tex, UINT* outNormalOrm ) const {
    outNormalOrm[0] = 0xFFFFFFFFu;
    outNormalOrm[1] = m_DefaultOrmTexture->GetSrvSlot();
    if ( tex ) {
        if ( MyDirectDrawSurface7* s = tex->GetSurface() ) {
            if ( GfxTexture* n = s->GetNormalmap() ) {
                D3D12Texture* d = D3D12Texture::From( n );
                if ( d->HasSRV() ) outNormalOrm[0] = d->GetSrvSlot();
            }
            if ( GfxTexture* o = s->GetFxMap() ) {
                D3D12Texture* d = D3D12Texture::From( o );
                if ( d->HasSRV() ) outNormalOrm[1] = EncodeOrmSlot( d->GetSrvSlot(), s->GetAvailableMaterials() );
            }
        }
    }
}


void D3D12GraphicsEngine::BindWorldFrameRootState( const XMFLOAT4X4& viewProj ) {
    // Every frame-constant root argument of World.RootSig, in one place: the lit world mesh, the lit
    // instanced VOBs and the lit quad marks (D3D12Fx.cpp) all need the identical set, and any pass that
    // binds a different root signature in between (water, decals, FX) has to re-establish all of it.
    // The caller sets its own PSO, per-material b6 constants and geometry.
    const FogConstants fog = MakeFogConstants();

    m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );   // b1 fog
    BindFrameLights();   // param 3 = light SRV (t1), param 4 = light count (b2) — MUST set both or the
                         // shader's light loop reads a garbage count and runs away (GPU TDR hang).
    // CSM sampling: param 7 = shadow CB (b3), param 8 = shadow-map array SRV (t4). The map was left in
    // PIXEL_SHADER_RESOURCE by the CSM pass; the PS samples it to darken sun-occluded surfaces.
    m_CmdList->SetGraphicsRootConstantBufferView( 7, m_ShadowCBGpu[m_FrameIndex] );
    m_CmdList->SetGraphicsRootDescriptorTable( 8, GetSrvGpuHandle( m_ShadowMap.GetSrvSlot() ) );
    m_CmdList->SetGraphicsRootDescriptorTable( 9, GetSrvGpuHandle( m_PointShadows.GetSrvSlot() ) );   // t5 point-shadow cubes
    m_CmdList->SetGraphicsRoot32BitConstants( 12, 1, &m_ActiveAOMaskSrvSlot, 0 );   // b7 AOCB (simple SSAO mask)
}


XRESULT D3D12GraphicsEngine::DrawWorldMesh( bool /*noTextures*/ ) {
    if ( !m_FrameOpen || !m_Pipelines.World.PSO || !m_Pipelines.World.RootSig || !m_DepthBuffer )
        return XR_SUCCESS;

    MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
    if ( !wm || !wm->GetMeshVertexBuffer() || !wm->GetMeshIndexBuffer() )
        return XR_SUCCESS;

    D3D12VertexBuffer* vb = D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() );
    D3D12VertexBuffer* ib = D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() );
    if ( !vb->GetResource() || !ib->GetResource() ) return XR_SUCCESS;

    // The wrapped world index buffer is a single merged 32-bit buffer (bound R32_UINT, like D3D11).
    const UINT numIndices = ib->GetSizeInBytes() / sizeof( uint32_t );
    if ( numIndices == 0 ) return XR_SUCCESS;

    DX_ZONE( m_CmdList, "Draw World Mesh" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw World Mesh" );

    // Camera matrices — replicate the D3D11 DrawWorldMesh setup exactly so ViewProj is byte-identical:
    // world verts are already world-space (identity world), transform is proj*view (reversed-Z).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetPipelineState( m_Pipelines.World.PSO.Get() );
    BindWorldFrameRootState( viewProj );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
    D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
    m_CmdList->IASetIndexBuffer( &ibv );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // GPU-driven submit (P2.11): one ExecuteIndirect over the shared per-frame command buffer (built by
    // BuildWorldDrawCommands before the depth prepass; water already peeled into g_FrameWaterSurfaces). Each
    // command sets this material's b6 { normal, orm, diffuse } bindless indices then draws its index range — so
    // the whole opaque world is one API call with zero per-draw descriptor binds (the CPU cost this targets).
    // Frame-constant root args (b0 ViewProj, b1 fog, lights, CSM, point-shadow cubes) are already set above.
    if ( m_WorldDrawCount == 0 ) return XR_SUCCESS;
    m_CmdList->ExecuteIndirect( m_WorldIndirectCmdSig.Get(), m_WorldDrawCount,
        m_WorldDrawArgs[m_FrameIndex].Get(), 0, nullptr, 0 );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += m_WorldDrawnIndices / 3;
    return XR_SUCCESS;
}


bool D3D12GraphicsEngine::UploadVobs(
    const std::vector<RenderBucket>& vobs,
    std::vector<FrameVobUpload>& uploads) {
    if ( !m_FrameOpen || !m_VobInstanceBuffer[m_FrameIndex] || !m_VobInstanceBufferPtr[m_FrameIndex] )
        return false;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    if ( !rs.RendererSettings.DrawVOBs )
        return false;

    const UINT frame = m_FrameIndex;
    
    bool hasInstances = false;
    thread_local std::vector<VobInstanceInfo> instances;
    for ( size_t i = 0; i < vobs.size(); ++i) {
        auto& bucket = vobs[i];
        auto& instances = bucket.instances;
        if (instances.empty()) {
            continue;
        }

        const UINT numInstances = instances.size();
        const UINT instBytes = numInstances * sizeof( VobInstanceInfo );

        if ( m_VobInstanceBufferOffset + instBytes > m_VobInstanceBufferCapacity ) {
            if ( !m_VobInstanceOverflowLogged ) {
                LogWarn() << "D3D12: VOB instance ring overflow (" << m_VobInstanceBufferCapacity
                    << " bytes/frame). Some VOBs dropped this frame.";
                m_VobInstanceOverflowLogged = true;
            }
            break;
        }
        hasInstances = true;

        const UINT instOffset = m_VobInstanceBufferOffset;
        memcpy( m_VobInstanceBufferPtr[frame] + instOffset, instances.data(), instBytes );
        m_VobInstanceBufferOffset += instBytes;

        FrameVobUpload up;
        up.visual = reinterpret_cast<MeshVisualInfo*>(g_vobInfoVisualIndexToVisualInfo[i]);
        up.instView = { m_VobInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( VobInstanceInfo ) };
        // Shadow cascades are CPU-culled and never GPU-compacted (BuildVobDrawCommands culled=false), so these
        // two only exist to keep the struct fully initialised.
        up.culledInstView = up.instView;
        up.numInstances = numInstances;
        up.cullVisualIndex = 0xFFFFFFFFu;
        uploads.push_back( up );
    }
    
    return hasInstances;
}


void D3D12GraphicsEngine::UploadFrameVobInstances() {
    // P2.9b-4a: snapshot every visible VOB visual's instances into the per-frame instance ring ONCE, before the
    // depth prepass and the light cull. DrawVobDepthPrepass and DrawVobsInstanced both draw from g_FrameVobUploads,
    // so the ring is filled a single time (the color pass adds no upload → its ring usage is unchanged). Gated on
    // DrawVOBs so that with VOBs disabled neither their depth nor their color is laid down (the cull must never
    // tighten a tile to geometry that isn't actually drawn). The per-frame ring offset was reset in OnBeginFrame.
    g_FrameVobUploads.clear();
    if ( !m_FrameOpen || !m_VobInstanceBuffer[m_FrameIndex] || !m_VobInstanceBufferPtr[m_FrameIndex] )
        return;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    if ( !rs.RendererSettings.DrawVOBs )
        return;

    // Animated static VOBs: a static-VOB visual built from an .MMS carries its zCMorphMesh in MorphMeshVisual
    // (see GothicAPI::OnAddVob) and its MeshInfo vertex buffers are U_DYNAMIC|CA_WRITE for exactly this
    // (WorldConverter::Extract3DSMeshFromVisual2) — so re-uploading the morphed vertices per frame lands in
    // D3D12VertexBuffer's per-frame-in-flight ring copy and can't race an in-flight GPU read. Mirrors D3D11's
    // UpdateMorphMeshVisuals(activeVisuals) call in DrawVOBsInstanced, gated on the same AnimateStaticVobs
    // setting and run at the same point in the frame (while snapshotting the visible visuals, before any draw).
    // Only visible visuals are touched, since the loop below already skips visuals with no instances.
    const bool animateStaticVobs = rs.RendererSettings.AnimateStaticVobs;

    const UINT frame = m_FrameIndex;

    // GPU culling (D3D12Cull.cpp): emit one VobCullVisual per visual alongside the instance memcpy — the local
    // bbox all its instances share plus where they landed in the ring. CSCull reads both the input and the
    // compacted output as StructuredBuffer<VobInstanceInfo>, so the record carries an ELEMENT index; the loop
    // below keeps the ring offset a whole multiple of the stride so byte offset / stride is exact. (Today it
    // trivially is — this is the first thing to allocate after OnBeginFrame reset the ring, and every block is
    // numInstances * sizeof(VobInstanceInfo) — but the ring is shared with the skeletal node-attachment
    // uploads, so aligning explicitly keeps a future reordering from silently mis-indexing every instance.)
    //
    // This is also the LAST point at which GPU culling can still be switched off: BuildVobDrawCommands (next)
    // stamps commands that reference the compacted buffer, and from then on the cull MUST run.
    if ( m_GpuVobCullActive && ( !m_VobCullVisualsPtr[m_FrameIndex] || !m_VobCulledInstances ) )
        m_GpuVobCullActive = false;

    m_VobCullVisualCount = 0;
    VobCullVisual* cullRecords = m_GpuVobCullActive
        ? reinterpret_cast<VobCullVisual*>( m_VobCullVisualsPtr[frame] ) : nullptr;

    for ( auto const& [visualPtr, visual] : Engine::GAPI->GetStaticMeshVisuals() ) {
        if ( !visual || visual->Instances.empty() ) continue;

        if ( animateStaticVobs && visual->MorphMeshVisual )
            WorldConverter::UpdateMorphMeshVisual( visual->MorphMeshVisual, visual );

        const UINT numInstances = static_cast<UINT>( visual->Instances.size() );
        constexpr UINT kInstStride = static_cast<UINT>( sizeof( VobInstanceInfo ) );
        const UINT instBytes = numInstances * kInstStride;

        // Keep every block's start a whole number of instances into the ring — see the note above.
        m_VobInstanceBufferOffset += ( kInstStride - m_VobInstanceBufferOffset % kInstStride ) % kInstStride;

        if ( m_VobInstanceBufferOffset + instBytes > m_VobInstanceBufferCapacity ) {
            if ( !m_VobInstanceOverflowLogged ) {
                LogWarn() << "D3D12: VOB instance ring overflow (" << m_VobInstanceBufferCapacity
                    << " bytes/frame). Some VOBs dropped this frame.";
                m_VobInstanceOverflowLogged = true;
            }
            break;
        }

        const UINT instOffset = m_VobInstanceBufferOffset;
        memcpy( m_VobInstanceBufferPtr[frame] + instOffset, visual->Instances.data(), instBytes );
        m_VobInstanceBufferOffset += instBytes;

        FrameVobUpload up;
        up.visual = visual;
        up.instView = { m_VobInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( VobInstanceInfo ) };
        up.culledInstView = up.instView;
        up.numInstances = numInstances;
        up.cullVisualIndex = 0xFFFFFFFFu;

        if ( cullRecords ) {
            if ( m_VobCullVisualCount < kMaxCullVisuals ) {
                VobCullVisual& rec = cullRecords[m_VobCullVisualCount];
                rec.BBoxMin = visual->BBox.Min;
                rec.BBoxMax = visual->BBox.Max;
                rec.InstanceBase = instOffset / kInstStride;
                rec.InstanceCount = numInstances;
                up.cullVisualIndex = m_VobCullVisualCount++;
                up.culledInstView.BufferLocation = m_VobCulledInstances->GetGPUVirtualAddress() + instOffset;
            } else if ( !m_VobCullVisualOverflowLogged ) {
                // Graceful: the remaining visuals keep instView + the CPU's instance count, so they render
                // uncompacted (i.e. unculled) instead of vanishing.
                LogWarn() << "D3D12: VOB cull-record overflow (" << kMaxCullVisuals
                    << " visuals/frame); the rest render without GPU culling this frame.";
                m_VobCullVisualOverflowLogged = true;
            }
        }

        g_FrameVobUploads.push_back( up );
    }
}


void D3D12GraphicsEngine::DrawVobDepthPrepass() {
    // P2.9b-4a: lay down instanced-VOB depth (alpha-clipped) into the Forward+ opaque prepass so the tiled light
    // cull sees VOB surfaces and bounds each tile's near plane to them — fixing the static 16x16 cutoff where a
    // light on an object in front of distant world geometry got dropped (the tile AABB used to sit at the far
    // world, missing the near VOB). Depth-only via m_Pipelines.World.DepthPrepassVobPSO; consumes the shared g_FrameVobUploads.
    // Same per-material diffuse bind as the color pass for the alpha cutout. Node attachments (weapons/heads) are
    // NOT here — they upload during the skeletal color pass, after the cull; they'll join once skeletal does.
    if ( !m_FrameOpen || !m_Pipelines.World.DepthPrepassVobIndirectPSO || !m_Pipelines.World.RootSig
        || !m_VobIndirectCmdSig || !m_DepthBuffer )
        return;
    ID3D12Resource* drawArgs = GetVobDrawArgsBuffer();
    if ( m_VobDrawCount == 0 || !drawArgs ) return;

    DX_ZONE( m_CmdList, "Depth Prepass (vobs)" );
    TracyD3D12ZoneCGX( m_CmdList.Get(), "Depth Prepass (vobs)" );

    // ViewProj — identical derivation to DrawVobsInstanced so the prepass depth matches the color pass exactly.
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetPipelineState( m_Pipelines.World.DepthPrepassVobIndirectPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );   // b0 ViewProj (fog/lights not referenced)
    // Frame-global wind (b4): dir/time/playerPos set once here; each command overwrites only minHeight/maxHeight
    // (b4[4..5]) per visual. VSDepth reads b4 unconditionally, so this baseline MUST be bound before ExecuteIndirect.
    m_CmdList->SetGraphicsRoot32BitConstants( 11, 12, &m_WindBuffer, 0 );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // One GPU-driven submit over the shared command set BuildVobDrawCommands filled (same set the color pass draws).
    // Each command sets its mesh/instance VBVs + IBV + b6 diffuse (PSDepthClipBindless alpha-clips) + b4 min/max
    // wind, then DrawIndexedInstanced — replacing the per-mesh IASetVertexBuffers/table/draw the CPU path issued.
    // drawArgs (GetVobDrawArgsBuffer): the GPU-culled DEFAULT copy (instance counts patched by CullVobsGPU) when
    // culling is active, else the CPU-written UPLOAD ring.
    m_CmdList->ExecuteIndirect( m_VobIndirectCmdSig.Get(), m_VobDrawCount, drawArgs, 0, nullptr, 0 );
}


XRESULT D3D12GraphicsEngine::DrawVobsInstanced() {
    if ( !m_FrameOpen || !m_Pipelines.World.VobIndirectPSO || !m_Pipelines.World.RootSig
        || !m_VobIndirectCmdSig || !m_DepthBuffer )
        return XR_SUCCESS;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    if ( !rs.RendererSettings.DrawVOBs )
        return XR_SUCCESS;
    ID3D12Resource* drawArgs = GetVobDrawArgsBuffer();
    if ( m_VobDrawCount == 0 || !drawArgs )
        return XR_SUCCESS;

    // Visible VOBs/lights/mobs were already collected once in OnStartWorldRendering (g_FrameVobs/Lights/Mobs);
    // BuildVobDrawCommands (called in OnStartWorldRendering, shared with the depth prepass) already resolved every
    // material's bindless indices + packed the mesh/instance/index views into m_VobDrawArgs — this pass just binds
    // the frame-constant root args and issues ONE ExecuteIndirect.

    // Reversed-Z ViewProj (recomputed; identical derivation to DrawWorldMesh).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = rs.TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    const FogConstants fog = MakeFogConstants();

    m_CmdList->SetPipelineState( m_Pipelines.World.VobIndirectPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );   // b1 fog
    BindFrameLights();   // param 3 = light SRV (t1), param 4 = light count (b2) — see DrawWorldMesh.
    m_CmdList->SetGraphicsRootConstantBufferView( 7, m_ShadowCBGpu[m_FrameIndex] );          // b3 shadow CB
    m_CmdList->SetGraphicsRootDescriptorTable( 8, GetSrvGpuHandle( m_ShadowMap.GetSrvSlot() ) );      // t4 shadow map
    m_CmdList->SetGraphicsRootDescriptorTable( 9, GetSrvGpuHandle( m_PointShadows.GetSrvSlot() ) ); // t5 point-shadow cubes
    // Frame-global wind (b4): dir/time/playerPos bound once; each indirect command overwrites only min/maxHeight
    // (b4[4..5]) per visual. Must be bound before ExecuteIndirect (VSMain reads b4 for the sway).
    m_CmdList->SetGraphicsRoot32BitConstants( 11, 12, &m_WindBuffer, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 12, 1, &m_ActiveAOMaskSrvSlot, 0 );   // b7 AOCB (simple SSAO mask)

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    static_assert( sizeof( VS_ExConstantBuffer_Wind ) == 48, "WindCB (b4) layout must match Vob.hlsl's WindCB" );

    {
        DX_ZONE( m_CmdList, "Draw Vobs" );
        TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw Vobs" );
        // One GPU-driven submit over the command set BuildVobDrawCommands filled: each command sets its mesh/
        // instance VBVs + IBV, b6 { normal, orm, diffuse } bindless indices (PSMainBindless samples all three),
        // b4 per-visual min/max wind, then DrawIndexedInstanced. Replaces the per-mesh table/BindMaterialMaps/
        // IASetVertexBuffers/DrawIndexedInstanced calls that dominated this CPU-bound pass.
        m_CmdList->ExecuteIndirect( m_VobIndirectCmdSig.Get(), m_VobDrawCount, drawArgs, 0, nullptr, 0 );
    }

    // Static skeletal MOBs (g_FrameMobs) are now prepared up front (PrepareFrameSkeletals) and drawn by
    // DrawSkeletalColor alongside the animated NPCs — no longer a nested call here (see OnStartWorldRendering).

    // NOTE: the per-visual Instances lists are cleared once at the end of OnStartWorldRendering (not here),
    // so they still get reset even when DrawVOBs is off and this pass early-outs above.

    rs.RendererInfo.FrameDrawnTriangles += m_VobDrawnTriangles;
    return XR_SUCCESS;
}


void D3D12GraphicsEngine::PrepareFrameSkeletals( std::vector<SkeletalVobInfo*>& vobs, const Frustum* cullFrustum, int shadowCascade,
    const DirectX::XMFLOAT3* sphereCenter, float sphereRange, UINT cascadeCount, bool collectGhosts ) {
    // P2.9b-4b (pre-cull) + shadow-cascade/point-shadow parity: run each candidate skeletal vob's once-per-frame
    // animation update, upload its instance + bone CBs (base meshes) and its node attachments' VOB-instance data
    // into the per-frame rings ONCE (cached in g_SkelUploadCache — the pose data is view-independent), and
    // RECORD the (possibly cached) GPU addresses into the caller's destination list: g_FrameSkelDraws/
    // g_FrameAttachDraws for the main view (shadowCascade < 0, default), D3D12ShadowMap::SkelDraws[c]/
    // AttachDraws[c] for CSM cascade c (shadowCascade >= 0), or D3D12PointShadows::SkelScratch/
    // AttachScratch for a point light (shadowCascade == -2) — mirrors D3D11's
    // Shadows::DrawSkeletalMeshes, which culls the FULL registered skeletal-vob list against the shadow's OWN
    // frustum/sphere rather than reusing the player's view-frustum-culled list (a caster invisible to the
    // player can still cast a visible shadow). NO draws here — DrawSkeletalDepthPrepass/DrawSkeletalColor read
    // g_FrameSkelDraws/g_FrameAttachDraws; D3D12ShadowMap::RecordCascade reads its own SkelDraws[c]/
    // AttachDraws[c]; D3D12PointShadows::Prepare reads its SkelScratch/AttachScratch.
    if ( !m_FrameOpen || !m_SkeletalCBBuffer[m_FrameIndex] || !m_SkeletalCBBufferPtr[m_FrameIndex] )
        return;
    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    if ( !rs.RendererSettings.DrawSkeletalMeshes )
        return;
    if ( vobs.empty() ) return;

    // Multi-cascade mode (cascadeCount > 1, shadowCascade >= 0): cullFrustum is an ARRAY of cascadeCount frusta
    // and each prepared vob is appended to EVERY cascade list whose frustum it intersects. One walk of the
    // registered skeletal-vob list serves all cascades — the per-vob GPU upload was already shared via
    // g_SkelUploadCache, but the walk, the distance cull and the Gothic animation/texani/morph work were not.
    const UINT numCascades = (shadowCascade >= 0) ? std::max<UINT>( 1u, cascadeCount ) : 1u;
    std::vector<FrameSkelDraw>&   outSkel   = (shadowCascade >= 0) ? m_ShadowMap.SkelDraws[shadowCascade]
                                             : (shadowCascade == -2) ? m_PointShadows.SkelScratch : g_FrameSkelDraws;
    std::vector<FrameAttachDraw>& outAttach = (shadowCascade >= 0) ? m_ShadowMap.AttachDraws[shadowCascade]
                                             : (shadowCascade == -2) ? m_PointShadows.AttachScratch : g_FrameAttachDraws;

    // Distance cull: by default around the player camera (SkeletalMeshDrawRadius) — the main-view/CSM case,
    // where cullFrustum does the real work and this is just a coarse pre-filter. When sphereCenter is given
    // (point-shadow case), cull around the LIGHT instead — its range IS the relevant radius, the camera isn't.
    const bool useSphereCull = sphereCenter != nullptr;
    const XMVECTOR cullOrigin = useSphereCull ? XMLoadFloat3( sphereCenter ) : Engine::GAPI->GetCameraPositionXM();
    const float cullRadius = useSphereCull ? sphereRange : rs.RendererSettings.SkeletalMeshDrawRadius;
    const XMVECTOR cullRadiusSq = XMVectorReplicate( cullRadius * cullRadius );
    // The morph-mesh gate below needs the distance to the PLAYER CAMERA specifically, which is NOT cullOrigin
    // (that's the light's position in the point-shadow case). Mirrors GothicAPI::DrawWorldMeshNaive, which
    // measures against the camera and only routes vobs closer than kMorphMeshMaxDistance through the morph path.
    const XMVECTOR camPosXm = Engine::GAPI->GetCameraPositionXM();
    const UINT frame = m_FrameIndex;
    const auto now = Engine::GAPI->GetTotalTimeDW();
    static std::vector<XMFLOAT4X4> boneCache;

    for ( SkeletalVobInfo* vi : vobs ) {
        if ( !vi || !vi->Vob || !vi->VisualInfo ) continue;
        if ( !vi->Vob->GetShowVisual() ) continue;

        // Ghost vobs (invisible-potion NPCs, fading spawns, spirits) never join the regular skinned draw on
        // either backend. What happens to them instead depends on the caller:
        //   - shadow callers (collectGhosts == false): dropped outright. D3D11 skips them the same way in
        //     Shadows::DrawSkeletalMeshes (D3D11GraphicsEngine.cpp:5909/6275/7004) — ghosts cast no shadows.
        //   - the main view (collectGhosts == true): rerouted into GothicAPI::TransparencyVobs below, so
        //     DrawGhostVobs draws them unlit+blended later in the frame. Handled after the cull/extract work
        //     rather than here, because the ghost pass needs a culled vob with its geometry actually built.
        // NOTE the deliberately different conditions: D3D11's shadow skips test
        // `GetVisualAlpha() && GetVobTransparency() < 0.7f`, but its main-view reroute
        // (GothicAPI::DrawWorldMeshNaive:1394) tests `GetVisualAlpha()` ALONE. A ghost at transparency >= 0.7
        // therefore draws as a ghost AND casts a normal shadow. Faithful, not an oversight.
        const bool isGhost = vi->Vob->GetVisualAlpha();
        if ( isGhost && !collectGhosts && vi->Vob->GetVobTransparency() < 0.7f ) continue;

        if ( XMVector3Greater( XMVector3LengthSq( cullOrigin - vi->Vob->GetPositionWorldXM() ), cullRadiusSq ) )
            continue;   // out of skeletal-draw range (player radius, or the light's sphere for point shadows)

        // Cull against the caller's frustum — the player's view for the main pass (no filter needed; that list
        // is already pre-culled by the caller) or the cascade frusta for shadows. In multi-cascade mode the
        // per-cascade results are remembered so the append at the bottom doesn't have to re-test.
        uint32_t cascadeMask = 0;   // bit c set => visible in cullFrustum[c]; only meaningful when numCascades > 1
        if ( cullFrustum ) {
            if ( numCascades > 1 ) {
                const zTBBox3D& bbox = vi->Vob->GetBBox();
                for ( UINT fi = 0; fi < numCascades; ++fi )
                    if ( cullFrustum[fi].Intersects( bbox ) ) cascadeMask |= (1u << fi);
                if ( cascadeMask == 0 ) continue;   // outside every cascade — no upload, no records
            } else if ( !cullFrustum->Intersects( vi->Vob->GetBBox() ) ) {
                continue;
            }
        }

        SkeletalMeshVisualInfo* visual = static_cast<SkeletalMeshVisualInfo*>( vi->VisualInfo );
        zCModel* model = static_cast<zCModel*>( vi->Vob->GetVisual() );
        if ( !model ) continue;

        if ( !visual->Ready.load() ) continue;   // still being built on a worker thread (GothicAPI::LoadzCModelData)

        // Some skeletal vobs arrive with their base mesh not yet extracted (SkeletalMeshes empty but the model
        // does carry soft-skin geometry) — build it lazily. Interactive MOBs whose ONLY renderable content is a
        // node attachment (a lamp post's lamp, some doors) legitimately stay empty and fall through to the
        // attachment loop below — they must NOT be skipped (this was the "MOBs don't render" bug).
        if ( visual->SkeletalMeshes.empty() && model->GetMeshSoftSkinList()->NumInArray > 0 )
            WorldConverter::ExtractSkeletalMeshFromVob( model, visual );

        // Main-view ghost reroute (see the isGhost note above). This is the D3D12 equivalent of the branch
        // GothicAPI::DrawWorldMeshNaive (:1394) takes on D3D11 — that function is the D3D11 collection path and
        // is never called on this backend, so nothing else has ever pushed a SKELETAL entry into
        // TransparencyVobs here. Static ghost VOBs already arrive via CVVH_AddNotDrawnVobToList
        // (GothicAPI.cpp:4698/6771, the `normalVob` branch), which is why DrawGhostVobs' skeletal half existed
        // but was unreachable: ghost NPCs simply rendered as nothing.
        //
        // Placed AFTER the distance/frustum cull and the lazy ExtractSkeletalMeshFromVob above (D3D11 pushes
        // slightly earlier, before the mesh work) so the ghost pass receives a culled vob whose geometry is
        // actually built — DrawGhostVobs skips entries with an empty SkeletalMeshes list.
        if ( isGhost ) {
            if ( collectGhosts ) {
                // Gothic lerps its animations only when this is set and below ~2000. The regular path below
                // hardcodes 500; ghosts get the real camera distance, which is what D3D11 passes (:1389).
                float camDist;
                XMStoreFloat( &camDist, XMVector3Length( vi->Vob->GetPositionWorldXM() - camPosXm ) );
                model->SetDistanceToCamera( camDist );
                model->UpdateAttachedVobs();
                vi->LastAniUpdateFrame = now;   // DrawGhostVobs re-runs UpdateAttachedVobs only if this is stale

                Engine::GAPI->GetTransparencyVobs().emplace_back(
                    camDist, vi->Vob->GetVobTransparency(), vi, nullptr );
            }
            continue;   // never joins the regular skinned draw, whether or not it was collected
        }

        // NOTE: UpdateMeshLibTexAniState is intentionally NOT called here. It mutates the model's SHARED texture
        // slots, so it's only meaningful immediately before drawing a specific instance's meshes (all instances
        // of a model share the slots). The MAIN-VIEW draw paths (DrawSkeletalDepthPrepass / DrawSkeletalColor)
        // call it per-record right before reading the materials — which is why FrameSkelDraw carries vobInfo.
        // The shadow-cascade recorder can't: it may run on a pool thread, where mutating Gothic's shared texani
        // state is unsafe. It reads the g_SkelMatSrvs snapshot the cache-miss branch below takes instead.

        // Upload cache: skip straight to the cached GPU addresses / attachment records if this vob was already
        // prepared by an earlier cull pass this frame (e.g. the main view already prepared an NPC that a shadow
        // cascade also wants — its pose doesn't change between passes).
        auto cacheIt = g_SkelUploadCache.find( vi );
        if ( cacheIt == g_SkelUploadCache.end() ) {
            // Camera distance for the MMS morph-mesh gate on this vob's attachments (see the loop below). Measured
            // once per vob here, inside the cache-miss branch, so whichever pass prepares the vob first this frame
            // decides — the value is view-independent (always the player camera), like the pose data itself.
            float camDist;
            XMStoreFloat( &camDist, XMVector3Length( vi->Vob->GetPositionWorldXM() - camPosXm ) );
            const bool inMorphMeshRange = camDist < kMorphMeshMaxDistance;

            model->SetDistanceToCamera( 500 );
            if ( vi->LastAniUpdateFrame != now ) {
                vi->LastAniUpdateFrame = now;
                model->UpdateAttachedVobs();   // once/frame — this is why the pass can't just re-run in a prepass
            }
            
            model->UpdateMeshLibTexAniState();

            // Snapshot THIS instance's per-material alpha-clip diffuse SRV slots while the model's shared texani
            // slots still describe it (see [[skeletal-texani-shared-slots]]) — the shadow-cascade recorder can't
            // re-run UpdateMeshLibTexAniState from a pool thread, so it indexes this instead. Same order as
            // visual->SkeletalMeshes iterates, which is stable for the rest of the frame (the map isn't mutated
            // after ExtractSkeletalMeshFromVob above). The outer vector grows monotonically and its inner vectors
            // keep their capacity across frames, so this settles into zero per-frame allocations.
            SkelUploadCache entry;
            if ( g_SkelMatSrvCount >= g_SkelMatSrvs.size() )
                g_SkelMatSrvs.emplace_back();
            {
                std::vector<UINT>& matSrvs = g_SkelMatSrvs[g_SkelMatSrvCount];
                matSrvs.clear();
                for ( auto const& [mat, meshList] : visual->SkeletalMeshes )
                    matSrvs.push_back( ResolveShadowDiffuseSlot( mat ? mat->GetAniTexture() : nullptr ) );
                entry.matSrvIndex = static_cast<uint32_t>( g_SkelMatSrvCount++ );
            }

            // Bone palette (object-space matrices) for the model's current animation pose. Needed for BOTH the
            // base skinned mesh AND the node-attachment world matrices, so compute it (and xmWorld) before either.
            boneCache.clear();
            model->GetBoneTransforms( &boneCache );
            UINT numBones = static_cast<UINT>( boneCache.size() );
            if ( numBones == 0 ) continue;
            if ( numBones > kSkeletalMaxBones ) numBones = kSkeletalMaxBones;

            const XMMATRIX xmWorld = vi->Vob->GetWorldMatrixXM() * XMMatrixScalingFromVector( model->GetModelScaleXM() );

            // Baked ground/ambient light for this vob (mirrors D3D11's DrawSkeletalMeshVobs non-shadow-branch
            // modelColor: DEFAULT_LIGHTMAP_POLY_COLOR indoors, else the ground polygon's lightStat at the vob's
            // position). A hardcoded white here (as this used to be, "first-light" placeholder) makes vertLighting
            // (i.col.g) == 1 always, which zeroes out ShadowAOStrength/WorldAOStrength's lerp(1.0, vertLighting,
            // strength) to a no-op regardless of the slider — skeletal meshes/attachments then can't darken
            // indoors like world/VOB geometry does. Sampled once per vob per frame (one polygon lookup), not
            // per-vertex.
            float4 groundLight( 1.0f, 1.0f, 1.0f, 1.0f );
            if ( vi->Vob->IsIndoorVob() ) {
                groundLight = DEFAULT_LIGHTMAP_POLY_COLOR_F;
            } else if ( zCPolygon* groundPoly = vi->Vob->GetGroundPoly() ) {
                float3 vobPos = vi->Vob->GetPositionWorld();
                float3 lightStat = groundPoly->GetLightStatAtPos( vobPos );
                groundLight = float4( lightStat.z / 255.0f, lightStat.y / 255.0f, lightStat.x / 255.0f, 1.0f );
            }

            // Base skinned mesh — skipped entirely for attachment-only MOBs (empty SkeletalMeshes). Mirrors
            // D3D11 DrawSkeletalMeshVobs, which guards its base pass on !SkeletalMeshes.empty() but always runs
            // attachments.
            if ( !visual->SkeletalMeshes.empty() ) {
                // Allocate the per-instance CB + bone CB from the per-frame ring (each 256-byte aligned so it can
                // be bound as a root CBV). Uploaded ONCE here; every consumer (prepass/color/shadow cascades)
                // reuses these two GPU addresses via the cache.
                const UINT instSize = static_cast<UINT>( sizeof( SkeletalInstanceCB ) );
                const UINT boneSize = numBones * static_cast<UINT>( sizeof( XMFLOAT4X4 ) );
                const UINT instOff = AlignCB( m_SkeletalCBBufferOffset );
                const UINT boneOff = AlignCB( instOff + instSize );
                if ( boneOff + boneSize > m_SkeletalCBBufferCapacity ) {
                    if ( !m_SkeletalCBOverflowLogged ) {
                        LogWarn() << "D3D12: skeletal CB ring overflow (" << m_SkeletalCBBufferCapacity
                                  << " bytes/frame). Some skeletal meshes dropped this frame.";
                        m_SkeletalCBOverflowLogged = true;
                    }
                    break;
                }

                SkeletalInstanceCB inst = {};
                XMStoreFloat4x4( &inst.World, xmWorld );
                inst.ModelColor = XMFLOAT4( groundLight.x, groundLight.y, groundLight.z, groundLight.w );
                inst.Fatness = model->GetModelFatness();

                uint8_t* ringBase = m_SkeletalCBBufferPtr[frame];
                memcpy( ringBase + instOff, &inst, instSize );
                memcpy( ringBase + boneOff, boneCache.data(), boneSize );
                m_SkeletalCBBufferOffset = boneOff + boneSize;

                const D3D12_GPU_VIRTUAL_ADDRESS ringGpu = m_SkeletalCBBuffer[frame]->GetGPUVirtualAddress();
                entry.instCb = ringGpu + instOff;
                entry.boneCb = ringGpu + boneOff;
                entry.hasBaseMesh = true;
            }

            // Node attachments (weapons/heads/lamps/held items): world = modelWorld * boneMatrix[node]. Upload
            // each as a VOB instance into the VOB ring NOW (pre-cull) so it can be depth-prepassed, color-drawn
            // AND shadow-cast from one snapshot. Lazily convert the node visual on first sight (or if changed).
            gtl::flat_hash_map<int, std::vector<MeshVisualInfo*>>& nodeAttachments = vi->NodeAttachments;
            zCArray<zCModelNodeInst*>* nodeList = model->GetNodeList();
            const int nodeCount = nodeList ? std::min<int>( static_cast<int>( boneCache.size() ), nodeList->NumInArray ) : 0;
            for ( int n = 0; n < nodeCount; ++n ) {
                zCModelNodeInst* node = nodeList->Array[n];
                if ( !node || !node->NodeVisual ) continue;   // no attachment on this node (e.g. sheathed weapon)

                // Extraction runs on a worker thread: unpacking the wedge lists and — dominating it —
                // creating this attachment's vertex/index/shadow-index buffers costs ~0.1ms per
                // D3D12MA::CreateResource, and an NPC walking into view brings several of them at once
                // (weapon + head morph mesh + held items). Doing that inline hitched the frame the NPC
                // spawned on, which is exactly what a player walking into town or a dense forest hits.
                // The attachment simply isn't drawn until the job lands (a frame or two later); the
                // Ready gate below is what enforces that. Note the visual-changed comparison is itself
                // gated on Ready, because the worker writes MeshVisualInfo::Visual as it finishes.
                auto it = nodeAttachments.find( n );
                if ( it == nodeAttachments.end() ) {
                    WorldConverter::ExtractNodeVisualAsync( n, node, nodeAttachments );
                    it = nodeAttachments.find( n );
                } else if ( !it->second.empty() && it->second[0]
                    && it->second[0]->Ready.load( std::memory_order_acquire )
                    && it->second[0]->Visual != node->NodeVisual ) {
                    WorldConverter::ExtractNodeVisualAsync( n, node, nodeAttachments );  // visual changed
                    it = nodeAttachments.find( n );
                }
                if ( it == nodeAttachments.end() ) continue;

                XMFLOAT4X4 attWorld;
                XMStoreFloat4x4( &attWorld, xmWorld * XMLoadFloat4x4( &boneCache[n] ) );
                for ( MeshVisualInfo* mvi : it->second ) {
                    if ( !mvi ) continue;
                    // Still being extracted on a worker thread — Meshes/MeshesByTexture are being written
                    // to right now, so skip this attachment entirely for this frame rather than race them.
                    if ( !mvi->Ready.load( std::memory_order_acquire ) || !mvi->Visual ) continue;
                    const bool isMMS = strcmp( mvi->Visual->GetFileExtension( 0 ), ".MMS" ) == 0;
                    // MMS attachments only MORPH within kMorphMeshMaxDistance of the camera — beyond that they
                    // render as their undeformed rest mesh. This mirrors D3D11 exactly: GothicAPI::
                    // DrawWorldMeshNaive splits the skeletal vobs into a `drawAsMorphMesh` list (dist < 1000, has
                    // an MMS attachment) drawn with distance=500 and a `drawRegular` list drawn with FLT_MAX, and
                    // DrawSkeletalMeshVobs' morph branch is gated on `isMMS && distance < 1000` — so far-away MMS
                    // attachments fall through to its plain instanced attachment path, which carries no
                    // Fatness/Scaling at all. Morphing is CPU-side (Gothic's own zCMorphMesh::AdvanceAnis +
                    // CalcVertexPositions, then a full vertex-buffer re-upload per animation frame), so this gate
                    // is a real per-frame CPU/bandwidth saving on crowds, not just a visual nicety.
                    const bool morphActive = isMMS && inMorphMeshRange;
                    // Fatness/Scaling inflate-along-normal (mirrors D3D11's VS_ExConstantBuffer_PerInstanceNode,
                    // VS_ExNode.hlsl: localPos = (pos + Fatness*normal) * Scaling). Only actively-morphing MMS
                    // attachments get a non-trivial value in D3D11 — a plain weapon/lamp/head attachment (and a
                    // beyond-range MMS one) is Fatness=0/Scaling=1, a no-op. Reuses the VobInstanceInfo wind
                    // fields (@132/@136) since node attachments never sway in the wind — see Vob.hlsl's
                    // VSMainAttach/VSDepthAttach, which reinterpret them as {Fatness, Scaling}.
                    const float attFatness = morphActive ? std::max<float>( 0.f, model->GetModelFatness() * 0.35f ) : 0.f;
                    const float attScaling = morphActive ? ( model->GetModelFatness() * 0.02f + 1.f ) : 1.f;
                    node->TexAniState.UpdateTexList();
                    if ( isMMS && !morphActive ) {
                        // Out of morph range: D3D11 still advances the morph mesh's TEXTURE animation for every
                        // MMS attachment regardless of distance (DrawSkeletalMeshVobs' `if (updateState)` block
                        // runs before its distance-gated morph branch), it just doesn't deform the geometry.
                        // UpdateMorphMeshVisual does this same UpdateTexList call itself, hence the else-branch.
                        reinterpret_cast<zCMorphMesh*>( mvi->Visual )->GetTexAniState()->UpdateTexList();
                    } else if ( morphActive ) {
                        // Facial morph meshes (heads) and bow/crossbow draw-animation meshes: deformation is done
                        // entirely by the original engine (zCMorphMesh::AdvanceAnis/CalcVertexPositions, opaque
                        // calls into Gothic's own compiled code) and re-uploaded into this MeshInfo's dynamic
                        // vertex buffer by UpdateMorphMeshVisual — backend-neutral (only touches GfxVertexBuffer::
                        // UpdateBuffer), so this is a straight reuse of the D3D11 path, not a reimplementation. Its
                        // own LastAniUpdateFrame guard already limits this to once per animation frame.
                        WorldConverter::UpdateMorphMeshVisual( mvi->Visual, mvi );
                    }
                    for ( auto const& [attMat, attMeshes] : mvi->Meshes ) {
                        zCTexture* attTex = attMat ? attMat->GetAniTexture() : nullptr;
                        for ( auto const& attMesh : attMeshes ) {
                            if ( !attMesh || attMesh->Indices.empty() ) continue;
                            if ( !attMesh->GetMeshVertexBuffer() || !attMesh->GetMeshIndexBuffer() ) continue;

                            const UINT instBytes = static_cast<UINT>( sizeof( VobInstanceInfo ) );
                            if ( m_VobInstanceBufferOffset + instBytes > m_VobInstanceBufferCapacity ) {
                                if ( !m_VobInstanceOverflowLogged ) {
                                    LogWarn() << "D3D12: VOB instance ring overflow (skeletal attachments dropped this frame).";
                                    m_VobInstanceOverflowLogged = true;
                                }
                                break;
                            }
                            VobInstanceInfo vii = {};
                            vii.world = attWorld;
                            vii.color = groundLight.ToDWORD();
                            vii.windStrenth = attFatness;             // reinterpreted as Fatness — see VSMainAttach
                            vii.canBeAffectedByPlayer = attScaling;   // reinterpreted as Scaling — see VSMainAttach
                            const UINT instOffset = m_VobInstanceBufferOffset;
                            memcpy( m_VobInstanceBufferPtr[frame] + instOffset, &vii, instBytes );
                            m_VobInstanceBufferOffset += instBytes;
                            const D3D12_VERTEX_BUFFER_VIEW attInstView = {
                                m_VobInstanceBuffer[frame]->GetGPUVirtualAddress() + instOffset, instBytes, sizeof( VobInstanceInfo ) };
                            // Diffuse SRV heap slot resolved HERE (main thread) so the MT shadow-cascade recorder
                            // never has to read Gothic texture state; the main-view prepass/color paths still use
                            // attTex directly because they CacheIn, which a shadow-only alpha cutout deliberately
                            // must not do.
                            entry.attachments.push_back( { attMesh.get(), attTex, attInstView, vi->Vob,
                                ResolveShadowDiffuseSlot( attTex ) } );
                        }
                    }
                }
            }

            cacheIt = g_SkelUploadCache.emplace( vi, std::move( entry ) ).first;
        }

        const SkelUploadCache& cached = cacheIt->second;
        if ( numCascades > 1 ) {
            // Multi-cascade: fan the (already-uploaded) records out to every cascade this vob is visible in.
            for ( UINT fi = 0; fi < numCascades; ++fi ) {
                if ( (cascadeMask & (1u << fi)) == 0 ) continue;
                if ( cached.hasBaseMesh )
                    m_ShadowMap.SkelDraws[fi].push_back( { vi, visual, cached.instCb, cached.boneCb, cached.matSrvIndex } );
                for ( const FrameAttachDraw& a : cached.attachments )
                    m_ShadowMap.AttachDraws[fi].push_back( a );
            }
        } else {
            if ( cached.hasBaseMesh )
                outSkel.push_back( { vi, visual, cached.instCb, cached.boneCb, cached.matSrvIndex } );
            for ( const FrameAttachDraw& a : cached.attachments )
                outAttach.push_back( a );
        }
    }
}


void D3D12GraphicsEngine::DrawSkeletalDepthPrepass() {
    // P2.9b-4b (pre-cull): lay down skeletal base-mesh + node-attachment depth into the Forward+ opaque prepass
    // so the tiled light cull bounds tiles to NPCs/monsters (fixing the static near-skeletal cutoff). Depth-only;
    // consumes the shared records. Base meshes via m_Pipelines.Skeletal.DepthPrepassPSO (skinned), attachments via
    // m_Pipelines.World.DepthPrepassVobPSO (packed vertex + instance) — both read only b0 + t0/s0, so no BindFrameLights.
    if ( !m_FrameOpen || !m_DepthBuffer ) return;
    if ( m_SkeletalDrawCount == 0 && m_AttachDrawCount == 0 ) return;

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };

    // Base skinned meshes (depth only) — one ExecuteIndirect over the command set BuildSkeletalDrawCommands
    // filled (the same set the color pass draws; PSDepthClip only reads b6's diffuse index for the alpha
    // cutout and ignores the normal/ORM constants each command also carries).
    if ( m_SkeletalDrawCount > 0 && m_SkeletalIndirectCmdSig && m_SkeletalDrawArgs[m_FrameIndex]
        && m_Pipelines.Skeletal.DepthPrepassPSO && m_Pipelines.Skeletal.RootSig ) {
        DX_ZONE( m_CmdList, "Depth Prepass (skeletal)" );
        TracyD3D12ZoneCGX( m_CmdList.Get(), "Depth Prepass (skeletal)" );
        m_CmdList->SetPipelineState( m_Pipelines.Skeletal.DepthPrepassPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_Pipelines.Skeletal.RootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
        // Each command sets its own b1 instance CBV + b2 bone CBV + skinned VBV + IBV + b6 material consts,
        // then DrawIndexed — replacing the per-vob root-CBV sets and per-mesh IA binds the CPU path issued.
        m_CmdList->ExecuteIndirect( m_SkeletalIndirectCmdSig.Get(), m_SkeletalDrawCount,
            m_SkeletalDrawArgs[m_FrameIndex].Get(), 0, nullptr, 0 );
    }

    // Node attachments (depth only) through the VOB attachment depth PSO (Fatness/Scaling variant — see
    // Vob.hlsl's VSDepthAttach; must match DrawSkeletalColor's attachment PSO choice or the prepass depth
    // won't reflect the same inflate/scale as the color pass, the same class of bug the wind fix addressed).
    if ( m_AttachDrawCount > 0 && m_VobIndirectCmdSig && m_AttachDrawArgs[m_FrameIndex]
        && m_Pipelines.World.DepthPrepassVobAttachPSO && m_Pipelines.World.RootSig ) {
        DX_ZONE( m_CmdList, "Depth Prepass (attachments)" );
        TracyD3D12ZoneCGX( m_CmdList.Get(), "Depth Prepass (attachments)" );
        m_CmdList->SetPipelineState( m_Pipelines.World.DepthPrepassVobAttachPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        // The shared VOB signature also writes b4[4..5]; VSDepthAttach never reads b4, but bind the frame-global
        // wind baseline anyway so the parameter is never left undefined for a later pass on this root sig.
        m_CmdList->SetGraphicsRoot32BitConstants( 11, 12, &m_WindBuffer, 0 );
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
        m_CmdList->ExecuteIndirect( m_VobIndirectCmdSig.Get(), m_AttachDrawCount,
            m_AttachDrawArgs[m_FrameIndex].Get(), 0, nullptr, 0 );
    }
}


void D3D12GraphicsEngine::DrawSkeletalColor() {
    // P2.9b-4b (post-cull): draw the skeletal base meshes + node attachments collected by PrepareFrameSkeletals,
    // lit through the tile grid. Base via m_Pipelines.Skeletal.PSO, attachments via m_Pipelines.World.VobPSO — same PSOs/binds as before the
    // 4b split, just consuming the shared records (no re-upload, no re-run of the once/frame animation update).
    if ( !m_FrameOpen || !m_Pipelines.Skeletal.PSO || !m_Pipelines.Skeletal.RootSig || !m_DepthBuffer ) return;
    if ( m_SkeletalDrawCount == 0 && m_AttachDrawCount == 0 ) return;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();

    // Reversed-Z ViewProj (identical derivation to DrawWorldMesh / DrawVobsInstanced).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = rs.TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    const FogConstants fog = MakeFogConstants();
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };

    // Base skinned meshes (lit) — one ExecuteIndirect over the shared command set (see the prepass).
    if ( m_SkeletalDrawCount > 0 && m_SkeletalIndirectCmdSig && m_SkeletalDrawArgs[m_FrameIndex] ) {
        DX_ZONE( m_CmdList, "Draw skeletal" );
        TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw skeletal" );
        m_CmdList->SetPipelineState( m_Pipelines.Skeletal.PSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_Pipelines.Skeletal.RootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->SetGraphicsRoot32BitConstants( 3, 8, &fog, 0 );   // b3 fog
        BindFrameLights( 4, 5, 6, 7 );   // light SRV(t1)+count(b4)+grid(t2)+index(t3) — MUST set all (see BindFrameLights)
        m_CmdList->SetGraphicsRootConstantBufferView( 8, m_ShadowCBGpu[m_FrameIndex] );        // b5 shadow CB
        m_CmdList->SetGraphicsRootDescriptorTable( 9, GetSrvGpuHandle( m_ShadowMap.GetSrvSlot() ) );    // t4 shadow map
        m_CmdList->SetGraphicsRootDescriptorTable( 10, GetSrvGpuHandle( m_PointShadows.GetSrvSlot() ) ); // t5 point-shadow cubes
        m_CmdList->SetGraphicsRoot32BitConstants( 12, 1, &m_ActiveAOMaskSrvSlot, 0 );   // b8 AOCB (simple SSAO mask)
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
        m_CmdList->ExecuteIndirect( m_SkeletalIndirectCmdSig.Get(), m_SkeletalDrawCount,
            m_SkeletalDrawArgs[m_FrameIndex].Get(), 0, nullptr, 0 );
    }

    // Node attachments (lit) through the VOB attachment PSO (Fatness/Scaling variant — see Vob.hlsl's
    // VSMainAttach; non-morph attachments get Fatness=0/Scaling=1, a no-op, so this is a drop-in replacement
    // for the plain VobPSO). BindFrameLights() is REQUIRED — the VOB PS reads the light count/grid, so an
    // unbound count would run the loop on garbage → GPU TDR hang.
    if ( m_AttachDrawCount > 0 && m_VobIndirectCmdSig && m_AttachDrawArgs[m_FrameIndex]
        && m_Pipelines.World.VobAttachPSO && m_Pipelines.World.RootSig ) {
        DX_ZONE( m_CmdList, "Draw attachments" );
        TracyD3D12ZoneCGX( m_CmdList.Get(), "Draw attachments" );
        m_CmdList->SetPipelineState( m_Pipelines.World.VobAttachPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );   // b1 fog (VOB root sig)
        BindFrameLights();
        m_CmdList->SetGraphicsRootConstantBufferView( 7, m_ShadowCBGpu[m_FrameIndex] );        // b3 shadow CB
        m_CmdList->SetGraphicsRootDescriptorTable( 8, GetSrvGpuHandle( m_ShadowMap.GetSrvSlot() ) );    // t4 shadow map
        m_CmdList->SetGraphicsRootDescriptorTable( 9, GetSrvGpuHandle( m_PointShadows.GetSrvSlot() ) ); // t5 point-shadow cubes
        m_CmdList->SetGraphicsRoot32BitConstants( 12, 1, &m_ActiveAOMaskSrvSlot, 0 );   // b7 AOCB (simple SSAO mask)
        // Frame-global wind baseline for the shared VOB signature's b4[4..5] partial writes — see the prepass.
        m_CmdList->SetGraphicsRoot32BitConstants( 11, 12, &m_WindBuffer, 0 );
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
        // Each command sets its own mesh/instance VBVs + IBV + b6 { normal, ORM, diffuse } bindless material
        // constants, then DrawIndexed — PSMainBindless reads the identical MaterialCB layout the base meshes use.
        m_CmdList->ExecuteIndirect( m_VobIndirectCmdSig.Get(), m_AttachDrawCount,
            m_AttachDrawArgs[m_FrameIndex].Get(), 0, nullptr, 0 );
    }

    // Counted once at build time (BuildSkeletalDrawCommands) rather than per draw — the commands are submitted
    // wholesale now, so there is no per-mesh CPU site left to accumulate from.
    rs.RendererInfo.FrameDrawnTriangles += m_SkeletalDrawnTriangles;
}
