#pragma once
#include "zCVob.h"

class zCVobLight : public zCVob {
public:

    struct LightInfoFlags {
        uint8_t isStatic : 1;
        uint8_t rangeAniSmooth : 1;
        uint8_t rangeAniLoop : 1;
        uint8_t colorAniSmooth : 1;
        uint8_t colorAniLoop : 1;
        uint8_t isTurnedOn : 1;
        uint8_t lightQuality : 4;
        uint8_t lightType : 4;
        uint8_t m_bCanMove : 1;
    };

    DWORD GetLightColor() const {
        return *reinterpret_cast<DWORD*>(THISPTR_OFFSET( GothicMemoryLocations::zCVobLight::Offset_LightColor ));
    }

    float GetLightRange() const {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCVobLight::Offset_Range ));
    }

    LightInfoFlags& GetLightInfoFlags() const {
        LightInfoFlags& flags = *reinterpret_cast<LightInfoFlags*>(THISPTR_OFFSET( GothicMemoryLocations::zCVobLight::Offset_LightInfo ));
        return flags;
    }

    bool IsEnabled() const {
        return *reinterpret_cast<DWORD*>(THISPTR_OFFSET( GothicMemoryLocations::zCVobLight::Offset_LightInfo )) & GothicMemoryLocations::zCVobLight::Mask_LightEnabled;
    }

    void DoAnimation() {
        reinterpret_cast<void( __fastcall* )( zCVobLight* )>( GothicMemoryLocations::zCVobLight::DoAnimation )( this );
    }

    bool IsStatic() const {
        int flags = *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCVobLight::Offset_LightInfo ));
        return (flags & 1) != 0;
    }

#ifndef PUBLIC_RELEASE
    byte data1[0x140];
    float flt[10];
#endif
};
