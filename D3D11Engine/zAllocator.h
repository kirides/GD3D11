#pragma once

#include <cstdlib>
#include "GothicMemoryLocations.h"

struct zAllocator {
    zAllocator() = delete;
    ~zAllocator() = delete;

    static void* zNew( size_t size ) {
#ifdef zALLOCATOR_SUPPORTED
        typedef void* (__cdecl* zMallocFunc)(size_t size);
        return (*reinterpret_cast<zMallocFunc*>(GothicMemoryLocations::zAllocator::Malloc))(size);
#else
        return std::malloc(size);
#endif
    }
    static void zFree( void* ptr ) {
#ifdef zALLOCATOR_SUPPORTED
        typedef void (__cdecl* zFreeFunc)(void* ptr);
        (*reinterpret_cast<zFreeFunc*>(GothicMemoryLocations::zAllocator::Free))(ptr);
#else
        std::free(ptr);
#endif
    }
};
