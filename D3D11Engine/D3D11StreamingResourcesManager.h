#pragma once

#include "pch.h"
#include <wrl/client.h>
#include <vector>
#include <array>
#include <unordered_map>
#include <queue>
#include <unordered_set>
#include "D3D11TextureAtlasManager.h"

// Forward declarations
class zCTexture;

/**
 * Streaming resources manager using D3D11 Tiled Resources (Reserved Resources).
 *
 * Creates Texture2DArray atlases backed by tile pools instead of fully-committed
 * GPU memory. Tiles are streamed in/out based on camera proximity and screen-space
 * priority, keeping memory footprint bounded even with large texture sets.
 *
 * All unmapped tiles point to a single default tile (magenta debug fill), so the
 * SRV is always valid and shaders never sample garbage data.
 *
 * Coarsest mip levels are preloaded immediately so objects always display at least
 * a low-resolution texture.
 */
class D3D11StreamingResourcesManager {
public:
    D3D11StreamingResourcesManager() = default;
    ~D3D11StreamingResourcesManager();

    // Public struct: source texture info (needed by graphics engine for feedback lookup)
    struct SourceTextureInfo {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        UINT x, y, slice;
        UINT width, height;
        UINT sourceMipLevels;
    };

    // --- Capability query (static, can be called before Init) ---
    static bool GetIsStreamingSupported( ID3D11Device1* device );

    // --- Lifecycle ---
    bool Init( ID3D11Device1* device, ID3D11DeviceContext1* context );
    void Shutdown();

    // --- Atlas creation ---
    // Creates a tiled Texture2DArray with the same bin-packing layout as
    // TextureManager::CreateAtlasArray, but backed by tile pools.
    // Returns a compatible AtlasResult so the rest of the pipeline is unchanged.
    TextureManager::AtlasResult CreateStreamingAtlasArray(
        std::basic_string_view<ID3D11Texture2D*> sourceTextures,
        UINT atlasSize = 2048, UINT mipLevels = 6 );

    // --- Per-frame streaming update ---
    // Called once per frame before draw calls. Evaluates tile priorities,
    // streams in/out tiles within the per-frame budget.
    // If requestedSources is non-null, only sources in the set are loaded (feedback-driven).
    // If null, all sources are loaded (backward compatibility / non-streaming fallback).
    void UpdateStreaming( const DirectX::XMFLOAT3& cameraPosition, float drawDistance, float currentTime,
                          const std::unordered_set<UINT>* requestedSources = nullptr );

    // --- Feedback query methods ---
    // Global source offset for a given format (cumulative count of sources in prior formats).
    UINT GetGlobalSourceOffset( DXGI_FORMAT fmt ) const;
    // Total number of sources across all formats.
    UINT GetTotalSourceCount() const;

    // Source texture list for a given format (for populating SubmeshGPUData.globalSourceIndex).
    const std::vector<SourceTextureInfo>& GetSourceTextures( DXGI_FORMAT fmt ) const;

    // --- World lifecycle ---
    void OnWorldUnloaded();

private:
    // --- Device interfaces (QueryInterface'd from ID3D11Device1/Context1) ---
    Microsoft::WRL::ComPtr<ID3D11Device2>        m_Device2;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext2>  m_Context2;

    // Keep a ref to ID3D11Device for non-tiled operations (staging textures etc.)
    Microsoft::WRL::ComPtr<ID3D11Device1>         m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1>   m_Context;

    bool m_Initialized = false;

    // =========================================================================
    // Tile Pool
    // =========================================================================
    // One tile pool per DXGI_FORMAT group (mirrors the atlas grouping).
    struct TilePool {
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer; // D3D11_RESOURCE_MISC_TILE_POOL
        UINT totalTiles  = 0; // capacity in 64KB tiles
        UINT usedTiles   = 0; // high-water allocation mark
        std::vector<UINT> freeTiles; // recycled tile indices (LIFO stack)
    };
    std::unordered_map<DXGI_FORMAT, TilePool> m_TilePools;

