#include "pch.h"
#include "D3D11StreamingResourcesManager.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>
#include <DirectXTex.h>

using Microsoft::WRL::ComPtr;

// =============================================================================
// Lifecycle
// =============================================================================

D3D11StreamingResourcesManager::~D3D11StreamingResourcesManager() {
    Shutdown();
}

bool D3D11StreamingResourcesManager::GetIsStreamingSupported( ID3D11Device1* device ) {
    if ( !device )
        return false;

    ComPtr<ID3D11Device2> device2;
    if ( FAILED( device->QueryInterface( IID_PPV_ARGS( &device2 ) ) ) )
        return false;

    D3D11_FEATURE_DATA_D3D11_OPTIONS1 options1 = {};
    if ( FAILED( device2->CheckFeatureSupport(
             D3D11_FEATURE_D3D11_OPTIONS1, &options1, sizeof( options1 ) ) ) )
        return false;

    // Tier 1 gives us reserved textures + tile pool + UpdateTileMappings.
    // Tier 2 adds clamped LOD feedback (nice but not required).
    return options1.TiledResourcesTier >= D3D11_TILED_RESOURCES_TIER_2;
}

bool D3D11StreamingResourcesManager::Init( ID3D11Device1* device, ID3D11DeviceContext1* context ) {
    if ( !device || !context )
        return false;

    // QueryInterface up to ID3D11Device2 / ID3D11DeviceContext2
    if ( FAILED( device->QueryInterface( IID_PPV_ARGS( &m_Device2 ) ) ) ) {
        LogError() << "[StreamingResources] Failed to QueryInterface ID3D11Device2";
        return false;
    }

    if ( FAILED( context->QueryInterface( IID_PPV_ARGS( &m_Context2 ) ) ) ) {
        LogError() << "[StreamingResources] Failed to QueryInterface ID3D11DeviceContext2";
        return false;
    }

    m_Device = device;
    m_Context = context;
    m_Initialized = true;

    LogInfo() << "[StreamingResources] Initialized successfully (Tiled Resources supported)";
    return true;
}

void D3D11StreamingResourcesManager::Shutdown() {
    OnWorldUnloaded();

    m_Context2.Reset();
    m_Device2.Reset();
    m_Context.Reset();
    m_Device.Reset();
    m_Initialized = false;
}

void D3D11StreamingResourcesManager::OnWorldUnloaded() {
    // Clear all tile tracking
    m_TileStates.clear();
    while ( !m_LoadQueue.empty() ) m_LoadQueue.pop();
    m_UnloadCandidates.clear();
    m_LoadedSources.clear();

    // Release tiled atlas textures
    m_TiledAtlases.clear();

    // Release tile pools
    m_TilePools.clear();
    m_DefaultTiles.clear();

    // Release source texture references
    m_SourceTextures.clear();

    // Clear global source offset tracking
    m_GlobalSourceOffsets.clear();
    m_TotalSourceCount = 0;

    // Reset staging ring
    for ( auto& s : m_StagingRing ) {
        s.texture.Reset();
        s.inUse = false;
    }
    m_StagingRingHead = 0;
}

// =============================================================================
// Feedback Query Methods
// =============================================================================

UINT D3D11StreamingResourcesManager::GetGlobalSourceOffset( DXGI_FORMAT fmt ) const {
    auto it = m_GlobalSourceOffsets.find( fmt );
    return ( it != m_GlobalSourceOffsets.end() ) ? it->second : 0;
}

UINT D3D11StreamingResourcesManager::GetTotalSourceCount() const {
    return m_TotalSourceCount;
}

const std::vector<D3D11StreamingResourcesManager::SourceTextureInfo>&
D3D11StreamingResourcesManager::GetSourceTextures( DXGI_FORMAT fmt ) const {
    auto it = m_SourceTextures.find( fmt );
    if ( it != m_SourceTextures.end() )
        return it->second;
    static const std::vector<SourceTextureInfo> empty;
    return empty;
}

// =============================================================================
// Key generation
// =============================================================================

uint64_t D3D11StreamingResourcesManager::MakeTileKey(
    DXGI_FORMAT fmt, UINT subresource, UINT tileX, UINT tileY ) {
    // Pack into 64 bits: [fmt:16][subresource:16][tileX:16][tileY:16]
    return ( static_cast<uint64_t>( fmt ) << 48 )
         | ( static_cast<uint64_t>( subresource & 0xFFFF ) << 32 )
         | ( static_cast<uint64_t>( tileX & 0xFFFF ) << 16 )
         | ( static_cast<uint64_t>( tileY & 0xFFFF ) );
}

uint64_t D3D11StreamingResourcesManager::MakeSourceKey(
    DXGI_FORMAT fmt, UINT sourceIndex, UINT mip ) {
    // Pack into 64 bits: [fmt:16][sourceIndex:32][mip:16]
    return ( static_cast<uint64_t>( fmt ) << 48 )
         | ( static_cast<uint64_t>( sourceIndex ) << 16 )
         | ( static_cast<uint64_t>( mip & 0xFFFF ) );
}

// =============================================================================
// Tile Pool Management
// =============================================================================

bool D3D11StreamingResourcesManager::CreateTilePool( DXGI_FORMAT fmt, UINT numTiles ) {
    D3D11_BUFFER_DESC poolDesc = {};
    poolDesc.ByteWidth = numTiles * TILE_SIZE_BYTES;
    poolDesc.Usage = D3D11_USAGE_DEFAULT;
    poolDesc.MiscFlags = D3D11_RESOURCE_MISC_TILE_POOL;

    TilePool pool;
    pool.totalTiles = numTiles;
    pool.usedTiles = 0;

    HRESULT hr = m_Device2->CreateBuffer( &poolDesc, nullptr, pool.buffer.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "[StreamingResources] Failed to create tile pool for format "
                   << static_cast<int>( fmt ) << " (hr=" << hr << ")";
        return false;
    }

    m_TilePools[fmt] = std::move( pool );

    LogInfo() << "[StreamingResources] Created tile pool: " << numTiles << " tiles ("
              << ( numTiles * TILE_SIZE_BYTES / ( 1024 * 1024 ) ) << " MB) for format "
              << static_cast<int>( fmt );
    return true;
}

void D3D11StreamingResourcesManager::GrowTilePool( DXGI_FORMAT fmt, UINT additionalTiles ) {
    auto it = m_TilePools.find( fmt );
    if ( it == m_TilePools.end() )
        return;

    TilePool& pool = it->second;
    UINT newTotal = pool.totalTiles + additionalTiles;
    UINT64 newSizeBytes = static_cast<UINT64>( newTotal ) * TILE_SIZE_BYTES;

    // ID3D11DeviceContext2::ResizeTilePool resizes the pool buffer in-place.
    // Existing tile data is preserved; new tiles are appended.
    HRESULT hr = m_Context2->ResizeTilePool( pool.buffer.Get(), newSizeBytes );
    if ( FAILED( hr ) ) {
        LogWarn() << "[StreamingResources] Failed to grow tile pool for format "
                  << static_cast<int>( fmt ) << " (hr=" << hr << ")";
        return;
    }

    pool.totalTiles = newTotal;

    LogInfo() << "[StreamingResources] Grew tile pool to " << newTotal << " tiles ("
              << ( newTotal * TILE_SIZE_BYTES / ( 1024 * 1024 ) ) << " MB) for format "
              << static_cast<int>( fmt );
}

