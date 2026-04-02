#include "D3D11MeshAtlasPass.h"
#include "D3D11GraphicsEngine.h"

#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "GothicAPI.h"
#include "GSky.h"
#include "RenderToTextureBuffer.h"
#include "WorldObjects.h"
#include "VertexTypes.h"
#include "zCTexture.h"
#include "zCMaterial.h"

#include <map>
#include <unordered_set>

// ----- globals defined in D3D11GraphicsEngine.cpp -----
extern bool SupportTextureAtlases;
namespace {
    constexpr DXGI_FORMAT VERTEX_INDEX_DXGI_FORMAT = sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
}

typedef void( __cdecl* PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT )(
    ID3D11DeviceContext* context, unsigned int drawCount,
    ID3D11Buffer* buffer, unsigned int alignedByteOffsetForArgs,
    unsigned int alignedByteStrideForArgs );
extern PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT DrawMultiIndexedInstancedIndirect;

// -------------------------------------------------------

D3D11MeshAtlasPass::D3D11MeshAtlasPass( D3D11GraphicsEngine* engine )
    : m_Engine( engine ) {
}

// ============================================================
//  Build – entry point called from OnWorldLoaded
// ============================================================
void D3D11MeshAtlasPass::Build() {
    // Reset everything
    for ( size_t i = 0; i < TEXTURE_ATLAS_MAX; i++ ) {
        m_WorldMeshDiffuseAtlasses[(DXGI_FORMAT)i].Destroy();
        m_WorldMeshNormalAtlasses[(DXGI_FORMAT)i].Destroy();
        m_WorldMeshFxAtlasses[(DXGI_FORMAT)i].Destroy();
    }
    m_WorldMeshDiffuseAtlasLookup.clear();
    m_WorldMeshNormalAtlasLookup.clear();
    m_WorldMeshFxAtlasLookup.clear();
    m_WorldMeshAtlasDrawGroups.clear();
    m_WorldMeshAtlasedSubmeshes.clear();
    m_WorldMeshGlobalVertexBuffer.reset();
    m_WorldMeshGlobalIndexBuffer.reset();
    m_WorldMeshGlobalInstanceIdBuffer.reset();
    m_WorldMeshSubmeshBuffer.reset();

    if ( !SupportTextureAtlases ||
        !Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.EnableAtlasWorldMesh ) {
        return;
    }

    BuildTextureAtlasses();

    if ( m_WorldMeshDiffuseAtlasLookup.empty() )
        return;

    BuildGeometryBuffers();
}

