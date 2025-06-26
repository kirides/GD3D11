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

    XRESULT LoadRainResources();

    /** Renders the rain-shadowmap */
    XRESULT DrawRainShadowmap();

    /** Returns the current rain-shadowmap camera replacement */
    CameraReplacement& GetRainShadowmapCameraRepl() { return RainShadowmapCameraRepl; }

    /** Returns the rain shadowmap */
    RenderToDepthStencilBuffer* GetRainShadowmap() { return RainShadowmap.get(); }
protected:

    /** Fills a vector of random raindrop data */
    void FillRandomRaindropData( std::vector<RainParticleInstanceInfo>& data );

    /** Rain */
    D3D11VertexBuffer* RainBufferInitial;
    D3D11VertexBuffer* RainBufferDrawFrom;
    D3D11VertexBuffer* RainBufferStreamTo;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> RainTextureArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> RainTextureArraySRV;
    std::unique_ptr<RenderToDepthStencilBuffer> RainShadowmap;
    CameraReplacement RainShadowmapCameraRepl;

};