UINT D3D11StreamingResourcesManager::AllocateTile( DXGI_FORMAT fmt ) {
    auto it = m_TilePools.find( fmt );
    if ( it == m_TilePools.end() )
        return UINT_MAX;

    TilePool& pool = it->second;

    // Prefer recycled tiles
    if ( !pool.freeTiles.empty() ) {
        UINT idx = pool.freeTiles.back();
        pool.freeTiles.pop_back();
        return idx;
    }

    // Allocate from high-water mark
    if ( pool.usedTiles < pool.totalTiles ) {
        return pool.usedTiles++;
    }

    // Pool exhausted — grow it
    UINT growth = std::max<UINT>( pool.totalTiles / 2, 64 );
    GrowTilePool( fmt, growth );

    if ( pool.usedTiles < pool.totalTiles ) {
        return pool.usedTiles++;
    }

    LogError() << "[StreamingResources] Tile pool exhausted and growth failed for format "
               << static_cast<int>( fmt );
    return UINT_MAX;
}

void D3D11StreamingResourcesManager::FreeTile( DXGI_FORMAT fmt, UINT tileIndex ) {
    auto it = m_TilePools.find( fmt );
    if ( it == m_TilePools.end() )
        return;

    it->second.freeTiles.push_back( tileIndex );
}

// =============================================================================
// Default Tile
// =============================================================================

void D3D11StreamingResourcesManager::FillDefaultTileData(
    DXGI_FORMAT fmt, std::vector<uint8_t>& outData ) {
    outData.resize( TILE_SIZE_BYTES );

    switch ( fmt ) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB: {
        // BC1 block: 8 bytes per 4x4 pixel block
        // Magenta = (255, 0, 255) encoded as two 16-bit RGB565 endpoints
        // RGB565: R=31, G=0, B=31 → 0xF81F
        uint8_t block[8] = {};
        uint16_t color = 0xF81F; // magenta in RGB565
        memcpy( &block[0], &color, 2 ); // color0
        memcpy( &block[2], &color, 2 ); // color1
        // Indices: all 0 (use color0) → block[4..7] = 0x00
        block[4] = 0x00; block[5] = 0x00; block[6] = 0x00; block[7] = 0x00;

        for ( UINT i = 0; i < TILE_SIZE_BYTES; i += 8 ) {
            memcpy( outData.data() + i, block, 8 );
        }
        break;
    }
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB: {
        // BC2 block: 16 bytes (8 alpha + 8 color)
        // Transparent-black: alpha = 0 so DoAlphaTest() clips these pixels,
        // preventing magenta seams on alpha-tested geometry (trees, fences).
        uint8_t block[16] = {};
        // Alpha: all 0x00 (fully transparent, 4 bits per pixel, 16 pixels)
        memset( &block[0], 0x00, 8 );
        // Color: black (RGB565 = 0x0000)
        uint16_t color = 0x0000;
        memcpy( &block[8], &color, 2 );
        memcpy( &block[10], &color, 2 );
        block[12] = 0x00; block[13] = 0x00; block[14] = 0x00; block[15] = 0x00;

        for ( UINT i = 0; i < TILE_SIZE_BYTES; i += 16 ) {
            memcpy( outData.data() + i, block, 16 );
        }
        break;
    }
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB: {
        // BC3 block: 16 bytes (8 alpha + 8 color)
        // Transparent-black: alpha = 0 so DoAlphaTest() clips these pixels,
        // preventing magenta seams on alpha-tested geometry (trees, fences).
        uint8_t block[16] = {};
        // Alpha: alpha0=0x00, alpha1=0x00, indices all 0 → all pixels = 0x00
        block[0] = 0x00; // alpha0
        block[1] = 0x00; // alpha1
        // Alpha indices: all 0 → bytes 2..7 = 0
        block[2] = 0x00; block[3] = 0x00; block[4] = 0x00;
        block[5] = 0x00; block[6] = 0x00; block[7] = 0x00;
        // Color: black (RGB565 = 0x0000)
        uint16_t color = 0x0000;
        memcpy( &block[8], &color, 2 );
        memcpy( &block[10], &color, 2 );
        block[12] = 0x00; block[13] = 0x00; block[14] = 0x00; block[15] = 0x00;

        for ( UINT i = 0; i < TILE_SIZE_BYTES; i += 16 ) {
            memcpy( outData.data() + i, block, 16 );
        }
        break;
    }
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
        // BGRA: B=0xFF, G=0x00, R=0xFF, A=0xFF → magenta
        for ( UINT i = 0; i < TILE_SIZE_BYTES; i += 4 ) {
            outData[i + 0] = 0xFF; // B
            outData[i + 1] = 0x00; // G
            outData[i + 2] = 0xFF; // R
            outData[i + 3] = 0xFF; // A
        }
        break;
    }
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: {
        // RGBA: R=0xFF, G=0x00, B=0xFF, A=0xFF → magenta
        for ( UINT i = 0; i < TILE_SIZE_BYTES; i += 4 ) {
            outData[i + 0] = 0xFF; // R
            outData[i + 1] = 0x00; // G
            outData[i + 2] = 0xFF; // B
            outData[i + 3] = 0xFF; // A
        }
        break;
    }
    default:
        // For unknown formats, fill with 0xFF pattern
        memset( outData.data(), 0xFF, TILE_SIZE_BYTES );
        break;
    }
}

void D3D11StreamingResourcesManager::InitDefaultTile( DXGI_FORMAT fmt ) {
    auto poolIt = m_TilePools.find( fmt );
    if ( poolIt == m_TilePools.end() )
        return;

    // Reserve tile 0 as the default tile
    TilePool& pool = poolIt->second;
    if ( pool.usedTiles == 0 )
        pool.usedTiles = 1; // tile 0 is reserved

    // Generate magenta fill data
    std::vector<uint8_t> tileData;
    FillDefaultTileData( fmt, tileData );

    // Upload default tile data to the tile pool at index 0.
    // We use UpdateTileMappings to map a temporary tiled texture tile to pool[0],
    // then write the data. But since we can't write directly to a tile pool,
    // we'll write via the atlas itself after mapping.
    //
    // For the initial setup, we write the default tile data via a staging texture
    // and CopySubresourceRegion to a temporary tile-mapped region.
    // Alternatively, we can use ID3D11DeviceContext2::UpdateTiles.
    auto atlasIt = m_TiledAtlases.find( fmt );
    if ( atlasIt == m_TiledAtlases.end() )
        return;

    const TiledAtlas& atlas = atlasIt->second;

    // First, map tile (0,0) of the coarsest mip to pool tile 0
    D3D11_TILED_RESOURCE_COORDINATE coord = {};
    coord.Subresource = D3D11CalcSubresource( atlas.mipLevels - 1, 0, atlas.mipLevels );
    coord.X = 0;
    coord.Y = 0;
    coord.Z = 0;

    D3D11_TILE_REGION_SIZE regionSize = {};
    regionSize.NumTiles = 1;
    regionSize.bUseBox = FALSE;

    UINT poolOffset = 0; // tile index 0

    m_Context2->UpdateTileMappings(
        atlas.texture.Get(),
        1,               // numRegions
        &coord,
        &regionSize,
        pool.buffer.Get(),
        1,               // numRanges
        nullptr,         // rangeFlags (nullptr = default = use tile pool offsets)
        &poolOffset,
        nullptr,         // rangeTileCounts (nullptr with single tile)
        0                // flags
    );

    // Now use UpdateTiles to write the data directly to the mapped tile
    D3D11_TILED_RESOURCE_COORDINATE updateCoord = coord;
    D3D11_TILE_REGION_SIZE updateRegion = regionSize;

    m_Context2->UpdateTiles(
        atlas.texture.Get(),
        &updateCoord,
        &updateRegion,
        tileData.data(),
        0 // flags
    );

    DefaultTile& dt = m_DefaultTiles[fmt];
    dt.poolIndex = 0;
    dt.initialized = true;

    LogInfo() << "[StreamingResources] Default magenta tile initialized for format "
              << static_cast<int>( fmt );
}

// =============================================================================
// Tile Mapping
// =============================================================================

