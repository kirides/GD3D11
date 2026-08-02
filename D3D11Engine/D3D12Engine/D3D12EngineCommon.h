#pragma once
// Shared internal helpers for the D3D12 backend translation units (D3D12GraphicsEngine.cpp and its
// split siblings D3D12Engine2D.cpp / D3D12PostFX.cpp / D3D12Scene.cpp). These were file-local statics
// in the original monolith; they are promoted here so the split TUs share ONE definition each.
// This header is D3D12-backend-private — do not include it outside D3D12Engine/.
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <deque>
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include "D3D12TracyDebug.h"

class zCTexture;
class zCVob;
class zCVobLight;
struct MeshInfo;
struct MeshVisualInfo;
struct SkeletalVobInfo;
struct SkeletalMeshVisualInfo;

// ---- Per-frame draw records shared between the geometry collectors (D3D12Scene.cpp) and the shadow
// modules (D3D12ShadowMap.cpp / D3D12PointShadows.cpp). They used to be file-local to the scene TU; the
// shadow passes consume the very same records (one collection pass feeds the main view, the CSM cascades
// and the point-light cubes), so the declarations live here now. All three are pure D3D12 handles +
// non-owning Gothic pointers — a record is valid for exactly the frame that built it.

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

// matSrvIndex indexes g_SkelMatSrvs (below): the per-material diffuse SRV heap slots for this vob's
// visual->SkeletalMeshes, snapshotted on the main thread while the model's shared texani slots were still
// set for THIS instance. A pool-thread recorder (the MT cascades) must use it instead of calling
// UpdateMeshLibTexAniState + GetAniTexture itself (Gothic's texani state is per-MODEL shared, not thread-safe).
struct FrameSkelDraw {
    SkeletalVobInfo*          vobInfo;
    SkeletalMeshVisualInfo*   visual;
    D3D12_GPU_VIRTUAL_ADDRESS instCb;
    D3D12_GPU_VIRTUAL_ADDRESS boneCb;
    uint32_t                  matSrvIndex;
};

// owner = the skeletal vob this attachment hangs off of (its NPC/MOB) — needed so point-shadow self-shadow
// exclusion (D3D12PointShadows::BuildExcludeList) can skip a torch-holding NPC's own attachments too, not
// just its base mesh; unused (nullptr-safe) by the main-view/CSM consumers, which don't exclude anything.
// srvSlot = the same main-thread-resolved diffuse SRV HEAP SLOT as FrameSkelDraw::matSrvIndex, for the same
// reason — the attachment passes are fully bindless (b6 MaterialCB, no t0 table), so what the recorder needs
// is the heap index, not a table handle. The main-view prepass/color paths still resolve `tex` themselves
// (they CacheIn, which the shadow paths deliberately don't), so both fields stay live.
// alphaTested = can the depth/caster PS' `clip(diffuse.a - 0.5)` ever discard for this attachment? Resolved on
// the main thread with srvSlot (a pool-thread recorder must not read Gothic texture state), and used by every
// depth-only consumer to route the attachment through a no-pixel-shader PSO when it can't.
struct FrameAttachDraw {
    MeshInfo*                   mesh;
    zCTexture*                  tex;
    D3D12_VERTEX_BUFFER_VIEW    instView;
    const zCVob*                owner;
    UINT                        srvSlot;
    bool                        alphaTested;
};

// Per-frame GPU point light. Filled by BuildFrameLightBuffer (D3D12Scene.cpp); the point-shadow slot
// selection (D3D12PointShadows::SelectShadowedLights) reads Range/PositionWorld/Color.w and writes
// ShadowCubeIndex/ShadowOrigin/ShadowRange. Mirrored in HLSL by Shaders/D3D12/include/ForwardPlusTypes.hlsl
// and LightCull.hlsl's TiledPointLight — all three MUST be changed together.
//
// This USED to be byte-identical to D3D11's 48-byte TiledPointLight; it deliberately is not any more. The
// D3D12 backend needs a light to be able to sample a shadow cube that is NOT centred on itself:
//   * clustered static lights (the 10-30 "atmospheric" fill lights a Gothic room is lit with) share ONE cube
//     rendered from their cluster centroid, so the cube lookup origin/far-plane differ from the light's own;
//   * the shadow tier bit selects which cube array the slot lives in (see kShadowTierLow).
// The two shader sides are separate declarations, so D3D11's TiledPointLight is unaffected.
struct GPULight {
    DirectX::XMFLOAT3 PositionView;    // 0
    float             Range;           // 12  shading falloff radius (range-clamped for unshadowed statics)
    DirectX::XMFLOAT4 Color;           // 16  (.w = static flag 0/1)
    DirectX::XMFLOAT3 PositionWorld;   // 32
    int32_t           ShadowCubeIndex; // 44  -1 = no shadow, else slot | tier bit (see kShadowTierLow)
    DirectX::XMFLOAT3 ShadowOrigin;    // 48  cube centre — == PositionWorld unless this light is clustered
    float             ShadowRange;     // 60  cube far-plane basis (far = ShadowRange*2) — == Range unless clustered
};
static_assert( sizeof( GPULight ) == 64, "GPULight must match the HLSL GPULight in ForwardPlusTypes.hlsl" );

