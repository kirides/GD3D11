#include "AutomationInput.h"

#define DIRECTINPUT_VERSION 0x0700
#include <dinput.h>
#include <thread>
#include "GothicMemoryLocations.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "BaseGraphicsEngine.h"
#include "ImGuiSettingsWindow.h"
#include "ImGuiShim.h"
#include "oCGame.h"
#include "oCNPC.h"
#include "zCCamera.h"
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

        // ---- Command channel (main thread: WM_AUTOMATION from the driver, or the per-frame tick) ---------------
        // <exe dir>\auto_cmd.txt holds "<id> <command> [args]"; the answer lands in auto_result.txt as "<id> OK|ERR ...".
        // Runtime settings come from a separate ini (never UserSettings.ini) and are never saved.
        constexpr const char* kRuntimeSettingsIni = "system\\GD3D11\\auto_settings.ini";  // relative to the game's cwd

#if !defined( BUILD_SPACER ) && !defined( BUILD_SPACER_NET )
#define AUTOMATION_HAS_GAME_CALLS 1
#if defined( BUILD_GOTHIC_2_6_fix ) || ( defined( BUILD_GOTHIC_1_08k ) && !defined( BUILD_1_12F ) )
#define AUTOMATION_HAS_BEAM 1
#endif
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

#ifdef AUTOMATION_HAS_BEAM
        /** Walks the loaded world's way-net list (zCListSort: {compare, data, next}); names at +0x5C, position at +0x44. */
        template <typename Fn>
        bool ForEachWaypoint( Fn&& fn ) {
            oCGame* game = oCGame::GetGame();
            if ( !game || !game->_zCSession_world ) return false;
            auto* world = reinterpret_cast<BYTE*>(game->_zCSession_world);
            auto* wayNet = *reinterpret_cast<BYTE**>(world + GothicMemoryLocations::zCWorld::Offset_WayNet);
            if ( !wayNet ) return false;
            struct Node { void* compare; BYTE* data; Node* next; };
            for ( Node* n = reinterpret_cast<Node*>(wayNet + 0x28); n; n = n->next ) {
                if ( !n->data ) continue;
                const float* pos = reinterpret_cast<const float*>(n->data + 0x44);
                const zSTRING* name = reinterpret_cast<const zSTRING*>(n->data + 0x5C);
                fn( name->ToChar(), XMFLOAT3( pos[0], pos[1], pos[2] ) );
            }
            return true;
        }

        std::string DumpWaypoints() {
            std::string out;
            ForEachWaypoint( [&]( const char* name, const XMFLOAT3& p ) {
                out += std::format( "{} {:.1f} {:.1f} {:.1f}\n", name, p.x, p.y, p.z );
            } );
            return out;
        }

        bool FindWaypoint( const std::string& name, XMFLOAT3& out ) {
            bool found = false;
            ForEachWaypoint( [&]( const char* n, const XMFLOAT3& p ) {
                if ( !found && _stricmp( n, name.c_str() ) == 0 ) { out = p; found = true; }
            } );
            return found;
        }
#endif

