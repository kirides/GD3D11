#pragma once
#include <vector>
#include <wrl/client.h>
#include <d3d11_1.h>
#include <algorithm>
#include "Logger.h"
#include "zFILE_VDFS.h"

// Define MAKEFOURCC if not already defined by Windows/DirectX headers
#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
    ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) | \
    ((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24 ))
#endif

namespace {

    constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "

    // FourCC codes
    constexpr uint32_t FOURCC_DX10 = MAKEFOURCC( 'D', 'X', '1', '0' );
    constexpr uint32_t FOURCC_ATI1 = MAKEFOURCC( 'A', 'T', 'I', '1' );
    constexpr uint32_t FOURCC_BC4U = MAKEFOURCC( 'B', 'C', '4', 'U' );
    constexpr uint32_t FOURCC_BC4S = MAKEFOURCC( 'B', 'C', '4', 'S' );

    struct DDS_PIXELFORMAT {
        uint32_t size;
        uint32_t flags;
        uint32_t fourCC;
        uint32_t rgbBitCount;
        uint32_t rBitMask;
        uint32_t gBitMask;
        uint32_t bBitMask;
        uint32_t aBitMask;
    };

    struct DDS_HEADER {
        uint32_t size;
        uint32_t flags;
        uint32_t height;
        uint32_t width;
        uint32_t pitchOrLinearSize;
        uint32_t depth;
        uint32_t mipMapCount;
        uint32_t reserved1[11];
        DDS_PIXELFORMAT ddspf;
        uint32_t caps;
        uint32_t caps2;
        uint32_t caps3;
        uint32_t caps4;
        uint32_t reserved2;
    };

    struct DDS_HEADER_DXT10 {
        uint32_t dxgiFormat; // maps to DXGI_FORMAT
        uint32_t resourceDimension;
        uint32_t miscFlag;
        uint32_t arraySize;
        uint32_t miscFlags2;
    };

    // Simple helper to calculate DDS memory pitch size and block-aligned dimensions.
    inline void GetSurfaceInfo( uint32_t width, uint32_t height, DXGI_FORMAT format,
                               uint32_t& outNumBytes, uint32_t& outRowBytes )
    {
        bool bc = false;
        uint32_t bpe = 0; // Bytes Per Element

        switch ( format ) {
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            bc = true;
            bpe = 8;
            break;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            bc = true;
            bpe = 8;
            break;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            bc = true;
            bpe = 16;
            break;
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
            bpe = 1;
            break;
        default:
            // Default fallback (e.g. RGBA 32-bit targets)
            bpe = 4;
            break;
        }

        if ( bc ) {
            uint32_t numBlocksWide = (width > 0) ? std::max<uint32_t>( 1, (width + 3) / 4 ) : 0;
            uint32_t numBlocksHigh = (height > 0) ? std::max<uint32_t>( 1, (height + 3) / 4 ) : 0;
            outRowBytes = numBlocksWide * bpe;
            outNumBytes = outRowBytes * numBlocksHigh;
        } else {
            outRowBytes = (width * bpe + 7) / 8; // Bit-to-byte alignment
            outNumBytes = outRowBytes * height;
        }
    }
}

typedef HRESULT( *VirtualFileReader )(const char* path, std::vector<uint8_t> buffer, long* numRead);

inline HRESULT zVdfsReadFile( const char* str, std::vector<uint8_t> buffer, long* numRead ) {
    auto file = zFILE_VDFS::Create( str );
    if ( !file->Exists() ) {
        LogError() << "File does not exist: " << str;
        return E_FAIL;
    }
    if ( file->Open( false ) != 0 ) {
        LogError() << "Failed to open filepath: " << str;
        return E_FAIL;
    }

    const auto size = file->Size();
    buffer.resize( size );
    const auto actuallyRead = file->Read( buffer.data(), size );
    file->Close();
    if ( numRead ) {
        *numRead = actuallyRead;
    }
}

