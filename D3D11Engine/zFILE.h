#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCMaterial.h"
#include "zSTRING.h"

class zFILE {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_zFILEOpen, hooked_Open  );
    }

    static int __fastcall hooked_Open( void* thisptr, void* unknwn, zSTRING& str, bool b ) {
        // File looks like this:  \_WORK\DATA\TEXTURES\_COMPILED\NW_CITY_PFLASTERSTEIN_01-C.TEX
        std::string file = str.ToChar();
        std::string ext;
        std::string name;


        // Extract the file extension and its name
        int extpos = file.find_last_of( "." );
        if ( extpos >= 0 ) {
            ext = &file[extpos + 1];
            //LogInfo() << "Got file ext: " << ext;

            int slashpos = file.find_last_of( "\\" );
            if ( slashpos >= 0 ) {
                name = &file[slashpos + 1]; // Strip directories
                name.resize( name.size() - (ext.size() + 1) ); // Strip file extension

                //LogInfo() << "Got file name: " << name;
            }
        }

        name.resize( name.size() - 2 ); // Strip -C

        Engine::GAPI->SetTextureTestBindMode( true, name );

        //LogInfo() << "Opening file: " << name;

        return HookedFunctions::OriginalFunctions.original_zFILEOpen( thisptr, str, b );
    }
};
