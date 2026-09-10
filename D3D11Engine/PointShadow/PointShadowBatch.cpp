#include "../pch.h"
#include "PointShadowBatch.h"

#include "../D3D11GraphicsEngine.h"
#include "../D3D11GShader.h"
#include "../D3D11PShader.h"
#include "../D3D11PipelineStateCache.h"
#include "../D3D11VShader.h"
#include "../D3D11VertexBuffer.h"
#include "../D3D7/MyDirectDrawSurface7.h"
#include "../Engine.h"
#include "../Frustum.h"
#include "../GfxTexture.h"
#include "../GothicAPI.h"
#include "../LightingResourceLog.h"
#include "../RenderToTextureBuffer.h"
#include "../WorldObjects.h"
#include "../zCBspTree.h"
#include "../zCMaterial.h"
#include "../zCTexture.h"
#include "../zCVob.h"
#include "../zCVobLight.h"
#include "PointShadowCasters.h"
#include "SkeletalCubeCasters.h"

extern bool RequiresNvidiaTiledShadowFaceFallback;

using Microsoft::WRL::ComPtr;
using PointShadowCasters::CasterPass;
using PointShadowCasters::CubeRenderScope;

namespace {
    constexpr DXGI_FORMAT VERTEX_INDEX_DXGI_FORMAT = sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    constexpr uint8_t kAllFaces = 0x3F;
    constexpr uint32_t kNoCaster = UINT32_MAX;
    // NPC attachments (weapons, torches) reach past the vob's bbox.
    constexpr float kSkeletalFacePad = 100.0f;

    /** How a pass rasterizes its six faces; each has its own vertex shaders. */
    enum class ECubeRaster : uint8_t {
        Layered,    // 6 instances routed by SV_RenderTargetArrayIndex
        Geometry,   // GS_Cubemap emits the 6 faces (UseLayeredRendering off)
        PerFace,    // NVIDIA fallback: one draw per face into its single-slice DSV
    };
    constexpr std::array<ECubeRaster, 3> kRasters = { ECubeRaster::Layered, ECubeRaster::Geometry, ECubeRaster::PerFace };

    struct RasterShaders {
        VShaderID Static;
        VShaderID Skeletal;
        VShaderID Node;
    };

    RasterShaders ShadersFor( ECubeRaster raster ) {
        switch ( raster ) {
        case ECubeRaster::Geometry: return { VShaderID::VS_ExCube, VShaderID::VS_ExSkeletalCube, VShaderID::VS_ExNodeCube };
        case ECubeRaster::PerFace:  return { VShaderID::VS_ExCubeFace, VShaderID::VS_ExSkeletalCubeFace, VShaderID::VS_ExNodeCubeFace };
        default:                    return { VShaderID::VS_ExLayered, VShaderID::VS_ExSkeletalLayered, VShaderID::VS_ExNodeLayered };
        }
    }

    struct CubePass {
        CubemapGSConstantBuffer Constants{};
        ComPtr<ID3D11DepthStencilView> ClearDSV;
        ComPtr<ID3D11DepthStencilView> DrawDSV;
        std::array<ComPtr<ID3D11DepthStencilView>, 6> FaceDSV;
        float Size = 0.0f;
        XMFLOAT3 Position{};
        ECubeRaster Raster = ECubeRaster::Layered;
        bool Clear = false;
        uint32_t FirstExcluded = 0;
        uint32_t NumExcluded = 0;
        // Per face for PerFace, [0] otherwise; re-allocated once the CB ring has moved past them.
        std::array<ConstantBufferAllocation, 6> CubeCB{};
        std::array<uint32_t, 6> CubeCBGeneration{};
    };

    /** One caster seen by one pass. */
    struct Visible {
        uint32_t Caster = 0;
        uint16_t Pass = 0;
        uint8_t FaceMask = kAllFaces;   // PerFace passes only: the faces the caster's bounds reach
        ECubeRaster Raster = ECubeRaster::Layered;
    };

    bool VisibleOrder( const Visible& a, const Visible& b ) {
        return std::tie( a.Raster, a.Caster, a.Pass ) < std::tie( b.Raster, b.Caster, b.Pass );
    }

    bool SameVisible( const Visible& a, const Visible& b ) {
        return a.Raster == b.Raster && a.Caster == b.Caster && a.Pass == b.Pass;
    }

    struct Phase {
        std::vector<CubePass> Passes;
        std::vector<Visible> World;
        std::vector<Visible> Vobs;
        std::vector<Visible> Skeletals;

        void Clear() {
            Passes.clear();
            World.clear();
            Vobs.clear();
            Skeletals.clear();
        }
    };

    struct StaticCaster {
        VS_ExConstantBuffer_PerInstance Instance;
        uint32_t FirstMesh = 0;
        uint32_t NumMeshes = 0;
    };

    enum class EWorldDraw : uint8_t { Range, FullMesh };

    struct WorldKey {
        const MeshInfo* Mesh = nullptr;
        uint32_t Offset = 0;
        uint32_t Count = 0;
        EWorldDraw Kind = EWorldDraw::Range;
        bool Alpha = false;
        bool operator==( const WorldKey& ) const = default;
    };

    struct WorldKeyHash {
        size_t operator()( const WorldKey& k ) const {
            size_t seed = std::hash<const MeshInfo*>{}( k.Mesh );
            seed ^= std::hash<uint64_t>{}( ( static_cast<uint64_t>( k.Offset ) << 32 ) | k.Count ) + 0x9e3779b9 + ( seed << 6 ) + ( seed >> 2 );
            return seed ^ ( ( static_cast<size_t>( k.Kind ) << 1 ) | static_cast<size_t>( k.Alpha ) );
        }
    };

