#include "AutomationInput.h"

#define DIRECTINPUT_VERSION 0x0700
#include <dinput.h>
#include <thread>
#include "GothicMemoryLocations.h"
#include "oCGame.h"
#include "oCNPC.h"
#include "zCWorld.h"
#include "zSTRING.h"

namespace AutomationInput {
    namespace {
        using GetDeviceStateFn = HRESULT( WINAPI* )(IDirectInputDevice7A*, DWORD, LPVOID);
        using GetDeviceDataFn = HRESULT( WINAPI* )(IDirectInputDevice7A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

        constexpr int VtGetDeviceState = 9;
        constexpr int VtGetDeviceData = 10;

        volatile BYTE* g_Keys = nullptr;      // shared with the driver
        BYTE g_LastSent[256] = {};            // what GetDeviceData has already reported as events
        GetDeviceStateFn g_OrigState = nullptr;
        GetDeviceDataFn g_OrigData = nullptr;

        HRESULT WINAPI HookedGetDeviceState( IDirectInputDevice7A* self, DWORD cb, LPVOID data ) {
            const HRESULT hr = g_OrigState( self, cb, data );
            if ( SUCCEEDED( hr ) && cb == 256 && data ) {
                auto* state = static_cast<BYTE*>(data);
                for ( int i = 0; i < 256; i++ ) state[i] |= g_Keys[i];
            }
            return hr;
        }

        HRESULT WINAPI HookedGetDeviceData( IDirectInputDevice7A* self, DWORD cb, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inOut, DWORD flags ) {
            const DWORD capacity = (rgdod && inOut) ? *inOut : 0;
            const HRESULT hr = g_OrigData( self, cb, rgdod, inOut, flags );
            if ( FAILED( hr ) || !rgdod || !inOut || cb != sizeof( DIDEVICEOBJECTDATA ) ) return hr;
            // Report synthetic key transitions as buffered events (a peek must not consume them).
            for ( int i = 0; i < 256 && *inOut < capacity; i++ ) {
                const BYTE now = g_Keys[i] & 0x80;
                if ( now == (g_LastSent[i] & 0x80) ) continue;
                DIDEVICEOBJECTDATA& e = rgdod[(*inOut)++];
                e = {};
                e.dwOfs = i;
                e.dwData = now;
                e.dwTimeStamp = GetTickCount();
                if ( !(flags & DIGDD_PEEK) ) g_LastSent[i] = now;
            }
            return hr;
        }

        template <typename Fn>
        void PatchVtable( void** vtable, int index, Fn hook, Fn& orig ) {
            DWORD old;
            if ( !VirtualProtect( &vtable[index], sizeof( void* ), PAGE_READWRITE, &old ) ) return;
            orig = reinterpret_cast<Fn>(vtable[index]);
            vtable[index] = reinterpret_cast<void*>(hook);
            VirtualProtect( &vtable[index], sizeof( void* ), old, &old );
        }

        // ---- Command channel (main thread, from GothicAPI::OnWorldUpdate) -----------------------------------
        // <exe dir>\auto_cmd.txt holds "<id> <command> [args]"; the answer lands in auto_result.txt as "<id> OK|ERR ...".
#if defined( BUILD_GOTHIC_2_6_fix ) || ( defined( BUILD_GOTHIC_1_08k ) && !defined( BUILD_1_12F ) )
#define AUTOMATION_HAS_BEAM 1
#endif
        std::string g_Dir;

        void WriteResult( const std::string& id, const std::string& text ) {
            const std::string tmp = g_Dir + "auto_result.tmp", dst = g_Dir + "auto_result.txt";
            if ( FILE* f = fopen( tmp.c_str(), "wb" ) ) {
                fprintf( f, "%s %s\n", id.c_str(), text.c_str() );
                fclose( f );
                MoveFileExA( tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING );
            }
        }

        /** Walks the loaded world's way-net list (zCListSort: {compare, data, next}); names at +0x5C, position at +0x44. */
        std::string DumpWaypoints() {
#ifdef AUTOMATION_HAS_BEAM
            oCGame* game = oCGame::GetGame();
            if ( !game || !game->_zCSession_world ) return {};
            auto* world = reinterpret_cast<BYTE*>(game->_zCSession_world);
            auto* wayNet = *reinterpret_cast<BYTE**>(world + GothicMemoryLocations::zCWorld::Offset_WayNet);
            if ( !wayNet ) return {};
            std::string out;
            struct Node { void* compare; BYTE* data; Node* next; };
            for ( Node* n = reinterpret_cast<Node*>(wayNet + 0x28); n; n = n->next ) {
                if ( !n->data ) continue;
                const float* pos = reinterpret_cast<const float*>(n->data + 0x44);
                const zSTRING* name = reinterpret_cast<const zSTRING*>(n->data + 0x5C);
                out += std::format( "{} {:.1f} {:.1f} {:.1f}\n", name->ToChar(), pos[0], pos[1], pos[2] );
            }
            return out;
#else
            return {};
#endif
        }

        std::string Execute( const std::string& cmd, const std::string& arg ) {
#ifdef AUTOMATION_HAS_BEAM
            oCNPC* player = oCGame::GetPlayer();
            if ( !player ) return "ERR no player (world not loaded)";
            if ( cmd == "beam" ) {
                zSTRING name( arg.c_str() );
                const int ok = reinterpret_cast<int( __fastcall* )(oCNPC*, int, const zSTRING&)>(GothicMemoryLocations::oCNPC::BeamTo)(player, 0, name);
                return ok ? "OK" : "ERR waypoint/freepoint/vob not found: " + arg;
            }
            if ( cmd == "pos" ) {
                const XMFLOAT3 p = player->GetPositionWorld();
                return std::format( "OK {:.1f} {:.1f} {:.1f}", p.x, p.y, p.z );
            }
            if ( cmd == "waypoints" ) {
                const std::string list = DumpWaypoints();
                if ( list.empty() ) return "ERR no way-net";
                if ( FILE* f = fopen( (g_Dir + "auto_waypoints.txt").c_str(), "wb" ) ) { fwrite( list.data(), 1, list.size(), f ); fclose( f ); }
                return "OK " + std::to_string( std::count( list.begin(), list.end(), '\n' ) );
            }
            return "ERR unknown command " + cmd;
#else
            return "ERR teleport is not supported on this Gothic build";
#endif
        }
    }

