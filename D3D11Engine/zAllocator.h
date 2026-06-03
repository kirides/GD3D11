#pragma once

#include "GothicMemoryLocations.h"

struct zAllocator {
    zAllocator() = delete;
    ~zAllocator() = delete;

    static void* zNew( size_t size ) {
        typedef void* (__cdecl* zMallocFunc)(size_t size);
        return (*reinterpret_cast<zMallocFunc*>(GothicMemoryLocations::zAllocator::Malloc))(size);
    }
    static void zFree( void* ptr ) {
        typedef void (__cdecl* zFreeFunc)(void* ptr);
        return (*reinterpret_cast<zFreeFunc*>(GothicMemoryLocations::zAllocator::Free))(ptr);
    }
};
