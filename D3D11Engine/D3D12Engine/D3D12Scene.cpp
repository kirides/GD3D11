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

// imgui_impl_dx12 calls CreateDXGIFactory1 directly (for tearing detection). dxgi.dll is present on
// every Windows 7+ and the D3D11 fallback swapchain already needs it at runtime, so a load-time link
// here is safe — it does NOT reintroduce the D3D12 soft-dependency that lets old systems fall back.
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
#include "D3D12EngineCommon.h"

// culledInstView / cullVisualIndex are the GPU-culling half (D3D12Cull.cpp): the same byte range as instView
// but based at m_VobCulledInstances (where CSCull compacts the survivors), plus this visual's index into the
// per-frame VobCullVisual record buffer. cullVisualIndex == 0xFFFFFFFF means "not culled" (the visual didn't
// fit the record cap, or GPU culling is off) and the command keeps instView + the CPU's instance count.
struct FrameVobUpload {
    MeshVisualInfo* visual;
    D3D12_VERTEX_BUFFER_VIEW instView;
    D3D12_VERTEX_BUFFER_VIEW culledInstView;
    UINT numInstances;
    uint32_t cullVisualIndex;
};

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

    // Point-light shadow selection (P2.10b/c): the shadowed lights chosen this frame (closest-in-range, capped
    // at kMaxShadowCubes) — filled by BuildFrameLightBuffer (which also writes ShadowCubeIndex into the GPU light
    // struct), consumed by RenderPointShadows to render each light's 6 cube faces.
    // Static/dynamic split (P2.10g): every winner gets its active cube = (static-aside copy) + dynamic overlay
    // each frame. renderStatic=true → also re-render the STATIC casters into the static-aside slot first (only
    // when the slot is fresh / the light moved / range changed); otherwise the cached static depth is reused.
    // renderDynamic (P2.10h round-robin): whether the per-frame skeletal overlay (Phase C) runs THIS frame for
    // this winner — always true for the closest kAlwaysDynamicCount winners, round-robined (a few per frame,
    // oldest-serviced-first) for the rest, so a large persisted-light count doesn't multiply the CPU cost of
    // sphere-culling the full skeletal-vob list against every single shadowed light every frame.
    struct FramePointShadow { DirectX::XMFLOAT3 posWS; float range; UINT slot; bool renderStatic; bool renderDynamic; };
    std::vector<FramePointShadow> g_FramePointShadows;

    // Per-frame VOB instance-ring snapshot (P2.9b-4a). UploadFrameVobInstances memcpys each visible visual's
    // instances into the ring ONCE (before the light cull), recording the resulting stream view + count here.
    // The depth prepass (DrawVobDepthPrepass) and the color pass (DrawVobsInstanced) then BOTH draw from these
    // records — no second upload, so the color pass's ring usage is unchanged. Rebuilt every frame.
    std::vector<FrameVobUpload> g_FrameVobUploads;

    // Per-frame skeletal shared-upload records (P2.9b-4b). PrepareFrameSkeletals runs the once-per-frame
    // animation update and uploads each vob's bone/instance CBs (base meshes) and its node attachments'
    // VOB-instance data (into the VOB ring) BEFORE the cull, recording GPU addresses here. Then
    // DrawSkeletalDepthPrepass (pre-cull, depth-only) and DrawSkeletalColor (post-cull, lit) both draw from
    // these — so the animation update is never run twice and nothing is uploaded twice. Rebuilt each frame.
    // matSrvIndex indexes g_SkelMatSrvs (below): the per-material diffuse descriptor handles for this vob's
    // visual->SkeletalMeshes, snapshotted on the main thread while the model's shared texani slots were still
    // set for THIS instance. The MT cascade recorder must use it instead of calling UpdateMeshLibTexAniState +
    // GetAniTexture itself (Gothic's texani state is per-MODEL shared and not thread-safe).
    struct FrameSkelDraw   { SkeletalVobInfo* vobInfo;  SkeletalMeshVisualInfo* visual; D3D12_GPU_VIRTUAL_ADDRESS instCb; D3D12_GPU_VIRTUAL_ADDRESS boneCb; uint32_t matSrvIndex; };
    // owner = the skeletal vob this attachment hangs off of (its NPC/MOB) — needed so point-shadow self-shadow
    // exclusion (BuildPointShadowExcludeList) can skip a torch-holding NPC's own attachments too, not just its
    // base mesh; unused (nullptr-safe) by the main-view/CSM consumers, which don't exclude anything.
    // srv = the same main-thread-resolved diffuse handle as FrameSkelDraw::matSrvIndex, for the same reason —
    // the main-view prepass/color paths still resolve `tex` themselves (they CacheIn, which the shadow paths
    // deliberately don't), so both fields stay live.
    struct FrameAttachDraw { MeshInfo* mesh; zCTexture* tex; D3D12_VERTEX_BUFFER_VIEW instView; const zCVob* owner; D3D12_GPU_DESCRIPTOR_HANDLE srv; };
    std::vector<FrameSkelDraw>   g_FrameSkelDraws;
    std::vector<FrameAttachDraw> g_FrameAttachDraws;

    // Per-cascade shadow-caster records (parity with D3D11's Shadows::DrawSkeletalMeshes, which culls the FULL
    // registered skeletal-vob list against the CASCADE frustum, not the player's view frustum — a caster outside
    // the player's view can still cast a shadow into it). Filled by PrepareFrameSkeletals(..., cascadeFrustum, c)
    // right before that cascade is rendered; cleared and rebuilt every frame. Must match
    // D3D12GraphicsEngine::kShadowCascades (private, so re-stated here — the array size is asserted below).
    constexpr UINT kShadowCascadeCount = 3;
    std::vector<FrameSkelDraw>   g_ShadowSkelDraws[kShadowCascadeCount];
    std::vector<FrameAttachDraw> g_ShadowAttachDraws[kShadowCascadeCount];

    // Point-shadow skeletal-caster scratch list — same FULL-registered-vob-list parity fix as the CSM cascades
    // above, applied to point lights: PrepareFrameSkeletals(..., sphereCenter, sphereRange) sphere-culls
    // Engine::GAPI->GetSkeletalMeshVobs() against THIS light instead of reusing the player-view-culled
    // g_FrameSkelDraws (a caster invisible to the player, but inside a torch's range, can still cast a shadow
    // into it). Cleared and refilled once per light inside RenderPointShadows's dynamic-overlay phase (not a
    // per-frame-once list like the cascades, since every point light has its own sphere). Node-attachment casters
    // are intentionally NOT drawn into point-shadow cubes yet (Phase C has no attachment draw path), so
    // g_PointShadowAttachDraws is populated by PrepareFrameSkeletals (it always fills both) but left unread here.
    std::vector<FrameSkelDraw>   g_PointShadowSkelDraws;
    std::vector<FrameAttachDraw> g_PointShadowAttachDraws;

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
    std::vector<std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>> g_SkelMatSrvs;
    size_t g_SkelMatSrvCount = 0;

    // Shadow-cascade world-mesh caster set, resolved ONCE per frame on the main thread from the union-frustum
    // section list: the per-material work (alpha/translucency filtering + the bindless diffuse index for the
    // alpha cutout, which needs Gothic's zCMaterial::GetAniTexture) is identical for every cascade — only the
    // frustum test differs. Hoisting it out both cuts that work to a third and leaves CullShadowCascade with
    // nothing but frustum tests and per-cascade writes, so it is safe on a pool thread.
    struct ShadowWorldCaster {
        const WorldMeshInfo* mesh;    // for IsWorldMeshVisibleInFrustum (bbox test only)
        uint32_t diffuseIdx;          // bindless SRV slot for PSShadowClip's alpha cutout
        UINT     indexCount;
        UINT     startIndex;
    };
    std::vector<ShadowWorldCaster> g_ShadowWorldCasters;

    // Per-cascade grass caster boxes surviving that cascade's frustum. Culled in CullShadowCascade so
    // RecordShadowCascade does nothing but issue draws.
    std::vector<GVegetationBox*> g_ShadowGrassBoxes[kShadowCascadeCount];

    // Grass caster wind constants (b1 GrassCB), filled once per frame on the main thread — only the fields
    // Vegetation.hlsl's VSDepth wind sway reads matter for a shadow caster. Mirrors
    // GVegetationBox::PopulateConstantBuffer (see DrawVegetation); hoisted out of the per-cascade recorders so
    // they never call into Gothic (GetTimeSeconds / GetPlayerVob) from a pool thread.
    struct ShadowGrassCBData { float Time; float WindStrength; float HeroAffectStrength; float _pad0; DirectX::XMFLOAT3 PlayerPosWS; float _pad1; };
    static_assert( sizeof( ShadowGrassCBData ) == 8 * sizeof( float ), "Grass.RootSig param 3 pushes 8 root constants" );
    ShadowGrassCBData g_ShadowGrassCB = {};

    // Forward+ MVP light buffer (P2.9a): the whole visible-light list is rebuilt from offset 0 each frame,
    // so the ring is just kBackBufferCount snapshots (no per-draw offset). Cap matches D3D11 MAX_TILED_LIGHTS.
    constexpr UINT kMaxFrameLights = 400;

    // Per-frame GPU point light — byte-identical to D3D11 TiledPointLight (48 B) so the layout is reusable
    // when the compute tiled-culling step (P2.9b) lands. Brute-force MVP fills PositionWorld/Range/Color only.
    struct GPULight {
        DirectX::XMFLOAT3 PositionView;    // 0  (filled with tiling; world-space shading for now)
        float             Range;           // 12
        DirectX::XMFLOAT4 Color;           // 16 (.w = static flag 0/1)
        DirectX::XMFLOAT3 PositionWorld;   // 32
        int32_t           ShadowCubeIndex; // 44 (-1 = no shadow)
    };
    static_assert( sizeof( GPULight ) == 48, "GPULight must match D3D11 TiledPointLight (48 bytes)" );

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
    RenderView g_ShadowPassVobs[3]; // per shadow cascade
}


void D3D12GraphicsEngine::OnAddVob(VobInfo* vi) {
    auto [it, inserted] = g_vobInfoVisualToBucket.try_emplace(vi->VisualInfo);
    if (inserted) {
        // newly seen visual, add it to our vob instancing helpers
        it->second = static_cast<int16_t>(g_vobInfoVisualIndexToVisualInfo.size());
        g_vobInfoVisualIndexToVisualInfo.push_back(it->first);
        g_GeometryPassVobs.buckets.push_back({});
        
        for (auto& v : g_ShadowPassVobs) {
            v.buckets.push_back({});
        }
    }
    vi->VisualIndex = it->second;

    // A VOB added after a nearby point light already cached its static-aside shadow cube would otherwise cast no
    // point-light shadow: the static cube is only re-rendered when the light is fresh / moved / resized (the
    // renderStatic gate in BuildFramePointShadows), not when world geometry around it changes. Walk the active
    // shadow slots and invalidate any whose light range the new VOB reaches, forcing a one-time static re-render
    // next frame (staticValid=false → renderStatic). Slots are empty during world load (owner==nullptr) so this is
    // a no-op then; the margin mirrors the static-VOB gather's cull (ps.range + visual->MeshSize * 0.5f).
    if ( vi->Vob && vi->VisualInfo ) {
        const XMFLOAT3 p = vi->Vob->GetPositionWorld();
        const float extent = vi->VisualInfo->MeshSize * 0.5f;
        for ( PointShadowSlot& ss : m_PointShadowSlots ) {
            if ( !ss.owner || !ss.staticValid ) continue;
            const float r = ss.range + extent;
            const float dx = p.x - ss.pos.x, dy = p.y - ss.pos.y, dz = p.z - ss.pos.z;
            if ( dx * dx + dy * dy + dz * dz < r * r )
                ss.staticValid = false;   // re-render this slot's static-aside next frame to include the new VOB
        }
    }
}


XRESULT D3D12GraphicsEngine::OnVobRemovedFromWorld( zCVob* vob ) {
    // Symmetric to OnAddVob's static-cube invalidation: a VOB removed from the world must stop casting into any
    // point light's cached static-aside shadow cube. D3D11 keys this off each light's per-vob VobCache
    // (D3D11PointLight::OnVobRemovedFromWorld); D3D12 keeps no per-slot vob list, so test the removed vob's world
    // AABB against each active slot's light sphere (the same closest-point AABB test the static world-section cull
    // uses) and force a one-time static re-render (staticValid=false) for any slot it intersects. Over-invalidation
    // is harmless (one extra static pass); under-invalidation would leave the removed vob's shadow frozen in the
    // cache. Slots are empty during world load (owner==nullptr) so this is a no-op then.
    if ( vob ) {
        const zTBBox3D& bb = vob->GetBBox();
        for ( PointShadowSlot& ss : m_PointShadowSlots ) {
            if ( !ss.owner || !ss.staticValid ) continue;
            const float cx = std::min( std::max( ss.pos.x, bb.Min.x ), bb.Max.x );
            const float cy = std::min( std::max( ss.pos.y, bb.Min.y ), bb.Max.y );
            const float cz = std::min( std::max( ss.pos.z, bb.Min.z ), bb.Max.z );
            const float dx = ss.pos.x - cx, dy = ss.pos.y - cy, dz = ss.pos.z - cz;
            if ( dx * dx + dy * dy + dz * dz < ss.range * ss.range )
                ss.staticValid = false;   // removed vob was within this light's static coverage → re-cache
        }
    }
    return XR_SUCCESS;
}