    // =========================================================================
    // Tiled Atlas Textures
    // =========================================================================
    struct TiledAtlas {
        Microsoft::WRL::ComPtr<ID3D11Texture2D>          texture; // D3D11_RESOURCE_MISC_TILED
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        UINT        atlasSize   = 0;
        UINT        mipLevels   = 0;
        UINT        arraySlices = 0;
        DXGI_FORMAT format      = DXGI_FORMAT_UNKNOWN;
    };
    std::unordered_map<DXGI_FORMAT, TiledAtlas> m_TiledAtlases;

    // =========================================================================
    // Default (null) Tile
    // =========================================================================
    // A single 64KB tile filled with magenta (1,0,1,1), mapped to all unmapped
    // tile regions so shaders always read valid data.
    struct DefaultTile {
        UINT poolIndex   = 0;
        bool initialized = false;
    };
    std::unordered_map<DXGI_FORMAT, DefaultTile> m_DefaultTiles;

    // =========================================================================
    // Tile State Tracking
    // =========================================================================
    enum class TileState : uint8_t {
        Unmapped,     // mapped to default magenta tile
        PendingLoad,  // queued for streaming in
        Resident,     // fully loaded with real data
        PendingUnload // cooldown timer running before unmap
    };

    struct TileInfo {
        UINT       poolTileIndex = 0;
        TileState  state         = TileState::Unmapped;
        float      lastUsedTime  = 0.0f;
        float      priority      = 0.0f;
        UINT       subresource   = 0;
        UINT       tileX         = 0;
        UINT       tileY         = 0;
        DXGI_FORMAT format       = DXGI_FORMAT_UNKNOWN;
        // Source-mip keys that depend on this tile. When evicted, these are
        // erased from m_LoadedSources so re-streaming can happen.
        std::vector<uint64_t> sourceKeys;
    };
    // key = MakeTileKey(format, subresource, tileX, tileY)
    std::unordered_map<uint64_t, TileInfo> m_TileStates;

    // =========================================================================
    // Streaming Request Queue
    // =========================================================================
    // Streaming operates at the source+mip granularity, NOT individual tiles.
    // Multiple non-uniform sources may share the same atlas tile; tile-level
    // streaming caused tiles to be re-mapped and overwritten each frame as
    // different sources "claimed" the shared tile, producing frame-flickering
    // and eventual black (uninitialized pool memory).
    struct StreamingRequest {
        UINT     sourceIndex = 0;  // index into m_SourceTextures[format]
        float    priority    = 0.0f;
        UINT     mipLevel    = 0;
        DXGI_FORMAT format   = DXGI_FORMAT_UNKNOWN;

        bool operator<( const StreamingRequest& other ) const {
            return priority < other.priority; // max-heap: highest priority first
        }
    };
    std::priority_queue<StreamingRequest> m_LoadQueue;
    std::vector<uint64_t> m_UnloadCandidates;

    // Tracks which source+mip combos have been fully uploaded.
    // Key = MakeSourceKey(format, sourceIndex, mip)
    std::unordered_set<uint64_t> m_LoadedSources;

    // =========================================================================
    // Source Texture References (for uploading tile data)
    // =========================================================================
    // Maps (format, PackItem index) to the source texture so we can read tile
    // data when streaming in. Populated during CreateStreamingAtlasArray.
    // (SourceTextureInfo is declared in the public section above.)
    // Key: format -> vector of source textures (matches atlas packing order)
    std::unordered_map<DXGI_FORMAT, std::vector<SourceTextureInfo>> m_SourceTextures;

    // =========================================================================
    // Global Source Offsets (for feedback texture indexing)
    // =========================================================================
    // Maps each format to its cumulative offset in the flat global source array.
    // Computed during CreateStreamingAtlasArray.
    std::unordered_map<DXGI_FORMAT, UINT> m_GlobalSourceOffsets;
    UINT m_TotalSourceCount = 0;

