#pragma once
#include "D3D11AtlasTypes.h"
#include "D3D11StructuredBuffer.h"
#include "D3D11VertexBuffer.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class D3D11GraphicsEngine;
class D3D11Texture;
class zCTexture;
struct MeshInfo;

/**
 * Encapsulates all texture-atlas-based GPU-driven rendering for world mesh geometry.
 *
 * Responsibilities:
 *  - Building per-format Texture2DArray atlases for world-mesh diffuse, normal, and FX textures
 *  - Building the merged global world-mesh VB/IB with per-submesh indirect-args buffers
 *  - Executing the multi-indirect draw of atlased world mesh geometry each frame
 *
 * The engine keeps one instance of this class. Call Build() when a new world
 * is loaded (OnWorldLoaded), and Draw() every frame instead of the old
 * DrawWorldMesh_Atlas().
 */
class D3D11MeshAtlasPass {
    friend class D3D11GraphicsEngine;
public:
    explicit D3D11MeshAtlasPass( D3D11GraphicsEngine* engine );

    /** (Re-)build atlases and geometry buffers.
     *  Called from D3D11GraphicsEngine::OnWorldLoaded(). */
    void Build();

    /** Draw atlased world mesh geometry via multi-indirect. */
    XRESULT Draw();

    /** True once Build() has completed and at least one draw group exists. */
    bool IsReady() const { return !m_WorldMeshAtlasDrawGroups.empty(); }

    /** Returns true if the given MeshInfo was atlased (used to skip it in the legacy path). */
    bool IsSubmeshAtlased( MeshInfo* mi ) const {
        return m_WorldMeshAtlasedSubmeshes.count( mi ) != 0;
    }

    /** Diffuse atlas lookup (read-only access for shadow passes). */
    const std::unordered_map<zCTexture*, TextureAtlasLookup>& GetDiffuseAtlasLookup() const {
        return m_WorldMeshDiffuseAtlasLookup;
    }

private:
    D3D11GraphicsEngine* m_Engine;

    // ---- Atlas textures (one array per texture type) ----
    std::unordered_map<zCTexture*,    TextureAtlasLookup> m_WorldMeshDiffuseAtlasLookup;
    std::unordered_map<D3D11Texture*, TextureAtlasLookup> m_WorldMeshNormalAtlasLookup;
    std::unordered_map<D3D11Texture*, TextureAtlasLookup> m_WorldMeshFxAtlasLookup;

    std::array<TextureManager::AtlasResult, TEXTURE_ATLAS_MAX> m_WorldMeshDiffuseAtlasses{};
    std::array<TextureManager::AtlasResult, TEXTURE_ATLAS_MAX> m_WorldMeshNormalAtlasses{};
    std::array<TextureManager::AtlasResult, TEXTURE_ATLAS_MAX> m_WorldMeshFxAtlasses{};

    // ---- Global geometry ----
    std::unique_ptr<D3D11VertexBuffer> m_WorldMeshGlobalVertexBuffer;
    std::unique_ptr<D3D11VertexBuffer> m_WorldMeshGlobalIndexBuffer;
    std::unique_ptr<D3D11VertexBuffer> m_WorldMeshGlobalInstanceIdBuffer;

    // ---- GPU submesh descriptors ----
    std::unique_ptr<D3D11StructuredBuffer<WorldMeshSubmeshGPUData>> m_WorldMeshSubmeshBuffer;

    // ---- Draw groups ----
    std::vector<AtlasDrawGroup>   m_WorldMeshAtlasDrawGroups;
    std::unordered_set<MeshInfo*> m_WorldMeshAtlasedSubmeshes;

    void BuildTextureAtlasses();
    void BuildGeometryBuffers();
};