void D3D11StreamingResourcesManager::MapTileToDefault(
    const TiledAtlas& atlas, UINT subresource,
    UINT tileX, UINT tileY, DXGI_FORMAT fmt ) {

    auto dtIt = m_DefaultTiles.find( fmt );
    if ( dtIt == m_DefaultTiles.end() || !dtIt->second.initialized )
        return;

    auto poolIt = m_TilePools.find( fmt );
    if ( poolIt == m_TilePools.end() )
        return;

    D3D11_TILED_RESOURCE_COORDINATE coord = {};
    coord.Subresource = subresource;
    coord.X = tileX;
    coord.Y = tileY;
    coord.Z = 0;

    D3D11_TILE_REGION_SIZE regionSize = {};
    regionSize.NumTiles = 1;
    regionSize.bUseBox = FALSE;

    UINT poolOffset = dtIt->second.poolIndex; // always tile 0

    // Map this tile to the shared default tile (many-to-one mapping is allowed)
    m_Context2->UpdateTileMappings(
        atlas.texture.Get(),
        1, &coord, &regionSize,
        poolIt->second.buffer.Get(),
        1, nullptr, &poolOffset, nullptr,
        0
    );
}

void D3D11StreamingResourcesManager::MapTileToPool(
    const TiledAtlas& atlas, UINT subresource,
    UINT tileX, UINT tileY, UINT poolTileIndex,
    DXGI_FORMAT fmt ) {

    auto poolIt = m_TilePools.find( fmt );
    if ( poolIt == m_TilePools.end() )
        return;

    D3D11_TILED_RESOURCE_COORDINATE coord = {};
    coord.Subresource = subresource;
    coord.X = tileX;
    coord.Y = tileY;
    coord.Z = 0;

    D3D11_TILE_REGION_SIZE regionSize = {};
    regionSize.NumTiles = 1;
    regionSize.bUseBox = FALSE;

    m_Context2->UpdateTileMappings(
        atlas.texture.Get(),
        1, &coord, &regionSize,
        poolIt->second.buffer.Get(),
        1, nullptr, &poolTileIndex, nullptr,
        0
    );
}

void D3D11StreamingResourcesManager::MapAllTilesToDefault(
    const TiledAtlas& atlas, DXGI_FORMAT fmt ) {

    auto dtIt = m_DefaultTiles.find( fmt );
    if ( dtIt == m_DefaultTiles.end() || !dtIt->second.initialized )
        return;
    auto poolIt = m_TilePools.find( fmt );
    if ( poolIt == m_TilePools.end() )
        return;

    // Use GetResourceTiling to discover the tile layout
    UINT numTilesForResource = 0;
    D3D11_PACKED_MIP_DESC packedMipDesc = {};
    D3D11_TILE_SHAPE tileShape = {};
    UINT numSubresourceTilings = atlas.mipLevels * atlas.arraySlices;
    std::vector<D3D11_SUBRESOURCE_TILING> subresourceTilings( numSubresourceTilings );

    m_Device2->GetResourceTiling(
        atlas.texture.Get(),
        &numTilesForResource,
        &packedMipDesc,
        &tileShape,
        &numSubresourceTilings,
        0,
        subresourceTilings.data()
    );

    // Map all tiles across all subresources to the default tile
    UINT defaultPoolOffset = dtIt->second.poolIndex;

    for ( UINT sub = 0; sub < numSubresourceTilings; ++sub ) {
        const auto& tiling = subresourceTilings[sub];
        if ( tiling.WidthInTiles == 0 || tiling.HeightInTiles == 0 )
            continue;

        UINT totalTilesInSub = tiling.WidthInTiles * tiling.HeightInTiles;

        // Map the entire subresource to the default tile using a single call
        D3D11_TILED_RESOURCE_COORDINATE coord = {};
        coord.Subresource = sub;
        coord.X = 0;
        coord.Y = 0;
        coord.Z = 0;

        D3D11_TILE_REGION_SIZE regionSize = {};
        regionSize.NumTiles = totalTilesInSub;
        regionSize.bUseBox = TRUE;
        regionSize.Width = tiling.WidthInTiles;
        regionSize.Height = tiling.HeightInTiles;
        regionSize.Depth = 1;

        // All tiles map to the same default pool tile (reuse mapping)
        UINT rangeFlag = D3D11_TILE_RANGE_REUSE_SINGLE_TILE;
        UINT rangeCount = totalTilesInSub;

        m_Context2->UpdateTileMappings(
            atlas.texture.Get(),
            1, &coord, &regionSize,
            poolIt->second.buffer.Get(),
            1, &rangeFlag, &defaultPoolOffset, &rangeCount,
            0
        );

        // Record tile states as Unmapped (pointing to default)
        UINT mip = sub % atlas.mipLevels;
        for ( UINT ty = 0; ty < tiling.HeightInTiles; ++ty ) {
            for ( UINT tx = 0; tx < tiling.WidthInTiles; ++tx ) {
                uint64_t key = MakeTileKey( fmt, sub, tx, ty );
                TileInfo& info = m_TileStates[key];
                info.state = TileState::Unmapped;
                info.subresource = sub;
                info.tileX = tx;
                info.tileY = ty;
                info.format = fmt;
                info.poolTileIndex = 0;
                info.lastUsedTime = 0.0f;
                info.priority = 0.0f;
            }
        }
    }

    // Handle packed mips (mips packed into shared tiles at the tail of the resource)
    if ( packedMipDesc.NumPackedMips > 0 && packedMipDesc.NumTilesForPackedMips > 0 ) {
        // For packed mips, we map the packed tile region for each array slice.
        // Each slice has NumTilesForPackedMips consecutive tiles starting at
        // packedMipDesc.StartTileIndexInOverallResource (for slice 0).
        for ( UINT slice = 0; slice < atlas.arraySlices; ++slice ) {
            UINT startTile = packedMipDesc.StartTileIndexInOverallResource
                           + slice * packedMipDesc.NumTilesForPackedMips;

            // Use NULL coordinates + NULL region to map by absolute tile index
            UINT rangeFlag = D3D11_TILE_RANGE_REUSE_SINGLE_TILE;
            UINT rangeCount = packedMipDesc.NumTilesForPackedMips;

            m_Context2->UpdateTileMappings(
                atlas.texture.Get(),
                1, nullptr, nullptr,     // NULL = map by start tile offset
                poolIt->second.buffer.Get(),
                1, &rangeFlag, &defaultPoolOffset, &rangeCount,
                0
            );
        }
    }

    LogInfo() << "[StreamingResources] Mapped all " << numTilesForResource
              << " tiles to default for format " << static_cast<int>( fmt );
}

// =============================================================================
// Subresource Tile Layout
// =============================================================================

void D3D11StreamingResourcesManager::GetSubresourceTileCount(
    const TiledAtlas& atlas, UINT mipLevel,
    UINT& tilesX, UINT& tilesY ) const {

    UINT numSubresourceTilings = atlas.mipLevels * atlas.arraySlices;
    std::vector<D3D11_SUBRESOURCE_TILING> tilings( numSubresourceTilings );
    UINT totalTiles = 0;
    D3D11_PACKED_MIP_DESC packedDesc = {};
    D3D11_TILE_SHAPE tileShape = {};

    m_Device2->GetResourceTiling(
        atlas.texture.Get(),
        &totalTiles, &packedDesc, &tileShape,
        &numSubresourceTilings, 0, tilings.data()
    );

    if ( mipLevel < numSubresourceTilings ) {
        tilesX = tilings[mipLevel].WidthInTiles;
        tilesY = tilings[mipLevel].HeightInTiles;
    } else {
        tilesX = 0;
        tilesY = 0;
    }
}

// =============================================================================
// Atlas Creation
// =============================================================================

