#pragma once
#include "D3D11AtlasTypes.h"
#include "D3D11StructuredBuffer.h"
#include "D3D11VertexBuffer.h"
#include "D3D11ConstantBuffer.h"
#include "VobCulling.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <unordered_map>
#include <vector>

class D3D11GraphicsEngine;
class Frustum;
class zCTexture;

/**
 * Encapsulates all texture-atlas-based GPU-driven rendering for static VOBs.
 *
 * Responsibilities:
 *  - Building per-format Texture2DArray atlases from static-VOB diffuse textures
 *  - Building the merged global VB/IB and per-submesh indirect-args buffer
 *  - Building the GPU structured buffers used by CS_CullVobs
 *  - Executing the GPU-culling compute pass and the subsequent indirect draw
 *
 * The engine keeps one instance of this class. Call Build() when a new world
 * is loaded (OnWorldLoaded), and Draw() every frame in place of the old
 * DrawVOBsIndirect().
 */
class D3D11VobAtlasPass {
    friend class D3D11GraphicsEngine;
public:
    explicit D3D11VobAtlasPass( D3D11GraphicsEngine* engine );

    /** (Re-)build atlases, geometry buffers, and GPU culling buffers.
     *  Called from D3D11GraphicsEngine::OnWorldLoaded(). */
    void Build();

    /** GPU-cull static VOBs and draw them with indirect multi-draw.
     *  bindPS=false is used in shadow passes to skip the pixel shader. */
    XRESULT Draw( const Frustum& frustum, bool bindPS = true );

    /** True once Build() has completed and at least one draw group exists. */
    bool IsReady() const { return !m_AtlasDrawGroups.empty(); }

    /** Atlas lookup (read-only access for other systems if needed). */
    const std::unordered_map<zCTexture*, TextureAtlasLookup>& GetAtlasLookup() const {
        return m_TextureAtlasLookup;
    }

private:
    D3D11GraphicsEngine* m_Engine;

    // ---- Atlas textures ----
    std::array<TextureManager::AtlasResult, TEXTURE_ATLAS_MAX> m_TextureAtlasses{};
    std::unordered_map<zCTexture*, TextureAtlasLookup>         m_TextureAtlasLookup;

    // ---- Global geometry ----
    std::unique_ptr<D3D11VertexBuffer> m_StaticGlobalVertexBuffer;
    std::unique_ptr<D3D11VertexBuffer> m_StaticGlobalIndexBuffer;
    std::unique_ptr<D3D11VertexBuffer> m_GlobalInstanceIdBuffer;
    std::vector<AtlasDrawGroup>        m_AtlasDrawGroups;

    // (legacy slot – not yet used but reserved for future streaming)
    std::unique_ptr<D3D11StructuredBuffer<VobInstanceInfoAtlas>> m_StaticVobInstanceBuffer;

    // ---- GPU culling buffers ----
    std::unique_ptr<D3D11StructuredBuffer<VobGPUData>>           m_VobGPUBuffer;
    std::unique_ptr<D3D11StructuredBuffer<SubmeshGPUData>>       m_SubmeshGPUBuffer;
    std::unique_ptr<D3D11StructuredBuffer<VobInstanceInfoAtlas>> m_InstanceBufferGPU;
    std::unique_ptr<D3D11IndirectBuffer>                         m_MergedIndirectArgs;
    Microsoft::WRL::ComPtr<ID3D11Buffer>                         m_IndirectArgsTemplate;
    std::unique_ptr<D3D11ConstantBuffer>                         m_CullConstantBuffer;
    std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS>      m_MergedArgsReset;
    UINT m_TotalMaxInstances = 0;

    void BuildTextureAtlasses();
    void BuildGeometryBuffers();
    void BuildGPUCullingBuffers();
};