    // =========================================================================
    // Staging Ring Buffer
    // =========================================================================
    static constexpr UINT STAGING_RING_SIZE = 8;
    struct StagingBuffer {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        bool inUse = false;
    };
    std::array<StagingBuffer, STAGING_RING_SIZE> m_StagingRing{};
    UINT m_StagingRingHead = 0;

    // =========================================================================
    // Budget & Tuning Constants
    // =========================================================================
    static constexpr UINT  TILE_SIZE_BYTES              = 65536;  // 64KB per tile
    static constexpr UINT  MAX_TILE_UPLOADS_PER_FRAME    = 8;  // tiles uploaded per frame in UpdateStreaming
    static constexpr UINT  MAX_TILE_UNMAPS_PER_FRAME     = 4;
    static constexpr float UNLOAD_COOLDOWN_SECONDS    = 5.0f;
    static constexpr UINT  INITIAL_POOL_TILES         = 512;    // 32 MB per pool
    static constexpr UINT  PRELOADED_COARSE_MIPS      = 1;     // preload ALL standard mips at creation

    // =========================================================================
    // Internal Helpers
    // =========================================================================

    // Tile pool management
    UINT AllocateTile( DXGI_FORMAT fmt );
    void FreeTile( DXGI_FORMAT fmt, UINT tileIndex );
    bool CreateTilePool( DXGI_FORMAT fmt, UINT numTiles );
    void GrowTilePool( DXGI_FORMAT fmt, UINT additionalTiles );

    // Default tile
    void InitDefaultTile( DXGI_FORMAT fmt );
    void FillDefaultTileData( DXGI_FORMAT fmt, std::vector<uint8_t>& outData );

    // Tile mapping
    void MapTileToDefault( const TiledAtlas& atlas, UINT subresource,
                           UINT tileX, UINT tileY, DXGI_FORMAT fmt );
    void MapTileToPool( const TiledAtlas& atlas, UINT subresource,
                        UINT tileX, UINT tileY, UINT poolTileIndex,
                        DXGI_FORMAT fmt );
    void MapAllTilesToDefault( const TiledAtlas& atlas, DXGI_FORMAT fmt );

    // Upload one source texture's mip region into the tiled atlas.
    // All tiles covering the source region must be mapped to pool tiles before calling.
    // Uses a bounded D3D11_BOX so non-uniform texture sizes are handled correctly.
    void UploadTileData( const TiledAtlas& atlas, UINT subresource,
                         const SourceTextureInfo& src, UINT srcMip );

    // Generate missing mip levels for a source texture that has fewer mips than
    // the atlas requires.  Uses DirectXTex to capture the source's last mip,
    // generate a proper box-filtered mip chain, re-compress to the atlas format,
    // and upload each generated level.  Tiles are mapped before upload.
    void GenerateMissingMips( const TiledAtlas& atlas, const SourceTextureInfo& src,
                              DXGI_FORMAT fmt, UINT srcIndex,
                              const std::vector<D3D11_SUBRESOURCE_TILING>& tilings,
                              UINT numSubresourceTilings,
                              const D3D11_TILE_SHAPE& tileShape );

    // Key generation
    static uint64_t MakeTileKey( DXGI_FORMAT fmt, UINT subresource,
                                 UINT tileX, UINT tileY );
    static uint64_t MakeSourceKey( DXGI_FORMAT fmt, UINT sourceIndex, UINT mip );

    // Preload coarsest mip levels (called from CreateStreamingAtlasArray)
    void PreloadCoarseMips( DXGI_FORMAT fmt );

    // Get number of tiles in a subresource dimension
    void GetSubresourceTileCount( const TiledAtlas& atlas, UINT mipLevel,
                                  UINT& tilesX, UINT& tilesY ) const;
};