// ============================================================
//  BuildTextureAtlasses
// ============================================================
void D3D11MeshAtlasPass::BuildTextureAtlasses() {
    struct DiffuseTextureInfo {
        zCTexture* gothicTexture;
        DXGI_FORMAT Format;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture2D;
    };
    struct AuxTextureInfo {
        D3D11Texture* engineTexture;
        DXGI_FORMAT Format;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture2D;
    };

    std::unordered_set<zCTexture*>    seenDiffuse;
    std::unordered_set<D3D11Texture*> seenNormal, seenFx;
    std::vector<DiffuseTextureInfo>   uniqueDiffuse;
    std::vector<AuxTextureInfo>       uniqueNormals, uniqueFx;

    auto& worldSections = Engine::GAPI->GetWorldSections();
    for ( auto& [x, row] : worldSections ) {
        for ( auto& [y, section] : row ) {
            for ( auto const& [meshKey, worldMeshInfo] : section.WorldMeshes ) {
                if ( !meshKey.Material ) continue;

                // Skip animated textures
                zCTexture* baseTex = meshKey.Material->GetTextureSingle();
                if ( !baseTex ) continue;
                unsigned char texFlags = *reinterpret_cast<unsigned char*>(
                    reinterpret_cast<DWORD>(baseTex) + GothicMemoryLocations::zCTexture::Offset_Flags );
                if ( texFlags & GothicMemoryLocations::zCTexture::Mask_FlagIsAnimated )
                    continue;

                // Only opaque + alpha-test
                int alphaFunc = meshKey.Material->GetAlphaFunc();
                if ( alphaFunc > zMAT_ALPHA_FUNC_NONE && alphaFunc != zMAT_ALPHA_FUNC_TEST )
                    continue;

                // Skip non-standard materials (water, portals, etc.)
                if ( meshKey.Info && meshKey.Info->MaterialType != MaterialInfo::MT_None )
                    continue;

                zCTexture* tex      = baseTex;
                auto cachedState    = tex->CacheIn( -1 );
                if ( cachedState != zRES_CACHED_IN ) continue;

                auto surface = tex->GetSurface();
                if ( !surface || !surface->IsSurfaceReady() ) continue;

                auto engineTex = surface->GetEngineTexture();
                if ( !engineTex ) continue;

                // Diffuse
                if ( seenDiffuse.insert( tex ).second ) {
                    D3D11_TEXTURE2D_DESC desc;
                    engineTex->GetTextureObject()->GetDesc( &desc );
                    if ( desc.Format >= 1 && desc.Format < TEXTURE_ATLAS_MAX )
                        uniqueDiffuse.push_back( { tex, desc.Format, engineTex->GetTextureObject() } );
                }

                // Normal map
                D3D11Texture* normalTex = surface->GetNormalmap();
                if ( normalTex && seenNormal.insert( normalTex ).second ) {
                    D3D11_TEXTURE2D_DESC desc;
                    normalTex->GetTextureObject()->GetDesc( &desc );
                    if ( desc.Format >= 1 && desc.Format < TEXTURE_ATLAS_MAX )
                        uniqueNormals.push_back( { normalTex, desc.Format, normalTex->GetTextureObject() } );
                }

                // FX map
                D3D11Texture* fxTex = surface->GetFxMap();
                if ( fxTex && seenFx.insert( fxTex ).second ) {
                    D3D11_TEXTURE2D_DESC desc;
                    fxTex->GetTextureObject()->GetDesc( &desc );
                    if ( desc.Format >= 1 && desc.Format < TEXTURE_ATLAS_MAX )
                        uniqueFx.push_back( { fxTex, desc.Format, fxTex->GetTextureObject() } );
                }
            }
        }
    }

    auto* device  = m_Engine->GetDevice().Get();
    auto* context = m_Engine->GetContext().Get();

    // Build per-format Texture2DArray atlases for diffuse textures
    {
        std::sort( uniqueDiffuse.begin(), uniqueDiffuse.end(),
            []( const DiffuseTextureInfo& a, const DiffuseTextureInfo& b ) { return a.Format < b.Format; } );

        size_t rangeStart = 0;
        while ( rangeStart < uniqueDiffuse.size() ) {
            DXGI_FORMAT fmt = uniqueDiffuse[rangeStart].Format;
            size_t rangeEnd = rangeStart;
            while ( rangeEnd < uniqueDiffuse.size() && uniqueDiffuse[rangeEnd].Format == fmt )
                rangeEnd++;

            std::vector<ID3D11Texture2D*> texPtrs;
            texPtrs.reserve( rangeEnd - rangeStart );
            for ( size_t i = rangeStart; i < rangeEnd; i++ )
                texPtrs.push_back( uniqueDiffuse[i].Texture2D.Get() );

            std::basic_string_view<ID3D11Texture2D*> txView( texPtrs.data(), texPtrs.size() );
            TextureManager::AtlasResult atlas = TextureManager::CreateAtlasArray( device, context, txView, 2048, 6 );

            for ( size_t i = 0; i < texPtrs.size(); i++ )
                m_WorldMeshDiffuseAtlasLookup[uniqueDiffuse[rangeStart + i].gothicTexture] = { fmt, atlas.descriptors[i] };

            m_WorldMeshDiffuseAtlasses[fmt] = atlas;
            rangeStart = rangeEnd;
        }
    }

    // Helper: build aux (normal/fx) atlases
    auto buildAuxAtlases = [&]( std::vector<AuxTextureInfo>& textures,
                                std::unordered_map<D3D11Texture*, TextureAtlasLookup>& lookup,
                                std::array<TextureManager::AtlasResult, TEXTURE_ATLAS_MAX>& atlasses ) {
        std::sort( textures.begin(), textures.end(),
            []( const AuxTextureInfo& a, const AuxTextureInfo& b ) { return a.Format < b.Format; } );

        size_t rangeStart = 0;
        while ( rangeStart < textures.size() ) {
            DXGI_FORMAT fmt = textures[rangeStart].Format;
            size_t rangeEnd = rangeStart;
            while ( rangeEnd < textures.size() && textures[rangeEnd].Format == fmt )
                rangeEnd++;

            std::vector<ID3D11Texture2D*> texPtrs;
            texPtrs.reserve( rangeEnd - rangeStart );
            for ( size_t i = rangeStart; i < rangeEnd; i++ )
                texPtrs.push_back( textures[i].Texture2D.Get() );

            std::basic_string_view<ID3D11Texture2D*> txView( texPtrs.data(), texPtrs.size() );
            TextureManager::AtlasResult atlas = TextureManager::CreateAtlasArray( device, context, txView, 2048, 6 );

            for ( size_t i = 0; i < texPtrs.size(); i++ )
                lookup[textures[rangeStart + i].engineTexture] = { fmt, atlas.descriptors[i] };

            atlasses[fmt] = atlas;
            rangeStart = rangeEnd;
        }
    };

    buildAuxAtlases( uniqueNormals, m_WorldMeshNormalAtlasLookup, m_WorldMeshNormalAtlasses );
    buildAuxAtlases( uniqueFx,      m_WorldMeshFxAtlasLookup,     m_WorldMeshFxAtlasses );

    LogInfo() << "World Mesh Atlas: " << uniqueDiffuse.size() << " diffuse, "
              << uniqueNormals.size() << " normal, " << uniqueFx.size() << " fx textures";
}

