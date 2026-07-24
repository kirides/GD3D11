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

#include <dxcapi.h>

#include "D3D12RenderQueue.h"
#include "InstancingUtils.h"

// imgui_impl_dx12 calls CreateDXGIFactory1 directly (for tearing detection). dxgi.dll is present on
// every Windows 7+ and the D3D11 fallback swapchain already needs it at runtime, so a load-time link
// here is safe — it does NOT reintroduce the D3D12 soft-dependency that lets old systems fall back.
#pragma comment(lib, "dxgi.lib")

// TODO: Replace dependency with runtime dynamic load of dxcompiler.dll (like D3D12CreateDevice) to avoid shipping it on systems that don't support D3D12.
#pragma comment(lib, "dxcompiler.lib")

using Microsoft::WRL::ComPtr;

static D3D12_HEAP_TYPE DefaultUploadHeapType = D3D12_HEAP_TYPE_UPLOAD;

// Mid-burst flush threshold for the batched copy uploader: caps how much staging (UPLOAD-heap VA —
// scarce in the 32-bit process) a no-Present world-load burst can accumulate before we submit the
// open batch and let its staging buffers recycle. Sized so typical per-frame streaming never trips it.
static constexpr UINT64 kCopyBatchFlushThresholdBytes = 32ull * 1024 * 1024;

static bool GetSkipDefaultHeapCopyAfterUpload() {
	return DefaultUploadHeapType == D3D12_HEAP_TYPE_GPU_UPLOAD;
}

// Why is BeginEvent not working as intended with Context on debugging this 32 bit app !!
// A global ring-buffer tracking recent recording phases mapped directly to command list slots
struct CPUBreadcrumbContext {
    UINT opIndex = 0;
    const wchar_t* pContextText = nullptr;
};

// Allocate space for tracking up to 2048 sequential draw states per frame execution
inline thread_local std::array<CPUBreadcrumbContext, 2048> g_CpuContextHistory;
inline thread_local UINT g_CurrentRecordingOpIndex = 0;

struct DXMarker {
    DXMarker( const Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& commandList, const wchar_t* text ) :
        c( commandList.Get() )
    {
        if ( c && text ) {
            // Track exactly what string context we are assigning to the CURRENT command slot
            if ( g_CurrentRecordingOpIndex < g_CpuContextHistory.size() ) {
                g_CpuContextHistory[g_CurrentRecordingOpIndex] = { g_CurrentRecordingOpIndex, text };
            }

            UINT byteSize = static_cast<UINT>( (wcslen( text ) + 1) * sizeof( wchar_t ) );
            c->BeginEvent( 0, text, byteSize );

            // Increment tracking slot to match what DRED maps under the hood
            g_CurrentRecordingOpIndex++;
        }
    }

    ~DXMarker() {
        if ( c ) {
            c->EndEvent();
            g_CurrentRecordingOpIndex++;
        }
    }

    DXMarker( const DXMarker& ) = delete;
    DXMarker& operator=( const DXMarker& ) = delete;

private:
    ID3D12GraphicsCommandList* c;
};

// Reset this counter to 0 EVERY TIME you call Reset() on your command list!
inline void ResetCpuContextTracker() {
    g_CurrentRecordingOpIndex = 0;
    for ( auto& slot : g_CpuContextHistory ) {
        slot.pContextText = nullptr;
    }
}

#define DX_ZONE(cmdList, nameStr) DXMarker marker_local_evt_##__LINE__(cmdList, L##nameStr)

struct FrameVobUpload { MeshVisualInfo* visual; D3D12_VERTEX_BUFFER_VIEW instView; UINT numInstances; };

namespace {
    // Swapchain / final-output format. R10G10B10A2 (10-bit) instead of R8: the tonemapped output has much
    // finer gradients (kills banding in sky/fog/soft shadows) at the same 32bpp. Also the format the 2D UI +
    // ImGui + the tonemap resolve write to. Flip-model swapchains support R10G10B10A2_UNORM natively.
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    // HDR scene-color format: the 3D world/VOB/skeletal/water/decal/particle passes accumulate lighting here in
    // linear-ish FLOAT (values may exceed 1.0 — bright sun + additive point lights no longer clip to white), then
    // a fullscreen tonemap resolves it into the swapchain. R16F 4-channel = 64bpp intermediate (recreated on resize).
    constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr UINT kSrvHeapCapacity = 65536;                  // texture SRVs (tier-1 max is 1M; bump-allocated)
    constexpr UINT kUIVertexBufferBytes = 16 * 1024 * 1024;   // per-frame 2D vertex ring (~280k ExVertex)
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

    // Phase-2 water shader (MVP). Same wrapped-world-mesh vertex as the opaque pass, but the packed
    // TexCoord2 (@28, half2) carries the per-material UV-scroll delta (set in WorldConverter for water
    // materials); the VS scrolls the diffuse UV by (delta * totalTime) exactly like VS_ExWater. The PS
    // samples the scrolled diffuse, applies distance fog, and outputs a constant alpha so the surface
    // blends translucently over the already-drawn opaque scene beneath it. Refraction / reflection /
    // scene-color + depth sampling + Gerstner waves (the full D3D11 PS_Water/VS_ExWater) are out of MVP.

    // Water surfaces peeled out of the opaque world pass (DrawWorldMesh) and drawn later, alpha-blended,
    // by DrawWaterSurfaces. Both run on the same thread within one frame (OnStartWorldRendering), so a
    // file-scope scratch map is safe — grouped by texture to minimize SRV binds. Cleared each frame.
    std::unordered_map<zCTexture*, std::vector<MeshInfo*>> g_FrameWaterSurfaces;

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
    struct FramePointShadow { DirectX::XMFLOAT3 posWS; float range; UINT slot; bool renderStatic; };
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
    struct FrameSkelDraw   { SkeletalVobInfo* vobInfo;  SkeletalMeshVisualInfo* visual; D3D12_GPU_VIRTUAL_ADDRESS instCb; D3D12_GPU_VIRTUAL_ADDRESS boneCb; };
    // owner = the skeletal vob this attachment hangs off of (its NPC/MOB) — needed so point-shadow self-shadow
    // exclusion (BuildPointShadowExcludeList) can skip a torch-holding NPC's own attachments too, not just its
    // base mesh; unused (nullptr-safe) by the main-view/CSM consumers, which don't exclude anything.
    struct FrameAttachDraw { MeshInfo* mesh; zCTexture* tex; D3D12_VERTEX_BUFFER_VIEW instView; const zCVob* owner; };
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
        std::vector<FrameAttachDraw> attachments;
    };
    gtl::flat_hash_map<SkeletalVobInfo*, SkelUploadCache> g_SkelUploadCache;

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
        if ( rs.RendererSettings.AtmosphericScattering ) {
            return Engine::GAPI->GetFogColor();
        }
        return DirectX::XMLoadFloat3( &rs.GraphicsState.FF_FogColor );
    }

    // Builds this frame's fog constants from Gothic's sky state. FogColor = GetSceneFogColorXM() (0..1,
    // weather / sky-override / AtmosphericScattering-mode correct — the same color used to clear the sky);
    // FogNear/FogFar = GraphicsState.FF_FogNear/FF_FogFar, the same values D3D11's ComputeFog() uses (set
    // once per frame in GSky.cpp from sky->GetMasterState()->FogDist, with Gothic's hardcoded 0.3 near
    // factor) — NOT GetFarZ(), which is an unrelated atmospheric-perspective far plane the height-fog PFX
    // uses for its density falloff and is typically much smaller than FogDist, which was making the fog
    // ramp in far too aggressively.
    FogConstants MakeFogConstants() {
        FogConstants fog = {};
        DirectX::XMFLOAT3 fc;
        DirectX::XMStoreFloat3( &fc, GetSceneFogColorXM() );
        fog.FogColor[0] = fc.x; fog.FogColor[1] = fc.y; fog.FogColor[2] = fc.z;

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

    D3D12_RESOURCE_BARRIER TransitionBarrier( ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after ) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        return b;
    }

    gtl::flat_hash_map<BaseVisualInfo*, int16_t> g_vobInfoVisualToBucket;
    std::vector<BaseVisualInfo*> g_vobInfoVisualIndexToVisualInfo;
    RenderView g_GeometryPassVobs;
    RenderView g_ShadowPassVobs[3]; // per shadow cascade
}

D3D12GraphicsEngine::D3D12GraphicsEngine() {
    m_LineRenderer = std::make_unique<D3D12LineRenderer>();
    m_Resolution = m_NewResolution = Engine::GAPI->GetRendererState().RendererSettings.LoadedResolution;
}

D3D12GraphicsEngine::~D3D12GraphicsEngine() {
    if ( m_SwapChainReady ) {
        WaitForGpuIdle();
        // Force-run all remaining cleanups
        for ( UINT i = 0; i < kBackBufferCount; ++i ) {
            for ( auto& cleanupCallback : m_PerFrameCleanupItems[i] ) {
                cleanupCallback();
            }
            m_PerFrameCleanupItems[i].clear();
        }
    }
    if ( m_FenceEvent ) CloseHandle( m_FenceEvent );
    if ( m_UploadEvent ) CloseHandle( m_UploadEvent );
}

XRESULT D3D12GraphicsEngine::Init() {
    if ( !m_Device.Init() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: device creation failed.";
        return XR_FAILED;
    }
    if ( !CreateAllocators() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create allocators.";
        return XR_FAILED;
    }
    if ( !CreateUploadObjects() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create upload objects.";
        return XR_FAILED;
    }
    if ( !InitCopyQueue() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to initialize the copy queue.";
        return XR_FAILED;
    }
    if ( !CreateSrvHeap() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create SRV heap.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.Init( &m_Device, &m_ShaderBackend ) ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to init the pipeline-state module.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateUI() || !CreateUIVertexBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the 2D/UI pipeline.";
        return XR_FAILED;
    }
    if ( !CreateWhiteTexture() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the white fallback texture.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateWorld() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the world-mesh pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateDepthPrepass() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the depth prepass pipeline.";
        return XR_FAILED;
    }
    if ( !CreateWorldIndirect() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the world ExecuteIndirect resources.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateLightCull() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the light-culling compute pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateVob() || !CreateVobInstanceBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the VOB pipeline.";
        return XR_FAILED;
    }
    if ( !CreateLightBuffer() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the point-light buffer.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateSkeletal() || !CreateSkeletalConstantBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the skeletal pipeline.";
        return XR_FAILED;
    }
    if ( !CreateShadowMap() ) {
        // Fatal: the lit world PSO samples the shadow map (t4) + CB (b3) unconditionally, so a missing map would
        // leave those root slots unbound. Failing here cleanly falls back to D3D11 (D3D12 is dev-forced/opt-in).
        // Runs after the depth-prepass + VOB + skeletal pipelines so the caster PSOs can reuse all three depth VS blobs.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the sun shadow map.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreatePointShadow() || !CreatePointShadowResources() ) {
        // Fatal: the lit PSOs sample the cube array (t5) unconditionally once P2.10d lands, so a missing resource
        // would leave that root slot unbound. Failing here cleanly falls back to D3D11 (D3D12 is dev-forced).
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create point-light shadow cubes.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateWater() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the water pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateParticle() || !CreateParticleInstanceBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the particle pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateDecal() || !CreateDecalQuadVB() || !CreateDecalInstanceBuffers() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the decal pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateTonemap() ) {
        // Fatal: the 3D scene PSOs now target the HDR scene-color RT (kSceneColorFormat), so without the tonemap
        // resolve nothing reaches the swapchain. Failing here cleanly falls back to D3D11 (D3D12 is dev-forced).
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the tonemap pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateLumAdapt() || !CreateLumAdaptedBuffer() ) {
        // Fatal: Tonemap.hlsl's PS now reads m_LumAdaptedBuffer (t1) unconditionally every frame — a missing
        // buffer would leave that root SRV unbound. Same reasoning as the tonemap PSO itself just above.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the dynamic-exposure (auto-exposure) pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreatePreview() ) {
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the inventory-item preview pipeline.";
        return XR_FAILED;
    }
    if ( !m_Pipelines.CreateBloom() ) {
        // Non-fatal: bloom is an opt-in visual enhancement (RendererSettings.EnableBloom, default off), not a
        // required resource any other PSO samples unconditionally — unlike tonemap/shadow/point-shadow above.
        // RenderBloom() guards on the PSOs existing and just skips the effect if this failed.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the bloom pipeline (bloom will be unavailable).";
    }
    if ( !m_Pipelines.CreateGhost() ) {
        // Non-fatal: ghost/transparency VOBs are a niche effect (invisible-potion/fade items). DrawGhostVobs()
        // guards on Ghost.PSO existing and, if this failed, simply drains+discards Engine::GAPI->TransparencyVobs
        // every frame instead of drawing it — GothicAPI still needs that drain to avoid an unbounded per-frame leak.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the ghost pipeline (ghost VOBs will be invisible).";
    }
    if ( !m_Pipelines.CreateVideo() ) {
        // Non-fatal: Bink cutscene playback (zBinkPlayer.cpp) is a niche path. DrawVertexArray's PS_Video branch
        // guards on Video.PSO existing and falls back to the normal FF/UI draw (a black/untextured quad) if not.
        LogWarn() << "D3D12GraphicsEngine::Init: failed to create the video (Bink) pipeline (cutscenes will not render).";
    }
    LogInfo() << "D3D12GraphicsEngine initialized (device + 2D + world + VOB + skeletal + water + particle + decal + HDR tonemap pipelines up). Swapchain is created once the game window is set.";
    return XR_SUCCESS;
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

bool D3D12GraphicsEngine::CreateAllocators() {
    D3D12MA::ALLOCATOR_DESC allocatorDesc{};
    allocatorDesc.pDevice = m_Device.GetDevice();
    allocatorDesc.pAdapter = m_Device.GetAdapter();
    allocatorDesc.Flags = D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED;

    if ( GetModuleHandleA( "renderdoc.dll" ) != NULL ) {
        allocatorDesc.Flags |= D3D12MA::ALLOCATOR_FLAGS::ALLOCATOR_FLAG_ALWAYS_COMMITTED;
    }

    if (FAILED(D3D12MA::CreateAllocator(&allocatorDesc, m_Allocator.ReleaseAndGetAddressOf()))) {
        return false;
    }
	if ( m_Allocator->IsGPUUploadHeapSupported() ) {
	    // Holy hell, D3D12_HEAP_TYPE_GPU_UPLOAD is fucking expensive ?? Do not use if doing many updates! this completely tanks FPS
	    // for example for dynamic verticies, this causes 99% usage in FixedFunction vertex updates
		// DefaultUploadHeapType = D3D12_HEAP_TYPE_GPU_UPLOAD;
	}
    
    return m_Allocator != nullptr;
}

bool D3D12GraphicsEngine::CreateUploadObjects() {
    ID3D12Device* device = m_Device.GetDevice();
    if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS( m_UploadAllocator.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_UploadAllocator.Get(), nullptr, IID_PPV_ARGS( m_UploadCmdList.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_UploadCmdList->Close();
    if ( FAILED( device->CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( m_UploadFence.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_UploadEvent = CreateEvent( nullptr, FALSE, FALSE, nullptr );
    return m_UploadEvent != nullptr;
}

bool D3D12GraphicsEngine::InitCopyQueue() {
    ID3D12Device* device = m_Device.GetDevice();
    if ( !device ) return false;

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    if ( FAILED( device->CreateCommandQueue( &queueDesc, IID_PPV_ARGS( m_CopyQueue.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    if ( FAILED( device->CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( m_CopyFence.ReleaseAndGetAddressOf() ) ) ) )
        return false;

    m_CopyFenceEvent = CreateEvent( nullptr, FALSE, FALSE, nullptr );
    return m_CopyFenceEvent != nullptr;
}

void D3D12GraphicsEngine::ReleaseCompletedCopyResources( UINT64 fenceValue ) {
    // Caller holds m_CopyQueueMutex. Pending batches are pushed in ascending fence order, so the front
    // is always the oldest — stop at the first not-yet-completed batch.
    while ( !m_PendingCopyReleases.empty() ) {
        auto& pending = m_PendingCopyReleases.front();
        if ( pending.FenceValue > fenceValue ) break;
        // Staging buffers are done being read: drop them, but recycle the command allocator+list for
        // reuse by the next batch (their copies have completed, so Reset is now legal).
        if ( pending.CopyAllocator && pending.CopyCommandList ) {
            m_FreeCopyCmdObjects.push_back( CopyCmdObjects{
                std::move( pending.CopyAllocator ), std::move( pending.CopyCommandList ) } );
        }
        m_PendingCopyReleases.pop_front(); // O(1) popped release, zero memory shifts
    }
}

void D3D12GraphicsEngine::WaitForCopyFence( UINT64 fenceValue ) {
    if ( !m_CopyFence || !m_CopyFenceEvent ) return;
    if ( m_CopyFence->GetCompletedValue() >= fenceValue ) return;
    m_CopyFence->SetEventOnCompletion( fenceValue, m_CopyFenceEvent );
    WaitForSingleObject( m_CopyFenceEvent, INFINITE );
}

void D3D12GraphicsEngine::TransitionTextureToSRVOnDirectQueue( ID3D12Resource* texture ) {
    if ( !texture || !m_Device.GetDevice() ) return;

    // Use the active frame's existing command list instead of creating temporary allocators & command lists
    if ( m_FrameOpen && m_CmdList ) {
        auto toSRV = TransitionBarrier( texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
        m_CmdList->ResourceBarrier( 1, &toSRV );
        return;
    }

    // Fallback if called outside frame boundaries: execute asynchronously on direct queue WITHOUT CPU blocking
    ID3D12Device* device = m_Device.GetDevice();
    ComPtr<ID3D12CommandAllocator> transitionAllocator;
    ComPtr<ID3D12GraphicsCommandList> transitionCmdList;

    if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS( transitionAllocator.ReleaseAndGetAddressOf() ) ) ) )
        return;
    if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        transitionAllocator.Get(), nullptr, IID_PPV_ARGS( transitionCmdList.ReleaseAndGetAddressOf() ) ) ) )
        return;

    auto toSRV = TransitionBarrier( texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
    transitionCmdList->ResourceBarrier( 1, &toSRV );
    if ( FAILED( transitionCmdList->Close() ) ) return;

    ID3D12CommandList* lists[] = { transitionCmdList.Get() };
    m_Device.GetDirectQueue()->ExecuteCommandLists( 1, lists );

    const UINT64 waitValue = ++m_UploadFenceValue;
    if ( FAILED( m_Device.GetDirectQueue()->Signal( m_UploadFence.Get(), waitValue ) ) ) return;

    // OPTIMIZATION: Defer deletion to m_PerFrameCleanupItems via fence value instead of CPU blocking with WaitForSingleObject!
    QueueCleanupJob( [allocator = transitionAllocator, list = transitionCmdList]() {
        // Keeps resources alive until current frame fence is reached on GPU
    } );
}

bool D3D12GraphicsEngine::UploadTextureSubresources( ID3D12Resource* dst, const D3D12_SUBRESOURCE_DATA* subresources, UINT numSubresources ) {
    if ( !dst || !subresources || numSubresources == 0 ) return false;
    ID3D12Device* device = m_Device.GetDevice();

    D3D12_RESOURCE_DESC desc = dst->GetDesc();

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts( numSubresources );
    std::vector<UINT>    numRows( numSubresources );
    std::vector<UINT64>  rowSizes( numSubresources );
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints( &desc, 0, numSubresources, 0, layouts.data(), numRows.data(), rowSizes.data(), &totalBytes );

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = totalBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = DefaultUploadHeapType;

	ComPtr<D3D12MA::Allocation> uploadAllocation;
	ComPtr<ID3D12Resource> upload;
	if ( FAILED( m_Allocator->CreateResource(
		&allocDesc,
		&bufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		uploadAllocation.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( upload.ReleaseAndGetAddressOf() ) ) ) ) {
		return false;
	}

	BYTE* mapped = nullptr;
	D3D12_RANGE noRead = { 0, 0 };
	if ( FAILED( upload->Map( 0, &noRead, reinterpret_cast<void**>(&mapped) ) ) )
		return false;

	for ( UINT i = 0; i < numSubresources; ++i ) {
		BYTE* dstSlice = mapped + layouts[i].Offset;
		const BYTE* srcData = reinterpret_cast<const BYTE*>( subresources[i].pData );
		for ( UINT row = 0; row < numRows[i]; ++row ) {
			memcpy( dstSlice + static_cast<SIZE_T>( layouts[i].Footprint.RowPitch ) * row,
				srcData + static_cast<SIZE_T>( subresources[i].RowPitch ) * row,
				static_cast<SIZE_T>( rowSizes[i] ) );
		}
	}
	upload->Unmap( 0, nullptr );

	// Record the copy into the shared, batched copy command list (submitted once at flush). The lock
	// serializes recording into the single list and guards the batch bookkeeping — see the header note.
	std::lock_guard<std::mutex> lock( m_CopyQueueMutex );
	if ( !BeginCopyBatch() )
		return false;

	for ( UINT i = 0; i < numSubresources; ++i ) {
		D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
		dstLoc.pResource = dst;
		dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLoc.SubresourceIndex = i;

		D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
		srcLoc.pResource = upload.Get();
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLoc.PlacedFootprint = layouts[i];

		m_CopyBatchList->CopyTextureRegion( &dstLoc, 0, 0, 0, &srcLoc, nullptr );
	}

	// Keep the staging buffer alive until the batch's copies complete (moved to the pending list at flush).
	m_CopyBatchUploadAllocs.push_back( std::move( uploadAllocation ) );
	m_CopyBatchUploadResources.push_back( std::move( upload ) );
	m_CopyBatchBytes += totalBytes;

	// Bound VA/memory growth during a long world-load burst that never Presents: flush mid-burst once
	// the accumulated staging crosses the threshold. Still one submit per threshold-worth, not per texture.
	if ( m_CopyBatchBytes >= kCopyBatchFlushThresholdBytes )
		FlushTextureUploadsLocked();

	ReleaseCompletedCopyResources( m_CopyFence->GetCompletedValue() );
	return true;
}

bool D3D12GraphicsEngine::UploadBufferData( ID3D12Resource* dst, const void* srcData, UINT64 sizeInBytes ) {
	if ( !dst || !srcData || sizeInBytes == 0 ) return false;

	// Create a temporary upload buffer for staging
	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = sizeInBytes;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = DefaultUploadHeapType;

	ComPtr<D3D12MA::Allocation> uploadAllocation;
	ComPtr<ID3D12Resource> upload;
	if ( FAILED( m_Allocator->CreateResource(
		&allocDesc,
		&bufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		uploadAllocation.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( upload.ReleaseAndGetAddressOf() ) ) ) ) {
		return false;
	}

	BYTE* mapped = nullptr;
	D3D12_RANGE noRead = { 0, 0 };
	if ( FAILED( upload->Map( 0, &noRead, reinterpret_cast<void**>(&mapped) ) ) )
		return false;
	memcpy( mapped, srcData, sizeInBytes );
	upload->Unmap( 0, nullptr );

	// Record into the shared batched copy list (submitted once at flush) — same rationale as
	// UploadTextureSubresources: no per-call CreateCommandList / ExecuteCommandLists / cross-queue Wait.
	std::lock_guard<std::mutex> lock( m_CopyQueueMutex );
	if ( !BeginCopyBatch() )
		return false;

	m_CopyBatchList->CopyBufferRegion( dst, 0, upload.Get(), 0, sizeInBytes );

	m_CopyBatchUploadAllocs.push_back( std::move( uploadAllocation ) );
	m_CopyBatchUploadResources.push_back( std::move( upload ) );
	m_CopyBatchBytes += sizeInBytes;

	if ( m_CopyBatchBytes >= kCopyBatchFlushThresholdBytes )
		FlushTextureUploadsLocked();

	ReleaseCompletedCopyResources( m_CopyFence->GetCompletedValue() );
	return true;
}

bool D3D12GraphicsEngine::BeginCopyBatch() {
	if ( m_CopyBatchOpen ) return true;
	ID3D12Device* device = m_Device.GetDevice();
	if ( !device ) return false;

	// Recycle a completed (allocator,list) pair if one is available, else create one.
	if ( !m_FreeCopyCmdObjects.empty() ) {
		m_CopyBatchAllocator = std::move( m_FreeCopyCmdObjects.back().Allocator );
		m_CopyBatchList = std::move( m_FreeCopyCmdObjects.back().List );
		m_FreeCopyCmdObjects.pop_back();
		if ( FAILED( m_CopyBatchAllocator->Reset() ) ) return false;         // safe: its copies completed (fence-gated recycle)
		if ( FAILED( m_CopyBatchList->Reset( m_CopyBatchAllocator.Get(), nullptr ) ) ) return false;
	} else {
		if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_COPY,
			IID_PPV_ARGS( m_CopyBatchAllocator.ReleaseAndGetAddressOf() ) ) ) )
			return false;
		if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_COPY,
			m_CopyBatchAllocator.Get(), nullptr, IID_PPV_ARGS( m_CopyBatchList.ReleaseAndGetAddressOf() ) ) ) )
			return false;
		// CreateCommandList returns the list already open for recording.
	}
	m_CopyBatchOpen = true;
	m_CopyBatchBytes = 0;
	return true;
}

