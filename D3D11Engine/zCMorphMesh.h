#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCModelTexAniState.h"
#include "zCProgMeshProto.h"


/** One entry of zCMorphMeshProto::aniList. Owned by the shared prototype, so morph meshes built from
    the same .MMS hand out the same ani pointers. */
class zCMorphMeshAni {
public:
    int GetNumVert() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Ani_Offset_NumVert ));
    }

    /** numFrames * numVert positions; for a refShape ani the first numVert are the shape. */
    float3* GetVertPosMatrix() {
        return *reinterpret_cast<float3**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Ani_Offset_VertPosMatrix ));
    }
};

/** The per-.MMS prototype. Cached by name in ZENGIN, so every NPC wearing the same head shares one. */
class zCMorphMeshProto {
public:
    zCProgMeshProto* GetMorphRefMesh() {
        return *reinterpret_cast<zCProgMeshProto**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Proto_Offset_MorphRefMesh ));
    }

    /** Pristine base vertex positions, one per morphRefMesh vertex. Never written after load. */
    float3* GetMorphRefMeshVertPos() {
        return *reinterpret_cast<float3**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Proto_Offset_MorphRefMeshVertPos ));
    }
};

class zCMorphMesh {
public:
    /** NOT per-instance: the constructor does morphMesh = morphProto->morphRefMesh->AddRef(), so every
        zCMorphMesh from the same .MMS returns the SAME progmesh and CalcVertexPositions deforms that one
        shared position list right before each instance draws. Hence our copies are snapshots, and the
        undeformed geometry has to come from the arrays above. */
    zCProgMeshProto* GetMorphMesh() {
        return *reinterpret_cast<zCProgMeshProto**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Offset_MorphMesh ));
    }

    zCMorphMeshProto* GetMorphProto() {
        return *reinterpret_cast<zCMorphMeshProto**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Offset_MorphProto ));
    }

    /** Non-null once a refShape-flagged ani has been started here; it then replaces morphRefMeshVertPos
        as the base the morph deltas are added to. */
    zCMorphMeshAni* GetRefShapeAni() {
        return *reinterpret_cast<zCMorphMeshAni**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Offset_RefShapeAni ));
    }

    zCModelTexAniState* GetTexAniState() {
        return reinterpret_cast<zCModelTexAniState*>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Offset_TexAniState ));
    }

    void CalcVertexPositions() {
        reinterpret_cast<void( __fastcall* )( zCMorphMesh* )>( GothicMemoryLocations::zCMorphMesh::CalcVertexPositions )( this );
    }

    void AdvanceAnis() {
        reinterpret_cast<void( __fastcall* )( zCMorphMesh* )>( GothicMemoryLocations::zCMorphMesh::AdvanceAnis )( this );
    }

    /** Identity of this instance's rest pose: equal keys mean byte-identical undeformed geometry. Both
        candidates live on the shared prototype, so heads given the same shape ani still collapse onto one
        key. Null when the rest pose can't be determined. */
    void* GetRestPoseKey() {
        if ( zCMorphMeshAni* shape = GetRefShapeAni() ) {
            return reinterpret_cast<void*>(shape);
        }
        zCMorphMeshProto* proto = GetMorphProto();
        return proto ? reinterpret_cast<void*>(proto->GetMorphRefMesh()) : nullptr;
    }

    /** The undeformed positions matching GetRestPoseKey(), indexed like morphMesh's position list. Null
        if unavailable, in which case the caller must fall back to the deformed path. */
    float3* GetRestPositions( int& outNumVert ) {
        outNumVert = 0;
        if ( zCMorphMeshAni* shape = GetRefShapeAni() ) {
            outNumVert = shape->GetNumVert();
            return outNumVert > 0 ? shape->GetVertPosMatrix() : nullptr;
        }
        zCMorphMeshProto* proto = GetMorphProto();
        zCProgMeshProto* ref = proto ? proto->GetMorphRefMesh() : nullptr;
        if ( !ref ) {
            return nullptr;
        }
        outNumVert = ref->GetPositionList()->NumInArray;
        return outNumVert > 0 ? proto->GetMorphRefMeshVertPos() : nullptr;
    }
};
