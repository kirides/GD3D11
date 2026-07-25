#pragma once
// Shared internal helpers for the D3D12 backend translation units (D3D12GraphicsEngine.cpp and its
// split siblings D3D12Engine2D.cpp / D3D12PostFX.cpp / D3D12Scene.cpp). These were file-local statics
// in the original monolith; they are promoted here so the split TUs share ONE definition each.
// This header is D3D12-backend-private — do not include it outside D3D12Engine/.
#include <array>
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

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
