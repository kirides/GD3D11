#include "pch.h"
#include "InventoryRenderer.h"
#include "BaseGraphicsEngine.h"
#include "D3D7/MyDirectDrawSurface7.h"
#include "Engine.h"
#include "GInventory.h"
#include "GothicAPI.h"
#include "HookedFunctions.h"
#include "UIRenderer2D.h"
#include "zCMaterial.h"
#include "zCModel.h"
#include "zCTexture.h"
#include "zCVisual.h"
#include "zCVob.h"

#if GD3D11_INVENTORY_RENDERER
namespace {
    constexpr size_t kMaxItemBones = 96;   // NUM_MAX_BONES of the item shaders

    enum class EItemVisual { Unsupported, Mesh, MorphMesh, Model };

    template <typename T>
    T& Field( void* object, unsigned int offset ) {
        return *reinterpret_cast<T*>( static_cast<char*>( object ) + offset );
    }

    void LogOnce( bool& logged, const char* message ) {
        if ( logged ) return;
        logged = true;
        LogWarn() << "InventoryRenderer: " << message;
    }
    // Built once by Gothic's own constructors and never destroyed; lives as long as the process.
    alignas(16) uint8_t s_CameraStorage[0x1000];
    alignas(16) uint8_t s_CameraVobStorage[0x200];
    void* s_Camera = nullptr;
    zCVob* s_CameraVob = nullptr;

    bool EnsureCamera() {
        if ( s_Camera ) return true;

        void*& activeCam = *reinterpret_cast<void**>( GothicMemoryLocations::zCCamera::Var_ActiveCam );
        const void* savedCam = activeCam;
        reinterpret_cast<void*( __thiscall* )( void* )>( GothicMemoryLocations::zCCamera::Constructor )( s_CameraStorage );
        reinterpret_cast<void*( __thiscall* )( void* )>( GothicMemoryLocations::zCVob::Constructor )( s_CameraVobStorage );
        activeCam = const_cast<void*>( savedCam );

        s_Camera = s_CameraStorage;
        s_CameraVob = reinterpret_cast<zCVob*>( s_CameraVobStorage );
        Field<zCVob*>( s_Camera, GothicMemoryLocations::zCCamera::Offset_ConnectedVob ) = s_CameraVob;
        return true;
    }

#if defined(BUILD_GOTHIC_1_CLASSIC)
    // Read straight from the file: zCOption::ReadReal would add the section to every Gothic.ini.
    float ReadAdvInventoryScale( const char* key ) {
        const std::string ini = Engine::GAPI->GetStartDirectory() + "\\system\\Gothic.ini";
        char buffer[32];
        if ( !::GetPrivateProfileStringA( "ADV_INVENTORY", key, "", buffer, sizeof( buffer ), ini.c_str() ) ) return 1.0f;
        const float scale = static_cast<float>( std::atof( buffer ) );
        LogInfo() << "InventoryRenderer: [ADV_INVENTORY] " << key << " = " << scale;
        return scale > 0.0f ? scale : 1.0f;
    }

    /** G1's counterpart of G2's zInventoryItemsDistanceScale, applied by Union's inventory inside the RenderItem we skip. */
    float GetItemDistanceScale( float addon ) {
        static const float itemScale = ReadAdvInventoryScale( "renderItemScaleMultiplier" );
        static const float activeScale = ReadAdvInventoryScale( "renderActiveItemScaleMultiplier" );
        return addon != 0.0f ? activeScale : itemScale;
    }
#endif