void D3D12GraphicsEngine::FlushTextureUploadsLocked() {
	if ( !m_CopyBatchOpen ) return;
	m_CopyBatchOpen = false;

	if ( FAILED( m_CopyBatchList->Close() ) ) {
		// Drop the batch; its staging buffers free with the ComPtr vectors, cmd objects are discarded.
		m_CopyBatchUploadAllocs.clear();
		m_CopyBatchUploadResources.clear();
		m_CopyBatchBytes = 0;
		return;
	}

	ID3D12CommandList* lists[] = { m_CopyBatchList.Get() };
	m_CopyQueue->ExecuteCommandLists( 1, lists );

	const UINT64 fenceValue = ++m_CopyFenceValue;
	m_CopyQueue->Signal( m_CopyFence.Get(), fenceValue );

	// ONE cross-queue GPU wait for the whole batch: the direct (render) queue won't sample any of
	// these textures until the batch's copies complete. Replaces the old per-texture render stall.
	m_Device.GetDirectQueue()->Wait( m_CopyFence.Get(), fenceValue );

	PendingCopyRelease pending;
	pending.FenceValue = fenceValue;
	pending.UploadAllocations = std::move( m_CopyBatchUploadAllocs );
	pending.UploadResources = std::move( m_CopyBatchUploadResources );
	pending.CopyAllocator = std::move( m_CopyBatchAllocator );
	pending.CopyCommandList = std::move( m_CopyBatchList );
	m_PendingCopyReleases.push_back( std::move( pending ) );

	m_CopyBatchUploadAllocs.clear();
	m_CopyBatchUploadResources.clear();
	m_CopyBatchBytes = 0;
}

void D3D12GraphicsEngine::FlushTextureUploads() {
	std::lock_guard<std::mutex> lock( m_CopyQueueMutex );
	FlushTextureUploadsLocked();
}

