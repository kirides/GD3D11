#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zSTRING.h"
#include "zCArray.h"
#include "zTypes.h"
#include "zCWorld.h"
#include "zCCamera.h"
#include "zCModelTexAniState.h"
#include "zCVisual.h"
#include "zCMeshSoftSkin.h"
#include "zCObject.h"
#include "AlignedAllocator.h"

class zCVisual;
class zCMeshSoftSkin;

struct zCModelNode;
struct zCModelNodeInst {
    zCModelNodeInst* ParentNode;
    zCModelNode* ProtoNode;
    zCVisual* NodeVisual;
    XMFLOAT4X4 Trafo;
    XMFLOAT4X4 TrafoObjToCam;
    zTBBox3D BBox3D;

    zCModelTexAniState TexAniState;
};

struct zCModelNode {
    zCModelNode* ParentNode;
    zSTRING	NodeName;
    zCVisual* Visual;
    XMFLOAT4X4 Transform;

    XMFLOAT3 NodeRotAxis;
    float NodeRotAngle;
    XMFLOAT3 Translation;
    XMFLOAT4X4 TransformObjToWorld;
    XMFLOAT4X4* NodeTransformList;
    zCModelNodeInst* LastInstNode;
    
    bool IsSlot() const {
        if (NodeName.Length() < 3) { return false; }
        const auto name = NodeName.ToChar();
        return (name[0]=='Z') && (name[1]=='S') && (name[2]=='_');
    }
};

struct zTMdl_NodeVobAttachment {
    zCVob* Vob;
    zCModelNodeInst* NodeInst;
};

class zCModelMeshLib;
struct zTMeshLibEntry {
    zCModelTexAniState TexAniState;
    zCModelMeshLib* MeshLibPtr;
};

class zCModelAni {
public:
    bool IsIdleAni() {
#ifdef BUILD_GOTHIC_1_08k
        return false;
#else
        DWORD value = *reinterpret_cast<DWORD*>(THISPTR_OFFSET( GothicMemoryLocations::zCModelAni::Offset_Flags ));
        return (value & GothicMemoryLocations::zCModelAni::Mask_FlagIdle) != 0;
#endif
    }

    int GetNumAniFrames() {
#ifdef BUILD_GOTHIC_1_08k
        return 0;
#else
        return *reinterpret_cast<uint16_t*>(THISPTR_OFFSET( GothicMemoryLocations::zCModelAni::Offset_NumFrames ));
#endif
    }
};

class zCModelAniActive {
public:
    zCModelAni* protoAni;
    zCModelAni* nextAni;
};

class zCModelMeshLib : public zCObject {
public:
    struct zTNodeMesh {
        zCVisual* Visual;
        int NodeIndex;
    };

    /** This returns the list of nodes which hold information about the bones and attachments later */
    zCArray<zTNodeMesh>* GetNodeList() {
        return &NodeList;
    }

    /** Returns the list of meshes which store the vertex-positions and weights */
    zCArray<zCMeshSoftSkin*>* GetMeshSoftSkinList() {
#ifndef BUILD_GOTHIC_1_08k
        return &SoftSkinList;
#else
        return nullptr;
#endif
    }

    const char* GetVisualName() {
        if ( GetMeshSoftSkinList()->NumInArray > 0 )
            return GetMeshSoftSkinList()->Array[0]->GetObjectName();

        return "";
        //return __GetVisualName().ToChar();
    }

private:
    zCArray<zTNodeMesh>			NodeList;
    zCArray<zCMeshSoftSkin*>	SoftSkinList;
};

