#pragma once
#include <vector>
#include <wrl/client.h>
#include <d3d11_1.h>
#include <algorithm>
#include "Logger.h"
#include "zFILE_VDFS.h"
#include "DDSFormat.h"   // shared DDS constants + format decoding / pitch math (DDS:: namespace)

namespace {

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

    // Block-aware DDS surface size, delegating to the shared format tables (handles BC1..BC7 + every
    // common uncompressed format, no silent 32-bit fallback for unknowns).
    inline void GetSurfaceInfo( uint32_t width, uint32_t height, DXGI_FORMAT format,
                               uint32_t& outNumBytes, uint32_t& outRowBytes )
    {
        outRowBytes = DDS::RowPitch( format, width );
        outNumBytes = DDS::SurfaceBytes( format, width, height );
    }
}

typedef HRESULT( *VirtualFileReader )(const char* path, std::vector<uint8_t>& buffer, long* numRead);

inline HRESULT zVdfsReadFile( const char* str, std::vector<uint8_t>& buffer, long* numRead ) {
    auto file = zFILE_VDFS::Create( str );
    if ( !file->Exists() ) {
        LogError() << "File does not exist: " << str;
        return E_FAIL;
    }
    if ( file->Open( false ) != zERRORS::zERROR_NONE ) {
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
    return S_OK;
}

// One mip level's raw pixel data within a parsed slice's file buffer (pointer stays valid as long as
// the owning ParsedTextureArray::fileBuffers entry is alive).
struct ParsedDDSMip {
    const uint8_t* data = nullptr;
    uint32_t rowPitch = 0;
    uint32_t sizeBytes = 0;
};

struct ParsedDDSSlice {
    std::vector<ParsedDDSMip> mips;
};

// Device-agnostic result of parsing a numbered sequence of same-shaped DDS files into a texture array's
// worth of subresource data. Backend-specific loaders (LoadTextureArray below for D3D11, D3D12's own
// loader) turn this into a real GPU resource; this struct only owns the CPU-side bytes.
struct ParsedTextureArray {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0, mipCount = 0;
    std::vector<std::vector<uint8_t>> fileBuffers;   // keeps raw bytes alive; slices[].mips[].data points into these
    std::vector<ParsedDDSSlice> slices;
};

// Reads `iNumTextures` sequentially-numbered DDS files (sTexturePrefix + %.4d + ".dds") via fileReader
// and parses each one's header + every mip level entirely on the CPU — no D3D11/D3D12 calls, so both
// backends' texture-array loaders share this instead of re-implementing DDS parsing. All slices must
// match slice 0's width/height/mip count/format.
inline HRESULT ParseTextureArrayDDS( const char* sTexturePrefix, int iNumTextures, VirtualFileReader fileReader, ParsedTextureArray& out ) {
    CHAR str[MAX_PATH];
    out.fileBuffers.assign( iNumTextures, {} );
    out.slices.assign( iNumTextures, {} );

    // Step 1: Read raw files from the VFS
    long numRead;
    for ( int i = 0; i < iNumTextures; i++ ) {
        sprintf( str, "%s%.4d.dds", sTexturePrefix, i );

        HRESULT hr = fileReader( str, out.fileBuffers[i], &numRead );
        if ( !SUCCEEDED( hr ) ) {
            LogError() << "Failed to read file: " << str;
            return E_FAIL;
        }
    }

    // Step 2: Parse raw memory streams entirely on CPU (thread-safe)
    for ( int i = 0; i < iNumTextures; i++ ) {
        const uint8_t* rawData = out.fileBuffers[i].data();
        const size_t rawSize = out.fileBuffers[i].size();

        if ( rawSize < (sizeof( uint32_t ) + sizeof( DDS_HEADER )) ) {
            LogError() << "DDS data too small at index: " << i << " was: " << rawSize << ", expected: " << (sizeof( uint32_t ) + sizeof( DDS_HEADER )) << " prefix was: " << sTexturePrefix;
            return E_FAIL;
        }

        // Validate DDS Magic Header Number
        uint32_t magicNumber = *reinterpret_cast<const uint32_t*>( rawData );
        if ( magicNumber != DDS::Magic ) {
            LogError() << "Invalid DDS magic number at index: " << i;
            return E_FAIL;
        }

        const auto* header = reinterpret_cast<const DDS_HEADER*>(rawData + sizeof( uint32_t ));
        if ( header->size != sizeof( DDS_HEADER ) || header->ddspf.size != sizeof( DDS_PIXELFORMAT ) ) {
            LogError() << "Malformed DDS header structurally at index: " << i;
            return E_FAIL;
        }

        // Detect format (shared decoder: BC1..BC7, DX10-extended, and uncompressed formats via masks).
        DXGI_FORMAT parsedFormat = DXGI_FORMAT_UNKNOWN;
        size_t offset = sizeof( uint32_t ) + sizeof( DDS_HEADER );

        if ( header->ddspf.flags & DDS::FlagFourCC ) {
            if ( header->ddspf.fourCC == DDS::Dx10 ) {
                // DX10 container: the secondary header carries the exact DXGI_FORMAT.
                if ( rawSize < (offset + sizeof( DDS_HEADER_DXT10 )) ) {
                    LogError() << "DDS file missing DXT10 extended header at index: " << i;
                    return E_FAIL;
                }
                const auto* headerDX10 = reinterpret_cast<const DDS_HEADER_DXT10*>( rawData + offset );
                parsedFormat = static_cast<DXGI_FORMAT>( headerDX10->dxgiFormat );
                offset += sizeof( DDS_HEADER_DXT10 );
            } else {
                parsedFormat = DDS::FromFourCC( header->ddspf.fourCC );
            }
        } else {
            parsedFormat = DDS::FromPixelFormat( header->ddspf.flags, header->ddspf.rgbBitCount,
                header->ddspf.rBitMask, header->ddspf.gBitMask, header->ddspf.bBitMask, header->ddspf.aBitMask );
        }

        // Reject anything the size math can't describe (neither a known BC format nor a known bpp).
        if ( DDS::BCBlockBytes( parsedFormat ) == 0 && DDS::BitsPerPixel( parsedFormat ) == 0 ) {
            LogError() << "Unsupported DDS format (fourCC=" << header->ddspf.fourCC << " flags=" << header->ddspf.flags
                       << " -> DXGI " << static_cast<int>( parsedFormat ) << ") at index: " << i;
            return E_FAIL;
        }

        uint32_t width = header->width;
        uint32_t height = header->height;
        uint32_t mipCount = std::max<uint32_t>( 1, header->mipMapCount );

        // Establish the first texture array's base characteristics
        if ( i == 0 ) {
            out.format = parsedFormat;
            out.width = width;
            out.height = height;
            out.mipCount = mipCount;
        } else {
            // Validate match integrity
            if ( width != out.width || height != out.height || mipCount != out.mipCount || parsedFormat != out.format ) {
                LogError() << "Texture index " << i << " mismatch with Texture 0 properties!";
                return E_FAIL;
            }
        }

        const uint8_t* bitData = rawData + offset;

        // Trace and assign each mip level for the slice
        uint32_t currentWidth = width;
        uint32_t currentHeight = height;

        out.slices[i].mips.resize( mipCount );
        for ( uint32_t mip = 0; mip < mipCount; mip++ ) {
            uint32_t subresourceSize = 0;
            uint32_t rowPitch = 0;

            GetSurfaceInfo( currentWidth, currentHeight, out.format, subresourceSize, rowPitch );

            if ( offset + subresourceSize > rawSize ) {
                LogError() << "Texture data out of bounds during layout allocation at index: " << i;
                return E_FAIL;
            }

            out.slices[i].mips[mip] = { bitData, rowPitch, subresourceSize };

            // Shift data read offset to point to the next mip-level
            bitData += subresourceSize;
            offset += subresourceSize;

            currentWidth = std::max<uint32_t>( 1, currentWidth >> 1 );
            currentHeight = std::max<uint32_t>( 1, currentHeight >> 1 );
        }
    }

    return S_OK;
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

    ParsedTextureArray parsed;
    HRESULT hr = ParseTextureArrayDDS( sTexturePrefix, iNumTextures, fileReader, parsed );
    if ( FAILED( hr ) ) return hr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = parsed.width;
    desc.Height = parsed.height;
    desc.MipLevels = parsed.mipCount;
    desc.ArraySize = iNumTextures;
    desc.Format = parsed.format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_IMMUTABLE; // CPU read-only, optimized VRAM layout.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    std::vector<D3D11_SUBRESOURCE_DATA> initData;
    initData.reserve( static_cast<size_t>(iNumTextures) * parsed.mipCount );
    for ( const auto& slice : parsed.slices ) {
        for ( const auto& mip : slice.mips ) {
            D3D11_SUBRESOURCE_DATA subData = {};
            subData.pSysMem = mip.data;
            subData.SysMemPitch = mip.rowPitch;
            subData.SysMemSlicePitch = mip.sizeBytes;
            initData.push_back( subData );
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