bool D3D12GraphicsEngine::CreateSrvHeap() {
	ID3D12Device* device = m_Device.GetDevice();
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	desc.NumDescriptors = kSrvHeapCapacity;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if ( FAILED( device->CreateDescriptorHeap( &desc, IID_PPV_ARGS( m_SrvHeap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_SrvDescriptorSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
	m_SrvHeapCapacity = kSrvHeapCapacity;
	m_SrvAllocated = 0;
	return true;
}

UINT D3D12GraphicsEngine::AllocateSrvSlot() {
	std::lock_guard<std::mutex> lock( m_SrvHeapMutex );

	// Try to reuse a freed slot first
	if ( !m_FreeSrvSlots.empty() ) {
		UINT slot = m_FreeSrvSlots.back();
		m_FreeSrvSlots.pop_back();
		return slot;
	}

	// Fall back to bump allocation
	if ( m_SrvAllocated >= m_SrvHeapCapacity ) {
		LogWarn() << "D3D12: SRV heap exhausted (" << m_SrvHeapCapacity << " descriptors).";
		return UINT_MAX;
	}
	return m_SrvAllocated++;
}

void D3D12GraphicsEngine::FreeSrvSlot( UINT slot ) {
	if ( slot == UINT_MAX
		|| slot == m_WhiteTexture->GetSrvSlot()
		|| slot == m_BlackTexture->GetSrvSlot()
		|| slot == m_DefaultOrmTexture->GetSrvSlot()
		) {
		return;
	}

	ID3D12Device* device = m_Device.GetDevice();

	std::lock_guard<std::mutex> lock( m_SrvHeapMutex );

	// Nullify the descriptor to prevent pointing to dead memory
	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = GetSrvCpuHandleLocked( slot );

	D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
	nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	nullDesc.Texture2D.MipLevels = 1;

	// Bind white texture to free slot.

	// Writing a null resource view to this descriptor slot safely clears it
	device->CreateShaderResourceView( m_WhiteTexture->GetResource(), &nullDesc, cpuHandle );

	m_FreeSrvSlots.push_back( slot );
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvCpuHandleLocked( UINT slot ) const {
	// Caller holds m_SrvHeapMutex.
	if ( std::ranges::contains( m_FreeSrvSlots, slot ) ) {
		// Ensure invalid slots provide some texture instead of breaking
		return GetSrvCpuHandleLocked( m_BlackTexture->GetSrvSlot() );
	}

	D3D12_CPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(slot) * m_SrvDescriptorSize;
	return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvGpuHandleLocked( UINT slot ) const {
	// Caller holds m_SrvHeapMutex.
	if ( std::ranges::contains( m_FreeSrvSlots, slot ) ) {
		// Ensure invalid slots provide some texture instead of breaking
		return GetSrvGpuHandleLocked( m_BlackTexture->GetSrvSlot() );
	}

	D3D12_GPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetGPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<UINT64>(slot) * m_SrvDescriptorSize;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvCpuHandle( UINT slot ) const {
	std::lock_guard<std::mutex> lock( m_SrvHeapMutex );
	return GetSrvCpuHandleLocked( slot );
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12GraphicsEngine::GetSrvGpuHandle( UINT slot ) const {
	std::lock_guard<std::mutex> lock( m_SrvHeapMutex );
	return GetSrvGpuHandleLocked( slot );
}



bool D3D12GraphicsEngine::CreateUIVertexBuffers() {
	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = DefaultUploadHeapType;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = kUIVertexBufferBytes;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	for ( UINT i = 0; i < kBackBufferCount; ++i ) {
		if ( FAILED( m_Allocator->CreateResource( &allocDesc, &bufDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, m_UIVertexBufferAlloc[i].ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( m_UIVertexBuffer[i].ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_UIVertexBuffer[i]->SetName( i == 0 ? L"UIVertexRing0" : L"UIVertexRing1" );
		D3D12_RANGE noRead = { 0, 0 };
		if ( FAILED( m_UIVertexBuffer[i]->Map( 0, &noRead, reinterpret_cast<void**>( &m_UIVertexBufferPtr[i] ) ) ) )
			return false;
	}
	m_UIVertexBufferCapacity = kUIVertexBufferBytes;
	return true;
}

bool D3D12GraphicsEngine::CreateWhiteTexture() {
	CreateTexture( m_WhiteTexture );
	const uint32_t white = 0xFFFFFFFFu;
	if ( XR_SUCCESS != m_WhiteTexture->Init( INT2( 1, 1 ), GfxTexture::ETextureFormat::TF_R8G8B8A8, 1, &white, "WhiteFallbackTexture" ) ) {
		return false;
	}

	CreateTexture( m_BlackTexture );
	const uint32_t black = 0xFF000000;
	if ( XR_SUCCESS != m_BlackTexture->Init( INT2( 1, 1 ), GfxTexture::ETextureFormat::TF_R8G8B8A8, 1, &black, "BlackFallbackTexture" ) ) {
		return false;
	}

	CreateTexture( m_DefaultOrmTexture );
	const uint32_t orm = 0xFF00D9FFu;
	if ( XR_SUCCESS != m_DefaultOrmTexture->Init( INT2( 1, 1 ), GfxTexture::ETextureFormat::TF_R8G8B8A8, 1, &orm, "DefaultOrmTexture(1,0.9,0)" ) ) {
		return false;
	}

	return true;
}

void D3D12GraphicsEngine::BindSurfaceTextures( int /*slot*/, GfxTexture* diffuse, GfxTexture* /*normalmap*/, unsigned int /*numTextures*/ ) {
	// Record the diffuse texture for the next 2D draw. Slot 0 only for now (normalmap unused in the UI path).
	m_CurrentTexture = diffuse;
}

// Glyph-quad builder — shared with the D3D11 backend (external linkage, defined in D3D11GraphicsEngine.cpp).
// Pure geometry (font atlas -> ExVertexStruct triangle list), no backend dependency, so we reuse it verbatim.
namespace UI::zFont {
	void AppendGlyphs( std::vector<ExVertexStruct>& vertices, std::string_view str, float x, float y,
		const ::zFont* font, zColor fontColor, float scale, zCCamera* camera );
}

void D3D12GraphicsEngine::DrawString( std::string_view str, float x, float y, const zFont* font, zColor& fontColor ) {
	if ( !m_FrameOpen || !font || !font->tex )
		return;

	// Strip trailing '/' markers (Gothic control chars), like D3D11 DrawString.
	size_t maxLen = str.size();
	while ( maxLen > 0 && str[maxLen - 1] == '/' ) --maxLen;
	if ( !maxLen ) return;
	str = str.substr( 0, maxLen );

	constexpr float FONT_CACHE_PRIO = -1.0f;
	zCTexture* tx = font->tex;
	if ( tx->CacheIn( FONT_CACHE_PRIO ) != zRES_CACHED_IN )
		return;

	// UIScale mirrors D3D11 DrawString: swim-bar width / 180 (the custom-font multiplier defaults to 1).
	float UIScale = 1.0f;
	static int savedBarSize = -1;
	if ( oCGame::GetGame() ) {
		if ( savedBarSize == -1 && oCGame::GetGame()->swimBar )
			savedBarSize = oCGame::GetGame()->swimBar->psizex;
		if ( savedBarSize > 0 )
			UIScale = static_cast<float>(savedBarSize) / 180.f;
	}

	// Build glyph quads over the font atlas (screen-space xyzrhw ExVertexStruct triangle list).
	static std::vector<ExVertexStruct> vertices;
	vertices.clear();
	UI::zFont::AppendGlyphs( vertices, str, x, y, font, fontColor, UIScale, zCCamera::GetCamera() );
	if ( vertices.empty() )
		return;

	// Text = texture * vertex color, alpha-blended. Force the FF stage + blend that DrawVertexArray reads,
	// save/restore so Gothic's tracked 2D state is undisturbed (mirrors D3D11 DrawString exactly).
	GothicRendererState& rs = Engine::GAPI->GetRendererState();
	GothicBlendStateInfo savedBlend = rs.BlendState.Clone();
	GothicDepthBufferStateInfo savedDepth = rs.DepthState.Clone();
	FixedFunctionStage::EColorOp    savedOp0 = rs.GraphicsState.FF_Stages[0].ColorOp;
	FixedFunctionStage::EColorOp    savedOp1 = rs.GraphicsState.FF_Stages[1].ColorOp;
	FixedFunctionStage::ETextureArg savedA1 = rs.GraphicsState.FF_Stages[0].ColorArg1;
	FixedFunctionStage::ETextureArg savedA2 = rs.GraphicsState.FF_Stages[0].ColorArg2;

	rs.BlendState.SetAlphaBlending();
	// Text is drawn over the finished scene (during the world pass the depth buffer holds scene depth); the
	// glyph quads must never depth-test, so force depth off — m_Pipelines.GetOrCreateUIPipeline picks the no-test PSO.
	rs.DepthState.DepthBufferEnabled = false;
	rs.DepthState.DepthWriteEnabled = false;
	rs.GraphicsState.FF_Stages[0].ColorOp = FixedFunctionStage::EColorOp::CO_MODULATE;
	rs.GraphicsState.FF_Stages[1].ColorOp = FixedFunctionStage::EColorOp::CO_DISABLE;
	rs.GraphicsState.FF_Stages[0].ColorArg1 = FixedFunctionStage::ETextureArg::TA_TEXTURE;
	rs.GraphicsState.FF_Stages[0].ColorArg2 = FixedFunctionStage::ETextureArg::TA_DIFFUSE;

	// Bind the font atlas as the diffuse texture for this draw only.
	GfxTexture* prevTex = m_CurrentTexture;
	if ( MyDirectDrawSurface7* surface = tx->GetSurface() )
		m_CurrentTexture = surface->GetEngineTexture();

	DrawVertexArray( vertices.data(), static_cast<unsigned int>(vertices.size()), 0, sizeof( ExVertexStruct ) );

	m_CurrentTexture = prevTex;
	rs.BlendState = savedBlend;
	rs.DepthState = savedDepth;
	rs.GraphicsState.FF_Stages[0].ColorOp = savedOp0;
	rs.GraphicsState.FF_Stages[1].ColorOp = savedOp1;
	rs.GraphicsState.FF_Stages[0].ColorArg1 = savedA1;
	rs.GraphicsState.FF_Stages[0].ColorArg2 = savedA2;
}

bool D3D12GraphicsEngine::CreateDepthBuffer( INT2 size ) {
	if ( size.x <= 0 || size.y <= 0 ) return false;
	ID3D12Device* device = m_Device.GetDevice();

	// DSV heap (single descriptor) — created once, reused across resizes.
	if ( !m_DsvHeap ) {
		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
		dsvHeapDesc.NumDescriptors = 1;
		dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if ( FAILED( device->CreateDescriptorHeap( &dsvHeapDesc, IID_PPV_ARGS( m_DsvHeap.ReleaseAndGetAddressOf() ) ) ) )
			return false;
		m_DsvDescriptorSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );
	}

	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = static_cast<UINT64>(size.x);
	dd.Height = static_cast<UINT>(size.y);
	dd.DepthOrArraySize = 1;
	dd.MipLevels = 1;
	dd.Format = DXGI_FORMAT_R32_TYPELESS;   // typeless so the same texels serve a D32_FLOAT DSV and an R32_FLOAT SRV
	dd.SampleDesc.Count = 1;
	dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	// Reversed-Z: the world clears depth to 0.0, so make that the optimized clear value.
	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DXGI_FORMAT_D32_FLOAT;
	clear.DepthStencil.Depth = 0.0f;

	// Born in DEPTH_WRITE. Now also SRV-readable: DispatchLightCulling brackets a NON_PIXEL_SHADER_RESOURCE
	// read of it (per-tile far-Z) and transitions back to DEPTH_WRITE, so it is DEPTH_WRITE at every other point.
	if ( FAILED( m_Allocator->CreateResource( &allocDesc, &dd,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, m_DepthBufferAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_DepthBuffer.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: failed to create the depth buffer (" << size.x << "x" << size.y << ").";
		return false;
	}
	m_DepthBuffer->SetName( L"DepthBuffer(D32)" );

	D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
	dsv.Format = DXGI_FORMAT_D32_FLOAT;   // typeless resource viewed as depth here
	dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	device->CreateDepthStencilView( m_DepthBuffer.Get(), &dsv, m_DsvHeap->GetCPUDescriptorHandleForHeapStart() );

	// R32_FLOAT SRV of the same texels for the light cull's per-tile far-Z read. Slot allocated once; the view is
	// (re)created every call so it always points at the current (post-resize) resource.
	if ( m_DepthSrvSlot == UINT_MAX ) {
		m_DepthSrvSlot = AllocateSrvSlot();
		if ( m_DepthSrvSlot == UINT_MAX ) return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC dsrv = {};
	dsrv.Format = DXGI_FORMAT_R32_FLOAT;   // typeless resource viewed as a single float channel
	dsrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	dsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	dsrv.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView( m_DepthBuffer.Get(), &dsrv, GetSrvCpuHandle( m_DepthSrvSlot ) );

	// Forward+ tile grid storage is resolution-dependent too — (re)build it here so it always tracks the
	// depth buffer's size (called from both init and the resize path). GPU is idle at both call sites.
	if ( !CreateLightCullBuffers( size ) ) return false;
	return true;
}

bool D3D12GraphicsEngine::CreateSceneColorTarget( INT2 size ) {
	// HDR scene-color render target (Phase 3): the 3D world/VOB/skeletal/water/decal/particle passes render into
	// this R16F target so lighting can exceed 1.0 (bright sun + stacked additive point lights keep their detail
	// instead of clipping to white). ResolveSceneToBackBuffer then tonemaps it into the swapchain. Resolution-
	// sized → (re)created here on init and every resize (RTV heap + SRV slot persist; only the resource + views
	// are rebuilt). DEFAULT-heap GPU memory (64bpp), so it barely touches the 32-bit CPU address space.
	if ( size.x <= 0 || size.y <= 0 ) return false;
	ID3D12Device* device = m_Device.GetDevice();
	if ( !device || !m_RtvHeap ) return false;

	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = static_cast<UINT64>(size.x);
	dd.Height = static_cast<UINT>(size.y);
	dd.DepthOrArraySize = 1;
	dd.MipLevels = 1;
	dd.Format = kSceneColorFormat;
	dd.SampleDesc.Count = 1;
	dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	// Born in RENDER_TARGET (the world pass renders straight into it; ResolveSceneToBackBuffer flips it to
	// PIXEL_SHADER_RESOURCE and back next frame). GPU is idle at every call site (init / post-WaitForGpuIdle resize).
	if ( FAILED( m_Allocator->CreateResource( &allocDesc, &dd,
		D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, m_SceneColorAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_SceneColor.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: failed to create the HDR scene-color target (" << size.x << "x" << size.y << ").";
		return false;
	}
	m_SceneColor->SetName( L"SceneColorHDR(R16F)" );
	m_SceneColorInPixelState = false;

	// RTV in the extra heap slot (index kBackBufferCount, past the swapchain RTVs).
	m_SceneColorRtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
	m_SceneColorRtv.ptr += static_cast<SIZE_T>(kBackBufferCount) * m_RtvDescriptorSize;
	device->CreateRenderTargetView( m_SceneColor.Get(), nullptr, m_SceneColorRtv );

	// SRV for the tonemap resolve (slot allocated once; view re-created each call to point at the current resource).
	if ( m_SceneColorSrvSlot == UINT_MAX ) {
		m_SceneColorSrvSlot = AllocateSrvSlot();
		if ( m_SceneColorSrvSlot == UINT_MAX ) return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = kSceneColorFormat;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView( m_SceneColor.Get(), &srv, GetSrvCpuHandle( m_SceneColorSrvSlot ) );
	return true;
}


void D3D12GraphicsEngine::BindSceneColorTarget() {
	// Make the HDR scene-color target the world pass's render target (+ keep the shared depth buffer). Transitions
	// it back from PIXEL_SHADER_RESOURCE (last frame's resolve left it there) to RENDER_TARGET when needed.
	if ( !m_SceneColor || !m_CmdList ) return;
	if ( m_SceneColorInPixelState ) {
		auto toRT = TransitionBarrier( m_SceneColor.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
		m_CmdList->ResourceBarrier( 1, &toRT );
		m_SceneColorInPixelState = false;
	}
	const bool haveDepth = m_DepthBuffer && m_DsvHeap;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
	if ( haveDepth ) dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, haveDepth ? &dsv : nullptr );
	m_ColorTargetIsHDR = true;
}

void D3D12GraphicsEngine::ResolveSceneToBackBuffer() {
	// Tonemap the finished HDR scene into the swapchain backbuffer, then leave the backbuffer bound so the 2D UI
	// (drawn after OnStartWorldRendering) composites on top in LDR. If HDR is unavailable, no-op (nothing to show).
	if ( !m_SceneColor || !m_Pipelines.Tonemap.PSO || !m_Pipelines.Tonemap.RootSig || !m_CmdList ) return;
	DX_ZONE( m_CmdList, "Tonemap resolve (HDR->swapchain)" );

	if ( !m_SceneColorInPixelState ) {
		auto toSrv = TransitionBarrier( m_SceneColor.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		m_CmdList->ResourceBarrier( 1, &toSrv );
		m_SceneColorInPixelState = true;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>(m_FrameIndex) * m_RtvDescriptorSize;
	m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, nullptr );   // no depth for the fullscreen resolve
	m_ColorTargetIsHDR = false;

	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->SetPipelineState( m_Pipelines.Tonemap.PSO.Get() );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.Tonemap.RootSig.Get() );
	m_CmdList->SetGraphicsRootDescriptorTable( 0, GetSrvGpuHandle( m_SceneColorSrvSlot ) );
	auto& tonemapSettings = Engine::GAPI->GetRendererState().RendererSettings;
	// { Exposure, LumWhite, ToneMapMode, pad }. MiddleGray is NOT sent — Tonemap.hlsl hardcodes its ACES-tuned
	// 0.18 target; RendererSettings.HDRMiddleGray (0.8) is calibrated for D3D11's own tonemap curves.
	struct { float Exposure; float LumWhite; UINT ToneMapMode; float _pad; } tonemapConsts = {
		tonemapSettings.Exposure > 0.0f ? tonemapSettings.Exposure : 1.0f,
		tonemapSettings.HDRLumWhite,
		static_cast<UINT>( tonemapSettings.HDRToneMap ),
		0.0f
	};
	m_CmdList->SetGraphicsRoot32BitConstants( 1, 4, &tonemapConsts, 0 );
	m_CmdList->SetGraphicsRootShaderResourceView( 2, m_LumAdaptedBuffer->GetGPUVirtualAddress() );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	m_CmdList->IASetVertexBuffers( 0, 0, nullptr );
	m_CmdList->DrawInstanced( 3, 1, 0, 0 );
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
	// 4096 (~192MB across 3 D32 slices) barely touches the 32-bit CPU address space.
	int desired = Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize;
	m_ShadowMapSize = static_cast<UINT>(std::min( std::max( desired, 1024 ), 4096 ));

	D3D12MA::ALLOCATION_DESC allocDesc = {};
	allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dd = {};
	dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dd.Width = m_ShadowMapSize;
	dd.Height = m_ShadowMapSize;
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
	if ( FAILED( m_Allocator->CreateResource( &allocDesc, &dd,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, m_ShadowMapAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_ShadowMap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_ShadowMap->SetName( L"SunShadowMap(D32 array)" );
	m_ShadowInPixelState = false;

	// DSV heap: one D32 DSV per cascade slice.
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = kShadowCascades;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	if ( FAILED( device->CreateDescriptorHeap( &dsvHeapDesc, IID_PPV_ARGS( m_ShadowDsvHeap.ReleaseAndGetAddressOf() ) ) ) )
		return false;
	m_ShadowDsvSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_DSV );
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

	// Array SRV (R32_FLOAT) covering all cascades — bound by the lit passes in a later increment.
	m_ShadowSrvSlot = AllocateSrvSlot();
	if ( m_ShadowSrvSlot == UINT_MAX ) return false;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = DXGI_FORMAT_R32_FLOAT;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2DArray.MipLevels = 1;
	srv.Texture2DArray.ArraySize = kShadowCascades;
	device->CreateShaderResourceView( m_ShadowMap.Get(), &srv, GetSrvCpuHandle( m_ShadowSrvSlot ) );

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
	cbDesc.Width = 256;   // one 256-aligned CB (cascade matrices + sun data fit well under 256B)
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
	// Temporal smoothing (P2.9c-3c): lerp toward the live sun dir so the origin-anchored snap grid rotates
	// gradually instead of jittering per frame (the lever arm from origin to a distant player turns tiny
	// sun drift into visible texel crawl). alpha small = strong smoothing; the day cycle is minutes-long so
	// a ~1s time constant lags imperceptibly while killing per-frame jitter.
	XMVECTOR toSun;
	if ( !m_SunDirInitialized ) {
		toSun = rawToSun;
		m_SunDirInitialized = true;
	} else {
		constexpr float alpha = 0.03f;
		toSun = XMVector3Normalize( XMVectorLerp( XMLoadFloat3( &m_SmoothedSunDir ), rawToSun, alpha ) );
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
	const float shadowFar = 20000.0f;
	const float lambda = 0.85f;
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
	if ( !m_FrameOpen || !m_ShadowMap || !m_ShadowCasterWorldPSO || !m_ShadowDsvHeap || !m_Pipelines.World.RootSig )
		return;

	DX_ZONE( m_CmdList, "Sun Shadows (cascades)" );

	// Return the map to DEPTH_WRITE if last frame's (future) lit sampling left it in PIXEL_SHADER_RESOURCE.
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

	MeshInfo* wm = Engine::GAPI->GetWrappedWorldMesh();
	D3D12VertexBuffer* vb = wm ? D3D12VertexBuffer::From( wm->GetMeshVertexBuffer() ) : nullptr;
	D3D12VertexBuffer* ib = wm ? D3D12VertexBuffer::From( wm->GetMeshIndexBuffer() ) : nullptr;
	const bool haveWorld = vb && ib && vb->GetResource() && ib->GetResource()
		&& (ib->GetSizeInBytes() / sizeof( uint32_t )) > 0;
	// haveSkel/haveAttach are now computed PER CASCADE below (g_ShadowSkelDraws[c]/g_ShadowAttachDraws[c]),
	// since the caster set differs per cascade frustum — see the per-cascade PrepareFrameSkeletals call.

	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_ShadowMapSize), static_cast<float>(m_ShadowMapSize), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, static_cast<LONG>(m_ShadowMapSize), static_cast<LONG>(m_ShadowMapSize) };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
	// Resolve + bind a material's diffuse to a root descriptor-table slot (white fallback) for the alpha cutout.
	auto bindDiffuse = [&]( zCTexture* tex, UINT rootParam ) {
		D3D12_GPU_DESCRIPTOR_HANDLE srv = whiteSrv;
		if ( tex && tex->GetCacheState() == zRES_CACHED_IN ) {
			if ( MyDirectDrawSurface7* surface = tex->GetSurface() ) {
				if ( GfxTexture* gfx = surface->GetEngineTexture() ) {
					D3D12Texture* d12 = D3D12Texture::From( gfx );
					if ( d12->HasSRV() ) srv = d12->GetSrvGpuHandle();
				}
			}
		}
		m_CmdList->SetGraphicsRootDescriptorTable( rootParam, srv );
		};

	const Frustum& unionShadowFrustum = m_CascadeFrustum[kShadowCascades - 1];
	static std::vector<WorldMeshSectionInfo*> shadowSections;
	shadowSections.clear();
	Engine::GAPI->CollectVisibleSections( shadowSections, &unionShadowFrustum, false );

	D3D12_CPU_DESCRIPTOR_HANDLE dsvBase = m_ShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
	for ( UINT c = 0; c < kShadowCascades; ++c ) {
		D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvBase;
		dsv.ptr += static_cast<SIZE_T>( c ) * m_ShadowDsvSize;
		m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, &dsv );   // DSV stays bound across the PSO switches below
		m_CmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr );   // normal-Z far
		if ( !sunUp ) continue;   // still leaves a valid (unshadowed) slice

		// --- World mesh (root sig: m_Pipelines.World.RootSig; b0 = cascade view-proj; t0 diffuse table @1) ---
		if ( haveWorld ) {
			DX_ZONE( m_CmdList, "World Mesh" );

			WorldDrawCommand* cmds = reinterpret_cast<WorldDrawCommand*>(m_ShadowWorldDrawArgsPtr[c][m_FrameIndex]);
			UINT drawCount = 0;

			for ( WorldMeshSectionInfo* section : shadowSections ) {
				if ( !section ) continue;
				for ( auto const& [meshKey, mesh] : section->WorldMeshes ) {
					if ( !mesh || mesh->Indices.empty() ) continue;
					if ( meshKey.Info && meshKey.Info->MaterialType != MaterialInfo::MT_None ) continue;

					// Frustum cull
					if ( !Engine::GAPI->IsWorldMeshVisibleInFrustum( mesh, m_CascadeFrustum[c] ) ) continue;

					// Skip translucent / blended geometry in shadow maps
					if ( (meshKey.Material->GetAlphaFunc() > zMAT_ALPHA_FUNC_NONE &&
						meshKey.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST)
						|| (meshKey.Material->GetAlphaFunc() == 0 && zColor( meshKey.Material->GetColor() ).bgra.alpha < 255) ) {
						continue;
					}

					if ( drawCount >= kMaxWorldDrawCommands ) break;

					// Resolve bindless diffuse index for alpha clipping
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

					WorldDrawCommand& cmd = cmds[drawCount++];
					cmd.MatNormalIndex = 0xFFFFFFFFu;
					cmd.MatOrmIndex = m_DefaultOrmTexture->GetSrvSlot();
					cmd.MatDiffuseIndex = diffuseIdx;
					cmd.Draw.IndexCountPerInstance = static_cast<UINT>(mesh->Indices.size());
					cmd.Draw.InstanceCount = 1;
					cmd.Draw.StartIndexLocation = mesh->BaseIndexLocation;
					cmd.Draw.BaseVertexLocation = 0;
					cmd.Draw.StartInstanceLocation = 0;
				}
			}

			m_ShadowWorldDrawCount[c] = drawCount;

			// 2. Dispatch Indirect Draw Call if commands exist
			if ( drawCount > 0 ) {
				m_CmdList->SetPipelineState( m_ShadowCasterWorldPSO.Get() );
				m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
				m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );

				const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
				const D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
				m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
				m_CmdList->IASetIndexBuffer( &ibv );

				m_CmdList->ExecuteIndirect(
					m_WorldIndirectCmdSig.Get(),
					drawCount,
					m_ShadowWorldDrawArgs[c][m_FrameIndex].Get(),
					0,
					nullptr,
					0
				);
			}
		}

		const auto& rs = Engine::GAPI->GetRendererState().RendererSettings;

		const float shadowDistance = 8000 + (12000.0f * std::max( 0.1f, rs.WorldShadowRangeScale ));

		thread_local std::vector<SkeletalVobInfo*> cascadeMobs;
		std::vector<TransparencyVobInfo> _nop;
		std::vector<VobLightInfo*> _nop2;

		g_ShadowPassVobs[c].Reset(); // TODO: maybe only at BeginFrame?

		D3D12RenderQueue queue( &g_ShadowPassVobs[c], &cascadeMobs, &_nop, &_nop2 );
		RndCullContext ctx;
		ctx.queue = &queue;
		ctx.frustum = m_CascadeFrustum[c];
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

		// Skeletal shadow casters (parity with D3D11's Shadows::DrawSkeletalMeshes): cull the FULL registered
		// skeletal-vob list against THIS cascade's frustum, not the player's view frustum — a caster invisible to
		// the player can still cast a visible shadow. Per-vob CB/attachment ring uploads are cached once per
		// frame (see g_SkelUploadCache in PrepareFrameSkeletals), so this costs nothing extra for casters already
		// prepared for the main view — only the (cheap) frustum test + record-append runs per cascade.
		g_ShadowSkelDraws[c].clear();
		g_ShadowAttachDraws[c].clear();
		PrepareFrameSkeletals( Engine::GAPI->GetSkeletalMeshVobs(), &m_CascadeFrustum[c], static_cast<int>( c ) );
		const bool haveSkel = m_ShadowCasterSkeletalPSO && m_Pipelines.Skeletal.RootSig && !g_ShadowSkelDraws[c].empty();
		const bool haveAttach = m_ShadowCasterVobPSO && !g_ShadowAttachDraws[c].empty();

		// --- Instanced VOBs (same root sig; two streams: packed vertex slot 0 + per-instance world slot 1) ---
		thread_local std::vector<FrameVobUpload> cascadeUploads;
		cascadeUploads.clear();

		const auto haveVobs = UploadVobs( g_ShadowPassVobs[c].buckets, cascadeUploads );
		if ( haveVobs ) {
			DX_ZONE( m_CmdList, "Vobs" );

			m_CmdList->SetPipelineState( m_ShadowCasterVobPSO.Get() );
			m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
			m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
			for ( const FrameVobUpload& up : cascadeUploads ) {
				MeshVisualInfo* visual = up.visual;
				if ( !visual ) continue;
				// Wind sway (b4, VS): a genuinely wind-flagged VOB casting a shadow must sway its caster
				// silhouette the same as its lit geometry — VSDepth (shared with the opaque prepass) reads
				// this unconditionally when INSTANCE_WINDFLUENCE.x/y > 0, so an unbound/stale root value here
				// would read garbage for exactly those casters. windDir/globalTime/playerPos were refreshed
				// once in OnStartWorldRendering.
				m_WindBuffer.minHeight = visual->BBox.Min.y;
				m_WindBuffer.maxHeight = visual->BBox.Max.y;
				m_CmdList->SetGraphicsRoot32BitConstants( 11, 12, &m_WindBuffer, 0 );
				for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
					bindDiffuse( meshKey.Material->GetAniTexture(), 1 );
					for ( MeshInfo* mi : meshList ) {
						if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() ) continue;
						D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
						D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
						if ( !mvb->GetResource() || !mib->GetResource() ) continue;
						const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
						const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, up.instView };
						m_CmdList->IASetVertexBuffers( 0, 2, views );
						const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
						m_CmdList->IASetIndexBuffer( &ibv );
						m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mi->Indices.size()), up.numInstances, 0, 0, 0 );
					}
				}
			}
		}

		// --- Skinned skeletals (root sig: m_Pipelines.Skeletal.RootSig; b0 cascade view-proj, b1 instance, b2 bones) ---
		if ( haveSkel ) {
			DX_ZONE( m_CmdList, "Skeletals" );

			m_CmdList->SetPipelineState( m_ShadowCasterSkeletalPSO.Get() );
			m_CmdList->SetGraphicsRootSignature( m_Pipelines.Skeletal.RootSig.Get() );
			m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
			for ( const FrameSkelDraw& d : g_ShadowSkelDraws[c] ) {
				if ( !d.visual ) continue;
				// Shared per-MODEL texture slots: refresh THIS instance's textures right before reading its
				// materials (see [[skeletal-texani-shared-slots]]) — required in the shadow pass too (alpha-clip).
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
						m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mesh->Indices.size()), 1, 0, 0, 0 );
					}
				}
			}
		}

		// --- Node attachments (weapons/heads) through the VOB caster PSO (packed vertex + single instance) ---
		if ( haveAttach ) {
			DX_ZONE( m_CmdList, "Skeletal Nodes" );

			// Attachment variant (Fatness/Scaling instead of wind, needs NORMAL) — must match the depth prepass/
			// color pass PSO choice for the same reason the wind fix required it (bit-identical transform).
			m_CmdList->SetPipelineState( m_ShadowCasterVobAttachPSO.Get() );
			m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
			m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &m_CascadeViewProj[c], 0 );
			for ( const FrameAttachDraw& a : g_ShadowAttachDraws[c] ) {
				if ( !a.mesh || !a.mesh->GetMeshVertexBuffer() || !a.mesh->GetMeshIndexBuffer() ) continue;
				D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( a.mesh->GetMeshVertexBuffer() );
				D3D12VertexBuffer* mib = D3D12VertexBuffer::From( a.mesh->GetMeshIndexBuffer() );
				if ( !mvb->GetResource() || !mib->GetResource() ) continue;
				bindDiffuse( a.tex, 1 );
				const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
				const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, a.instView };
				m_CmdList->IASetVertexBuffers( 0, 2, views );
				const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
				m_CmdList->IASetIndexBuffer( &ibv );
				m_CmdList->DrawIndexedInstanced( static_cast<UINT>(a.mesh->Indices.size()), 1, 0, 0, 0 );
			}
		}
	}

	// Hand the whole array to PIXEL_SHADER_RESOURCE for the (future) lit-pass PCF sampling; reverted at the top
	// of next frame's shadow pass. Also re-binds the main RT/DSV so subsequent passes draw to the backbuffer.
	auto toSrv = TransitionBarrier( m_ShadowMap.Get(),
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	m_CmdList->ResourceBarrier( 1, &toSrv );
	m_ShadowInPixelState = true;

	// Restore the HDR scene-color RT (+ shared depth) for the lit passes that follow — the world pass renders
	// into the HDR target, not the swapchain (Phase 3); the tonemap resolve composites it at the end of the frame.
	D3D12_CPU_DESCRIPTOR_HANDLE mainDsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
	m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, m_DepthBuffer ? &mainDsv : nullptr );
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

	// ============================ Phase B — COPY static-aside -> active cube ============================
	{
		DX_ZONE( m_CmdList, "Copy Static->Active" );
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

		m_CmdList->CopyResource( m_PointShadowCube.Get(), m_PointShadowStaticCube.Get() );

		auto toDepth = TransitionBarrier( m_PointShadowCube.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE );
		m_CmdList->ResourceBarrier( 1, &toDepth );
		m_PointShadowInPixelState = false;   // active cube now in DEPTH_WRITE for the dynamic overlay
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
	auto toSrv = TransitionBarrier( m_PointShadowCube.Get(),
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	m_CmdList->ResourceBarrier( 1, &toSrv );
	m_PointShadowInPixelState = true;

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

bool D3D12GraphicsEngine::CreateBloomResources( INT2 size ) {
	// Bloom pyramid (mirrors D3D11PFX_Bloom's mip-chain sizing exactly): mip i is size >> (i+1), stop once the
	// shorter side drops below kBloomMinMipSize. Recreated on every resize; SRV/UAV heap slots are allocated
	// ONCE (persist across resizes, like m_SceneColorSrvSlot) and just get their views re-pointed at the fresh
	// resource. Non-fatal on failure — RenderBloom() checks m_BloomMipCount > 0 before doing anything.
	m_BloomMipCount = 0;
	if ( size.x < 4 || size.y < 4 ) return false;
	ID3D12Device* device = m_Device.GetDevice();
	if ( !device ) return false;

	int mipCount = 0;
	for ( int i = 0; i < kBloomMaxMips; ++i ) {
		int mw = size.x >> (i + 1);
		int mh = size.y >> (i + 1);
		if ( mw < kBloomMinMipSize || mh < kBloomMinMipSize ) break;
		mipCount++;
	}
	if ( mipCount < 1 ) return false;

	D3D12MA::ALLOCATION_DESC heapDefault = {};
	heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	auto makeTex = [&]( int w, int h, ComPtr<ID3D12Resource>& out, ComPtr<D3D12MA::Allocation>& outAlloc, const wchar_t* name ) -> bool {
		D3D12_RESOURCE_DESC dd = {};
		dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		dd.Width = static_cast<UINT64>( w );
		dd.Height = static_cast<UINT>( h );
		dd.DepthOrArraySize = 1;
		dd.MipLevels = 1;
		dd.Format = kSceneColorFormat;
		dd.SampleDesc.Count = 1;
		dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if ( FAILED( m_Allocator->CreateResource( &heapDefault, &dd,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, outAlloc.ReleaseAndGetAddressOf(),
			IID_PPV_ARGS( out.ReleaseAndGetAddressOf() ) ) ) ) {
			LogWarn() << "D3D12: failed to create a bloom pyramid texture (" << w << "x" << h << ").";
			return false;
		}
		out->SetName( name );
		return true;
		};

	auto ensureSlot = [&]( UINT& slot ) -> bool {
		if ( slot == UINT_MAX ) slot = AllocateSrvSlot();
		return slot != UINT_MAX;
		};

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = kSceneColorFormat;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = kSceneColorFormat;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

	for ( int i = 0; i < mipCount; ++i ) {
		const int mw = std::max( 1, size.x >> (i + 1) );
		const int mh = std::max( 1, size.y >> (i + 1) );
		m_BloomMipSize[i] = { mw, mh };

		wchar_t nameBuf[32];
		swprintf_s( nameBuf, L"BloomDown%d", i );
		if ( !makeTex( mw, mh, m_BloomDown[i], m_BloomDownAlloc[i], nameBuf ) ) return false;
		if ( !ensureSlot( m_BloomDownSrvSlot[i] ) || !ensureSlot( m_BloomDownUavSlot[i] ) ) return false;
		device->CreateShaderResourceView( m_BloomDown[i].Get(), &srvDesc, GetSrvCpuHandle( m_BloomDownSrvSlot[i] ) );
		device->CreateUnorderedAccessView( m_BloomDown[i].Get(), nullptr, &uavDesc, GetSrvCpuHandle( m_BloomDownUavSlot[i] ) );

		if ( i < mipCount - 1 ) {
			swprintf_s( nameBuf, L"BloomUp%d", i );
			if ( !makeTex( mw, mh, m_BloomUp[i], m_BloomUpAlloc[i], nameBuf ) ) return false;
			if ( !ensureSlot( m_BloomUpUavSlot[i] ) || !ensureSlot( m_BloomUpSrvSlot[i] ) ) return false;
			device->CreateUnorderedAccessView( m_BloomUp[i].Get(), nullptr, &uavDesc, GetSrvCpuHandle( m_BloomUpUavSlot[i] ) );

			// Upsample SRV table for level i needs 2 CONTIGUOUS slots (t0=source, rewritten per-frame in
			// RenderBloom; t1=down[i], fixed here). One pair PER IN-FLIGHT FRAME (see the header comment on
			// m_BloomUpSrvPairSlot) — allocate each frame's pair together so bump-allocation guarantees
			// contiguity, then write that copy's t1 (down[i]) now — it never changes until the next resize.
			for ( UINT frame = 0; frame < kBackBufferCount; ++frame ) {
				if ( m_BloomUpSrvPairSlot[frame][i] == UINT_MAX ) {
					const UINT base = AllocateSrvSlot();
					const UINT next = AllocateSrvSlot();
					if ( base == UINT_MAX || next == UINT_MAX || next != base + 1 ) return false;
					m_BloomUpSrvPairSlot[frame][i] = base;
				}
				device->CreateShaderResourceView( m_BloomDown[i].Get(), &srvDesc, GetSrvCpuHandle( m_BloomUpSrvPairSlot[frame][i] + 1 ) );
			}
		}
	}

	m_BloomMipCount = mipCount;
	return true;
}

void D3D12GraphicsEngine::RenderBloom() {
	// Prefilter (bright-pass) -> downsample chain -> upsample chain -> additive composite onto the HDR scene
	// color. Mirrors D3D11PFX_Bloom::Render pass-for-pass. Called from OnStartWorldRendering right before
	// ResolveSceneToBackBuffer, while m_SceneColor is still bound as the world pass's render target.
	auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
	if ( !settings.EnableBloom ) return;
	if ( m_BloomMipCount < 1 || !m_Pipelines.Bloom.PrefilterPSO || !m_Pipelines.Bloom.DownsamplePSO
		|| !m_Pipelines.Bloom.UpsamplePSO || !m_Pipelines.Bloom.CompositePSO || !m_SceneColor )
		return;

	DX_ZONE( m_CmdList, "Bloom" );
	const int mipCount = m_BloomMipCount;

	struct BloomCB { float texelSizeX, texelSizeY, threshold, knee, intensity, filterRadius, padX, padY; };
	BloomCB cb = { 0, 0, settings.BloomThreshold, settings.BloomKnee, settings.BloomStrength, settings.BloomRadius, 0, 0 };

	// Scene color is still RENDER_TARGET (bound by BindSceneColorTarget for the world pass) — flip it to a
	// shader-resource state so the prefilter can sample it, then unbind it as an RTV (compute can't run with it
	// bound). The prefilter is a COMPUTE shader, so this must be NON_PIXEL_SHADER_RESOURCE, not
	// PIXEL_SHADER_RESOURCE — reading a resource through an SRV while it is in the wrong state returns garbage on
	// real hardware (this, plus the bloom-mip UAV-vs-SRV state bug below, was the flickering-black-screen cause).
	if ( !m_SceneColorInPixelState ) {
		auto toSrv = TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
		m_CmdList->ResourceBarrier( 1, &toSrv );
		m_SceneColorInPixelState = true;
	}
	m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );

	// A compute pass writes each bloom mip through a UAV; the next pass reads it through an SRV. That REQUIRES a
	// real state transition (UNORDERED_ACCESS -> shader-resource), NOT just a UAV barrier — a UAV barrier only
	// orders UAV<->UAV access and performs no cache flush / decompress, so the SRV read of a still-UAV-state
	// texture is undefined. Combined NON_PIXEL|PIXEL so both the compute chain (t0/t1) and the graphics composite
	// can read it. The bloom mips start each frame in UNORDERED_ACCESS (created that way, and reset back at the
	// end of this function) so "before" is always UNORDERED_ACCESS here.
	constexpr D3D12_RESOURCE_STATES kBloomRead =
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	auto toBloomRead = [&]( ID3D12Resource* res ) {
		auto b = TransitionBarrier( res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kBloomRead );
		m_CmdList->ResourceBarrier( 1, &b );
		};

	// --- Prefilter: scene color -> down[0] ---
	{
		m_CmdList->SetPipelineState( m_Pipelines.Bloom.PrefilterPSO.Get() );
		m_CmdList->SetComputeRootSignature( m_Pipelines.Bloom.DownRootSig.Get() );
		cb.texelSizeX = 1.0f / m_Resolution.x;
		cb.texelSizeY = 1.0f / m_Resolution.y;
		m_CmdList->SetComputeRoot32BitConstants( 0, 8, &cb, 0 );
		m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_SceneColorSrvSlot ) );
		m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_BloomDownUavSlot[0] ) );
		const UINT gx = (m_BloomMipSize[0].x + 7) / 8, gy = (m_BloomMipSize[0].y + 7) / 8;
		m_CmdList->Dispatch( gx, gy, 1 );
		toBloomRead( m_BloomDown[0].Get() );
	}

	// --- Downsample chain: down[i-1] -> down[i] ---
	m_CmdList->SetPipelineState( m_Pipelines.Bloom.DownsamplePSO.Get() );
	for ( int i = 1; i < mipCount; ++i ) {
		cb.texelSizeX = 1.0f / m_BloomMipSize[i - 1].x;
		cb.texelSizeY = 1.0f / m_BloomMipSize[i - 1].y;
		m_CmdList->SetComputeRoot32BitConstants( 0, 8, &cb, 0 );
		m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_BloomDownSrvSlot[i - 1] ) );
		m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_BloomDownUavSlot[i] ) );
		const UINT gx = (m_BloomMipSize[i].x + 7) / 8, gy = (m_BloomMipSize[i].y + 7) / 8;
		m_CmdList->Dispatch( gx, gy, 1 );
		toBloomRead( m_BloomDown[i].Get() );
	}

	// --- Upsample chain: current = down[mipCount-1]; for i = mipCount-2..0: up[i] = down[i] + tent(current) ---
	// Each level's t0 (variable source) is written into the pair slot right before dispatch; t1 (down[i]) was
	// already written once at creation (CreateBloomResources) and never changes.
	D3D12_SHADER_RESOURCE_VIEW_DESC upSrcSrvDesc = {};
	upSrcSrvDesc.Format = kSceneColorFormat;
	upSrcSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	upSrcSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	upSrcSrvDesc.Texture2D.MipLevels = 1;

	UINT finalBloomSrvSlot = m_BloomDownSrvSlot[0];
	if ( mipCount >= 2 ) {
		m_CmdList->SetPipelineState( m_Pipelines.Bloom.UpsamplePSO.Get() );
		m_CmdList->SetComputeRootSignature( m_Pipelines.Bloom.UpRootSig.Get() );
		for ( int i = mipCount - 2; i >= 0; --i ) {
			const bool firstStep = (i == mipCount - 2);
			ID3D12Resource* source = firstStep ? m_BloomDown[mipCount - 1].Get() : m_BloomUp[i + 1].Get();
			// Double-buffered by m_FrameIndex: this CPU-side descriptor write must never land in the same heap
			// slot a still-in-flight PRIOR frame's GPU dispatch is reading (see the header comment on
			// m_BloomUpSrvPairSlot) — that race was the actual cause of the flickering black regions in bloom.
			const UINT pairSlot = m_BloomUpSrvPairSlot[m_FrameIndex][i];
			m_Device.GetDevice()->CreateShaderResourceView( source, &upSrcSrvDesc, GetSrvCpuHandle( pairSlot ) );

			const int srcW = firstStep ? m_BloomMipSize[mipCount - 1].x : m_BloomMipSize[i + 1].x;
			const int srcH = firstStep ? m_BloomMipSize[mipCount - 1].y : m_BloomMipSize[i + 1].y;
			cb.texelSizeX = 1.0f / srcW;
			cb.texelSizeY = 1.0f / srcH;
			m_CmdList->SetComputeRoot32BitConstants( 0, 8, &cb, 0 );
			m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( pairSlot ) );
			m_CmdList->SetComputeRootDescriptorTable( 2, GetSrvGpuHandle( m_BloomUpUavSlot[i] ) );
			const UINT gx = (m_BloomMipSize[i].x + 7) / 8, gy = (m_BloomMipSize[i].y + 7) / 8;
			m_CmdList->Dispatch( gx, gy, 1 );
			toBloomRead( m_BloomUp[i].Get() );

			// Refresh this level's canonical SRV (read by the i-1 step's t0 above, and by the composite pass
			// once i reaches 0) — same resource/view every resize, so this is a cheap no-op-content rewrite,
			// not a new allocation.
			m_Device.GetDevice()->CreateShaderResourceView( m_BloomUp[i].Get(), &upSrcSrvDesc, GetSrvCpuHandle( m_BloomUpSrvSlot[i] ) );
		}
		finalBloomSrvSlot = m_BloomUpSrvSlot[0];
	}

	// --- Composite: additively blend the (upsampled) bloom onto the HDR scene color ---
	m_CmdList->SetPipelineState( m_Pipelines.Bloom.CompositePSO.Get() );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.Bloom.CompositeRootSig.Get() );
	m_CmdList->SetGraphicsRootDescriptorTable( 0, GetSrvGpuHandle( finalBloomSrvSlot ) );
	m_CmdList->SetGraphicsRoot32BitConstant( 1, *reinterpret_cast<const UINT*>( &settings.BloomStrength ), 0 );

	auto toRt = TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
	m_CmdList->ResourceBarrier( 1, &toRt );
	m_SceneColorInPixelState = false;

	const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
	const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, nullptr );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	m_CmdList->IASetVertexBuffers( 0, 0, nullptr );
	m_CmdList->DrawInstanced( 3, 1, 0, 0 );

	// Return every written bloom mip to UNORDERED_ACCESS so next frame's compute passes can write them again and
	// so the "before" state is deterministic at the top of the next RenderBloom (the toBloomRead transitions
	// above all assume UNORDERED_ACCESS). Done AFTER the composite, which still reads the final mip as an SRV.
	{
		D3D12_RESOURCE_BARRIER resets[2 * kBloomMaxMips];
		UINT n = 0;
		for ( int i = 0; i < mipCount; ++i )
			resets[n++] = TransitionBarrier( m_BloomDown[i].Get(), kBloomRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
		for ( int i = 0; i < mipCount - 1; ++i )
			resets[n++] = TransitionBarrier( m_BloomUp[i].Get(), kBloomRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
		if ( n ) m_CmdList->ResourceBarrier( n, resets );
	}

	m_ColorTargetIsHDR = true;   // just rebound m_SceneColor as RTV above
}

bool D3D12GraphicsEngine::CreateLumAdaptedBuffer() {
	// One-time, fixed-size persistent buffer (dynamic exposure): holds the temporally-adapted average scene
	// luminance CS_LumAdapt writes every frame and Tonemap's PS reads unconditionally. DEFAULT heap (UAV
	// requires it); the buffer's initial content is never read as history — CS_LumAdapt's FirstFrame flag
	// makes the very first dispatch snap directly to that frame's luminance instead of blending against it.
	D3D12MA::ALLOCATION_DESC heapDefault = {};
	heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = sizeof( float );
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	// Created directly in the SRV-read state (not UNORDERED_ACCESS): if CreateLumPartialBuffer ever fails on a
	// resize (RenderLuminanceAdapt then bails at its guard every frame, never touching this buffer), Tonemap's
	// root SRV read must still see a valid state — PIXEL_SHADER_RESOURCE is exactly that, and is otherwise the
	// state RenderLuminanceAdapt itself always leaves the buffer in at the end of a successful frame.
	if ( FAILED( m_Allocator->CreateResource( &heapDefault, &bd,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, m_LumAdaptedBufferAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_LumAdaptedBuffer.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: failed to create the dynamic-exposure adapted-luminance buffer.";
		return false;
	}
	m_LumAdaptedBuffer->SetName( L"AdaptedLuminance" );
	m_LumAdaptedBufferAlloc->SetName( L"AllocAdaptedLuminance" );
	m_LumAdaptedBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	m_LumAdaptInitialized = false;
	return true;
}

bool D3D12GraphicsEngine::CreateLumPartialBuffer( INT2 size ) {
	// Resolution-dependent (recreated on resize, like the bloom pyramid): one {sum,count} float2 per 16x16
	// reduce-pass thread group. Always fully written by CS_LumReduce and fully consumed by CS_LumAdapt within
	// the same frame, so — unlike m_LumAdaptedBuffer — it needs no persistent cross-frame value.
	if ( size.x <= 0 || size.y <= 0 ) return false;
	m_LumGroupsX = (static_cast<UINT>(size.x) + 15) / 16;
	m_LumGroupsY = (static_cast<UINT>(size.y) + 15) / 16;
	const UINT numGroups = m_LumGroupsX * m_LumGroupsY;
	if ( numGroups == 0 ) return false;

	D3D12MA::ALLOCATION_DESC heapDefault = {};
	heapDefault.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = static_cast<UINT64>(numGroups) * (sizeof( float ) * 2);   // float2 {sum, count}
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if ( FAILED( m_Allocator->CreateResource( &heapDefault, &bd,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, m_LumPartialBufferAlloc.ReleaseAndGetAddressOf(),
		IID_PPV_ARGS( m_LumPartialBuffer.ReleaseAndGetAddressOf() ) ) ) ) {
		LogWarn() << "D3D12: failed to create the dynamic-exposure partial-sum buffer (" << size.x << "x" << size.y << ").";
		m_LumPartialCapacity = 0;
		return false;
	}
	m_LumPartialBuffer->SetName( L"LumPartialSums" );
	m_LumPartialBufferAlloc->SetName( L"AllocLumPartialSums" );
	m_LumPartialCapacity = numGroups;
	return true;
}

void D3D12GraphicsEngine::RenderLuminanceAdapt() {
	// Dynamic exposure (auto-exposure): reduces the finished HDR scene color (post-bloom) to one average
	// luminance, temporally adapts it toward last frame's value (CS_LumReduce -> CS_LumAdapt), and leaves the
	// result in m_LumAdaptedBuffer for Tonemap's PS to read. Called after RenderBloom(), before
	// ResolveSceneToBackBuffer(): scene color is RENDER_TARGET on entry (RenderBloom's composite left it bound
	// that way) and must be RENDER_TARGET again on exit (ResolveSceneToBackBuffer's own transition assumes that).
	if ( !m_FrameOpen || !m_Pipelines.LumReduce.PSO || !m_Pipelines.LumReduce.RootSig
		|| !m_Pipelines.LumAdapt.PSO || !m_Pipelines.LumAdapt.RootSig
		|| !m_SceneColor || !m_LumAdaptedBuffer || !m_LumPartialBuffer
		|| m_LumGroupsX == 0 || m_LumGroupsY == 0 )
		return;

	DX_ZONE( m_CmdList, "Dynamic Exposure (luminance reduce+adapt)" );

	if ( !m_SceneColorInPixelState ) {
		auto toSrv = TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
		m_CmdList->ResourceBarrier( 1, &toSrv );
		m_SceneColorInPixelState = true;
	}
	m_CmdList->OMSetRenderTargets( 0, nullptr, FALSE, nullptr );

	auto toUav = TransitionBarrier( m_LumAdaptedBuffer.Get(), m_LumAdaptedBufferState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
	m_CmdList->ResourceBarrier( 1, &toUav );
	m_LumAdaptedBufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	// --- Level 1: reduce scene color -> per-group partial sums ---
	struct LumReduceCB { UINT Width, Height, NumGroupsX, _pad; };
	LumReduceCB reduceCb = { static_cast<UINT>(m_Resolution.x), static_cast<UINT>(m_Resolution.y), m_LumGroupsX, 0 };
	m_CmdList->SetPipelineState( m_Pipelines.LumReduce.PSO.Get() );
	m_CmdList->SetComputeRootSignature( m_Pipelines.LumReduce.RootSig.Get() );
	m_CmdList->SetComputeRoot32BitConstants( 0, 4, &reduceCb, 0 );
	m_CmdList->SetComputeRootDescriptorTable( 1, GetSrvGpuHandle( m_SceneColorSrvSlot ) );
	m_CmdList->SetComputeRootUnorderedAccessView( 2, m_LumPartialBuffer->GetGPUVirtualAddress() );
	m_CmdList->Dispatch( m_LumGroupsX, m_LumGroupsY, 1 );

	// Scene color is done being read (compute) this pass; hand it back to RENDER_TARGET for ResolveSceneToBackBuffer.
	auto toRt = TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
	m_CmdList->ResourceBarrier( 1, &toRt );
	m_SceneColorInPixelState = false;

	// PartialSums: UAV write (above) -> SRV read (below) needs a real state transition, not just a UAV barrier.
	auto partialToSrv = TransitionBarrier( m_LumPartialBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
	m_CmdList->ResourceBarrier( 1, &partialToSrv );

	// --- Level 2: reduce partial sums -> one temporally-adapted luminance value ---
	struct LumAdaptCB { UINT NumPartials; float DeltaTime; UINT FirstFrame; float _pad; };
	LumAdaptCB adaptCb = { m_LumGroupsX * m_LumGroupsY, Engine::GAPI->GetDeltaTime(), m_LumAdaptInitialized ? 0u : 1u, 0.0f };
	m_CmdList->SetPipelineState( m_Pipelines.LumAdapt.PSO.Get() );
	m_CmdList->SetComputeRootSignature( m_Pipelines.LumAdapt.RootSig.Get() );
	m_CmdList->SetComputeRoot32BitConstants( 0, 4, &adaptCb, 0 );
	m_CmdList->SetComputeRootShaderResourceView( 1, m_LumPartialBuffer->GetGPUVirtualAddress() );
	m_CmdList->SetComputeRootUnorderedAccessView( 2, m_LumAdaptedBuffer->GetGPUVirtualAddress() );
	m_CmdList->Dispatch( 1, 1, 1 );
	m_LumAdaptInitialized = true;

	// Reset PartialSums to UNORDERED_ACCESS for next frame's reduce write; AdaptedLum to a PS-readable state for
	// Tonemap (both ResolveSceneToBackBuffer and the thumbnail/screenshot re-tonemap read it later this frame).
	D3D12_RESOURCE_BARRIER post[2] = {
		TransitionBarrier( m_LumPartialBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS ),
		TransitionBarrier( m_LumAdaptedBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ),
	};
	m_CmdList->ResourceBarrier( 2, post );
	m_LumAdaptedBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
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
		struct Cand { UINT dstIdx; zCVobLight* vob; float distSq; bool isStatic; };
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
			cands.push_back( { i, s_lightVobs[i], distSq, L.Color.w == 0.0f } );   // Color.w: 0 = static light
		}
		std::sort( cands.begin(), cands.end(), []( const Cand& a, const Cand& b ) { return a.distSq < b.distSq; } );
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
			g_FramePointShadows.push_back( { np, L.Range, static_cast<UINT>(slot), renderStatic } );
			if ( renderStatic ) { ss.pos = np; ss.range = L.Range; }   // staticValid stamped once actually drawn
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


void D3D12GraphicsEngine::DrawWaterSurfaces() {
	if ( !m_FrameOpen || !m_Pipelines.Water.PSO || !m_Pipelines.Water.RootSig || !m_DepthBuffer || g_FrameWaterSurfaces.empty() )
		return;

	DX_ZONE( m_CmdList, "DrawWaterSurfaces" );

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

	const FogConstants fog = MakeFogConstants();
	// b2: { totalTime (ms, drives UV scroll), water alpha (translucency), pad, pad }.
	const float water[4] = { Engine::GAPI->GetTotalTime(), 0.7f, 0.0f, 0.0f };

	m_CmdList->SetPipelineState( m_Pipelines.Water.PSO.Get() );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.Water.RootSig.Get() );
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
	m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );
	m_CmdList->SetGraphicsRoot32BitConstants( 3, 4, water, 0 );

	D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
	D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );

	D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGpuVirtualAddress(), vb->GetSizeInBytes(), sizeof( ExVertexStructGPU ) };
	D3D12_INDEX_BUFFER_VIEW  ibv = { ib->GetGpuVirtualAddress(), ib->GetSizeInBytes(), DXGI_FORMAT_R32_UINT };
	m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
	m_CmdList->IASetIndexBuffer( &ibv );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
	unsigned int drawnIndices = 0;
	for ( auto const& [tex, meshes] : g_FrameWaterSurfaces ) {
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
		for ( MeshInfo* mesh : meshes ) {
			if ( !mesh || mesh->Indices.empty() ) continue;
			m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mesh->Indices.size()), 1,
				mesh->BaseIndexLocation, 0, 0 );
			drawnIndices += static_cast<unsigned int>(mesh->Indices.size());
		}
	}

	Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += drawnIndices / 3;
	g_FrameWaterSurfaces.clear();
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

	if ( !m_FrameOpen || !m_Pipelines.Ghost.PSO || !m_Pipelines.Ghost.RootSig ) {
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

	m_CmdList->SetPipelineState( m_Pipelines.Ghost.PSO.Get() );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.Ghost.RootSig.Get() );
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );

	D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
	D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
	unsigned int drawnTris = 0;

	// D3D11 draws these back-to-front (painter's algorithm) via a min/max-heap drain; the legacy
	// CollectVisibleVobs path (used by this backend, GothicAPI.cpp's std::ranges::sort(TransparencyVobs,
	// CompareGhostDistance)) instead leaves the vector plain-sorted NEAREST-first, so iterate it in reverse.
	for ( auto it = transparencyVobs.rbegin(); it != transparencyVobs.rend(); ++it ) {
		const TransparencyVobInfo& info = *it;
		if ( !info.normalVob || !info.normalVob->VisualInfo ) continue;   // skeletal ghosts: not yet ported (owed-debt)

		VobInfo* vi = info.normalVob;
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

XRESULT D3D12GraphicsEngine::SetViewport( const ViewportInfo& vp ) {
	m_CurrentViewport.TopLeftX = static_cast<float>(vp.TopLeftX);
	m_CurrentViewport.TopLeftY = static_cast<float>(vp.TopLeftY);
	m_CurrentViewport.Width = static_cast<float>(vp.Width);
	m_CurrentViewport.Height = static_cast<float>(vp.Height);
	m_CurrentViewport.MinDepth = vp.MinZ;
	m_CurrentViewport.MaxDepth = vp.MaxZ;
	m_CurrentScissor = {
		static_cast<LONG>(vp.TopLeftX), static_cast<LONG>(vp.TopLeftY),
		static_cast<LONG>(vp.TopLeftX + vp.Width), static_cast<LONG>(vp.TopLeftY + vp.Height) };
	return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
	if ( !m_SwapChainReady || !m_FrameOpen || !m_Pipelines.UI.RootSig || numVertices == 0 || !vertices )
		return XR_SUCCESS;

	// zBinkPlayer (cutscene playback) feeds its YUV quad through this same draw entry, but needs the
	// dedicated 3-texture YUV->RGB pipeline instead of the FF texture-stage emulation below.
	if ( m_ActivePixelShader == PShaderID::PS_Video && m_Pipelines.Video.PSO )
		return DrawVideoVertexArray( vertices, numVertices, startVertex, stride );

	GothicRendererState& rs = Engine::GAPI->GetRendererState();

	// The sky pass (DrawSky -> zCSkyController_Outdoor::RenderSkyPre) feeds its FF draws through this same
	// path, but needs real backface culling, the HDR scene-color RTV format, and z pinned to the reversed-Z
	// far plane (D3D11's VS_TransformedEx_MAX_Z) instead of the plain 2D UI defaults.
	const bool isSkyPass = rs.RendererInfo.RenderStage == STAGE_DRAW_SKY;
	const D3D12_CULL_MODE cullMode = isSkyPass ? static_cast<D3D12_CULL_MODE>(rs.RasterizerState.CullMode) : D3D12_CULL_MODE_NONE;
	// MyDirect3DDevice7::DrawPrimitive/DrawPrimitiveVB force FrontCounterClockwise=true for the sky FVFs
	// (Gothic's sky geometry is wound CCW) — honor whatever the D3D7 layer set rather than hardcoding it,
	// so this stays correct if a future FF draw path relies on the same state.
	const bool frontCCW = rs.RasterizerState.FrontCounterClockwise;

	// Emulate Gothic's per-draw fixed-function blend mode by selecting the matching PSO.
	ID3D12PipelineState* pso = m_Pipelines.GetOrCreateUIPipeline( rs.BlendState, rs.DepthState, cullMode, m_ColorTargetIsHDR, isSkyPass, frontCCW );
	if ( !pso ) return XR_SUCCESS;

	const UINT frame = m_FrameIndex;
	const UINT bytes = stride * numVertices;
	if ( m_UIVertexBufferOffset + bytes > m_UIVertexBufferCapacity ) {
		if ( !m_UIOverflowLogged ) {
			LogWarn() << "D3D12: 2D vertex ring overflow (" << m_UIVertexBufferCapacity
				<< " bytes/frame). Some UI geometry dropped this frame.";
			m_UIOverflowLogged = true;
		}
		return XR_SUCCESS;
	}

	memcpy( m_UIVertexBufferPtr[frame] + m_UIVertexBufferOffset, vertices, bytes );
	const D3D12_GPU_VIRTUAL_ADDRESS gpuVA = m_UIVertexBuffer[frame]->GetGPUVirtualAddress() + m_UIVertexBufferOffset;
	m_UIVertexBufferOffset += bytes;

	m_CmdList->SetPipelineState( pso );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.UI.RootSig.Get() );

	// The 2D/UI path is inherently full-screen: D3D11 forces a full-backbuffer viewport in OnBeginFrame
	// ("otherwise Gothic can't render its initial menu UI") and its BindViewportInformation reads that
	// live viewport. Gothic can leave a tiny/degenerate D3D7 viewport set at the menu (observed 1x1),
	// which would collapse the whole UI into a single pixel. Fall back to the full backbuffer when the
	// tracked viewport is degenerate, so the transform + rasterizer cover the screen like on D3D11.
	D3D12_VIEWPORT vp = m_CurrentViewport;
	D3D12_RECT     sc = m_CurrentScissor;
	if ( vp.Width < 2.0f || vp.Height < 2.0f ) {
		vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
		sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	}

	// Viewport transform constants (pixels / GothicUIScale), mirroring D3D11 BindViewportInformation.
	const float scale = std::max<float>( 0.001f, rs.RendererSettings.GothicUIScale );
	const float vpConsts[4] = {
		vp.TopLeftX / scale, vp.TopLeftY / scale,
		vp.Width / scale,    vp.Height / scale };
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 4, vpConsts, 0 );

	// Fixed-function stage state (b1) — same struct D3D11 binds as FFPipelineConstantBuffer.
	m_CmdList->SetGraphicsRoot32BitConstants( 2, sizeof( GothicGraphicsState ) / 4, &rs.GraphicsState, 0 );

	// Diffuse texture (fall back to 1x1 white for untextured colored draws / failed uploads).
	D3D12_GPU_DESCRIPTOR_HANDLE srv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
	if ( m_CurrentTexture ) {
		D3D12Texture* t = D3D12Texture::From( m_CurrentTexture );
		if ( t->HasSRV() ) {
            srv = t->GetSrvGpuHandle();
        } else {
            srv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
        }
    } else 	{
	    srv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
	}
	m_CmdList->SetGraphicsRootDescriptorTable( 1, srv );

	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	D3D12_VERTEX_BUFFER_VIEW vbv = { gpuVA, bytes, stride };
	m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
	m_CmdList->DrawInstanced( numVertices, 1, startVertex, 0 );

	Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += numVertices / 3;
	return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::DrawVideoVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
	// All three planes must be uploaded (zBinkPlayer always binds Y/U/V together before drawing); if any
	// is missing, skip rather than sampling garbage/unbound descriptors.
	D3D12Texture* planeY = m_VideoTextures[0] ? D3D12Texture::From( m_VideoTextures[0] ) : nullptr;
	D3D12Texture* planeU = m_VideoTextures[1] ? D3D12Texture::From( m_VideoTextures[1] ) : nullptr;
	D3D12Texture* planeV = m_VideoTextures[2] ? D3D12Texture::From( m_VideoTextures[2] ) : nullptr;
	if ( !planeY || !planeU || !planeV || !planeY->HasSRV() || !planeU->HasSRV() || !planeV->HasSRV() )
		return XR_SUCCESS;

	const UINT frame = m_FrameIndex;
	const UINT bytes = stride * numVertices;
	if ( m_UIVertexBufferOffset + bytes > m_UIVertexBufferCapacity ) {
		if ( !m_UIOverflowLogged ) {
			LogWarn() << "D3D12: 2D vertex ring overflow (" << m_UIVertexBufferCapacity
				<< " bytes/frame). Some UI geometry dropped this frame.";
			m_UIOverflowLogged = true;
		}
		return XR_SUCCESS;
	}

	memcpy( m_UIVertexBufferPtr[frame] + m_UIVertexBufferOffset, vertices, bytes );
	const D3D12_GPU_VIRTUAL_ADDRESS gpuVA = m_UIVertexBuffer[frame]->GetGPUVirtualAddress() + m_UIVertexBufferOffset;
	m_UIVertexBufferOffset += bytes;

	m_CmdList->SetPipelineState( m_Pipelines.Video.PSO.Get() );
	m_CmdList->SetGraphicsRootSignature( m_Pipelines.Video.RootSig.Get() );

	// Same degenerate-viewport fallback as the FF/UI path above (Gothic can leave a tiny D3D7 viewport set).
	D3D12_VIEWPORT vp = m_CurrentViewport;
	D3D12_RECT     sc = m_CurrentScissor;
	if ( vp.Width < 2.0f || vp.Height < 2.0f ) {
		vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
		sc = { 0, 0, m_Resolution.x, m_Resolution.y };
	}

	const float scale = std::max<float>( 0.001f, Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale );
	const float vpConsts[4] = {
		vp.TopLeftX / scale, vp.TopLeftY / scale,
		vp.Width / scale,    vp.Height / scale };
	m_CmdList->SetGraphicsRoot32BitConstants( 0, 4, vpConsts, 0 );

	m_CmdList->SetGraphicsRootDescriptorTable( 1, planeY->GetSrvGpuHandle() );
	m_CmdList->SetGraphicsRootDescriptorTable( 2, planeU->GetSrvGpuHandle() );
	m_CmdList->SetGraphicsRootDescriptorTable( 3, planeV->GetSrvGpuHandle() );

	m_CmdList->RSSetViewports( 1, &vp );
	m_CmdList->RSSetScissorRects( 1, &sc );
	m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

	D3D12_VERTEX_BUFFER_VIEW vbv = { gpuVA, bytes, stride };
	m_CmdList->IASetVertexBuffers( 0, 1, &vbv );
	m_CmdList->DrawInstanced( numVertices, 1, startVertex, 0 );

	Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += numVertices / 3;
	return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::DrawVertexBufferFF( GfxVertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
	if ( !vb || numVertices == 0 ) return XR_SUCCESS;

	// The D3D7 fixed-function vertex-buffer path (MyDirect3DDevice7::DrawPrimitiveVB) only ever feeds
	// Gothic_XYZRHW_DIF_T1_Vertex (28 bytes) — the sky dome + a few HUD strips. Rather than a second
	// input layout + VS variant, snapshot the CPU-side verts (the buffer is a persistently-mapped
	// upload resource, never GPU-bound here), convert to ExVertexStruct, and reuse the validated 2D/UI
	// draw path — exactly what DrawPrimitive does for the immediate (non-VB) case.
	if ( stride != sizeof( Gothic_XYZRHW_DIF_T1_Vertex ) )
		return XR_SUCCESS; // unknown FF-VB format — nothing else is emitted through this path

	const uint8_t* base = static_cast<const uint8_t*>(D3D12VertexBuffer::From( vb )->GetMappedData());
	if ( !base ) return XR_SUCCESS;

	const Gothic_XYZRHW_DIF_T1_Vertex* src =
		reinterpret_cast<const Gothic_XYZRHW_DIF_T1_Vertex*>(base + static_cast<size_t>(startVertex) * stride);

	static std::vector<ExVertexStruct> exv; // reused; the render path is single-threaded (matches DrawPrimitive)
	exv.resize( numVertices );
	for ( unsigned int i = 0; i < numVertices; ++i ) {
		exv[i].Position = src[i].xyz;
		exv[i].Normal.x = src[i].rhw;
		exv[i].TexCoord = src[i].texCoord;
		exv[i].Color = src[i].color;
	}

	return DrawVertexArray( exv.data(), numVertices, 0, sizeof( ExVertexStruct ) );
}