class zCModelPrototype {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
        //DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCModelPrototypeLoadModelASC, Hooked_LoadModelASC  );
        //DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCModelPrototypeReadMeshAndTreeMSB, Hooked_ReadMeshAndTreeMSB  );
    }

    /** This is called on load time for models */
    /*
    static int __fastcall Hooked_LoadModelASC( void* thisptr, void* unknwn, const zSTRING& file ) {
        LogInfo() << "Loading Model: " << file.ToChar();
        int r = HookedFunctions::OriginalFunctions.original_zCModelPrototypeLoadModelASC( thisptr, file );

        // Pre-Load this model for us, too
        if ( r ) {
        }

        return r;
    }
    */

    /** This is called on load time for models */
    /*
    static int __fastcall Hooked_ReadMeshAndTreeMSB( void* thisptr, void* unknwn, int& i, class zCFileBIN& f ) {
        LogInfo() << "Loading Model!";
        int r = HookedFunctions::OriginalFunctions.original_zCModelPrototypeReadMeshAndTreeMSB( thisptr, i, f );

        // Pre-Load this model for us, too
        if ( r ) {
        }

        return r;
    }
    */

    /** This returns the list of nodes which hold information about the bones and attachments later */
    zCArray<zCModelNode*>* GetNodeList() {
        return reinterpret_cast<zCArray<zCModelNode*>*>(THISPTR_OFFSET( GothicMemoryLocations::zCModelPrototype::Offset_NodeList ));
    }

    /** Returns the list of meshes which store the vertex-positions and weights */
    zCArray<zCMeshSoftSkin*>* GetMeshSoftSkinList() {
#ifndef BUILD_GOTHIC_1_08k
        return reinterpret_cast<zCArray<zCMeshSoftSkin*>*>(THISPTR_OFFSET( GothicMemoryLocations::zCModelPrototype::Offset_MeshSoftSkinList ));
#else
        return nullptr;
#endif
    }

    /** Returns the name of the first Mesh inside this */
    const char* GetVisualName() {
        if ( GetMeshSoftSkinList()->NumInArray > 0 )
            return GetMeshSoftSkinList()->Array[0]->GetObjectName();

        return "";
    }

    /** The .MDS/.ASC this prototype was built from (ZENGIN's modelProtoFileName). */
    const zSTRING& GetModelProtoFileName() const {
        return *reinterpret_cast<const zSTRING*>(THISPTR_OFFSET( GothicMemoryLocations::zCModelPrototype::Offset_ModelProtoFileName ));
    }
};

class zCModel : public zCVisual {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
        /*#ifndef BUILD_GOTHIC_1_08k
                DWORD dwProtect;
                VirtualProtect((void *)GothicMemoryLocations::zCModel::AdvanceAnis, GothicMemoryLocations::zCModel::SIZE_AdvanceAnis, PAGE_EXECUTE_READWRITE, &dwProtect);

                byte unsmoothAnisFix[] = {0x75, 0x00, 0xC7, 0x44, 0x24, 0x78, 0x01, 0x00, 0x00, 0x00}; // Replaces a jnz in AdvanceAnis - Thanks to killer-m!
                memcpy((void *)GothicMemoryLocations::zCModel::RPL_AniQuality, unsmoothAnisFix, sizeof(unsmoothAnisFix));
        #endif*/

#ifdef BUILD_GOTHIC_2_6_fix
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCModelGetLowestLODNumPolys, Hooked_zCModelGetLowestLODNumPolys  );
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zCModelGetLowestLODPoly, Hooked_zCModelGetLowestLODPoly  );
#endif
    }

    /** Fix particle emitter setup */
#ifdef BUILD_GOTHIC_2_6_fix
    static int __fastcall Hooked_zCModelGetLowestLODNumPolys( void* thisptr ) {
        return Engine::GAPI->GetLowestLODNumPolys_SkeletalMesh( static_cast<zCModel*>(thisptr) );
    }

    static float3* __fastcall Hooked_zCModelGetLowestLODPoly( void* thisptr, void*, const int polyId, float3*& polyNormal ) {
        return Engine::GAPI->GetLowestLODPoly_SkeletalMesh( static_cast<zCModel*>(thisptr), polyId, polyNormal );
    }