void D3D12GraphicsEngine::OnLoadWorld()
{
    g_vobInfoVisualToBucket.clear();
    g_vobInfoVisualIndexToVisualInfo.clear();
    g_GeometryPassVobs.Reset();
    for (auto& v : g_ShadowPassVobs) {
        v.Reset();
    }
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




// Shared with D3D11 (ImGuiShim.cpp / D3D11ShadowMap.cpp): only these five power-of-two steps are offered/valid.
// 8192 is the hard ceiling for both backends — a single 16384 D32 cascade slice is ~1GB, not worth the VRAM.
UINT D3D12GraphicsEngine::ClampShadowMapSize( int desired ) {
	static constexpr int steps[] = { 512, 1024, 2048, 4096, 8192 };
	int clamped = std::clamp( desired, steps[0], steps[_countof( steps ) - 1] );
	int nearest = steps[0];
	int bestDist = std::abs( clamped - nearest );
	for ( int s : steps ) {
		int dist = std::abs( clamped - s );
		if ( dist < bestDist ) { bestDist = dist; nearest = s; }
	}
	return static_cast<UINT>(nearest);
}

bool D3D12GraphicsEngine::CreateShadowMapTextureAndViews( UINT size ) {
	// Builds/rebuilds just the sized GPU state: the resource + its per-cascade DSVs + the array SRV. Called once
	// from CreateShadowMap (after the DSV heap + SRV slot are allocated) and again from ResizeShadowMap whenever
	// the resolution setting changes — the heap/slot themselves don't depend on resolution, so they're untouched.
	ID3D12Device* device = m_Device.GetDevice();

	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = size;
	dd.Height = size;
	dd.DepthOrArraySize = static_cast<UINT16>(kShadowCascades);
	dd.MipLevels = 1;
	dd.Format = DXGI_FORMAT_R32_TYPELESS;   // D32 DSV per slice + one R32_FLOAT array SRV for the lit-pass sampler
	dd.SampleDesc.Count = 1;
	dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DXGI_FORMAT_D32_FLOAT;
	clear.DepthStencil.Depth = 1.0f;        // normal-Z: 1.0 == far (NOT reversed-Z)

	// Born in DEPTH_WRITE; each frame RenderSunShadows writes then transitions to PIXEL_SHADER_RESOURCE and back.
	m_ShadowMapAlloc.Reset();
	m_ShadowMap.Reset();
	if ( FAILED( m_Allocator->CreateResource( &allocDesc, &dd,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, m_ShadowMapAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_ShadowMap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_ShadowMap->SetName( L"SunShadowMap(D32 array)" );
	m_ShadowInPixelState = false;

	D3D12_CPU_DESCRIPTOR_HANDLE dsvH = m_ShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
	for ( UINT c = 0; c < kShadowCascades; ++c ) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = DXGI_FORMAT_D32_FLOAT;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsv.Texture2DArray.FirstArraySlice = c;
		dsv.Texture2DArray.ArraySize = 1;
		device->CreateDepthStencilView( m_ShadowMap.Get(), &dsv, dsvH );
		dsvH.ptr += m_ShadowDsvSize;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = DXGI_FORMAT_R32_FLOAT;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2DArray.MipLevels = 1;
	srv.Texture2DArray.ArraySize = kShadowCascades;
	device->CreateShaderResourceView( m_ShadowMap.Get(), &srv, GetSrvCpuHandle( m_ShadowSrvSlot ) );

	return true;
}

bool D3D12GraphicsEngine::ResizeShadowMap( UINT newSize ) {
	if ( newSize == m_ShadowMapSize || !m_ShadowDsvHeap || m_ShadowSrvSlot == UINT_MAX ) return false;

	// The old resource may still be read by in-flight command lists (lit passes sampling last frame's shadow
	// map), so stall the whole GPU before freeing it — this only happens on a settings change, never per-frame.
	WaitForGpuIdle();

	m_ShadowMapSize = newSize;
	if ( !CreateShadowMapTextureAndViews( m_ShadowMapSize ) ) return false;

	LogInfo() << "D3D12: shadow map resized to " << m_ShadowMapSize << "x" << m_ShadowMapSize;
	return true;
}

bool D3D12GraphicsEngine::CreateShadowMap() {
	// CSM sun shadow map (P2.9c-1): a Texture2DArray of kShadowCascades D32 slices + a caster PSO. Reuses the
	// depth-prepass world VS (b0 = a view-proj, t0 diffuse for alpha-clip) but with NORMAL-Z (LESS_EQUAL, clear
	// 1.0) state — the directional caster is NOT reversed-Z (mirrors the D3D11 shadow map). Created once at init
	// (fixed resolution, not swapchain-sized). Needs the depth-prepass shaders + m_Pipelines.World.RootSig to exist.
	ID3D12Device* device = m_Device.GetDevice();
	if ( !m_Pipelines.World.RootSig || !m_Pipelines.World.DepthPrepassVsBlob ) return false;

	// Resolution from the shared quality setting (same knob D3D11 uses), clamped to a sane range. Bigger = smaller
	// world-units/texel = far less sub-texel foliage flicker + tighter near shadows. DEFAULT-heap (GPU) memory, so
	// 8192 (~768MB across 3 D32 slices) barely touches the 32-bit CPU address space — it's all GPU-side.
	int desired = Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize;
	m_ShadowMapSize = ClampShadowMapSize( desired );

	// DSV heap: one D32 DSV per cascade slice. Descriptor COUNT never changes with resolution, so this heap is
	// allocated once here and reused as-is by ResizeShadowMap (only the underlying resource + its views change).
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = kShadowCascades;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( device->CreateDescriptorHeap( &dsvHeapDesc, IID_PPV_ARGS( m_ShadowDsvHeap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_ShadowDsvSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );

	// Array SRV (R32_FLOAT) covering all cascades — bound by the lit passes. The slot itself is permanent (bindless
	// index baked into shaders/CBs elsewhere); ResizeShadowMap just re-points it at the new resource.
	m_ShadowSrvSlot = AllocateSrvSlot();
	if ( m_ShadowSrvSlot == UINT_MAX ) return false;

	if ( !CreateShadowMapTextureAndViews( m_ShadowMapSize ) ) return false;

	// Caster PSO. Void PS (PSShadowClip) so no RTV is needed; front-face cull + slope-scaled depth bias fight
	// shadow acne (front-culling casts back faces, standard for opaque shadow maps).
	if ( !m_ShaderBackend.CompileFromFile( "DepthPrepass.hlsl", "PSShadowClip", Shadermodel_PS, m_ShadowCasterPsBlob.ReleaseAndGetAddressOf() ) )
		return false;

	const D3D12_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
	pso.pRootSignature = m_Pipelines.World.RootSig.Get();
	pso.VS = { m_Pipelines.World.DepthPrepassVsBlob->GetBufferPointer(), m_Pipelines.World.DepthPrepassVsBlob->GetBufferSize() };
	pso.PS = { m_ShadowCasterPsBlob->GetBufferPointer(), m_ShadowCasterPsBlob->GetBufferSize() };
	pso.InputLayout = { layout, _countof( layout ) };
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = 0;                    // depth-only shadow pass (no color target bound)
	pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	pso.SampleDesc.Count = 1;
	pso.SampleMask = UINT_MAX;
	pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;   // cast back faces
	pso.RasterizerState.DepthClipEnable = TRUE;
	pso.RasterizerState.DepthBias = 0;                   // normal-Z: positive bias pushes casters away from the light
	pso.RasterizerState.SlopeScaledDepthBias = 0.0f;
	pso.RasterizerState.DepthBiasClamp = 0.0f;
	pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
	pso.DepthStencilState.DepthEnable = TRUE;
	pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;   // normal-Z
	pso.DepthStencilState.StencilEnable = FALSE;
	if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_ShadowCasterWorldPSO.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: CreateGraphicsPipelineState failed (shadow caster).";
		return false;
	}

	// VOB caster PSO (P2.9c-2): reuse the VOB depth-prepass VSDepth (two-stream: packed vertex + per-instance
	// world matrix) + m_Pipelines.World.RootSig, with the same caster state (front cull, bias, LESS_EQUAL, no RTV). Also
	// used for node attachments (weapons/heads) which are packed vertex + instance like ordinary VOBs.
	if ( m_Pipelines.World.DepthPrepassVobVsBlob ) {
		if ( !m_ShaderBackend.CompileFromFile( "Vob.hlsl", "PSShadowClip", Shadermodel_PS, m_ShadowCasterVobPsBlob.ReleaseAndGetAddressOf() ) )
			return false;
		// Same VSDepth blob as the opaque depth prepass — it now unconditionally reads INSTANCE_WINDFLUENCE
		// (Vob.hlsl's ApplyVobWind), so this layout needs the element too (node attachments carry zeroes there,
		// a no-op; genuinely wind-flagged VOB casters need it for their shadow silhouette to sway like their lit
		// geometry — see the WindCB bind at the "Vobs" caster draw site below).
		const D3D12_INPUT_ELEMENT_DESC vobLayout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
			{ "INSTANCE_WINDFLUENCE",  0, DXGI_FORMAT_R32G32_FLOAT,       1, 132, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		};
		pso.pRootSignature = m_Pipelines.World.RootSig.Get();
		pso.VS = { m_Pipelines.World.DepthPrepassVobVsBlob->GetBufferPointer(), m_Pipelines.World.DepthPrepassVobVsBlob->GetBufferSize() };
		pso.PS = { m_ShadowCasterVobPsBlob->GetBufferPointer(), m_ShadowCasterVobPsBlob->GetBufferSize() };
		pso.InputLayout = { vobLayout, _countof( vobLayout ) };
		if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_ShadowCasterVobPSO.ReleaseAndGetAddressOf() ) ) ) ) {
			LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB shadow caster).";
			return false;
		}

		// Bindless-diffuse VOB shadow-caster PSO (ExecuteIndirect, P2.12): same VSDepth + wind-only vobLayout +
		// caster state as m_ShadowCasterVobPSO, only the void PS swapped to PSShadowClipBindless (diffuse alpha-clip
		// from the SRV heap). Lets each CSM cascade's instanced-VOB casters submit as one ExecuteIndirect. The blob
		// is local (needed only until PSO creation); the attach block below resets pso.VS/PS/layout for itself.
		ComPtr<ID3DBlob> vobIndirectShadowPs;
		if ( !m_ShaderBackend.CompileFromFile( "Vob.hlsl", "PSShadowClipBindless", Shadermodel_PS, vobIndirectShadowPs.ReleaseAndGetAddressOf() ) )
			return false;
		pso.PS = { vobIndirectShadowPs->GetBufferPointer(), vobIndirectShadowPs->GetBufferSize() };
		if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_ShadowCasterVobIndirectPSO.ReleaseAndGetAddressOf() ) ) ) ) {
			LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB shadow caster, indirect).";
			return false;
		}
	}

	// Node-attachment CSM caster variant (VSDepthAttach: Fatness/Scaling inflate-along-normal instead of wind —
	// see Vob.hlsl and World.VobAttachPSO/DepthPrepassVobAttachPSO). Needs NORMAL in the layout, unlike the
	// plain VOB caster above, so it reuses World.DepthPrepassVobAttachVsBlob (already compiled with that
	// layout in CreateWorld) rather than DepthPrepassVobVsBlob. Reuses PSShadowClip unchanged.
	if ( m_Pipelines.World.DepthPrepassVobAttachVsBlob && m_ShadowCasterVobPsBlob ) {
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
		pso.pRootSignature = m_Pipelines.World.RootSig.Get();
		pso.VS = { m_Pipelines.World.DepthPrepassVobAttachVsBlob->GetBufferPointer(), m_Pipelines.World.DepthPrepassVobAttachVsBlob->GetBufferSize() };
		pso.PS = { m_ShadowCasterVobPsBlob->GetBufferPointer(), m_ShadowCasterVobPsBlob->GetBufferSize() };
		pso.InputLayout = { vobAttachLayout, _countof( vobAttachLayout ) };
		if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_ShadowCasterVobAttachPSO.ReleaseAndGetAddressOf() ) ) ) ) {
			LogWarn() << "D3D12: CreateGraphicsPipelineState failed (VOB attachment shadow caster).";
			return false;
		}
	}

	// Skeletal caster PSO (P2.9c-2): reuse the skeletal depth-prepass VSDepth (matrix-palette skinning) +
	// m_Pipelines.Skeletal.RootSig + the skinned input layout, same caster state.
	if ( m_Pipelines.Skeletal.DepthPrepassVsBlob && m_Pipelines.Skeletal.RootSig ) {
		if ( !m_ShaderBackend.CompileFromFile( "Skeletal.hlsl", "PSShadowClip", Shadermodel_PS, m_ShadowCasterSkeletalPsBlob.ReleaseAndGetAddressOf() ) )
			return false;
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
		pso.pRootSignature = m_Pipelines.Skeletal.RootSig.Get();
		pso.VS = { m_Pipelines.Skeletal.DepthPrepassVsBlob->GetBufferPointer(), m_Pipelines.Skeletal.DepthPrepassVsBlob->GetBufferSize() };
		pso.PS = { m_ShadowCasterSkeletalPsBlob->GetBufferPointer(), m_ShadowCasterSkeletalPsBlob->GetBufferSize() };
		pso.InputLayout = { skelLayout, _countof( skelLayout ) };
		if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_ShadowCasterSkeletalPSO.ReleaseAndGetAddressOf() ) ) ) ) {
			LogWarn() << "D3D12: CreateGraphicsPipelineState failed (skeletal shadow caster).";
			return false;
		}
	}

	// Per-frame-in-flight shadow-sampling CB (b3 in the lit passes): cascade view-projs + sun dir + strength +
	// texel sizes. Small + written once per frame, so a persistently-mapped UPLOAD buffer per frame context
	// (no ring offset needed — one struct per frame). Filled in RenderSunShadows, bound by the lit draws.
	D3D12MA::ALLOCATION_DESC uploadAlloc = {};
	uploadAlloc.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC cbDesc = {};
	cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	// 512, not 256: the first 256B hold the cascade matrices + sun data (RenderSunShadows), the tail holds
	// the scene-wetness block (UploadWetnessConstants — written later in the frame, after the rain shadow
	// pass has its camera). Two disjoint byte ranges of the same CB, so neither write clobbers the other.
	cbDesc.Width = 512;
	cbDesc.Height = 1;
	cbDesc.DepthOrArraySize = 1;
	cbDesc.MipLevels = 1;
	cbDesc.Format = DXGI_FORMAT_UNKNOWN;
	cbDesc.SampleDesc.Count = 1;
	cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_Allocator->CreateResource( &uploadAlloc, &cbDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_ShadowCBAlloc[i].ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( m_ShadowCB[i].ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_ShadowCB[i]->SetName( L"ShadowSamplingCB" );
		D3D12_RANGE noRead = { 0, 0 };
		void* mapped = nullptr;
		if ( FAILED( m_ShadowCB[i]->Map( 0, &noRead, &mapped ) ) ) return false;
		m_ShadowCBMapped[i] = static_cast<uint8_t*>( mapped );
		m_ShadowCBGpu[i] = m_ShadowCB[i]->GetGPUVirtualAddress();
	}
	return true;
}


