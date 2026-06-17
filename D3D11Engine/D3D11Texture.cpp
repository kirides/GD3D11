#include "pch.h"
#include "D3D11Texture.h"
#include "Engine.h"
#include "D3D11GraphicsEngineBase.h"
#include "GothicAPI.h"
#include <DDSTextureLoader.h>
#include "RenderToTextureBuffer.h"
#include "D3D11_Helpers.h"
#include "TextureConversions.h"
#include "zFILE_VDFS.h"

extern bool NativeSupport16BitTextures;
static unsigned char* ConvertedData = nullptr;
static size_t ConvertedDataSize = 0;

unsigned char* ConvertTextureData( UINT TextureWidth, UINT TextureHeight, DXGI_FORMAT TextureFormat, unsigned char* data ) {
    UINT realDataSize = TextureWidth * TextureHeight * 4;
    if ( ConvertedDataSize < realDataSize ) {
        if (ConvertedData) {
            free( ConvertedData );
        }
        ConvertedData = reinterpret_cast<unsigned char*>( malloc( realDataSize ) );
        ConvertedDataSize = realDataSize;
    }
    if ( TextureFormat == DXGI_FORMAT_B5G6R5_UNORM ) {
        Convert565to8888( ConvertedData, data, realDataSize );
    } else if ( TextureFormat == DXGI_FORMAT_B5G5R5A1_UNORM ) {
        Convert1555to8888( ConvertedData, data, realDataSize );
    } else {
        Convert4444to8888( ConvertedData, data, realDataSize );
    }
    return ConvertedData;
}

D3D11Texture::D3D11Texture() {}

D3D11Texture::~D3D11Texture() {
    Thumbnail.Reset();
    Texture.Reset();
    ShaderResourceView.Reset();
}

/** Initializes the texture object */
XRESULT D3D11Texture::Init( INT2 size, ETextureFormat format, UINT mipMapCount, void* data, const std::string& fileName ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    TextureFormat = static_cast<DXGI_FORMAT>(format);
    TextureSize = size;
    MipMapCount = mipMapCount;
    if ( Is16BitTexture() && !NativeSupport16BitTextures ) {
        format = ETextureFormat::TF_B8G8R8A8;
    }

    CD3D11_TEXTURE2D_DESC textureDesc(
        static_cast<DXGI_FORMAT>(format),
        size.x,
        size.y,
        1,
        mipMapCount,
        D3D11_BIND_SHADER_RESOURCE, data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT, 0, 1, 0, 0 );

    D3D11_SUBRESOURCE_DATA initialData = {};
    initialData.pSysMem = data;
    if ( format == ETextureFormat::TF_B8G8R8A8 ) {
        initialData.SysMemPitch = size.x * 4;
    }

    LE( engine->GetDevice()->CreateTexture2D( &textureDesc, data ? &initialData : nullptr, Texture.ReleaseAndGetAddressOf() ) );
    SetDebugName( Texture.Get(), "D3D11Texture(\"" + fileName + "\")->Texture" );

    D3D11_SHADER_RESOURCE_VIEW_DESC descRV = {};
    descRV.Format = DXGI_FORMAT_UNKNOWN;
    descRV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    descRV.Texture2D.MipLevels = mipMapCount;
    descRV.Texture2D.MostDetailedMip = 0;
    LE( engine->GetDevice()->CreateShaderResourceView( Texture.Get(), &descRV, ShaderResourceView.ReleaseAndGetAddressOf() ) );
    SetDebugName( ShaderResourceView.Get(), "D3D11Texture(\"" + fileName + "\")->ShaderResourceView" );

    return XR_SUCCESS;
}

