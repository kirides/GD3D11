#pragma once

#ifndef __FUNCSIG__
#define __FUNCSIG__ __builtin_FUNCSIG()
#endif

#pragma warning(disable: 4731) // Change of ebp from inline assembly
#pragma warning(disable: 4244) // Loss of data during conversion

#if !PUBLIC_RELEASE
#define DEBUG_D3D11
// assume SSE3 is available when compiling in debug mode for SSE2
#if _M_IX86_FP == 2 // SSE2
#define _XM_SSE3_INTRINSICS_
#endif
#endif

#include <tracy/public/tracy/Tracy.hpp>
#include <tracy/public/client/TracyCallstack.hpp>
#include <tracy/public/tracy/TracyD3D11.hpp>
#include "MemoryTracker.h"
#include <Windows.h>
#include <wrl/client.h>
#include <chrono>
#include <magic_enum/magic_enum.hpp>
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
#include <parallel_hashmap/phmap.h>

#include "Logger.h"
#include "Types.h"
#include "VertexTypes.h"

using namespace DirectX;

#ifndef VERSION_NUMBER
#define VERSION_NUMBER "17.10-dev"
#define IS_DEV_BUILD
#endif
#ifndef BUILD_DATE
#define BUILD_DATE __DATE__
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

// Makro to convert a makro value to a string literal
#define VALUE(string) #string
#define TO_LITERAL(string) VALUE(string)

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


/* Thin span-like data structure to allow re-using parts of a big std::vector for example */
namespace std {
    template<typename T>
    struct span {
    private:
        T* _data = nullptr;
        size_t _size = 0;

    public:
        span() = delete;
        span(const span&) = default;
        span(span&&) = default;

        constexpr span( T* data, size_t size ) : _data( data ), _size( size ) {}
        constexpr span( std::vector<T>& data ) : _data( data.data() ), _size( data.size() ) {}

        T& operator[]( size_t i ) { return _data[i]; }
        const T& operator[]( size_t i ) const { return _data[i]; }
        T* begin() { return _data; }
        T* end() { return _data + _size; }
        const size_t size() const { return _size; }
        const T* data() { return _data; }

    };
}

#if defined(BUILD_GOTHIC_2_6_fix)
#define SWITCH_ENGINE(G1, G1A, G2A, OTHER) G2A
#elif defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
#define SWITCH_ENGINE(G1, G1A, G2A, OTHER) G1
#elif defined(BUILD_GOTHIC_1_08k) && defined(BUILD_1_12F)
#define SWITCH_ENGINE(G1, G1A, G2A, OTHER) G1A
#else
#define SWITCH_ENGINE(G1, G1A, G2A, OTHER) OTHER
#endif