    std::array<Phase, 2> s_Phases;
    std::vector<std::move_only_function<void()>> s_BoundaryActions;
    std::vector<const zCVob*> s_Excluded;

    // Casters deduped by pointer across every pass of both phases.
    gtl::flat_hash_map<WorldKey, uint32_t, WorldKeyHash> s_WorldIndex;
    std::vector<CasterMeshDraw> s_WorldDraws;
    gtl::flat_hash_map<const VobInfo*, uint32_t> s_VobIndex;
    std::vector<StaticCaster> s_Vobs;
    std::vector<CasterMeshDraw> s_VobMeshes;

    // Collection scratch, kept for its capacity.
    std::vector<WorldMeshSectionInfo*> s_Sections;
    std::vector<MeshDrawRange> s_Ranges;
    std::vector<VobInfo*> s_VobScratch;
    std::vector<SkeletalVobInfo*> s_MobScratch;

    ID3D11Buffer* NativeBuffer( GfxVertexBuffer* buffer ) {
        return buffer ? D3D11VertexBuffer::From( buffer )->GetVertexBuffer().Get() : nullptr;
    }

    /** The cube faces a box reaches: a 90-degree face frustum holds a sphere whose centre lies within radius*sqrt(2)
        of its four side planes. Conservative; the collector's range test stands in for the far plane. */
    uint8_t FaceMaskOf( const XMFLOAT3& light, const zTBBox3D& box, float pad ) {
        const XMVECTOR mn = XMLoadFloat3( &box.Min );
        const XMVECTOR mx = XMLoadFloat3( &box.Max );
        XMFLOAT3 d;
        XMStoreFloat3( &d, ( mn + mx ) * 0.5f - XMLoadFloat3( &light ) );
        const float k = ( XMVectorGetX( XMVector3Length( mx - mn ) ) * 0.5f + pad ) * 1.41421356f;
        const float ax = std::abs( d.x ), ay = std::abs( d.y ), az = std::abs( d.z );

        uint8_t mask = 0;
        if (  d.x + k >= ay &&  d.x + k >= az ) mask |= 1 << 0;
        if ( -d.x + k >= ay && -d.x + k >= az ) mask |= 1 << 1;
        if (  d.y + k >= ax &&  d.y + k >= az ) mask |= 1 << 2;
        if ( -d.y + k >= ax && -d.y + k >= az ) mask |= 1 << 3;
        if (  d.z + k >= ax &&  d.z + k >= ay ) mask |= 1 << 4;
        if ( -d.z + k >= ax && -d.z + k >= ay ) mask |= 1 << 5;
        return mask ? mask : kAllFaces;   // only a NaN box reaches no face at all
    }

    uint32_t AddWorldDraw( EWorldDraw kind, MeshInfo* mesh, uint32_t offset, uint32_t count, zCTexture* alphaTexture ) {
        const WorldKey key{ mesh, offset, count, kind, alphaTexture != nullptr };
        if ( auto it = s_WorldIndex.find( key ); it != s_WorldIndex.end() ) {
            return it->second;
        }

        CasterMeshDraw draw;
        draw.VertexBuffer = NativeBuffer( mesh->GetMeshVertexBuffer() );
        if ( kind == EWorldDraw::Range ) {
            draw.IndexBuffer = NativeBuffer( ShadowCasting::ShadowAwareIndexBuffer( mesh, alphaTexture != nullptr ) );
            if ( !draw.IndexBuffer ) return kNoCaster;
        } else if ( mesh->MeshIndexBuffer ) {
            draw.IndexBuffer = NativeBuffer( mesh->GetMeshIndexBuffer() );
        }
        if ( !draw.VertexBuffer ) return kNoCaster;
        draw.Count = count;
        draw.Offset = offset;
        draw.AlphaTexture = alphaTexture;

        const uint32_t index = static_cast<uint32_t>( s_WorldDraws.size() );
        s_WorldDraws.push_back( draw );
        s_WorldIndex.emplace( key, index );
        return index;
    }

    /** False when the range casts nothing: a non-default material, or an alpha-tested texture still streaming in. */
    bool AddWorldRange( Phase& phase, uint16_t pass, ECubeRaster raster, const MeshDrawRange& r, float alphaRef ) {
        if ( !r.Mesh || !r.Key.Info || r.Key.Info->MaterialType != MaterialInfo::MT_None ) return false;

        zCTexture* alphaTexture = nullptr;
        zCMaterial* mat = r.Key.Material;
        if ( mat && mat->GetTextureSingle() && ( mat->HasAlphaTest() || mat->GetTextureSingle()->HasAlphaChannel() ) ) {
            zCTexture* aniTex = alphaRef > 0.0f ? mat->GetAniTexture() : nullptr;
            if ( !aniTex || aniTex->GetCacheState() != zRES_CACHED_IN ) return false;
            alphaTexture = aniTex;
        }

        const uint32_t caster = AddWorldDraw( EWorldDraw::Range, r.Mesh, r.IndexOffset, r.IndexCount, alphaTexture );
        if ( caster == kNoCaster ) return false;
        phase.World.push_back( { caster, pass, kAllFaces, raster } );
        return true;
    }

