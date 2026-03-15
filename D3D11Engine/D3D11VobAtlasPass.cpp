#include "D3D11VobAtlasPass.h"
#include "D3D11GraphicsEngine.h"

#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11CShader.h"
#include "D3D11ConstantBuffer.h"
#include "GothicAPI.h"
#include "GSky.h"
#include "RenderToTextureBuffer.h"
#include "WorldObjects.h"
#include "VertexTypes.h"
#include "zCTexture.h"
#include "zCMaterial.h"
#include "zCVob.h"
#include "zCVisual.h"

#include <map>
#include <unordered_set>

// ----- globals defined in D3D11GraphicsEngine.cpp -----
extern bool SupportTextureAtlases;
extern float vobAnimation_WindStrength;
namespace {
    constexpr DXGI_FORMAT VERTEX_INDEX_DXGI_FORMAT = sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
}

typedef void( __cdecl* PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT )(
    ID3D11DeviceContext* context, unsigned int drawCount,
    ID3D11Buffer* buffer, unsigned int alignedByteOffsetForArgs,
    unsigned int alignedByteStrideForArgs );
extern PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT DrawMultiIndexedInstancedIndirect;

// -------------------------------------------------------

D3D11VobAtlasPass::D3D11VobAtlasPass( D3D11GraphicsEngine* engine )
    : m_Engine( engine ) {
}

// ============================================================
//  Build – entry point called from OnWorldLoaded
// ============================================================
void D3D11VobAtlasPass::Build() {
    // Reset everything
    for ( size_t i = 0; i < TEXTURE_ATLAS_MAX; i++ )
        m_TextureAtlasses[(DXGI_FORMAT)i].Destroy();
    m_TextureAtlasLookup.clear();
    m_AtlasDrawGroups.clear();

    if ( !SupportTextureAtlases ||
        !Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.EnableAtlasStaticVobs ) {
        return;
    }

    BuildTextureAtlasses();

    if ( m_TextureAtlasLookup.empty() )
        return;

    BuildGeometryBuffers();
    BuildGPUCullingBuffers();
}

// ============================================================
//  BuildTextureAtlasses
// ============================================================
void D3D11VobAtlasPass::BuildTextureAtlasses() {
    struct TextureInfo {
        zCTexture* gothicTexture;
        DXGI_FORMAT Format;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture2D;
    };

    std::unordered_set<zCTexture*> seenTextures;
    std::vector<TextureInfo> uniqueTextures;

    for ( auto vobInfo : m_Engine->m_StaticVobs ) {
        for ( auto& byTex : reinterpret_cast<MeshVisualInfo*>(vobInfo->VisualInfo)->MeshesByTexture ) {
            zCTexture* tex = byTex.first.Material->GetTexture();

            if ( !tex ) {
                auto vis = reinterpret_cast<MeshVisualInfo*>(vobInfo->VisualInfo)->Visual;
                LogError()
                    << "Texture not found for visual " << vobInfo->VisualInfo->VisualName
                    << " Visual Type: " << vis->GetVisualType();

                continue;
            }

            if ( !seenTextures.insert( tex ).second ) {
                continue;
            }

            auto cachedState = tex->CacheIn( -1 );
            if ( cachedState != zRES_CACHED_IN ) {
                LogError() << "Texture " << tex->GetName() << " was not cached in";
                continue;
            }

            auto surface = tex->GetSurface();
            if ( !surface || !surface->IsSurfaceReady() ) {
                LogError() << "Texture " << tex->GetName() << " surface not ready";
                continue;
            }

            auto engineTex = surface->GetEngineTexture();
            if ( !engineTex ) {
                LogError() << "Texture " << tex->GetName() << " no engine texture";
                continue;
            }

            D3D11_TEXTURE2D_DESC desc;
            engineTex->GetTextureObject()->GetDesc( &desc );
            if ( desc.Format < 1 || desc.Format >= TEXTURE_ATLAS_MAX ) {
                LogError() << "Texture " << tex->GetName() << " has unsupported format for atlas: " << desc.Format;
                continue;
            }
            uniqueTextures.push_back( { tex, desc.Format, engineTex->GetTextureObject() } );
        }
    }

    // Sort by format so same-format textures are contiguous
    std::sort( uniqueTextures.begin(), uniqueTextures.end(),
        []( const TextureInfo& a, const TextureInfo& b ) { return a.Format < b.Format; } );

    // Create one Texture2DArray atlas per contiguous format range
    size_t rangeStart = 0;
    while ( rangeStart < uniqueTextures.size() ) {
        DXGI_FORMAT fmt = uniqueTextures[rangeStart].Format;
        size_t rangeEnd = rangeStart;
        while ( rangeEnd < uniqueTextures.size() && uniqueTextures[rangeEnd].Format == fmt )
            rangeEnd++;

        std::vector<ID3D11Texture2D*> texPtrs;
        texPtrs.reserve( rangeEnd - rangeStart );
        for ( size_t i = rangeStart; i < rangeEnd; i++ )
            texPtrs.push_back( uniqueTextures[i].Texture2D.Get() );

        std::basic_string_view<ID3D11Texture2D*> txView( texPtrs.data(), texPtrs.size() );
        TextureManager::AtlasResult atlas = TextureManager::CreateAtlasArray(
            m_Engine->GetDevice().Get(), m_Engine->GetContext().Get(), txView, 2048, 6 );

        for ( size_t i = 0; i < texPtrs.size(); i++ ) {
            m_TextureAtlasLookup[uniqueTextures[rangeStart + i].gothicTexture] = {
                fmt, atlas.descriptors[i]
            };
        }
        m_TextureAtlasses[fmt] = atlas;
        rangeStart = rangeEnd;
    }

    LogInfo() << "VOB Atlas: " << uniqueTextures.size() << " unique textures, "
              << m_TextureAtlasLookup.size() << " mapped";
}

