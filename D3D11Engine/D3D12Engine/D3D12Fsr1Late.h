#pragma once

// FSR 1 in AMD's order: on the tonemapped, anti-aliased LDR image, before the UI (D3D12 backend, SDR output).
// The frame goes tonemap (render res) -> SMAA (render res) -> EASU + RCAS -> display chain. See RecordFsr1Late.
// Included only by the .cpp files that create or record it, so D3D12GraphicsEngine.h stays light.
struct Fsr1LateTargets {
    // Render-res, DisplayFormat: [0] the tonemapped scene, [1] SMAA's output. Rest in RENDER_TARGET.
    Microsoft::WRL::ComPtr<Rhi::Resource> In[2];
    UINT                                  InSrvSlot[2] = { UINT_MAX, UINT_MAX };
    D3D12_CPU_DESCRIPTOR_HANDLE           InRtv[2] = {};
    // Display-res DisplayFormat UAV that FFX writes; legacy-tracked because FFX barriers it with legacy barriers.
    // Rests in UNORDERED_ACCESS.
    Microsoft::WRL::ComPtr<Rhi::Resource> Out;
    bool                                  NeedsDiscard = true;   // created un-zeroed: discard once before the first draw
    INT2                                  RenderSize = {};
    INT2                                  DisplaySize = {};
};