    uint32_t AddVob( VobInfo* vob ) {
        if ( auto it = s_VobIndex.find( vob ); it != s_VobIndex.end() ) {
            return it->second;
        }

        StaticCaster caster;
        vob->UpdateVobConstantBuffer( caster.Instance );
        caster.FirstMesh = static_cast<uint32_t>( s_VobMeshes.size() );
        for ( auto const& [mat, meshes] : vob->VisualInfo->Meshes ) {
            zCTexture* alphaTexture = nullptr;
            if ( mat && mat->GetTextureSingle() ) {
                if ( ( mat->GetAlphaFunc() != zMAT_ALPHA_FUNC_NONE && mat->GetAlphaFunc() != zMAT_ALPHA_FUNC_MAT_DEFAULT )
                    || mat->GetTextureSingle()->HasAlphaChannel() ) {
                    zCTexture* aniTex = mat->GetAniTexture();
                    if ( aniTex && aniTex->GetCacheState() == zRES_CACHED_IN ) {
                        alphaTexture = aniTex;
                    }
                }
            }
            for ( auto const& mesh : meshes ) {
                CasterMeshDraw draw;
                draw.VertexBuffer = NativeBuffer( mesh->GetMeshVertexBuffer() );
                draw.IndexBuffer = NativeBuffer( ShadowCasting::ShadowAwareIndexBuffer( mesh.get(), alphaTexture != nullptr ) );
                if ( !draw.VertexBuffer || !draw.IndexBuffer ) continue;
                draw.Count = ShadowCasting::ShadowAwareIndexCount( mesh.get(), alphaTexture != nullptr );
                draw.AlphaTexture = alphaTexture;
                s_VobMeshes.push_back( draw );
            }
        }
        caster.NumMeshes = static_cast<uint32_t>( s_VobMeshes.size() ) - caster.FirstMesh;

        const uint32_t index = static_cast<uint32_t>( s_Vobs.size() );
        s_Vobs.push_back( caster );
        s_VobIndex.emplace( vob, index );
        return index;
    }