bool D3D12GraphicsEngine::CreateGrassShadowCaster() {
	// GVegetationBox grass CSM caster: reuses Grass.RootSig (b0 cascade view-proj VS, b1 GrassCB for the same
	// wind sway VSMain applies — so the shadow silhouette doesn't lag the swaying blades, mirrors D3D11's
	// VS_GrassInstancedShadow.hlsl; t0 grass texture for the alpha-clip) with a new depth-only VS/PS pair
	// (VSDepth/PSShadowClip in Vegetation.hlsl). Called from Init() AFTER m_Pipelines.CreateGrass() (needs
	// Grass.RootSig to exist) — unlike the world/VOB/skeletal casters built inside CreateShadowMap(), this one
	// doesn't touch any shadow-map GPU resource, just the DXGI_FORMAT_D32_FLOAT DSV format constant, so the
	// Init() ordering (CreateShadowMap runs before CreateGrass) doesn't matter here. Non-fatal: DrawVegetation's
	// shadow contribution is simply skipped (grass casts no shadow) if this fails.
	if ( !m_Pipelines.Grass.RootSig ) return false;

	if ( !m_ShaderBackend.CompileFromFile( "Vegetation.hlsl", "VSDepth", Shadermodel_VS, m_ShadowCasterGrassVsBlob.ReleaseAndGetAddressOf() ) )
		return false;
	if ( !m_ShaderBackend.CompileFromFile( "Vegetation.hlsl", "PSShadowClip", Shadermodel_PS, m_ShadowCasterGrassPsBlob.ReleaseAndGetAddressOf() ) )
		return false;

	// Slot 0 = SimpleObjectVertexStruct (Position@0, TexCoord@12); slot 1 = per-instance world matrix — identical
	// layout to Grass.PSO's (see CreateGrass), VSDepth just skips the lighting-only fields the color VS reads.
	const D3D12_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
	pso.pRootSignature = m_Pipelines.Grass.RootSig.Get();
	pso.VS = { m_ShadowCasterGrassVsBlob->GetBufferPointer(), m_ShadowCasterGrassVsBlob->GetBufferSize() };
	pso.PS = { m_ShadowCasterGrassPsBlob->GetBufferPointer(), m_ShadowCasterGrassPsBlob->GetBufferSize() };
	pso.InputLayout = { layout, _countof( layout ) };
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = 0;   // depth-only shadow pass (no color target bound)
	pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	pso.SampleDesc.Count = 1;
	pso.SampleMask = UINT_MAX;
	pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	// CULL_NONE (not FRONT like the opaque/VOB/skeletal casters above): grass cards are thin double-sided
	// planes — matches Grass.PSO's own culling (see CreateGrass), so both faces still cast into the map.
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso.RasterizerState.DepthClipEnable = TRUE;
	pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
	pso.DepthStencilState.DepthEnable = TRUE;
	pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;   // normal-Z, matches the other casters
	pso.DepthStencilState.StencilEnable = FALSE;

	ID3D12Device* device = m_Device.GetDevice();
	if ( FAILED( device->CreateGraphicsPipelineState( &pso, IID_PPV_ARGS( m_ShadowCasterGrassPSO.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: CreateGraphicsPipelineState failed (grass shadow caster).";
		return false;
	}
	return true;
}


bool D3D12GraphicsEngine::CreatePointShadowResources() {
	// P2.10a: the point-light shadow cube ARRAY GPU RESOURCES — the active + static-aside cube textures, their
	// per-slot 6-slice DSV heaps, the TextureCubeArray SRV, and the per-frame face-matrix CB + VOB-instance rings.
	// The caster PIPELINES (root sigs, shaders, PSOs) live in m_Pipelines.PointShadow (CreatePointShadow). Mirrors
	// D3D11's Forward+ TextureCubeArray (SHADOW_CUBE_SIZE 128, MAX_SHADOW_CUBEMAPS shared slots, R16 depth,
	// NORMAL-Z). Non-fatal at init: on failure the point lights simply stay unshadowed.
	ID3D12Device* device = m_Device.GetDevice();
	if ( !device ) return false;

	// --- Cube array resource: Texture2DArray with kMaxShadowCubes*6 R16 slices (interpreted as a TextureCubeArray
	// by the SRV). NORMAL-Z depth (clear 1.0, LESS_EQUAL). Born in DEPTH_WRITE (RenderPointShadows flips it to
	// PIXEL_SHADER_RESOURCE for the lit pass and back next frame).
	D3D12MA::ALLOCATION_DESC defaultAlloc = {};
	defaultAlloc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = kPointShadowCubeSize;
	dd.Height = kPointShadowCubeSize;
	dd.DepthOrArraySize = static_cast<UINT16>(kMaxShadowCubes * 6);
	dd.MipLevels = 1;
	dd.Format = DXGI_FORMAT_R16_TYPELESS;
	dd.SampleDesc.Count = 1;
	dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DXGI_FORMAT_D16_UNORM;
	clear.DepthStencil.Depth = 1.0f;
	if ( FAILED( m_Allocator->CreateResource( &defaultAlloc, &dd,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, m_PointShadowCubeAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_PointShadowCube.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_PointShadowCube->SetName( L"PointShadowCubeArray(D16)" );
	m_PointShadowInPixelState = false;

	// One DSV per cube slot: a 6-slice Texture2DArray view (FirstArraySlice = slot*6). SV_RenderTargetArrayIndex
	// 0..5 from the VS then selects the face within the bound slot.
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = kMaxShadowCubes;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( device->CreateDescriptorHeap( &dsvHeapDesc, IID_PPV_ARGS( m_PointShadowDsvHeap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_PointShadowDsvSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );
	D3D12_CPU_DESCRIPTOR_HANDLE dsvH = m_PointShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
	for ( UINT s = 0; s < kMaxShadowCubes; ++s ) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = DXGI_FORMAT_D16_UNORM;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsv.Texture2DArray.FirstArraySlice = s * 6;
		dsv.Texture2DArray.ArraySize = 6;
		device->CreateDepthStencilView( m_PointShadowCube.Get(), &dsv, dsvH );
		dsvH.ptr += m_PointShadowDsvSize;
	}

	// --- Static-aside cube (P2.10g): a SECOND identical cube array holding static-caster depth only. Same desc
	// (so CopyResource into the active cube is legal), born in DEPTH_WRITE, with its own per-slot 6-slice DSV heap.
	// No SRV — it's never sampled; its depth is copied into the active cube each frame before the dynamic overlay.
	if ( FAILED( m_Allocator->CreateResource( &defaultAlloc, &dd,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, m_PointShadowStaticCubeAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_PointShadowStaticCube.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_PointShadowStaticCube->SetName( L"PointShadowStaticCubeArray(D16)" );
	m_PointShadowStaticState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

	D3D12_DESCRIPTOR_HEAP_DESC staticDsvHeapDesc = {};
	staticDsvHeapDesc.NumDescriptors = kMaxShadowCubes;
	staticDsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( device->CreateDescriptorHeap( &staticDsvHeapDesc, IID_PPV_ARGS( m_PointShadowStaticDsvHeap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	D3D12_CPU_DESCRIPTOR_HANDLE sdsvH = m_PointShadowStaticDsvHeap->GetCPUDescriptorHandleForHeapStart();
	for ( UINT s = 0; s < kMaxShadowCubes; ++s ) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = DXGI_FORMAT_D16_UNORM;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsv.Texture2DArray.FirstArraySlice = s * 6;
		dsv.Texture2DArray.ArraySize = 6;
		device->CreateDepthStencilView( m_PointShadowStaticCube.Get(), &dsv, sdsvH );
		sdsvH.ptr += m_PointShadowDsvSize;
	}

	// TextureCubeArray SRV (R16_UNORM) over all cubes — sampled by the tiled point-light loop in P2.10d.
	m_PointShadowSrvSlot = AllocateSrvSlot();
	if ( m_PointShadowSrvSlot == UINT_MAX ) return false;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = DXGI_FORMAT_R16_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.TextureCubeArray.MipLevels = 1;
	srv.TextureCubeArray.NumCubes = kMaxShadowCubes;
	device->CreateShaderResourceView( m_PointShadowCube.Get(), &srv, GetSrvCpuHandle( m_PointShadowSrvSlot ) );

	// Per-frame ring for the face-matrix CB: one 512-byte (256-aligned; 6 matrices = 384B) slot per shadowed
	// light, so each light's cube draw binds its own root CBV without clobbering earlier same-frame draws.
	D3D12MA::ALLOCATION_DESC uploadAlloc = {};
	uploadAlloc.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC cbDesc = {};
	cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	cbDesc.Width = static_cast<UINT64>(kMaxShadowCubes) * 512;
	cbDesc.Height = 1;
	cbDesc.DepthOrArraySize = 1;
	cbDesc.MipLevels = 1;
	cbDesc.Format = DXGI_FORMAT_UNKNOWN;
	cbDesc.SampleDesc.Count = 1;
	cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_Allocator->CreateResource( &uploadAlloc, &cbDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_PointShadowCBAlloc[i].ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( m_PointShadowCB[i].ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_PointShadowCB[i]->SetName( L"PointShadowFaceCB" );
		D3D12_RANGE noRead = { 0, 0 };
		void* mapped = nullptr;
		if ( FAILED( m_PointShadowCB[i]->Map( 0, &noRead, &mapped ) ) ) return false;
		m_PointShadowCBMapped[i] = static_cast<uint8_t*>( mapped );
		m_PointShadowCBGpu[i] = m_PointShadowCB[i]->GetGPUVirtualAddress();
	}

	// Per-frame TIGHT VOB-instance ring for the point-shadow VOB caster (P2.10e). RenderPointShadows range-culls
	// each visible VOB's instances against every shadowed light and packs the in-range ones' 64-byte world matrix
	// here (only the near casters, not the whole visible set) — so the cube pass draws proportional to actual
	// nearby geometry. Persistently mapped UPLOAD; offset reset at the top of RenderPointShadows; drop+log on
	// overflow (never reallocates — see the 32-bit per-frame-allocation rule).
	m_PointShadowVobInstCapacity = static_cast<UINT>(kPointShadowMaxVobInstances) * sizeof( XMFLOAT4X4 );
	D3D12_RESOURCE_DESC viDesc = {};
	viDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	viDesc.Width = m_PointShadowVobInstCapacity;
	viDesc.Height = 1;
	viDesc.DepthOrArraySize = 1;
	viDesc.MipLevels = 1;
	viDesc.Format = DXGI_FORMAT_UNKNOWN;
	viDesc.SampleDesc.Count = 1;
	viDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_Allocator->CreateResource( &uploadAlloc, &viDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_PointShadowVobInstAlloc[i].ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( m_PointShadowVobInst[i].ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_PointShadowVobInst[i]->SetName( L"PointShadowVobInstRing" );
		m_PointShadowVobInstAlloc[i]->SetName( L"AllocPointShadowVobInstRing" );
		D3D12_RANGE noRead = { 0, 0 };
		void* mapped = nullptr;
		if ( FAILED( m_PointShadowVobInst[i]->Map( 0, &noRead, &mapped ) ) ) return false;
		m_PointShadowVobInstPtr[i] = static_cast<uint8_t*>( mapped );
		m_PointShadowVobInstGpu[i] = m_PointShadowVobInst[i]->GetGPUVirtualAddress();
	}
	return true;
}


void D3D12GraphicsEngine::ComputeCascadeMatrices() {
	using namespace DirectX;
	// P2.9c-3a: stable, frustum-fit + texel-snapped cascades — mirrors D3D11 CalculateCascadeMatrices
	// (D3D11ShadowMap.cpp). Per cascade: fit a bounding SPHERE to the camera's frustum SLICE [splitNear,splitFar]
	// (rotation-invariant → no shimmer from turning), quantize the radius, snap the sphere centre to the shadow
	// texel grid anchored at the world origin (→ no crawl when translating), pull the light back, and derive the
	// ortho Z bounds from the slice + the scene BBox. Replaces the old camera-centred concentric boxes.
	Engine::GAPI->GetSky()->RenderSky(); // <-- does not render, but calculates atmosphere data like AC_LightPos

	float3 lp = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos;
	XMVECTOR rawToSun = XMVector3Normalize( XMVectorSet( lp.x, lp.y, lp.z, 0.0f ) );
	// Temporal smoothing (P2.9c-3c), now driven by the same user-facing knobs D3D11 exposes
	// (settings.SmoothShadowCameraUpdate / SmoothShadowFrequency — see D3D11ShadowMap::CalculateTemporalInterpolatedPosition,
	// which this mirrors): ON lerps toward the live sun dir by a frequency-derived blend factor and then quantizes
	// the direction to discrete 1/frequency steps, so the origin-anchored snap grid rotates in locked steps instead
	// of jittering every frame (the lever arm from origin to a distant player turns tiny sun drift into visible
	// texel crawl — this is what fixes it, not just cosmetic smoothing). OFF tracks the live direction exactly
	// (real-time), trading that texel crawl for a shadow that never lags the sun.
	XMVECTOR toSun;
	const auto& shadowDirSettings = Engine::GAPI->GetRendererState().RendererSettings;
	if ( !m_SunDirInitialized ) {
		toSun = rawToSun;
		m_SunDirInitialized = true;
	} else if ( shadowDirSettings.SmoothShadowCameraUpdate ) {
		const float frequency = std::max( 1.0f, shadowDirSettings.SmoothShadowFrequency );
		const float blendFactor = std::clamp( frequency / 10000.0f, 0.001f, 0.5f );
		XMVECTOR blended = XMVector3Normalize( XMVectorLerp( XMLoadFloat3( &m_SmoothedSunDir ), rawToSun, blendFactor ) );
		XMVECTOR scale = XMVectorReplicate( frequency );
		XMVECTOR quantized = XMVectorRound( XMVectorMultiply( blended, scale ) );
		toSun = XMVector3Normalize( XMVectorDivide( quantized, scale ) );
	} else {
		toSun = rawToSun;
	}
	XMStoreFloat3( &m_SmoothedSunDir, toSun );
	XMStoreFloat3( &m_SunDirWS, toSun );   // world-space dir TOWARD the sun (for the lit-pass N.L term)
	const XMVECTOR lightDir = XMVectorNegate( toSun );   // sun -> scene (the caster's look direction)
	const XMVECTOR worldUp = XMVectorSet( 0, 1, 0, 0 );
	const XMVECTOR up = (fabsf( lp.y ) > 0.95f) ? XMVectorSet( 0, 0, 1, 0 ) : worldUp;

	// Camera basis for reconstructing world-space frustum-slice corners: inverse(view) is camera->world, and the
	// projection diagonal gives the half-angle scales (_11 = 1/tan(fovX/2), _22 = 1/tan(fovY/2)). GothicAPI's
	// getters are column-major but the proj DIAGONAL is transpose-invariant, so we read _11/_22 straight off it.
	const XMMATRIX viewStd = XMMatrixTranspose( Engine::GAPI->GetViewMatrixXM() );   // row-vector standard view
	const XMMATRIX invView = XMMatrixInverse( nullptr, viewStd );
	const XMFLOAT4X4& projCM = Engine::GAPI->GetProjectionMatrix();
	const float projXScale = projCM._11;
	const float projYScale = projCM._22;

	// Practical split scheme (blend of uniform + logarithmic), from shadowNear..shadowFar in world units.
	const float shadowNear = 15.0f;
	const float shadowFar = 8000 + (12000.0f * std::max( 0.1f, shadowDirSettings.WorldShadowRangeScale ));

	float lambda = 0.90f;
    switch ( m_ShadowMapSize ) {
    case 512:
        lambda = 0.95f;
        break;
    case 1024:
        lambda = 0.93f;
        break;
    case 4096:
        lambda = 0.88f;
        break;
    case 8192:
        lambda = 0.82f;
        break;
    }
    Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.Lambda = lambda;

	float splits[kShadowCascades + 1];
	splits[0] = shadowNear;
	splits[kShadowCascades] = shadowFar;
	for ( UINT i = 1; i < kShadowCascades; ++i ) {
		float p = static_cast<float>( i ) / static_cast<float>( kShadowCascades );
		float logS = shadowNear * powf( shadowFar / shadowNear, p );
		float uniS = shadowNear + (shadowFar - shadowNear) * p;
		splits[i] = uniS + lambda * (logS - uniS);
	}

	// Scene-BBox light-space Z extent (tightens the ortho depth so casters between the light and the slice are
	// captured without shooting miles past the level). Recomputed per cascade against that cascade's lightView.
	zTBBox3D sceneBox = {};
	bool haveScene = false;
	if ( auto wi = Engine::GAPI->GetLoadedWorldInfo() )
		if ( wi->BspTree && wi->BspTree->GetRootNode() ) { sceneBox = wi->BspTree->GetRootNode()->BBox3D; haveScene = true; }

	const float lightDotUp = std::max( fabsf( XMVectorGetX( XMVector3Dot( lightDir, worldUp ) ) ), 0.05f );
	const float dynamicPullback = std::clamp( 4000.0f / lightDotUp, 2000.0f, 15000.0f );

	for ( UINT c = 0; c < kShadowCascades; ++c ) {
		// 8 world-space corners of the camera frustum slice [splits[c], splits[c+1]].
		XMFLOAT3 corners[8];
		int ci = 0;
		for ( int f = 0; f < 2; ++f ) {
			float d = splits[c + f];
			float xe = d / projXScale, ye = d / projYScale;
			for ( int sy = -1; sy <= 1; sy += 2 )
				for ( int sx = -1; sx <= 1; sx += 2 ) {
					XMVECTOR vVS = XMVectorSet( sx * xe, sy * ye, d, 1.0f );
					XMStoreFloat3( &corners[ci++], XMVector3TransformCoord( vVS, invView ) );
				}
		}

		// Minimal bounding sphere of the slice: slide the centre along the near->far axis so near/far radii equal.
		XMVECTOR nearC = XMVectorZero(), farC = XMVectorZero();
		for ( int i = 0; i < 4; ++i ) nearC += XMLoadFloat3( &corners[i] );
		for ( int i = 4; i < 8; ++i ) farC += XMLoadFloat3( &corners[i] );
		nearC *= 0.25f; farC *= 0.25f;
		XMVECTOR axis = XMVectorSubtract( farC, nearC );
		float L = XMVectorGetX( XMVector3Length( axis ) );
		XMVECTOR viewDir = (L > 1e-4f) ? XMVectorScale( axis, 1.0f / L ) : lightDir;
		float nearRSq = XMVectorGetX( XMVector3LengthSq( XMVectorSubtract( XMLoadFloat3( &corners[0] ), nearC ) ) );
		float farRSq = XMVectorGetX( XMVector3LengthSq( XMVectorSubtract( XMLoadFloat3( &corners[4] ), farC ) ) );
		float optimalX = std::clamp( (L * L + farRSq - nearRSq) / std::max( 2.0f * L, 1e-4f ), 0.0f, L );
		XMVECTOR frustumCenter = XMVectorAdd( nearC, XMVectorScale( viewDir, optimalX ) );

		float radius = 0.0f;
		for ( int i = 0; i < 8; ++i )
			radius = std::max( radius, XMVectorGetX( XMVector3Length( XMVectorSubtract( XMLoadFloat3( &corners[i] ), frustumCenter ) ) ) );
		radius = std::ceil( radius * 16.0f ) / 16.0f;   // quantize → no micro-scaling from FOV/aspect rounding
		const float cascadeSize = radius * 2.0f;
		const float texelSize = cascadeSize / static_cast<float>( m_ShadowMapSize );
		m_CascadeTexelWorld[c] = texelSize;   // world units/texel → the lit-pass normal bias

		// Texel-snap the centre on a GLOBAL light-space grid anchored at the world origin (unmoving as the player
		// translates), then transform back to world.
		XMMATRIX gridView = XMMatrixLookToLH( XMVectorZero(), lightDir, up );
		XMVECTOR cLS = XMVector3TransformCoord( frustumCenter, gridView );
		float snapX = std::floor( XMVectorGetX( cLS ) / texelSize ) * texelSize;
		float snapY = std::floor( XMVectorGetY( cLS ) / texelSize ) * texelSize;
		XMVECTOR snappedLS = XMVectorSet( snapX, snapY, XMVectorGetZ( cLS ), 1.0f );
		XMVECTOR snappedWS = XMVector3TransformCoord( snappedLS, XMMatrixInverse( nullptr, gridView ) );

		const float pullBack = std::max( 10000.0f, radius * 2.0f );
		XMVECTOR lightPos = XMVectorSubtract( snappedWS, XMVectorScale( lightDir, pullBack ) );
		XMMATRIX lightView = XMMatrixLookToLH( lightPos, lightDir, up );

		// Ortho Z from the slice corners' light-space depth, widened by the dynamic (sun-angle) pullback and the
		// scene BBox so occluders above/behind the slice still lie within the depth range.
		float minZ = FLT_MAX, maxZ = -FLT_MAX;
		for ( int i = 0; i < 8; ++i ) {
			float z = XMVectorGetZ( XMVector3TransformCoord( XMLoadFloat3( &corners[i] ), lightView ) );
			minZ = std::min( minZ, z ); maxZ = std::max( maxZ, z );
		}
		float orthoNear = std::max( 1.0f, minZ - dynamicPullback );
		float orthoFar = maxZ + 5000.0f;
		if ( haveScene ) {
			const XMFLOAT3 sc[8] = {
				{ sceneBox.Min.x, sceneBox.Min.y, sceneBox.Min.z }, { sceneBox.Max.x, sceneBox.Min.y, sceneBox.Min.z },
				{ sceneBox.Min.x, sceneBox.Max.y, sceneBox.Min.z }, { sceneBox.Max.x, sceneBox.Max.y, sceneBox.Min.z },
				{ sceneBox.Min.x, sceneBox.Min.y, sceneBox.Max.z }, { sceneBox.Max.x, sceneBox.Min.y, sceneBox.Max.z },
				{ sceneBox.Min.x, sceneBox.Max.y, sceneBox.Max.z }, { sceneBox.Max.x, sceneBox.Max.y, sceneBox.Max.z } };
			float sMinZ = FLT_MAX, sMaxZ = -FLT_MAX;
			for ( int i = 0; i < 8; ++i ) {
				float z = XMVectorGetZ( XMVector3TransformCoord( XMLoadFloat3( &sc[i] ), lightView ) );
				sMinZ = std::min( sMinZ, z ); sMaxZ = std::max( sMaxZ, z );
			}
			orthoNear = std::min( orthoNear, sMinZ - 100.0f );
			orthoFar = std::min( orthoFar, sMaxZ + 500.0f );
		}
		orthoNear = std::max( 1.0f, orthoNear );
		if ( orthoFar <= orthoNear + 1.0f ) orthoFar = orthoNear + 1.0f;

		XMMATRIX proj = XMMatrixOrthographicLH( cascadeSize, cascadeSize, orthoNear, orthoFar );
		// Store (View*Proj)^T (see the c-1 convention note): our lightView/proj are standard row-vector matrices,
		// so we transpose the product to match the column-major bytes the caster VS + sampling PS read back.
		XMStoreFloat4x4( &m_CascadeViewProj[c], XMMatrixTranspose( XMMatrixMultiply( lightView, proj ) ) );

		m_CascadeFrustum[c].BuildOrthographic( lightView,
			cascadeSize,
			cascadeSize,
			orthoNear,
			orthoFar,
			Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendBack,
			Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendFront,
			Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendSide );
	}
}


void D3D12GraphicsEngine::RenderSunShadows() {
	// P2.9c-1/-2/-3b: render the opaque casters (world mesh + instanced VOBs + skinned skeletals + node
	// attachments) into each cascade slice from the sun's POV. Still PRODUCES the shadow map only — nothing
	// samples it yet, so the frame is visually unchanged; inspect the D32 Texture2DArray in RenderDoc (each slice
	// should show the scene depth from the sun angle, now including VOB/NPC silhouettes). Stable cascades + the
	// lit-pass PCF sampling are later increments. World-mesh/VOB casters are culled against the CASCADE frustum
	// per cascade (shadowSections / ctx.frustum = m_CascadeFrustum[c] below). Skeletal casters are now ALSO culled
	// per cascade (PrepareFrameSkeletals against the full registered vob list + m_CascadeFrustum[c], not the
	// player's view frustum) instead of reusing the main view's g_FrameSkelDraws/g_FrameAttachDraws wholesale —
	// a caster invisible to the player can still cast a visible shadow. Per-vob CB/attachment ring uploads stay
	// cached once per frame (g_SkelUploadCache) so this adds no redundant upload cost for casters already
	// prepared for the main view.
	//
	// Plan item #7 (MT cascades) split this into four phases so the two expensive per-cascade jobs — the BSP/vob
	// cull and the command recording — can run concurrently, one pool task per cascade:
	//   A) main thread: cascade matrices, the sampling CB, and everything IDENTICAL across cascades (the world-mesh
	//      caster set with its resolved bindless materials, the grass wind CB).
	//   B) per cascade, CONCURRENT (CullShadowCascade): frustum tests + CollectVisibleVobs + grass box cull.
	//   C) main thread: the steps that mutate Gothic state or a SHARED upload ring — the per-cascade VOB instance
	//      upload + indirect-arg build, and ONE multi-cascade skeletal preparation pass.
	//   D) per cascade, CONCURRENT (RecordShadowCascade): the actual draws, into one command list per cascade.
	// The whole MT path is gated on RendererSettings.ThreadedShadowCulling (shared with D3D11ShadowMap's own
	// cascade fan-out) and on the per-cascade command lists existing, so it degrades to the original serial
	// driver — same output, same order — whenever either is unavailable.
	if ( !m_FrameOpen || !m_ShadowMap || !m_ShadowCasterWorldPSO || !m_ShadowDsvHeap || !m_Pipelines.World.RootSig )
		return;

	// NOTE: no function-scope DX_ZONE here — the MT path closes and resubmits m_CmdList mid-function, which would
	// split a BeginEvent/EndEvent pair across two command lists. RecordShadowCascade emits its own per-cascade
	// markers instead (on whichever list it is recording into).

	// Return the map to DEPTH_WRITE if last frame's lit sampling left it in PIXEL_SHADER_RESOURCE.
	if ( m_ShadowInPixelState ) {
		auto toDepth = TransitionBarrier( m_ShadowMap.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE );
		m_CmdList->ResourceBarrier( 1, &toDepth );
		m_ShadowInPixelState = false;
	}

	ComputeCascadeMatrices();

	// Sun below the horizon → clear each slice to far (1.0 = unshadowed) and skip ALL casting.
	const float3 lp = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos;
	const bool sunUp = (lp.y > 0.0f);

	// Upload this frame's shadow-sampling CB (b3 for the lit passes): cascade view-projs + sun dir + darkening
	// strength + per-cascade texel size. Layout MUST match the HLSL ShadowCB (row-major matrices, HLSL packing).
	if ( m_ShadowCBMapped[m_FrameIndex] ) {
		// Layout MUST match the HLSL ShadowCB (256B, row-major matrices). Stage-2 PBR sun params come from the
		// shared RendererSettings (same knobs D3D11 feeds SQ_LightColor/SQ_ShadowStrength/SQ_*AOStrength from).
		struct ShadowCBData {
			XMFLOAT4X4 CascadeViewProj[kShadowCascades];
			XMFLOAT3   SunDirWS;          float ShadowMapSize;
			XMFLOAT3   SunColor;          float SunIntensity;
			XMFLOAT3   CascadeTexelWorld; float AmbientStrength;
			float ShadowAOStrength; float WorldAOStrength; float _pad0; float _pad1;
		} cb;
		// The scene-wetness tail is written at kWetnessCbOffset by UploadWetnessConstants; this head must end
		// exactly there or the two writes overlap (or leave a hole the HLSL ShadowCB doesn't expect).
		static_assert( sizeof( cb ) == kWetnessCbOffset, "ShadowCB head size must match the HLSL layout" );
		const auto& set = Engine::GAPI->GetRendererState().RendererSettings;
		for ( UINT c = 0; c < kShadowCascades; ++c ) cb.CascadeViewProj[c] = m_CascadeViewProj[c];
		cb.SunDirWS = m_SunDirWS;
		cb.ShadowMapSize = static_cast<float>( m_ShadowMapSize );
		cb.CascadeTexelWorld = XMFLOAT3( m_CascadeTexelWorld[0], m_CascadeTexelWorld[1], m_CascadeTexelWorld[2] );

		// Rain dims the sun toward RainSunLightStrength (parity with D3D11's SQ_LightColor.a lerp).
		const float rain = Engine::GAPI->GetRainFXWeight();
		const float sunStrength = set.SunLightStrength
			+ (set.RainSunLightStrength - set.SunLightStrength) * std::min( 1.0f, rain * 2.0f );

		// Ambient/sky strength (SQ_ShadowStrength). Night is a bit brighter than before (0.3 -> 0.5, per user)
		// so interiors aren't too dark after dusk; interiors also self-darken via baked vertLighting-as-AO.
		float ambient = sunUp ? set.ShadowStrength : set.ShadowStrength * 0.5f;

		// BSP-indoor override (parity with D3D11): interiors use a NEUTRAL white sun (no warm outdoor tint) at a
		// softened intensity (no hard raking sun through a cave), and worldAO fully tracks the baked light. We keep
		// a non-zero ambient (D3D11 zeroes it for G2 -> torch-only) so interiors that already look fine don't go dark.
		bool indoor = false;
		if ( auto* wi = Engine::GAPI->GetLoadedWorldInfo() )
			if ( wi->BspTree )
				indoor = (wi->BspTree->GetBspTreeMode() == zBSP_MODE_INDOOR);

		if ( indoor ) {
			cb.SunColor = XMFLOAT3( 1.0f, 1.0f, 1.0f );
			cb.SunIntensity = sunUp ? sunStrength * 0.5f : 0.0f;
			cb.AmbientStrength = ambient;
			cb.WorldAOStrength = 1.0f;
		} else {
			cb.SunColor = XMFLOAT3( set.SunLightColor.x, set.SunLightColor.y, set.SunLightColor.z );
			cb.SunIntensity = sunUp ? sunStrength : 0.0f;   // no direct sun when it's below the horizon
			cb.AmbientStrength = ambient;
			cb.WorldAOStrength = set.WorldAOStrength;
		}
		cb.ShadowAOStrength = set.ShadowAOStrength;
		memcpy( m_ShadowCBMapped[m_FrameIndex], &cb, sizeof( cb ) );
	}

	// --- Phase A (main thread): resolve everything that is shared by ALL cascades ------------------------
	// The world-mesh caster SET is per-frame, not per-cascade: the alpha/translucency filter and the bindless
	// diffuse index (which needs Gothic's opaque zCMaterial::GetAniTexture) come out the same for every cascade
	// — only the frustum test differs. Hoisting it here cuts that work to a third AND leaves CullShadowCascade
	// with nothing but bbox tests, which is what makes the per-cascade cull safe on a pool thread.
	MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
	D3D12VertexBuffer* vb = wm ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
	D3D12VertexBuffer* ib = wm ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;
	const bool haveWorld = vb && ib && vb->GetResource() && ib->GetResource()
		&& (ib->GetSizeInBytes() / sizeof( uint32_t )) > 0;

	g_ShadowWorldCasters.clear();
	if ( haveWorld && sunUp ) {
		const Frustum& unionShadowFrustum = m_CascadeFrustum[kShadowCascades - 1];
		static std::vector<WorldMeshSectionInfo*> shadowSections;
		shadowSections.clear();
		Engine::GAPI->CollectVisibleSections( shadowSections, &unionShadowFrustum, false );

		for ( WorldMeshSectionInfo* section : shadowSections ) {
			if ( !section ) continue;
			for ( auto const& [meshKey, mesh] : section->WorldMeshes ) {
				if ( !mesh || mesh->Indices.empty() ) continue;
				if ( meshKey.Info && meshKey.Info->MaterialType != MaterialInfo::MT_None ) continue;

				// Skip translucent / blended geometry in shadow maps
				if ( (meshKey.Material->GetAlphaFunc() > zMAT_ALPHA_FUNC_NONE &&
					meshKey.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST)
					|| (meshKey.Material->GetAlphaFunc() == 0 && zColor( meshKey.Material->GetColor() ).bgra.alpha < 255) ) {
					continue;
				}

				// Resolve the bindless diffuse index for PSShadowClip's alpha cutout.
				zCTexture* tex = meshKey.Material->GetAniTexture();
				uint32_t diffuseIdx = m_BlackTexture->GetSrvSlot();
				if ( tex && tex->GetCacheState() == zRES_CACHED_IN ) {
					if ( MyDirectDrawSurface7* s = tex->GetSurface() ) {
						if ( GfxTexture* gfx = s->GetEngineTexture() ) {
							D3D12Texture* d = D3D12Texture::From( gfx );
							if ( d->HasSRV() ) diffuseIdx = d->GetSrvSlot();
						}
					}
				}

				g_ShadowWorldCasters.push_back( { mesh, diffuseIdx,
					static_cast<UINT>( mesh->Indices.size() ), mesh->BaseIndexLocation } );
			}
		}
	}

	// Grass caster wind constants — mirrors GVegetationBox::PopulateConstantBuffer (see DrawVegetation); only the
	// fields VSDepth's wind sway reads matter for a caster. Computed once here (Gothic reads) so the per-cascade
	// recorders never touch the engine for it.
	const auto& rsA = Engine::GAPI->GetRendererState().RendererSettings;
	g_ShadowGrassCB = {};
	g_ShadowGrassCB.Time = Engine::GAPI->GetTimeSeconds();
	g_ShadowGrassCB.WindStrength = rsA.WindQuality > 0 ? rsA.GlobalWindStrength : 0.0f;
	if ( rsA.HeroAffectsObjects ) {
		g_ShadowGrassCB.PlayerPosWS = Engine::GAPI->GetPlayerVob() ? Engine::GAPI->GetPlayerVob()->GetPositionWorld() : XMFLOAT3( 0, 0, 0 );
		g_ShadowGrassCB.HeroAffectStrength = 1.0f;
	}

	// MT path requires the per-cascade allocator/list pairs AND the shared ThreadedShadowCulling toggle (the same
	// switch D3D11ShadowMap uses for its own cascade fan-out, so the maintainer can A/B both backends from the
	// one ImGui checkbox / ini key). Any of those missing → the original single-threaded driver below.
	// (Also requires sunUp: with the sun down there is nothing to cull or draw, only three DSV clears, which are
	// cheaper to record inline than to fan out and resubmit m_CmdList for.)
	const bool threadedCascades = sunUp
		&& m_CascadeCmdListsReady
		&& rsA.ThreadedShadowCulling
		&& Engine::RenderingThreadPool != nullptr;

	if ( !sunUp ) {
		// Nothing casts; each cascade still gets its slice cleared to far (= unshadowed) in Phase D.
		for ( UINT c = 0; c < kShadowCascades; ++c ) {
			m_ShadowWorldDrawCount[c] = 0;
			m_ShadowVobDrawCount[c] = 0;
			g_ShadowGrassBoxes[c].clear();
			g_ShadowSkelDraws[c].clear();
			g_ShadowAttachDraws[c].clear();
		}
	} else {
		// --- Phase B: per-cascade culling, concurrent when enabled ---------------------------------------
		if ( threadedCascades ) {
			std::array<std::future<void>, kShadowCascades> jobs;
			for ( UINT c = 0; c < kShadowCascades; ++c ) {
				jobs[c] = Engine::RenderingThreadPool->enqueue(
					[]( const std::stop_token& token, D3D12GraphicsEngine* self, UINT cascade ) {
						if ( token.stop_requested() ) return;
						ZoneScopedN( "Cull shadow cascade" );
						self->CullShadowCascade( cascade );
					}, this, c ).future;
			}
			for ( auto& j : jobs ) if ( j.valid() ) j.get();
		} else {
			for ( UINT c = 0; c < kShadowCascades; ++c )
				CullShadowCascade( c );
		}

		// --- Phase C (main thread): everything that mutates Gothic state or writes a SHARED upload ring ---
		// The VOB instance ring and the indirect-arg build both bump m_VobInstanceBufferOffset / call
		// zCTexture::CacheIn, so they stay serial; they are cheap next to the BSP walk Phase B just parallelized.
		static std::vector<FrameVobUpload> cascadeUploads;
		for ( UINT c = 0; c < kShadowCascades; ++c ) {
			m_ShadowVobDrawCount[c] = 0;
			cascadeUploads.clear();
			if ( !UploadVobs( g_ShadowPassVobs[c].buckets, cascadeUploads ) ) continue;
			// GPU-driven VOB casters (P2.12): build this cascade's command set from the uploads (diffuse-only
			// material resolution — the void PSShadowClipBindless just alpha-clips), submitted as ONE
			// ExecuteIndirect in Phase D. Same command signature/PSO family as the main-view VOB pass; the
			// per-command b4 min/max makes wind-flagged casters sway their silhouette identically to their lit
			// geometry (VSDepth reads b4 unconditionally).
			if ( !m_ShadowCasterVobIndirectPSO || !m_VobIndirectCmdSig || !m_ShadowVobDrawArgsPtr[c][m_FrameIndex] )
				continue;
			// culled=false: the cascades CPU-cull against their own frustum (a caster invisible to the player can
			// still cast into view), so they draw the uncompacted ring with the CPU's instance counts.
			m_ShadowVobDrawCount[c] = BuildVobDrawCommands( cascadeUploads, m_ShadowVobDrawArgsPtr[c][m_FrameIndex], false,
				kMaxShadowVobDrawCommands, false );
		}

		// Skeletal shadow casters (parity with D3D11's Shadows::DrawSkeletalMeshes): cull the FULL registered
		// skeletal-vob list against the cascade frusta, not the player's view frustum — a caster invisible to the
		// player can still cast a visible shadow. ONE multi-cascade pass now (cascadeCount = kShadowCascades)
		// instead of one pass per cascade: the per-vob upload was already cached across passes (g_SkelUploadCache),
		// but the list walk, the distance cull and the Gothic animation/texani/morph work were not. It also has to
		// be a single MAIN-THREAD pass — all of that mutates Gothic state and the skeletal CB / VOB instance rings.
		for ( UINT c = 0; c < kShadowCascades; ++c ) { g_ShadowSkelDraws[c].clear(); g_ShadowAttachDraws[c].clear(); }
		PrepareFrameSkeletals( Engine::GAPI->GetSkeletalMeshVobs(), &m_CascadeFrustum[0], 0, nullptr, 0.0f, kShadowCascades );
	}

	// --- Phase D: command recording ---------------------------------------------------------------------
	if ( threadedCascades ) {
		// The cascade lists have to land on the direct queue BETWEEN what is already recorded into m_CmdList this
		// frame (OnBeginFrame's clears, DrawSky) and everything recorded after this call (the lit passes that
		// sample the finished map), so close+submit m_CmdList here and reopen it on the same frame allocator.
		// Queue order ends up [m_CmdList part A][cascade 0..N-1][m_CmdList part B], so the transition back to
		// PIXEL_SHADER_RESOURCE at the bottom of this function still executes after every slice is written.
		SubmitRecordedCommandsAndReopen();

		std::array<std::future<void>, kShadowCascades> jobs;
		std::array<bool, kShadowCascades> recorded = {};
		for ( UINT c = 0; c < kShadowCascades; ++c ) {
			jobs[c] = Engine::RenderingThreadPool->enqueue(
				[]( const std::stop_token& token, D3D12GraphicsEngine* self, UINT cascade, bool up, bool* okOut ) {
					if ( token.stop_requested() ) return;
					ZoneScopedN( "Record shadow cascade" );
					ID3D12CommandAllocator* alloc = self->m_CascadeCmdAllocators[cascade][self->m_FrameIndex].Get();
					ID3D12GraphicsCommandList* cl = self->m_CascadeCmdLists[cascade][self->m_FrameIndex].Get();
					if ( !alloc || !cl ) return;
					// Safe without a GPU wait: this (cascade, frame-in-flight) pair was last used
					// kBackBufferCount frames ago and Present() already fenced on that frame.
					if ( FAILED( alloc->Reset() ) ) return;
					if ( FAILED( cl->Reset( alloc, nullptr ) ) ) return;
					ResetCpuContextTracker();   // per-thread breadcrumb ring — see D3D12EngineCommon.h
					self->RecordShadowCascade( cascade, cl, up );
					// Only a successfully closed list may be executed; a failed Close leaves it unusable.
					*okOut = SUCCEEDED( cl->Close() );
				}, this, c, sunUp, &recorded[c] ).future;
		}
		for ( auto& j : jobs ) if ( j.valid() ) j.get();

		ID3D12CommandList* lists[kShadowCascades] = {};
		UINT numLists = 0;
		for ( UINT c = 0; c < kShadowCascades; ++c )
			if ( recorded[c] ) lists[numLists++] = m_CascadeCmdLists[c][m_FrameIndex].Get();
		if ( numLists > 0 )
			m_Device.GetDirectQueue()->ExecuteCommandLists( numLists, lists );
		if ( numLists != kShadowCascades && !m_CascadeRecordFailureLogged ) {
			m_CascadeRecordFailureLogged = true;   // log once, not once per frame
			LogWarn() << "D3D12: " << (kShadowCascades - numLists)
				<< " shadow cascade(s) failed to record — those cascades keep last frame's depth.";
		}
	} else {
		for ( UINT c = 0; c < kShadowCascades; ++c )
			RecordShadowCascade( c, m_CmdList.Get(), sunUp );
	}

	// Hand the whole array to PIXEL_SHADER_RESOURCE for the lit-pass PCF sampling; reverted at the top of next
	// frame's shadow pass. Also re-binds the main RT/DSV so subsequent passes draw to the scene-color target.
	auto toSrv = TransitionBarrier( m_ShadowMap.Get(),
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	m_CmdList->ResourceBarrier( 1, &toSrv );
	m_ShadowInPixelState = true;

	// Restore the HDR scene-color RT (+ shared depth) for the lit passes that follow — the world pass renders
	// into the HDR target, not the swapchain (Phase 3); the tonemap resolve composites it at the end of the frame.
	D3D12_CPU_DESCRIPTOR_HANDLE mainDsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, m_DepthBuffer ? &mainDsv : nullptr );
}


D3D12_GPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::ResolveShadowDiffuseSrv( zCTexture* tex ) const {
	if ( tex && tex->GetCacheState() == zRES_CACHED_IN ) {
		if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
			if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
				D3D12Texture* d12 = D3D12Texture::From( gfx );
				if ( d12->HasSRV() ) return d12->GetSrvGpuHandle();
			}
		}
	}
	return GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
}


void D3D12GraphicsEngine::CullShadowCascade( UINT cascade ) {
	// One cascade's culling. Pool-thread safe BY CONSTRUCTION: it writes only this cascade's own state
	// (m_ShadowWorldDrawArgsPtr[c][frame] + m_ShadowWorldDrawCount[c], g_ShadowPassVobs[c],
	// g_ShadowGrassBoxes[c]) and otherwise only reads data that is immutable for the frame. Everything that
	// mutated Gothic state or a SHARED ring was hoisted out of here: the world-mesh material resolution into
	// Phase A (g_ShadowWorldCasters), the VOB instance upload + indirect-arg build and the whole skeletal
	// preparation into Phase C. What remains is frustum tests, per-cascade UPLOAD-ring writes, and
	// CollectVisibleVobs — which D3D11ShadowMap already fans out identically under ThreadedShadowCulling
	// (BspTreeVobVisitor's per-visitor atomic seen-bit on VobInfo::VisibleInRenderPass is what makes the BSP
	// walk re-entrant, and the shadow config sets CollectLights=false, so the one genuinely mutating branch in
	// CollectLeafVobs — lazily allocating a VobLightInfo — is never reached).
	const UINT c = cascade;
	const Frustum& frustum = m_CascadeFrustum[c];

	// --- World mesh: bbox-test the pre-resolved caster set into this cascade's ExecuteIndirect arg buffer ---
	m_ShadowWorldDrawCount[c] = 0;
	if ( uint8_t* argPtr = m_ShadowWorldDrawArgsPtr[c][m_FrameIndex] ) {
		WorldDrawCommand* cmds = reinterpret_cast<WorldDrawCommand*>( argPtr );
		UINT drawCount = 0;
		const uint32_t defaultOrm = m_DefaultOrmTexture->GetSrvSlot();
		for ( const ShadowWorldCaster& caster : g_ShadowWorldCasters ) {
			if ( !Engine::GAPI->IsWorldMeshVisibleInFrustum( caster.mesh, frustum ) ) continue;
			if ( drawCount >= kMaxWorldDrawCommands ) break;

			WorldDrawCommand& cmd = cmds[drawCount++];
			cmd.MatNormalIndex = 0xFFFFFFFFu;
			cmd.MatOrmIndex = defaultOrm;
			cmd.MatDiffuseIndex = caster.diffuseIdx;
			// m_WorldIndirectCmdSig pushes FOUR b6 constants, so this one has to be written too — the caster PS
			// (PSShadowClip) never reads it, but leaving it unwritten put a stale ring value into a root constant.
			cmd.MatNormalStrength = 0.0f;
			cmd.Draw.IndexCountPerInstance = caster.indexCount;
			cmd.Draw.InstanceCount = 1;
			cmd.Draw.StartIndexLocation = caster.startIndex;
			cmd.Draw.BaseVertexLocation = 0;
			cmd.Draw.StartInstanceLocation = 0;
		}
		m_ShadowWorldDrawCount[c] = drawCount;
	}

	// --- Instanced VOBs: collect this cascade's visible set. The instance-ring upload + indirect-arg build
	// happen serially in Phase C (they share m_VobInstanceBufferOffset and CacheIn textures). ---
	const auto& rs = Engine::GAPI->GetRendererState().RendererSettings;
	const float shadowDistance = 8000 + (12000.0f * std::max( 0.1f, rs.WorldShadowRangeScale ));

	// thread_local, not plain locals: these scratch lists would otherwise re-allocate every cascade every frame.
	// (CollectMobs is false below so cascadeMobs stays empty; nopTransparency does receive the alpha-blended
	// vobs CVVH_AddNotDrawnVobToList peels off, which the shadow pass simply discards.)
	thread_local std::vector<SkeletalVobInfo*> cascadeMobs;
	thread_local std::vector<TransparencyVobInfo> nopTransparency;
	thread_local std::vector<VobLightInfo*> nopLights;
	cascadeMobs.clear(); nopTransparency.clear(); nopLights.clear();

	g_ShadowPassVobs[c].Reset();

	D3D12RenderQueue queue( &g_ShadowPassVobs[c], &cascadeMobs, &nopTransparency, &nopLights );
	RndCullContext ctx;
	ctx.queue = &queue;
	ctx.frustum = frustum;
	ctx.cameraPosition = Engine::GAPI->GetCameraPosition();
	ctx.stage = RenderStage::STAGE_DRAW_SHADOWS;
	ctx.drawDistances.OutdoorVobs = std::max( 20000.0f, shadowDistance );
	ctx.drawDistances.OutdoorVobsSmall = std::max( 20000.0f, shadowDistance );
	ctx.drawDistances.IndoorVobs = std::max( 20000.0f, shadowDistance );
	ctx.drawDistances.VisualFX = 0.0f;
	ctx.drawDistancesSq.OutdoorVobs = ctx.drawDistances.OutdoorVobs * ctx.drawDistances.OutdoorVobs;
	ctx.drawDistancesSq.OutdoorVobsSmall = ctx.drawDistances.OutdoorVobsSmall * ctx.drawDistances.OutdoorVobsSmall;
	ctx.drawDistancesSq.IndoorVobs = ctx.drawDistances.IndoorVobs * ctx.drawDistances.IndoorVobs;
	ctx.drawDistancesSq.VisualFX = 0.0f;

	ctx.drawFlags.DrawVOBs = rs.DrawVOBs;
	ctx.drawFlags.DrawMobs = rs.DrawMobs;
	ctx.drawFlags.EnableDynamicLighting = rs.EnableDynamicLighting;
	ctx.drawFlags.EnableOcclusionCulling = false; // shadows do not use the players view frustum for culling, so occlusion culling would be inaccurate and cause popping.
	ctx.drawFlags.CullVobs = rs.DebugSettings.Culling.CullVobs;
	ctx.drawFlags.CollectIndoorVobs = false;
	ctx.drawFlags.CollectMobs = false;
	ctx.drawFlags.CollectLights = false;

	Engine::GAPI->CollectVisibleVobs( ctx ); // uses rendercontext and does not mutate objects.

	// --- GVegetationBox grass casters: mirrors GVegetationBox::RenderVegetationShadow, culling each box against
	// THIS cascade's frustum (not the player's view frustum, like the VOB/skeletal casters above). Boxes are few,
	// so there is no per-instance CPU cost here like the VOB/skeletal culling has. ---
	g_ShadowGrassBoxes[c].clear();
	if ( m_ShadowCasterGrassPSO && m_Pipelines.Grass.RootSig ) {
		for ( GVegetationBox* box : Engine::GAPI->GetVegetationBoxes() ) {
			if ( !box || box->GetSpotCount() == 0 ) continue;
			XMFLOAT3 bbMin, bbMax;
			box->GetBoundingBox( &bbMin, &bbMax );
			if ( !frustum.Intersects( zTBBox3D{ bbMin, bbMax } ) ) continue;
			g_ShadowGrassBoxes[c].push_back( box );
		}
	}
}


void D3D12GraphicsEngine::RecordShadowCascade( UINT cascade, ID3D12GraphicsCommandList* cmdList, bool sunUp ) {
	// Issues one cascade's caster draws into the command list it is handed (m_CmdList on the serial path, that
	// cascade's own list on the MT path). Pool-thread safe for the same reason CullShadowCascade is: it reads
	// ONLY per-cascade state and values already resolved on the main thread — the arg buffers + counts, the
	// pre-culled grass box list, the skeletal records with their MAIN-THREAD-resolved diffuse handles
	// (g_SkelMatSrvs / FrameAttachDraw::srv). No Gothic mutation, no UpdateMeshLibTexAniState, no ring writes.
	if ( !cmdList || !m_ShadowDsvHeap ) return;
	const UINT c = cascade;

	D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_ShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
	dsv.ptr += static_cast<SIZE_T>( c ) * m_ShadowDsvSize;

	// A freshly-Reset command list carries no descriptor heap. On the serial path m_CmdList already has the same
	// heap bound, so re-binding is a no-op — hence unconditional rather than branched on the caller.
	if ( m_SrvHeap ) {
		ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
		cmdList->SetDescriptorHeaps( 1, heaps );
	}

	DX_ZONE( cmdList, "Sun Shadow Cascade" );

	cmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );   // DSV stays bound across the PSO switches below
	cmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );   // normal-Z far
	// Sun below the horizon → the slice stays cleared to far, i.e. fully unshadowed, and nothing casts.
	if ( !sunUp ) return;

	// Resolved once: GetSrvGpuHandle takes m_SrvHeapMutex and linear-scans the free-slot list, and all three
	// cascade recorders would otherwise hit it per material.
	const D3D12_GPU_DESCRIPTOR_HANDLE blackSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );

	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_ShadowMapSize), static_cast<float>(m_ShadowMapSize), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, static_cast<LONG>(m_ShadowMapSize), static_cast<LONG>(m_ShadowMapSize) };
	cmdList->RSSetViewports( 1, &vp );
	cmdList->RSSetScissorRects( 1, &sc );
	cmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
	D3D12VertexBuffer* vb = wm ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
	D3D12VertexBuffer* ib = wm ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;

	// --- World mesh (root sig: m_Pipelines.World.RootSig; b0 = cascade view-proj; b6 bindless material) ---
	if ( m_ShadowWorldDrawCount[c] > 0 && vb && ib && m_ShadowWorldDrawArgs[c][m_FrameIndex] ) {
		DX_ZONE( cmdList, "World Mesh" );

		cmdList->SetPipelineState( m_ShadowCasterWorldPSO.Get() );
		cmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
		cmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );

		const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
		const D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
		cmdList->IASetVertexBuffers( 0, 1, &vbv );
		cmdList->IASetIndexBuffer( &ibv );

		cmdList->ExecuteIndirect( m_WorldIndirectCmdSig.Get(), m_ShadowWorldDrawCount[c],
			m_ShadowWorldDrawArgs[c][m_FrameIndex].Get(), 0, nullptr, 0 );
	}

	// --- Instanced VOBs: one ExecuteIndirect over the command set Phase C built for this cascade ---
	if ( m_ShadowVobDrawCount[c] > 0 && m_ShadowCasterVobIndirectPSO && m_VobIndirectCmdSig
		&& m_ShadowVobDrawArgs[c][m_FrameIndex] ) {
		DX_ZONE( cmdList, "Vobs" );
		cmdList->SetPipelineState( m_ShadowCasterVobIndirectPSO.Get() );
		cmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
		cmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
		cmdList->SetGraphicsRoot32BitConstants( 11, 12, &m_WindBuffer, 0 );   // b4 frame-global wind baseline
		cmdList->ExecuteIndirect( m_VobIndirectCmdSig.Get(), m_ShadowVobDrawCount[c],
			m_ShadowVobDrawArgs[c][m_FrameIndex].Get(), 0, nullptr, 0 );
	}

	// --- Skinned skeletals (root sig: m_Pipelines.Skeletal.RootSig; b0 cascade view-proj, b1 instance, b2 bones) ---
	if ( m_ShadowCasterSkeletalPSO && m_Pipelines.Skeletal.RootSig && !g_ShadowSkelDraws[c].empty() ) {
		DX_ZONE( cmdList, "Skeletals" );

		cmdList->SetPipelineState( m_ShadowCasterSkeletalPSO.Get() );
		cmdList->SetGraphicsRootSignature( m_Pipelines.Skeletal.RootSig.Get() );
		cmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
		for ( const FrameSkelDraw& d : g_ShadowSkelDraws[c] ) {
			if ( !d.visual ) continue;
			// Shared per-MODEL texture slots: the alpha-clip diffuse for each of this instance's materials was
			// snapshotted on the main thread right after ITS UpdateMeshLibTexAniState (see
			// [[skeletal-texani-shared-slots]] and g_SkelMatSrvs) — calling it here would both be wrong for a
			// second instance of the same model and unsafe from a pool thread.
			const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>* matSrvs =
				(d.matSrvIndex < g_SkelMatSrvCount) ? &g_SkelMatSrvs[d.matSrvIndex] : nullptr;

			cmdList->SetGraphicsRootConstantBufferView( 1, d.instCb );
			cmdList->SetGraphicsRootConstantBufferView( 2, d.boneCb );
			size_t matIdx = 0;
			for ( auto const& [mat, meshList] : d.visual->SkeletalMeshes ) {
				const D3D12_GPU_DESCRIPTOR_HANDLE srv = (matSrvs && matIdx < matSrvs->size())
					? (*matSrvs)[matIdx] : blackSrv;
				++matIdx;
				cmdList->SetGraphicsRootDescriptorTable( 3, srv );
				for ( auto const& mesh : meshList ) {
					if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer ) continue;
					D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
					D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
					if ( !mvb->GetResource() || !mib->GetResource() ) continue;
					const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExSkelVertexStruct ) };
					cmdList->IASetVertexBuffers( 0, 1, &vbv );
					const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
					cmdList->IASetIndexBuffer( &ibv );
					cmdList->DrawIndexedInstanced( static_cast<UINT>(mesh->Indices.size()), 1, 0, 0, 0 );
				}
			}
		}
	}

	// --- Node attachments (weapons/heads) through the VOB caster PSO (packed vertex + single instance) ---
	if ( m_ShadowCasterVobAttachPSO && m_Pipelines.World.RootSig && !g_ShadowAttachDraws[c].empty() ) {
		DX_ZONE( cmdList, "Skeletal Nodes" );

		// Attachment variant (Fatness/Scaling instead of wind, needs NORMAL) — must match the depth prepass/
		// color pass PSO choice for the same reason the wind fix required it (bit-identical transform).
		cmdList->SetPipelineState( m_ShadowCasterVobAttachPSO.Get() );
		cmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
		cmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
		for ( const FrameAttachDraw& a : g_ShadowAttachDraws[c] ) {
			if ( !a.mesh || !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() ) continue;
			D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
			D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
			if ( !mvb->GetResource() || !mib->GetResource() ) continue;
			cmdList->SetGraphicsRootDescriptorTable( 1, a.srv );   // resolved on the main thread, see FrameAttachDraw
			const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
			const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, a.instView };
			cmdList->IASetVertexBuffers( 0, 2, views );
			const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
			cmdList->IASetIndexBuffer( &ibv );
			cmdList->DrawIndexedInstanced( static_cast<UINT>(a.mesh->Indices.size()), 1, 0, 0, 0 );
		}
	}

	// --- GVegetationBox grass (own root sig: b0 cascade view-proj, b1 GrassCB for the same wind sway VSMain
	// applies, t0 grass texture for the alpha-clip) — CULL_NONE caster, see CreateGrassShadowCaster. The boxes
	// were culled against this cascade's frustum in CullShadowCascade; the CB was filled in Phase A. ---
	if ( !g_ShadowGrassBoxes[c].empty() && m_ShadowCasterGrassPSO && m_Pipelines.Grass.RootSig ) {
		DX_ZONE( cmdList, "Grass" );

		bool grassBound = false;
		for ( GVegetationBox* box : g_ShadowGrassBoxes[c] ) {
			GMeshSimple* mesh = box->GetVegetationMesh();
			GfxVertexBuffer* instBuf = box->GetInstancingBuffer();
			if ( !mesh || !instBuf ) continue;
			D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->GetVertexBuffer() );
			D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->GetIndexBuffer() );
			D3D12VertexBuffer* mib_inst = D3D12VertexBuffer::From( instBuf );
			if ( !mvb || !mib || !mib_inst || !mvb->GetResource() || !mib->GetResource() || !mib_inst->GetResource() ) continue;

			if ( !grassBound ) {
				grassBound = true;
				cmdList->SetPipelineState( m_ShadowCasterGrassPSO.Get() );
				cmdList->SetGraphicsRootSignature( m_Pipelines.Grass.RootSig.Get() );
				cmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
				cmdList->SetGraphicsRoot32BitConstants( 3, 8, &g_ShadowGrassCB, 0 );   // b1 GrassCB (wind sway)
			}

			D3D12_GPU_DESCRIPTOR_HANDLE grassSrv = blackSrv;
			if ( GfxTexture* grassTex = box->GetVegetationTexture() ) {
				D3D12Texture* d12 = D3D12Texture::From( grassTex );
				if ( d12 && d12->HasSRV() ) grassSrv = d12->GetSrvGpuHandle();
			}
			cmdList->SetGraphicsRootDescriptorTable( 1, grassSrv );

			const UINT numIndices = mesh->GetNumIndices();
			const UINT numInstances = static_cast<UINT>( box->GetSpotCount() );
			const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( SimpleObjectVertexStruct ) };
			const D3D12_VERTEX_BUFFER_VIEW instVbv = { mib_inst->GetGpuVirtualAddress(), mib_inst->GetSizeInBytes(), sizeof( XMFLOAT4X4 ) };
			const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, instVbv };
			cmdList->IASetVertexBuffers( 0, 2, views );
			const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
			cmdList->IASetIndexBuffer( &ibv );
			cmdList->DrawIndexedInstanced( numIndices, numInstances, 0, 0, 0 );
		}
	}
}