TextureManager::AtlasResult D3D11StreamingResourcesManager::CreateStreamingAtlasArray(
    std::basic_string_view<ID3D11Texture2D*> sourceTextures,
    UINT atlasSize, UINT mipLevels ) {

    if ( sourceTextures.empty() || !m_Initialized )
        return {};

    TextureManager::AtlasResult result;
    result.descriptors.resize( sourceTextures.size() );

    // --- 1. Get format from first texture ---
    D3D11_TEXTURE2D_DESC firstDesc;
    sourceTextures[0]->GetDesc( &firstDesc );
    DXGI_FORMAT atlasFormat = firstDesc.Format;

    // --- 2. Run the same shelf-packing algorithm as TextureManager ---
    const UINT blockSize = []( DXGI_FORMAT fmt ) -> UINT {
        switch ( fmt ) {
            case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
            case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
            case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
                return 4;
            default: return 1;
        }
    }( atlasFormat );

    const UINT MipAlignment = blockSize * ( 1 << ( mipLevels - 1 ) );

    struct PackItem {
        int originalIndex;
        UINT width, height;
        UINT x, y, slice;
        D3D11_TEXTURE2D_DESC desc;
    };

    std::vector<PackItem> items;
    items.reserve( sourceTextures.size() );

    for ( size_t i = 0; i < sourceTextures.size(); ++i ) {
        D3D11_TEXTURE2D_DESC desc;
        sourceTextures[i]->GetDesc( &desc );
        items.push_back( { static_cast<int>( i ), desc.Width, desc.Height, 0, 0, 0, desc } );
    }

    // Sort by height descending for shelf packing
    std::sort( items.begin(), items.end(), []( const PackItem& a, const PackItem& b ) {
        return a.height > b.height;
    } );

    auto Align = []( UINT value, UINT alignment ) -> UINT {
        return ( value + alignment - 1 ) & ~( alignment - 1 );
    };

    // Shelf packing
    UINT currentX = 0, currentY = 0, currentShelfHeight = 0, currentSlice = 0;
    for ( auto& item : items ) {
        UINT alignedW = Align( item.width, MipAlignment );
        UINT alignedH = Align( item.height, MipAlignment );

        if ( currentX + alignedW > atlasSize ) {
            currentX = 0;
            currentY += Align( currentShelfHeight, MipAlignment );
            currentShelfHeight = 0;
        }
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
        currentShelfHeight = std::max<UINT>( currentShelfHeight, alignedH );
    }

    UINT totalSlices = currentSlice + 1;

    // --- 2b. Clamp mip levels for tiled resource array constraints ---
    // On Tier 2 (and Tier 1): when ArraySize > 1, every mip must have dimensions
    // >= the standard tile extent. Sub-tile mips ("packed mips") are NOT supported
    // for texture arrays. Determine tile dimensions and limit mip count.
    if ( totalSlices > 1 ) {
        UINT tileW = 128, tileH = 128; // conservative default for 32bpp
        switch ( atlasFormat ) {
            case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
                tileW = 512; tileH = 256; break;  // 0.5 bytes/texel
            case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
            case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
                tileW = 256; tileH = 256; break;  // 1 byte/texel
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                tileW = 128; tileH = 128; break;  // 4 bytes/texel
            default: break;
        }

        // Count how many mip levels fit without going below tile extents
        UINT maxMips = 0;
        for ( UINT m = 0; m < mipLevels; ++m ) {
            UINT mipW = std::max<UINT>( 1u, atlasSize >> m );
            UINT mipH = std::max<UINT>( 1u, atlasSize >> m );
            if ( mipW < tileW || mipH < tileH )
                break;
            maxMips = m + 1;
        }
        maxMips = std::max<UINT>( maxMips, 1 ); // at least 1 mip

        if ( maxMips < mipLevels ) {
            LogInfo() << "[StreamingResources] Clamping mip levels from " << mipLevels
                      << " to " << maxMips << " for array size " << totalSlices
                      << " (tile extent " << tileW << "x" << tileH
                      << ", format " << static_cast<int>( atlasFormat ) << ")";
            mipLevels = maxMips;
        }
    }

    // --- 3. Create the tiled Texture2DArray ---
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
    arrayDesc.MiscFlags = D3D11_RESOURCE_MISC_TILED;

    ComPtr<ID3D11Texture2D> tiledTexture;
    HRESULT hr = m_Device2->CreateTexture2D( &arrayDesc, nullptr, tiledTexture.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "[StreamingResources] Failed to create tiled Texture2DArray (hr=" << hr << ")";
        return {};
    }

    // --- 4. Create tile pool ---
    if ( !CreateTilePool( atlasFormat, INITIAL_POOL_TILES ) ) {
        return {};
    }

    // --- 5. Store the atlas ---
    TiledAtlas& atlas = m_TiledAtlases[atlasFormat];
    atlas.texture = tiledTexture;
    atlas.atlasSize = atlasSize;
    atlas.mipLevels = mipLevels;
    atlas.arraySlices = totalSlices;
    atlas.format = atlasFormat;

    // --- 6. Initialize default tile and map all to it ---
    InitDefaultTile( atlasFormat );
    MapAllTilesToDefault( atlas, atlasFormat );

    // --- 7. Create SRV ---
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = atlasFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = mipLevels;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = totalSlices;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = m_Device2->CreateShaderResourceView( tiledTexture.Get(), &srvDesc, srv.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "[StreamingResources] Failed to create SRV for tiled atlas (hr=" << hr << ")";
        return {};
    }
    atlas.srv = srv;

    // --- 8. Store source texture info for streaming uploads ---
    auto& sources = m_SourceTextures[atlasFormat];
    sources.clear();
    sources.reserve( items.size() );
    for ( const auto& item : items ) {
        SourceTextureInfo si;
        si.texture = sourceTextures[item.originalIndex];
        si.x = item.x;
        si.y = item.y;
        si.slice = item.slice;
        si.width = item.width;
        si.height = item.height;
        si.sourceMipLevels = item.desc.MipLevels;
        sources.push_back( std::move( si ) );
    }

    // --- 8b. Compute global source offsets for feedback texture indexing ---
    // Each format's sources get a contiguous range in the flat feedback texture.
    // This must be done after all atlas formats have been populated, but since
    // CreateStreamingAtlasArray is called once per format, we recompute every time.
    m_GlobalSourceOffsets.clear();
    m_TotalSourceCount = 0;
    for ( const auto& [fmt, srcVec] : m_SourceTextures ) {
        m_GlobalSourceOffsets[fmt] = m_TotalSourceCount;
        m_TotalSourceCount += static_cast<UINT>( srcVec.size() );
    }

    // --- 9. Write descriptors in original input order ---
    for ( const auto& item : items ) {
        TextureDescriptor& outDesc = result.descriptors[item.originalIndex];
        outDesc.slice = item.slice;
        outDesc.uStart = static_cast<float>( item.x ) / atlasSize;
        outDesc.vStart = static_cast<float>( item.y ) / atlasSize;
        outDesc.uEnd = static_cast<float>( item.x + item.width ) / atlasSize;
        outDesc.vEnd = static_cast<float>( item.y + item.height ) / atlasSize;
    }

    // --- 10. Hand back raw pointers for AtlasResult (caller manages lifetime) ---
    // Note: The tiled texture and SRV are owned by m_TiledAtlases; return raw ptrs
    // that the AtlasResult can reference. We override Destroy() behavior by keeping
    // our own refs.
    result.atlasTextureArray = tiledTexture.Get();
    result.atlasSRV = srv.Get();

    // AddRef so the raw pointers in AtlasResult remain valid
    result.atlasTextureArray->AddRef();
    result.atlasSRV->AddRef();

    // --- 11. Preload coarsest mip levels ---
    PreloadCoarseMips( atlasFormat );

    LogInfo() << "[StreamingResources] Created streaming atlas: "
              << atlasSize << "x" << atlasSize << " x " << totalSlices << " slices, "
              << mipLevels << " mips, format " << static_cast<int>( atlasFormat );

    return result;
}

// =============================================================================
// Preload Coarse Mips
// =============================================================================