/** Initializes the texture from a file */
XRESULT D3D11Texture::Init( const std::string& file ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    //LogInfo() << "Loading Engine-Texture: " << file;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> res;
    if ( std::filesystem::path( file ).is_absolute() ) {
        LE( CreateDDSTextureFromFileEx(
            engine->GetDevice().Get(),
            Toolbox::ToWideChar( file.c_str() ).c_str(),
            0,  // maxsize (0 means no limit)
            D3D11_USAGE_IMMUTABLE,
            D3D11_BIND_SHADER_RESOURCE,
            0, // cpuAccessFlags (0 for immutable)
            0, // miscFlags
            DirectX::DDS_LOADER_DEFAULT,
            reinterpret_cast<ID3D11Resource**>(res.ReleaseAndGetAddressOf()),
            ShaderResourceView.GetAddressOf()) );
    } else {
        std::vector<uint8_t> fileData;
        
        zFILE_VDFS::Ptr vdfsFile;
        if ( !file.empty() && file[0] != '\\' ) {
            vdfsFile = zFILE_VDFS::Create( ("\\" + file).c_str());
        } else {
            vdfsFile = zFILE_VDFS::Create( file.c_str() );
        }
        if ( !vdfsFile || !vdfsFile->Exists() || vdfsFile->Open(false) != zERROR_NONE ) {
            LogError() << "Failed to load texture from VDFS: " << file;
            return XR_FAILED;
        }
        fileData.resize( vdfsFile->Size() );
        vdfsFile->Read( fileData.data(), fileData.size() );
        vdfsFile->Close();

        LE( CreateDDSTextureFromMemoryEx(
            engine->GetDevice().Get(),
            fileData.data(),
            fileData.size(),
            0,  // maxsize (0 means no limit)
            D3D11_USAGE_IMMUTABLE,
            D3D11_BIND_SHADER_RESOURCE,
            0, // cpuAccessFlags (0 for immutable)
            0, // miscFlags
            DirectX::DDS_LOADER_DEFAULT,
            reinterpret_cast<ID3D11Resource**>(res.ReleaseAndGetAddressOf()),
            ShaderResourceView.GetAddressOf() ) );
    }


    if ( !ShaderResourceView.Get() || !res.Get() )
        return XR_FAILED;

    D3D11_TEXTURE2D_DESC desc;
    res->GetDesc( &desc );

    Texture = res;
    TextureFormat = desc.Format;

    TextureSize.x = desc.Width;
    TextureSize.y = desc.Height;
    SetDebugName( res.Get(), "D3D11Texture(\"" + file + "\")->Texture" );
    SetDebugName( ShaderResourceView.Get(), "D3D11Texture(\"" + file + "\")->ShaderResourceView" );

    return XR_SUCCESS;
}

XRESULT D3D11Texture::Init( const uint8_t* data, size_t size, const std::string& debugFileName ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    //LogInfo() << "Loading Engine-Texture: " << debugFileName;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> res;
    LE( CreateDDSTextureFromMemory( engine->GetDevice().Get(), data, size,
        reinterpret_cast<ID3D11Resource**>(res.ReleaseAndGetAddressOf()), ShaderResourceView.GetAddressOf() ) );

    if ( !ShaderResourceView.Get() || !res.Get() )
        return XR_FAILED;

    D3D11_TEXTURE2D_DESC desc;
    res->GetDesc( &desc );

    Texture = res;
    TextureFormat = desc.Format;

    TextureSize.x = desc.Width;
    TextureSize.y = desc.Height;
    SetDebugName( res.Get(), "D3D11Texture(\"" + debugFileName + "\")->Texture" );
    SetDebugName( ShaderResourceView.Get(), "D3D11Texture(\"" + debugFileName + "\")->ShaderResourceView" );

    return XR_SUCCESS;
}

/** Updates the Texture-Object */
XRESULT D3D11Texture::UpdateData( void* data, int mip ) {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    UINT TextureWidth = (TextureSize.x >> mip);
    UINT TextureHeight = (TextureSize.y >> mip);

    void* srcData = data;
    UINT rowPitch = GetRowPitchBytes( mip );

    if ( Is16BitTexture() && !NativeSupport16BitTextures ) {
        srcData = ConvertTextureData( TextureWidth, TextureHeight, TextureFormat, reinterpret_cast<unsigned char*>( data ) );
        rowPitch = GetRowPitchBytes( mip ) * 2;
    }

    // UpdateSubresource directly into the DEFAULT texture — no staging texture needed
    engine->GetContext()->UpdateSubresource( Texture.Get(), mip, nullptr, srcData, rowPitch, 0 );
    return XR_SUCCESS;
}