// High bit of GPULight::ShadowCubeIndex selecting the LOW-RESOLUTION static cube array
// (D3D12PointShadows::kStaticCubeSize) over the full-res dynamic one. Bit 30, so the value stays a positive
// int and the existing "ShadowCubeIndex >= 0 means shadowed" test in every shader keeps working untouched;
// the slot itself is the low 30 bits. Mirrored as kShadowTierLow in ForwardPlusTypes.hlsl.
constexpr int32_t kShadowTierLow = 0x40000000;
// Bit 29: this light's slot also has a valid DYNAMIC (skeletal overlay) cube, so the lit pass samples the
// dynamic array as well and mins the two results. Full-res slots only — the low tier has no dynamic twin.
// Absent = pure static shadow, and no second sample is taken. Mirrored in ForwardPlusTypes.hlsl.
constexpr int32_t kShadowHasDynamic = 0x20000000;
constexpr int32_t kShadowSlotMask = 0x1FFFFFFF;

// This frame's visible-VOB instance-ring snapshot (UploadFrameVobInstances) — the depth prepass, the color
// pass AND the point-shadow static-VOB gather all draw from it. Defined in D3D12Scene.cpp.
extern std::vector<FrameVobUpload> g_FrameVobUploads;
// This frame's collected static vobs (the D3D12 counterpart of D3D11's RenderedVobs). Read at frame end by
// D3D12GraphicsEngine::StoreVobPreviousTransforms to roll each vob's world matrix into its motion-vector history.
struct VobInfo;
extern std::vector<VobInfo*> g_FrameVobs;

// Per-vob snapshot of the diffuse SRV heap SLOT for each entry of visual->SkeletalMeshes, in that map's
// iteration order — see FrameSkelDraw::matSrvIndex. Slots, not descriptor handles: the skeletal root signature
// has no diffuse table any more, the shaders index ResourceDescriptorHeap[] with these. Only the live prefix
// [0, g_SkelMatSrvCount) is valid each frame. Defined in D3D12Scene.cpp (PrepareFrameSkeletals owns it);
// read by the CSM cascade recorder.
// DEQUE, not vector, and that matters: the CSM cascades now record on worker threads that hold a
// `const std::vector<UINT>*` into this container (D3D12ShadowMap::RecordCascade), while the main thread is
// still appending to it — the point-shadow prepare runs its own PrepareFrameSkeletals after the cascade jobs
// have launched. A vector's push_back would reallocate and turn those pointers into use-after-free; deque
// guarantees references to existing elements survive a push_back.
// alphaTested rides along for the same reason it does on FrameAttachDraw: the shadow/depth casters want to skip
// the alpha-clip pixel shader entirely for a material whose diffuse has no alpha channel, and only the main
// thread may ask Gothic that question.
struct SkelMatSlot {
    UINT slot;
    bool alphaTested;
};
extern std::deque<std::vector<SkelMatSlot>> g_SkelMatSrvs;
extern size_t g_SkelMatSrvCount;

// Water surfaces peeled out of the opaque world pass (BuildWorldDrawCommands, D3D12Scene.cpp) and drawn
// later by DrawWaterSurfaces (D3D12Water.cpp), grouped by texture to minimize SRV binds. Both run on the
// same thread within one frame (OnStartWorldRendering), so a single file-scope scratch map is safe; it is
// filled at build time and cleared by the water pass. Defined in D3D12Water.cpp.
extern std::unordered_map<zCTexture*, std::vector<MeshInfo*>> g_FrameWaterSurfaces;

