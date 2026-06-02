#pragma once
#include <wrl/client.h>

class D3D11Texture {
public:
    D3D11Texture();
    ~D3D11Texture();

    /** Layec out for DXGI */
    enum ETextureFormat {
        TF_R8 = DXGI_FORMAT_R8_UNORM,
        TF_B8G8R8A8 = DXGI_FORMAT_B8G8R8A8_UNORM,
        TF_B5G6R5 = DXGI_FORMAT_B5G6R5_UNORM,
        TF_B5G5R5A1 = DXGI_FORMAT_B5G5R5A1_UNORM,
        TF_B4G4R4A4 = DXGI_FORMAT_B4G4R4A4_UNORM,
        TF_DXT1 = DXGI_FORMAT_BC1_UNORM,
        TF_DXT3 = DXGI_FORMAT_BC2_UNORM,
        TF_DXT5 = DXGI_FORMAT_BC3_UNORM
    };

    /** Initializes the texture object */
    XRESULT Init( INT2 size, ETextureFormat format, UINT mipMapCount = 1, void* data = nullptr, const std::string& fileName = "" );

    /** Initializes the texture from a file */
    XRESULT Init( const std::string& file );

    XRESULT Init( const uint8_t* data, size_t size, const std::string& debugFileName );

    /** Updates the Texture-Object */
    XRESULT UpdateData( void* data, int mip = 0 );

    /** Updates the Texture-Object using the deferred context (For loading in an other thread) */
    XRESULT UpdateDataDeferred( void* data, int mip );

    /** Returns the RowPitch-Bytes */
    UINT GetRowPitchBytes( int mip );

    /** Returns the size of the texture in bytes */
    UINT GetSizeInBytes( int mip );

    /** Returns if texture is 16bit type */
    bool Is16BitTexture();

    /** Binds this texture to a pixelshader */
    XRESULT BindToPixelShader( int slot );

    /** Binds this texture to a pixelshader */
    XRESULT BindToVertexShader( int slot );

    /** Binds this texture to a domainshader */
    XRESULT BindToDomainShader( int slot );

    /** Returns the texture-object */
    const Microsoft::WRL::ComPtr<ID3D11Texture2D>& GetTextureObject() { return Texture; }

    /** Returns the shader resource view */
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& GetShaderResourceView() { return ShaderResourceView; }

    /** Creates a thumbnail for this */
    XRESULT CreateThumbnail();

    /** Returns the thumbnail of this texture. If this returns nullptr, you need to create one first */
    const Microsoft::WRL::ComPtr<ID3D11Texture2D>& GetThumbnail();
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& GetThumbnailSRV() { return ThumbnailSRV;}

    /** Generates mipmaps for this texture (may be slow!) */
    XRESULT GenerateMipMaps();
    XRESULT GenerateMipMapsDeferred();

    /** Returns this textures ID */
    UINT16 GetID() { return ID; };

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

