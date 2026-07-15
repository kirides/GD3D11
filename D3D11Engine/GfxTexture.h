#pragma once
#include <cstdint>
#include <string>
#include <dxgiformat.h>

#include "Types.h"

enum XRESULT : int;

/** Backend-neutral 2D texture.
    Both D3D11Texture and the future D3D12Texture derive from this. Scene code holds GfxTexture
    pointers and only touches this neutral API; the native texture / view objects stay on the
    concrete backend classes.

    ETextureFormat intentionally uses DXGI_FORMAT values: DXGI is the shared format currency for
    both the D3D11 and D3D12 backends (see the shared DXGIHelper in the plan), so no translation
    is needed and existing DXGI round-trips keep working. */
class GfxTexture {
public:
    virtual ~GfxTexture() = default;

    enum ETextureFormat {
        TF_R8       = DXGI_FORMAT_R8_UNORM,
        TF_B8G8R8A8 = DXGI_FORMAT_B8G8R8A8_UNORM,
        TF_R8G8B8A8 = DXGI_FORMAT_R8G8B8A8_UNORM,
        TF_B5G6R5   = DXGI_FORMAT_B5G6R5_UNORM,
        TF_B5G5R5A1 = DXGI_FORMAT_B5G5R5A1_UNORM,
        TF_B4G4R4A4 = DXGI_FORMAT_B4G4R4A4_UNORM,
        TF_DXT1     = DXGI_FORMAT_BC1_UNORM,
        TF_DXT3     = DXGI_FORMAT_BC2_UNORM,
        TF_DXT5     = DXGI_FORMAT_BC3_UNORM
    };

    /** Initializes the texture object */
    virtual XRESULT Init( INT2 size, ETextureFormat format, unsigned int mipMapCount = 1, void* data = nullptr, const std::string& fileName = "" ) = 0;

    /** Initializes the texture from a file / from an in-memory encoded image */
    virtual XRESULT Init( const std::string& file ) = 0;
    virtual XRESULT Init( const uint8_t* data, size_t size, const std::string& debugFileName ) = 0;

    /** Updates the texture (immediate / deferred context) */
    virtual XRESULT UpdateData( void* data, int mip = 0 ) = 0;
    virtual XRESULT UpdateDataDeferred( void* data, int mip ) = 0;

    /** Returns the row-pitch / total size in bytes for a mip level */
    virtual unsigned int GetRowPitchBytes( int mip ) = 0;
    virtual unsigned int GetSizeInBytes( int mip ) = 0;

    /** Returns if texture is a 16bit type */
    virtual bool Is16BitTexture() = 0;

    /** Binds this texture to the given shader stage slot */
    virtual XRESULT BindToPixelShader( int slot ) = 0;
    virtual XRESULT BindToVertexShader( int slot ) = 0;
    virtual XRESULT BindToDomainShader( int slot ) = 0;

    /** Creates a thumbnail for this texture */
    virtual XRESULT CreateThumbnail() = 0;

    /** Generates mipmaps for this texture (may be slow!) */
    virtual XRESULT GenerateMipMaps() = 0;
    virtual XRESULT GenerateMipMapsDeferred() = 0;

    /** Returns this texture's ID */
    virtual uint16_t GetID() = 0;
};