#ifdef AUTOMATION_HAS_GAME_CALLS
        // ---- Free camera: Gothic's own free-look (F6, as the editor does) with the hero put to sleep ---------------
        bool g_FreeCam = false;
        float g_Yaw = 0, g_Pitch = 0;  // radians; forward = (cos p sin y, -sin p, cos p cos y), as in the editor camera

        zCVob* CamVob() {
            oCGame* game = oCGame::GetGame();
            return game ? game->_zCSession_camVob : nullptr;
        }

        void ReadCameraAngles() {
            const XMMATRIX w = XMMatrixTranspose( XMLoadFloat4x4( CamVob()->GetWorldMatrixPtr() ) );
            XMFLOAT4X4 m;
            XMStoreFloat4x4( &m, w );
            g_Yaw = atan2f( m._31, m._33 );
            g_Pitch = atan2f( -m._32, sqrtf( m._31 * m._31 + m._33 * m._33 ) );
        }

        void ApplyPose( const XMFLOAT3& pos, float yaw, float pitch ) {
            const float maxPitch = XM_PIDIV2 - 0.01f;
            g_Yaw = yaw;
            g_Pitch = std::clamp( pitch, -maxPitch, maxPitch );
            const XMMATRIX world = XMMatrixRotationRollPitchYaw( g_Pitch, g_Yaw, 0.0f ) * XMMatrixTranslation( pos.x, pos.y, pos.z );
            XMStoreFloat4x4( CamVob()->GetWorldMatrixPtr(), XMMatrixTranspose( world ) );
            zCCamera::GetCamera()->Activate();
            Engine::GAPI->SetViewTransformXM( Engine::GAPI->GetViewMatrixXM() );
        }

        std::string SetFreeCam( bool on ) {
            if ( !CamVob() ) return "ERR no world loaded";
            if ( Engine::ImGuiHandle && Engine::ImGuiHandle->GetIsEditorVisible() ) return "ERR close the editor panel first";
            if ( on == g_FreeCam ) return "OK";
            if ( on ) {
                oCGame::GetGame()->TestKey( GOTHIC_KEY::F6 );   // free-look on
                Engine::GAPI->SetEnableGothicInput( false );    // hero + camera AI sleep, game input off
                ReadCameraAngles();
            } else {
                oCGame::GetGame()->TestKey( GOTHIC_KEY::F4 );   // free-look off
                Engine::GAPI->SetEnableGothicInput( true );
            }
            g_FreeCam = on;
            return "OK";
        }

        std::string PoseText() {
            const XMFLOAT3 p = Engine::GAPI->GetCameraPosition();
            return std::format( "OK {:.1f} {:.1f} {:.1f} {:.2f} {:.2f}", p.x, p.y, p.z, XMConvertToDegrees( g_Yaw ), XMConvertToDegrees( g_Pitch ) );
        }

        std::vector<std::string> Split( const std::string& s ) {
            std::vector<std::string> out;
            size_t i = 0;
            while ( i < s.size() ) {
                while ( i < s.size() && s[i] == ' ' ) i++;
                const size_t j = s.find( ' ', i );
                if ( i < s.size() ) out.push_back( s.substr( i, j == std::string::npos ? std::string::npos : j - i ) );
                if ( j == std::string::npos ) break;
                i = j;
            }
            return out;
        }

        bool ToFloat( const std::string& s, float& out ) {
            char* end = nullptr;
            out = strtof( s.c_str(), &end );
            return end && *end == 0 && !s.empty();
        }

        /** campos x y z [yawDeg pitchDeg]  |  lookat x y z|@name [dist]  |  camget  |  freecam on|off */
        std::string ExecuteCamera( const std::string& cmd, const std::string& arg ) {
            if ( cmd == "freecam" ) return SetFreeCam( arg == "on" || arg == "1" );
            if ( !CamVob() ) return "ERR no world loaded";
            if ( cmd == "camget" ) {
                if ( !g_FreeCam ) ReadCameraAngles();
                return PoseText();
            }
            const auto t = Split( arg );
            if ( cmd == "campos" ) {
                float v[5] = {};
                if ( t.size() < 3 || t.size() == 4 || t.size() > 5 ) return "ERR usage: campos x y z [yaw pitch]";
                for ( size_t i = 0; i < t.size(); i++ ) if ( !ToFloat( t[i], v[i] ) ) return "ERR bad number " + t[i];
                if ( auto r = SetFreeCam( true ); r != "OK" ) return r;
                ApplyPose( XMFLOAT3( v[0], v[1], v[2] ), t.size() == 5 ? XMConvertToRadians( v[3] ) : g_Yaw, t.size() == 5 ? XMConvertToRadians( v[4] ) : g_Pitch );
                return PoseText();
            }
            if ( cmd == "lookat" ) {
                if ( t.empty() ) return "ERR usage: lookat x y z|@name [dist]";
                XMFLOAT3 target;
                size_t next = 1;
                if ( t[0][0] == '@' ) {
                    const std::string name = t[0].substr( 1 );
                    if ( _stricmp( name.c_str(), "hero" ) == 0 || _stricmp( name.c_str(), "player" ) == 0 ) {
                        oCNPC* player = oCGame::GetPlayer();
                        if ( !player ) return "ERR no player";
                        target = player->GetPositionWorld();
                    }
#ifdef AUTOMATION_HAS_BEAM
                    else if ( !FindWaypoint( name, target ) ) return "ERR unknown waypoint " + name;
#else
                    else return "ERR named targets are not supported on this Gothic build";
#endif
                } else {
                    float v[3];
                    if ( t.size() < 3 ) return "ERR usage: lookat x y z|@name [dist]";
                    for ( int i = 0; i < 3; i++ ) if ( !ToFloat( t[i], v[i] ) ) return "ERR bad number " + t[i];
                    target = XMFLOAT3( v[0], v[1], v[2] );
                    next = 3;
                }
                float dist = 0;
                if ( t.size() > next && !ToFloat( t[next], dist ) ) return "ERR bad distance " + t[next];
                if ( auto r = SetFreeCam( true ); r != "OK" ) return r;

                const XMFLOAT3 cam = Engine::GAPI->GetCameraPosition();
                XMVECTOR dir = XMVectorSet( target.x - cam.x, target.y - cam.y, target.z - cam.z, 0 );
                if ( XMVectorGetX( XMVector3Length( dir ) ) < 1e-3f ) return "ERR camera is at the target";
                dir = XMVector3Normalize( dir );
                XMFLOAT3 d;
                XMStoreFloat3( &d, dir );
                XMFLOAT3 pos = cam;
                if ( dist > 0 ) {  // fly toward the target along the current line of sight and stop dist away
                    pos = XMFLOAT3( target.x - d.x * dist, target.y - d.y * dist, target.z - d.z * dist );
                }
                ApplyPose( pos, atan2f( d.x, d.z ), atan2f( -d.y, sqrtf( d.x * d.x + d.z * d.z ) ) );
                return PoseText();
            }
            return {};
        }

        /** Loads the runtime ini over the live settings (the driver writes launch ini + overrides there). Never saves. */
        std::string ApplyRuntimeSettings( bool reloadShaders ) {
            if ( !Toolbox::FileExists( kRuntimeSettingsIni ) ) return std::string( "ERR " ) + kRuntimeSettingsIni + " missing";
            auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
            Engine::GAPI->LoadMenuSettings( kRuntimeSettingsIni );
            Engine::GAPI->LoadRendererWorldSettings( settings, kRuntimeSettingsIni );
            ImGuiSettings::FixupSettings( settings );
            if ( Engine::GraphicsEngine ) {
                settings.ApplyDeviceCapabilities( Engine::GraphicsEngine->GetDeviceCapabilities() );
                if ( reloadShaders ) Engine::GraphicsEngine->ReloadShaders();
            }
            Engine::GAPI->UpdateTextureMaxSize();
            return "OK";
        }
#endif

        std::string Execute( const std::string& cmd, const std::string& arg ) {
#ifdef AUTOMATION_HAS_GAME_CALLS
            if ( cmd == "applyini" ) return ApplyRuntimeSettings( arg == "reload" );
            if ( cmd == "reload" ) {
                if ( !Engine::GraphicsEngine ) return "ERR no graphics engine";
                Engine::GraphicsEngine->ReloadShaders();
                return "OK";
            }
            if ( cmd == "freecam" || cmd == "campos" || cmd == "lookat" || cmd == "camget" ) return ExecuteCamera( cmd, arg );
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
#endif
            return "ERR unknown command " + cmd;
#else
            return "ERR automation commands are not supported on this build";
#endif
        }
    }

    void Tick( bool force ) {
        if ( !g_Keys ) return;
        static unsigned frame = 0;
        if ( !force && (++frame & 7) != 0 ) return;  // poll about every 8th frame
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
