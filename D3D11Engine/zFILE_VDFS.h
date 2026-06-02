#pragma once
#include "zSTRING.h"

typedef int zERROR_ID;

class zFILE_VDFS {
public:

    static zFILE_VDFS* Create( const zSTRING& fileName ) {
        auto file = (zFILE_VDFS*)malloc( GothicMemoryLocations::zFILE_VDFS::StructSize );
        if ( file ) {
            reinterpret_cast<void( __thiscall* )(zFILE_VDFS*, const zSTRING&)>(GothicMemoryLocations::zFILE_VDFS::Constructor2)(file, fileName);
        }
        return file;
    }

    static void Delete( zFILE_VDFS* _this ) {
        reinterpret_cast<void( __thiscall* )(zFILE_VDFS*)>(GothicMemoryLocations::zFILE_VDFS::Destructor)(_this);
        free( _this );
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
