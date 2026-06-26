#pragma once
#include "zCClassDef.h"
#include "zCVob.h"

class oCNPC;

class oCMob : public zCVob
{
public:
    static const zCClassDef* GetStaticClassDef() {
        return reinterpret_cast<const zCClassDef*>(GothicMemoryLocations::zCClassDef::oCMOB);
    }

    bool HasName() {
#if defined(BUILD_GOTHIC_2_6_fix) || (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F))
        zSTRING str;
        auto* _ = reinterpret_cast<zSTRING* (__fastcall*)(void* object, void* edx, zSTRING * retBuffer)>(GothicMemoryLocations::oCMob::GetName)(this, nullptr, &str);
        return str.Length() > 0;
#else
        return true;
#endif
    }
};

class oCMobInter : public oCMob
{
public:
    static const zCClassDef* GetStaticClassDef() {
        return reinterpret_cast<const zCClassDef*>(GothicMemoryLocations::zCClassDef::oCMobInter);
    }

    bool IsInteractingWith( oCNPC* other ) {
#if defined(BUILD_GOTHIC_2_6_fix) || (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F))
        return reinterpret_cast<int( __thiscall* )(oCMobInter*, oCNPC*)>(GothicMemoryLocations::oCMobInter::IsInteractingWith)(this, other) != 0;
#else
        return false;
#endif
    }
};

