#pragma once
#include "pch.h"
#include "ShaderIDs.h"
#include "TexturePool.h"

class D3D11PFX_FSR3;
class D3D11PFX_FSR2;
class D3D11PFX_FSR1;
class D3D11PFX_CAS;
class D3D11PFX_TAA;
struct RenderToTextureBuffer;
class D3D11PFX_Blur;
class D3D11PFX_HeightFog;
class D3D11PFX_DistanceBlur;
class D3D11PFX_HDR;
class D3D11PFX_SMAA;
class D3D11PFX_GodRays;
class D3D11PFX_DepthOfField;
class D3D11NVHBAO;
class D3D11PFX_SAO;
class D3D11PFX_SimpleSharpen;
class D3D11PFX_ASSAO;

class D3D11PfxRenderer {
public:
    D3D11PfxRenderer();
    ~D3D11PfxRenderer();

    /** Called on resize */
    XRESULT OnResize( const INT2& newResolution );

    /** Blurs the given texture */
    XRESULT BlurTexture( RenderToTextureBuffer* texture, bool leaveResultInD4_2 = false, float scale = 1.0f, const XMFLOAT4& colorMod = XMFLOAT4( 1, 1, 1, 1 ), PShaderID finalCopyShader = PShaderID::PS_PFX_Simple );

    /** Renders the heightfog */
    XRESULT RenderHeightfog();

    /** Renders the distance blur effect */
    XRESULT RenderDistanceBlur(ID3D11ShaderResourceView* diffuse );

    /** Renders the HDR-Effect */
    XRESULT RenderHDR(ID3D11RenderTargetView* output, ID3D11ShaderResourceView* backbuffer);

    /** Renders the SMAA-Effect */
    XRESULT RenderSMAA(ID3D11ShaderResourceView* backbuffer);

    XRESULT RenderTAA(const ComPtr<ID3D11ShaderResourceView>& velocityBuffer);
    XRESULT RenderCAS( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input, INT2 inputSize, const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& output, INT2 outputSize, RenderToTextureBuffer& intermediateBuffer );
    XRESULT RenderSimpleSharpen( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& source, INT2 sourceSize, RenderToTextureBuffer* dest, INT2 destSize );

    /** Renders the godrays-Effect */
    XRESULT RenderGodRays(ID3D11ShaderResourceView* backbuffer, ID3D11ShaderResourceView* depth);

    /** Renders the depth-of-field effect */
    XRESULT RenderDepthOfField(ID3D11ShaderResourceView* backbuffer);

    /** Copies the given texture to the given RTV */
    XRESULT CopyTextureToRTV( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& texture, const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv, INT2 targetResolution = INT2( 0, 0 ), bool useCustomPS = false, INT2 offset = INT2( 0, 0 ) );

    /** Unbinds texturesamplers from the pixel-shader */
    XRESULT UnbindPSResources( int num );

    /** Draws a fullscreenquad */
    XRESULT DrawFullScreenQuad();

    /** Draws the HBAO-Effect to the given buffer */
    XRESULT DrawHBAO(const ComPtr<ID3D11RenderTargetView>& rtv, const ComPtr<ID3D11ShaderResourceView>& pFullResDepthTexSRV, const ComPtr<
                     ID3D11ShaderResourceView>& pFullResNormalTexSRV);

    /** Renders the SAO effect */
    XRESULT RenderSAO( ID3D11ShaderResourceView* depthSRV,
                       ID3D11ShaderResourceView* normalsSRV,
                       ID3D11RenderTargetView* outputRTV );

    /** Computes SAO into internal buffer, skipping the final modulate blit */
    XRESULT RenderSAOCompute( ID3D11ShaderResourceView* depthSRV,
                              ID3D11ShaderResourceView* normalsSRV );

    /** Returns the SRV of the last computed SAO result (R8_UNORM) */
    ID3D11ShaderResourceView* GetSAOResultSRV() const;

    /** Renders godrays mask+zoom to a pool texture, skipping the final additive blit */
    XRESULT RenderGodRaysToTexture( ID3D11ShaderResourceView* backbuffer,
                                    ID3D11ShaderResourceView* normals,
                                    ID3D11ShaderResourceView** outGodRaysSRV );

    /** Renders the PostFX composition uber pass (SAO + HeightFog + GodRays) */
    XRESULT RenderPostFXComposition( ID3D11RenderTargetView* outputRTV,
                                     ID3D11ShaderResourceView* backbufferSRV,
                                     ID3D11ShaderResourceView* saoSRV,
                                     ID3D11ShaderResourceView* godraysSRV,
                                     ID3D11ShaderResourceView* depthSRV );

    XRESULT RenderASSAO( ID3D11RenderTargetView* outputRTV,
                            ID3D11ShaderResourceView* depthCopy,
                            ID3D11ShaderResourceView* normals );

    /** Accessors */
    TextureHandle GetTempBuffer();
    TextureHandle GetBackbufferTempBuffer();
    TextureHandle GetTempBufferDS4();

    D3D11PFX_TAA* GetTAAEffect() { return FX_TAA.get(); }
    D3D11PFX_CAS* GetCAS() { return PFX_CAS.get(); }
    D3D11PFX_FSR1* GetFSR1() { return PFX_FSR1.get(); }
    D3D11PFX_FSR2* GetFSR2() { return PFX_FSR2.get(); }
    D3D11PFX_FSR3* GetFSR3() { return PFX_FSR3.get(); }

    void OnEndFrame() {
        m_texturePool->GiveTick();
        m_depthStencilPool->GiveTick();
        FreeResources();
    }

    // Free any unused resources, like SMAA buffers if the player doesnt use it.
    void FreeResources();

    TexturePool* GetTexturePool() { return m_texturePool.get(); }
    DepthStencilPool* GetDepthStencilPool() { return m_depthStencilPool.get(); }
private:
    /** Blur effect referenced here because it's often needed by PFX */
    std::unique_ptr<D3D11PFX_Blur> FX_Blur;
    std::unique_ptr<D3D11PFX_HeightFog> FX_HeightFog;
    std::unique_ptr<D3D11PFX_DistanceBlur> FX_DistanceBlur;
    std::unique_ptr<D3D11PFX_HDR> FX_HDR;
    std::unique_ptr<D3D11PFX_SMAA> FX_SMAA;
    std::unique_ptr<D3D11PFX_GodRays> FX_GodRays;
    std::unique_ptr<D3D11PFX_DepthOfField> FX_DepthOfField;

    /** SAO effect (FL11+ only) */
    std::unique_ptr<D3D11PFX_SAO> FX_SAO;

    /** Nivida HBAO+ */
    std::unique_ptr<D3D11NVHBAO> NvHBAO;

    std::unique_ptr<D3D11PFX_TAA> FX_TAA;

    std::unique_ptr<D3D11PFX_CAS> PFX_CAS;
    std::unique_ptr<D3D11PFX_SimpleSharpen> PFX_SimpleSharpen;
    std::unique_ptr<D3D11PFX_FSR1> PFX_FSR1;
    std::unique_ptr<D3D11PFX_FSR2> PFX_FSR2;
    std::unique_ptr<D3D11PFX_FSR3> PFX_FSR3;
    std::unique_ptr<D3D11PFX_ASSAO> PFX_ASSAO;
    std::unique_ptr<TexturePool> m_texturePool;
    std::unique_ptr<DepthStencilPool> m_depthStencilPool;
};