#endif

    /** Creates an array of matrices for the bone transforms */
    void __fastcall RenderNodeList( zTRenderContext& renderContext, zCArray<XMFLOAT4X4*>& boneTransforms, zCRenderLightContainer& lightContainer, int lightingMode = 0 ) {
        reinterpret_cast<void( __fastcall* )( zCModel*, zTRenderContext&, zCArray<XMFLOAT4X4*>&, zCRenderLightContainer&, int )>
            ( GothicMemoryLocations::zCModel::RenderNodeList )( this, renderContext, boneTransforms, lightContainer, lightingMode );
    }

    /** Returns the current amount of active animations */
    int GetNumActiveAnimations() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_NumActiveAnis ));
    }

    /** Returns true if only an idle-animation is running */
    bool IdleAnimationRunning() {
#ifdef BUILD_GOTHIC_1_08k
        return false;
#else
        zCModelAniActive* activeAni = *reinterpret_cast<zCModelAniActive**>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_AniChannels ));
        return GetNumActiveAnimations() == 1 && activeAni->protoAni->GetNumAniFrames() <= 1;
#endif
    }

    /** This is needed for the animations to work at full framerate */
    void SetDistanceToCamera( float dist ) {
        *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_DistanceModelToCamera )) = dist;
    }

    /** Updates stuff like blinking eyes, etc */
    void UpdateMeshLibTexAniState() {
        for ( int i = 0; i < GetMeshLibList()->NumInArray; i++ )
            GetMeshLibList()->Array[i]->TexAniState.UpdateTexList();
    }

    int GetIsVisible() {
#ifndef BUILD_GOTHIC_1_08k
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_Flags )) & 1;
#else
        return 1;
#endif
    }

    void SetIsVisible( bool visible ) {
#ifndef BUILD_GOTHIC_1_08k
        int v = visible ? 1 : 0;

        byte* flags = reinterpret_cast<byte*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_Flags ));

        *flags &= ~1;
        *flags |= v;
#else
        // Do nothing yet
        // FIXME
#endif
    }

    XMFLOAT3 GetModelScale() {
        return *reinterpret_cast<XMFLOAT3*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_ModelScale ));
    }

    XMVECTOR GetModelScaleXM() {
        return XMLoadFloat3( reinterpret_cast<XMFLOAT3*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_ModelScale )) );
    }

    float GetModelFatness() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_ModelFatness ));
    }

    int GetDrawHandVisualsOnly() {
#ifndef BUILD_GOTHIC_1_08k
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_DrawHandVisualsOnly ));
#else
        return 0; // First person not implemented in G1
#endif
    }

    int GetMainPrototypeReferences() {
        zCArray<zCModelPrototype*>* prototypes = GetModelProtoList();
        if ( prototypes->NumInArray > 0 ) {
            zCModelPrototype* prototype = prototypes->Array[0];
            if ( prototype ) {
                return *reinterpret_cast<int*>(reinterpret_cast<DWORD>(prototype) + 0x08); // Get reference counter
            }
        }
        return 2; // Allow leakage because it is only container for 3DS models(mob) - better than crash
    }

    zCArray<zCModelNodeInst*>* GetNodeList() const {
        return reinterpret_cast<zCArray<zCModelNodeInst*>*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_NodeList ));
    }

    zCArray<zCMeshSoftSkin*>* GetMeshSoftSkinList() const {
        return reinterpret_cast<zCArray<zCMeshSoftSkin*>*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_MeshSoftSkinList ));
    }

    zCArray<zCModelPrototype*>* GetModelProtoList() const {
        return reinterpret_cast<zCArray<zCModelPrototype*>*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_ModelProtoList ));
    }

    zCArray<zTMeshLibEntry*>* GetMeshLibList() const {
        return reinterpret_cast<zCArray<zTMeshLibEntry*>*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_MeshLibList ));
    }

    zCArray<zTMdl_NodeVobAttachment>* GetAttachedVobList() const {
        return reinterpret_cast<zCArray<zTMdl_NodeVobAttachment>*>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_AttachedVobList ));
    }

#ifndef BUILD_SPACER
    /** Looks a node up by name, exactly as ZENGIN does. Case sensitive - node names are upper case. */
    zCModelNodeInst* SearchNode( const zSTRING& name ) {
        return reinterpret_cast<zCModelNodeInst*( __fastcall* )( zCModel*, int, const zSTRING& )>
            ( GothicMemoryLocations::zCModel::SearchNode )( this, 0, name );
    }
