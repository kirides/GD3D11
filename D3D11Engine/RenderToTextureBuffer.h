#pragma once
#include "pch.h"

/** Helper structs for quickly creating render-to-texture buffers */
// DXGI_FORMAT_R8G8B8A8_UNORM
// 
const DXGI_FORMAT DXGI_FORMAT_ENGINE_SWAPCHAIN = DXGI_FORMAT_B8G8R8A8_UNORM;
const DXGI_FORMAT DXGI_FORMAT_ENGINE_DEFAULT = DXGI_FORMAT_R8G8B8A8_UNORM;

/** Struct for a texture that can be used as shader resource AND rendertarget */
struct RenderToTextureBuffer {
    ~RenderToTextureBuffer() {
    }

    /** Creates the render-to-texture buffers */
    RenderToTextureBuffer( ID3D11Device* device,
        UINT SizeX, 
        UINT SizeY,
        DXGI_FORMAT Format, 
        HRESULT* Result = nullptr, 
        DXGI_FORMAT RTVFormat = DXGI_FORMAT_UNKNOWN, 
        DXGI_FORMAT SRVFormat = DXGI_FORMAT_UNKNOWN, 
        int MipLevels = 1,
        UINT arraySize = 1,
        uint32_t bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
        UINT sampleCount = 1,
        UINT sampleQuality = 0);

    /** Binds the texture to the pixel shader */
    void BindToPixelShader( ID3D11DeviceContext* context, int slot ) {
        context->PSSetShaderResources( slot, 1, ShaderResView.GetAddressOf() );
    };

    const Microsoft::WRL::ComPtr<ID3D11Texture2D>& GetTexture() { return Texture; }
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& GetShaderResView() { return ShaderResView; }
    const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& GetRenderTargetView() { return RenderTargetView; }
    const Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& GetUnorderedAccessView() { return UnorderedAccessView; }

    //void SetTexture( ID3D11Texture2D* tx ) { Texture = tx; }
    //void SetShaderResView( Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv ) { ShaderResView = srv.Get(); }
    //void SetRenderTargetView( Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv ) { RenderTargetView = rtv.Get(); }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& GetRTVCubemapFace( UINT i ) { return CubeMapRTVs[i]; }

    UINT GetSizeX() { return SizeX; }
    UINT GetSizeY() { return SizeY; }
    UINT GetSampleCount() { return SampleCount; }
private:

    /** The Texture object */
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture;

    /** Shader and rendertarget resource views */
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ShaderResView;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> RenderTargetView;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> UnorderedAccessView;

    // Rendertargets for the cubemap-faces, if this is a cubemap
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> CubeMapRTVs[6];

    UINT SizeX;
    UINT SizeY;
    UINT SampleCount = 1;

    void ReleaseAll() {
        Texture.Reset();
        ShaderResView.Reset();
        UnorderedAccessView.Reset();
        RenderTargetView.Reset();
    }
};

/** Struct for a texture that can be used as shader resource AND depth stencil target */
struct RenderToDepthStencilBuffer {
    ~RenderToDepthStencilBuffer() {
    }

    /** Wraps pre-existing views into a shared TextureCubeArray. faceDSVs = 6 single-slice DSVs over the
        same range as dsv; arrayDSV = the whole array as one FirstArraySlice=0 view. */
    RenderToDepthStencilBuffer(
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
        UINT SizeX, UINT SizeY,
        const Microsoft::WRL::ComPtr<ID3D11DepthStencilView>* faceDSVs = nullptr,
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> arrayDSV = nullptr,
        UINT baseArraySlice = 0 )
        : Texture( std::move( texture ) ), SizeX( SizeX ), SizeY( SizeY ),
          BaseArraySlice( baseArraySlice ), ShaderResView( std::move( srv ) ),
          DepthStencilView( std::move( dsv ) ), ArrayDepthStencilView( std::move( arrayDSV ) ) {
        if ( faceDSVs ) {
            for ( int i = 0; i < 6; ++i ) CubeMapDSVs[i] = faceDSVs[i];
        }
    }

    /** Creates the render-to-texture buffers */
    RenderToDepthStencilBuffer( ID3D11Device* device, UINT SizeX, UINT SizeY, DXGI_FORMAT Format, HRESULT* Result = nullptr, DXGI_FORMAT DSVFormat = DXGI_FORMAT_UNKNOWN, DXGI_FORMAT SRVFormat = DXGI_FORMAT_UNKNOWN, UINT arraySize = 1, UINT sampleCount = 1, UINT sampleQuality = 0 );

    void BindToVertexShader( const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context, int slot ) {
        context->VSSetShaderResources( slot, 1, ShaderResView.GetAddressOf() );
    }

    void BindToPixelShader( const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context, int slot ) {
        context->PSSetShaderResources( slot, 1, ShaderResView.GetAddressOf() );
    }

    const Microsoft::WRL::ComPtr<ID3D11Texture2D>& GetTexture() const { return Texture; }
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& GetShaderResView() const { return ShaderResView; }
    const Microsoft::WRL::ComPtr<ID3D11DepthStencilView>& GetDepthStencilView() const { return DepthStencilView; }
    /** Null when the device couldn't provide one; callers must fall back to a depth copy then. */
    const Microsoft::WRL::ComPtr<ID3D11DepthStencilView>& GetDepthStencilViewReadOnly() const { return DepthStencilViewReadOnly; }
    /** The whole shared array as one view, or null for a buffer that owns its texture. */
    const Microsoft::WRL::ComPtr<ID3D11DepthStencilView>& GetArrayDepthStencilView() const { return ArrayDepthStencilView; }
    /** Slice of face 0 inside the shared array; 0 when this buffer owns its texture. */
    UINT GetBaseArraySlice() const { return BaseArraySlice; }
    UINT GetSizeX() const { return SizeX; }
    UINT GetSizeY() const { return SizeY; }
    UINT GetSampleCount() const { return SampleCount; }

    const Microsoft::WRL::ComPtr<ID3D11DepthStencilView>& GetDSVCubemapFace( UINT i ) { return CubeMapDSVs[i]; }

    //void SetTexture( Microsoft::WRL::ComPtr<ID3D11Texture2D> tx ) { Texture = tx.Get(); }
    //void SetShaderResView( Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv ) { ShaderResView = srv.Get(); }
    //void SetDepthStencilView( Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv ) { DepthStencilView = dsv.Get(); }

private:

    // The Texture object
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture;

    UINT SizeX;
    UINT SizeY;
    UINT SampleCount = 1;
    UINT BaseArraySlice = 0;

    // Shader and rendertarget resource views
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ShaderResView;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> DepthStencilView;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> DepthStencilViewReadOnly;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> ArrayDepthStencilView;

    // Rendertargets for the cubemap-faces, if this is a cubemap
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> CubeMapDSVs[6];
};