    void CollectCasters( Phase& phase, uint16_t passIndex, const CasterPass& pass ) {
        const CubePass& cube = phase.Passes[passIndex];
        const ECubeRaster raster = cube.Raster;
        const XMFLOAT3 lightPos = cube.Position;
        const uint32_t firstExcluded = cube.FirstExcluded;
        const uint32_t numExcluded = cube.NumExcluded;

        auto& rs = Engine::GAPI->GetRendererState();
        const XMVECTOR position = XMLoadFloat3( &lightPos );
        const XMVECTOR rangeSq = XMVectorReplicate( pass.Range * pass.Range );
        const bool indoor = pass.Light->IsIndoorVob;
        const bool isOutdoor = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;
        const float alphaRef = rs.GraphicsState.FF_AlphaRef;

        auto isExcluded = [&]( const zCVob* vob ) {
            const auto first = s_Excluded.begin() + firstExcluded;
            return std::find( first, first + numExcluded, vob ) != first + numExcluded;
        };
        auto outOfRange = [&]( const zCVob* vob ) {
            return XMVector3Greater( XMVector3LengthSq( position - vob->GetPositionWorldXM() ), rangeSq );
        };
        auto faceMaskOf = [&]( const zCVob* vob, float pad ) {
            return raster == ECubeRaster::PerFace ? FaceMaskOf( lightPos, vob->GetBBox(), pad ) : kAllFaces;
        };

        Frustum sphere;
        sphere.BuildCubemapFace( position, pass.Range, 0 );   // a cube frustum is a sphere
        bool sectionsCollected = false;
        auto sections = [&]() -> const std::vector<WorldMeshSectionInfo*>& {
            if ( !sectionsCollected ) {
                s_Sections.clear();
                Engine::GAPI->CollectVisibleSections( s_Sections, &sphere, true );
                sectionsCollected = true;
            }
            return s_Sections;
        };

        if ( ( pass.CasterMask & SHADOW_CASTER_WORLD ) && rs.RendererSettings.DrawWorldMesh ) {
            std::vector<MeshDrawRange>* cache = pass.WorldMeshCache;
            if ( cache && !cache->empty() ) {
                for ( const MeshDrawRange& r : *cache ) {
                    AddWorldRange( phase, passIndex, raster, r, alphaRef );
                }
            } else if ( rs.RendererSettings.FastShadows ) {
                for ( WorldMeshSectionInfo* section : sections() ) {
                    MeshInfo* mesh = section->FullStaticMesh;
                    if ( !mesh ) continue;
                    const uint32_t count = static_cast<uint32_t>( mesh->MeshIndexBuffer ? mesh->Indices.size() : mesh->Vertices.size() );
                    const uint32_t caster = AddWorldDraw( EWorldDraw::FullMesh, mesh, 0, count, nullptr );
                    if ( caster != kNoCaster ) phase.World.push_back( { caster, passIndex, kAllFaces, raster } );
                }
            } else {
                s_Ranges.clear();
                Engine::GAPI->CollectVisibleMeshRanges( sphere, true, s_Ranges );
                for ( const MeshDrawRange& r : s_Ranges ) {
                    if ( AddWorldRange( phase, passIndex, raster, r, alphaRef ) && cache ) {
                        cache->push_back( r );
                    }
                }
            }
        }

        // Only vobs still in the BSP lists are static: anything added after load or moved since draws in the
        // overlay, or every step it takes would invalidate the cube that just baked it.
        auto isDynamicVob = []( const VobInfo* vob ) { return vob->ParentBSPNodes.empty(); };

        if ( ( pass.CasterMask & SHADOW_CASTER_VOBS ) && rs.RendererSettings.DrawVOBs ) {
            std::list<VobInfo*>* cache = pass.VobCache;
            // The slot's baked-vob list is built from this cache, so a caster that has started moving leaves it.
            if ( cache ) cache->remove_if( isDynamicVob );
            const bool useCache = cache && !cache->empty();
            if ( !useCache ) {
                s_VobScratch.clear();
                for ( WorldMeshSectionInfo* section : sections() ) {
                    for ( VobInfo* vob : section->Vobs ) {
                        if ( !vob->VisualInfo ) continue;  // Seems to happen in Gothic 1
                        if ( isDynamicVob( vob ) ) continue;
                        // Rides an NPC - the animated pass owns it, and caching it here let a throwaway held
                        // item invalidate every nearby light's static cube when it despawned.
                        if ( ShadowCasting::IsAttachedToNpc( vob->Vob ) ) continue;
                        if ( !vob->Vob->GetShowVisual() ) continue;
                        // Don't render inside-vobs when the light is outside and vice-versa.
                        if ( isOutdoor && vob->Vob->IsIndoorVob() != indoor ) continue;
                        if ( outOfRange( vob->Vob ) || isExcluded( vob->Vob ) ) continue;
                        s_VobScratch.push_back( vob );
                    }
                }
                if ( cache ) cache->assign( s_VobScratch.begin(), s_VobScratch.end() );
            }

            auto add = [&]( VobInfo* vob ) {
                // Still being filled in on a worker thread (Extract3DSMeshFromVisual2Async).
                if ( !vob->VisualInfo->GetIsReady() ) return;
                phase.Vobs.push_back( { AddVob( vob ), passIndex, faceMaskOf( vob->Vob, 0.0f ), raster } );
            };
            if ( useCache ) {
                for ( VobInfo* vob : *cache ) add( vob );
            } else {
                for ( VobInfo* vob : s_VobScratch ) add( vob );
            }
        }

        auto addSkeletal = [&]( SkeletalVobInfo* vi ) {
            const uint32_t record = SkeletalCubeCasters::RecordFor( vi );
            if ( record == SkeletalCubeCasters::kNoRecord ) return;
            phase.Skeletals.push_back( { record, passIndex, faceMaskOf( vi->Vob, kSkeletalFacePad ), raster } );
        };

        if ( ( pass.CasterMask & SHADOW_CASTER_MOBS ) && rs.RendererSettings.DrawMobs ) {
            std::list<SkeletalVobInfo*>* cache = pass.MobCache;
            // Same for a MOB that started moving (a door, a chest lid): the animated pass draws it from then on.
            if ( cache ) cache->remove_if( []( const SkeletalVobInfo* mob ) { return ShadowCasting::IsAnimatedShadowCaster( mob ); } );
            const bool useCache = cache && !cache->empty();
            if ( !useCache ) {
                s_MobScratch.clear();
                for ( SkeletalVobInfo* mob : Engine::GAPI->GetSkeletalMeshVobs() ) {
                    if ( !mob->VisualInfo ) continue;  // Seems to happen in Gothic 1
                    // Animated or NPC-attached MOBs belong to the animated pass - see IsAnimatedShadowCaster.
                    if ( ShadowCasting::IsAnimatedShadowCaster( mob ) ) continue;
                    if ( !mob->Vob->GetShowVisual() ) continue;
                    if ( isOutdoor && mob->Vob->IsIndoorVob() != indoor ) continue;
                    // Anything with a skinned mesh is assumed to move; chests, chairs and beds don't have one.
                    if ( auto* skel = dynamic_cast<SkeletalMeshVisualInfo*>( mob->VisualInfo ); skel && !skel->SkeletalMeshes.empty() ) continue;
                    if ( outOfRange( mob->Vob ) || isExcluded( mob->Vob ) ) continue;
                    s_MobScratch.push_back( mob );
                }
                if ( cache ) cache->assign( s_MobScratch.begin(), s_MobScratch.end() );
            }
            if ( useCache ) {
                for ( SkeletalVobInfo* mob : *cache ) addSkeletal( mob );
            } else {
                for ( SkeletalVobInfo* mob : s_MobScratch ) addSkeletal( mob );
            }
        }

        if ( ( pass.CasterMask & SHADOW_CASTER_ANIMATED ) && rs.RendererSettings.DrawSkeletalMeshes ) {
            for ( SkeletalVobInfo* vi : Engine::GAPI->GetAnimatedSkeletalMeshVobs() ) {
                if ( !vi->VisualInfo ) continue;  // Seems to happen in Gothic 1
                // Ghosts shouldn't have shadows
                if ( vi->Vob->GetVisualAlpha() && vi->Vob->GetVobTransparency() < 0.7f ) continue;
                if ( outOfRange( vi->Vob ) ) continue;
                if ( isOutdoor && vi->Vob->IsIndoorVob() != indoor ) continue;
                if ( isExcluded( vi->Vob ) ) continue;
                addSkeletal( vi );
            }
        }

        // Dropped, carried and moved items - the static gather leaves them out, so they cast here.
        if ( ( pass.CasterMask & SHADOW_CASTER_ANIMATED ) && rs.RendererSettings.DrawVOBs ) {
            for ( VobInfo* vi : Engine::GAPI->GetDynamicallyAddedVobs() ) {
                if ( !vi || !vi->Vob || !vi->VisualInfo || !vi->VisualInfo->GetIsReady() ) continue;
                if ( !vi->Vob->GetShowVisual() ) continue;
                const float reach = pass.Range + vi->VisualInfo->MeshSize * 0.5f;
                if ( XMVector3Greater( XMVector3LengthSq( position - vi->Vob->GetPositionWorldXM() ), XMVectorReplicate( reach * reach ) ) ) continue;
                if ( isOutdoor && vi->Vob->IsIndoorVob() != indoor ) continue;
                if ( isExcluded( vi->Vob ) ) continue;
                phase.Vobs.push_back( { AddVob( vi ), passIndex, faceMaskOf( vi->Vob, 0.0f ), raster } );
            }
        }
    }

