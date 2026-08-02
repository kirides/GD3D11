#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCModelTexAniState.h"
#include "zCProgMeshProto.h"


/** One entry of zCMorphMeshProto::aniList. Owned by the (shared) prototype, so two zCMorphMeshes
    built from the same .MMS hand out the same zCMorphMeshAni pointers. */
class zCMorphMeshAni {
public:
    int GetNumVert() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Ani_Offset_NumVert ));
    }

    /** numFrames * numVert positions. For a refShape ani only the first numVert are the shape itself. */
    float3* GetVertPosMatrix() {
        return *reinterpret_cast<float3**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Ani_Offset_VertPosMatrix ));
    }
};

/** The per-.MMS prototype. Refcounted and cached by name in ZENGIN (zCMorphMeshProto::SearchName), so
    every NPC wearing the same head shares one of these - which is exactly what makes a single rest
    mesh serve all of them. */
class zCMorphMeshProto {
public:
    zCProgMeshProto* GetMorphRefMesh() {
        return *reinterpret_cast<zCProgMeshProto**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Proto_Offset_MorphRefMesh ));
    }

    /** The pristine, never-written base vertex positions, one per morphRefMesh vertex. Read only. */
    float3* GetMorphRefMeshVertPos() {
        return *reinterpret_cast<float3**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Proto_Offset_MorphRefMeshVertPos ));
    }
};

class zCMorphMesh {
public:
    /** NOTE: this is NOT a per-instance mesh - zCMorphMesh's constructor does
        morphMesh = morphProto->morphRefMesh->AddRef(), so every zCMorphMesh built from the same .MMS
        returns the SAME zCProgMeshProto, and CalcVertexPositions deforms that one shared position list
        immediately before each instance is drawn. That is why our converted copies have to be snapshots,
        and why the undeformed geometry has to come from the arrays below rather than from here. */
    zCProgMeshProto* GetMorphMesh() {
        return *reinterpret_cast<zCProgMeshProto**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Offset_MorphMesh ));
    }

    zCMorphMeshProto* GetMorphProto() {
        return *reinterpret_cast<zCMorphMeshProto**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Offset_MorphProto ));
    }

    /** Non-null only once a refShape ani has been started on this instance (zCMorphMesh::StartAni
        routes anis flagged refShape here instead of onto a blend channel). When set it replaces
        morphRefMeshVertPos as the base the morph deltas are added to. */
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

    /** Identity of this instance's rest pose. Two zCMorphMeshes with the same key have byte-identical
        undeformed geometry, so they can share one converted rest mesh (and therefore batch together).
        Both candidates live on shared objects - morphRefMesh is the prototype's own mesh and a
        refShapeAni is an entry of the prototype's aniList - so heads that were given the same shape ani
        still collapse onto one key. Returns nullptr when the rest pose can't be determined. */
    void* GetRestPoseKey() {
        if ( zCMorphMeshAni* shape = GetRefShapeAni() ) {
            return reinterpret_cast<void*>(shape);
        }
        zCMorphMeshProto* proto = GetMorphProto();
        return proto ? reinterpret_cast<void*>(proto->GetMorphRefMesh()) : nullptr;
    }

    /** The undeformed positions matching GetRestPoseKey(), indexed exactly like morphMesh's position
        list. 'outNumVert' is how many of them are valid. Returns nullptr if unavailable, in which case
        the caller must fall back to the deformed path. */
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