    /** Places the camera exactly like oCItem::RenderItem does, without activating it. */
    bool PlaceCamera( void* item, void* viewItem, float addon, XMMATRIX& clipFromWorld, zTViewportData& viewport ) {
        EnsureCamera();

        void*& activeCam = *reinterpret_cast<void**>( GothicMemoryLocations::zCCamera::Var_ActiveCam );
        const void* savedCam = activeCam;
        // UpdateViewport pushes a projection through zrenderer; the UI after us must not see it.
        GothicRendererState& rs = Engine::GAPI->GetRendererState();
        const XMFLOAT4X4 savedProj = rs.TransformState.TransformProj;
        const XMFLOAT4X4 savedProjUnjittered = rs.TransformState.TransformProjUnjittered;

        // Vanilla places a fresh camera: no viewport yet, so SetFOV(45) takes its 4:3 fallback.
        zTViewportData& vp = Field<zTViewportData>( s_Camera, GothicMemoryLocations::zCCamera::Offset_VpData );
        vp.xDim = 0;
        reinterpret_cast<void( __thiscall* )( void*, void*, float )>( GothicMemoryLocations::oCItem::RenderItemPlaceCamera )( item, s_Camera, addon );

#if defined(BUILD_GOTHIC_1_CLASSIC)
        // The camera sits at (x, y, -dist) looking at the origin; scaling z is G2's dist *= scale.
        if ( const float distanceScale = GetItemDistanceScale( addon ); distanceScale != 1.0f ) {
            XMFLOAT3 position = s_CameraVob->GetPositionWorld();
            position.z *= distanceScale;
            reinterpret_cast<void( __thiscall* )( void*, const XMFLOAT3& )>( GothicMemoryLocations::zCVob::SetPositionWorld )( s_CameraVob, position );
        }
#endif

        Field<void*>( s_Camera, GothicMemoryLocations::zCCamera::Offset_TargetView ) = viewItem;
        HookedFunctions::OriginalFunctions.original_zCCamera__UpdateViewport( s_Camera );
        Field<void*>( s_Camera, GothicMemoryLocations::zCCamera::Offset_TargetView ) = nullptr;

        activeCam = const_cast<void*>( savedCam );
        rs.TransformState.TransformProj = savedProj;
        rs.TransformState.TransformProjUnjittered = savedProjUnjittered;

        if ( vp.xDim <= 0 || vp.yDim <= 0 ) return false;
        viewport = vp;

        const float fovH = Field<float>( s_Camera, GothicMemoryLocations::zCCamera::Offset_FovH );
        const float fovV = Field<float>( s_Camera, GothicMemoryLocations::zCCamera::Offset_FovV );
        const float xScale = 1.0f / std::tan( fovH * 0.5f );
        const float yScale = 1.0f / std::tan( fovV * 0.5f );

        // Reversed depth with an infinite far plane and near = 1, as GothicAPI::GetProjectionMatrix.
        const XMMATRIX proj(
            xScale, 0.0f, 0.0f, 0.0f,
            0.0f, yScale, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            0.0f, 0.0f, 1.0f, 0.0f );
        const XMMATRIX view = XMMatrixInverse( nullptr, s_CameraVob->GetWorldMatrixXM() );
        clipFromWorld = XMMatrixMultiply( proj, view );
        return true;
    }

    void RotateItem( void* item, float addon ) {
        const XMFLOAT3 origin( 0.0f, 0.0f, 0.0f );
        reinterpret_cast<void( __thiscall* )( void*, const XMFLOAT3& )>( GothicMemoryLocations::zCVob::SetPositionWorld )( item, origin );

#if defined(BUILD_GOTHIC_1_CLASSIC)
        // SystemPack's animated inventory lives inside the RenderItem body we skip; GD3D11 reroutes it here.
        if ( addon != 0.0f && HookedFunctionInfo::AnimatedInventoryPatched ) {
            HookedFunctionInfo::hooked_RotateInInventory( reinterpret_cast<DWORD>( item ) );
            return;
        }
#endif
        if ( addon != 0.0f ) {
            reinterpret_cast<void( __thiscall* )( void* )>( GothicMemoryLocations::oCItem::RotateInInventory )( item );
        } else {
            reinterpret_cast<void( __thiscall* )( void*, int )>( GothicMemoryLocations::oCItem::RotateForInventory )( item, 1 );
        }
    }

    EItemVisual ClassifyVisual( zCVisual* visual ) {
        for ( int e = 0; ; ++e ) {
            const char* ext = visual->GetFileExtension( e );
            if ( !ext || !*ext ) break;
            if ( strcmp( ext, ".3DS" ) == 0 ) return EItemVisual::Mesh;
            if ( strcmp( ext, ".MMS" ) == 0 ) return EItemVisual::MorphMesh;
            if ( strcmp( ext, ".MDS" ) == 0 || strcmp( ext, ".ASC" ) == 0 ) return EItemVisual::Model;
        }
        return EItemVisual::Unsupported;
    }

