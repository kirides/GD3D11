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
};

class oCItem : public zCVob {
public:
    static const zCClassDef* GetStaticClassDef() {
        return reinterpret_cast<const zCClassDef*>(GothicMemoryLocations::zCClassDef::oCItem);
    }
};