XRESULT D3D12GraphicsEngine::OnStartWorldRendering() {

	// m_PresentPending prevents inventory-world from rendering the whole game scenery for every inventory tile.
	// The engine sadly works like that.
	// the first OnStartWorldRendering after a Present() will be the correct one to draw the world.
	if ( m_PresentPending ) return XR_SUCCESS;

    Engine::GAPI->SetFarPlane( Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE);

	// zCBspNodeRender hook — Gothic's BSP traversal is replaced; we draw the world ourselves.
	// Order mirrors D3D11's DrawWorldMeshNaive: sky background, world mesh, skeletal (NPCs/monsters),
	// then instanced static VOBs. The sky is a fog-colored fill so the horizon dissolves into the
	// per-pixel distance fog of the geometry.

	// Collect the frame's visible vobs/lights/mobs ONCE (CollectVisibleVobs has side effects — it fills each
	// visual's Instances list — so it must run exactly once), then rebuild the per-frame point-light buffer so
	// every geometry pass (world, VOBs, skeletal) lights against the same visible-light set. Mirrors D3D11
	// filling m_FrameLights during collection.
	g_FrameVobs.clear(); g_FrameLights.clear(); g_FrameMobs.clear();
	Engine::GAPI->CollectVisibleVobs( g_FrameVobs, g_FrameLights, g_FrameMobs );
	BuildFrameLightBuffer();
	// Snapshot ALL opaque instanced geometry ONCE, before the depth prepass + cull, so every geometry pass draws
	// from one shared upload: VOB instances (g_FrameVobUploads), then skeletal base/attachment CBs + instances
	// (g_FrameSkelDraws/g_FrameAttachDraws — PrepareFrameSkeletals also runs the once/frame animation update, so
	// it MUST run exactly once). Both skeletal lists (animated + static mobs) are prepared here up front.
	UploadFrameVobInstances();
	g_FrameSkelDraws.clear(); g_FrameAttachDraws.clear();
	g_SkelUploadCache.clear();   // per-vob CB/attachment upload cache — rebuilt fresh each frame
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
	// Forward+ opaque depth prepass — lays down ALL opaque depth before the lit passes so the tiled light cull
	// bounds each tile to real geometry: world mesh, then instanced VOBs, then skeletal (NPCs/monsters) + node
	// attachments. Visually a no-op (the color passes re-pass on GREATER_EQUAL and rewrite the same depth).
	// Build the world-mesh ExecuteIndirect command set ONCE (P2.11) — the shared visible-section collection +
	// per-material bindless-index resolution + water peel-out. Both the depth prepass and the color pass draw
	// from it, so the BSP walk happens once (was per-pass) and neither pass issues per-material CPU draw calls.
	BuildWorldDrawCommands();
    {
        DX_ZONE( m_CmdList, "Depth Prepass" );
        DrawDepthPrepass();
	    DrawVobDepthPrepass();
	    DrawSkeletalDepthPrepass();
    }
	// Forward+ tiled light cull: consume this frame's light buffer + the prepass depth to record which point
	// lights touch each 16x16 screen tile (bounded to real geometry on both the near and far side).
	DispatchLightCulling();
    {
        DX_ZONE( m_CmdList, "Lit Geometry Pass" );
	    DrawWorldMesh();
	    {
		    DX_ZONE( m_CmdList, "Draw skeletal (color)" );
		    DrawSkeletalColor();   // base meshes + node attachments, lit through the tile grid (both lists)
	    }
	    DrawVobsInstanced();
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

	// Bloom (P2.11, opt-in via EnableBloom): must run before the tonemap resolve below, while the scene is still
	// linear HDR — additively blending a mip pyramid of the scene's own bright pixels back onto itself.
	RenderBloom();
	RenderLuminanceAdapt();

	// Phase 3 HDR: the 3D scene is complete — tonemap the HDR target into the swapchain and rebind the backbuffer
	// so Gothic's subsequent 2D UI/HUD draws (and the ImGui overlay in Present) composite on top in LDR.
	ResolveSceneToBackBuffer();

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
	args[0].Constant.Num32BitValuesToSet = 3;                 // normal, orm, diffuse
	args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

	D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
	sigDesc.ByteStride = sizeof( WorldDrawCommand );          // 32 B; MUST match the struct + shader layout
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
                        if ( d->HasSRV() ) ormIdx = d->GetSrvSlot();
                    }
                }
            }

            WorldDrawCommand& c = cmds[count];
            c.MatNormalIndex  = normalIdx;
            c.MatOrmIndex     = ormIdx;
            c.MatDiffuseIndex = diffuseIdx;
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
                if ( d->HasSRV() ) idx[1] = d->GetSrvSlot();
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
        up.numInstances = numInstances;
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

    const UINT frame = m_FrameIndex;
    for ( auto const& [visualPtr, visual] : Engine::GAPI->GetStaticMeshVisuals() ) {
        if ( !visual || visual->Instances.empty() ) continue;

        const UINT numInstances = static_cast<UINT>( visual->Instances.size() );
        const UINT instBytes = numInstances * static_cast<UINT>( sizeof( VobInstanceInfo ) );

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
        up.numInstances = numInstances;
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
    if ( !m_FrameOpen || !m_Pipelines.World.DepthPrepassVobPSO || !m_Pipelines.World.RootSig || !m_DepthBuffer )
        return;
    if ( g_FrameVobUploads.empty() ) return;

    DX_ZONE( m_CmdList, "Depth Prepass (vobs)" );

    // ViewProj — identical derivation to DrawVobsInstanced so the prepass depth matches the color pass exactly.
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    m_CmdList->SetPipelineState( m_Pipelines.World.DepthPrepassVobPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );   // b0 ViewProj (fog/lights not referenced)

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    for ( const FrameVobUpload& up : g_FrameVobUploads ) {
        MeshVisualInfo* visual = up.visual;
        if ( !visual ) continue;
        const UINT numInstances = up.numInstances;

        // Wind sway (b4, VS): must match the color pass's ApplyVobWind exactly (VSDepth/VSMain share the
        // function) or the reversed-Z GREATER_EQUAL depth test discards swaying geometry — see the Vob.hlsl
        // comment on ApplyVobWind. windDir/globalTime/playerPos were refreshed once in OnStartWorldRendering.
        m_WindBuffer.minHeight = visual->BBox.Min.y;
        m_WindBuffer.maxHeight = visual->BBox.Max.y;
        m_CmdList->SetGraphicsRoot32BitConstants( 11, 12, &m_WindBuffer, 0 );

        for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
            zCTexture* tex = meshKey.Material->GetAniTexture();
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

            for ( MeshInfo* mi : meshList ) {
                if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() )
                    continue;
                D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
                D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
                if ( !mvb->GetResource() || !mib->GetResource() ) continue;

                const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
                const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, up.instView };
                m_CmdList->IASetVertexBuffers( 0, 2, views );

                const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                m_CmdList->IASetIndexBuffer( &ibv );

                m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mi->Indices.size()), numInstances, 0, 0, 0 );
            }
        }
    }
}