    GfxTexture* ResolveTexture( zCMaterial* material ) {
        zCTexture* texture = material ? material->GetAniTexture() : nullptr;
        if ( !texture || texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) return nullptr;
        MyDirectDrawSurface7* surface = texture->GetSurface();
        return surface ? surface->GetEngineTexture() : nullptr;
    }

    XMFLOAT4X4 StoreMatrix( FXMMATRIX m ) {
        XMFLOAT4X4 out;
        XMStoreFloat4x4( &out, m );
        return out;
    }

    /** Every drawable sub-mesh of a static visual under one instance, created on the first draw. */
    void RecordMeshVisual( UIRenderer2D& ui, MeshVisualInfo* visual, FXMMATRIX clipFromObject ) {
        if ( !visual || !visual->GetIsReady() ) return;
        Engine::GraphicsEngine->OnInventoryVisualUsed( visual );

        uint32_t instance = UINT32_MAX;
        for ( auto const& [material, meshes] : visual->Meshes ) {
            GfxTexture* texture = ResolveTexture( material );
            if ( !texture ) continue;

            for ( auto const& mesh : meshes ) {
                if ( !mesh || mesh->Indices.empty() || !mesh->GetMeshVertexBuffer() || !mesh->GetMeshIndexBuffer() ) continue;
                if ( instance == UINT32_MAX ) instance = ui.AddItemInstance( StoreMatrix( clipFromObject ) );

                UIItemDraw draw;
                draw.Mesh = mesh.get();
                draw.Texture = texture;
                draw.Instance = instance;
                ui.AddItemDraw( draw );
            }
        }
    }

    /** Skinned base meshes plus node attachments, mirroring DrawVobSingle( SkeletalVobInfo* ). */
    void RecordModel( UIRenderer2D& ui, SkeletalVobInfo* vob, FXMMATRIX clipFromWorld, CXMMATRIX world ) {
        SkeletalMeshVisualInfo* visualInfo = static_cast<SkeletalMeshVisualInfo*>( vob->VisualInfo );
        if ( !visualInfo || !visualInfo->GetIsReady() ) return;
        zCModel* model = static_cast<zCModel*>( vob->Vob->GetVisual() );
        if ( !model ) return;

        // Into our own buffer, not GetBoneTransforms(): that one clobbers ZenGin's world-space node cache.
        static std::vector<XMFLOAT4X4> bones; // Main thread only, keeps its capacity
        bones.clear();
        model->GetBoneTransformsTo( bones );
        if ( bones.size() > kMaxItemBones ) bones.resize( kMaxItemBones );

        const XMMATRIX clipFromObject = XMMatrixMultiply( clipFromWorld, world );
        if ( !visualInfo->SkeletalMeshes.empty() && !bones.empty() ) {
            uint32_t instance = UINT32_MAX;
            uint32_t boneOffset = 0;
            for ( auto const& [material, meshes] : visualInfo->SkeletalMeshes ) {
                GfxTexture* texture = ResolveTexture( material );
                if ( !texture ) continue;

                for ( auto const& mesh : meshes ) {
                    if ( !mesh || mesh->Indices.empty() || !mesh->MeshVertexBuffer || !mesh->MeshIndexBuffer ) continue;
                    if ( instance == UINT32_MAX ) {
                        instance = ui.AddItemInstance( StoreMatrix( clipFromObject ) );
                        boneOffset = ui.AddItemBones( bones );
                    }

                    UIItemDraw draw;
                    draw.SkinnedMesh = mesh.get();
                    draw.Texture = texture;
                    draw.Instance = instance;
                    draw.BoneOffset = boneOffset;
                    draw.BoneCount = static_cast<uint32_t>( bones.size() );
                    ui.AddItemDraw( draw );
                }
            }
        }

        zCArray<zCModelNodeInst*>* nodeList = model->GetNodeList();
        const int numNodes = nodeList ? std::min<int>( nodeList->NumInArray, static_cast<int>( bones.size() ) ) : 0;
        auto& attachments = vob->NodeAttachments;
        for ( int i = 0; i < numNodes; i++ ) {
            zCModelNodeInst* node = nodeList->Array[i];
            if ( !node || !node->NodeVisual ) continue;

            // Extract on first sight, re-extract when the node's visual changed (only comparable once Ready).
            auto attachment = attachments.find( i );
            if ( attachment == attachments.end()
                || (!attachment->second.empty() && attachment->second[0]->GetIsReady()
                    && node->NodeVisual != attachment->second[0]->Visual) ) {
                WorldConverter::ExtractNodeVisualAsync( i, node, attachments );
                attachment = attachments.find( i );
            }
            if ( attachment == attachments.end() ) continue;

            const XMMATRIX nodeClipFromObject = XMMatrixMultiply( clipFromObject, XMLoadFloat4x4( &bones[i] ) );
            for ( MeshVisualInfo* mvi : attachment->second ) {
                if ( !mvi || !mvi->Visual ) continue;
                RecordMeshVisual( ui, mvi, nodeClipFromObject );
            }
        }
    }
}
#endif