// Alpha-blended world-mesh surfaces (ice, glass, magic barriers) peeled out of the opaque world pass by
// BuildWorldDrawCommands (D3D12Scene.cpp) and drawn back-to-front by DrawWorldTransparencyMeshes
// (D3D12Transparency.cpp), which also owns the definition. Mirrors D3D11's FrameTransparencyMeshes.
// Same single-threaded per-frame lifetime as g_FrameWaterSurfaces above.
class zCMaterial;
struct WorldTransparencyMesh {
    zCMaterial* Material;
    MeshInfo*   Mesh;
    float       DistanceSq;   // camera -> mesh/section bbox center; sorted DESCENDING (far first)
};
extern std::vector<WorldTransparencyMesh> g_FrameWorldTransparency;
// The two material types D3D11 collects by TYPE rather than by alpha func, each drawn as its own sorted
// sub-pass with its own pixel shader (D3D11: FrameTransparencyMeshesPortal / FrameTransparencyMeshesWaterfall).
extern std::vector<WorldTransparencyMesh> g_FrameWorldTransparencyPortal;   // MT_Portal (gated on DrawG1ForestPortals)
extern std::vector<WorldTransparencyMesh> g_FrameWorldTransparencyFoam;     // MT_WaterfallFoam

// Blended instanced VOBs (cobwebs, hanging cloth, magic sheets) peeled out of the opaque VOB ExecuteIndirect
// set by BuildVobDrawCommands (D3D12Scene.cpp) and replayed by DrawVobAlphaMeshes (D3D12Transparency.cpp,
// which owns the definition). Mirrors D3D11's m_AlphaMeshes / DrawFrameAlphaMeshes. Everything is resolved at
// build time so the pass itself only switches PSO + a handful of root constants per entry; the buffer views
// point into this frame's instance ring, so the list is strictly single-frame (same lifetime as
// g_FrameWaterSurfaces above) and is cleared by UploadFrameVobInstances as well as by the pass.
struct VobAlphaMesh {
    D3D12_VERTEX_BUFFER_VIEW MeshVBV;        // packed ExVertexStruct
    D3D12_VERTEX_BUFFER_VIEW InstVBV;        // per-instance VobInstanceInfo (UNculled — see BuildVobDrawCommands)
    D3D12_INDEX_BUFFER_VIEW  IBV;
    uint32_t MatIndices[3];                  // b6 MaterialCB { normal, orm, diffuse }, same order as VobDrawCommand
    float    WindMinHeight;                  // b4[4..5], per visual
    float    WindMaxHeight;
    UINT     IndexCount;
    UINT     NumInstances;
    bool     Additive;                       // zMAT_ALPHA_FUNC_ADD -> additive blend, else plain alpha blending
};
extern std::vector<VobAlphaMesh> g_FrameVobAlpha;

// Per-frame linear-fog parameters, bound to the 3D shaders as 8 root 32-bit constants. Field order MUST match
// the HLSL `cbuffer FogCB { float3 FogColor; float FogNear; float3 CamPosWS; float FogFar; }` (root constants
// map by DWORD offset). The VS computes distance(worldPos, CamPosWS) (== view-space distance for a rigid view
// transform), the PS lerps toward FogColor over [FogNear, FogFar].
struct FogConstants {
    float FogColor[3];
    float FogNear;
    float CamPos[3];
    float FogFar;
};
static_assert( sizeof( FogConstants ) == 32, "FogConstants must be 8 DWORDs to match the fog root constants" );
// Built from Gothic's sky state in D3D12Scene.cpp (which owns the sky-color/height-fog state it reads);
// exported for the passes that live in their own TU and bind the same b1.
FogConstants MakeSceneFogConstants();

// Upload-heap type used by every persistently-mapped ring / staging allocation. A single knob (kept
// as an inline variable so all split TUs see the same value); the GPU_UPLOAD path is future work.
inline D3D12_HEAP_TYPE DefaultUploadHeapType = D3D12_HEAP_TYPE_UPLOAD;
inline bool GetSkipDefaultHeapCopyAfterUpload() {
    return DefaultUploadHeapType == D3D12_HEAP_TYPE_GPU_UPLOAD;
}

