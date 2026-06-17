#pragma once
#include "zSTRING.h"
#include "zAllocator.h"

typedef int zERROR_ID;

enum zERRORS {
    zERROR_NONE = 0,
};

class zFILE_VDFS {
private:
    struct {
        void* _Destructor;
        void* SetMode;
        void* GetMode;
        void* SetPath;
        void* SetDrive;
        void* SetDir;
        void* SetFile;
        void* SetFilename;
        void* SetExt;
        void* GetFileHandle; // 10
        void* GetFullPath;
        void* GetPath;
        void* GetDirectoryPath;
        void* GetDrive;
        void* GetDir;
        void* GetFile;
        void* GetFilename;
        void* GetExt;
        void* SetCurrentDir;
        void* ChangeDir; // 20
        void* SearchFile;
        void* FindFirst;
        void* FindNext;
        void* DirCreate;
        void* DirRemove;
        void* DirExists;
        void* FileMove;
        void* FileCopy;
        void* FileMove2;
        void* FileCopy2;  // 30
        void* FileDelete;
        void* IsOpened;
        void* Create;
        void* Create2; // const zSTRING& s
        void* Open2; // const zSTRING& s, bool writeMode = false
        zERROR_ID( __thiscall* Open )(zFILE_VDFS*, bool);
        void* Exists2; // const zSTRING& s
        bool( __thiscall* Exists )(zFILE_VDFS*);
        zERROR_ID( __thiscall* Close )(zFILE_VDFS*);
        void* Reset;
        void* Append;
        long( __thiscall* Size )(zFILE_VDFS*);
        void* Pos;
        void* Seek;
        void* Eof;
        void* GetStats;
        void* Write;
        void* Write2;
        void* Write3;
        void* Read2;
        void* Read3;
        long( __thiscall* Read )(zFILE_VDFS*, void*, long);
        void* ReadChar;
        void* SeekText;
        void* ReadBlock;
        void* UpdateBlock;
        void* GetFreeDiskSpace;
        void* FlushBuffer;
    } *vftable;

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
        return vftable->Exists( this );
    }

    zERROR_ID Open( bool openWrite ) {
        return vftable->Open( this, openWrite );
    }
    zERROR_ID Close() {
        return vftable->Close( this );
    }

    long Read( void* scr, long bytes ) {
        return vftable->Read( this, scr, bytes );
    }

    long Size() {
        return vftable->Size( this );
    }
};