bool D3D12GraphicsEngine::BuildPointShadowExcludeList( zCVobLight* lightVob, std::vector<const zCVob*>& excludeOut ) {
	excludeOut.clear();
	if ( Engine::GAPI->GetRendererState().RendererSettings.AllowSelfShadowingPointlights ) return false;
	if ( !lightVob ) return false;

	// PFX-spawned lights (spell effects etc.) aren't excluded — mirrors D3D11 GetHasOriginVob's
	// `!info->IsPFXVobLight` gate (only carried-item lights get self-shadow exclusion).
	auto li = Engine::GAPI->VobLightMap.find( lightVob );
	if ( li != Engine::GAPI->VobLightMap.end() && li->second->IsPFXVobLight ) return false;

	// Only lights attached to a carried item get exclusion (mirrors D3D11 GetHasOriginVob): walk the light
	// vob's ancestor chain looking for an oCVisualFX whose origin is an oCItem, or an oCItem ancestor directly.
	bool hasOriginVob = false;
	for ( const zCVob* vob = lightVob->GetVobParent(); vob; vob = vob->GetVobParent() ) {
		if ( auto visFx = vob->As<oCVisualFX>() ) {
			if ( const zCVob* origin = visFx->GetOrigin(); origin && origin->As<oCItem>() ) { hasOriginVob = true; break; }
		} else if ( vob->As<oCItem>() ) {
			hasOriginVob = true;
			break;
		}
	}
	if ( !hasOriginVob ) return false;

	// Collect the light vob's full ancestor chain, also following any oCVisualFX origin sideways (mirrors
	// D3D11 CollectVobTreeToExclude) — e.g. a torch item's owning NPC ends up excluded from its own light's
	// shadow cube, which is what prevents the "huge shadow blob from the player's own body" artifact.
	std::vector<const zCVob*> stack;
	stack.push_back( lightVob );
	while ( !stack.empty() ) {
		const zCVob* vob = stack.back();
		stack.pop_back();
		if ( !vob || std::find( excludeOut.begin(), excludeOut.end(), vob ) != excludeOut.end() ) continue;
		excludeOut.push_back( vob );
		if ( auto vfx = vob->As<oCVisualFX>() ) {
			if ( zCVob* origin = vfx->GetOrigin() ) stack.push_back( origin );
		}
		if ( zCVob* parent = vob->GetVobParent() ) stack.push_back( parent );
	}
	return true;
}


