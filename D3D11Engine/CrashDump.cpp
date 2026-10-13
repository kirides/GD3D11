#include "pch.h"
#include "CrashDump.h"

#include "Detours/detours.h"
#include "Engine.h"
#include "Logging.h"

#include <dbghelp.h>

namespace CrashDump {

    namespace {

        using MiniDumpWriteDumpFn = BOOL( WINAPI* )( HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
            PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION );
        using SetFilterFn = LPTOP_LEVEL_EXCEPTION_FILTER( WINAPI* )( LPTOP_LEVEL_EXCEPTION_FILTER );

        MiniDumpWriteDumpFn g_writeDump = nullptr;
        SetFilterFn g_origSetFilter = SetUnhandledExceptionFilter;
        LPTOP_LEVEL_EXCEPTION_FILTER g_next = nullptr; // the filter the game's CRT set; runs after the dump
        std::wstring g_dir;
        volatile LONG g_crashed = 0;

        struct Crash {
            EXCEPTION_POINTERS* Info;
            DWORD ThreadId;
        };

        /** "module+offset" of a code address. */
        std::string Where( const void* address ) {
            HMODULE module = nullptr;
            char path[MAX_PATH] = {};
            if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     static_cast<LPCSTR>( address ), &module ) && GetModuleFileNameA( module, path, MAX_PATH ) ) {
                const char* name = strrchr( path, '\\' );
                return std::format( "{}+{:x}", name ? name + 1 : path,
                    reinterpret_cast<uintptr_t>( address ) - reinterpret_cast<uintptr_t>( module ) );
            }
            return std::format( "{}", address );
        }

        /** Memory around the crashed thread's registers; IndirectlyReferencedMemory only follows stack pointers. */
        struct RegisterMemory {
            static constexpr uintptr_t Radius = 512;
            std::array<std::pair<ULONG64, ULONG>, 9> Ranges = {};
            size_t Count = 0;
            size_t Next = 0;

            void Add( uintptr_t center ) {
                MEMORY_BASIC_INFORMATION mbi;
                if ( !VirtualQuery( reinterpret_cast<void*>( center ), &mbi, sizeof( mbi ) ) || mbi.State != MEM_COMMIT ) return;
                constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                if ( !( mbi.Protect & readable ) || ( mbi.Protect & PAGE_GUARD ) ) return;

                // Clipped to the region so the dump never touches an unreadable neighbour.
                const uintptr_t regionLo = reinterpret_cast<uintptr_t>( mbi.BaseAddress );
                const uintptr_t regionHi = regionLo + mbi.RegionSize;
                const uintptr_t lo = std::max( center > Radius ? center - Radius : 0, regionLo );
                const uintptr_t hi = std::min( center < UINTPTR_MAX - Radius ? center + Radius : UINTPTR_MAX, regionHi );
                if ( hi > lo && Count < Ranges.size() ) {
                    Ranges[Count++] = { lo, static_cast<ULONG>( hi - lo ) };
                }
            }

            explicit RegisterMemory( const CONTEXT& ctx ) {
                for ( DWORD reg : { ctx.Eax, ctx.Ebx, ctx.Ecx, ctx.Edx, ctx.Esi, ctx.Edi, ctx.Ebp, ctx.Esp, ctx.Eip } ) {
                    Add( reg );
                }
            }
        };

        BOOL CALLBACK DumpCallback( void* param, PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output ) {
            if ( input->CallbackType != MemoryCallback ) return TRUE;
            RegisterMemory& memory = *static_cast<RegisterMemory*>( param );
            if ( memory.Next >= memory.Count ) return FALSE; // no more ranges
            output->MemoryBase = memory.Ranges[memory.Next].first;
            output->MemorySize = memory.Ranges[memory.Next].second;
            ++memory.Next;
            return TRUE;
        }

        /** Own thread: MiniDumpWriteDump wants the crashed thread stopped, and a stack overflow leaves it no stack. */
        DWORD WINAPI WriteDump( void* param ) {
            const Crash& crash = *static_cast<Crash*>( param );
            SYSTEMTIME t;
            GetLocalTime( &t );
            const std::wstring path = std::format( L"{}\\GD3D11-{:04}{:02}{:02}-{:02}{:02}{:02}.dmp", g_dir, t.wYear,
                t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond );

            bool written = false;
            HANDLE file = CreateFileW( path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
            if ( file != INVALID_HANDLE_VALUE ) {
                // No full memory: a 32-bit process near its 4 GiB limit would produce a multi-GB file.
                const DWORD type = MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithHandleData |
                    MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules;
                MINIDUMP_EXCEPTION_INFORMATION exception = { crash.ThreadId, crash.Info, FALSE };
                RegisterMemory memory( *crash.Info->ContextRecord );
                MINIDUMP_CALLBACK_INFORMATION callback = { &DumpCallback, &memory };
                written = g_writeDump( GetCurrentProcess(), GetCurrentProcessId(), file, static_cast<MINIDUMP_TYPE>( type ),
                    &exception, nullptr, &callback );
                CloseHandle( file );
                if ( !written ) {
                    DeleteFileW( path.c_str() );
                }
            }

            // After the dump: the crashed thread may hold the log's lock.
            const EXCEPTION_RECORD& record = *crash.Info->ExceptionRecord;
            Logging::Err( "CRASH: exception {:08x} at {} (thread {}); {}: {}", record.ExceptionCode,
                Where( record.ExceptionAddress ), crash.ThreadId, written ? "dump written to" : "no dump written to",
                std::filesystem::path( path ).string() );
            Logging::Flush();
            return 0;
        }

        LONG WINAPI OnCrash( EXCEPTION_POINTERS* info ) {
            if ( InterlockedExchange( &g_crashed, 1 ) == 0 ) {
                Crash crash = { info, GetCurrentThreadId() };
                if ( HANDLE thread = CreateThread( nullptr, 0, &WriteDump, &crash, 0, nullptr ) ) {
                    WaitForSingleObject( thread, 60000 );
                    CloseHandle( thread );
                }
            }
            LPTOP_LEVEL_EXCEPTION_FILTER next = g_next;
            return next ? next( info ) : EXCEPTION_CONTINUE_SEARCH;
        }

        /** Filters set after ours (the game's CRT sets one at startup) run after the dump instead of replacing it. */
        LPTOP_LEVEL_EXCEPTION_FILTER WINAPI HookedSetFilter( LPTOP_LEVEL_EXCEPTION_FILTER filter ) {
            return static_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(
                InterlockedExchangePointer( reinterpret_cast<void* volatile*>( &g_next ), reinterpret_cast<void*>( filter ) ) );
        }

    } // namespace

    void Install() {
        wchar_t system[MAX_PATH] = {};
        GetSystemDirectoryW( system, MAX_PATH );
        HMODULE dbghelp = LoadLibraryW( ( std::wstring( system ) + L"\\dbghelp.dll" ).c_str() );
        g_writeDump = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>( GetProcAddress( dbghelp, "MiniDumpWriteDump" ) ) : nullptr;
        if ( !g_writeDump ) {
            Logging::Wrn( "Crash dumps: MiniDumpWriteDump not found in dbghelp.dll" );
            return;
        }

        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW( nullptr, exe, MAX_PATH );
        g_dir = ( std::filesystem::path( exe ).parent_path() / L"GD3D11" / L"Crashes" ).wstring();
        std::error_code ec;
        std::filesystem::create_directories( g_dir, ec );

        g_next = SetUnhandledExceptionFilter( &OnCrash );
        DetourAttach( reinterpret_cast<PVOID*>( &g_origSetFilter ), reinterpret_cast<PVOID>( &HookedSetFilter ) );
        Logging::Inf( "Crash dumps go to {}\\GD3D11-*.dmp", std::filesystem::path( g_dir ).string() );
    }

}
