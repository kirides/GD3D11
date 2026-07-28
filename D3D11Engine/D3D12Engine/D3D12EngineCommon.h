#pragma once
// Shared internal helpers for the D3D12 backend translation units (D3D12GraphicsEngine.cpp and its
// split siblings D3D12Engine2D.cpp / D3D12PostFX.cpp / D3D12Scene.cpp). These were file-local statics
// in the original monolith; they are promoted here so the split TUs share ONE definition each.
// This header is D3D12-backend-private — do not include it outside D3D12Engine/.
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>
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

// matSrvIndex indexes g_SkelMatSrvs (below): the per-material diffuse descriptor handles for this vob's
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
// srv = the same main-thread-resolved diffuse handle as FrameSkelDraw::matSrvIndex, for the same reason —
// the main-view prepass/color paths still resolve `tex` themselves (they CacheIn, which the shadow paths
// deliberately don't), so both fields stay live.
struct FrameAttachDraw {
    MeshInfo*                   mesh;
    zCTexture*                  tex;
    D3D12_VERTEX_BUFFER_VIEW    instView;
    const zCVob*                owner;
    D3D12_GPU_DESCRIPTOR_HANDLE srv;
};

// Per-frame GPU point light — byte-identical to D3D11 TiledPointLight (48 B). Filled by
// BuildFrameLightBuffer (D3D12Scene.cpp); the point-shadow slot selection
// (D3D12PointShadows::SelectShadowedLights) reads Range/PositionWorld/Color.w and writes ShadowCubeIndex.
struct GPULight {
    DirectX::XMFLOAT3 PositionView;    // 0
    float             Range;           // 12
    DirectX::XMFLOAT4 Color;           // 16 (.w = static flag 0/1)
    DirectX::XMFLOAT3 PositionWorld;   // 32
    int32_t           ShadowCubeIndex; // 44 (-1 = no shadow)
};
static_assert( sizeof( GPULight ) == 48, "GPULight must match D3D11 TiledPointLight (48 bytes)" );

// This frame's visible-VOB instance-ring snapshot (UploadFrameVobInstances) — the depth prepass, the color
// pass AND the point-shadow static-VOB gather all draw from it. Defined in D3D12Scene.cpp.
extern std::vector<FrameVobUpload> g_FrameVobUploads;

// Per-vob snapshot of the diffuse descriptor handle for each entry of visual->SkeletalMeshes, in that map's
// iteration order — see FrameSkelDraw::matSrvIndex. Only the live prefix [0, g_SkelMatSrvCount) is valid each
// frame. Defined in D3D12Scene.cpp (PrepareFrameSkeletals owns it); read by the CSM cascade recorder.
extern std::vector<std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>> g_SkelMatSrvs;
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