XRESULT D3D12GraphicsEngine::DrawVobsInstanced() {
    if ( !m_FrameOpen || !m_Pipelines.World.VobPSO || !m_Pipelines.World.RootSig || !m_DepthBuffer )
        return XR_SUCCESS;

    GothicRendererState& rs = Engine::GAPI->GetRendererState();
    if ( !rs.RendererSettings.DrawVOBs )
        return XR_SUCCESS;

    // Visible VOBs/lights/mobs were already collected once in OnStartWorldRendering (g_FrameVobs/Lights/Mobs);
    // this pass just consumes them (each visual's Instances list was filled by that collection).

    // Reversed-Z ViewProj (recomputed; identical derivation to DrawWorldMesh).
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const XMFLOAT4X4& viewM = rs.TransformState.TransformView;
    const XMFLOAT4X4& projM = Engine::GAPI->GetProjectionMatrix();
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4( &viewProj, XMMatrixMultiply( XMLoadFloat4x4( &projM ), XMLoadFloat4x4( &viewM ) ) );

    const FogConstants fog = MakeFogConstants();

    m_CmdList->SetPipelineState( m_Pipelines.World.VobPSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.World.RootSig.Get() );
    m_CmdList->SetGraphicsRoot32BitConstants( 0, 16, &viewProj, 0 );
    m_CmdList->SetGraphicsRoot32BitConstants( 2, 8, &fog, 0 );   // b1 fog
    BindFrameLights();   // param 3 = light SRV (t1), param 4 = light count (b2) — see DrawWorldMesh.
    m_CmdList->SetGraphicsRootConstantBufferView( 7, m_ShadowCBGpu[m_FrameIndex] );          // b3 shadow CB
    m_CmdList->SetGraphicsRootDescriptorTable( 8, GetSrvGpuHandle( m_ShadowSrvSlot ) );      // t4 shadow map
    m_CmdList->SetGraphicsRootDescriptorTable( 9, GetSrvGpuHandle( m_PointShadowSrvSlot ) ); // t5 point-shadow cubes

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(m_Resolution.x), static_cast<float>(m_Resolution.y), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = GetSrvGpuHandle( m_BlackTexture->GetSrvSlot() );
    unsigned int drawnTris = 0;

    // Wind sway (b4, VS): windDir/globalTime/playerPos were already refreshed once this frame in
    // OnStartWorldRendering; only the per-visual bounding-box min/maxHeight fallback (no WindMetaData SRV yet)
    // is refreshed here, mirroring D3D11's per-draw g_windBuffer.minHeight/maxHeight fill (D3D11GraphicsEngine.cpp
    // ~7938).
    static_assert( sizeof( VS_ExConstantBuffer_Wind ) == 48, "WindCB (b4) layout must match Vob.hlsl's WindCB" );

    {
        DX_ZONE( m_CmdList, "Draw Vobs" );
        // Instances were snapshotted into the ring once by UploadFrameVobInstances (before the cull); draw from
        // those shared records so the color pass adds no second upload (its ring usage is unchanged).
        for ( const FrameVobUpload& up : g_FrameVobUploads ) {
            MeshVisualInfo* visual = up.visual;
            if ( !visual ) continue;
            const UINT numInstances = up.numInstances;

            m_WindBuffer.minHeight = visual->BBox.Min.y;
            m_WindBuffer.maxHeight = visual->BBox.Max.y;
            m_CmdList->SetGraphicsRoot32BitConstants( 11, 12, &m_WindBuffer, 0 );

            for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
                zCTexture* tex = meshKey.Material->GetAniTexture();
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
                BindMaterialMaps( tex, 10 );   // b6 bindless normal/ORM indices for this material

                for ( MeshInfo* mi : meshList ) {
                    if ( !mi || mi->Indices.empty() || !mi->GetMeshVertexBuffer() || !mi->GetMeshIndexBuffer() )
                        continue;
                    D3D12VertexBuffer* mvb = D3D12VertexBuffer::From( mi->GetMeshVertexBuffer() );
                    D3D12VertexBuffer* mib = D3D12VertexBuffer::From( mi->GetMeshIndexBuffer() );
                    if ( !mvb->GetResource() || !mib->GetResource() ) continue;

                    const D3D12_VERTEX_BUFFER_VIEW vbv = { mvb->GetGpuVirtualAddress(), mvb->GetSizeInBytes(), sizeof( ExVertexStruct ) };
                    const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, up.instView };
                    m_CmdList->IASetVertexBuffers( 0, 2, views );

                    // VOB sub-mesh index buffers are 16-bit (VERTEX_INDEX), unlike the 32-bit wrapped world mesh.
                    const D3D12_INDEX_BUFFER_VIEW ibv = { mib->GetGpuVirtualAddress(), mib->GetSizeInBytes(), DXGI_FORMAT_R16_UINT };
                    m_CmdList->IASetIndexBuffer( &ibv );

                    m_CmdList->DrawIndexedInstanced( static_cast<UINT>(mi->Indices.size()), numInstances, 0, 0, 0 );
                    drawnTris += (static_cast<unsigned int>(mi->Indices.size()) / 3) * numInstances;
                }
            }
        }
    }

    // Static skeletal MOBs (g_FrameMobs) are now prepared up front (PrepareFrameSkeletals) and drawn by
    // DrawSkeletalColor alongside the animated NPCs — no longer a nested call here (see OnStartWorldRendering).

    // NOTE: the per-visual Instances lists are cleared once at the end of OnStartWorldRendering (not here),
    // so they still get reset even when DrawVOBs is off and this pass early-outs above.

    rs.RendererInfo.FrameDrawnTriangles += drawnTris;
    return XR_SUCCESS;
}

