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
    //void Set_emAdjustShpToOrigin(int val) {
    //    *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::oCVisualFX::Offset_emAdjustShpToOrigin )) = val;
    //}

    //int Get_emAdjustShpToOrigin() {
    //    return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::oCVisualFX::Offset_emAdjustShpToOrigin ));
    //}

    //void AdjustShapeToOrigin() {
    //    reinterpret_cast<void( __thiscall* )(oCVisualFX*)>(GothicMemoryLocations::oCVisualFX::AdjustShapeToOrigin)(this);
    //}
};

