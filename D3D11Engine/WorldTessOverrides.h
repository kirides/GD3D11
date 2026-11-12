#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <DirectXMath.h>

class zCTexture;
class zCMaterial;
struct WorldMeshInfo;
struct WorldMeshSectionInfo;

/** Bytes per wrapped vertex in the welded tessellation stream; Shaders/D3D12/include/WorldTess.hlsl mirrors it. */
constexpr uint32_t kWorldTessStreamStride = 20;

/** Per world mesh (one section x one material) tessellation overrides. Defaults reproduce the global behaviour. */
struct WorldTessParams {
    float Roundness = 1.0f;     // 0..1, scales the PN smoothing
    float Factor = 1.0f;        // 0..2, scales the edge factor (0 = never subdivided)
    float Displacement = 1.0f;  // 0..4, scales the height-map amplitude (0 = flat)
    float Reserved = 0.0f;

    bool IsDefault() const { return Roundness == 1.0f && Factor == 1.0f && Displacement == 1.0f && Reserved == 0.0f; }
};
static_assert( sizeof( WorldTessParams ) == 16 );

/** What the converter knows about one world mesh, in table-index order. */
struct WorldTessMeshRef {
    WorldMeshInfo* Mesh = nullptr;
    std::string Key;   // texture name without extension (material name without texture), taken at build time
    zCMaterial* Material = nullptr;
    int SectionX = 0;
    int SectionY = 0;
    float UvDensity = 0.0f;   // mean UV units per world unit along the mesh's edges; picks the height-map LOD
};

/** Owner table behind the welded tessellation stream: meshes [0, meshCount), then boundary entries (positions shared
    by 2+ meshes, value = per-field min over their meshes), so every vertex at one position reads the same value. */
class WorldTessOverrides {
public:
    /** Called by the world converter. boundaryOffsets/boundaryMeshes is CSR: boundary b owns meshes [off[b], off[b+1]). */
    void Build( std::vector<WorldTessMeshRef>&& meshes, const std::vector<uint32_t>& boundaryOffsets, const std::vector<uint32_t>& boundaryMeshes,
        const std::vector<uint32_t>& boundaryCanonical, const std::vector<uint8_t>& boundaryPinned );

    /** Drops everything (world reset). */
    void Reset();

    uint32_t GetMeshCount() const { return static_cast<uint32_t>( m_Meshes.size() ); }
    uint32_t GetBoundaryCount() const { return m_BoundaryOffsets.empty() ? 0 : static_cast<uint32_t>( m_BoundaryOffsets.size() - 1 ); }

    /** Canonical (dominant-vertex) mesh of a boundary entry; boundary is relative to the boundary range (table index - meshCount). UINT32_MAX if out of range. */
    uint32_t GetBoundaryCanonicalMesh( uint32_t boundary ) const { return boundary < m_BoundaryCanonical.size() ? m_BoundaryCanonical[boundary] : UINT32_MAX; }

    WorldTessParams Get( uint32_t mesh ) const;
    void Set( uint32_t mesh, const WorldTessParams& p );
    /** Every mesh in every section that uses this texture. */
    void SetForTexture( zCTexture* texture, const WorldTessParams& p );
    void ResetMesh( uint32_t mesh );

    /** Section coords and persistence key (texture name without extension, else material name) of a mesh. */
    bool GetMeshInfo( uint32_t mesh, int& sectionX, int& sectionY, std::string& key ) const;

    /** The mesh's texture, else its material's single texture, else null. */
    /** Read live from the material like the renderer does: a cached zCTexture* can be freed (savegame loads). */
    zCTexture* GetMeshTexture( uint32_t mesh ) const;
    zCMaterial* GetMeshMaterial( uint32_t mesh ) const { return mesh < m_Meshes.size() ? m_Meshes[mesh].Material : nullptr; }
    float GetMeshUvDensity( uint32_t mesh ) const { return mesh < m_Meshes.size() ? m_Meshes[mesh].UvDensity : 0.0f; }

    uint32_t GetVersion() const { return m_Version; }
    /** Meshes [0, meshCount) then boundaries; .x Roundness .y Factor .z Displacement (boundaries: the canonical mesh's) .w 0 (backend writes it). Empty without a world. */
    const std::vector<DirectX::XMFLOAT4>& GetGpuTable();

    /** Unsaved edits since the last load/save. */
    bool IsDirty() const { return m_Dirty; }

    /** `<zen>.wts`; Load resolves keys against the built meshes, unknown lines survive for the next Save. */
    bool Load( const std::string& file );
    bool Save( const std::string& file );

private:
    void Touch() { ++m_Version; m_Dirty = true; m_TableVersion = ~0u; }

    std::vector<WorldTessMeshRef> m_Meshes;
    std::vector<WorldTessParams> m_Params;          // parallel to m_Meshes
    std::vector<uint32_t> m_BoundaryOffsets;        // CSR, boundaryCount + 1 entries (empty without boundaries)
    std::vector<uint32_t> m_BoundaryCanonical;      // canonical mesh per boundary entry
    std::vector<uint8_t> m_BoundaryPinned;          // open-edge pinned entries: flat (roundness 0, displacement 0)
    std::vector<uint16_t> m_BoundaryMeshes16;      // used while meshCount <= 65535
    std::vector<uint32_t> m_BoundaryMeshes32;
    std::vector<std::string> m_UnknownLines;        // .wts lines that matched no mesh, rewritten on save

    std::vector<DirectX::XMFLOAT4> m_Table;
    uint32_t m_TableVersion = ~0u;
    uint32_t m_Version = 0;
    bool m_Dirty = false;
};

WorldTessOverrides& GetWorldTessOverrides();