inline HRESULT LoadTextureArray(
    ID3D11Device* pd3dDevice,
    const char* sTexturePrefix,
    int iNumTextures,
    VirtualFileReader fileReader,
    ID3D11Texture2D** ppTex2D,
    ID3D11ShaderResourceView** ppSRV )
{
    if ( !ppTex2D || !ppSRV ) {
        LogError() << "invalid argument: ppTex2D or ppSRV should not be null";
        return E_FAIL;
    }

    HRESULT hr = S_OK;
    D3D11_TEXTURE2D_DESC desc = {};
    DXGI_FORMAT texFormat = DXGI_FORMAT_UNKNOWN;
    CHAR str[MAX_PATH];

    // Keep raw file data buffers alive in RAM until we finish allocating our texture array.
    std::vector<std::vector<uint8_t>> fileBuffers( iNumTextures );
    std::vector<D3D11_SUBRESOURCE_DATA> initData;

    // Step 1: Read raw files from the VFS
    long numRead;
    for ( int i = 0; i < iNumTextures; i++ ) {
        sprintf( str, "%s%.4d.dds", sTexturePrefix, i );

        hr = fileReader( str, fileBuffers[i], &numRead );
        if ( !SUCCEEDED( hr) ) {
            LogError() << "Failed to read file: " << str;
            return E_FAIL;
        }
    }

    // Step 2: Parse raw memory streams entirely on CPU (thread-safe)
    for ( int i = 0; i < iNumTextures; i++ ) {
        const uint8_t* rawData = fileBuffers[i].data();
        const size_t rawSize = fileBuffers[i].size();

        if ( rawSize < (sizeof( uint32_t ) + sizeof( DDS_HEADER )) ) {
            LogError() << "DDS data too small at index: " << i;
            return E_FAIL;
        }

        // Validate DDS Magic Header Number
        uint32_t magicNumber = *reinterpret_cast<const uint32_t*>( rawData );
        if ( magicNumber != DDS_MAGIC ) {
            LogError() << "Invalid DDS magic number at index: " << i;
            return E_FAIL;
        }

        const auto* header = reinterpret_cast<const DDS_HEADER*>(rawData + sizeof( uint32_t ));
        if ( header->size != sizeof( DDS_HEADER ) || header->ddspf.size != sizeof( DDS_PIXELFORMAT ) ) {
            LogError() << "Malformed DDS header structurally at index: " << i;
            return E_FAIL;
        }

        // Detect Format
        DXGI_FORMAT parsedFormat = DXGI_FORMAT_UNKNOWN;
        size_t offset = sizeof( uint32_t ) + sizeof( DDS_HEADER );

        if ( header->ddspf.flags & 0x4 ) { // DDPF_FOURCC
            uint32_t fourCC = header->ddspf.fourCC;

            if ( fourCC == FOURCC_DX10 ) {
                // If it is a DX10 container, parse the secondary header to get the exact DXGI_FORMAT
                if ( rawSize < (offset + sizeof( DDS_HEADER_DXT10 )) ) {
                    LogError() << "DDS file missing DXT10 extended header at index: " << i;
                    return E_FAIL;
                }
                const auto* headerDX10 = reinterpret_cast<const DDS_HEADER_DXT10*>( rawData + offset );
                parsedFormat = static_cast<DXGI_FORMAT>( headerDX10->dxgiFormat );
                offset += sizeof( DDS_HEADER_DXT10 );
            } else if ( fourCC == FOURCC_ATI1 || fourCC == FOURCC_BC4U ) {
                parsedFormat = DXGI_FORMAT_BC4_UNORM;
            } else if ( fourCC == FOURCC_BC4S ) {
                parsedFormat = DXGI_FORMAT_BC4_SNORM;
            }
        } else if ( header->ddspf.flags & 0x20000 ) { // DDPF_LUMINANCE / Single channel
            if ( header->ddspf.rgbBitCount == 8 ) {
                parsedFormat = DXGI_FORMAT_R8_UNORM;
            }
        }

        if ( parsedFormat == DXGI_FORMAT_UNKNOWN ) {
            LogError() << "Unsupported format (only BC4_UNORM, BC4_SNORM, and R8_UNORM supported) at index: " << i;
            return E_FAIL;
        }

        uint32_t width = header->width;
        uint32_t height = header->height;
        uint32_t mipCount = std::max<uint32_t>( 1, header->mipMapCount );

        // Establish the first texture array's base characteristics
        if ( i == 0 ) {
            texFormat = parsedFormat;
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = mipCount;
            desc.ArraySize = iNumTextures;
            desc.Format = texFormat;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Usage = D3D11_USAGE_IMMUTABLE; // CPU read-only, optimized VRAM layout.
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;

            initData.reserve( iNumTextures * mipCount );
        } else {
            // Validate match integrity 
            if ( width != desc.Width || height != desc.Height || mipCount != desc.MipLevels || parsedFormat != texFormat ) {
                LogError() << "Texture index " << i << " mismatch with Texture 0 properties!";
                return E_FAIL;
            }
        }

        const uint8_t* bitData = rawData + offset;

        // Trace and assign each mip level for the slice
        uint32_t currentWidth = width;
        uint32_t currentHeight = height;

        for ( uint32_t mip = 0; mip < mipCount; mip++ ) {
            uint32_t subresourceSize = 0;
            uint32_t rowPitch = 0;

            GetSurfaceInfo( currentWidth, currentHeight, texFormat, subresourceSize, rowPitch );

            if ( offset + subresourceSize > rawSize ) {
                LogError() << "Texture data out of bounds during layout allocation at index: " << i;
                return E_FAIL;
            }

            D3D11_SUBRESOURCE_DATA subData = {};
            subData.pSysMem = bitData;
            subData.SysMemPitch = rowPitch;
            subData.SysMemSlicePitch = subresourceSize;
            initData.push_back( subData );

            // Shift data read offset to point to the next mip-level
            bitData += subresourceSize;
            offset += subresourceSize;

            currentWidth = std::max<uint32_t>( 1, currentWidth >> 1 );
            currentHeight = std::max<uint32_t>( 1, currentHeight >> 1 );
        }
    }

    // Step 3: Atomic GPU Array allocation (using thread-safe pd3dDevice)
    hr = pd3dDevice->CreateTexture2D( &desc, initData.data(), ppTex2D );
    if ( FAILED( hr ) || !(*ppTex2D) ) {
        LogError() << "Failed to allocate 2D Texture Array (Error Code: " << hr << ")";
        return E_FAIL;
    }

    // Step 4: Create Shader Resource View
    D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
    SRVDesc.Format = desc.Format;
    SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    SRVDesc.Texture2DArray.MipLevels = desc.MipLevels;
    SRVDesc.Texture2DArray.ArraySize = iNumTextures;

    hr = pd3dDevice->CreateShaderResourceView( *ppTex2D, &SRVDesc, ppSRV );

    return hr;
}

inline HRESULT LoadTextureArray(
    ID3D11Device* pd3dDevice,
    const char* sTexturePrefix,
    int iNumTextures,
    ID3D11Texture2D** ppTex2D,
    ID3D11ShaderResourceView** ppSRV )
{
    return LoadTextureArray(
        pd3dDevice,
        sTexturePrefix,
        iNumTextures,
        zVdfsReadFile,
        ppTex2D,
        ppSRV );
}

