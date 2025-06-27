#pragma once

#pragma warning(disable: 4731) // Change of ebp from inline assembly
#pragma warning(disable: 4244) // Loss of data during conversion
#include <Windows.h>
#include <wrl/client.h>
#include <chrono>
#include <d3d11_4.h>
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
#include <filesystem>

#include "Logger.h"
#include "Types.h"
#include "VertexTypes.h"

using namespace DirectX;

#ifndef VERSION_NUMBER
#define VERSION_NUMBER "17.8-rev'SK5"
#endif

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

typedef unsigned short (*ZQuantizeHalfFloat)(float input);
typedef void (*ZQuantizeHalfFloat_X4)(float* input, unsigned short* output);
typedef float (*ZUnquantizeHalfFloat)(unsigned short input);
typedef void (*ZUnquantizeHalfFloat_X4)(unsigned short* input, float* output);

extern ZQuantizeHalfFloat QuantizeHalfFloat;
extern ZQuantizeHalfFloat_X4 QuantizeHalfFloat_X4;
extern ZUnquantizeHalfFloat UnquantizeHalfFloat;
extern ZUnquantizeHalfFloat_X4 UnquantizeHalfFloat_X4;
extern ZUnquantizeHalfFloat_X4 UnquantizeHalfFloat_X8;
