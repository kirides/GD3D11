#pragma once
#include "pch.h"
#include "D3D11PFX_Effect.h"
#include "D3D11ConstantBuffer.h"

struct RenderToTextureBuffer;

// TAA Constant buffer structure
#pragma pack (push, 1)
struct TAAConstantBuffer {
    XMFLOAT4X4 InvViewProj;
    XMFLOAT4X4 PrevViewProj;
    XMFLOAT2 JitterOffset;
    XMFLOAT2 Resolution;
    float BlendFactor;      // 0.0 = all history, 1.0 = all current
    float MotionScale;
    XMFLOAT2 Padding;
};

// Velocity buffer constant buffer
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

    /** Renders the TAA effect */
    void RenderPostFX(
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& currentFrameSRV,
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& depthSRV,
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& velocitySRV);

    /** Gets current jitter offset for camera, scaled by resolution */
    XMFLOAT2 GetJitterOffset() const { return m_CurrentJitter; }

    /** jitter in -0.5 to 0.5 range */
    XMFLOAT2 GetJitterOffsetUnscaled() const { return m_CurrentJitterUnscaled; }
    
    /** Advances to next jitter sample */
    void AdvanceJitter();

    /** Generates the velocity buffer from depth */
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
    // History buffer (previous frame's AA'd result)
    std::unique_ptr<RenderToTextureBuffer> m_HistoryBuffer;
    
    // Velocity buffer (screen-space motion vectors)
    std::unique_ptr<RenderToTextureBuffer> m_VelocityBuffer;
    
    // TAA constant buffer
    std::unique_ptr<D3D11ConstantBuffer> m_TAAConstantBuffer;
    
    // Velocity buffer constant buffer
    std::unique_ptr<D3D11ConstantBuffer> m_VelocityConstantBuffer;
    
    Microsoft::WRL::ComPtr<ID3D11SamplerState>       m_samplerLinear;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>       m_samplerPoint;

    int m_JitterIndex;
    XMFLOAT2 m_CurrentJitter;
    XMFLOAT2 m_CurrentJitterUnscaled;
    XMFLOAT2 m_PreviousJitter;
    
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
