#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCVob.h"
#include "zViewTypes.h"

class oCVisualFX : public zCVob {
public:
    static const zCClassDef* GetStaticClassDef() {
        return reinterpret_cast<const zCClassDef*>(GothicMemoryLocations::zCClassDef::oCVisualFX);
    }
    
    zCVob* GetOrigin() const {
        return *reinterpret_cast<zCVob**>(THISPTR_OFFSET( GothicMemoryLocations::oCVisualFX::Offset_origin ));
    }

    /** When set, oCVisualFX owns the particle emitter's shape mesh and points it at the origin
        vob's visual (oCVisualFX::CalcPFXMesh). */
    bool GetAdjustShapeToOrigin() const {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::oCVisualFX::Offset_emAdjustShpToOrigin )) != 0;
    }

    /** Name of the origin model's node this effect rides on, from the VisualFX script instance
        (emTrjOriginNode). Empty when the effect is not node-bound. */
    const zSTRING* GetOriginNodeName() const {
        return reinterpret_cast<zSTRING*>(THISPTR_OFFSET( GothicMemoryLocations::oCVisualFX::Offset_emTrjOriginNode_S ));
    }

    /** Resolved node for GetOriginNodeName. ZENGIN resolves it once in oCVisualFX::Init and leaves
        it null if the origin had no zCModel visual yet - see GothicAPI::RepairShapeMeshEmitter. */
    zCModelNodeInst* GetOriginNode() const {
        return *reinterpret_cast<zCModelNodeInst**>(THISPTR_OFFSET( GothicMemoryLocations::oCVisualFX::Offset_orgNode ));
    }

    void SetOriginNode( zCModelNodeInst* node ) {
        *reinterpret_cast<zCModelNodeInst**>(THISPTR_OFFSET( GothicMemoryLocations::oCVisualFX::Offset_orgNode )) = node;
    }
};

class oCItem : public zCVob {
public:
    static const zCClassDef* GetStaticClassDef() {
        return reinterpret_cast<const zCClassDef*>(GothicMemoryLocations::zCClassDef::oCItem);
    }
};