void D3D11StreamingResourcesManager::PreloadCoarseMips( DXGI_FORMAT fmt ) {
    auto atlasIt = m_TiledAtlases.find( fmt );
    if ( atlasIt == m_TiledAtlases.end() )
        return;

    auto srcIt = m_SourceTextures.find( fmt );
    if ( srcIt == m_SourceTextures.end() )
        return;

    const TiledAtlas& atlas = atlasIt->second;
    const auto& sources = srcIt->second;

    // Query the tiling layout once to determine standard vs packed mip levels.
    // Packed mips are below the minimum tile dimension and cannot have individual
    // tiles mapped in a Texture2DArray — they are already covered by the default
    // tile mapping established in MapAllTilesToDefault.
    UINT numSubresourceTilings = atlas.mipLevels * atlas.arraySlices;
    std::vector<D3D11_SUBRESOURCE_TILING> tilings( numSubresourceTilings );
    UINT totalTilesInResource = 0;
    D3D11_PACKED_MIP_DESC packedDesc = {};
    D3D11_TILE_SHAPE tileShape = {};
    m_Device2->GetResourceTiling(
        atlas.texture.Get(),
        &totalTilesInResource, &packedDesc, &tileShape,
        &numSubresourceTilings, 0, tilings.data()
    );

    // Standard mips: [0, standardMips) support per-tile mappings.
    // Packed mips: [standardMips, mipLevels) share a single packed allocation.
    UINT standardMips = atlas.mipLevels - packedDesc.NumPackedMips;
    if ( standardMips == 0 ) {
        LogInfo() << "[StreamingResources] All mips are packed for format "
                  << static_cast<int>( fmt ) << " — nothing to preload";
        return;
    }

    // Preload the PRELOADED_COARSE_MIPS coarsest *standard* mip levels only.
    // The original code iterated atlas.mipLevels - PRELOADED_COARSE_MIPS which,
    // for a 2048px BC atlas (tile=256px), lands on mip 5 (64px) — a packed mip.
    // GetSubresourceTileCount then returns (0,0) and every source is skipped,
    // leaving all tiles on the default magenta tile with no real data ever loaded.
    UINT preloadStart = ( standardMips > PRELOADED_COARSE_MIPS )
                      ? standardMips - PRELOADED_COARSE_MIPS
                      : 0;

    UINT tilesUploaded = 0;

    for ( UINT mip = preloadStart; mip < standardMips; ++mip ) {
        for ( UINT srcIdx = 0; srcIdx < static_cast<UINT>( sources.size() ); ++srcIdx ) {
            const auto& src = sources[srcIdx];

            // Missing mips are handled by GenerateMissingMips below — skip here
            if ( mip >= src.sourceMipLevels )
                continue;

            UINT dstSub = D3D11CalcSubresource( mip, src.slice, atlas.mipLevels );
            if ( dstSub >= numSubresourceTilings )
                continue;

            const auto& tiling = tilings[dstSub];
            if ( tiling.WidthInTiles == 0 || tiling.HeightInTiles == 0 )
                continue; // Packed or degenerate — skip

            // Compute which tiles in this subresource are touched by the source
            // texture's region.  Non-uniform sources may cover only a sub-rect of
            // the atlas subresource, so we must not map tiles outside that rect.
            UINT regionX = src.x >> mip;
            UINT regionY = src.y >> mip;
            UINT regionW = std::max<UINT>( 1u, src.width  >> mip );
            UINT regionH = std::max<UINT>( 1u, src.height >> mip );

            UINT tileW = tileShape.WidthInTexels;
            UINT tileH = tileShape.HeightInTexels;

            UINT tileStartX = regionX / tileW;
            UINT tileStartY = regionY / tileH;
            UINT tileEndX   = std::min<UINT>( tiling.WidthInTiles  - 1, ( regionX + regionW - 1 ) / tileW );
            UINT tileEndY   = std::min<UINT>( tiling.HeightInTiles - 1, ( regionY + regionH - 1 ) / tileH );

            // Ensure all tiles covering this source region are mapped to real pool
            // tiles.  Mapping MUST happen before CopySubresourceRegion, otherwise
            // the GPU silently discards writes to unmapped (default) tiles.
            for ( UINT ty = tileStartY; ty <= tileEndY; ++ty ) {
                for ( UINT tx = tileStartX; tx <= tileEndX; ++tx ) {
                    uint64_t tileKey = MakeTileKey( fmt, dstSub, tx, ty );
                    auto stateIt = m_TileStates.find( tileKey );
                    if ( stateIt != m_TileStates.end() &&
                         stateIt->second.state == TileState::Resident )
                        continue; // Shared tile already resident from an earlier source

                    UINT poolTile = AllocateTile( fmt );
                    if ( poolTile == UINT_MAX )
                        continue; // Pool exhausted — leave on default tile

                    MapTileToPool( atlas, dstSub, tx, ty, poolTile, fmt );

                    TileInfo info;
                    info.state         = TileState::Resident;
                    info.poolTileIndex = poolTile;
                    info.subresource   = dstSub;
                    info.tileX         = tx;
                    info.tileY         = ty;
                    info.format        = fmt;
                    info.lastUsedTime  = 0.0f;
                    m_TileStates[tileKey] = info;
                    ++tilesUploaded;
                }
            }

            // Upload this source's pixel data.  Always upload — even when all
            // tiles were already Resident from an earlier source.  Multiple
            // sources share the same 64KB tile but occupy different (x,y) regions
            // within it.  Gating on "anyNewTiles" caused the second source's
            // region to never be written, leaving it as uninitialized pool memory
            // (black) or transparent (invisible alpha-tested geometry).
            UploadTileData( atlas, dstSub, src, mip );

            // Track as loaded so UpdateStreaming won't re-upload and eviction can
            // properly invalidate this source-mip if the tile is reclaimed.
            uint64_t srcKey = MakeSourceKey( fmt, srcIdx, mip );
            m_LoadedSources.insert( srcKey );
        }
    }

    // Generate missing mip levels for sources that have fewer mips than the atlas.
    // This is called once per source at atlas creation time; the generated data is
    // uploaded immediately so coarse mips are always available.
    for ( UINT srcIdx = 0; srcIdx < static_cast<UINT>( sources.size() ); ++srcIdx ) {
        const auto& src = sources[srcIdx];
        if ( src.sourceMipLevels < atlas.mipLevels ) {
            GenerateMissingMips( atlas, src, fmt, srcIdx,
                                 tilings, numSubresourceTilings, tileShape );
        }
    }

    LogInfo() << "[StreamingResources] Preloaded " << PRELOADED_COARSE_MIPS
              << " coarsest standard mip(s) (" << tilesUploaded << " tiles) for format "
              << static_cast<int>( fmt );
}

// =============================================================================
// Tile Data Upload
// =============================================================================

void D3D11StreamingResourcesManager::UploadTileData(
    const TiledAtlas& atlas, UINT subresource,
    const SourceTextureInfo& src, UINT srcMip ) {

    if ( srcMip >= src.sourceMipLevels )
        return; // Source texture doesn't have this mip level

    UINT srcSub = D3D11CalcSubresource( srcMip, 0, src.sourceMipLevels );

    // Destination offset within the atlas subresource at this mip level
    UINT mipX = src.x >> srcMip;
    UINT mipY = src.y >> srcMip;

    // Source region dimensions at this mip level.
    // Using an explicit D3D11_BOX ensures non-uniform texture sizes are handled
    // correctly — for packed atlas contents src.width/height may differ per entry,
    // and a nullptr pSrcBox would copy the full source extent to an unintended region.
    UINT mipW = std::max<UINT>( 1u, src.width  >> srcMip );
    UINT mipH = std::max<UINT>( 1u, src.height >> srcMip );

    D3D11_BOX srcBox = {};
    srcBox.left   = 0;
    srcBox.top    = 0;
    srcBox.front  = 0;
    srcBox.right  = mipW;
    srcBox.bottom = mipH;
    srcBox.back   = 1;

    // All tiles covering (mipX, mipY, mipW, mipH) must already be mapped to real
    // pool tiles before this call — writes to unmapped tiles are silently discarded.
    m_Context->CopySubresourceRegion(
        atlas.texture.Get(), subresource,
        mipX, mipY, 0,
        src.texture.Get(), srcSub,
        &srcBox
    );
}