// ============================================================
//  BuildGeometryBuffers
// ============================================================
void D3D11VobAtlasPass::BuildGeometryBuffers() {
    std::vector<ExVertexStruct> allVertices;
    std::vector<VERTEX_INDEX> allIndices;
    std::map<DXGI_FORMAT, AtlasDrawGroup> groupsByFormat;
    std::unordered_set<MeshInfo*> processedMeshes;

    // Pre-count to avoid incremental reallocation
    {
        size_t totalVertices = 0, totalIndices = 0;
        std::unordered_set<MeshInfo*> counted;
        for ( auto const& [proto, visual] : Engine::GAPI->GetStaticMeshVisuals() ) {
            for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
                if ( m_TextureAtlasLookup.find( meshKey.Texture ) == m_TextureAtlasLookup.end() )
                    continue;
                for ( MeshInfo* mi : meshList ) {
                    if ( counted.insert( mi ).second ) {
                        totalVertices += mi->Vertices.size();
                        totalIndices += mi->Indices.size();
                    }
                }
            }
        }
        allVertices.reserve( totalVertices );
        allIndices.reserve( totalIndices );
    }

    for ( auto const& [proto, visual] : Engine::GAPI->GetStaticMeshVisuals() ) {
        for ( auto const& [meshKey, meshList] : visual->MeshesByTexture ) {
            auto it = m_TextureAtlasLookup.find( meshKey.Texture );
            if ( it == m_TextureAtlasLookup.end() )
                continue;

            const TextureAtlasLookup& lookup = it->second;
            auto& group = groupsByFormat[lookup.atlasFormat];
            group.format = lookup.atlasFormat;

            for ( MeshInfo* mi : meshList ) {
                if ( !processedMeshes.insert( mi ).second )
                    continue;

                UINT baseVertex = static_cast<UINT>(allVertices.size());
                UINT startIndex = static_cast<UINT>(allIndices.size());

                allVertices.insert( allVertices.end(), mi->Vertices.begin(), mi->Vertices.end() );
                allIndices.insert( allIndices.end(), mi->Indices.begin(), mi->Indices.end() );

                StaticSubmeshEntry entry;
                entry.indexCount          = static_cast<UINT>(mi->Indices.size());
                entry.startIndexLocation  = startIndex;
                entry.baseVertexLocation  = static_cast<int>(baseVertex);
                entry.atlasDesc           = lookup.descriptor;
                entry.visual              = visual;
                group.submeshes.push_back( entry );

                D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args = {};
                args.IndexCountPerInstance = entry.indexCount;
                args.InstanceCount         = 0;
                args.StartIndexLocation    = entry.startIndexLocation;
                args.BaseVertexLocation    = entry.baseVertexLocation;
                args.StartInstanceLocation = 0;
                group.indirectArgs.push_back( args );
            }
        }
    }

    if ( allVertices.empty() ) {
        LogWarn() << "D3D11VobAtlasPass::BuildGeometryBuffers: No vertices to process";
        return;
    }

    m_StaticGlobalVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    m_StaticGlobalVertexBuffer->Init(
        allVertices.data(),
        static_cast<unsigned int>(allVertices.size() * sizeof( ExVertexStruct )),
        D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_IMMUTABLE,
        D3D11VertexBuffer::CA_NONE );

    m_StaticGlobalIndexBuffer = std::make_unique<D3D11VertexBuffer>();
    m_StaticGlobalIndexBuffer->Init(
        allIndices.data(),
        static_cast<unsigned int>(allIndices.size() * sizeof( VERTEX_INDEX )),
        D3D11VertexBuffer::B_INDEXBUFFER,
        D3D11VertexBuffer::U_IMMUTABLE,
        D3D11VertexBuffer::CA_NONE );

    UINT maxInstanceIds = static_cast<UINT>(m_Engine->m_StaticVobs.size() * 4);
    if ( maxInstanceIds < 4096 )
        maxInstanceIds = 4096;
    std::vector<uint32_t> instanceIds( maxInstanceIds );
    for ( uint32_t i = 0; i < maxInstanceIds; i++ )
        instanceIds[i] = i;

    m_GlobalInstanceIdBuffer = std::make_unique<D3D11VertexBuffer>();
    m_GlobalInstanceIdBuffer->Init(
        instanceIds.data(),
        static_cast<unsigned int>(instanceIds.size() * sizeof( uint32_t )),
        D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_IMMUTABLE,
        D3D11VertexBuffer::CA_NONE );

    m_AtlasDrawGroups.clear();
    for ( auto& [fmt, group] : groupsByFormat ) {
        if ( group.indirectArgs.empty() )
            continue;

        UINT bufSize = static_cast<UINT>(group.indirectArgs.size() * sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ));
        group.indirectBuffer = std::make_unique<D3D11IndirectBuffer>();
        group.indirectBuffer->Init(
            group.indirectArgs.data(), bufSize,
            D3D11IndirectBuffer::B_VERTEXBUFFER,
            D3D11IndirectBuffer::U_DYNAMIC,
            D3D11IndirectBuffer::CA_WRITE );

        m_AtlasDrawGroups.push_back( std::move( group ) );
    }

    LogInfo() << "VOB Atlas geometry: " << allVertices.size() << " vertices, "
              << allIndices.size() << " indices, "
              << m_AtlasDrawGroups.size() << " atlas groups, "
              << processedMeshes.size() << " unique submeshes";
}

