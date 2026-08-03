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

    int GetNumFrames() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Ani_Offset_NumFrames ));
    }

    /** numFrames * numVert positions; for a refShape ani the first numVert are the shape. */
    float3* GetVertPosMatrix() {
        return *reinterpret_cast<float3**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Ani_Offset_VertPosMatrix ));
    }

    /** Maps this ani's slot j to a vertex index in the morph mesh's position list - the ani only
        touches numVert of the mesh's vertices, and does so by indirection. */
    int* GetVertIndexList() {
        return *reinterpret_cast<int**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Ani_Offset_VertIndexList ));
    }

    /** A "shape" ani replaces what earlier channels accumulated instead of blending against it
        (CalcVertPositions forces weight1M to 1 for these). */
    bool IsShape() {
        return (*reinterpret_cast<uint8_t*>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Ani_Offset_Flags ))
            & GothicMemoryLocations::zCMorphMesh::Ani_Mask_Shape) != 0;
    }
};

/** One active blend channel of a zCMorphMesh. AdvanceAnis owns these - it integrates the weight,
    advances the frame and deletes the entry when it has faded out. We only ever read them. */
class zTMorphAniEntry {
public:
    zCMorphMeshAni* GetAni() {
        return *reinterpret_cast<zCMorphMeshAni**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Entry_Offset_Ani ));
    }
    float GetWeight() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Entry_Offset_Weight ));
    }
    int GetActFrameInt() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Entry_Offset_ActFrameInt ));
    }
    int GetNextFrameInt() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Entry_Offset_NextFrameInt ));
    }
    float GetFrac() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Entry_Offset_Frac ));
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

    /** Active blend channels, in the order CalcVertPositions folds them - the order is load-bearing,
        see MorphBlend.h. Read the count off the THIRD dword: zCArraySort is
        { T* array; int numAlloc; int numInArray; }, and this array shrinks as anis fade out, so
        numAlloc (what zCArrayAdapt would report) is not the live count. */
    int GetNumAniChannels() {
        uint8_t* arr = reinterpret_cast<uint8_t*>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Offset_AniChannels ));
        return *reinterpret_cast<int*>(arr + GothicMemoryLocations::zCMorphMesh::ArraySort_Offset_NumInArray);
    }

    zTMorphAniEntry* GetAniChannel( int i ) {
        uint8_t* arr = reinterpret_cast<uint8_t*>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Offset_AniChannels ));
        zTMorphAniEntry** entries = *reinterpret_cast<zTMorphAniEntry***>(arr + GothicMemoryLocations::zCMorphMesh::ArraySort_Offset_Array);
        return entries ? entries[i] : nullptr;
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