// =============================================================================
// Missing Mip Generation
// =============================================================================

void D3D11StreamingResourcesManager::GenerateMissingMips(
    const TiledAtlas& atlas, const SourceTextureInfo& src,
    DXGI_FORMAT fmt, UINT srcIndex,
    const std::vector<D3D11_SUBRESOURCE_TILING>& tilings,
    UINT numSubresourceTilings,
    const D3D11_TILE_SHAPE& tileShape ) {

    if ( src.sourceMipLevels >= atlas.mipLevels )
        return; // No missing mips

    // Capture the source texture to CPU memory (creates an internal staging copy)
    DirectX::ScratchImage captured;
    if ( FAILED( DirectX::CaptureTexture( m_Device.Get(), m_Context.Get(), src.texture.Get(), captured ) ) ) {
        LogWarn() << "[StreamingResources] CaptureTexture failed for source " << srcIndex;
        return;
    }

    // Grab the last available mip as the downsampling base
    const DirectX::Image* lastMipImg = captured.GetImage( src.sourceMipLevels - 1, 0, 0 );
    if ( !lastMipImg ) return;

    // GenerateMipMaps requires uncompressed input — decompress BC textures first
    DirectX::ScratchImage decompressed;
    const DirectX::Image* baseImg = lastMipImg;
    if ( DirectX::IsCompressed( lastMipImg->format ) ) {
        if ( FAILED( DirectX::Decompress( *lastMipImg, DXGI_FORMAT_R8G8B8A8_UNORM, decompressed ) ) ) {
            LogWarn() << "[StreamingResources] Decompress failed for source " << srcIndex;
            return;
        }
        baseImg = decompressed.GetImage( 0, 0, 0 );
    }

    // Generate: level 0 = base (already copied to atlas), levels 1..N = the missing mips
    UINT levelsToGen = atlas.mipLevels - src.sourceMipLevels + 1;
    DirectX::ScratchImage mipChain;
    if ( FAILED( DirectX::GenerateMipMaps( *baseImg, DirectX::TEX_FILTER_BOX, levelsToGen, mipChain ) ) ) {
        LogWarn() << "[StreamingResources] GenerateMipMaps failed for source " << srcIndex;
        return;
    }

    // Re-compress the generated levels back to the atlas BC format.
    // Try GPU-accelerated compression first; fall back to CPU if unsupported.
    const DirectX::ScratchImage* finalChain = &mipChain;
    DirectX::ScratchImage recompressed;
    if ( DirectX::IsCompressed( fmt ) ) {
        HRESULT hr = DirectX::Compress( m_Device.Get(),
            mipChain.GetImages(), mipChain.GetImageCount(), mipChain.GetMetadata(),
            fmt, DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_ALPHA_WEIGHT_DEFAULT,
            recompressed );
        if ( FAILED( hr ) ) {
            // GPU BC compression not supported — use CPU path
            recompressed = DirectX::ScratchImage{};
            if ( FAILED( DirectX::Compress(
                mipChain.GetImages(), mipChain.GetImageCount(), mipChain.GetMetadata(),
                fmt, DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_ALPHA_WEIGHT_DEFAULT,
                recompressed ) ) ) {
                LogWarn() << "[StreamingResources] Compress failed for source " << srcIndex;
                return;
            }
        }
        finalChain = &recompressed;
    }

    // Upload each generated mip level
    for ( UINT mip = src.sourceMipLevels; mip < atlas.mipLevels; ++mip ) {
        // chainIdx 0 = the base (already in atlas), so generated levels start at 1
        UINT chainIdx = mip - src.sourceMipLevels + 1;
        const DirectX::Image* genImg = finalChain->GetImage( chainIdx, 0, 0 );
        if ( !genImg || !genImg->pixels ) continue;

        UINT dstSub = D3D11CalcSubresource( mip, src.slice, atlas.mipLevels );
        if ( dstSub >= numSubresourceTilings )
            continue;

        // Map tiles before uploading
        const auto& tiling = tilings[dstSub];
        if ( tiling.WidthInTiles > 0 && tiling.HeightInTiles > 0 ) {
            UINT regionX = src.x >> mip;
            UINT regionY = src.y >> mip;
            UINT regionW = std::max<UINT>( 1u, src.width  >> mip );
            UINT regionH = std::max<UINT>( 1u, src.height >> mip );

            UINT tw = tileShape.WidthInTexels;
            UINT th = tileShape.HeightInTexels;

            UINT tileStartX = regionX / tw;
            UINT tileStartY = regionY / th;
            UINT tileEndX   = std::min<UINT>( tiling.WidthInTiles  - 1, ( regionX + regionW - 1 ) / tw );
            UINT tileEndY   = std::min<UINT>( tiling.HeightInTiles - 1, ( regionY + regionH - 1 ) / th );

            for ( UINT ty = tileStartY; ty <= tileEndY; ++ty ) {
                for ( UINT tx = tileStartX; tx <= tileEndX; ++tx ) {
                    uint64_t tileKey = MakeTileKey( fmt, dstSub, tx, ty );
                    auto stateIt = m_TileStates.find( tileKey );
                    if ( stateIt != m_TileStates.end() &&
                         stateIt->second.state == TileState::Resident )
                        continue;

                    UINT poolTile = AllocateTile( fmt );
                    if ( poolTile == UINT_MAX )
                        continue;

                    MapTileToPool( atlas, dstSub, tx, ty, poolTile, fmt );

                    TileInfo info;
                    info.state         = TileState::Resident;
                    info.poolTileIndex = poolTile;
                    info.subresource   = dstSub;
                    info.tileX         = tx;
                    info.tileY         = ty;
                    info.format        = fmt;
                    info.lastUsedTime  = 0.0f;
                    m_TileStates[tileKey] = info;
                }
            }
        }

        // Create temporary immutable texture and copy to atlas
        D3D11_TEXTURE2D_DESC tmpDesc = {};
        tmpDesc.Width            = static_cast<UINT>( genImg->width );
        tmpDesc.Height           = static_cast<UINT>( genImg->height );
        tmpDesc.MipLevels        = 1;
        tmpDesc.ArraySize        = 1;
        tmpDesc.Format           = genImg->format;
        tmpDesc.SampleDesc.Count = 1;
        tmpDesc.Usage            = D3D11_USAGE_IMMUTABLE;
        tmpDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem     = genImg->pixels;
        initData.SysMemPitch = static_cast<UINT>( genImg->rowPitch );

        ComPtr<ID3D11Texture2D> tmpTex;
        if ( SUCCEEDED( m_Device->CreateTexture2D( &tmpDesc, &initData, tmpTex.GetAddressOf() ) ) ) {
            UINT mipX = src.x >> mip;
            UINT mipY = src.y >> mip;
            D3D11_BOX box = { 0, 0, 0,
                             static_cast<UINT>( genImg->width ),
                             static_cast<UINT>( genImg->height ), 1 };
            m_Context->CopySubresourceRegion(
                atlas.texture.Get(), dstSub, mipX, mipY, 0,
                tmpTex.Get(), 0, &box );
        }

        // Mark as loaded
        uint64_t srcKey = MakeSourceKey( fmt, srcIndex, mip );
        m_LoadedSources.insert( srcKey );
    }

    LogInfo() << "[StreamingResources] Generated " << ( atlas.mipLevels - src.sourceMipLevels )
              << " missing mip(s) for source " << srcIndex
              << " (had " << src.sourceMipLevels << ", atlas needs " << atlas.mipLevels << ")";
}