void D3D12GraphicsEngine::RenderPointShadows() {
	// P2.10g — static/dynamic split (the D3D11 static-aside model). Per shadowed light, the active cube is built
	// each frame as (cached static-only depth) + (this frame's dynamic casters overlaid). Three phases:
	//   A) STATIC pass — for slots whose light is fresh / moved / resized (renderStatic), (re)render the STATIC
	//      casters (world mesh + instanced VOBs) into the static-aside cube slot. Amortized: usually a no-op.
	//   B) COPY — CopyResource the whole static-aside cube into the active cube (cheap ~6MB depth copy).
	//   C) DYNAMIC overlay — render the moving casters (skeletal NPCs) into the active cube over the copied
	//      static depth (LESS_EQUAL, no clear), every frame for every shadowed light.
	// So per-frame cost is one depth copy + the few near dynamic draws — the expensive static cull/draw is paid
	// once. Casters are range-culled to each light's sphere (360°, not the camera frustum). NORMAL-Z depth; the
	// active cube round-trips DEPTH_WRITE/COPY_DEST -> PIXEL_SHADER_RESOURCE for the lit pass and back next frame.
	if ( !m_FrameOpen || !m_PointShadowCube || !m_PointShadowStaticCube || !m_Pipelines.PointShadow.CasterWorldPSO
		|| !m_PointShadowDsvHeap || !m_PointShadowStaticDsvHeap || !m_Pipelines.PointShadow.RootSig )
		return;
	if ( g_FramePointShadows.empty() ) return;

	DX_ZONE( m_CmdList, "Point Shadows (cubes)" );

	MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
	D3D12VertexBuffer* vb = wm ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
	D3D12VertexBuffer* ib = wm ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;
	const bool haveWorld = vb && ib && vb->GetResource() && ib->GetResource();
	const bool haveVobs = m_Pipelines.PointShadow.CasterVobPSO && !g_FrameVobUploads.empty()
		&& m_PointShadowVobInstPtr[m_FrameIndex];
	// Skeletal casters are now sphere-culled per light against the FULL registered vob list (see the Phase-C
	// loop below), not the player-view-culled g_FrameSkelDraws, so gate on the registry instead of that list.
	const bool haveSkel = m_Pipelines.PointShadow.CasterSkeletalPSO && m_Pipelines.PointShadow.SkeletalRootSig
		&& !Engine::GAPI->GetSkeletalMeshVobs().empty();

	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(kPointShadowCubeSize), static_cast<float>(kPointShadowCubeSize), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, static_cast<LONG>(kPointShadowCubeSize), static_cast<LONG>(kPointShadowCubeSize) };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
	auto bindDiffuse = [&]( zCTexture* tex, UINT rootParam ) {
		D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
		if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN )
			if ( MyDirectDrawSurface7* surface = tex->GetSurface() )
				if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
					D3D12Texture* d12 = D3D12Texture::From( gfx );
					if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
				}
		m_CmdList->SetGraphicsRootDescriptorTable( rootParam, srv );
		};

	// Standard D3D cube face order: +X, -X, +Y, -Y, +Z, -Z, with the canonical per-face up vectors.
	static const XMVECTORF32 kFaceDir[6] = {
		{ { {  1, 0, 0, 0 } } }, { { { -1, 0, 0, 0 } } }, { { { 0,  1, 0, 0 } } },
		{ { { 0, -1, 0, 0 } } }, { { {  0, 0, 1, 0 } } }, { { { 0, 0, -1, 0 } } } };
	static const XMVECTORF32 kFaceUp[6] = {
		{ { { 0, 1, 0, 0 } } }, { { { 0, 1, 0, 0 } } }, { { { 0, 0, -1, 0 } } },
		{ { { 0, 0, 1, 0 } } }, { { { 0, 1, 0, 0 } } }, { { { 0, 1, 0, 0 } } } };

	const D3D12_CPU_DESCRIPTOR_HANDLE activeDsvBase = m_PointShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
	const D3D12_CPU_DESCRIPTOR_HANDLE staticDsvBase = m_PointShadowStaticDsvHeap->GetCPUDescriptorHandleForHeapStart();
	auto& worldSections = Engine::GAPI->GetWorldSections();

	// Precompute each winner's 6 face view-projs into its per-frame CB slot (transpose(view*proj) — same
	// column-major convention the world/CSM shaders read back). Both the static and dynamic passes bind this.
	for ( const FramePointShadow& ps : g_FramePointShadows ) {
		if ( ps.slot >= kMaxShadowCubes ) continue;
		const XMVECTOR eye = XMLoadFloat3( &ps.posWS );
		const XMMATRIX proj = XMMatrixPerspectiveFovLH( XM_PIDIV2, 1.0f, 15.0f, ps.range * 2.0f );
		XMFLOAT4X4* faceVP = reinterpret_cast<XMFLOAT4X4*>(m_PointShadowCBMapped[m_FrameIndex] + static_cast<size_t>(ps.slot) * 512);
		for ( int f = 0; f < 6; ++f ) {
			XMMATRIX vw = XMMatrixLookAtLH( eye, XMVectorAdd( eye, kFaceDir[f] ), kFaceUp[f] );
			XMStoreFloat4x4( &faceVP[f], XMMatrixTranspose( XMMatrixMultiply( vw, proj ) ) );
		}
	}
	auto faceCb = [&]( UINT slot ) { return m_PointShadowCBGpu[m_FrameIndex] + static_cast<UINT64>( slot ) * 512; };

	// ============================ Phase A — STATIC pass (into the static-aside cube) ============================
	bool anyStatic = false;
	for ( const FramePointShadow& ps : g_FramePointShadows ) if ( ps.renderStatic && ps.slot < kMaxShadowCubes ) { anyStatic = true; break; }

	if ( anyStatic ) {
		DX_ZONE( m_CmdList, "Static Pass" );
		if ( m_PointShadowStaticState != D3D12_RESOURCE_STATE_DEPTH_WRITE ) {
			auto b = TransitionBarrier( m_PointShadowStaticCube.Get(), m_PointShadowStaticState, D3D12_RESOURCE_STATE_DEPTH_WRITE );
			m_CmdList->ResourceBarrier( 1, &b );
			m_PointShadowStaticState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		}

		// Reset the tight VOB-instance ring — only static-VOB gathers use it now (dynamic pass has no VOBs).
		m_PointShadowVobInstOffset = 0;
		uint8_t* const viBase = m_PointShadowVobInstPtr[m_FrameIndex];
		const D3D12_GPU_VIRTUAL_ADDRESS viGpu = m_PointShadowVobInstGpu[m_FrameIndex];

		for ( const FramePointShadow& ps : g_FramePointShadows ) {
			if ( ps.slot >= kMaxShadowCubes || !ps.renderStatic ) continue;
			m_PointShadowSlots[ps.slot].staticValid = true;   // static-aside now holds this slot's static depth

			D3D12_CPU_DESCRIPTOR_HANDLE dsv = staticDsvBase;
			dsv.ptr += static_cast<SIZE_T>(ps.slot) * m_PointShadowDsvSize;
			m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );
			m_CmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );

			const float rangeSq = ps.range * ps.range;

			// --- World mesh: range-cull sections (AABB nearest-point), draw all 6 faces in one call. ---
			if ( haveWorld ) {
				DX_ZONE( m_CmdList, "World Mesh" );
				m_CmdList->SetPipelineState( m_Pipelines.PointShadow.CasterWorldPSO.Get() );
				m_CmdList->SetGraphicsRootSignature( m_Pipelines.PointShadow.RootSig.Get() );
				m_CmdList->SetGraphicsRootConstantBufferView( 0, faceCb( ps.slot ) );
				const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
				const D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
				m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
				m_CmdList->IASetIndexBuffer( &ibv );
				zCTexture* boundTex = nullptr;
				for ( auto& [sx, col] : worldSections ) {
					for ( auto& [sy, section] : col ) {
						const zTBBox3D& bb = section.BoundingBox;
						float cx = std::min( std::max( ps.posWS.x, bb.Min.x ), bb.Max.x );
						float cy = std::min( std::max( ps.posWS.y, bb.Min.y ), bb.Max.y );
						float cz = std::min( std::max( ps.posWS.z, bb.Min.z ), bb.Max.z );
						float dx = ps.posWS.x - cx, dy = ps.posWS.y - cy, dz = ps.posWS.z - cz;
						if ( dx * dx + dy * dy + dz * dz >= rangeSq ) continue;   // section outside the light sphere
						for ( auto const& [meshKey, mesh] : section.WorldMeshes ) {
							if ( !mesh || mesh->Indices.empty() ) continue;
							if ( meshKey.Info && meshKey.Info->MaterialType == MaterialInfo::MT_Water ) continue;
							zCTexture* tex = meshKey.Material->GetAniTexture();
							if ( tex != boundTex ) { bindDiffuse( tex, 1 ); boundTex = tex; }
							m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mesh->Indices.size()), 6, mesh->BaseIndexLocation, 0, 0 );
						}
					}
				}
			}

			// --- Instanced VOBs (static decoration): range-cull instances, pack 64B world matrices into the tight
			// ring, draw count*6 (InstanceDataStepRate=6 → 6 faces per real instance). Same root sig as world.
			// Skipped for isStatic() lights — mirrors D3D11's RenderStaticShadowPass staticCasterMask, which
			// restricts a static light's (cached) cube to world-mesh-only casters, no VOBs/MOBS. ---
			if ( haveVobs && !m_PointShadowSlots[ps.slot].isStatic ) {
				DX_ZONE( m_CmdList, "Vobs" );
				m_CmdList->SetPipelineState( m_Pipelines.PointShadow.CasterVobPSO.Get() );
				m_CmdList->SetGraphicsRootSignature( m_Pipelines.PointShadow.RootSig.Get() );
				m_CmdList->SetGraphicsRootConstantBufferView( 0, faceCb( ps.slot ) );
				for ( const FrameVobUpload& up : g_FrameVobUploads ) {
					MeshVisualInfo* visual = up.visual;
					if ( !visual || visual->Instances.empty() ) continue;
					const float cullR = ps.range + visual->MeshSize * 0.5f;   // sphere test allows for VOB extent
					const float cullRSq = cullR * cullR;

					const UINT gatherStart = m_PointShadowVobInstOffset;
					UINT count = 0;
					bool overflow = false;
					for ( const VobInstanceInfo& inst : visual->Instances ) {
						float dx = inst.world._14 - ps.posWS.x, dy = inst.world._24 - ps.posWS.y, dz = inst.world._34 - ps.posWS.z;
						if ( dx * dx + dy * dy + dz * dz >= cullRSq ) continue;
						if ( m_PointShadowVobInstOffset + sizeof( XMFLOAT4X4 ) > m_PointShadowVobInstCapacity ) {
							if ( !m_PointShadowVobInstOverflowLogged ) {
								LogWarn() << "D3D12: point-shadow VOB instance ring overflow ("
									<< m_PointShadowVobInstCapacity << " bytes/frame); some cube casters dropped.";
								m_PointShadowVobInstOverflowLogged = true;
							}
							overflow = true;
							break;
						}
						memcpy( viBase + m_PointShadowVobInstOffset, &inst.world, sizeof( XMFLOAT4X4 ) );
						m_PointShadowVobInstOffset += sizeof( XMFLOAT4X4 );
						++count;
					}
					if ( count == 0 ) { if ( overflow ) break; continue; }

					const D3D12_VERTEX_BUFFER_VIEW instView = { viGpu + gatherStart, count * static_cast<UINT>(sizeof( XMFLOAT4X4 )), static_cast<UINT>(sizeof( XMFLOAT4X4 )) };
					for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
						bindDiffuse( meshKey.Material->GetAniTexture(), 1 );
						for ( MeshInfo* mi : meshList ) {
							if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() ) continue;
							D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
							D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
							if ( !mvb->GetResource() || !mib->GetResource() ) continue;
							const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
							const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, instView };
							m_CmdList->IASetVertexBuffers( 0, 2, views );
							const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
							m_CmdList->IASetIndexBuffer( &ibv );
							m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mi->Indices.size()), count * 6, 0, 0, 0 );
						}
					}
					if ( overflow ) break;
				}
			}
		}
	}

	// ============================ Phase B — COPY static-aside -> active cube (per-slot, touched only) ============================
	// Only the slots being (re)drawn THIS frame (static change and/or scheduled dynamic overlay, see the
	// round-robin scheduling in BuildFrameLightBuffer) get their active cube refreshed from static-aside. A far
	// light skipped this frame keeps EXACTLY what was last composited into its active cube — including its last
	// dynamic overlay — instead of being stomped back to pure static every frame and losing it. Copies are
	// per-slot (6 face subresources) rather than one whole-array CopyResource so skipped slots cost nothing.
	{
		DX_ZONE( m_CmdList, "Copy Static->Active" );
		bool anyTouched = false;
		for ( const FramePointShadow& ps : g_FramePointShadows )
			if ( ps.slot < kMaxShadowCubes && ( ps.renderStatic || ps.renderDynamic ) ) { anyTouched = true; break; }

		if ( anyTouched ) {
			D3D12_RESOURCE_BARRIER pre[2];
			UINT n = 0;
			if ( m_PointShadowStaticState != D3D12_RESOURCE_STATE_COPY_SOURCE ) {
				pre[n++] = TransitionBarrier( m_PointShadowStaticCube.Get(), m_PointShadowStaticState, D3D12_RESOURCE_STATE_COPY_SOURCE );
				m_PointShadowStaticState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			const D3D12_RESOURCE_STATES activeCur = m_PointShadowInPixelState
				? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_DEPTH_WRITE;
			pre[n++] = TransitionBarrier( m_PointShadowCube.Get(), activeCur, D3D12_RESOURCE_STATE_COPY_DEST );
			m_CmdList->ResourceBarrier( n, pre );

			for ( const FramePointShadow& ps : g_FramePointShadows ) {
				if ( ps.slot >= kMaxShadowCubes || !( ps.renderStatic || ps.renderDynamic ) ) continue;
				for ( UINT face = 0; face < 6; ++face ) {
					const UINT sub = ps.slot * 6 + face;
					D3D12_TEXTURE_COPY_LOCATION dstLoc = { m_PointShadowCube.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
					dstLoc.SubresourceIndex = sub;
					D3D12_TEXTURE_COPY_LOCATION srcLoc = { m_PointShadowStaticCube.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
					srcLoc.SubresourceIndex = sub;
					m_CmdList->CopyTextureRegion( &dstLoc, 0, 0, 0, &srcLoc, nullptr );
				}
			}

			auto toDepth = TransitionBarrier( m_PointShadowCube.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE );
			m_CmdList->ResourceBarrier( 1, &toDepth );
			m_PointShadowInPixelState = false;   // active cube now in DEPTH_WRITE for the dynamic overlay
		}
	}

	// ============================ Phase C — DYNAMIC overlay (skeletal NPCs into active cube) ============================
	// Rendered over the copied static depth (LESS_EQUAL, NO clear) so moving casters composite with the cached
	// static occluders. Runs every frame for every shadowed light — this is the real-time part of the split.
	if ( haveSkel ) {
		DX_ZONE( m_CmdList, "Dynamic Overlay (skeletals)" );
		static std::vector<const zCVob*> excludeVobs;
		// Coarse per-vob mesh-size margin for the sphere pre-filter below (PrepareFrameSkeletals doesn't know a
		// vob's actual mesh extent yet — the exact per-record cull, ps.range + visual->MeshSize*0.5f, still
		// runs below once the visual is resolved).
		constexpr float kSkeletalCullPad = 6.0f;
		for ( const FramePointShadow& ps : g_FramePointShadows ) {
			if ( ps.slot >= kMaxShadowCubes ) continue;
			// isStatic() lights never get the dynamic overlay — mirrors D3D11's GetCurrentShadowMode, which forces
			// a static light down to PLS_STATIC_ONLY regardless of the global EnablePointlightShadows setting, so
			// it never runs RenderAnimatedShadowPass. Its active cube stays exactly the copied static-aside depth.
			if ( m_PointShadowSlots[ps.slot].isStatic ) continue;
			// Round-robin gate (P2.10h): far/less-important dynamic lights only get the skeletal overlay on their
			// scheduled frame (see BuildFrameLightBuffer) — skipped frames just keep this frame's copied static
			// depth, so a moving caster's shadow lags a few frames behind instead of costing a full sphere-cull
			// every frame for every one of the now much larger (128) persisted-light set.
			if ( !ps.renderDynamic ) continue;
			// Re-bind every light: the node-attachment block below switches to PointShadow.RootSig (a DIFFERENT,
			// smaller root signature) for the tail of each light's iteration, so the skeletal root sig/PSO can't
			// be assumed still bound once we're past the first light — re-set it unconditionally per light instead
			// of once before the loop (bug: 2nd+ shadowed light's instCb/boneCb/diffuse binds landed on the wrong
			// root signature's parameter slots — GPU device hang on hardware, caught via D3D12 validation).
			m_CmdList->SetPipelineState( m_Pipelines.PointShadow.CasterSkeletalPSO.Get() );
			m_CmdList->SetGraphicsRootSignature( m_Pipelines.PointShadow.SkeletalRootSig.Get() );
			D3D12_CPU_DESCRIPTOR_HANDLE dsv = activeDsvBase;
			dsv.ptr += static_cast<SIZE_T>(ps.slot) * m_PointShadowDsvSize;
			m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );   // no clear — keep the copied static depth
			m_CmdList->SetGraphicsRootConstantBufferView( 0, faceCb( ps.slot ) );

			// Self-shadow exclusion (P2.10 owed-debt item): a light carried by an NPC (e.g. a torch in its hand)
			// otherwise casts that NPC's own body as a huge shadow blob into its own cube — see
			// BuildPointShadowExcludeList / D3D11's GetHasOriginVob+SetupVobsToExclude.
			const bool hasExclusions = BuildPointShadowExcludeList( m_PointShadowSlots[ps.slot].owner, excludeVobs );

			// Sphere-cull the FULL registered skeletal-vob list against THIS light (parity with the CSM cascade
			// fix — a caster invisible to the player, but within a torch's range, can still cast a shadow into
			// it), reusing g_SkelUploadCache so an NPC already prepared for the main view/a cascade this frame
			// costs nothing extra here beyond the sphere test + record append. Same O(lights * vobs) CPU cost
			// D3D11's own per-light DrawWorldAround pays for its animated-shadow pass — cheap distance checks,
			// not GPU work (the static-aside split already amortizes the expensive part).
			g_PointShadowSkelDraws.clear();
			g_PointShadowAttachDraws.clear();
			PrepareFrameSkeletals( Engine::GAPI->GetSkeletalMeshVobs(), nullptr, -2, &ps.posWS, ps.range + kSkeletalCullPad );

			for ( const FrameSkelDraw& d : g_PointShadowSkelDraws ) {
				if ( !d.visual || !d.vobInfo || !d.vobInfo->Vob ) continue;
				if ( hasExclusions && std::find( excludeVobs.begin(), excludeVobs.end(), d.vobInfo->Vob ) != excludeVobs.end() )
					continue;
				const XMFLOAT3 pos = d.vobInfo->Vob->GetPositionWorld();
				const float cullR = ps.range + d.visual->MeshSize * 0.5f;
				float dx = pos.x - ps.posWS.x, dy = pos.y - ps.posWS.y, dz = pos.z - ps.posWS.z;
				if ( dx * dx + dy * dy + dz * dz >= cullR * cullR ) continue;

				// Shared per-MODEL texture slots: refresh THIS instance's textures right before reading its
				// materials (see [[skeletal-texani-shared-slots]]) — required in the cube alpha-clip pass too.
				zCModel* model = static_cast<zCModel*>(d.vobInfo->Vob->GetVisual());
				model->UpdateMeshLibTexAniState();

				m_CmdList->SetGraphicsRootConstantBufferView( 1, d.instCb );
				m_CmdList->SetGraphicsRootConstantBufferView( 2, d.boneCb );
				for ( auto const& [mat, meshList] : d.visual->SkeletalMeshes ) {
					bindDiffuse( mat ? mat->GetAniTexture() : nullptr, 3 );
					for ( auto const& mesh : meshList ) {
						if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer ) continue;
						D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
						D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
						if ( !mvb->GetResource() || !mib->GetResource() ) continue;
						const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExSkelVertexStruct ) };
						m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
						const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
						m_CmdList->IASetIndexBuffer( &ibv );
						m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mesh->Indices.size()), 6, 0, 0, 0 );
					}
				}
			}

			// --- Node attachments (weapons/torches/held items) — owed-debt item finally ported: mirrors the CSM
			// cascade's "Skeletal Nodes" pass (line ~2105 above) but through the point-shadow VOB caster PSO (CBV
			// per-face view-projs, not root constants) and 6 face instances instead of 1. Runs once per light
			// (not per skeletal draw) since g_PointShadowAttachDraws already holds every attachment sphere-culled
			// against THIS light by the PrepareFrameSkeletals call above. Without this, a held torch/weapon never
			// cast a shadow into its own point-shadow cube even though its owning NPC's body did. Same self-shadow
			// exclusion as the body (a torch-carrying NPC's own held item shouldn't blob-shadow the light it's
			// carrying).
			if ( m_Pipelines.PointShadow.CasterVobPSO && !g_PointShadowAttachDraws.empty() ) {
				DX_ZONE( m_CmdList, "Skeletal Nodes" );
				m_CmdList->SetPipelineState( m_Pipelines.PointShadow.CasterVobPSO.Get() );
				m_CmdList->SetGraphicsRootSignature( m_Pipelines.PointShadow.RootSig.Get() );
				m_CmdList->SetGraphicsRootConstantBufferView( 0, faceCb( ps.slot ) );
				for ( const FrameAttachDraw& a : g_PointShadowAttachDraws ) {
					if ( !a.mesh || !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() ) continue;
					if ( hasExclusions && a.owner && std::find( excludeVobs.begin(), excludeVobs.end(), a.owner ) != excludeVobs.end() )
						continue;
					D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
					D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
					if ( !mvb->GetResource() || !mib->GetResource() ) continue;
					bindDiffuse( a.tex, 1 );
					const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
					const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, a.instView };
					m_CmdList->IASetVertexBuffers( 0, 2, views );
					const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
					m_CmdList->IASetIndexBuffer( &ibv );
					m_CmdList->DrawIndexedInstanced( static_cast<UINT>(a.mesh->Indices.size()), 6, 0, 0, 0 );
				}
			}
		}
	}

	// ============================ Phase D — active cube -> PIXEL_SHADER_RESOURCE for the lit pass ============================
	// Conditional on m_PointShadowInPixelState: if Phase B found nothing to touch this frame (no static change,
	// no slot due for its dynamic round-robin turn), the active cube was left exactly as Phase D put it last
	// frame — already PIXEL_SHADER_RESOURCE — and re-issuing this transition would barrier from the wrong state.
	if ( !m_PointShadowInPixelState ) {
		auto toSrv = TransitionBarrier( m_PointShadowCube.Get(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		m_CmdList->ResourceBarrier( 1, &toSrv );
		m_PointShadowInPixelState = true;
	}

	// Restore the HDR scene-color RT (+ shared depth) for the lit passes that follow — the world pass renders
	// into the HDR target, not the swapchain (Phase 3); the tonemap resolve composites it at the end of the frame.
	D3D12_CPU_DESCRIPTOR_HANDLE mainDsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, m_DepthBuffer ? &mainDsv : nullptr );
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

	// Point-light shadow selection (P2.10c + static-aside/round-robin P2.10f). Pick the closest-to-camera
	// in-range lights (up to kMaxShadowCubes) as this frame's "winners", but assign each winner a STABLE cube
	// slot keyed by its light Vob identity (kept across frames, not reassigned by proximity every frame). A
	// slot's rendered content persists in the cube array, so a STATIC winner whose light didn't move can reuse
	// its cached cube (render=false) instead of re-culling + re-rendering all world/VOB/skeletal casters each
	// frame. Dynamic (moving) lights, newly-assigned slots, and moved/range-changed lights render (render=true).
	// Mirrors D3D11 DrawPointlightShadows' distance/range gating (it keys off the player; camera is close enough).
	g_FramePointShadows.clear();
	if ( m_PointShadowCube && count > 0 ) {
		const XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
		struct Cand { UINT dstIdx; zCVobLight* vob; float distSq; float sortKey; bool isStatic; };
		static std::vector<Cand> cands;
		cands.clear();
		for ( UINT i = 0; i < count; ++i ) {
			const GPULight& L = dst[i];
			const float range = L.Range;
			if ( range <= 0.0f ) continue;
			XMVECTOR d = XMVectorSubtract( XMLoadFloat3( &L.PositionWorld ), camPos );
			float distSq = XMVectorGetX( XMVector3LengthSq( d ) );
			const float maxSq = (range * 9.0f) * (range * 9.0f);   // D3D11 distMaxShadowSq
			if ( distSq >= maxSq ) continue;
			// Sticky/hysteresis: a light that already owns a slot gets its distance discounted before ranking,
			// so it takes a meaningfully closer newcomer to evict it rather than a marginal distance difference.
			// Without this, a burst of newly-frustum-visible lights (e.g. rotating to face an outdoor cluster)
			// could bump an already-shadowed indoor light out of the winner set on a single frame, snapping its
			// shadow off (and letting its light bleed through walls unshadowed) until it re-wins a slot later —
			// the exact "shadows pop on/off when turning a few degrees" artifact this guards against.
			bool isIncumbent = false;
			for ( UINT s = 0; s < kMaxShadowCubes; ++s ) if ( m_PointShadowSlots[s].owner == s_lightVobs[i] ) { isIncumbent = true; break; }
			constexpr float kIncumbentBias = 0.35f;   // incumbent must be ~1.7x farther than a challenger to lose its slot
			float sortKey = isIncumbent ? distSq * kIncumbentBias : distSq;
			cands.push_back( { i, s_lightVobs[i], distSq, sortKey, L.Color.w == 0.0f } );   // Color.w: 0 = static light
		}
		std::sort( cands.begin(), cands.end(), []( const Cand& a, const Cand& b ) { return a.sortKey < b.sortKey; } );
		if ( cands.size() > kMaxShadowCubes ) cands.resize( kMaxShadowCubes );

		// Release slots whose owner is no longer a winner (frees them + invalidates their cached content).
		for ( UINT s = 0; s < kMaxShadowCubes; ++s ) {
			zCVobLight* o = m_PointShadowSlots[s].owner;
			if ( !o ) continue;
			bool stillWinner = false;
			for ( const Cand& c : cands ) if ( c.vob == o ) { stillWinner = true; break; }
			if ( !stillWinner ) m_PointShadowSlots[s] = PointShadowSlot{};
		}

		// Assign each winner a stable slot (keep its existing one, else grab a free one) and decide render vs cache.
		for ( const Cand& c : cands ) {
			int slot = -1;
			for ( UINT s = 0; s < kMaxShadowCubes; ++s )
				if ( m_PointShadowSlots[s].owner == c.vob ) { slot = static_cast<int>( s ); break; }
			if ( slot < 0 ) {
				for ( UINT s = 0; s < kMaxShadowCubes; ++s )
					if ( !m_PointShadowSlots[s].owner ) { slot = static_cast<int>( s ); break; }
				if ( slot < 0 ) continue;   // no free slot (can't happen: winners <= kMaxShadowCubes)
				m_PointShadowSlots[slot].owner = c.vob;
				m_PointShadowSlots[slot].staticValid = false;   // fresh occupant → must render static
			}
			PointShadowSlot& ss = m_PointShadowSlots[slot];
			ss.isStatic = c.isStatic;

			GPULight& L = dst[c.dstIdx];
			const XMFLOAT3& np = L.PositionWorld;
			const float moveEps = 0.5f;   // Gothic world units; below this the light hasn't meaningfully moved
			bool moved = std::fabs( np.x - ss.pos.x ) > moveEps
				|| std::fabs( np.y - ss.pos.y ) > moveEps
				|| std::fabs( np.z - ss.pos.z ) > moveEps;
			bool rangeChanged = std::fabs( L.Range - ss.range ) > 1.0f;
			// Static-aside is re-rendered only when fresh / the light moved / range changed; otherwise reused.
			// The DYNAMIC (skeletal) overlay + the static->active copy run every frame regardless (see RenderPointShadows).
			bool renderStatic = !ss.staticValid || moved || rangeChanged;

			L.ShadowCubeIndex = static_cast<int32_t>(slot);
			g_FramePointShadows.push_back( { np, L.Range, static_cast<UINT>(slot), renderStatic, false } );
			if ( renderStatic ) { ss.pos = np; ss.range = L.Range; }   // staticValid stamped once actually drawn
		}

		// Round-robin the per-frame skeletal DYNAMIC overlay across non-static winners (P2.10h). With
		// kMaxShadowCubes now persisting many more lights, running the full sphere-cull-against-registered-
		// skeletal-vobs pass for every single winner every frame would multiply CPU cost with light count.
		// The nearest kAlwaysDynamicCount winners (where a moving caster's shadow lag would be most visible)
		// always get it; the rest take turns via a stale-frames counter so every dynamic light's overlay still
		// refreshes periodically instead of never. Static-aside geometry shadows (walls, VOBs) are unaffected —
		// those persist via the static cache regardless of whether the dynamic overlay runs this frame.
		constexpr UINT kAlwaysDynamicCount = 8;   // NPCs barely move frame-to-frame; only the player (camera) moves a lot,
		// and it won't be this close to more than a handful of lights at once, nor perceive the lag on distant ones.
		constexpr UINT kDynamicRoundRobinBudget = 6;

		std::vector<FramePointShadow*> nonStatic;
		nonStatic.reserve( g_FramePointShadows.size() );
		for ( FramePointShadow& ps : g_FramePointShadows ) if ( !m_PointShadowSlots[ps.slot].isStatic ) nonStatic.push_back( &ps );
		std::sort( nonStatic.begin(), nonStatic.end(), [&]( const FramePointShadow* a, const FramePointShadow* b ) {
			XMVECTOR da = XMVectorSubtract( XMLoadFloat3( &a->posWS ), camPos );
			XMVECTOR db = XMVectorSubtract( XMLoadFloat3( &b->posWS ), camPos );
			return XMVectorGetX( XMVector3LengthSq( da ) ) < XMVectorGetX( XMVector3LengthSq( db ) );
		} );

		const size_t closeCount = std::min<size_t>( kAlwaysDynamicCount, nonStatic.size() );
		for ( size_t idx = 0; idx < closeCount; ++idx ) {
			nonStatic[idx]->renderDynamic = true;
			m_PointShadowSlots[nonStatic[idx]->slot].dynamicStaleFrames = 0;
		}
		for ( size_t idx = closeCount; idx < nonStatic.size(); ++idx ) ++m_PointShadowSlots[nonStatic[idx]->slot].dynamicStaleFrames;

		// Among the "far" set, service the most-stale slots first, up to this frame's round-robin budget.
		std::sort( nonStatic.begin() + closeCount, nonStatic.end(), [&]( const FramePointShadow* a, const FramePointShadow* b ) {
			return m_PointShadowSlots[a->slot].dynamicStaleFrames > m_PointShadowSlots[b->slot].dynamicStaleFrames;
		} );
		for ( size_t idx = closeCount, serviced = 0; idx < nonStatic.size() && serviced < kDynamicRoundRobinBudget; ++idx, ++serviced ) {
			nonStatic[idx]->renderDynamic = true;
			m_PointShadowSlots[nonStatic[idx]->slot].dynamicStaleFrames = 0;
		}

		// A slot whose STATIC base is (re)rendered this frame must also refresh its dynamic overlay, round-robin
		// turn or not: RenderPointShadows' Phase B only copies static-aside -> active for slots it's about to
		// touch (see there), and the previous active cube's dynamic content was composited against the OLD
		// static depth — invalid once the static geometry/position changes. Not forcing this would either ghost
		// stale skeletal shadows onto a new depth base, or (since Phase B still must copy the new static in)
		// silently drop the overlay for a light that's actually due for a refresh.
		for ( FramePointShadow& ps : g_FramePointShadows ) {
			if ( ps.renderStatic && !ps.renderDynamic ) {
				ps.renderDynamic = true;
				m_PointShadowSlots[ps.slot].dynamicStaleFrames = 0;
			}
		}
	}
}