void D3D12GraphicsEngine::PrepareFrameSkeletals( std::vector<SkeletalVobInfo*>& vobs, const Frustum* cullFrustum, int shadowCascade,
    const DirectX::XMFLOAT3* sphereCenter, float sphereRange ) {
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
        // is already pre-culled by the caller) or THIS cascade's frustum for shadows.
        if ( cullFrustum && !cullFrustum->Intersects( vi->Vob->GetBBox() ) ) continue;

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
        // of a model share the slots). The draw paths (DrawSkeletalDepthPrepass / DrawSkeletalColor / the shadow
        // draw loop) call it per-record right before reading the materials — which is why FrameSkelDraw carries
        // vobInfo.

        // Upload cache: skip straight to the cached GPU addresses / attachment records if this vob was already
        // prepared by an earlier cull pass this frame (e.g. the main view already prepared an NPC that a shadow
        // cascade also wants — its pose doesn't change between passes).
        auto cacheIt = g_SkelUploadCache.find( vi );
        if ( cacheIt == g_SkelUploadCache.end() ) {
            model->SetDistanceToCamera( 500 );
            if ( vi->LastAniUpdateFrame != now ) {
                vi->LastAniUpdateFrame = now;
                model->UpdateAttachedVobs();   // once/frame — this is why the pass can't just re-run in a prepass
            }
            
            model->UpdateMeshLibTexAniState();

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

            SkelUploadCache entry;

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
                    bool isMMS = strcmp( mvi->Visual->GetFileExtension( 0 ), ".MMS" ) == 0;
                    // Fatness/Scaling inflate-along-normal (mirrors D3D11's VS_ExConstantBuffer_PerInstanceNode,
                    // VS_ExNode.hlsl: localPos = (pos + Fatness*normal) * Scaling). Only MMS morph-mesh attachments
                    // get a non-trivial value in D3D11 — a plain weapon/lamp/head attachment is Fatness=0/Scaling=1,
                    // a no-op — so ordinary attachments are unaffected by this addition; only MMS ones (facial
                    // morphs, bow/crossbow draw meshes) now size/inflate correctly against the model's Fatness
                    // slider instead of always rendering at their raw rest-mesh size. Reuses the VobInstanceInfo
                    // wind fields (@132/@136) since node attachments never sway in the wind — see Vob.hlsl's
                    // VSMainAttach/VSDepthAttach, which reinterpret them as {Fatness, Scaling}.
                    const float attFatness = isMMS ? std::max<float>( 0.f, model->GetModelFatness() * 0.35f ) : 0.f;
                    const float attScaling = isMMS ? ( model->GetModelFatness() * 0.02f + 1.f ) : 1.f;
                    node->TexAniState.UpdateTexList();
                    if ( isMMS ) {
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
                            entry.attachments.push_back( { attMesh.get(), attTex, attInstView, vi->Vob } );
                        }
                    }
                }
            }

            cacheIt = g_SkelUploadCache.emplace( vi, std::move( entry ) ).first;
        }

        const SkelUploadCache& cached = cacheIt->second;
        if ( cached.hasBaseMesh )
            outSkel.push_back( { vi, visual, cached.instCb, cached.boneCb } );
        for ( const FrameAttachDraw& a : cached.attachments )
            outAttach.push_back( a );
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

XRESULT D3D12GraphicsEngine::SetWindow( HWND hWnd ) {
    LogInfo() << "D3D12: Creating swapchain";
    CommonSetWindow( hWnd ); // stores m_OutputWindow, force-activates, clips cursor, hides mouse cursor

    // Use the configured target resolution (NOT the current client rect — Gothic creates its window
    // tiny, so GetClientRect here would size the swapchain to a few pixels). Mirrors the D3D11 path,
    // which takes RendererSettings.LoadedResolution. OnResize sizes the OS window + builds the swapchain.
    INT2 size = Engine::GAPI->GetRendererState().RendererSettings.LoadedResolution;
    if ( size.x <= 0 || size.y <= 0 ) {
        RECT rc = {};
        GetClientRect( hWnd, &rc );
        size = INT2( std::max<int>( 800, rc.right - rc.left ), std::max<int>( 600, rc.bottom - rc.top ) );
    }

    m_NewResolution = size;
    return OnResize( size );
}

void D3D12GraphicsEngine::QueueSrvResourceForRelease( UINT slot, Microsoft::WRL::ComPtr<ID3D12Resource> resource )
{
    QueueCleanupJob( [this, slot, resource = std::move(resource)]() {
        // Recycle the descriptor slot safely
        this->FreeSrvSlot( slot );

        // The ComPtr 'resource' capture naturally dies here, releasing the ID3D12Resource;
    } );
}

void D3D12GraphicsEngine::QueueCleanupJob( std::move_only_function<void()> callback )
{
    if ( callback == nullptr ) return;
    // No slot to recycle — just hold a reference until this frame index comes back around (after its
    // fence is waited on in MoveToNextFrame), then drop it. The capture keeps the resource alive until
    // every command list that could reference it has finished on the GPU.
    // m_CleanupFrameIndex (not m_FrameIndex) — safe to read from a resource-loading worker thread; see
    // its declaration comment.
    const UINT frameIndex = m_CleanupFrameIndex.load( std::memory_order_relaxed );
    std::lock_guard<std::mutex> lock( m_CleanupMutex );
    m_PerFrameCleanupItems[frameIndex].emplace_back( std::move(callback) );
}

void D3D12GraphicsEngine::QueueResourceForRelease( Microsoft::WRL::ComPtr<ID3D12Resource> resource )
{
    if ( !resource ) return;
    // No slot to recycle — just hold a reference until this frame index comes back around (after its
    // fence is waited on in MoveToNextFrame), then drop it. The capture keeps the resource alive until
    // every command list that could reference it has finished on the GPU.
    QueueCleanupJob( [resource = std::move(resource)]() {} );
}

void D3D12GraphicsEngine::QueueAllocationForRelease( Microsoft::WRL::ComPtr<D3D12MA::Allocation> value )
{
    if ( !value ) return;
    // No slot to recycle — just hold a reference until this frame index comes back around (after its
    // fence is waited on in MoveToNextFrame), then drop it. The capture keeps the resource alive until
    // every command list that could reference it has finished on the GPU.
    QueueCleanupJob( [resource = std::move(value)]() { } );
}

/** Sizes the actual OS window to the target resolution and tells Gothic about the mode so its 2D
    UI coordinate space matches. Mirrors the windowed / borderless branch of the D3D11 backend. */
void D3D12GraphicsEngine::ResizeOutputWindow( INT2 size ) {
    if ( !m_OutputWindow || size.x <= 0 || size.y <= 0 ) return;

#ifndef BUILD_SPACER
    RECT desktopRect = {};
    GetClientRect( GetDesktopWindow(), &desktopRect );
    const bool borderless = ( size.x >= desktopRect.right && size.y >= desktopRect.bottom );

    if ( borderless ) {
        // Fullscreen-borderless: strip the frame and cover the desktop.
        ApplyWindowStyle( WindowModes::WINDOW_MODE_FULLSCREEN_BORDERLESS, RECT{ 0, 0, desktopRect.right, desktopRect.bottom } );
    } else {
        // Windowed: fixed-size window whose CLIENT area equals the target resolution.
        LONG style = ( WS_OVERLAPPEDWINDOW | WS_VISIBLE ) & ~( WS_MAXIMIZEBOX | WS_THICKFRAME );
        RECT wr = { 0, 0, size.x, size.y };
        AdjustWindowRectEx( &wr, style, FALSE, WS_EX_APPWINDOW );

        RECT cur = {};
        int x = 0, y = 0;
        if ( GetWindowRect( m_OutputWindow, &cur ) ) { x = cur.left; y = cur.top; }
        ApplyWindowStyle( WindowModes::WINDOW_MODE_WINDOWED, RECT{ x, y, x + (wr.right - wr.left), y + (wr.bottom - wr.top) } );
    }

    zCView::SetWindowMode( size.x, size.y, 32 );
    // Inform Gothic of the resolution (drives its virtual UI coordinate space).
    zCView::SetVirtualMode( size.x, size.y, 32 );
    POINT virtualSize = { 8192, 8192 };
    zCViewDraw::GetScreen().SetVirtualSize( virtualSize );
#endif
}