// =============================================================================
// Per-Frame Streaming Update
// =============================================================================

void D3D11StreamingResourcesManager::UpdateStreaming(
    const DirectX::XMFLOAT3& cameraPosition, float drawDistance, float currentTime,
    const std::unordered_set<UINT>* requestedSources ) {

    if ( !m_Initialized )
        return;

    // -------------------------------------------------------------------------
    // The streaming unit is a (source, mip) pair — NOT individual tiles.
    //
    // Multiple non-uniform source textures share 64KB atlas tiles.  The old
    // tile-level approach allocated a fresh pool tile per-source-per-tile,
    // causing the same atlas tile to be re-mapped to different pool memory
    // each frame (frame-flickering) and leaving newly-allocated tiles filled
    // with uninitialized zeros (black).
    //
    // The correct flow is:
    //   1. Decide which (source, mip) pairs need loading.
    //   2. For each, ensure ALL atlas tiles that the source covers are mapped
    //      to real pool tiles (allocate only if currently default-mapped).
    //   3. Upload the source mip data via CopySubresourceRegion ONCE.
    //   4. Record the (source, mip) as loaded so we never re-upload it.
    // -------------------------------------------------------------------------

    // --- 1. Build fresh load queue ---
    // Clear any stale requests from previous frames.  The priority_queue has no
    // clear(), so swap with an empty one.
    { decltype( m_LoadQueue ) empty; m_LoadQueue.swap( empty ); }

    for ( auto& [fmt, sources] : m_SourceTextures ) {
        auto atlasIt = m_TiledAtlases.find( fmt );
        if ( atlasIt == m_TiledAtlases.end() )
            continue;

        const TiledAtlas& atlas = atlasIt->second;

        // Query tile layout once per atlas
        UINT numSubresourceTilings = atlas.mipLevels * atlas.arraySlices;
        D3D11_PACKED_MIP_DESC packedDesc = {};
        D3D11_TILE_SHAPE tileShape = {};
        UINT totalTiles = 0;
        {
            std::vector<D3D11_SUBRESOURCE_TILING> tmp( numSubresourceTilings );
            m_Device2->GetResourceTiling(
                atlas.texture.Get(),
                &totalTiles, &packedDesc, &tileShape,
                &numSubresourceTilings, 0, tmp.data()
            );
        }

        UINT standardMips = atlas.mipLevels - packedDesc.NumPackedMips;

        // Determine the preloaded mip range so we skip those
        UINT preloadStart = ( standardMips > PRELOADED_COARSE_MIPS )
                          ? standardMips - PRELOADED_COARSE_MIPS
                          : 0;

        for ( UINT srcIdx = 0; srcIdx < static_cast<UINT>( sources.size() ); ++srcIdx ) {
            const auto& src = sources[srcIdx];

            // Feedback-driven filtering: if requestedSources is provided, only stream
            // sources that the GPU reported as needing data. Otherwise load everything.
            if ( requestedSources ) {
                UINT globalIdx = m_GlobalSourceOffsets[fmt] + srcIdx;
                if ( requestedSources->count( globalIdx ) == 0 )
                    continue; // Not requested by GPU feedback — skip
            }

            for ( UINT mip = 0; mip < standardMips; ++mip ) {
                // Skip mips that were preloaded at atlas creation
                if ( mip >= preloadStart )
                    continue;

                // Skip if the source doesn't have this mip — generated mips
                // are handled by GenerateMissingMips during PreloadCoarseMips.
                // For mips that were already generated, m_LoadedSources will
                // short-circuit below.
                if ( mip >= src.sourceMipLevels )
                    continue;

                // Skip if already uploaded
                uint64_t srcKey = MakeSourceKey( fmt, srcIdx, mip );
                if ( m_LoadedSources.count( srcKey ) )
                    continue;

                // Priority: coarser mips first (higher priority number)
                float priority = static_cast<float>( atlas.mipLevels - mip );

                StreamingRequest req;
                req.sourceIndex = srcIdx;
                req.priority    = priority;
                req.mipLevel    = mip;
                req.format      = fmt;
                m_LoadQueue.push( req );
            }
        }
    }

    // --- 2. Process load queue (no per-frame cap) ---
    // All visible source-mips are loaded immediately. A per-frame budget previously
    // caused multi-frame pop-in and invisible alpha-tested geometry (BC2/BC3 default
    // tiles are transparent). The cost is bounded by the number of newly-visible
    // sources, which is typically small after the initial load.
    while ( !m_LoadQueue.empty() ) {
        StreamingRequest req = m_LoadQueue.top();
        m_LoadQueue.pop();

        // Double-check: may have been loaded by a higher-priority path
        uint64_t srcKey = MakeSourceKey( req.format, req.sourceIndex, req.mipLevel );
        if ( m_LoadedSources.count( srcKey ) )
            continue;

        auto atlasIt = m_TiledAtlases.find( req.format );
        if ( atlasIt == m_TiledAtlases.end() )
            continue;

        auto srcIt = m_SourceTextures.find( req.format );
        if ( srcIt == m_SourceTextures.end() || req.sourceIndex >= srcIt->second.size() )
            continue;

        const TiledAtlas& atlas = atlasIt->second;
        const SourceTextureInfo& src = srcIt->second[req.sourceIndex];

        UINT dstSub = D3D11CalcSubresource( req.mipLevel, src.slice, atlas.mipLevels );

        // Query tiling for tile shape
        UINT numSubresourceTilings = atlas.mipLevels * atlas.arraySlices;
        std::vector<D3D11_SUBRESOURCE_TILING> tilings( numSubresourceTilings );
        UINT totalTiles = 0;
        D3D11_PACKED_MIP_DESC packedDesc = {};
        D3D11_TILE_SHAPE tileShape = {};
        m_Device2->GetResourceTiling(
            atlas.texture.Get(),
            &totalTiles, &packedDesc, &tileShape,
            &numSubresourceTilings, 0, tilings.data()
        );

        if ( dstSub >= numSubresourceTilings )
            continue;

        const auto& tiling = tilings[dstSub];
        if ( tiling.WidthInTiles == 0 || tiling.HeightInTiles == 0 )
            continue;

        // Compute tile range this source covers at this mip
        UINT regionX = src.x >> req.mipLevel;
        UINT regionY = src.y >> req.mipLevel;
        UINT regionW = std::max<UINT>( 1u, src.width  >> req.mipLevel );
        UINT regionH = std::max<UINT>( 1u, src.height >> req.mipLevel );
        UINT tileW = tileShape.WidthInTexels;
        UINT tileH = tileShape.HeightInTexels;

        UINT tileStartX = regionX / tileW;
        UINT tileStartY = regionY / tileH;
        UINT tileEndX   = std::min<UINT>( tiling.WidthInTiles  - 1, ( regionX + regionW - 1 ) / tileW );
        UINT tileEndY   = std::min<UINT>( tiling.HeightInTiles - 1, ( regionY + regionH - 1 ) / tileH );

        // Ensure all covered tiles are mapped to real pool tiles.
        // Tiles may already be resident from another source that shares them — skip those.
        for ( UINT ty = tileStartY; ty <= tileEndY; ++ty ) {
            for ( UINT tx = tileStartX; tx <= tileEndX; ++tx ) {
                uint64_t tileKey = MakeTileKey( req.format, dstSub, tx, ty );
                auto stateIt = m_TileStates.find( tileKey );
                if ( stateIt != m_TileStates.end() &&
                     stateIt->second.state == TileState::Resident ) {
                    // Tile already resident — still record our source key so eviction
                    // of this shared tile invalidates all dependent sources.
                    stateIt->second.sourceKeys.push_back( srcKey );
                    continue;
                }

                UINT poolTile = AllocateTile( req.format );
                if ( poolTile == UINT_MAX )
                    continue; // Pool exhausted — leave on default

                MapTileToPool( atlas, dstSub, tx, ty, poolTile, req.format );

                TileInfo info;
                info.state         = TileState::Resident;
                info.poolTileIndex = poolTile;
                info.subresource   = dstSub;
                info.tileX         = tx;
                info.tileY         = ty;
                info.format        = req.format;
                info.lastUsedTime  = currentTime;
                info.sourceKeys.push_back( srcKey );
                m_TileStates[tileKey] = info;
            }
        }

        // Upload source data once (all tiles are now mapped)
        UploadTileData( atlas, dstSub, src, req.mipLevel );

        m_LoadedSources.insert( srcKey );

        // Touch all tiles this source covers so they won't be unloaded
        for ( UINT ty = tileStartY; ty <= tileEndY; ++ty ) {
            for ( UINT tx = tileStartX; tx <= tileEndX; ++tx ) {
                uint64_t tileKey = MakeTileKey( req.format, dstSub, tx, ty );
                auto stateIt = m_TileStates.find( tileKey );
                if ( stateIt != m_TileStates.end() ) {
                    stateIt->second.lastUsedTime = currentTime;
                }
            }
        }

    }

    // --- 3. Touch resident tiles based on feedback ---
    // When feedback-driven: only touch tiles belonging to requested sources.
    // When no feedback (requestedSources == nullptr): touch everything (legacy behavior).
    if ( requestedSources ) {
        // Build a set of (format, srcIdx) pairs that are requested
        for ( auto& [fmt, sources] : m_SourceTextures ) {
            auto atlasIt = m_TiledAtlases.find( fmt );
            if ( atlasIt == m_TiledAtlases.end() )
                continue;

            const TiledAtlas& atlas = atlasIt->second;

            for ( UINT srcIdx = 0; srcIdx < static_cast<UINT>( sources.size() ); ++srcIdx ) {
                UINT globalIdx = m_GlobalSourceOffsets[fmt] + srcIdx;
                if ( requestedSources->count( globalIdx ) == 0 )
                    continue; // Not visible — don't touch, let it age out

                const auto& src = sources[srcIdx];

                // Touch all tiles this source covers across all loaded mips
                UINT numSub = atlas.mipLevels * atlas.arraySlices;
                std::vector<D3D11_SUBRESOURCE_TILING> tilings( numSub );
                UINT tt = 0;
                D3D11_PACKED_MIP_DESC pd = {};
                D3D11_TILE_SHAPE ts = {};
                m_Device2->GetResourceTiling(
                    atlas.texture.Get(),
                    &tt, &pd, &ts, &numSub, 0, tilings.data() );

                UINT standardMips = atlas.mipLevels - pd.NumPackedMips;
                for ( UINT mip = 0; mip < standardMips; ++mip ) {
                    UINT dstSub = D3D11CalcSubresource( mip, src.slice, atlas.mipLevels );
                    if ( dstSub >= numSub ) continue;

                    UINT regionX = src.x >> mip;
                    UINT regionY = src.y >> mip;
                    UINT regionW = std::max<UINT>( 1u, src.width  >> mip );
                    UINT regionH = std::max<UINT>( 1u, src.height >> mip );
                    UINT tileW = ts.WidthInTexels;
                    UINT tileH = ts.HeightInTexels;
                    if ( tileW == 0 || tileH == 0 ) continue;

                    UINT tileStartX = regionX / tileW;
                    UINT tileStartY = regionY / tileH;
                    UINT tileEndX = std::min<UINT>( tilings[dstSub].WidthInTiles > 0 ? tilings[dstSub].WidthInTiles - 1 : 0,
                                                    ( regionX + regionW - 1 ) / tileW );
                    UINT tileEndY = std::min<UINT>( tilings[dstSub].HeightInTiles > 0 ? tilings[dstSub].HeightInTiles - 1 : 0,
                                                    ( regionY + regionH - 1 ) / tileH );

                    for ( UINT ty = tileStartY; ty <= tileEndY; ++ty ) {
                        for ( UINT tx = tileStartX; tx <= tileEndX; ++tx ) {
                            uint64_t tileKey = MakeTileKey( fmt, dstSub, tx, ty );
                            auto stateIt = m_TileStates.find( tileKey );
                            if ( stateIt != m_TileStates.end() && stateIt->second.state == TileState::Resident ) {
                                stateIt->second.lastUsedTime = currentTime;
                            }
                        }
                    }
                }
            }
        }
    } else {
        // No feedback — touch everything (backward compatibility)
        for ( auto& [key, tile] : m_TileStates ) {
            if ( tile.state == TileState::Resident ) {
                tile.lastUsedTime = currentTime;
            }
        }
    }

    // --- 4. Identify unload candidates ---
    // (Currently all resident tiles are touched every frame above, so nothing
    //  will be unloaded. This section is kept for future distance-based eviction
    //  where step 3 would only touch tiles near the camera.)
    m_UnloadCandidates.clear();
    for ( auto& [key, tile] : m_TileStates ) {
        if ( tile.state == TileState::Resident &&
             ( currentTime - tile.lastUsedTime ) > UNLOAD_COOLDOWN_SECONDS ) {

            // Don't unload preloaded coarse mips
            auto atlasIt = m_TiledAtlases.find( tile.format );
            if ( atlasIt != m_TiledAtlases.end() ) {
                UINT mipLevels = atlasIt->second.mipLevels;
                UINT mip = tile.subresource % mipLevels;

                // Query tile layout once per atlas
                UINT numSubresourceTilings = atlasIt->second.mipLevels * atlasIt->second.arraySlices;
                D3D11_PACKED_MIP_DESC packedDesc = {};
                D3D11_TILE_SHAPE tileShape = {};
                UINT totalTiles = 0;
                {
                    std::vector<D3D11_SUBRESOURCE_TILING> tmp( numSubresourceTilings );
                    m_Device2->GetResourceTiling(
                        atlasIt->second.texture.Get(),
                        &totalTiles, &packedDesc, &tileShape,
                        &numSubresourceTilings, 0, tmp.data()
                    );
                }

                UINT standardMips = mipLevels - packedDesc.NumPackedMips;
                UINT preloadStart = ( standardMips > PRELOADED_COARSE_MIPS )
                                  ? standardMips - PRELOADED_COARSE_MIPS : 0;
                if ( mip >= preloadStart )
                    continue; // Don't unload preloaded coarse mips
            }

            tile.state = TileState::PendingUnload;
            m_UnloadCandidates.push_back( key );
        }
    }

    // --- 5. Process unload queue (frame-budgeted) ---
    UINT unmapsThisFrame = 0;
    for ( auto key : m_UnloadCandidates ) {
        if ( unmapsThisFrame >= MAX_TILE_UNMAPS_PER_FRAME )
            break;

        auto stateIt = m_TileStates.find( key );
        if ( stateIt == m_TileStates.end() )
            continue;

        TileInfo& tile = stateIt->second;
        if ( tile.state != TileState::PendingUnload )
            continue;

        auto atlasIt = m_TiledAtlases.find( tile.format );
        if ( atlasIt == m_TiledAtlases.end() )
            continue;

        // Remap to default tile
        MapTileToDefault( atlasIt->second, tile.subresource,
                          tile.tileX, tile.tileY, tile.format );

        // Free the pool tile
        FreeTile( tile.format, tile.poolTileIndex );

        tile.state = TileState::Unmapped;
        tile.poolTileIndex = 0;
        unmapsThisFrame++;

        // Remove m_LoadedSources entries that depended on this tile so the
        // source will be re-uploaded if it becomes visible again.
        for ( uint64_t sk : tile.sourceKeys ) {
            m_LoadedSources.erase( sk );
        }
        tile.sourceKeys.clear();
    }
}
