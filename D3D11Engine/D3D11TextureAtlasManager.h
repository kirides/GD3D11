#pragma once
#include "pch.h"

#include <d3d11.h>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <DirectXTex.h>
#include "ConstantBufferStructs.h"

// Internal struct for bin packing
struct PackItem {
    int originalIndex;
    UINT width;
    UINT height;
    UINT x, y, slice;
    ID3D11Texture2D* texture;
    D3D11_TEXTURE2D_DESC desc;
};

class TextureManager {
private:
    // Helper to align sizes for power-of-two mip boundaries
    static UINT Align( UINT value, UINT alignment ) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    // Returns the block size for BC compressed formats (4), or 1 for uncompressed
    static UINT GetBlockSize( DXGI_FORMAT fmt ) {
        switch ( fmt ) {
            case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
            case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
            case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
                return 4;
            default: return 1;
        }
    }

    // Generates the mip levels that are missing from the source texture (item.desc.MipLevels < mipLevels)
    // by decompressing the last source mip, running box-filter downsampling, re-compressing to
    // atlasFormat, and uploading each new level into the atlas via a temporary immutable texture.
    static void GenerateMissingMips(
        ID3D11Device* device, ID3D11DeviceContext* context,
        ID3D11Texture2D* atlasTextureArray,
        const PackItem& item, DXGI_FORMAT atlasFormat, UINT mipLevels )
    {
        // Capture the source texture to CPU memory (creates an internal staging copy)
        DirectX::ScratchImage captured;
        if ( FAILED( DirectX::CaptureTexture( device, context, item.texture, captured ) ) )
            return;

        // Grab the last available mip as the downsampling base
        const DirectX::Image* lastMipImg = captured.GetImage( item.desc.MipLevels - 1, 0, 0 );
        if ( !lastMipImg ) return;

        // GenerateMipMaps requires uncompressed input — decompress BC textures first
        DirectX::ScratchImage decompressed;
        const DirectX::Image* baseImg = lastMipImg;
        if ( DirectX::IsCompressed( lastMipImg->format ) ) {
            if ( FAILED( DirectX::Decompress( *lastMipImg, DXGI_FORMAT_R8G8B8A8_UNORM, decompressed ) ) )
                return;
            baseImg = decompressed.GetImage( 0, 0, 0 );
        }

        // Generate: level 0 = base (already copied to atlas), levels 1..N = the missing mips
        UINT levelsToGen = mipLevels - item.desc.MipLevels + 1;
        DirectX::ScratchImage mipChain;
        if ( FAILED( DirectX::GenerateMipMaps( *baseImg, DirectX::TEX_FILTER_BOX, levelsToGen, mipChain ) ) )
            return;

        // Re-compress the generated levels back to the atlas BC format.
        // Try GPU-accelerated compression first; fall back to CPU if unsupported.
        const DirectX::ScratchImage* finalChain = &mipChain;
        DirectX::ScratchImage recompressed;
        if ( DirectX::IsCompressed( atlasFormat ) ) {
            HRESULT hr = DirectX::Compress( device,
                mipChain.GetImages(), mipChain.GetImageCount(), mipChain.GetMetadata(),
                atlasFormat, DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_ALPHA_WEIGHT_DEFAULT,
                recompressed );
            if ( FAILED( hr ) ) {
                // GPU BC compression not supported on this hardware — use CPU path
                recompressed = DirectX::ScratchImage{};
                if ( FAILED( DirectX::Compress(
                    mipChain.GetImages(), mipChain.GetImageCount(), mipChain.GetMetadata(),
                    atlasFormat, DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_ALPHA_WEIGHT_DEFAULT,
                    recompressed ) ) )
                    return;
            }
            finalChain = &recompressed;
        }

        // Upload each new mip via a temporary immutable texture + CopySubresourceRegion
        for ( UINT mip = item.desc.MipLevels; mip < mipLevels; ++mip ) {
            // chainIdx 0 = the base (already in atlas), so start at 1
            UINT chainIdx = mip - item.desc.MipLevels + 1;
            const DirectX::Image* src = finalChain->GetImage( chainIdx, 0, 0 );
            if ( !src || !src->pixels ) continue;

            // BC formats require texture dimensions to be multiples of the block size (4).
            // Small mips can be sub-block, so align up to avoid CREATETEXTURE2D_INVALIDDIMENSIONS.
            UINT bsz = GetBlockSize( atlasFormat );
            D3D11_TEXTURE2D_DESC tmpDesc = {};
            tmpDesc.Width            = Align( (UINT)src->width, bsz );
            tmpDesc.Height           = Align( (UINT)src->height, bsz );
            tmpDesc.MipLevels        = 1;
            tmpDesc.ArraySize        = 1;
            tmpDesc.Format           = src->format;
            tmpDesc.SampleDesc.Count = 1;
            tmpDesc.Usage            = D3D11_USAGE_IMMUTABLE;
            tmpDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA initData = {};
            initData.pSysMem     = src->pixels;
            initData.SysMemPitch = (UINT)src->rowPitch;

            ID3D11Texture2D* tmpTex = nullptr;
            if ( SUCCEEDED( device->CreateTexture2D( &tmpDesc, &initData, &tmpTex ) ) ) {
                UINT mipX   = item.x >> mip;
                UINT mipY   = item.y >> mip;
                UINT dstSub = D3D11CalcSubresource( mip, item.slice, mipLevels );
                D3D11_BOX box = { 0, 0, 0, tmpDesc.Width, tmpDesc.Height, 1 };
                context->CopySubresourceRegion( atlasTextureArray, dstSub, mipX, mipY, 0, tmpTex, 0, &box );
                tmpTex->Release();
            }
        }
    }

public:
    struct AtlasResult {
        ID3D11Texture2D* atlasTextureArray = nullptr;
        ID3D11ShaderResourceView* atlasSRV = nullptr;
        std::vector<TextureDescriptor> descriptors;

