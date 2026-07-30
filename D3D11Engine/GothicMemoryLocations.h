#pragma once

/** Memory location switch */
//#define BUILD_GOTHIC_1_08k
//#define BUILD_GOTHIC_2_6_fix

#define PTR_OFFSET(ptr, x) (reinterpret_cast<DWORD>(ptr) + (x))
#define THISPTR_OFFSET(x) PTR_OFFSET(this, x)

template<typename T, size_t n >
static void PatchAddr( unsigned int adr, const T( &v )[n] ) {
    DWORD dwProtect;
    size_t len = n - 1;
    if ( VirtualProtect( reinterpret_cast<void*>(adr), len, PAGE_EXECUTE_READWRITE, &dwProtect ) ) {
        memcpy( reinterpret_cast<void*>(adr), v, len );
        VirtualProtect( reinterpret_cast<void*>(adr), len, dwProtect, &dwProtect );
        FlushInstructionCache( GetCurrentProcess(), reinterpret_cast<void*>(adr), len );
    }
}

static void PatchCall( unsigned int adr, unsigned int func ) {
    DWORD dwOldProtect, dwNewProtect, dwNewCall;
    dwNewCall = func - adr - 5;
    if ( VirtualProtect( reinterpret_cast<void*>(adr), 5, PAGE_EXECUTE_READWRITE, &dwOldProtect ) ) {
        *reinterpret_cast<BYTE*>(adr) = 0xE8;
        *reinterpret_cast<DWORD*>(adr + 1) = dwNewCall;
        VirtualProtect( reinterpret_cast<void*>(adr), 5, dwOldProtect, &dwNewProtect );
        FlushInstructionCache( GetCurrentProcess(), reinterpret_cast<void*>(adr), 5 );
    }
}

static void PatchJMP( unsigned int adr, unsigned int jmp ) {
    DWORD dwOldProtect, dwNewProtect, dwNewCall;
    dwNewCall = jmp - adr - 5;
    if ( VirtualProtect( reinterpret_cast<void*>(adr), 5, PAGE_EXECUTE_READWRITE, &dwOldProtect ) ) {
        *reinterpret_cast<BYTE*>(adr) = 0xE9;
        *reinterpret_cast<DWORD*>(adr + 1) = dwNewCall;
        VirtualProtect( reinterpret_cast<void*>(adr), 5, dwOldProtect, &dwNewProtect );
        FlushInstructionCache( GetCurrentProcess(), reinterpret_cast<void*>(adr), 5 );
    }
}

#define INST_NOP 0x90
#define REPLACE_OP(addr, op) {unsigned char* a = (unsigned char*)addr; *a = op;}
#define REPLACE_RANGE(start, end_incl, op) {for(int i=start; i<=end_incl;i++){REPLACE_OP(i, op);}}

#ifdef BUILD_GOTHIC_1_08k
#ifdef BUILD_1_12F
#include "GothicMemoryLocations1_12f.h"
#else
#include "GothicMemoryLocations1_08k.h"
#endif
#endif

#ifdef BUILD_GOTHIC_2_6_fix
#ifdef BUILD_SPACER
#include "GothicMemoryLocations2_6_fix_Spacer.h"
#else
#include "GothicMemoryLocations2_6_fix.h"
#endif
#endif