    std::span<const Visible> RasterGroup( const std::vector<Visible>& seen, ECubeRaster raster ) {
        const auto lo = std::lower_bound( seen.begin(), seen.end(), raster,
            []( const Visible& v, ECubeRaster r ) { return v.Raster < r; } );
        const auto hi = std::upper_bound( lo, seen.end(), raster,
            []( ECubeRaster r, const Visible& v ) { return r < v.Raster; } );
        return std::span<const Visible>( lo, hi );
    }

    /** Calls fn once per caster with the run of passes that see it. */
    template<typename Fn>
    void ForEachCaster( std::span<const Visible> group, Fn&& fn ) {
        for ( size_t i = 0; i < group.size(); ) {
            size_t end = i + 1;
            while ( end < group.size() && group[end].Caster == group[i].Caster ) ++end;
            fn( group[i].Caster, group.subspan( i, end - i ) );
            i = end;
        }
    }

    /** Redundant-bind filter for one flush: shaders, material, buffers and the pass target. */
    class CasterDrawState {
    public:
        explicit CasterDrawState( D3D11GraphicsEngine* engine )
            : m_Engine( engine ),
              m_CubeShadow( engine->GetShaderManager().GetPShader( PShaderID::PS_CubeShadow ).get() ),
              m_White( engine->GetWhiteTexture() ) {}

        D3D11GraphicsEngine* Engine() const { return m_Engine; }

        /** Binds a family's vertex shader, plus GS_Cubemap for Geometry. */
        void UseVertexShader( VShaderID shader, ECubeRaster raster ) {
            m_Engine->SetActiveVertexShader( shader );
            if ( raster == ECubeRaster::Geometry ) {
                m_Engine->SetActiveGShader( GShaderID::GS_Cubemap );
                m_Engine->GetActiveGS()->Apply();
            } else {
                Context()->GSSetShader( nullptr, nullptr, 0 );
            }
            m_Engine->SetupVS_ExMeshDrawCall();   // also re-applies the active PS_CubeShadow
            m_PixelShaderBound = true;
            m_CubeCB = {};                        // the GS slot needs it again after a family switch
            m_VertexBuffer = nullptr;
            m_IndexBuffer = nullptr;
        }

        void BindMaterial( zCTexture* alphaTexture ) {
            if ( !alphaTexture ) {
                // Opaque: the cube keeps the rasterizer's own depth, so no pixel shader at all.
                if ( m_PixelShaderBound ) {
                    D3D11PipelineStateCache::SetPixelShader( Context(), nullptr );
                    m_PixelShaderBound = false;
                }
                return;
            }
            if ( !m_PixelShaderBound ) {
                m_CubeShadow->Apply();
                m_PixelShaderBound = true;
            }
            MyDirectDrawSurface7* surface = alphaTexture->GetSurface();
            GfxTexture* texture = surface ? surface->GetEngineTexture() : nullptr;
            if ( !texture ) texture = m_White;
            if ( texture != m_Texture ) {
                texture->BindToPixelShader( 0 );
                m_Texture = texture;
            }
        }

        void BindBuffers( const CasterMeshDraw& draw, UINT stride ) {
            if ( draw.VertexBuffer != m_VertexBuffer || stride != m_Stride ) {
                const UINT offset = 0;
                Context()->IASetVertexBuffers( 0, 1, &draw.VertexBuffer, &stride, &offset );
                m_VertexBuffer = draw.VertexBuffer;
                m_Stride = stride;
            }
            if ( draw.IndexBuffer && draw.IndexBuffer != m_IndexBuffer ) {
                Context()->IASetIndexBuffer( draw.IndexBuffer, VERTEX_INDEX_DXGI_FORMAT, 0 );
                m_IndexBuffer = draw.IndexBuffer;
            }
        }

        const ConstantBufferAllocation& CubeCB( CubePass& pass, UINT face ) {
            ConstantBufferPool* pool = m_Engine->GetConstantBufferPool();
            if ( pass.CubeCBGeneration[face] != pool->GetGeneration() ) {
                CubemapGSConstantBuffer constants = pass.Constants;
                constants.PCR_Face = face;
                pass.CubeCB[face] = m_Engine->AllocateDynamicCB( &constants );
                pass.CubeCBGeneration[face] = pool->GetGeneration();
            }
            return pass.CubeCB[face];
        }

        void BindTarget( CubePass& pass, UINT face ) {
            ID3D11DepthStencilView* dsv = pass.Raster == ECubeRaster::PerFace ? pass.FaceDSV[face].Get() : pass.DrawDSV.Get();
            if ( dsv != m_DSV ) {
                Context()->OMSetRenderTargets( 0, nullptr, dsv );
                m_DSV = dsv;
            }
            if ( pass.Size != m_ViewportSize ) {
                const D3D11_VIEWPORT viewport = { 0.0f, 0.0f, pass.Size, pass.Size, 0.0f, 1.0f };
                Context()->RSSetViewports( 1, &viewport );
                m_ViewportSize = pass.Size;
            }
            const ConstantBufferAllocation& cb = CubeCB( pass, face );
            if ( !( cb == m_CubeCB ) ) {
                m_Engine->BindDynamicCBToVertexShader( 3, cb );
                if ( pass.Raster == ECubeRaster::Geometry ) {
                    m_Engine->BindDynamicCBToGeometryShader( 2, cb );
                }
                m_CubeCB = cb;
            }
        }

        void UnbindTarget() {
            Context()->OMSetRenderTargets( 0, nullptr, nullptr );
            m_DSV = nullptr;
        }