static bool CheckTearingSupport() {
    BOOL allowTearing = FALSE;
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory5)))) {
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    }
    return allowTearing == TRUE;
}

bool D3D12GraphicsEngine::CreateSwapChain( INT2 size ) {
    m_Resolution = size;

    m_TearingSupported = CheckTearingSupport();
    
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = static_cast<UINT>( size.x );
    scd.Height = static_cast<UINT>( size.y );
    scd.Format = kBackBufferFormat;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kBackBufferCount;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags = m_TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = m_Device.GetFactory()->CreateSwapChainForHwnd(
        m_Device.GetDirectQueue(), m_OutputWindow, &scd, nullptr, nullptr, swapChain1.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogWarn() << "CreateSwapChainForHwnd failed (0x" << std::hex << hr << ").";
        return false;
    }

    // GD3D11 manages fullscreen itself; disable DXGI's Alt+Enter handling.
    m_Device.GetFactory()->MakeWindowAssociation( m_OutputWindow, DXGI_MWA_NO_ALT_ENTER );

    if ( FAILED( swapChain1.As( &m_SwapChain ) ) ) {
        LogWarn() << "Swapchain does not support IDXGISwapChain3.";
        return false;
    }
    m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();
    m_CleanupFrameIndex.store( m_FrameIndex, std::memory_order_relaxed );

    if ( !CreateFrameResources() ) return false;
    if ( !AcquireBackBufferRTVs() ) return false;
    if ( !CreateDepthBuffer( size ) ) return false;
    if ( !CreateSceneColorTarget( size ) ) return false;   // HDR scene RT (RTV heap now exists with the extra slot)
    CreateBloomResources( size );   // non-fatal: bloom is opt-in (EnableBloom, default off); RenderBloom no-ops if this failed
    if ( !CreateLumPartialBuffer( size ) ) {
        // Non-fatal: RenderLuminanceAdapt() guards on m_LumPartialBuffer and just skips this frame's luminance
        // update if missing — m_LumAdaptedBuffer (created once in Init) keeps its last valid value, so Tonemap
        // is never left reading an unwritten buffer.
        LogWarn() << "D3D12GraphicsEngine::CreateSwapChain: failed to create the dynamic-exposure partial-sum buffer.";
    }

    m_SwapChainReady = true;

    // Bring up the ImGui overlay on the D3D12 backend (mirrors D3D11's OnResize-time init). ImGui
    // texture SRVs are allocated out of our shader-visible heap via callbacks; drawn each Present().
    if ( Engine::ImGuiHandle && !Engine::ImGuiHandle->Initiated && m_SrvHeap ) {
        Engine::ImGuiHandle->InitD3D12( m_OutputWindow, this, m_Device.GetDevice(),
            m_Device.GetDirectQueue(), kBackBufferCount, kBackBufferFormat, m_SrvHeap.Get() );
    }
    return true;
}

bool D3D12GraphicsEngine::CreateFrameResources() {
    ID3D12Device* device = m_Device.GetDevice();

    // RTV descriptor heap: one per backbuffer + 1 for the HDR scene-color target (slot kBackBufferCount).
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kBackBufferCount + 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if ( FAILED( device->CreateDescriptorHeap( &rtvHeapDesc, IID_PPV_ARGS( m_RtvHeap.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_RtvDescriptorSize = device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_RTV );

    // Per-frame command allocators
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS( m_CmdAllocators[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
    }

    // A single command list (created recording, then closed — OnBeginFrame resets it each frame)
    if ( FAILED( device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_CmdAllocators[m_FrameIndex].Get(), nullptr, IID_PPV_ARGS( m_CmdList.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_CmdList->Close();

    // Frame-sync fence
    if ( FAILED( device->CreateFence( m_FenceValues[m_FrameIndex], D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS( m_Fence.ReleaseAndGetAddressOf() ) ) ) )
        return false;
    m_FenceValues[m_FrameIndex]++;

    if ( !m_FenceEvent ) {
        m_FenceEvent = CreateEvent( nullptr, FALSE, FALSE, nullptr );
        if ( !m_FenceEvent ) return false;
    }
    return true;
}

bool D3D12GraphicsEngine::AcquireBackBufferRTVs() {
    ID3D12Device* device = m_Device.GetDevice();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_SwapChain->GetBuffer( i, IID_PPV_ARGS( m_BackBuffers[i].ReleaseAndGetAddressOf() ) ) ) )
            return false;
        m_BackBuffers[i]->SetName( i == 0 ? L"BackBuffer0" : L"BackBuffer1" );
        device->CreateRenderTargetView( m_BackBuffers[i].Get(), nullptr, rtvHandle );
        rtvHandle.ptr += m_RtvDescriptorSize;
    }
    return true;
}

XRESULT D3D12GraphicsEngine::OnBeginFrame() {
    if ( !m_SwapChainReady ) return XR_SUCCESS;

    // Apply a pending TriggerResize() request here — the command list from the previous frame is already
    // Closed+Executed+Presented at this point (no open recording to disrupt), so this is the one place in
    // the frame it's safe to stall the GPU and swap the depth/scene-color/swapchain resources out from under
    // ourselves. Mirrors D3D11GraphicsEngine::OnBeginFrame's `if (NewResolution != Resolution) OnResize(...)`.
    if ( m_NewResolution.x > 0 && m_NewResolution.y > 0
        && ( m_NewResolution.x != m_Resolution.x || m_NewResolution.y != m_Resolution.y ) ) {
        OnResize( m_NewResolution );
    }

    {
        std::lock_guard<std::mutex> lock( m_CopyQueueMutex );
        ReleaseCompletedCopyResources( m_CopyFence->GetCompletedValue() );
    }

    // Finalize any textures a Gothic resource-manager worker thread finished loading since last frame
    // (MyDirectDrawSurface7::Unlock's worker-thread branch calls UpdateDataDeferred + AddFrameLoadedTexture,
    // but leaves MyDirectDrawSurface7::IsReady false until this runs) — mirrors
    // D3D11GraphicsEngine::OnBeginFrame's identical block. D3D12Texture doesn't use the D3D11-only
    // staging-texture/mip-map deferral lists (GetStagingTextures/GetMipMapGeneration stay empty for this
    // backend — its uploads already go through the thread-safe copy-queue batcher), so only the
    // ready-flag handshake is needed here.
    Engine::GAPI->EnterResourceCriticalSection();
    Engine::GAPI->SetFrameProcessedTexturesReady();
    Engine::GAPI->LeaveResourceCriticalSection();

    HRESULT hr = m_CmdAllocators[m_FrameIndex]->Reset();
    if ( FAILED( hr ) ) {
        WaitForGpuIdle();
        hr = m_CmdAllocators[m_FrameIndex]->Reset();
        if ( FAILED( hr ) ) return XR_FAILED;
    }
    hr = m_CmdList->Reset( m_CmdAllocators[m_FrameIndex].Get(), nullptr );
    if ( FAILED( hr ) ) return XR_FAILED;
    ResetCpuContextTracker();

    auto toRT = TransitionBarrier( m_BackBuffers[m_FrameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET );
    m_CmdList->ResourceBarrier( 1, &toRT );

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>( m_FrameIndex ) * m_RtvDescriptorSize;

    // Bind the backbuffer + depth target for the frame. The 3D world pass (OnStartWorldRendering) uses
    // the depth buffer; the 2D/UI PSO has depth disabled, so it draws over the result regardless.
    const bool haveDepth = m_DepthBuffer && m_DsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
    if ( haveDepth ) dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();

    m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, haveDepth ? &dsv : nullptr );
    m_ColorTargetIsHDR = false;
    m_CmdList->ClearRenderTargetView( rtv, m_ClearColor, 0, nullptr );
    if ( haveDepth )  // reversed-Z: clear to 0.0
        m_CmdList->ClearDepthStencilView( dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr );

    // Bind the shader-visible SRV heap for this frame's 2D draws (descriptor tables reference it).
    ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
    m_CmdList->SetDescriptorHeaps( 1, heaps );

    // Reset the per-frame 2D vertex ring + VOB instance ring + default the viewport to the full backbuffer.
    m_UIVertexBufferOffset = 0;
    m_UIOverflowLogged = false;
    m_VobInstanceBufferOffset = 0;
    m_VobInstanceOverflowLogged = false;
    m_SkeletalCBBufferOffset = 0;
    m_SkeletalCBOverflowLogged = false;
    m_ParticleInstanceBufferOffset = 0;
    m_ParticleInstanceOverflowLogged = false;
    m_DecalInstanceBufferOffset = 0;
    m_DecalInstanceOverflowLogged = false;
    m_LightOverflowLogged = false;   // light buffer is rebuilt from 0 each frame in BuildFrameLightBuffer
    if ( !Engine::GAPI->IsGamePaused() )
        UpdateWindAnimation( m_WindBuffer );   // advances windDir/globalTime; DrawVobsInstanced fills min/maxHeight/playerPos
    m_CurrentTexture = nullptr;
    m_CurrentViewport = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    m_CurrentScissor = { 0, 0, m_Resolution.x, m_Resolution.y };

    // this seems to do nothing on its own - CURRENTLY. But who knows what will happen when we do 3d.
    zCView::SetWindowMode(
        m_Resolution.x,
        m_Resolution.y,
        32 );

    // This ensures any 2D UI is rendered with the proper resolution.
    // needs to be per-frame or it won't do anything.
    zCView::SetVirtualMode(
        static_cast<int>(m_Resolution.x),
        static_cast<int>(m_Resolution.y),
        32 );

    //POINT virtualSize = { 8192, 8192 };
    //zCViewDraw::GetScreen().SetVirtualSize( virtualSize );

    m_FrameOpen = true;
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::OnEndFrame() {
    if ( !m_SwapChainReady || !m_FrameOpen ) return XR_SUCCESS;
    Present();
    m_FrameOpen = false;
    m_PresentPending = false;
    return XR_SUCCESS;
}

#ifdef DEBUG_D3D11

static const wchar_t* GetOpName( D3D12_AUTO_BREADCRUMB_OP op ) {
    switch ( op ) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return L"SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return L"BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return L"EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return L"DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return L"DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return L"ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return L"Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return L"CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return L"CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return L"CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return L"CopyTiles";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return L"ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return L"ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return L"ClearUnorderedAccessView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return L"ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return L"ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return L"ExecuteBundle";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return L"Present";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return L"BuildRaytracingAccelerationStructure";
    case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO: return L"EmitRaytracingAccelerationStructurePostBuildInfo";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return L"CopyRaytracingAccelerationStructure";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return L"DispatchRays";
    case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEMETACOMMAND: return L"InitializeMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND: return L"ExecuteMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_ESTIMATEMOTION: return L"EstimateMotion";
    case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return L"EnhancedBarrier";
    default: return L"Unknown D3D12 Command";
    }
}

static const wchar_t* SafeWideString( const wchar_t* str ) {
    return str ? str : L"[Unnamed Object]";
}

static const wchar_t* FindCpuRecordedContext( UINT crashIndex ) {
    const wchar_t* lastKnownContext = L"Unknown/Outside Scopes";

    // Look back through what the CPU logged during recording up to the crash point
    for ( UINT i = 0; i <= crashIndex; ++i ) {
        if ( i < g_CpuContextHistory.size() && g_CpuContextHistory[i].pContextText != nullptr ) {
            lastKnownContext = g_CpuContextHistory[i].pContextText;
        }
    }
    return lastKnownContext;
}

static void PrintNode( const D3D12_AUTO_BREADCRUMB_NODE1* node ) {
    if ( !node ) {
        return;
    }

    std::wstring builder{};
    builder.reserve(1024);

    builder.append( L"--- Outstanding Command List GPU Breadcrumbs ---\n" );
    builder.append( L"Command List Debug Name: " ).append( SafeWideString( node->pCommandListDebugNameW ) ).append( L"\n" );
    builder.append( L"Command Queue Debug Name: " ).append( SafeWideString( node->pCommandQueueDebugNameW ) ).append( L"\n" );
    OutputDebugStringW( builder.c_str() );

    // Log out the History of GPU Operations recorded
    // Note: pLastBreadcrumbValue points to the number of completed operations.
    // Operations *up to* (*node->pLastBreadcrumbValue) finished. Anything past failed or hung.
    UINT completedOps = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;

    builder.clear();
    builder.append( L"Completed Op Count: " ).append( std::to_wstring( completedOps ) ).append( L" / " ).append( std::to_wstring( node->BreadcrumbCount ) ).append( L"\n" );
    OutputDebugStringW( builder.c_str() );

    for ( UINT i = 0; i < node->BreadcrumbCount; ++i ) {
        builder.clear();

        if ( i < completedOps ) {
            builder.append( L" [ok] " );
        } else if ( i == completedOps ) {
            builder.append( L" [ERR] " );
        } else {
            builder.append( L" [ ] " );
        }

        builder.append( L"Op #" ).append( std::to_wstring( i ) ).append( L": " );
        builder.append( GetOpName( node->pCommandHistory[i] ) );

        if ( i == completedOps ) {
            builder.append( L"   <=== !!! HARDWARE HANG DETECTED AT THIS OPERATION !!!" );

            // Pull the exact recorded context step tied directly to this operation cluster!
            const wchar_t* contextAtCrash = FindCpuRecordedContext( completedOps );
            builder.append( L"\n   <=== !!! ACTIVE SCOPE AT TIME OF HARDWARE CRASH: \"" )
                .append( contextAtCrash ).append( L"\" !!!" );
        }

        builder.append( L"\n" );
        OutputDebugStringW( builder.c_str() );
    }

    if ( node->pNext ) {
        PrintNode( node->pNext );
    }
#undef PRINT_NODE_FIELD
}

static void DiagnoseErrors(ID3D12Device* device) {
    // Enable the debug layer before device creation when available (best-effort).
    if ( HMODULE d3d12 = GetModuleHandleA( "d3d12.dll" ) ) {
        auto getDebug = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress( d3d12, "D3D12GetDebugInterface" ));

        ComPtr<ID3D12DeviceRemovedExtendedData1> pRemovedExtendedData;
        if ( SUCCEEDED( device->QueryInterface( IID_PPV_ARGS( pRemovedExtendedData.ReleaseAndGetAddressOf() ) ) ) ) {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 output;
            if (SUCCEEDED( pRemovedExtendedData->GetAutoBreadcrumbsOutput1( &output ) )) {
                PrintNode( output.pHeadAutoBreadcrumbNode );
            }
        }
    }
}
#endif

XRESULT D3D12GraphicsEngine::Present() {
    if ( !m_SwapChainReady || !m_FrameOpen ) return XR_SUCCESS;

    // Draw the ImGui overlay last, on top of the 2D UI, while the backbuffer is still a render target.
    // The SRV heap is bound (OnBeginFrame); re-bind the RTV defensively in case a draw changed it.
    if ( Engine::ImGuiHandle && Engine::ImGuiHandle->Initiated ) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>( m_FrameIndex ) * m_RtvDescriptorSize;
        m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, nullptr );
        Engine::ImGuiHandle->RenderLoopD3D12( m_CmdList.Get() );
    }

    auto toPresent = TransitionBarrier( m_BackBuffers[m_FrameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT );
    m_CmdList->ResourceBarrier( 1, &toPresent );

    // Submit any batched texture/buffer uploads accumulated this frame and insert the single
    // copy->direct cross-queue wait BEFORE the frame's graphics execute, so everything sampled below
    // is ready. Cheap no-op when nothing was cached in.
    FlushTextureUploads();

    if ( FAILED( m_CmdList->Close() ) ) return XR_FAILED;

    ID3D12CommandList* lists[] = { m_CmdList.Get() };
    m_Device.GetDirectQueue()->ExecuteCommandLists( 1, lists );

    const bool vsync = Engine::GAPI->GetRendererState().RendererSettings.EnableVSync;
    const UINT syncInterval = vsync ? 1 : 0;

    UINT presentFlags = 0;
    if (!vsync && m_TearingSupported) {
        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    
    HRESULT hr = m_SwapChain->Present( syncInterval, presentFlags );
    if ( FAILED( hr ) ) {
        auto r = static_cast<uint32_t>(hr);
        if ( hr == DXGI_ERROR_DEVICE_REMOVED) {
#ifdef DEBUG_D3D11
            DiagnoseErrors( m_Device.GetDevice() );
#endif
            auto removedReason = m_Device.GetDevice()->GetDeviceRemovedReason();
            auto msg = std::format( "D3D12 Present failed (0x{:08X}, reason: 0x{:08X})", r, static_cast<uint32_t>(removedReason) );
            LogWarn() << "D3D12 Present failed (0x" << std::hex << r << ").";
            MessageBoxA( NULL, msg.c_str(), "GD3D11 (DX12): Error", MB_OK );
        } else {
            auto msg = std::format( "D3D12 Present failed (0x{:08X})", r );
            LogWarn() << "D3D12 Present failed (0x" << std::hex << r << ").";
            MessageBoxA( NULL, msg.c_str(), "GD3D11 (DX12): Error", MB_OK);
        }
        exit( hr );
        return XR_FAILED;
    }

    MoveToNextFrame();
    return XR_SUCCESS;
}

void D3D12GraphicsEngine::MoveToNextFrame() {
    const UINT64 currentFenceValue = m_FenceValues[m_FrameIndex];
    m_Device.GetDirectQueue()->Signal( m_Fence.Get(), currentFenceValue );

    m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();
    m_CleanupFrameIndex.store( m_FrameIndex, std::memory_order_relaxed );

    if ( m_Fence->GetCompletedValue() < m_FenceValues[m_FrameIndex] ) {
        m_Fence->SetEventOnCompletion( m_FenceValues[m_FrameIndex], m_FenceEvent );
        WaitForSingleObject( m_FenceEvent, INFINITE );
    }
    m_FenceValues[m_FrameIndex] = currentFenceValue + 1;

    // Clean up all resources slated for deletion from its last pass. Swap the slot's jobs out under the
    // lock, then run them unlocked — a job (e.g. QueueSrvResourceForRelease's) can itself lock other
    // mutexes (m_SrvHeapMutex), and a worker thread may be concurrently emplace_back-ing into a
    // different slot via QueueCleanupJob.
    std::vector<std::move_only_function<void()>> jobs;
    {
        std::lock_guard<std::mutex> lock( m_CleanupMutex );
        jobs.swap( m_PerFrameCleanupItems[m_FrameIndex] );
    }
    for ( auto& cleanupCallback : jobs ) {
        cleanupCallback(); // Calls FreeSrvSlot() and drops the captured ComPtrs
    }
}

void D3D12GraphicsEngine::WaitForGpuIdle() {
    if ( !m_Fence || !m_Device.GetDirectQueue() ) return;

    // Submit any still-open upload batch first, so its copies are actually queued before we wait on
    // the copy fence below (an un-flushed batch has recorded copies that were never signaled).
    FlushTextureUploads();

    if ( m_CopyFence && m_CopyFenceEvent ) {
        const UINT64 copyFenceValue = m_CopyFenceValue;
        if ( copyFenceValue > 0 && m_CopyFence->GetCompletedValue() < copyFenceValue ) {
            m_CopyFence->SetEventOnCompletion( copyFenceValue, m_CopyFenceEvent );
            WaitForSingleObject( m_CopyFenceEvent, INFINITE );
        }
    }

    // Calculate a "one-off" future value beyond all active frames.
    // This avoids colliding with any m_FenceValues currently in flight.
    UINT64 completedValue = m_Fence->GetCompletedValue();
    UINT64 idleValue = completedValue + 1;

    // Scan all active frames to ensure we choose a value strictly greater 
    // than any pending fence signals.
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( m_FenceValues[i] >= idleValue ) {
            idleValue = m_FenceValues[i] + 1;
        }
    }

    // Queue the signal command on the GPU timeline.
    // Because GPU execution is sequential, this milestone is only reached 
    // when ALL work previously queued has finished.
    if ( FAILED( m_Device.GetDirectQueue()->Signal( m_Fence.Get(), idleValue ) ) ) return;

    // Perform a CPU wait using a transient local event.
    if ( m_Fence->GetCompletedValue() < idleValue ) {
        // Create an anonymous, auto-reset event
        HANDLE eventHandle = CreateEventEx( nullptr, nullptr, 0, EVENT_ALL_ACCESS );
        if ( eventHandle ) {
            m_Fence->SetEventOnCompletion( idleValue, eventHandle );
            WaitForSingleObject( eventHandle, INFINITE );
            CloseHandle( eventHandle );
        }
    }

    // Update our CPU-side trackers so they know the GPU is completely caught up.
    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        m_FenceValues[i] = idleValue;
    }
}

void D3D12GraphicsEngine::FlushCommandListSync() {
    // Closes + submits whatever is currently recorded in m_CmdList and blocks until the GPU has consumed
    // it, then Resets the SAME per-frame allocator/list so recording can continue. This is NOT the normal
    // once-per-frame Close/Execute in Present() (no PRESENT transition, no MoveToNextFrame/frame-index
    // advance) — it exists solely for GetBackbufferData, which must synchronously read pixels back mid-
    // frame (Gothic's savegame-thumbnail Lock() fires before this frame's own Present).
    if ( !m_CmdList || !m_Fence || !m_Device.GetDirectQueue() ) return;

    // Ensure any batched uploads recorded before this mid-frame sync are submitted + waited-on, so the
    // pixels read back here reflect textures cached in this frame.
    FlushTextureUploads();

    if ( FAILED( m_CmdList->Close() ) ) return;

    ID3D12CommandList* lists[] = { m_CmdList.Get() };
    m_Device.GetDirectQueue()->ExecuteCommandLists( 1, lists );

    const UINT64 waitValue = ++m_FenceValues[m_FrameIndex];
    if ( SUCCEEDED( m_Device.GetDirectQueue()->Signal( m_Fence.Get(), waitValue ) ) ) {
        if ( m_Fence->GetCompletedValue() < waitValue ) {
            m_Fence->SetEventOnCompletion( waitValue, m_FenceEvent );
            WaitForSingleObject( m_FenceEvent, INFINITE );
        }
    }

    m_CmdAllocators[m_FrameIndex]->Reset();
    m_CmdList->Reset( m_CmdAllocators[m_FrameIndex].Get(), nullptr );
}

void D3D12GraphicsEngine::RestoreFrameRenderTarget() {
    // Mirrors the RTV/viewport/heap portion of OnBeginFrame's tail (NOT the per-frame ring-offset resets —
    // those must stay untouched, this runs mid-frame after rings may already have been consumed).
    if ( !m_CmdList || !m_RtvHeap ) return;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>( m_FrameIndex ) * m_RtvDescriptorSize;

    const bool haveDepth = m_DepthBuffer && m_DsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
    if ( haveDepth ) dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();

    m_CmdList->OMSetRenderTargets( 1, &rtv, FALSE, haveDepth ? &dsv : nullptr );
    m_ColorTargetIsHDR = false;

    if ( m_SrvHeap ) {
        ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
        m_CmdList->SetDescriptorHeaps( 1, heaps );
    }

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );
    m_CurrentViewport = vp;
    m_CurrentScissor = sc;
    m_CurrentTexture = nullptr;
}