// ============================================================
//  BuildGPUCullingBuffers
// ============================================================
void D3D11VobAtlasPass::BuildGPUCullingBuffers() {
    if ( m_AtlasDrawGroups.empty() || m_Engine->m_StaticVobs.empty() )
        return;

    // --- 1. Build visual -> vob-count mapping ---
    std::unordered_map<MeshVisualInfo*, UINT> vobsPerVisual;
    std::unordered_map<MeshVisualInfo*, std::vector<size_t>> vobIndicesByVisual;

    for ( size_t i = 0; i < m_Engine->m_StaticVobs.size(); i++ ) {
        auto* visual = reinterpret_cast<MeshVisualInfo*>(m_Engine->m_StaticVobs[i]->VisualInfo);
        vobsPerVisual[visual]++;
        vobIndicesByVisual[visual].push_back( i );
    }

    // --- 2. Build merged indirect args + SubmeshGPUData ---
    std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> mergedArgs;
    std::unordered_map<MeshVisualInfo*, std::vector<SubmeshGPUData>> visualSubmeshMap;

    UINT runningInstanceOffset = 0;
    UINT globalArgIndex        = 0;

    for ( auto& group : m_AtlasDrawGroups ) {
        group.mergedArgsOffset = static_cast<UINT>(mergedArgs.size() * sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ));
        group.mergedArgsCount  = static_cast<UINT>(group.indirectArgs.size());

        for ( size_t si = 0; si < group.submeshes.size(); si++ ) {
            const auto& submesh    = group.submeshes[si];
            MeshVisualInfo* visual = submesh.visual;
            UINT maxInstances      = vobsPerVisual.count( visual ) ? vobsPerVisual[visual] : 0;

            D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args = {};
            args.IndexCountPerInstance  = submesh.indexCount;
            args.InstanceCount          = 0;
            args.StartIndexLocation     = submesh.startIndexLocation;
            args.BaseVertexLocation     = submesh.baseVertexLocation;
            args.StartInstanceLocation  = runningInstanceOffset;
            mergedArgs.push_back( args );

            SubmeshGPUData smGPU = {};
            smGPU.slice               = submesh.atlasDesc.slice;
            smGPU.uStart              = submesh.atlasDesc.uStart;
            smGPU.vStart              = submesh.atlasDesc.vStart;
            smGPU.uEnd                = submesh.atlasDesc.uEnd;
            smGPU.vEnd                = submesh.atlasDesc.vEnd;
            smGPU.argIndex            = globalArgIndex;
            smGPU.instanceBaseOffset  = runningInstanceOffset;
            smGPU.globalSourceIndex   = 0;

            visualSubmeshMap[visual].push_back( smGPU );

            runningInstanceOffset += maxInstances;
            globalArgIndex++;
        }
    }

    m_TotalMaxInstances = runningInstanceOffset;

    // --- 3. Flatten per-visual submesh entries ---
    struct VisualSubmeshRange { UINT start; UINT count; };
    std::unordered_map<MeshVisualInfo*, VisualSubmeshRange> visualSubmeshRanges;
    std::vector<SubmeshGPUData> submeshGPU;

    for ( auto& [visual, entries] : visualSubmeshMap ) {
        UINT start = static_cast<UINT>(submeshGPU.size());
        for ( auto& entry : entries )
            submeshGPU.push_back( entry );
        visualSubmeshRanges[visual] = { start, static_cast<UINT>(entries.size()) };
    }

    // --- 4. Build VobGPUData ---
    std::vector<VobGPUData> vobGPU;
    vobGPU.reserve( m_Engine->m_StaticVobs.size() );

    for ( size_t i = 0; i < m_Engine->m_StaticVobs.size(); i++ ) {
        VobInfo* v     = m_Engine->m_StaticVobs[i];
        auto* visual   = reinterpret_cast<MeshVisualInfo*>(v->VisualInfo);

        VobGPUData data = {};
        DirectX::BoundingBox bb = Frustum::BBoxFromzTBBox3D( v->Vob->GetBBox() );
        data.aabbCenter = bb.Center;
        data.aabbExtent = bb.Extents;
        data.world      = v->WorldMatrix;
        data.prevWorld  = v->WorldMatrix;
        data.color      = v->GroundColor;

        zTAnimationMode aniMode = v->Vob->GetVisualAniMode();
        if ( aniMode != zVISUAL_ANIMODE_NONE ) {
            data.aniModeStrength        = v->Vob->GetVisualAniModeStrength();
            data.canBeAffectedByPlayer  = (!v->Vob->GetDynColl() ? 1.0f : 0.0f);
        } else {
            data.aniModeStrength        = 0.0f;
            data.canBeAffectedByPlayer  = 0.0f;
        }

        auto it = visualSubmeshRanges.find( visual );
        if ( it != visualSubmeshRanges.end() ) {
            data.submeshStart = it->second.start;
            data.submeshCount = it->second.count;
        }
        vobGPU.push_back( data );
    }

    // --- 5. Upload to GPU ---
    auto* device  = m_Engine->GetDevice().Get();
    auto* context = m_Engine->GetContext().Get();

    m_VobGPUBuffer = std::make_unique<D3D11StructuredBuffer<VobGPUData>>();
    m_VobGPUBuffer->Init( device, static_cast<UINT>(vobGPU.size()), false, false );
    m_VobGPUBuffer->UpdateBufferDefault( context, vobGPU.data(), static_cast<UINT>(vobGPU.size()) );

    m_SubmeshGPUBuffer = std::make_unique<D3D11StructuredBuffer<SubmeshGPUData>>();
    m_SubmeshGPUBuffer->Init( device, static_cast<UINT>(submeshGPU.size()), false, false );
    m_SubmeshGPUBuffer->UpdateBufferDefault( context, submeshGPU.data(), static_cast<UINT>(submeshGPU.size()) );

    UINT instanceCapacity = std::max( m_TotalMaxInstances, 1u );
    m_InstanceBufferGPU = std::make_unique<D3D11StructuredBuffer<VobInstanceInfoAtlas>>();
    m_InstanceBufferGPU->Init( device, instanceCapacity, false, true );

    UINT argsSize = static_cast<UINT>(mergedArgs.size() * sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ));
    m_MergedIndirectArgs = std::make_unique<D3D11IndirectBuffer>();
    m_MergedIndirectArgs->Init(
        mergedArgs.data(), argsSize,
        D3D11IndirectBuffer::B_UNORDERED_ACCESS,
        D3D11IndirectBuffer::U_DEFAULT,
        D3D11IndirectBuffer::CA_NONE );

    m_MergedArgsReset = mergedArgs;

    D3D11_BUFFER_DESC templateDesc = {};
    templateDesc.ByteWidth   = argsSize;
    templateDesc.Usage       = D3D11_USAGE_DEFAULT;
    templateDesc.BindFlags   = 0;
    templateDesc.MiscFlags   = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;

    D3D11_SUBRESOURCE_DATA templateData = {};
    templateData.pSysMem = mergedArgs.data();
    device->CreateBuffer( &templateDesc, &templateData, m_IndirectArgsTemplate.ReleaseAndGetAddressOf() );

    CullConstants initCB = {};
    m_CullConstantBuffer = std::make_unique<D3D11ConstantBuffer>( sizeof( CullConstants ), &initCB );

    if ( m_TotalMaxInstances > 0 ) {
        std::vector<uint32_t> instanceIds( m_TotalMaxInstances );
        for ( uint32_t i = 0; i < m_TotalMaxInstances; i++ )
            instanceIds[i] = i;

        m_GlobalInstanceIdBuffer = std::make_unique<D3D11VertexBuffer>();
        m_GlobalInstanceIdBuffer->Init(
            instanceIds.data(),
            static_cast<unsigned int>(instanceIds.size() * sizeof( uint32_t )),
            D3D11VertexBuffer::B_VERTEXBUFFER,
            D3D11VertexBuffer::U_IMMUTABLE,
            D3D11VertexBuffer::CA_NONE );
    }

    LogInfo() << "VOB Atlas GPU Culling: " << vobGPU.size() << " vobs, "
              << submeshGPU.size() << " submesh entries, "
              << mergedArgs.size() << " indirect args, "
              << m_TotalMaxInstances << " max instances";
}