        /** Draws into every pass (and PerFace face) that sees the caster; a pass excluding slotVob is skipped. */
        template<typename DrawFn>
        void DrawIntoTargets( std::span<const Visible> seen, std::vector<CubePass>& passes, const zCVob* slotVob, DrawFn&& draw ) {
            for ( const Visible& v : seen ) {
                CubePass& pass = passes[v.Pass];
                if ( slotVob && Excludes( pass, slotVob ) ) continue;
                if ( pass.Raster == ECubeRaster::PerFace ) {
                    for ( UINT face = 0; face < 6; ++face ) {
                        if ( ( v.FaceMask & ( 1u << face ) ) == 0 ) continue;
                        BindTarget( pass, face );
                        draw( 1u );
                    }
                } else {
                    BindTarget( pass, 0 );
                    draw( pass.Raster == ECubeRaster::Layered ? 6u : 1u );
                }
            }
        }

        void Draw( const CasterMeshDraw& draw, UINT instances ) {
            if ( draw.IndexBuffer ) {
                Context()->DrawIndexedInstanced( draw.Count, instances, draw.Offset, 0, 0 );
            } else {
                Context()->DrawInstanced( draw.Count, instances, draw.Offset, 0 );
            }
            m_Triangles += draw.Count / 3;
        }

        unsigned int Triangles() const { return m_Triangles; }

    private:
        auto Context() const { return m_Engine->GetContext().Get(); }

        static bool Excludes( const CubePass& pass, const zCVob* vob ) {
            const auto first = s_Excluded.begin() + pass.FirstExcluded;
            return std::find( first, first + pass.NumExcluded, vob ) != first + pass.NumExcluded;
        }

        D3D11GraphicsEngine* m_Engine;
        D3D11PShader* m_CubeShadow;
        GfxTexture* m_White;
        GfxTexture* m_Texture = nullptr;
        bool m_PixelShaderBound = false;
        ID3D11Buffer* m_VertexBuffer = nullptr;
        ID3D11Buffer* m_IndexBuffer = nullptr;
        UINT m_Stride = 0;
        ID3D11DepthStencilView* m_DSV = nullptr;
        float m_ViewportSize = -1.0f;
        ConstantBufferAllocation m_CubeCB{};
        unsigned int m_Triangles = 0;
    };

    void DrawWorld( CasterDrawState& state, Phase& phase, ECubeRaster raster ) {
        const std::span<const Visible> group = RasterGroup( phase.World, raster );
        if ( group.empty() ) return;
        D3D11GraphicsEngine* g = state.Engine();

        state.UseVertexShader( ShadersFor( raster ).Static, raster );
        VS_ExConstantBuffer_PerInstance identity;
        XMStoreFloat4x4( &identity.World, XMMatrixIdentity() );
        identity.Color = 0xFFFFFFFF;
        g->BindDynamicCBToVertexShader( g->GetActiveVS()->GetInputIndex( "Matrices_PerInstances" ), g->AllocateDynamicCB( &identity ) );

        ForEachCaster( group, [&]( uint32_t caster, std::span<const Visible> seen ) {
            const CasterMeshDraw& draw = s_WorldDraws[caster];
            state.BindMaterial( draw.AlphaTexture );
            state.BindBuffers( draw, sizeof( ExVertexStruct ) );
            state.DrawIntoTargets( seen, phase.Passes, nullptr, [&]( UINT instances ) { state.Draw( draw, instances ); } );
        } );
    }

    void DrawVobs( CasterDrawState& state, Phase& phase, ECubeRaster raster ) {
        const std::span<const Visible> group = RasterGroup( phase.Vobs, raster );
        if ( group.empty() ) return;
        D3D11GraphicsEngine* g = state.Engine();

        state.UseVertexShader( ShadersFor( raster ).Static, raster );
        const int instanceSlot = g->GetActiveVS()->GetInputIndex( "Matrices_PerInstances" );

        ForEachCaster( group, [&]( uint32_t index, std::span<const Visible> seen ) {
            const StaticCaster& vob = s_Vobs[index];
            g->BindDynamicCBToVertexShader( instanceSlot, g->AllocateDynamicCB( &vob.Instance ) );
            for ( uint32_t m = vob.FirstMesh; m < vob.FirstMesh + vob.NumMeshes; ++m ) {
                const CasterMeshDraw& draw = s_VobMeshes[m];
                state.BindMaterial( draw.AlphaTexture );
                state.BindBuffers( draw, sizeof( ExVertexStruct ) );
                state.DrawIntoTargets( seen, phase.Passes, nullptr, [&]( UINT instances ) { state.Draw( draw, instances ); } );
            }
        } );
    }

