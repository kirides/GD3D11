#pragma once

#pragma warning(disable: 4731) // Change of ebp from inline assembly
#pragma warning(disable: 4244) // Loss of data during conversion
#include <Windows.h>
#include <wrl/client.h>
#include <chrono>
#include <d3d11_1.h>
#include <DirectXMath.h>
#include <future>
#include <list>
#include <map>
#include <array>
#include <mmsystem.h>
#include <set>
#include <signal.h>
#include <unordered_map>
#include <unordered_set>

#include "Logger.h"
#include "Types.h"
#include "VertexTypes.h"

using namespace DirectX;

#if _MSC_VER < 1900
;
#else
#define stdext std
#endif

#define ENABLE_TESSELATION 0

#define VERSION_NUMBER "17.8-dev8"
__declspec(selectany) const char* VERSION_NUMBER_STR = VERSION_NUMBER;

extern bool FeatureLevel10Compatibility;
extern bool GMPModeActive;

/** D3D7-Call logging */
#define DebugWriteValue(value, check) if (value == check) { LogInfo() << " - " << #check; }
#define DebugWriteFlag(value, check) if ((value & check) == check) { LogInfo() << " - " << #check; }
#define DebugWrite(debugMessage) DebugWrite_i(debugMessage, (void *) this);

/** Debugging */
#define SAFE_RELEASE(x) if (x) { x->Release(); x = nullptr; }
#define SAFE_DELETE(x) delete x; x = nullptr;
//#define V(x) x

/** zCObject Managing */
void zCObject_AddRef( void* o );
void zCObject_Release( void* o );

/** Writes a string of the D3D7-Call log */
void DebugWrite_i( LPCSTR lpDebugMessage, void* thisptr );

/** Computes the size in bytes of the given FVF */
int ComputeFVFSize( DWORD fvf );

inline unsigned short quantizeHalfFloat( float v )
{
    union { float f; unsigned int ui; } u = { v };
    unsigned int ui = u.ui;

    int s = ( ui >> 16 ) & 0x8000;
    int em = ui & 0x7fffffff;

    int h = ( em - ( 112 << 23 ) + ( 1 << 12 ) ) >> 13;
    h = ( em < ( 113 << 23 ) ) ? 0 : h;
    h = ( em >= ( 143 << 23 ) ) ? 0x7c00 : h;
    h = ( em > ( 255 << 23 ) ) ? 0x7e00 : h;
    return static_cast<unsigned short>(s | h);
}