void D3D12GraphicsEngine::BindFrameLights( UINT srvParam, UINT countParam, UINT gridParam, UINT indexParam ) {
	// Bind the Forward+ tiled point-light root params: srvParam = the light StructuredBuffer as a root SRV
	// (t1), countParam = LightCB { LightCount, NumTilesX, LimitLightIntensity }, gridParam/indexParam = the
	// per-tile grid (t2) and light-index list (t3) root SRVs produced by DispatchLightCulling. EVERY draw
	// whose bound PSO reads the tiled light loop MUST call this after setting its root signature, or the loop
	// bound (Count) and grid are UNDEFINED root values and can run billions of iterations → GPU timeout/
	// removal. Root args are cleared on every SetGraphicsRootSignature. The param indices differ per root
	// sig: m_Pipelines.World.RootSig uses (3,4,5,6) — the default — for the world mesh / instanced VOBs /
	// node attachments; m_Pipelines.Skeletal.RootSig uses (5,6,7,8).
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
	// bucketed by texture) + FrameParticleInfo (blend mode per texture). The mesh-PFX sub-call inside
	// (DrawFrameParticleMeshes) is a no-op on D3D12 — mesh-shaped effects are a later step.
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
	for ( auto it = transparencyVobs.rbegin(); it != transparencyVobs.rend(); ++it ) {
		const TransparencyVobInfo& info = *it;

		// Skeletal ghosts (invisible/fading NPCs) — mirrors D3D11's DrawTransparencyVobs skeletalVob branch, minus
		// its same-mesh Z-prepass (same simplification the non-skeletal ghost path already made). Bone transforms
		// are computed here directly rather than via PrepareFrameSkeletals/g_SkelUploadCache: that pass explicitly
		// excludes ghost vobs (they never enter the normal skinned-color draw), so there is no cached pose to reuse.
		if ( info.skeletalVob ) {
			SkeletalVobInfo* skel = info.skeletalVob;
			if ( !skel->Vob || !skel->VisualInfo ) continue;
			if ( !m_Pipelines.GhostSkeletal.PSO || !m_Pipelines.GhostSkeletal.RootSig ) continue;   // pipeline unavailable (logged at Init)
			if ( !m_SkeletalCBBuffer[frame] || !m_SkeletalCBBufferPtr[frame] ) continue;

			zCModel* model = static_cast<zCModel*>( skel->Vob->GetVisual() );
			if ( !model ) continue;
			SkeletalMeshVisualInfo* visual = static_cast<SkeletalMeshVisualInfo*>( skel->VisualInfo );
			if ( visual->SkeletalMeshes.empty() ) continue;   // node-attachment-only ghosts: not handled (rare, owed-debt)

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
				D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
				zCTexture* tex = mat ? mat->GetAniTexture() : nullptr;
				if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
					if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
						if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
							D3D12Texture* d12 = D3D12Texture::From( gfx );
							if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
						}
					}
				}
				m_CmdList->SetGraphicsRootDescriptorTable( 4, srv );
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
			m_CmdList->SetGraphicsRootDescriptorTable( 10, GetSrvGpuHandle( m_ShadowSrvSlot ) );
			m_CmdList->SetGraphicsRootDescriptorTable( 11, GetSrvGpuHandle( m_PointShadowSrvSlot ) );
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
	PrepareFrameSkeletals( Engine::GAPI->GetAnimatedSkeletalMeshVobs() );
	PrepareFrameSkeletals( g_FrameMobs );

	// Refresh the wind CB's player position ONCE here (before shadows/prepass/color all run this frame) — windDir/
	// globalTime were already advanced once in OnBeginFrame; minHeight/maxHeight are refreshed per-visual right
	// before each pass's VOB draws (DrawVobDepthPrepass / RenderSunShadows / DrawVobsInstanced).
	if ( zCVob* player = Engine::GAPI->GetPlayerVob() ) {
		m_WindBuffer.playerPos = player->GetPositionWorld();
	}

	// Phase 3 HDR: redirect the whole 3D scene into the R16F scene-color target (OnBeginFrame bound the
	// swapchain for 2D-only/menu frames; here we switch to HDR so lighting can exceed 1.0). Depth is shared.
	BindSceneColorTarget();

	DrawSky();
	// CSM sun shadows (P2.9c): render the opaque casters into the cascade shadow map from the sun's POV. Runs
	// before the main geometry; rebinds the backbuffer RT/DSV when done. Nothing samples the map yet (later
	// increment), so this is visually a no-op — inspect the shadow Texture2DArray in RenderDoc.
	RenderSunShadows();
	// Point-light shadow cubes (P2.10): render each selected shadowed light's 6 faces into the shared cube array.
	// Runs before the lit passes that sample it; leaves the backbuffer RT/DSV rebound. Visually a no-op until the
	// point-light loop samples the cubes (P2.10d).
	RenderPointShadows();
	// Rain shadowmap (D3D12 rain parity): world-mesh-only depth cast from an orthographic camera along
	// the rain-velocity direction, so DrawRainParticles' VS can zero out indoor/roofed raindrops. Leaves
	// the backbuffer RT/DSV rebound after (like the two shadow passes above).
	RenderRainShadowmap();
	// Scene wetness ("wet ground"): publish this frame's rain camera + wetness/time constants into the tail
	// of the shared shadow CB so the lit World/Vob/Skeletal pixel shaders can darken, ripple and gloss up
	// the surfaces the rain actually reaches. Unconditional (it also has to publish the "not wet" state) and
	// necessarily AFTER RenderRainShadowmap, which is what computes m_RainShadowViewProj.
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
    {
        DX_ZONE( m_CmdList, "Depth Prepass" );
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
    {
        DX_ZONE( m_CmdList, "Lit Geometry Pass" );
	    DrawWorldMesh();
	    {
		    DX_ZONE( m_CmdList, "Draw skeletal (color)" );
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
		DrawDecalList( decals, true );
	}

	// Water: alpha-blended over the finished opaque scene (world + NPCs + VOBs + opaque decals).
	DrawWaterSurfaces();

	{
		DX_ZONE( m_CmdList, "Draw decals (transparent)" );
		DrawDecalList( decals, false );
	}

	// Particles last: billboarded PFX (fire, smoke, magic, dust) blended over everything, depth-tested
	// against the opaque scene but not writing depth. Mirrors D3D11's late DrawParticlesSimple pass.
	{
		DX_ZONE( m_CmdList, "Draw particles" );
		DrawParticleEffects();
	}

	// Rain/snow (D3D12 rain parity, step 2): unlit placeholder billboards, always "wet" — see
	// DrawRainParticles. Same late-transparency slot D3D11 draws rain in.
	{
		DX_ZONE( m_CmdList, "Draw rain" );
		DrawRainParticles();
	}

	// Ghosts (invisible-potion/fade-out items): drawn last of the alpha content, mirrors D3D11's "Draw ghosts"
	// pass placement (after the transparency waterfall/decals, before post-FX). MUST run every frame even if
	// EnableBloom/etc. are off — it also drains GothicAPI::TransparencyVobs, which nothing else consumes.
	{
		DX_ZONE( m_CmdList, "Draw ghosts" );
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

	// Debug/editor lines last, on the finished LDR image — same slot as D3D11's "Draw Debug Lines" render-graph
	// pass (after post-FX, before Gothic's 2D UI composites on top). Both calls also CLEAR their cache, so this
	// is what keeps the line lists from growing unbounded across frames.
	{
		DX_ZONE( m_CmdList, "Draw debug lines" );
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
    
    
    for ( UINT c = 0; c < kShadowCascades; ++c ) {
        for ( UINT i = 0; i < kBackBufferCount; ++i ) {
            if ( FAILED( m_Allocator->CreateResource( &upload, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_ShadowWorldDrawArgsAlloc[c][i].ReleaseAndGetAddressOf(),
                IID_PPV_ARGS( m_ShadowWorldDrawArgs[c][i].ReleaseAndGetAddressOf() ) ) ) )
                return false;
            
            m_ShadowWorldDrawArgs[c][i]->SetName( L"ShadowWorldDrawArgsRing" );
            D3D12_RANGE noRead = { 0, 0 };
            void* mapped = nullptr;
            if ( FAILED( m_ShadowWorldDrawArgs[c][i]->Map( 0, &noRead, &mapped ) ) ) return false;
            m_ShadowWorldDrawArgsPtr[c][i] = static_cast<uint8_t*>( mapped );
            m_ShadowWorldDrawArgsGpu[c][i] = m_ShadowWorldDrawArgs[c][i]->GetGPUVirtualAddress();
        }
    }
    return true;
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

            // Water is transparent — bucket it by texture for the later alpha-blended pass, skip the opaque command set.
            if ( meshKey.Info) {
                if ( meshKey.Info->MaterialType == MaterialInfo::MT_Water ) {
                    g_FrameWaterSurfaces[meshKey.Material->GetAniTexture()].push_back( mesh );
                    continue;
                } else if ( meshKey.Info->MaterialType == MaterialInfo::MT_Portal ) {
                    // don't draw portal meshes
                    continue;
                }
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
    for ( UINT c = 0; c < kShadowCascades; ++c )
        for ( UINT i = 0; i < kBackBufferCount; ++i )
            if ( !makeRing( kMaxShadowVobDrawCommands, m_ShadowVobDrawArgs[c][i], m_ShadowVobDrawArgsAlloc[c][i], m_ShadowVobDrawArgsPtr[c][i], L"ShadowVobDrawArgsRing" ) )
                return false;
    return true;
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
    UINT idx[2] = { 0xFFFFFFFFu, m_DefaultOrmTexture->GetSrvSlot() };
    if ( tex ) {
        if ( MyDirectDrawSurface7* s = tex->GetSurface() ) {
            if ( GfxTexture* n = s->GetNormalmap() ) {
                D3D12Texture* d = D3D12Texture::From( n );
                if ( d->HasSRV() ) idx[0] = d->GetSrvSlot();
            }
            if ( GfxTexture* o = s->GetFxMap() ) {
                D3D12Texture* d = D3D12Texture::From( o );
                if ( d->HasSRV() ) idx[1] = EncodeOrmSlot( d->GetSrvSlot(), s->GetAvailableMaterials() );
            }
        }
    }
    m_CmdList->SetGraphicsRoot32BitConstants( matRootParam, 2, idx, 0 );
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

    // Camera matrices — replicate the D3D11 DrawWorldMesh setup exactly so ViewProj is byte-identical:
    // world verts are already world-space (identity world), transform is proj*view (reversed-Z).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    const FogConstants fog = MakeFogConstants();

    m_CmdList->SetPipelineState( m_Pipelines.World.PSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );   // b1 fog
    BindFrameLights();   // param 3 = light SRV (t1), param 4 = light count (b2) — MUST set both or the
                         // shader's light loop reads a garbage count and runs away (GPU TDR hang).
    // CSM sampling: param 7 = shadow CB (b3), param 8 = shadow-map array SRV (t4). The map was left in
    // PIXEL_SHADER_RESOURCE by RenderSunShadows; the PS samples it to darken sun-occluded surfaces.
    m_CmdList->SetGraphicsRootConstantBufferView( 7, m_ShadowCBGpu[m_FrameIndex] );
    m_CmdList->SetGraphicsRootDescriptorTable( 8, GetSrvGpuHandle( m_ShadowSrvSlot ) );
    m_CmdList->SetGraphicsRootDescriptorTable( 9, GetSrvGpuHandle( m_PointShadowSrvSlot ) );   // t5 point-shadow cubes
    m_CmdList->SetGraphicsRoot32BitConstants( 12, 1, &m_ActiveAOMaskSrvSlot, 0 );   // b7 AOCB (simple SSAO mask)

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
    m_CmdList->SetGraphicsRootDescriptorTable( 8, GetSrvGpuHandle( m_ShadowSrvSlot ) );      // t4 shadow map
    m_CmdList->SetGraphicsRootDescriptorTable( 9, GetSrvGpuHandle( m_PointShadowSrvSlot ) ); // t5 point-shadow cubes
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
    const DirectX::XMFLOAT3* sphereCenter, float sphereRange, UINT cascadeCount ) {
    // P2.9b-4b (pre-cull) + shadow-cascade/point-shadow parity: run each candidate skeletal vob's once-per-frame
    // animation update, upload its instance + bone CBs (base meshes) and its node attachments' VOB-instance data
    // into the per-frame rings ONCE (cached in g_SkelUploadCache — the pose data is view-independent), and
    // RECORD the (possibly cached) GPU addresses into the caller's destination list: g_FrameSkelDraws/
    // g_FrameAttachDraws for the main view (shadowCascade < 0, default), g_ShadowSkelDraws[c]/
    // g_ShadowAttachDraws[c] for CSM cascade c (shadowCascade >= 0), or g_PointShadowSkelDraws/
    // g_PointShadowAttachDraws for a point light (shadowCascade == -2) — mirrors D3D11's
    // Shadows::DrawSkeletalMeshes, which culls the FULL registered skeletal-vob list against the shadow's OWN
    // frustum/sphere rather than reusing the player's view-frustum-culled list (a caster invisible to the
    // player can still cast a visible shadow). NO draws here — DrawSkeletalDepthPrepass/DrawSkeletalColor read
    // g_FrameSkelDraws/g_FrameAttachDraws; RenderSunShadows reads g_ShadowSkelDraws[c]/g_ShadowAttachDraws[c];
    // RenderPointShadows reads g_PointShadowSkelDraws.
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
    std::vector<FrameSkelDraw>&   outSkel   = (shadowCascade >= 0) ? g_ShadowSkelDraws[shadowCascade]
                                             : (shadowCascade == -2) ? g_PointShadowSkelDraws : g_FrameSkelDraws;
    std::vector<FrameAttachDraw>& outAttach = (shadowCascade >= 0) ? g_ShadowAttachDraws[shadowCascade]
                                             : (shadowCascade == -2) ? g_PointShadowAttachDraws : g_FrameAttachDraws;

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

        // Ghosts don't cast shadows either (parity with D3D11's Shadows::DrawSkeletalMeshes ghost skip).
        if ( vi->Vob->GetVisualAlpha() && vi->Vob->GetVobTransparency() < 0.7f ) continue;

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

        // Some skeletal vobs arrive with their base mesh not yet extracted (SkeletalMeshes empty but the model
        // does carry soft-skin geometry) — build it lazily. Interactive MOBs whose ONLY renderable content is a
        // node attachment (a lamp post's lamp, some doors) legitimately stay empty and fall through to the
        // attachment loop below — they must NOT be skipped (this was the "MOBs don't render" bug).
        if ( visual->SkeletalMeshes.empty() && model->GetMeshSoftSkinList()->NumInArray > 0 )
            WorldConverter::ExtractSkeletalMeshFromVob( model, visual );

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

            // Snapshot THIS instance's per-material alpha-clip diffuse handles while the model's shared texani
            // slots still describe it (see [[skeletal-texani-shared-slots]]) — the shadow-cascade recorder can't
            // re-run UpdateMeshLibTexAniState from a pool thread, so it indexes this instead. Same order as
            // visual->SkeletalMeshes iterates, which is stable for the rest of the frame (the map isn't mutated
            // after ExtractSkeletalMeshFromVob above). The outer vector grows monotonically and its inner vectors
            // keep their capacity across frames, so this settles into zero per-frame allocations.
            SkelUploadCache entry;
            if ( g_SkelMatSrvCount >= g_SkelMatSrvs.size() )
                g_SkelMatSrvs.emplace_back();
            {
                std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& matSrvs = g_SkelMatSrvs[g_SkelMatSrvCount];
                matSrvs.clear();
                for ( auto const& [mat, meshList] : visual->SkeletalMeshes )
                    matSrvs.push_back( ResolveShadowDiffuseSrv( mat ? mat->GetAniTexture() : nullptr ) );
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

                auto it = nodeAttachments.find( n );
                if ( it == nodeAttachments.end() ) {
                    WorldConverter::ExtractNodeVisual( n, node, nodeAttachments );
                    it = nodeAttachments.find( n );
                } else if ( !it->second.empty() && it->second[0] && it->second[0]->Visual != node->NodeVisual ) {
                    WorldConverter::ExtractNodeVisual( n, node, nodeAttachments );  // visual changed
                    it = nodeAttachments.find( n );
                }
                if ( it == nodeAttachments.end() ) continue;

                XMFLOAT4X4 attWorld;
                XMStoreFloat4x4( &attWorld, xmWorld * XMLoadFloat4x4( &boneCache[n] ) );
                for ( MeshVisualInfo* mvi : it->second ) {
                    if ( !mvi ) continue;
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
                            // srv resolved HERE (main thread) so the MT shadow-cascade recorder never has to read
                            // Gothic texture state; the main-view prepass/color paths still use attTex directly
                            // because they CacheIn, which a shadow-only alpha cutout deliberately must not do.
                            entry.attachments.push_back( { attMesh.get(), attTex, attInstView, vi->Vob,
                                ResolveShadowDiffuseSrv( attTex ) } );
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
                    g_ShadowSkelDraws[fi].push_back( { vi, visual, cached.instCb, cached.boneCb, cached.matSrvIndex } );
                for ( const FrameAttachDraw& a : cached.attachments )
                    g_ShadowAttachDraws[fi].push_back( a );
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
    if ( g_FrameSkelDraws.empty() && g_FrameAttachDraws.empty() ) return;

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );

    // Base skinned meshes (depth only).
    if ( !g_FrameSkelDraws.empty() && m_Pipelines.Skeletal.DepthPrepassPSO && m_Pipelines.Skeletal.RootSig ) {
        DX_ZONE( m_CmdList, "Depth Prepass (skeletal)" );
        m_CmdList->SetPipelineState( m_Pipelines.Skeletal.DepthPrepassPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_Pipelines.Skeletal.RootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        for ( const FrameSkelDraw& d : g_FrameSkelDraws ) {
            if ( !d.visual ) continue;
            zCModel* model = static_cast<zCModel*>(d.vobInfo->Vob->GetVisual());
            model->UpdateMeshLibTexAniState(); // before drawing we NEED to TexAni, the models share the same textures, causing incorrect textures if not done correctly.

            m_CmdList->SetGraphicsRootConstantBufferView( 1, d.instCb );
            m_CmdList->SetGraphicsRootConstantBufferView( 2, d.boneCb );
            for ( auto const& [mat, meshList] : d.visual->SkeletalMeshes ) {
                D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
                zCTexture* tex = mat ? mat->GetAniTexture() : nullptr;
                if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
                        if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                            D3D12Texture* d12 = D3D12Texture::From( gfx );
                            if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                        }
                    }
                }
                m_CmdList->SetGraphicsRootDescriptorTable( 3, srv );
                BindMaterialMaps( tex, 12 );   // b6 bindless normal/ORM indices (no-op in the depth prepass)
                for ( auto const& mesh : meshList ) {
                    if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer )
                        continue;
                    D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mesh->MeshVertexBuffer.get() );
                    D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mesh->MeshIndexBuffer.get() );
                    if ( !mvb->GetResource() || !mib->GetResource() ) continue;
                    const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExSkelVertexStruct ) };
                    m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
                    const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                    m_CmdList->IASetIndexBuffer( &ibv );
                    m_CmdList->DrawIndexedInstanced( static_cast<UINT>( mesh->Indices.size() ), 1, 0, 0, 0 );
                }
            }
        }
    }

    // Node attachments (depth only) through the VOB attachment depth PSO (Fatness/Scaling variant — see
    // Vob.hlsl's VSDepthAttach; must match DrawSkeletalColor's attachment PSO choice or the prepass depth
    // won't reflect the same inflate/scale as the color pass, the same class of bug the wind fix addressed).
    if ( !g_FrameAttachDraws.empty() && m_Pipelines.World.DepthPrepassVobAttachPSO && m_Pipelines.World.RootSig ) {
        DX_ZONE( m_CmdList, "Depth Prepass (attachments)" );
        m_CmdList->SetPipelineState( m_Pipelines.World.DepthPrepassVobAttachPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        for ( const FrameAttachDraw& a : g_FrameAttachDraws ) {
            if ( !a.mesh || !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() ) continue;
            D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
            D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
            if ( !mvb->GetResource() || !mib->GetResource() ) continue;

            D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
            if ( a.tex && a.tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                if ( MyDirectDrawSurface7* surface = a.tex->GetSurface() ) {
                    if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                        D3D12Texture* d12 = D3D12Texture::From( gfx );
                        if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                    }
                }
            }
            m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );
            BindMaterialMaps( a.tex, 10 );   // b6 bindless normal/ORM indices (no-op in the depth prepass)

            const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
            const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, a.instView };
            m_CmdList->IASetVertexBuffers( 0, 2, views );
            const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
            m_CmdList->IASetIndexBuffer( &ibv );
            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( a.mesh->Indices.size() ), 1, 0, 0, 0 );
        }
    }
}


