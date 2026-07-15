#pragma once
#include <wrl/client.h>
#include "GfxTexture.h"

class D3D11Texture : public GfxTexture {
public:
    D3D11Texture();
    ~D3D11Texture() override;

    /** Initializes the texture object */
    XRESULT Init( INT2 size, ETextureFormat format, UINT mipMapCount = 1, void* data = nullptr, const std::string& fileName = "" ) override;

    /** Initializes the texture from a file */
    XRESULT Init( const std::string& file ) override;

    XRESULT Init( const uint8_t* data, size_t size, const std::string& debugFileName ) override;

    /** Updates the Texture-Object */
    XRESULT UpdateData( void* data, int mip = 0 ) override;

    /** Updates the Texture-Object using the deferred context (For loading in an other thread) */
    XRESULT UpdateDataDeferred( void* data, int mip ) override;

    /** Returns the RowPitch-Bytes */
    UINT GetRowPitchBytes( int mip ) override;

    /** Returns the size of the texture in bytes */
    UINT GetSizeInBytes( int mip ) override;

    /** Returns if texture is 16bit type */
    bool Is16BitTexture() override;

    /** Binds this texture to a pixelshader */
    XRESULT BindToPixelShader( int slot ) override;

    /** Binds this texture to a pixelshader */
    XRESULT BindToVertexShader( int slot ) override;

    /** Binds this texture to a domainshader */
    XRESULT BindToDomainShader( int slot ) override;

    /** Returns the texture-object */
    const Microsoft::WRL::ComPtr<ID3D11Texture2D>& GetTextureObject() { return Texture; }

    /** Returns the shader resource view */
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& GetShaderResourceView() { return ShaderResourceView; }

    /** Creates a thumbnail for this */
    XRESULT CreateThumbnail() override;

    /** Returns the thumbnail of this texture. If this returns nullptr, you need to create one first */
    const Microsoft::WRL::ComPtr<ID3D11Texture2D>& GetThumbnail();
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& GetThumbnailSRV() { return ThumbnailSRV;}

    /** Generates mipmaps for this texture (may be slow!) */
    XRESULT GenerateMipMaps() override;
    XRESULT GenerateMipMapsDeferred() override;

    /** Returns this textures ID */
    UINT16 GetID() override { return ID; };

    /** Backend downcast from the neutral base. Safe by construction: the only concrete
        GfxTexture implementation is D3D11Texture while the D3D11 backend is active. */
    static D3D11Texture* From( GfxTexture* texture ) { return static_cast<D3D11Texture*>( texture ); }

private:
    /** The ID of this texture */
    UINT16 ID;

    /** D3D11 objects */
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ShaderResourceView;
    DXGI_FORMAT TextureFormat;
    INT2 TextureSize;
    int MipMapCount;

    /** Thumbnail */
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Thumbnail;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ThumbnailSRV;
};

