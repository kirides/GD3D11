#pragma once
#include "pch.h"
#include "D3D11PFX_Effect.h"

struct RenderToTextureBuffer;

// Constant buffer for the Intel Graphics Optimized TAA resolve (CS_PFX_TAAResolve.hlsl). Mirrors the D3D12
// backend's TaaConstants (D3D12Taa.cpp) minus the bindless *Index fields, which D3D11 doesn't need since every
// texture is bound to a fixed register instead.
#pragma pack (push, 1)
struct TAAResolveConstantBuffer {
    XMFLOAT4X4 InvUnjitteredViewProj;
    XMFLOAT4X4 PrevViewProj;
    XMFLOAT4 Resolution;        // width, height, 1/width, 1/height
    XMFLOAT4 JitterTolerance;   // jitter.xy (pixels), DepthTolerance, unused
    XMFLOAT4 PrevJitter;        // previous frame's jitter.xy (pixels), .zw unused
    uint32_t FrameNumber;
    uint32_t DebugFlags;
    uint32_t HistoryValid;
    uint32_t Padding;
};

// Velocity buffer constant buffer (unchanged: still used by the camera-reconstructed fallback path in
// RenderVelocityBuffer, which is only exercised when DebugSettings.TAA.DepthMotionVectors forces it).
struct VelocityBufferConstantBuffer {
    XMFLOAT4X4 InvViewProj;      // Current frame's unjittered inverse view-projection
    XMFLOAT4X4 PrevViewProj;     // Previous frame's unjittered view-projection
    XMFLOAT2 JitterOffset;       // Current jitter in UV space
    XMFLOAT2 PrevJitterOffset;   // Previous jitter in UV space
    XMFLOAT2 Resolution;
    XMFLOAT2 Padding;
};
#pragma pack (pop)

class D3D11PFX_TAA : public D3D11PFX_Effect {
public:
    D3D11PFX_TAA(D3D11PfxRenderer* rnd);
    ~D3D11PFX_TAA() override = default;

    /** Initialize TAA resources */
    bool Init();

    /** Called on resize */
    void OnResize(const INT2& size);

    /** Renders the TAA effect (Intel Graphics Optimized TAA resolve, compute-dispatched) */
    void RenderPostFX(
        RenderToTextureBuffer& renderTarget,
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& depthSRV,
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& velocitySRV);

    /** Gets current jitter offset for camera, scaled by resolution */
    XMFLOAT2 GetJitterOffset() const { return m_CurrentJitter; }

    /** jitter in -0.5 to 0.5 range */
    XMFLOAT2 GetJitterOffsetUnscaled() const { return m_CurrentJitterUnscaled; }

    /** Advances to next jitter sample */
    void AdvanceJitter();

    /** Generates the velocity buffer from depth (fallback path, see DepthMotionVectors) */
    void RenderVelocityBuffer(
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& depthSRV);

    /** Gets the velocity buffer SRV */
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> GetVelocityBufferSRV() const;

    /** Draws this effect to the given buffer */
    XRESULT Render(RenderToTextureBuffer* fxbuffer) override { return XR_SUCCESS; }

    void OnDisabled();

    /** Gets the current frame's unjittered view-projection matrix */
    const XMFLOAT4X4& GetUnjitteredViewProj() const { return m_UnjitteredViewProj; }

    void ReleaseResources();

private:
    // History ping-pong (previous frame's AA'd result, .a = accumulated confidence weight in [0.5, 1)). Two
    // buffers because the resolve reads last frame's history while writing this frame's, and both must exist
    // simultaneously (no in-place UAV read-modify-write of a texture being sampled elsewhere).
    std::unique_ptr<RenderToTextureBuffer> m_HistoryBuffer[2];
    UINT m_HistoryIndex = 0;
    bool m_HistoryValid = false;

    // Private previous-depth snapshot. Cannot borrow the engine's DepthStencilBufferCopy: that buffer is
    // refilled with THIS frame's depth later in the same frame (for upscaling), so by the time next frame's
    // TAA runs it would no longer hold what TAA needs. Snapshotted at the end of RenderPostFX instead.
    std::unique_ptr<RenderToTextureBuffer> m_PrevDepthBuffer;
    bool m_PrevDepthValid = false;

    // Velocity buffer (screen-space motion vectors) — only used by the camera-reconstructed fallback path.
    std::unique_ptr<RenderToTextureBuffer> m_VelocityBuffer;

    Microsoft::WRL::ComPtr<ID3D11SamplerState>       m_samplerLinear;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>       m_samplerPoint;

    int m_JitterIndex;
    XMFLOAT2 m_CurrentJitter;
    XMFLOAT2 m_CurrentJitterUnscaled;
    XMFLOAT2 m_PreviousJitter;
    // Previous frame's jitter in PIXELS. m_PreviousJitter is the UV-scaled one the velocity CB wants; the
    // resolve's disocclusion lookup works in pixels, so it needs its own snapshot.
    XMFLOAT2 m_PreviousJitterUnscaled;
    uint32_t m_FrameNumber = 0;

    // Previous frame matrices for reprojection
    XMFLOAT4X4 m_PrevViewProj;
    XMFLOAT4X4 m_UnjitteredViewProj;

    // Previous camera position for motion vector calculation
    XMFLOAT3 m_PrevCameraPosition;

    int m_Width;
    int m_Height;
    bool m_FirstFrame;
    bool m_recreate = true;
};