/** Unpacks one R10G10B10A2_UNORM texel (the swapchain/tonemap-target format) into BGRA8 — the 32bpp
    layout MyDirectDrawSurface7::Lock's DDLOCK_READONLY path hands to Gothic (masks 0x00FF0000/0x0000FF00/
    0x000000FF for R/G/B, alpha unused). 10->8 bit is a rounded scale; plenty for a save thumbnail. */
static void UnpackR10G10B10A2ToBGRA8( uint32_t packed, byte* dstBGRA ) {
    const uint32_t r10 = packed & 0x3FFu;
    const uint32_t g10 = (packed >> 10) & 0x3FFu;
    const uint32_t b10 = (packed >> 20) & 0x3FFu;
    dstBGRA[0] = static_cast<byte>( (b10 * 255u + 511u) / 1023u );
    dstBGRA[1] = static_cast<byte>( (g10 * 255u + 511u) / 1023u );
    dstBGRA[2] = static_cast<byte>( (r10 * 255u + 511u) / 1023u );
    dstBGRA[3] = 255;
}

/** Returns the data of the backbuffer (savegame thumbnail / screenshot). See D3D11GraphicsEngine's
    counterpart for the calling convention: MyDirectDrawSurface7::Lock calls OnStartWorldRendering() right
    before this to force a fresh world render, then reads *data as a top-down 32bpp BGRA8 buffer. */
void D3D12GraphicsEngine::GetBackbufferData( bool thumbnail, byte** data, INT2& buffersize, int& pixelsize ) {
    *data = nullptr;
    pixelsize = 4;
    buffersize = thumbnail ? INT2( 256, 256 ) : m_Resolution;

    if ( !m_CmdList || !m_SwapChainReady || !m_SceneColor || !m_Pipelines.Tonemap.PSO || !m_Pipelines.Tonemap.RootSig ) {
        LogInfo() << (thumbnail ? "Thumbnail failed. D3D12 backend not ready" : "GetBackbufferData failed. D3D12 backend not ready");
        return;
    }

    ID3D12Device* device = m_Device.GetDevice();

    // The world + tonemap-resolve commands recorded by the OnStartWorldRendering() call the caller just
    // made are still sitting unexecuted in m_CmdList — nothing has actually landed on the GPU yet. Flush
    // + block (D3D12 has no immediate-context Flush() like D3D11's GetContext()->Flush()), then keep
    // recording afterwards so the rest of this frame's 2D UI + Present continue on the same list.
    FlushCommandListSync();

    // Re-tonemap the (now GPU-resident) HDR scene into a fresh render target sized to what the caller
    // asked for. We deliberately do NOT copy the real swapchain backbuffer: it's DXGI_FORMAT_R10G10B10A2_
    // UNORM (kBackBufferFormat) which the Tonemap PSO is baked for, but Gothic's Lock() consumer expects a
    // simple 32bpp BGRA8 buffer, and a thumbnail additionally needs downscaling to 256x256 — both are
    // exactly what one more tonemap draw at a smaller viewport gives us for free (mirrors D3D11's
    // GetBackbufferData drawing HDRBackBuffer through PS_PFX_GammaCorrectInv into a differently-sized RT).
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = static_cast<UINT64>( buffersize.x );
    td.Height = static_cast<UINT>( buffersize.y );
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = kBackBufferFormat;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = kBackBufferFormat;

    Microsoft::WRL::ComPtr<D3D12MA::Allocation> captureAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource> captureTex;
    if ( FAILED( m_Allocator->CreateResource( &allocDesc, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
        &clearValue, captureAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( captureTex.ReleaseAndGetAddressOf() ) ) ) ) {
        LogInfo() << (thumbnail ? "Thumbnail failed. Capture texture could not be created" : "GetBackbufferData failed. Capture texture could not be created");
        RestoreFrameRenderTarget();
        return;
    }
    captureTex->SetName( L"BackbufferCapture" );

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> captureRtvHeap;
    if ( FAILED( device->CreateDescriptorHeap( &rtvHeapDesc, IID_PPV_ARGS( captureRtvHeap.ReleaseAndGetAddressOf() ) ) ) ) {
        LogInfo() << (thumbnail ? "Thumbnail failed. RTV heap could not be created" : "GetBackbufferData failed. RTV heap could not be created");
        RestoreFrameRenderTarget();
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE captureRtv = captureRtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView( captureTex.Get(), nullptr, captureRtv );

    // m_SceneColor was left in PIXEL_SHADER_RESOURCE state by OnStartWorldRendering's ResolveSceneToBackBuffer
    // call (now genuinely true on the GPU after the flush above) — safe to sample without re-transitioning.
    m_CmdList->OMSetRenderTargets( 1, &captureRtv, FALSE, nullptr );
    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( buffersize.x ), static_cast<float>( buffersize.y ), 0.0f, 1.0f };
    const D3D12_RECT     sc = { 0, 0, buffersize.x, buffersize.y };
    m_CmdList->RSSetViewports( 1, &vp );
    m_CmdList->RSSetScissorRects( 1, &sc );

    if ( m_SrvHeap ) {
        ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
        m_CmdList->SetDescriptorHeaps( 1, heaps );
    }

    m_CmdList->SetPipelineState( m_Pipelines.Tonemap.PSO.Get() );
    m_CmdList->SetGraphicsRootSignature( m_Pipelines.Tonemap.RootSig.Get() );
    m_CmdList->SetGraphicsRootDescriptorTable( 0, GetSrvGpuHandle( m_SceneColorSrvSlot ) );
    auto& tonemapSettings = Engine::GAPI->GetRendererState().RendererSettings;
    const float exposureConsts[2] = { tonemapSettings.Exposure > 0.0f ? tonemapSettings.Exposure : 1.0f, tonemapSettings.HDRMiddleGray };
    m_CmdList->SetGraphicsRoot32BitConstants( 1, 2, exposureConsts, 0 );
    m_CmdList->SetGraphicsRootShaderResourceView( 2, m_LumAdaptedBuffer->GetGPUVirtualAddress() );
    m_CmdList->IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    m_CmdList->IASetVertexBuffers( 0, 0, nullptr );
    m_CmdList->DrawInstanced( 3, 1, 0, 0 );

    auto toCopySrc = TransitionBarrier( captureTex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE );
    m_CmdList->ResourceBarrier( 1, &toCopySrc );

    D3D12_RESOURCE_DESC capDesc = captureTex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0; UINT64 rowSizeBytes = 0, totalBytes = 0;
    device->GetCopyableFootprints( &capDesc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes );

    D3D12MA::ALLOCATION_DESC rbAllocDesc = {};
    rbAllocDesc.HeapType = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rbDesc = {};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = totalBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<D3D12MA::Allocation> readbackAlloc;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if ( FAILED( m_Allocator->CreateResource( &rbAllocDesc, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, readbackAlloc.ReleaseAndGetAddressOf(), IID_PPV_ARGS( readback.ReleaseAndGetAddressOf() ) ) ) ) {
        LogInfo() << (thumbnail ? "Thumbnail failed. Readback buffer could not be created" : "GetBackbufferData failed. Readback buffer could not be created");
        RestoreFrameRenderTarget();
        return;
    }

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = readback.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = captureTex.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    m_CmdList->CopyTextureRegion( &dstLoc, 0, 0, 0, &srcLoc, nullptr );

    // Block until the copy has actually landed in the readback buffer before we Map it.
    FlushCommandListSync();

    byte* d = new byte[ static_cast<size_t>( buffersize.x ) * static_cast<size_t>( buffersize.y ) * 4 ];
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>( totalBytes ) };
    void* mapped = nullptr;
    if ( SUCCEEDED( readback->Map( 0, &readRange, &mapped ) ) ) {
        const uint8_t* srcRow = reinterpret_cast<const uint8_t*>( mapped );
        byte* dstRow = d;
        for ( int row = 0; row < buffersize.y; ++row ) {
            const uint32_t* srcPixels = reinterpret_cast<const uint32_t*>( srcRow );
            byte* dstPixels = dstRow;
            for ( int col = 0; col < buffersize.x; ++col ) {
                UnpackR10G10B10A2ToBGRA8( srcPixels[col], dstPixels );
                dstPixels += 4;
            }
            srcRow += footprint.Footprint.RowPitch;
            dstRow += static_cast<size_t>( buffersize.x ) * 4;
        }
        readback->Unmap( 0, nullptr );
    } else {
        LogInfo() << (thumbnail ? "Thumbnail failed" : "GetBackbufferData failed");
    }

    *data = d;

    // The flushes above Reset the command list without leaving anything bound — rebind the swapchain
    // backbuffer so Gothic's subsequent 2D UI draws (and Present's ImGui pass) land correctly.
    RestoreFrameRenderTarget();
}

bool D3D12GraphicsEngine::ResizeSwapChain( INT2 size ) {
    if ( !m_SwapChainReady ) return false;
    if ( size.x <= 0 || size.y <= 0 ) return false;
    if ( size.x == m_Resolution.x && size.y == m_Resolution.y ) return true;

    WaitForGpuIdle();
    for ( UINT i = 0; i < kBackBufferCount; ++i ) m_BackBuffers[i].Reset();

    // Must pass the SAME flags the swapchain was created with (CreateSwapChainForHwnd's scd.Flags) —
    // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING can't be added/removed via ResizeBuffers, only the create call.
    const UINT swapChainFlags = m_TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT hr = m_SwapChain->ResizeBuffers( kBackBufferCount,
        static_cast<UINT>( size.x ), static_cast<UINT>( size.y ), kBackBufferFormat, swapChainFlags );
    if ( FAILED( hr ) ) {
        LogWarn() << "D3D12 ResizeBuffers failed (0x" << std::hex << hr << ").";
        return false;
    }

    m_Resolution = size;
    m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();
    m_CleanupFrameIndex.store( m_FrameIndex, std::memory_order_relaxed );
    if ( !AcquireBackBufferRTVs() ) return false;
    if ( !CreateDepthBuffer( size ) ) return false;   // GPU is idle (WaitForGpuIdle above), safe to recreate
    if ( !CreateSceneColorTarget( size ) ) return false;   // HDR scene RT tracks the new resolution too
    CreateBloomResources( size );   // non-fatal: see the CreateSwapChain call site
    if ( !CreateLumPartialBuffer( size ) ) {
        LogWarn() << "D3D12GraphicsEngine::OnResize: failed to create the dynamic-exposure partial-sum buffer.";
    }
    return true;
}

XRESULT D3D12GraphicsEngine::OnResize( INT2 newSize ) {
    if ( newSize.x <= 0 || newSize.y <= 0 ) return XR_SUCCESS;

    // Never exceed the monitor's current desktop resolution, no matter where the size came from (initial
    // SetWindow() at launch, an in-game TriggerResize(), a stale/forced ini or -zRes commandline value,
    // etc.) — clamping only the ImGui dropdown's offered list isn't enough, since callers can still reach
    // OnResize() directly with an oversized value. A window/swapchain bigger than the desktop is at best
    // wasted GPU memory (DXGI_SCALING_STRETCH silently covers for it) and at worst an off-screen window.
    const int maxWidth = GetSystemMetrics( SM_CXSCREEN );
    const int maxHeight = GetSystemMetrics( SM_CYSCREEN );
    if ( maxWidth > 0 && maxHeight > 0 && ( newSize.x > maxWidth || newSize.y > maxHeight ) ) {
        LogWarn() << "D3D12GraphicsEngine::OnResize: requested " << newSize.x << "x" << newSize.y
            << " exceeds the desktop resolution (" << maxWidth << "x" << maxHeight << ") — clamping.";
        newSize.x = std::min( newSize.x, maxWidth );
        newSize.y = std::min( newSize.y, maxHeight );
        m_NewResolution = newSize;   // keep in sync so OnBeginFrame doesn't re-trigger this every frame
    }

    if ( m_SwapChainReady && newSize.x == m_Resolution.x && newSize.y == m_Resolution.y )
        return XR_SUCCESS; // nothing to do

    ResizeOutputWindow( newSize );

    if ( !m_SwapChainReady ) {
        if ( !CreateSwapChain( newSize ) ) {
            LogWarn() << "D3D12GraphicsEngine::OnResize: swapchain creation failed.";
            return XR_FAILED;
        }
        LogInfo() << "D3D12 swapchain created (" << newSize.x << "x" << newSize.y << ").";
    } else {
        if ( !ResizeSwapChain( newSize ) ) {
            // Depth/scene-color/light-cull targets may now be a mix of old- and new-resolution (or null, if
            // resource creation itself failed). Don't retry every frame — leave m_Resolution at whatever
            // ResizeSwapChain last got to and let the render path's existing null checks (haveDepth, etc.)
            // keep the frame from touching a mismatched/null DSV until the user tries again.
            LogWarn() << "D3D12GraphicsEngine::OnResize: ResizeSwapChain failed (" << newSize.x << "x" << newSize.y << ").";
            m_NewResolution = m_Resolution;
            return XR_FAILED;
        }
    }

    if ( Engine::ImGuiHandle && Engine::ImGuiHandle->Initiated ) {
        Engine::ImGuiHandle->OnResize( newSize );
    }
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::TriggerResize( INT2 resolution ) {
    // Just record the request (mirrors D3D11GraphicsEngine::TriggerResize / NewResolution) — applied at the
    // top of the next OnBeginFrame, never here. This can run mid-frame (e.g. from the ImGui settings window,
    // while the command list is open and mid-recording), and resizing the swapchain/depth/scene-color targets
    // right now would touch resources the currently-recording command list still references.
    m_NewResolution = resolution;
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::Clear( const float4& /*color*/ ) {
    return XR_SUCCESS; // first-light clears to the sentinel color in OnBeginFrame
}

XRESULT D3D12GraphicsEngine::CreateVertexBuffer( std::unique_ptr<GfxVertexBuffer>& outBuffer ) {
    outBuffer = std::make_unique<D3D12VertexBuffer>();
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::CreateTexture( GfxTexture** outTexture ) {
    if ( outTexture ) *outTexture = new D3D12Texture();
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::CreateTexture( std::unique_ptr<GfxTexture>& outTexture ) {
    outTexture = std::make_unique<D3D12Texture>();
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::CreateTexture( std::unique_ptr<D3D12Texture>& outTexture ) {
    outTexture = std::make_unique<D3D12Texture>();
    return XR_SUCCESS;
}

XRESULT D3D12GraphicsEngine::GetDisplayModeList( std::vector<DisplayModeInfo>* modeList, bool includeSuperSampling ) {
    if ( !modeList ) return XR_SUCCESS;

    modeList->clear();
    if ( XR_SUCCESS != DXGI_GetDisplayModeList( m_Device.GetDevice()->GetAdapterLuid(), m_OutputWindow, &m_CachedDisplayModes ) ) {
        m_CachedDisplayModes.clear();
        m_CachedDisplayModes.push_back( DisplayModeInfo( std::max<int>( 1, m_Resolution.x ), std::max<int>( 1, m_Resolution.y ), 60, 1 ) );
    }
    return AppendCachedDisplayModes( modeList, includeSuperSampling );
}

BaseLineRenderer* D3D12GraphicsEngine::GetLineRenderer() {
    return m_LineRenderer.get();
}