    void DrawSkeletals( CasterDrawState& state, Phase& phase, ECubeRaster raster, bool bonesReady ) {
        const std::span<const Visible> group = RasterGroup( phase.Skeletals, raster );
        if ( group.empty() ) return;
        D3D11GraphicsEngine* g = state.Engine();
        D3D11SkeletalPoseCache& poses = g->GetSkeletalPoseCache();
        const bool structuredBones = !FeatureLevel10Compatibility;
        const RasterShaders shaders = ShadersFor( raster );

        // The structured-bone shaders have no cbuffer fallback, so a failed upload drops the bodies.
        if ( bonesReady ) {
            state.UseVertexShader( shaders.Skeletal, raster );
            auto& vs = g->GetActiveVS();
            const int instanceSlot = vs->GetInputIndex( "Matrices_PerInstances" );
            const int boneRangeSlot = vs->GetInputIndex( "BoneTransformRange" );
            const int bonesSlot = vs->GetInputIndex( "BoneTransforms" );
            if ( structuredBones && bonesSlot >= 0 ) {
                ID3D11ShaderResourceView* bonesSRV = poses.GetBonesSRV();
                g->GetContext()->VSSetShaderResources( bonesSlot, 1, &bonesSRV );
            }

            ForEachCaster( group, [&]( uint32_t index, std::span<const Visible> seen ) {
                const SkeletalCubeCasters::Record& record = SkeletalCubeCasters::GetRecord( index );
                const std::span<const CasterMeshDraw> meshes = SkeletalCubeCasters::BodyMeshes( record );
                if ( meshes.empty() ) return;

                g->BindDynamicCBToVertexShader( instanceSlot, g->AllocateDynamicCB( &record.Instance ) );
                if ( structuredBones ) {
                    const VS_ExConstantBuffer_SkeletalBoneRange range = { record.Pose.Offset, record.Pose.Offset, record.Pose.Count, 1u };
                    g->BindDynamicCBToVertexShader( boneRangeSlot, g->AllocateDynamicCB( &range ) );
                } else {
                    const std::span<const XMFLOAT4X4> bones = poses.Bones( record.Pose );
                    g->BindDynamicCBToVertexShader( bonesSlot, g->AllocateDynamicCB( bones.data(),
                        sizeof( XMFLOAT4X4 ) * std::min<uint32_t>( record.Pose.Count, NUM_MAX_BONES ) ) );
                }

                for ( const CasterMeshDraw& draw : meshes ) {
                    state.BindMaterial( draw.AlphaTexture );
                    state.BindBuffers( draw, sizeof( ExSkelVertexStruct ) );
                    state.DrawIntoTargets( seen, phase.Passes, nullptr, [&]( UINT instances ) { state.Draw( draw, instances ); } );
                }
            } );
        }

        state.UseVertexShader( shaders.Node, raster );
        const int nodeInstanceSlot = g->GetActiveVS()->GetInputIndex( "Matrices_PerInstances" );
        ForEachCaster( group, [&]( uint32_t index, std::span<const Visible> seen ) {
            const SkeletalCubeCasters::Record& record = SkeletalCubeCasters::GetRecord( index );
            for ( const SkeletalCubeCasters::AttachmentDraw& attachment : SkeletalCubeCasters::Attachments( record ) ) {
                g->BindDynamicCBToVertexShader( nodeInstanceSlot, g->AllocateDynamicCB( &attachment.Instance ) );
                for ( const CasterMeshDraw& draw : SkeletalCubeCasters::AttachmentMeshes( attachment ) ) {
                    state.BindMaterial( draw.AlphaTexture );
                    state.BindBuffers( draw, sizeof( ExVertexStruct ) );
                    state.DrawIntoTargets( seen, phase.Passes, attachment.SlotVob, [&]( UINT instances ) { state.Draw( draw, instances ); } );
                }
            }
        } );
    }

    void SortVisible( std::vector<Visible>& seen ) {
        std::sort( seen.begin(), seen.end(), VisibleOrder );
        // A MOB can be both on the animated list and in a stale MOB cache of the same pass.
        seen.erase( std::unique( seen.begin(), seen.end(), SameVisible ), seen.end() );
    }

    void DrawPhase( CasterDrawState& state, Phase& phase ) {
        if ( phase.Passes.empty() ) return;
        auto _ = state.Engine()->RecordGraphicsEvent( GE_NAME( "PointShadowBatch::DrawPhase" ) );
        auto context = state.Engine()->GetContext().Get();

        for ( CubePass& pass : phase.Passes ) {
            if ( pass.Clear ) {
                if ( pass.ClearDSV ) {
                    context->ClearDepthStencilView( pass.ClearDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );
                } else {
                    for ( auto& face : pass.FaceDSV ) {
                        if ( face ) context->ClearDepthStencilView( face.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );
                    }
                }
            }
            // Up front, so the per-draw binds below almost never allocate.
            const UINT faces = pass.Raster == ECubeRaster::PerFace ? 6 : 1;
            for ( UINT face = 0; face < faces; ++face ) {
                state.CubeCB( pass, face );
            }
        }

        SortVisible( phase.World );
        SortVisible( phase.Vobs );
        SortVisible( phase.Skeletals );

        const bool bonesReady = FeatureLevel10Compatibility || state.Engine()->GetSkeletalPoseCache().Flush();
        for ( ECubeRaster raster : kRasters ) {
            DrawWorld( state, phase, raster );
            DrawVobs( state, phase, raster );
            DrawSkeletals( state, phase, raster, bonesReady );
        }
    }
}

namespace PointShadowBatch {

    void Begin() {
        for ( Phase& phase : s_Phases ) phase.Clear();
        s_BoundaryActions.clear();
        s_Excluded.clear();
        s_WorldIndex.clear();
        s_WorldDraws.clear();
        s_VobIndex.clear();
        s_Vobs.clear();
        s_VobMeshes.clear();
        SkeletalCubeCasters::BeginPass();
    }

