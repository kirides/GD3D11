#pragma once
#include "zSTRING.h"

typedef int zERROR_ID;

class zFILE_VDFS {
public:
    zFILE_VDFS() = delete;
    ~zFILE_VDFS() = delete;

    // --- Lifetime Management ---
    // Returns a managed unique_ptr with a custom deleter to prevent memory leaks
    struct Deleter {
        void operator()( zFILE_VDFS* file ) const {
            if ( file ) {
                reinterpret_cast<void( __thiscall* )(zFILE_VDFS*)>(GothicMemoryLocations::zFILE_VDFS::Destructor)(file);
                std::free( file );
            }
        }
    };
    using Ptr = std::unique_ptr<zFILE_VDFS, Deleter>;

    static Ptr Create( const zSTRING& fileName ) {
        auto* memory = std::malloc( GothicMemoryLocations::zFILE_VDFS::StructSize );
        if ( !memory ) return nullptr;

        auto* file = reinterpret_cast<zFILE_VDFS*>(memory);
        reinterpret_cast<void( __thiscall* )(zFILE_VDFS*, const zSTRING&)>(GothicMemoryLocations::zFILE_VDFS::Constructor2)(file, fileName);
        return Ptr( file );
    }

    bool Exists() {
        return reinterpret_cast<bool( __thiscall* )(zFILE_VDFS*)>(GothicMemoryLocations::zFILE_VDFS::Exists)(this);
    }

    zERROR_ID Open( bool writeMode ) {
        return reinterpret_cast<zERROR_ID( __thiscall* )(zFILE_VDFS*, bool)>(GothicMemoryLocations::zFILE_VDFS::Open)(this, writeMode);
    }
    zERROR_ID Close() {
        return reinterpret_cast<zERROR_ID( __thiscall* )(zFILE_VDFS*)>(GothicMemoryLocations::zFILE_VDFS::Close)(this);
    }

    long Read( void* scr, long bytes ) {
        return reinterpret_cast<long( __thiscall* )(zFILE_VDFS*, void*, long)>(GothicMemoryLocations::zFILE_VDFS::Read)(this, scr, bytes);
    }

    long Size() {
        return reinterpret_cast<long( __thiscall* )(zFILE_VDFS*)>(GothicMemoryLocations::zFILE_VDFS::Size)(this);
    }
};