/** Updates the Texture-Object using the deferred context (For loading in an other thread) */
XRESULT D3D11Texture::UpdateDataDeferred( void* data, int mip ) {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    UINT TextureWidth = (TextureSize.x >> mip);
    UINT TextureHeight = (TextureSize.y >> mip);

    ID3D11Texture2D* stagingTexture;
    D3D11_TEXTURE2D_DESC stagingTextureDesc;
    Texture.Get()->GetDesc( &stagingTextureDesc );
    stagingTextureDesc.Width = TextureWidth;
    stagingTextureDesc.Height = TextureHeight;
    stagingTextureDesc.MipLevels = 1;
    stagingTextureDesc.BindFlags = 0;
    stagingTextureDesc.MiscFlags = 0;
    stagingTextureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    stagingTextureDesc.Usage = D3D11_USAGE_STAGING;

    D3D11_SUBRESOURCE_DATA stagingTextureData;
    if ( Is16BitTexture() && !NativeSupport16BitTextures ) {
        stagingTextureData.pSysMem = ConvertTextureData( TextureWidth, TextureHeight, TextureFormat, reinterpret_cast<unsigned char*>(data) );
        stagingTextureData.SysMemPitch = GetRowPitchBytes( mip ) * 2;
    } else {
        stagingTextureData.pSysMem = data;
        stagingTextureData.SysMemPitch = GetRowPitchBytes( mip );
    }
    stagingTextureData.SysMemSlicePitch = 0;

    HRESULT result = engine->GetDevice()->CreateTexture2D( &stagingTextureDesc, &stagingTextureData, &stagingTexture );
    if ( FAILED( result ) )
        return XR_FAILED;
    SetDebugName( stagingTexture, "D3D11Texture->UpdateDataDeferred->stagingTexture" );

    Engine::GAPI->AddStagingTexture( mip, stagingTexture, Texture.Get() );
    return XR_SUCCESS;
}

/** Returns the RowPitch-Bytes */
UINT D3D11Texture::GetRowPitchBytes( int mip ) {
    int px = (TextureSize.x >> mip);
    //int py = (TextureSize.y >> mip);

    if ( TextureFormat == DXGI_FORMAT_R8_UNORM ) {
        return px;
    } else if ( TextureFormat == DXGI_FORMAT_B5G6R5_UNORM ||
        TextureFormat == DXGI_FORMAT_B5G5R5A1_UNORM ||
        TextureFormat == DXGI_FORMAT_B4G4R4A4_UNORM ) {
        return px * 2;
    } else if ( TextureFormat == DXGI_FORMAT_BC1_UNORM || TextureFormat == DXGI_FORMAT_BC2_UNORM ||
        TextureFormat == DXGI_FORMAT_BC3_UNORM ) {
        return Toolbox::GetDDSRowPitchSize( px, TextureFormat == DXGI_FORMAT_BC1_UNORM );
    } else { // Use B8G8R8A8
        return px * 4;
    }
}

/** Returns the size of the texture in bytes */
UINT D3D11Texture::GetSizeInBytes( int mip ) {
    int px = (TextureSize.x >> mip);
    int py = (TextureSize.y >> mip);

    if ( TextureFormat == DXGI_FORMAT_R8_UNORM ) {
        return px * py;
    } else if ( TextureFormat == DXGI_FORMAT_B5G6R5_UNORM ||
        TextureFormat == DXGI_FORMAT_B5G5R5A1_UNORM ||
        TextureFormat == DXGI_FORMAT_B4G4R4A4_UNORM ) {
        return px * py * 2;
    } else if ( TextureFormat == DXGI_FORMAT_BC1_UNORM || TextureFormat == DXGI_FORMAT_BC2_UNORM ||
        TextureFormat == DXGI_FORMAT_BC3_UNORM ) {
        return Toolbox::GetDDSStorageRequirements( px, py, TextureFormat == DXGI_FORMAT_BC1_UNORM );
    } else { // Use B8G8R8A8
        return px * py * 4;
    }
}

/** Returns if texture is 16bit type */
bool D3D11Texture::Is16BitTexture() {
    if ( TextureFormat == DXGI_FORMAT_B5G6R5_UNORM ||
        TextureFormat == DXGI_FORMAT_B5G5R5A1_UNORM ||
        TextureFormat == DXGI_FORMAT_B4G4R4A4_UNORM ) {
        return true;
    }
    return false;
}

