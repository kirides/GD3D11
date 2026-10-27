#include "ShaderNames.h"

#include <mutex>
#include <unordered_map>

namespace {
    uint64_t HashCode( const void* code, size_t size ) {
        uint64_t h = 0xCBF29CE484222325ull;
        const auto* p = static_cast<const uint8_t*>( code );
        for ( size_t i = 0; i < size; ++i ) h = ( h ^ p[i] ) * 0x100000001B3ull;
        return h ^ size;
    }

    std::mutex g_Mutex;
    std::unordered_map<uint64_t, std::string> g_Names;
}

namespace ShaderNames {
    void Register( const void* code, size_t size, const std::string& name ) {
        if ( !code || !size ) return;
        const uint64_t key = HashCode( code, size );
        std::lock_guard<std::mutex> lock( g_Mutex );
        g_Names[key] = name;
    }

    std::string Lookup( const void* code, size_t size ) {
        if ( !code || !size ) return {};
        const uint64_t key = HashCode( code, size );
        std::lock_guard<std::mutex> lock( g_Mutex );
        const auto it = g_Names.find( key );
        return it != g_Names.end() ? it->second : std::string();
    }

    std::string Describe( const void* vs, size_t vsSize, const void* ps, size_t psSize ) {
        const std::string v = Lookup( vs, vsSize ), p = Lookup( ps, psSize );
        if ( v.empty() ) return p;
        return p.empty() ? v : v + " | " + p;
    }
}
