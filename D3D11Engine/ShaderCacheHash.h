#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

// Shared FNV-1a 64 hashing for the D3D11/D3D12 on-disk shader caches.
namespace ShaderCacheHash {
    constexpr uint64_t kOffset = 1469598103934665603ull;
    constexpr uint64_t kPrime = 1099511628211ull;

    inline uint64_t HashBytes( const void* data, size_t size, uint64_t seed = kOffset ) {
        const unsigned char* p = static_cast<const unsigned char*>( data );
        uint64_t h = seed;
        for ( size_t i = 0; i < size; ++i ) { h ^= p[i]; h *= kPrime; }
        return h;
    }
    inline uint64_t HashString( const char* str, uint64_t seed = kOffset ) {
        return str ? HashBytes( str, strlen( str ), seed ) : HashBytes( "", 0, seed );
    }
}
