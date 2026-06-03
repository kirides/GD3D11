#pragma once
#include "zSTRING.h"
#include "zAllocator.h"

typedef int zERROR_ID;

class zFILE_VDFS {
private:
    ~zFILE_VDFS() {
        reinterpret_cast<void( __thiscall* )(zFILE_VDFS*)>(GothicMemoryLocations::zFILE_VDFS::Destructor)(this);
    }
public:
    zFILE_VDFS( const zSTRING& fileName ) {
        reinterpret_cast<void( __thiscall* )(zFILE_VDFS*, const zSTRING&)>(GothicMemoryLocations::zFILE_VDFS::Constructor2)(this, fileName);
    }

    struct Deleter {
        void operator()( zFILE_VDFS* file ) const {
            if ( file ) {
                delete file;
            }
        }
    };

    using Ptr = std::unique_ptr<zFILE_VDFS, Deleter>;

    static Ptr Create( const zSTRING& fileName ) {

        auto ptr = new zFILE_VDFS( fileName );

        return Ptr( ptr );
    }

    static void* operator new(std::size_t count) {
        return zAllocator::zNew( std::max( count, GothicMemoryLocations::zFILE_VDFS::StructSize ) );
    }

    static void operator delete(void* ptr) {
        zAllocator::zFree( ptr );
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