    void Tick() {
        if ( !g_Keys ) return;
        static unsigned frame = 0;
        if ( (++frame & 7) != 0 ) return;  // poll about every 8th frame
        const std::string path = g_Dir + "auto_cmd.txt";
        FILE* f = fopen( path.c_str(), "rb" );
        if ( !f ) return;
        char buf[512] = {};
        const size_t n = fread( buf, 1, sizeof( buf ) - 1, f );
        fclose( f );
        DeleteFileA( path.c_str() );
        std::string line( buf, n );
        while ( !line.empty() && (line.back() == '\n' || line.back() == '\r') ) line.pop_back();
        const size_t a = line.find( ' ' );
        if ( a == std::string::npos ) return;
        const size_t b = line.find( ' ', a + 1 );
        const std::string id = line.substr( 0, a );
        const std::string cmd = line.substr( a + 1, b == std::string::npos ? std::string::npos : b - a - 1 );
        const std::string arg = b == std::string::npos ? std::string() : line.substr( b + 1 );
        const std::string result = Execute( cmd, arg );
        Logging::Inf( "Automation: '{}' -> {}", line, result );
        WriteResult( id, result );
    }

    void Init() {
#ifndef BUILD_SPACER
        std::string cmd = GetCommandLineA();
        for ( auto& c : cmd ) c = static_cast<char>(toupper( static_cast<unsigned char>(c)) );
        Logging::Inf( "Automation: Init, cmdline '{}'", cmd );
        if ( cmd.find( "-GAUTOMATION" ) == std::string::npos ) return;

        // File-backed so the driver needs no shared object namespace: <exe dir>/auto_keys.bin, 256 bytes.
        char path[MAX_PATH];
        GetModuleFileNameA( nullptr, path, MAX_PATH );
        *strrchr( path, '\\' ) = 0;
        g_Dir = std::string( path ) + "\\";
        strcat_s( path, "\\auto_keys.bin" );
        HANDLE file = CreateFileA( path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
        if ( file == INVALID_HANDLE_VALUE ) { Logging::Err( "Automation: cannot open {} ({})", path, GetLastError() ); return; }
        HANDLE map = CreateFileMappingA( file, nullptr, PAGE_READWRITE, 0, 256, nullptr );
        if ( !map ) { Logging::Err( "Automation: CreateFileMapping failed ({})", GetLastError() ); return; }
        g_Keys = static_cast<volatile BYTE*>(MapViewOfFile( map, FILE_MAP_ALL_ACCESS, 0, 0, 256 ));
        if ( !g_Keys ) { Logging::Err( "Automation: MapViewOfFile failed ({})", GetLastError() ); return; }
        memset( const_cast<BYTE*>(g_Keys), 0, 256 );
        const_cast<BYTE*>(g_Keys)[0] = 0xA5;  // handshake for the driver; DIK 0 is unused
        Logging::Inf( "Automation: key block ready" );

        // The keyboard device is created by the game after our DLL loads; wait for it, then patch its (class-shared) vtable.
        std::thread( [] {
            for ( int tries = 0; tries < 600; tries++ ) {
                Sleep( 100 );
                auto* dev = *reinterpret_cast<IDirectInputDevice7A**>(GothicMemoryLocations::GlobalObjects::DInput7DeviceKeyboard);
                if ( !dev ) continue;
                void** vtable = *reinterpret_cast<void***>(dev);
                PatchVtable( vtable, VtGetDeviceState, &HookedGetDeviceState, g_OrigState );
                PatchVtable( vtable, VtGetDeviceData, &HookedGetDeviceData, g_OrigData );
                Logging::Inf( "Automation: DirectInput keyboard hooked" );
                return;
            }
            Logging::Err( "Automation: keyboard device never appeared" );
        } ).detach();
#endif
    }
}