// ============================================================
//  BuildGeometryBuffers
// ============================================================
void D3D11MeshAtlasPass::BuildGeometryBuffers() {
    std::vector<ExVertexStruct>         allVertices;
    std::vector<VERTEX_INDEX>           allIndices;
    std::vector<WorldMeshSubmeshGPUData> submeshGPU;

    std::map<DXGI_FORMAT, AtlasDrawGroup> groupsByFormat;
    std::unordered_set<MeshInfo*>         processedMeshes;

    // Pre-count
    {
        size_t totalVertices = 0, totalIndices = 0, totalSubmeshes = 0;
        auto& ws = Engine::GAPI->GetWorldSections();
        for ( auto& [x, row] : ws ) {
            for ( auto& [y, section] : row ) {
                for ( auto const& [meshKey, worldMeshInfo] : section.WorldMeshes ) {
                    if ( !meshKey.Material ) continue;
                    zCTexture* tex = meshKey.Material->GetTextureSingle();
                    if ( m_WorldMeshDiffuseAtlasLookup.find( tex ) != m_WorldMeshDiffuseAtlasLookup.end() ) {
                        totalVertices += worldMeshInfo->Vertices.size();
                        totalIndices  += worldMeshInfo->Indices.size();
                        totalSubmeshes++;
                    }
                }
            }
        }
        allVertices.reserve( totalVertices );
        allIndices.reserve( totalIndices );
        submeshGPU.reserve( totalSubmeshes );
    }

    auto& worldSections = Engine::GAPI->GetWorldSections();
    for ( auto& [x, row] : worldSections ) {
        for ( auto& [y, section] : row ) {
            for ( auto const& [meshKey, worldMeshInfo] : section.WorldMeshes ) {
                if ( !meshKey.Material ) continue;

                zCTexture* tex    = meshKey.Material->GetTextureSingle();
                auto diffIt       = m_WorldMeshDiffuseAtlasLookup.find( tex );
                if ( diffIt == m_WorldMeshDiffuseAtlasLookup.end() )
                    continue;

                MeshInfo* mi = worldMeshInfo;
                if ( !processedMeshes.insert( mi ).second )
                    continue;

                m_WorldMeshAtlasedSubmeshes.insert( mi );

                const TextureAtlasLookup& diffLookup = diffIt->second;
                auto& group  = groupsByFormat[diffLookup.atlasFormat];
                group.format = diffLookup.atlasFormat;

                UINT baseVertex  = static_cast<UINT>(allVertices.size());
                UINT startIndex  = static_cast<UINT>(allIndices.size());

                allVertices.insert( allVertices.end(), mi->Vertices.begin(), mi->Vertices.end() );
                allIndices.insert(  allIndices.end(),  mi->Indices.begin(),  mi->Indices.end() );

                WorldMeshSubmeshGPUData gpuData = {};
                gpuData.diffuseSlice = diffLookup.descriptor.slice;
                gpuData.dUStart      = diffLookup.descriptor.uStart;
                gpuData.dVStart      = diffLookup.descriptor.vStart;
                gpuData.dUEnd        = diffLookup.descriptor.uEnd;
                gpuData.dVEnd        = diffLookup.descriptor.vEnd;

                UINT flags = 0;
                auto surface = tex->GetSurface();
                if ( surface ) {
                    D3D11Texture* normalTex = surface->GetNormalmap();
                    if ( normalTex ) {
                        auto normIt = m_WorldMeshNormalAtlasLookup.find( normalTex );
                        if ( normIt != m_WorldMeshNormalAtlasLookup.end() ) {
                            gpuData.normalSlice = normIt->second.descriptor.slice;
                            gpuData.nUStart     = normIt->second.descriptor.uStart;
                            gpuData.nVStart     = normIt->second.descriptor.vStart;
                            gpuData.nUEnd       = normIt->second.descriptor.uEnd;
                            gpuData.nVEnd       = normIt->second.descriptor.vEnd;
                            flags |= 1; // HAS_NORMAL
                        }
                    }

                    D3D11Texture* fxTex = surface->GetFxMap();
                    if ( fxTex ) {
                        auto fxIt = m_WorldMeshFxAtlasLookup.find( fxTex );
                        if ( fxIt != m_WorldMeshFxAtlasLookup.end() ) {
                            gpuData.fxSlice = fxIt->second.descriptor.slice;
                            gpuData.fUStart = fxIt->second.descriptor.uStart;
                            gpuData.fVStart = fxIt->second.descriptor.vStart;
                            gpuData.fUEnd   = fxIt->second.descriptor.uEnd;
                            gpuData.fVEnd   = fxIt->second.descriptor.vEnd;
                            flags |= 2; // HAS_FX
                        }
                    }
                }

                int alphaFunc = meshKey.Material->GetAlphaFunc();
                if ( alphaFunc == zMAT_ALPHA_FUNC_TEST || tex->HasAlphaChannel() )
                    flags |= 4; // ALPHA_TEST

                gpuData.flags = flags;

                UINT submeshIndex = static_cast<UINT>(submeshGPU.size());
                submeshGPU.push_back( gpuData );

                D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args = {};
                args.IndexCountPerInstance  = static_cast<UINT>(mi->Indices.size());
                args.InstanceCount          = 1;
                args.StartIndexLocation     = startIndex;
                args.BaseVertexLocation     = static_cast<int>(baseVertex);
                args.StartInstanceLocation  = submeshIndex;
                group.indirectArgs.push_back( args );
            }
        }
    }

    if ( allVertices.empty() ) {
        LogWarn() << "D3D11MeshAtlasPass::BuildGeometryBuffers: No world mesh vertices for atlas";
        return;
    }

    m_WorldMeshGlobalVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    m_WorldMeshGlobalVertexBuffer->Init(
        allVertices.data(),
        static_cast<unsigned int>(allVertices.size() * sizeof( ExVertexStruct )),
        D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_IMMUTABLE,
        D3D11VertexBuffer::CA_NONE );

    m_WorldMeshGlobalIndexBuffer = std::make_unique<D3D11VertexBuffer>();
    m_WorldMeshGlobalIndexBuffer->Init(
        allIndices.data(),
        static_cast<unsigned int>(allIndices.size() * sizeof( VERTEX_INDEX )),
        D3D11VertexBuffer::B_INDEXBUFFER,
        D3D11VertexBuffer::U_IMMUTABLE,
        D3D11VertexBuffer::CA_NONE );

    UINT maxIds = static_cast<UINT>(submeshGPU.size());
    if ( maxIds < 256 ) maxIds = 256;
    std::vector<uint32_t> instanceIds( maxIds );
    for ( uint32_t i = 0; i < maxIds; i++ )
        instanceIds[i] = i;

    m_WorldMeshGlobalInstanceIdBuffer = std::make_unique<D3D11VertexBuffer>();
    m_WorldMeshGlobalInstanceIdBuffer->Init(
        instanceIds.data(),
        static_cast<unsigned int>(instanceIds.size() * sizeof( uint32_t )),
        D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_IMMUTABLE,
        D3D11VertexBuffer::CA_NONE );

    auto* device  = m_Engine->GetDevice().Get();
    auto* context = m_Engine->GetContext().Get();

    m_WorldMeshSubmeshBuffer = std::make_unique<D3D11StructuredBuffer<WorldMeshSubmeshGPUData>>();
    m_WorldMeshSubmeshBuffer->Init( device, static_cast<UINT>(submeshGPU.size()), false, false );
    m_WorldMeshSubmeshBuffer->UpdateBufferDefault( context, submeshGPU.data(), static_cast<UINT>(submeshGPU.size()) );

    m_WorldMeshAtlasDrawGroups.clear();
    for ( auto& [fmt, group] : groupsByFormat ) {
        if ( group.indirectArgs.empty() )
            continue;

        UINT bufSize = static_cast<UINT>(group.indirectArgs.size() * sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ));
        group.indirectBuffer = std::make_unique<D3D11IndirectBuffer>();
        group.indirectBuffer->Init(
            group.indirectArgs.data(), bufSize,
            D3D11IndirectBuffer::B_VERTEXBUFFER,
            D3D11IndirectBuffer::U_IMMUTABLE,
            D3D11IndirectBuffer::CA_NONE );

        m_WorldMeshAtlasDrawGroups.push_back( std::move( group ) );
    }

    LogInfo() << "World Mesh Atlas geometry: " << allVertices.size() << " vertices, "
              << allIndices.size() << " indices, "
              << m_WorldMeshAtlasDrawGroups.size() << " format groups, "
              << submeshGPU.size() << " submeshes";
}