/** Binds this texture to a pixelshader */
XRESULT D3D11Texture::BindToPixelShader( int slot ) {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->PSSetShaderResources( slot, 1, ShaderResourceView.GetAddressOf() );
    return XR_SUCCESS;
}

/** Binds this texture to a pixelshader */
XRESULT D3D11Texture::BindToVertexShader( int slot ) {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    engine->GetContext()->VSSetShaderResources( slot, 1, ShaderResourceView.GetAddressOf() );
    engine->GetContext()->DSSetShaderResources( slot, 1, ShaderResourceView.GetAddressOf() );

    return XR_SUCCESS;
}

/** Binds this texture to a domainshader */
XRESULT D3D11Texture::BindToDomainShader( int slot ) {
    reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine)->GetContext()->DSSetShaderResources( slot, 1, ShaderResourceView.GetAddressOf() );
    return XR_SUCCESS;
}

/** Creates a thumbnail for this */
XRESULT D3D11Texture::CreateThumbnail() {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    HRESULT hr;
    // Since D2D can't load DXTn-Textures on Windows 7, we copy it over to a smaller texture here in d3d11

    // Create texture
    CD3D11_TEXTURE2D_DESC textureDesc(
        DXGI_FORMAT_ENGINE_DEFAULT,
        256,
        256,
        1,
        1,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, 1, 0, 0 );

    ThumbnailSRV.Reset();
    LE( engine->GetDevice()->CreateTexture2D( &textureDesc, nullptr, Thumbnail.ReleaseAndGetAddressOf() ) );

    // Create temporary RTV
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> tempRTV;
    LE( engine->GetDevice()->CreateRenderTargetView( Thumbnail.Get(), nullptr, tempRTV.GetAddressOf() ) );
    if ( !tempRTV.Get() )
        return XR_FAILED;

    const float clearColor[4] = { 1.f, 0.f, 0.f, 1.f };
    engine->GetContext()->ClearRenderTargetView( tempRTV.Get(), clearColor );

    // Copy main texture to it
    engine->GetContext()->PSSetShaderResources( 0, 1, ShaderResourceView.GetAddressOf() );

    ID3D11RenderTargetView* oldRTV[2];
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;

    engine->GetContext()->OMGetRenderTargets( 2, oldRTV, oldDSV.GetAddressOf() );

    engine->GetContext()->OMSetRenderTargets( 1, tempRTV.GetAddressOf(), nullptr );
    engine->DrawQuad( INT2( 0, 0 ), INT2( 256, 256 ) );

    engine->GetContext()->OMSetRenderTargets( 2, oldRTV, oldDSV.Get() );
    
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = textureDesc.Format; // Anpassen an dein Texturformat
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    
    engine->GetDevice()->CreateShaderResourceView(Thumbnail.Get(), &srvDesc, ThumbnailSRV.GetAddressOf());

    return XR_SUCCESS;
}

/** Returns the thumbnail of this texture. If this returns nullptr, you need to create one first */
const Microsoft::WRL::ComPtr<ID3D11Texture2D>& D3D11Texture::GetThumbnail() {
    return Thumbnail;
}

/** Generates mipmaps for this texture (may be slow!) */
XRESULT D3D11Texture::GenerateMipMaps() {
    if ( MipMapCount == 1 )
        return XR_SUCCESS;

    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    std::unique_ptr<RenderToTextureBuffer> b = std::make_unique<RenderToTextureBuffer>( engine->GetDevice().Get(), TextureSize.x, TextureSize.y,
        DXGI_FORMAT_ENGINE_DEFAULT, nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, MipMapCount );

    // Copy the main image
    engine->GetContext()->CopySubresourceRegion( b->GetTexture().Get(), 0, 0, 0, 0, Texture.Get(), 0, nullptr );

    // Generate mips
    engine->GetContext()->GenerateMips( b->GetShaderResView().Get() );

    // Copy the full chain back
    engine->GetContext()->CopyResource( Texture.Get(), b->GetTexture().Get() );

    return XR_SUCCESS;
}

XRESULT D3D11Texture::GenerateMipMapsDeferred() {
    if ( MipMapCount == 1 )
        return XR_SUCCESS;

    Engine::GAPI->AddMipMapGeneration( this );

    return XR_SUCCESS;
}