        void Destroy() {
            SAFE_RELEASE( atlasSRV );
            SAFE_RELEASE( atlasTextureArray );
            descriptors.clear();
        }
    };

    static AtlasResult CreateAtlasArray( ID3D11Device* device, ID3D11DeviceContext* context,
        std::basic_string_view<ID3D11Texture2D*> sourceTextures,
        // const std::vector<ID3D11Texture2D*>& sourceTextures,
        UINT atlasSize = 2048, UINT mipLevels = 6 )
    {
        if ( sourceTextures.empty() ) return {};

        AtlasResult result;
        result.descriptors.resize( sourceTextures.size() );

        // Determine format from first texture for alignment calculation.
        // For BC formats (blockSize=4), coordinates must remain block-aligned at every mip level.
        D3D11_TEXTURE2D_DESC firstDesc;
        sourceTextures[0]->GetDesc( &firstDesc );
        DXGI_FORMAT atlasFormat = firstDesc.Format;

        const UINT blockSize = GetBlockSize( atlasFormat );
        const UINT MipAlignment = blockSize * (1 << (mipLevels - 1));

        std::vector<PackItem> items;
        items.reserve( sourceTextures.size() );

        // 1. Extract info and validate
        for ( size_t i = 0; i < sourceTextures.size(); ++i ) {
            D3D11_TEXTURE2D_DESC desc;
            sourceTextures[i]->GetDesc( &desc );

            if ( desc.Format != atlasFormat ) {
                // For a Texture2DArray, all formats must match. 
                throw std::runtime_error( "All textures must have the same DXGI_FORMAT." );
            }

            items.push_back( { (int)i, desc.Width, desc.Height, 0, 0, 0, sourceTextures[i], desc});
        }

        // 2. Sort by height descending for optimal shelf-packing
        std::sort( items.begin(), items.end(), []( const PackItem& a, const PackItem& b ) {
            return a.height > b.height;
        } );

        // 3. CPU Bin Packing (Shelf Packing Algorithm)
        UINT currentX = 0, currentY = 0, currentShelfHeight = 0, currentSlice = 0;

        for ( auto& item : items ) {
            UINT alignedW = Align( item.width, MipAlignment );
            UINT alignedH = Align( item.height, MipAlignment );

            // Move to next shelf if it doesn't fit horizontally
            if ( currentX + alignedW > atlasSize ) {
                currentX = 0;
                currentY += Align( currentShelfHeight, MipAlignment );
                currentShelfHeight = 0;
            }

            // Move to next array slice if it doesn't fit vertically
            if ( currentY + alignedH > atlasSize ) {
                currentSlice++;
                currentX = 0;
                currentY = 0;
                currentShelfHeight = 0;
            }

            item.x = currentX;
            item.y = currentY;
            item.slice = currentSlice;

            currentX += alignedW;
            currentShelfHeight = std::max( currentShelfHeight, alignedH );
        }

        UINT totalSlices = currentSlice + 1;

        // 4. Create the target Texture2DArray
        D3D11_TEXTURE2D_DESC arrayDesc = {};
        arrayDesc.Width = atlasSize;
        arrayDesc.Height = atlasSize;
        arrayDesc.MipLevels = mipLevels;
        arrayDesc.ArraySize = totalSlices;
        arrayDesc.Format = atlasFormat;
        arrayDesc.SampleDesc.Count = 1;
        arrayDesc.SampleDesc.Quality = 0;
        arrayDesc.Usage = D3D11_USAGE_DEFAULT;
        arrayDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if ( FAILED( device->CreateTexture2D( &arrayDesc, nullptr, &result.atlasTextureArray ) ) ) {
            throw std::runtime_error( "Failed to create Texture2DArray atlas." );
        }

        // 5. GPU CopySubresourceRegion (Extremely fast, zero CPU-readback)

        for ( const auto& item : items ) {
            UINT maxMipsToCopy = std::min( item.desc.MipLevels, mipLevels );

            for ( UINT mip = 0; mip < maxMipsToCopy; ++mip ) {
                // Calculate scaled coordinates for the current mip level
                UINT mipX = item.x >> mip;
                UINT mipY = item.y >> mip;

                // Mip source & destination indices
                UINT srcSub = D3D11CalcSubresource( mip, 0, item.desc.MipLevels );
                UINT dstSub = D3D11CalcSubresource( mip, item.slice, mipLevels );

                context->CopySubresourceRegion(
                    result.atlasTextureArray, dstSub,
                    mipX, mipY, 0,
                    item.texture, srcSub,
                    nullptr // nullptr means copy the whole subresource
                );
            }

            // 5b. Fill missing MIP levels using DirectXTex bilinear downsampling + re-compression.
            if ( item.desc.MipLevels < mipLevels )
                GenerateMissingMips( device, context, result.atlasTextureArray, item, atlasFormat, mipLevels );

            // Write out descriptors in the *original* input order
            TextureDescriptor& outDesc = result.descriptors[item.originalIndex];
            outDesc.slice = item.slice;
            outDesc.uStart = (float)item.x / atlasSize;
            outDesc.vStart = (float)item.y / atlasSize;
            outDesc.uEnd = (float)(item.x + item.width) / atlasSize;
            outDesc.vEnd = (float)(item.y + item.height) / atlasSize;
        }

        // 6. Create SRV
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = atlasFormat;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.MipLevels = mipLevels;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = totalSlices;

        device->CreateShaderResourceView( result.atlasTextureArray, &srvDesc, &result.atlasSRV);

        return result;
    }
};