// HDR scene-color format: the 3D passes accumulate lighting here in linear-ish FLOAT (values may
// exceed 1.0), then the tonemap resolve writes the swapchain. Shared by the engine-core target
// creation and the post-FX (bloom/luminance) passes that sample it.
inline constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Display-buffer format used ONLY when real HDR scanout is active. The tonemap resolve, Gothic's 2D UI, SMAA,
// the sharpen pass and the ImGui overlay then composite into an m_HdrDisplay of this format instead of writing
// the swapchain directly; it stores EXTENDED-sRGB values (the normal gamma encoding, but >1.0 allowed = brighter
// than paper white), so every one of those passes keeps its SDR shader and its SDR blending behaviour, and only
// HdrEncode.hlsl converts to ST.2084 at present time.
inline constexpr DXGI_FORMAT kHdrDisplayFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// --- Motion-vector / normal G-buffer (D3D12Motion.cpp) ---
// Two extra render targets the Forward+ DEPTH PREPASS writes alongside depth (it is the only full-scene pass
// that runs before lighting, so both products are ready for anything that wants them pre-shading):
//   * velocity — screen-space motion vectors in UV units, prevUV - currUV, matching D3D11's PS_Diffuse
//     CalculateVelocity convention exactly (see Shaders/D3D12/include/MotionVectors.hlsl). Consumers: TAA, FSR3.
//   * normal   — the shading normal, OCTAHEDRAL-encoded into two channels (world space, same encoding the packed
//     36-byte world vertex already uses at offset 12 — see World.hlsl's DecodeOctNormal). Consumer: XeGTAO.
// Both are RG16F: the octahedral pair lives in [-1,1] and velocities are fractions of a screen, so fp16 has
// ample range, and 32 bpp for the pair is half what an RGBA16F normal target would cost.
// TWO CHANNELS, and it stays that way: RG16F UV-space motion is the interchange format every temporal consumer
// expects (FSR3, XeGTAO's denoiser, DLSS-style APIs all take a 2-channel motion vector). Anything an individual
// algorithm wants beyond that — Intel's TAA, for instance, likes an expected depth delta alongside it — has to
// be derived in that algorithm's own pass rather than widened into this shared buffer.
inline constexpr DXGI_FORMAT kVelocityFormat = DXGI_FORMAT_R16G16_FLOAT;
inline constexpr DXGI_FORMAT kGBufferNormalFormat = DXGI_FORMAT_R16G16_FLOAT;

// Sentinel the velocity target is cleared to each frame. FillCameraVelocity (end of world rendering, when depth
// is final) replaces every pixel STILL holding it with a camera-only depth reprojection — that is what covers
// the sky, water, decals and the transparents, none of which are in the depth prepass. Real velocities
// are fractions of a screen (|v| <= ~2), so this can never collide with one, and it survives fp16 exactly.
inline constexpr float kVelocitySentinel = -1.0e4f;

// ...and the sentinel the NORMAL target is cleared to. Nothing ever fills these in (a pixel with no prepass
// coverage has no normal), so XeGTAO's LoadNormal has to be able to RECOGNISE them: -2 is outside the [-1,1]
// an octahedral pair can encode, whereas the obvious 0 clear is the perfectly legal encoding of world (0,0,1)
// and would silently misreport every surface facing exactly +Z. Must match kOctNormalSentinel in
// Shaders/D3D12/include/MotionVectors.hlsl and the ClearRenderTargetView call in BeginMotionGBuffer (which in
// turn must match the optimized clear value CreateMotionResources passes, or fast-clear is lost).
inline constexpr float kGBufferNormalSentinel = -2.0f;

// --- CPU breadcrumb / debug-marker ring (DRED forensics + PIX events) ---
// Why is BeginEvent not working as intended with Context on debugging this 32 bit app !!
// A global ring-buffer tracking recent recording phases mapped directly to command list slots.
struct CPUBreadcrumbContext {
    UINT opIndex = 0;
    const wchar_t* pContextText = nullptr;
};

// Allocate space for tracking up to 2048 sequential draw states per frame execution.
inline thread_local std::array<CPUBreadcrumbContext, 2048> g_CpuContextHistory;
inline thread_local UINT g_CurrentRecordingOpIndex = 0;

struct DXMarker {
    DXMarker( const Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& commandList, const wchar_t* text ) :
        DXMarker( commandList.Get(), text )
    {
    }

    // Raw-pointer overload: the MT shadow-cascade recorder (PrepareSunShadows / RecordShadowCascade) is handed a
    // bare ID3D12GraphicsCommandList* so the same body can record into m_CmdList or into a per-cascade list.
    // The breadcrumb ring this writes is thread_local, so concurrent recorders don't collide.
    DXMarker( ID3D12GraphicsCommandList* commandList, const wchar_t* text ) :
        c( commandList )
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

// Simple whole-resource transition barrier (legacy barriers; no enhanced-barrier path on inbox D3D12).
inline D3D12_RESOURCE_BARRIER TransitionBarrier( ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after ) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}
