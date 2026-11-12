#pragma once
#include "pch.h"
#include "WorldConverter.h"
#include "GothicAPI.h"

/** Wrapper-class for some generic effects */

class D3D11VertexBuffer;
struct RenderToDepthStencilBuffer;
class D3D11Effect {
public:
    D3D11Effect();
    ~D3D11Effect();

    /** Draws GPU-Based rain */
    XRESULT DrawRain();
    XRESULT DrawRain_CS();

    /** Rain shadowmap + sampler; the drop/flake textures load lazily (EnsureRainTextures) */
    XRESULT LoadRainResources();

    /** Renders the rain-shadowmap */
    XRESULT DrawRainShadowmap();

    /** Returns the current rain-shadowmap camera replacement */
    CameraReplacement& GetRainShadowmapCameraRepl() { return RainShadowmapCameraRepl; }

    /** Returns the rain shadowmap */
    RenderToDepthStencilBuffer* GetRainShadowmap() { return RainShadowmap.get(); }
protected:

    /** Fills vectors of random raindrop data, split into mutable and immutable parts */
    void FillRandomRaindropData( std::vector<RainParticleDynamic>& dynamicData, std::vector<RainParticleStatic>& staticData );

    /** Kicks/polls the async load of the rain or snow array; true once its SRV is usable */
    bool EnsureRainTextures( bool snow );

    /** Rain */
    std::unique_ptr<GfxVertexBuffer> RainBufferStatic;
    std::unique_ptr<GfxVertexBuffer> RainBufferInitial;
    std::unique_ptr<GfxVertexBuffer> RainBufferDrawFrom;
    std::unique_ptr<GfxVertexBuffer> RainBufferStreamTo;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> RainTextureArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> RainTextureArraySRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> SnowTextureArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> SnowTextureArraySRV;
    std::shared_ptr<struct D3D11RainTextureJob> RainTextureJob, SnowTextureJob;
    std::unique_ptr<RenderToDepthStencilBuffer> RainShadowmap;
    CameraReplacement RainShadowmapCameraRepl;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_RainDropShadowSamplerState;
};

