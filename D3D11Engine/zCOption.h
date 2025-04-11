#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "BaseGraphicsEngine.h"
#include <algorithm>
#include "zSTRING.h"

class zCOption {
public:

    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCOptionReadInt), hooked_zOptionReadInt );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCOptionReadBool), hooked_zOptionReadBool );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCOptionReadDWORD), hooked_zOptionReadDWORD );
    }

    /** Returns true if the given string is in the commandline of the game */
    bool IsParameter( const std::string& str ) {
        std::string cmdLine = GetCommandLineNormalized();
        if ( cmdLine == "" ) {
            return false;
        }

        std::string cmd = str;

        // Make them uppercase
        std::transform( cmd.begin(), cmd.end(), cmd.begin(), ::toupper );

        return cmdLine.find( "-" + cmd ) != std::string::npos;
    }

    /** Returns the commandline */
    std::string GetCommandline() {
        char* s = GetCommandLineA();
        if ( *s == '"' ) {
            ++s;
            while ( *s ) if ( *s++ == '"' ) break; // skip until end of "..."
        } else {
            while ( *s && *s != ' ' && *s != '\t' ) ++s; // skip until first space/tab
        }
        while ( *s == ' ' || *s == '\t' ) s++; // Skip whitespace

        std::string cmdLine = s;
        return cmdLine;
    }

    /** Returns the value of the given parameter. If the parameter is not in the commandline, it returns "" */
    std::string ParameterValue( const std::string& str ) {
        std::string cmdLine = GetCommandLineNormalized();
        if ( cmdLine == "" ) {
            return "";
        }

        std::string cmd = str;

        // Make them uppercase
        std::transform( cmd.begin(), cmd.end(), cmd.begin(), ::toupper );

        int pos = cmdLine.find( "-" + cmd );
        if ( pos == std::string::npos )
            return ""; // Not in commandline

        unsigned int paramPos = pos + 1 + cmd.length() + 1; // Skip everything until the -, then 
                                                   // the -, then the param-name and finally the :
        // Safety-check
        if ( paramPos >= cmdLine.length() )
            return "";

        // *Snip*
        std::string arg = cmdLine.substr( paramPos );

        // Snip the rest of the commandline, if there is any
        if ( arg.find_first_of( ' ' ) != std::string::npos )
            arg = arg.substr( 0, arg.find_first_of( ' ' ) );

        return arg;
    }

    /** Reads config stuff */
    static int __fastcall hooked_zOptionReadBool( void* thisptr, void* unknwn, zSTRING const& section, char const* var, int def ) {

        int r = HookedFunctions::OriginalFunctions.original_zCOptionReadBool( thisptr, section, var, def );
        if ( _stricmp( var, "zWaterAniEnabled" ) == 0 ) {
            Engine::GAPI->SetIntParamFromConfig( "zWaterAniEnabled", 0 );
            return 0; // Disable water animations
        } else if ( _stricmp( var, "zStartupWindowed" ) == 0 ) {
            Engine::GAPI->SetIntParamFromConfig( "zStartupWindowed", r );
            return 1;
        } else if ( _stricmp( var, "gameAbnormalExit" ) == 0 ) {
#ifndef PUBLIC_RELEASE
            // No VDFS bullshit when testing
            return 0;
#endif
        }

        Engine::GAPI->SetIntParamFromConfig( var, r );
        return r;
    }

    /** Reads config stuff */
    static long __fastcall Do_hooked_zOptionReadInt( void* thisptr, zSTRING const& section, char const* var, int def ) {
        BaseGraphicsEngine* engine = Engine::GraphicsEngine;
        // TODO: Make Option checkable
        // LogInfo() << "Reading Gothic-Config: " << var;
        if ( !engine ) {
            LogWarn() << "ENGINE wasn't initialized yet! WTF! - Reading Gothic-Config: " << var;
        }

        static bool once = false;
        if ( !once ) {
            once = true;
            LogInfo() << "Forcing zVidResFullscreenX";
            LogInfo() << "Forcing zVidResFullscreenY";
            LogInfo() << "Forcing zVidResFullscreenBPP = 32";
            LogInfo() << "Forcing zTexMaxSize = 16384";
            LogInfo() << "Forcing zTexCacheOutTimeMSec = 1024";
            LogInfo() << "Forcing zTexCacheSizeMaxBytes = 2147483648";
            LogInfo() << "Forcing zSndCacheOutTimeMSec = 1024";
            LogInfo() << "Forcing zSndCacheSizeMaxBytes = 536870912";
        }

        if ( _stricmp( var, "zVidResFullscreenX" ) == 0 ) {
            if ( engine ) {
                return engine->GetResolution().x;
            }
        } else if ( _stricmp( var, "zVidResFullscreenY" ) == 0 ) {
            if ( engine ) {
                return engine->GetResolution().y;
            }
        } else if ( _stricmp( var, "zVidResFullscreenBPP" ) == 0 ) {
            return 32;
        } else if ( _stricmp( var, "zTexMaxSize" ) == 0 ) {
            return Engine::GAPI->GetRendererState().RendererSettings.textureMaxSize;
        } else if ( _stricmp( var, "zTexCacheOutTimeMSec" ) == 0 ) // Previous values were taken outta fucking ass
        {
            return 1024;
        } else if ( _stricmp( var, "zTexCacheSizeMaxBytes" ) == 0 ) {
            return 2147483648;
        } else if ( _stricmp( var, "zSndCacheOutTimeMSec" ) == 0 ) {
            return 1024;
        } else if ( _stricmp( var, "zSndCacheSizeMaxBytes" ) == 0 ) {
            return 536870912;
        } else if ( _stricmp( var, "zVidDevice" ) == 0 ) {
            return 0;
        }

        return HookedFunctions::OriginalFunctions.original_zCOptionReadInt( thisptr, section, var, def );
    }

    /** Reads config stuff */
    static unsigned long __fastcall hooked_zOptionReadDWORD( void* thisptr, void* unknwn, zSTRING const& section, char const* var, unsigned long def ) {
        BaseGraphicsEngine* engine = Engine::GraphicsEngine;
        // TODO: Make Option checkable
        // LogInfo() << "Reading Gothic-Config: " << var;

        if ( _stricmp( var, "zTexCacheOutTimeMSec" ) == 0 ) // Previous values were taken from fucking ass
        {
            return 1024;
        } else if ( _stricmp( var, "zTexCacheSizeMaxBytes" ) == 0 ) {
            return 2147483648;
        } else if ( _stricmp( var, "zSndCacheOutTimeMSec" ) == 0 ) {
            return 1024;
        } else if ( _stricmp( var, "zSndCacheSizeMaxBytes" ) == 0 ) {
            return 536870912;
        }

        return HookedFunctions::OriginalFunctions.original_zCOptionReadDWORD( thisptr, section, var, def );
    }

    static long __fastcall hooked_zOptionReadInt( void* thisptr, void* unknwn, zSTRING const& section, char const* var, int def ) {
        int i = Do_hooked_zOptionReadInt( thisptr, section, var, def );

        // Save the variable
        Engine::GAPI->SetIntParamFromConfig( "var", i );

        return i;
    }

    void WriteString( zSTRING const& section, char const* var, zSTRING def ) {
        reinterpret_cast<void( __thiscall* )( zCOption*, zSTRING const&, const char*, zSTRING, int )>( GothicMemoryLocations::zCOption::WriteString )( this, section, var, def, 0 );
    }

    static zCOption* GetOptions() { return *reinterpret_cast<zCOption**>(GothicMemoryLocations::GlobalObjects::zCOption); }
private:
    std::string GetCommandLineNormalized() {
        std::string cmdLine = this->GetCommandline();

        // Make them uppercase
        std::transform( cmdLine.begin(), cmdLine.end(), cmdLine.begin(), ::toupper );

        return cmdLine;
    }
};
