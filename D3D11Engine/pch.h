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
#include <mmsystem.h>
#include <set>
#include <signal.h>
#include <unordered_map>
#include <unordered_set>

#include "Logger.h"
#include "Types.h"
#include "VertexTypes.h"

#if _MSC_VER < 1900
;
#else
#define stdext std
#endif

#define VERSION_NUMBER "17.7-dev19-fix4"
__declspec(selectany) const char* VERSION_NUMBER_STR = VERSION_NUMBER;

extern bool GMPModeActive;

/** D3D7-Call logging */
#define DebugWriteValue(value, check) if (value == check) { LogInfo() << " - " << #check; }
#define DebugWriteFlag(value, check) if ((value & check) == check) { LogInfo() << " - " << #check; }
#define DebugWrite(debugMessage) DebugWrite_i(debugMessage, (void *) this);

/** Debugging */
#define SAFE_RELEASE(x) if (x) { x->Release(); x = nullptr; }
#define SAFE_DELETE(x) delete x; x = nullptr;
//#define V(x) x
//#define BUILD_SPACER_NET 1

/** Writes a string of the D3D7-Call log */
void DebugWrite_i( LPCSTR lpDebugMessage, void* thisptr );

/** Computes the size in bytes of the given FVF */
int ComputeFVFSize( DWORD fvf );


