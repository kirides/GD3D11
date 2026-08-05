#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Logger.h"

class zCPathSearch {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
#ifndef BUILD_SPACER
        DisableCorrectPosForNearClip();
#endif
    }

#ifndef BUILD_SPACER
    /** Turns zCPathSearch::CorrectPosForNearClip() into `return 0`.

        Called once per frame by the AI camera. To get the default near clip distance it
        constructs a throwaway zCCamera on the heap (0x934 bytes, ctor builds a zCMaterial and
        a zCMesh), reads nearClipZ out of it and destroys it again - then bails out with
        `if (defaultNearClipZ == 1.0f) return 0;`. zCCamera's ctor picks 1 for every float
        depth buffer, so vanilla always takes that early out and the rest of the function is
        unreachable. Our projection pins the near plane at 1.0 on reversed-Z and ignores
        ZenGin's nearClipZ anyway.

        `XOR EAX,EAX / RET 4` (__thiscall, one stack arg), written before the SEH frame is set
        up and before ESP is touched. The single caller ignores the return value and the zVEC3&
        it passes is only read on this path. */
    static void DisableCorrectPosForNearClip() {
        const unsigned int addr = GothicMemoryLocations::zCPathSearch::CorrectPosForNearClip;
        if ( !addr )
            return; // Not located in this binary - leave the vanilla behavior alone.

        // MOV EAX,FS:[0] / PUSH -1 / PUSH <seh handler> - bail out rather than corrupt code if
        // this isn't the build we mapped, or if a SystemPack/Union plugin already detoured it.
        static const unsigned char expected[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68 };
        if ( std::memcmp( reinterpret_cast<const void*>(addr), expected, sizeof( expected ) ) != 0 ) {
            LogWarn() << "zCPathSearch::CorrectPosForNearClip has an unexpected prologue at "
                << std::hex << addr << " - keeping ZenGin's implementation";
            return;
        }

        PatchAddr( addr, "\x33\xC0\xC2\x04\x00" ); // XOR EAX,EAX ; RET 4
    }
#endif
};