bool InventoryRenderer::IsSupported() {
    return GD3D11_INVENTORY_RENDERER != 0;
}

bool InventoryRenderer::IsActive() {
    return IsSupported() && Engine::GraphicsEngine && Engine::GraphicsEngine->SupportsUI2D()
        && Engine::GAPI->GetRendererState().RendererSettings.InventoryRenderMode == GothicRendererSettings::INVENTORY_RENDER_RENDERITEM;
}

bool InventoryRenderer::RenderItem( void* item, void* viewItem, float addon ) {
#if GD3D11_INVENTORY_RENDERER
    if ( !item || !viewItem || !IsActive() ) return false;
    ZoneScoped;

    zCVob* vob = static_cast<zCVob*>( item );
    zCVisual* visual = vob->GetVisual();
    if ( !visual ) {
        // Vanilla's burning-torch hack draws a child vob, which needs the pseudo-world's vob tree.
        static bool logged = false;
        LogOnce( logged, "item without a visual, using ZenGin's preview render for it." );
        return false;
    }

    const EItemVisual kind = ClassifyVisual( visual );
    if ( kind == EItemVisual::Unsupported ) {
        static bool logged = false;
        LogOnce( logged, "item visual type not supported, using ZenGin's preview render for it." );
        return false;
    }

    RotateItem( item, addon );

    XMMATRIX clipFromWorld;
    zTViewportData viewport;
    if ( !PlaceCamera( item, viewItem, addon, clipFromWorld, viewport ) ) return true;

    UIRenderer2D& ui = Engine::GraphicsEngine->GetUIRenderer2D();
    ui.BeginItemPreview( static_cast<float>( viewport.xMin ), static_cast<float>( viewport.yMin ),
        static_cast<float>( viewport.xDim ), static_cast<float>( viewport.yDim ) );

    const XMMATRIX world = vob->GetWorldMatrixXM();
    if ( kind == EItemVisual::Mesh || kind == EItemVisual::MorphMesh ) {
        RecordMeshVisual( ui, Engine::GAPI->GetOrCreateProgMeshVisual( visual, kind == EItemVisual::MorphMesh ),
            XMMatrixMultiply( clipFromWorld, world ) );
    } else if ( static_cast<zCModel*>( visual )->GetMeshSoftSkinList()->NumInArray == 0 ) {
        // Without a softskin, GothicAPI::LoadzCModelData would never become ready (see GInventory).
        RecordMeshVisual( ui, Engine::GAPI->GetOrCreateFlattenedModelVisual( visual ), XMMatrixMultiply( clipFromWorld, world ) );
    } else if ( SkeletalVobInfo* skeletal = Engine::GAPI->GetInventory()->GetOrCreateSkeletal( vob ) ) {
        RecordModel( ui, skeletal, clipFromWorld, world );
    }

    ui.EndItemPreview();
    return true;
#else
    return false;
#endif
}