// ============================================================
//  Draw – per-frame GPU-cull + indirect draw
// ============================================================
XRESULT D3D11VobAtlasPass::Draw( const Frustum& frustum, bool bindPS ) {
    if ( m_AtlasDrawGroups.empty() || !m_VobGPUBuffer ||
         !m_StaticGlobalVertexBuffer || !m_StaticGlobalIndexBuffer )
        return XR_SUCCESS;

    auto _ = m_Engine->RecordGraphicsEvent( L"DrawVOBsIndirect" );
    auto& context = m_Engine->GetContext();

    // --- 0. Build Hi-Z pyramid for occlusion culling (main pass only) ---
    const bool useHiZ = bindPS && m_Engine->m_HiZTexture && m_Engine->m_HiZSRV;
    if ( useHiZ ) {
        m_Engine->CopyDepthStencil();
        m_Engine->BuildHiZPyramid();
    }

    // --- 1. Reset indirect args InstanceCounts ---
    context->CopyResource( m_MergedIndirectArgs->GetIndirectBuffer().Get(),
                           m_IndirectArgsTemplate.Get() );

    // --- 2. Update cull constant buffer ---
    CullConstants cb = {};
    memcpy( cb.frustumPlanes, frustum.GetPlanes().data(), 6 * sizeof( XMFLOAT4 ) );
    cb.cameraPosition    = Engine::GAPI->GetCameraPosition();
    cb.drawDistance      = Engine::GAPI->GetRendererState().RendererSettings.OutdoorVobDrawRadius;
    cb.globalWindStrength = vobAnimation_WindStrength;
    cb.windAdvanced      = (Engine::GAPI->GetRendererState().RendererSettings.WindQuality
                            == GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED) ? 1 : 0;
    cb.numVobs           = static_cast<UINT>(m_Engine->m_StaticVobs.size());
    cb.feedbackFrameNumber = 0;

    if ( useHiZ ) {
        cb.enableHiZ  = 1;
        cb.hiZMipCount = m_Engine->m_HiZMipCount;
        cb.hiZWidth    = static_cast<float>(m_Engine->DepthStencilBuffer->GetSizeX());
        cb.hiZHeight   = static_cast<float>(m_Engine->DepthStencilBuffer->GetSizeY());

        XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
        auto& projF   = Engine::GAPI->GetProjectionMatrix();
        XMStoreFloat4x4( &cb.viewProjection, XMMatrixMultiply( view, XMLoadFloat4x4( &projF ) ) );
    } else {
        cb.enableHiZ  = 0;
        cb.hiZMipCount = 0;
        cb.hiZWidth    = 0.0f;
        cb.hiZHeight   = 0.0f;
        XMStoreFloat4x4( &cb.viewProjection, XMMatrixIdentity() );
    }

    m_CullConstantBuffer->UpdateBuffer( &cb );
    m_CullConstantBuffer->BindToComputeShader( 0 );

    // --- 3. Dispatch CS_CullVobs ---
    auto cullCS = m_Engine->ShaderManager->GetCShader( CShaderID::CS_CullVobs );
    if ( !cullCS )
        return XR_SUCCESS;
    cullCS->Apply();

    ID3D11ShaderResourceView* srvs[2] = {
        m_VobGPUBuffer->GetSRV(),
        m_SubmeshGPUBuffer->GetSRV()
    };
    context->CSSetShaderResources( 0, 2, srvs );

    if ( useHiZ ) {
        ID3D11ShaderResourceView* hiZSRV = m_Engine->m_HiZSRV.Get();
        context->CSSetShaderResources( 2, 1, &hiZSRV );
    }

    ID3D11UnorderedAccessView* uavs[2] = {
        m_InstanceBufferGPU->GetUAV(),
        m_MergedIndirectArgs->GetUnorderedAccessView().Get()
    };
    context->CSSetUnorderedAccessViews( 0, 2, uavs, nullptr );

    UINT numGroups = (static_cast<UINT>(m_Engine->m_StaticVobs.size()) + 63) / 64;
    context->Dispatch( numGroups, 1, 1 );

    // Unbind CS resources
    ID3D11ShaderResourceView* nullSRV[3] = { nullptr, nullptr, nullptr };
    ID3D11UnorderedAccessView* nullUAV[2] = { nullptr, nullptr };
    context->CSSetShaderResources( 0, 3, nullSRV );
    context->CSSetUnorderedAccessViews( 0, 2, nullUAV, nullptr );
    context->CSSetShader( nullptr, nullptr, 0 );

    // --- 4. Bind global geometry ---
    UINT strides[2] = { sizeof( ExVertexStruct ), sizeof( uint32_t ) };
    UINT offsets[2] = { 0, 0 };
    ID3D11Buffer* vbs[2] = {
        m_StaticGlobalVertexBuffer->GetVertexBuffer().Get(),
        m_GlobalInstanceIdBuffer->GetVertexBuffer().Get()
    };
    context->IASetVertexBuffers( 0, 2, vbs, strides, offsets );
    context->IASetIndexBuffer( m_StaticGlobalIndexBuffer->GetVertexBuffer().Get(),
                               VERTEX_INDEX_DXGI_FORMAT, 0 );
    context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // --- 5. Bind instance structured buffer to VS t1 ---
    ID3D11ShaderResourceView* instSRV = m_InstanceBufferGPU->GetSRV();
    context->VSSetShaderResources( 1, 1, &instSRV );

    // --- 6. Set vertex shader ---
    m_Engine->SetActiveVertexShader( VShaderID::VS_ExInstancedObjIndirectAtlas );
    m_Engine->SetupVS_ExMeshDrawCall();
    m_Engine->SetupVS_ExConstantBuffer();

    VS_ExConstantBuffer_Wind windBuff{};
    m_Engine->ApplyWindProps( windBuff );
    m_Engine->ActiveVS->GetConstantBuffer()[1]->UpdateBuffer( &windBuff );
    m_Engine->ActiveVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

    if ( bindPS )
        context->PSSetShaderResources( 4, 1, m_Engine->ReflectionCube.GetAddressOf() );

    m_Engine->ActiveVS->Apply();

    // --- 7. Draw per atlas group ---
    MaterialInfo defMaterial{};
    GSky* sky = Engine::GAPI->GetSky();

    for ( auto& group : m_AtlasDrawGroups ) {
        ID3D11ShaderResourceView* srv = m_TextureAtlasses[group.format].atlasSRV;
        if ( !srv )
            continue;

        const bool needsPS = bindPS || (group.format == DXGI_FORMAT_BC2_UNORM);

        if ( needsPS ) {
            context->PSSetShaderResources( 0, 1, &srv );

            if ( bindPS && group.format != DXGI_FORMAT_BC2_UNORM )
                m_Engine->SetActivePixelShader( PShaderID::PS_DiffuseAtlas );
            else
                m_Engine->SetActivePixelShader( PShaderID::PS_DiffuseAtlasAlphaTest );

            m_Engine->ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
                &Engine::GAPI->GetRendererState().GraphicsState );
            m_Engine->ActivePS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

            m_Engine->ActivePS->GetConstantBuffer()[1]->UpdateBuffer( &sky->GetAtmosphereCB() );
            m_Engine->ActivePS->GetConstantBuffer()[1]->BindToPixelShader( 1 );

            m_Engine->ActivePS->GetConstantBuffer()[2]->UpdateBuffer( &defMaterial.buffer );
            m_Engine->ActivePS->GetConstantBuffer()[2]->BindToPixelShader( 2 );

            m_Engine->OutdoorVobsConstantBuffer->BindToPixelShader( 3 );

            m_Engine->ActivePS->Apply();
        } else {
            context->PSSetShader( nullptr, nullptr, 0 );
        }

        DrawMultiIndexedInstancedIndirect(
            context.Get(),
            group.mergedArgsCount,
            m_MergedIndirectArgs->GetIndirectBuffer().Get(),
            group.mergedArgsOffset,
            sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) );
    }

    // Unbind instance buffer from VS
    ID3D11ShaderResourceView* nullVSSRV = nullptr;
    context->VSSetShaderResources( 1, 1, &nullVSSRV );

    return XR_SUCCESS;
}