#endif

    /* Updates the world matrices of the attached VOBs */
    void UpdateAttachedVobs() {
        reinterpret_cast<void( __fastcall* )( zCModel* )>( GothicMemoryLocations::zCModel::UpdateAttachedVobs )( this );
    }

    /** Fills a vector of (viewspace) bone-transformation matrices for this frame */
    void GetBoneTransforms( std::vector<XMFLOAT4X4>* transforms ) const {
        zCArray<zCModelNodeInst*>* nodeList = GetNodeList();
        if ( !nodeList )
            return;

        const auto num = nodeList->NumInArray;
        const auto array = nodeList->Array;

        transforms->reserve( transforms->size() + num );

        for ( int i = 0; i < num; i++ ) {
            zCModelNodeInst* node = array[i];
            const zCModelNodeInst* parent = node->ParentNode;

            // Calculate transform for this node
            if ( parent ) {
                XMStoreFloat4x4( &node->TrafoObjToCam, XMMatrixMultiply(XMLoadFloat4x4( &parent->TrafoObjToCam ) , XMLoadFloat4x4( &node->Trafo ) ) );
            } else {
                node->TrafoObjToCam = node->Trafo;
            }

            transforms->push_back( node->TrafoObjToCam );
        }
    }

    /** Same accumulation as GetBoneTransforms, but into a caller-owned scratch buffer.
        GetBoneTransforms writes its result into zCModelNodeInst::TrafoObjToCam, which ZENGIN uses
        as the *world*-space node cache (zCModel::CalcNodeListBBoxWorld, read back by
        GetNodePositionWorld/GetBBox3DNodeWorld). Overwriting it with model-space matrices is fine
        while we own the frame, but not from inside an engine callback - use this there. */
    void GetBoneTransformsTo( std::vector<XMFLOAT4X4>& transforms ) const {
        zCArray<zCModelNodeInst*>* nodeList = GetNodeList();
        if ( !nodeList )
            return;

        const auto num = nodeList->NumInArray;
        const auto array = nodeList->Array;

        const size_t base = transforms.size();
        transforms.resize( base + num );

        for ( int i = 0; i < num; i++ ) {
            zCModelNodeInst* node = array[i];
            const zCModelNodeInst* parent = node->ParentNode;

            if ( parent ) {
                // The node list is parent-before-child, and the parent is nearly always the
                // immediately preceding entry, so this walk back is O(1) in practice.
                int parentIdx = -1;
                for ( int j = i - 1; j >= 0; --j ) {
                    if ( array[j] == parent ) { parentIdx = j; break; }
                }
                if ( parentIdx >= 0 ) {
                    XMStoreFloat4x4( &transforms[base + i],
                        XMMatrixMultiply( XMLoadFloat4x4( &transforms[base + parentIdx] ), XMLoadFloat4x4( &node->Trafo ) ) );
                    continue;
                }
            }
            transforms[base + i] = node->Trafo;
        }
    }

    const std::string_view GetVisualName() const {
        if ( GetMeshSoftSkinList()->NumInArray > 0 )
            return GetMeshSoftSkinList()->Array[0]->GetObjectNameView();

        return "";
        //return __GetVisualName().ToChar();
    }

    /** The .MDS/.ASC file this model was built from.
     *
     *  Reimplements ZENGIN's zCModel::GetVisualName (zModel.cpp:3828) rather than calling it: that one
     *  returns zSTRING *by value*, so every call allocated and freed a string through ZENGIN's
     *  allocator just to read a name we already have in memory. The view points straight at the
     *  prototype's own string and stays valid for as long as the prototype does, same as the
     *  GetVisualName() above. */
    std::string_view GetModelName() const {
        zCArray<zCModelPrototype*>* protos = GetModelProtoList();
        if ( protos->NumInArray <= 0 || !protos->Array[0] )
            return {};

        return protos->Array[0]->GetModelProtoFileName().ToView();
    }

    zCVob* GetHomeVob() const {
        return *reinterpret_cast<zCVob**>(THISPTR_OFFSET( GothicMemoryLocations::zCModel::Offset_HomeVob ));
    }

private:

};