    void Queue( const CubeRenderScope& scope, const CasterPass& pass ) {
        if ( !pass.Light || !pass.Light->Vob || !pass.Target || pass.Phase >= s_Phases.size() ) return;
        Phase& phase = s_Phases[pass.Phase];
        if ( phase.Passes.size() >= UINT16_MAX ) {
            static bool s_overflowReported = false;
            if ( !s_overflowReported ) {
                s_overflowReported = true;
                Logging::Wrn( "PointShadowBatch: more than {} point-light passes in one phase; the rest are dropped", UINT16_MAX );
            }
            return;
        }

        const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
        const bool absoluteSlice = PointShadowCasters::UsesAbsoluteSliceIndexing( pass.Target );

        CubePass cube;
        cube.Constants = scope.Constants();
        cube.Constants.PCR_SliceBase = absoluteSlice ? pass.Target->GetBaseArraySlice() : 0u;
        cube.Constants.PCR_Face = 0;
        cube.Raster = ( !absoluteSlice && RequiresNvidiaTiledShadowFaceFallback && pass.TargetIsSharedArray )
            ? ECubeRaster::PerFace
            : ( settings.DebugSettings.FeatureSet.UseLayeredRendering ? ECubeRaster::Layered : ECubeRaster::Geometry );

        // Drawing through the whole array leaves no view offset; the clear stays on this light's window.
        cube.ClearDSV = pass.Target->GetDepthStencilView();
        cube.DrawDSV = absoluteSlice ? pass.Target->GetArrayDepthStencilView() : cube.ClearDSV;
        bool hasTarget = cube.DrawDSV != nullptr;
        if ( cube.Raster == ECubeRaster::PerFace ) {
            hasTarget = true;
            for ( UINT face = 0; face < 6; ++face ) {
                cube.FaceDSV[face] = pass.Target->GetDSVCubemapFace( face );
                hasTarget = hasTarget && cube.FaceDSV[face] != nullptr;
            }
        }

        // No DSV means the whole pass would draw into nothing and the light samples stale depth.
        static bool s_targetReported = false;
        if ( !LightingLog::RequireOnce( hasTarget ? pass.Target : nullptr, s_targetReported, std::format(
            "Point-light shadow cube DSV ({}^2 target, raster {})", pass.Target->GetSizeX(), static_cast<int>( cube.Raster ) ) ) ) {
            return;
        }

        cube.Size = static_cast<float>( pass.Target->GetSizeX() );
        cube.Position = pass.Light->Vob->GetPositionWorld();
        cube.Clear = pass.ClearDepth;
        cube.FirstExcluded = static_cast<uint32_t>( s_Excluded.size() );
        PointShadowCasters::CollectExcludedVobs( pass.Light, s_Excluded );
        cube.NumExcluded = static_cast<uint32_t>( s_Excluded.size() ) - cube.FirstExcluded;

        const uint16_t passIndex = static_cast<uint16_t>( phase.Passes.size() );
        phase.Passes.push_back( std::move( cube ) );
        CollectCasters( phase, passIndex, pass );
    }

    void AtPhaseBoundary( std::move_only_function<void()> action ) {
        s_BoundaryActions.push_back( std::move( action ) );
    }

    void Flush() {
        if ( s_Phases[0].Passes.empty() && s_Phases[1].Passes.empty() ) {
            for ( auto& action : s_BoundaryActions ) action();
            Begin();
            return;
        }

        ZoneScopedN( "PointShadowBatch::Flush" );
        D3D11GraphicsEngine* g = AsD3D11Engine( Engine::GraphicsEngine );
        auto _ = g->RecordGraphicsEvent( GE_NAME( "PointShadowBatch::Flush" ) );
        auto& rs = Engine::GAPI->GetRendererState();
        auto context = g->GetContext().Get();

        D3D11_VIEWPORT savedViewport{};
        UINT numViewports = 1;
        context->RSGetViewports( &numViewports, &savedViewport );
        const bool savedColorWrites = rs.BlendState.ColorWritesEnabled;
        const bool savedDepthClip = rs.RasterizerState.DepthClipEnable;
        const bool savedCubeSwitch = ( rs.GraphicsState.FF_GSwitches & GSWITCH_CUBE_SHADOW ) != 0;

        g->SetRenderingStage( DES_SHADOWMAP_CUBE );
        rs.RasterizerState.SetDefault();
        rs.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
        rs.RasterizerState.DepthClipEnable = true;
        rs.RasterizerState.SetDirty();
        rs.DepthState.SetDefault();
        rs.DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::ECompareFunc::CF_COMPARISON_LESS_EQUAL;
        rs.DepthState.SetDirty();
        // Depth only: the casters write no SV_Depth.
        rs.BlendState.ColorWritesEnabled = false;
        rs.BlendState.SetDirty();
        rs.GraphicsState.SetGraphicsSwitch( GSWITCH_CUBE_SHADOW, true );

        ID3D11ShaderResourceView* const nullSRVs[6] = {};
        context->PSSetShaderResources( 0, 6, nullSRVs );
        g->SetActivePixelShader( PShaderID::PS_CubeShadow );
        // PS_CubeShadow's alpha test reads FF_AlphaRef.
        g->GetShaderManager().GetPShader( PShaderID::PS_CubeShadow )->UpdateBuffer(
            "FFPipelineConstantBuffer", &rs.GraphicsState, sizeof( rs.GraphicsState ) );
        g->SetActiveVertexShader( VShaderID::VS_ExLayered );
        g->SetupVS_ExConstantBuffer();

        CasterDrawState state( g );
        DrawPhase( state, s_Phases[0] );
        if ( !s_BoundaryActions.empty() ) {
            state.UnbindTarget();   // an action may copy into a cube still bound as the depth target
            for ( auto& action : s_BoundaryActions ) action();
        }
        DrawPhase( state, s_Phases[1] );
        state.UnbindTarget();

        context->RSSetViewports( 1, &savedViewport );
        context->GSSetShader( nullptr, nullptr, 0 );
        g->SetActiveVertexShader( VShaderID::VS_Ex );
        rs.BlendState.ColorWritesEnabled = savedColorWrites;
        rs.BlendState.SetDirty();
        rs.RasterizerState.DepthClipEnable = savedDepthClip;
        rs.RasterizerState.SetDirty();
        rs.GraphicsState.SetGraphicsSwitch( GSWITCH_CUBE_SHADOW, savedCubeSwitch );
        Engine::GAPI->SetFarPlane( rs.RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE );
        g->SetRenderingStage( DES_MAIN );

        rs.RendererInfo.FrameDrawnTriangles += state.Triangles();
        Begin();
    }

} // namespace PointShadowBatch