// ============================================================
//  Draw – per-frame indirect draw of atlased world mesh
// ============================================================
XRESULT D3D11MeshAtlasPass::Draw() {
    if ( m_WorldMeshAtlasDrawGroups.empty() ||
         !m_WorldMeshGlobalVertexBuffer || !m_WorldMeshGlobalIndexBuffer )
        return XR_SUCCESS;

    auto _ = m_Engine->RecordGraphicsEvent( L"DrawWorldMesh_Atlas" );
    auto& context = m_Engine->GetContext();

    m_Engine->SetDefaultStates();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();

    context->DSSetShader( nullptr, nullptr, 0 );
    context->HSSetShader( nullptr, nullptr, 0 );

    // --- Bind global geometry ---
    UINT strides[2] = { sizeof( ExVertexStruct ), sizeof( uint32_t ) };
    UINT offsets[2] = { 0, 0 };
    ID3D11Buffer* vbs[2] = {
        m_WorldMeshGlobalVertexBuffer->GetVertexBuffer().Get(),
        m_WorldMeshGlobalInstanceIdBuffer->GetVertexBuffer().Get()
    };
    context->IASetVertexBuffers( 0, 2, vbs, strides, offsets );
    context->IASetIndexBuffer( m_WorldMeshGlobalIndexBuffer->GetVertexBuffer().Get(),
                               VERTEX_INDEX_DXGI_FORMAT, 0 );
    context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // Submesh structured buffer -> VS t1
    ID3D11ShaderResourceView* submeshSRV = m_WorldMeshSubmeshBuffer->GetSRV();
    context->VSSetShaderResources( 1, 1, &submeshSRV );

    // Vertex shader
    m_Engine->SetActiveVertexShader( VShaderID::VS_ExWorldAtlas );
    m_Engine->SetupVS_ExMeshDrawCall();
    m_Engine->SetupVS_ExConstantBuffer();
    m_Engine->ActiveVS->Apply();

    // Pixel shader
    m_Engine->SetActivePixelShader( PShaderID::PS_WorldAtlas );

    m_Engine->ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
        &Engine::GAPI->GetRendererState().GraphicsState );
    m_Engine->ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

    GSky* sky = Engine::GAPI->GetSky();
    m_Engine->ActivePS->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
    m_Engine->ActivePS->GetConstantBuffer()[1]->BindToPixelShader( 1 );

    MaterialInfo defMaterial{};
    m_Engine->ActivePS->GetConstantBuffer()[2]->UpdateBuffer( &defMaterial.buffer );
    m_Engine->ActivePS->GetConstantBuffer()[2]->BindToPixelShader( 2 );

    m_Engine->InfiniteRangeConstantBuffer->BindToPixelShader( 3 );

    context->PSSetShaderResources( 4, 1, m_Engine->ReflectionCube.GetAddressOf() );

    m_Engine->ActivePS->Apply();

    // --- Draw per format group ---
    for ( auto& group : m_WorldMeshAtlasDrawGroups ) {
        ID3D11ShaderResourceView* diffuseSRV = m_WorldMeshDiffuseAtlasses[group.format].atlasSRV;
        if ( !diffuseSRV ) continue;

        // Bind first available normal/fx atlases (format grouping is per-diffuse)
        ID3D11ShaderResourceView* normalSRV = nullptr;
        ID3D11ShaderResourceView* fxSRV     = nullptr;
        for ( size_t i = 0; i < TEXTURE_ATLAS_MAX; i++ ) {
            if ( !normalSRV && m_WorldMeshNormalAtlasses[i].atlasSRV )
                normalSRV = m_WorldMeshNormalAtlasses[i].atlasSRV;
            if ( !fxSRV && m_WorldMeshFxAtlasses[i].atlasSRV )
                fxSRV = m_WorldMeshFxAtlasses[i].atlasSRV;
        }

        ID3D11ShaderResourceView* psSRVs[3] = { diffuseSRV, normalSRV, fxSRV };
        context->PSSetShaderResources( 0, 3, psSRVs );

        DrawMultiIndexedInstancedIndirect(
            context.Get(),
            static_cast<UINT>(group.indirectArgs.size()),
            group.indirectBuffer->GetIndirectBuffer().Get(),
            0,
            sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) );
    }

    // Unbind submesh buffer from VS
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->VSSetShaderResources( 1, 1, &nullSRV );

    return XR_SUCCESS;
}
