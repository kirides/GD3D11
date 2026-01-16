#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCModelTexAniState.h"


class zCMorphMesh {
public:
    static const zCClassDef* GetStaticClassDef() {
        return reinterpret_cast<const zCClassDef*>(GothicMemoryLocations::zCClassDef::zCMorphMesh);
    }

    zCProgMeshProto* GetMorphMesh() {
        return *reinterpret_cast<zCProgMeshProto**>(THISPTR_OFFSET( GothicMemoryLocations::zCMorphMesh::Offset_MorphMesh ));
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
};