void D3D12GraphicsEngine::DrawSkeletalColor() {
    // P2.9b-4b (post-cull): draw the skeletal base meshes + node attachments collected by PrepareFrameSkeletals,
    // lit through the tile grid. Base via m_Pipelines.Skeletal.PSO, attachments via m_Pipelines.World.VobPSO — same PSOs/binds as before the
    // 4b split, just consuming the shared records (no re-upload, no re-run of the once/frame animation update).
    if ( !m_FrameOpen || !m_Pipelines.Skeletal.PSO || !m_Pipelines.Skeletal.RootSig || !m_DepthBuffer ) return;
    if ( g_FrameSkelDraws.empty() && g_FrameAttachDraws.empty() ) return;

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
    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    unsigned int drawnTris = 0;

    // Base skinned meshes (lit).
    if ( !g_FrameSkelDraws.empty() ) {
        DX_ZONE( m_CmdList, "Draw skeletal" );
        m_CmdList->SetPipelineState( m_Pipelines.Skeletal.PSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_Pipelines.Skeletal.RootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->SetGraphicsRoot32BitConstants( 4, 8, &fog, 0 );   // b3 fog
        BindFrameLights( 5, 6, 7, 8 );   // light SRV(t1)+count(b4)+grid(t2)+index(t3) — MUST set all (see BindFrameLights)
        m_CmdList->SetGraphicsRootConstantBufferView( 9, m_ShadowCBGpu[m_FrameIndex] );        // b5 shadow CB
        m_CmdList->SetGraphicsRootDescriptorTable( 10, GetSrvGpuHandle( m_ShadowSrvSlot ) );   // t4 shadow map
        m_CmdList->SetGraphicsRootDescriptorTable( 11, GetSrvGpuHandle( m_PointShadowSrvSlot ) ); // t5 point-shadow cubes
        m_CmdList->SetGraphicsRoot32BitConstants( 13, 1, &m_ActiveAOMaskSrvSlot, 0 );   // b8 AOCB (simple SSAO mask)
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        for ( const FrameSkelDraw& d : g_FrameSkelDraws ) {
            if ( !d.visual ) continue;
            zCModel* model = static_cast<zCModel*>(d.vobInfo->Vob->GetVisual());
            model->UpdateMeshLibTexAniState(); // before drawing we NEED to TexAni, the models share the same textures, causing incorrect textures if not done correctly.

            m_CmdList->SetGraphicsRootConstantBufferView( 1, d.instCb );
            m_CmdList->SetGraphicsRootConstantBufferView( 2, d.boneCb );
            for ( auto const& [mat, meshList] : d.visual->SkeletalMeshes ) {
                D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
                zCTexture* tex = mat ? mat->GetAniTexture() : nullptr;
                if ( tex && tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
                        if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                            D3D12Texture* d12 = D3D12Texture::From( gfx );
                            if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                        }
                    }
                }
                m_CmdList->SetGraphicsRootDescriptorTable( 3, srv );
                BindMaterialMaps( tex, 12 );   // b6 bindless normal/ORM indices (no-op in the depth prepass)
                for ( auto const& mesh : meshList ) {
                    if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer )
                        continue;
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
        }
    }

    // Node attachments (lit) through the VOB attachment PSO (Fatness/Scaling variant — see Vob.hlsl's
    // VSMainAttach; non-morph attachments get Fatness=0/Scaling=1, a no-op, so this is a drop-in replacement
    // for the plain VobPSO). BindFrameLights() is REQUIRED — the VOB PS reads the light count/grid, so an
    // unbound count would run the loop on garbage → GPU TDR hang.
    if ( !g_FrameAttachDraws.empty() && m_Pipelines.World.VobAttachPSO && m_Pipelines.World.RootSig ) {
        DX_ZONE( m_CmdList, "Draw attachments" );
        m_CmdList->SetPipelineState( m_Pipelines.World.VobAttachPSO.Get() );
        m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
        m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
        m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );   // b1 fog (VOB root sig)
        BindFrameLights();
        m_CmdList->SetGraphicsRootConstantBufferView( 7, m_ShadowCBGpu[m_FrameIndex] );        // b3 shadow CB
        m_CmdList->SetGraphicsRootDescriptorTable( 8, GetSrvGpuHandle( m_ShadowSrvSlot ) );    // t4 shadow map
        m_CmdList->SetGraphicsRootDescriptorTable( 9, GetSrvGpuHandle( m_PointShadowSrvSlot ) ); // t5 point-shadow cubes
        m_CmdList->SetGraphicsRoot32BitConstants( 12, 1, &m_ActiveAOMaskSrvSlot, 0 );   // b7 AOCB (simple SSAO mask)
        m_CmdList->RSSetViewports( 1, &vp );
        m_CmdList->RSSetScissorRects( 1, &sc );
        m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        for ( const FrameAttachDraw& a : g_FrameAttachDraws ) {
            if ( !a.mesh || !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() ) continue;
            D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
            D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
            if ( !mvb->GetResource() || !mib->GetResource() ) continue;

            D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
            if ( a.tex && a.tex->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                if ( MyDirectDrawSurface7* surface = a.tex->GetSurface() ) {
                    if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
                        D3D12Texture* d12 = D3D12Texture::From( gfx );
                        if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
                    }
                }
            }
            m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );
            BindMaterialMaps( a.tex, 10 );   // b6 bindless normal/ORM indices (no-op in the depth prepass)

            const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
            const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, a.instView };
            m_CmdList->IASetVertexBuffers( 0, 2, views );
            const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
            m_CmdList->IASetIndexBuffer( &ibv );
            m_CmdList->DrawIndexedInstanced( static_cast<UINT>( a.mesh->Indices.size() ), 1, 0, 0, 0 );
            drawnTris += static_cast<unsigned int>( a.mesh->Indices.size() ) / 3;
        }
    }

    rs.RendererInfo.FrameDrawnTriangles += drawnTris;
}
