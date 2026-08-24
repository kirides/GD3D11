#pragma once

/** Tiny in-memory binary serialization helpers shared by the SqliteBlobStore-backed caches
    (MeshOptimizeCache.h, D3D11ShaderManager.cpp, D3D12ShaderBackend.cpp). Each of those owns its own
    record layout (magic + format version + key + payload) and validity rules - this only factors out the
    byte-level Append/Read plumbing so it isn't hand-rolled three times. */

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ByteCursor {

    template<typename T> void AppendPod( std::vector<uint8_t>& buf, const T& value ) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>( &value );
        buf.insert( buf.end(), p, p + sizeof( T ) );
    }
    inline void AppendBytes( std::vector<uint8_t>& buf, const void* data, size_t size ) {
        const uint8_t* p = static_cast<const uint8_t*>( data );
        buf.insert( buf.end(), p, p + size );
    }
    template<typename T> void AppendVector( std::vector<uint8_t>& buf, const std::vector<T>& v ) {
        if ( !v.empty() ) {
            AppendBytes( buf, v.data(), v.size() * sizeof( T ) );
        }
    }
    inline void AppendString( std::vector<uint8_t>& buf, const std::string& s ) {
        AppendBytes( buf, s.data(), s.size() );
    }

    /** Bounds-checked read cursor over an in-memory buffer (e.g. what SqliteBlobStore::TryGet hands
        back). Every Read* returns false and leaves the buffer(s) it was reading into unspecified on
        underflow, so a truncated or corrupted blob is just a miss - never a read past the end. */
    struct Reader {
        const uint8_t* p;
        const uint8_t* end;

        Reader( const uint8_t* data, size_t size ) : p( data ), end( data + size ) {}

        size_t Remaining() const { return static_cast<size_t>( end - p ); }

        template<typename T> bool ReadPod( T& out ) {
            if ( Remaining() < sizeof( T ) ) return false;
            memcpy( &out, p, sizeof( T ) );
            p += sizeof( T );
            return true;
        }
        bool ReadBytes( void* dst, size_t size ) {
            if ( Remaining() < size ) return false;
            if ( size ) memcpy( dst, p, size );
            p += size;
            return true;
        }
        template<typename T> bool ReadVector( std::vector<T>& out, uint32_t count, uint32_t maxCount ) {
            if ( count > maxCount ) return false;
            out.resize( count );
            return ReadBytes( out.data(), static_cast<size_t>( count ) * sizeof( T ) );
        }
        bool ReadString( std::string& out, uint32_t len ) {
            out.resize( len );
            return ReadBytes( out.data(), len );
        }
    };

}   // namespace ByteCursor
