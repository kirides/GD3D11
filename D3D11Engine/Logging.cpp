#include "pch.h"
#include "Logging.h"

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <print>
#include <thread>
#include <utility>
#include <vector>

namespace Logging {

    namespace {

        constexpr size_t MessageStackBytes = 1024;          // one record; longer ones take the heap fallback
        constexpr size_t FlushThresholdBytes = 64 * 1024;   // wake the worker early once a buffer holds this much
        constexpr size_t MaxBufferBytes = 1024 * 1024;      // hard cap per buffer; past it records are dropped and counted
        constexpr size_t RetainedBytes = 128 * 1024;        // capacity kept across flushes; a burst's excess is given back
        constexpr auto FlushInterval = std::chrono::seconds( 1 );

        struct State {
            std::mutex QueueMutex;
            std::condition_variable Wake;

            std::vector<char> Buffers[2];
            int Active = 0;
            uint32_t Dropped = 0;
            bool Stopping = false;
            bool FlushRequested = false;

            std::mutex FileMutex; // serialises the file write: worker vs. a Shutdown() drain
        };

        /** Deliberately leaked: the worker is detached and must never touch a destroyed global
            during CRT teardown. */
        State& Get() {
            static State* state = [] {
                auto* s = new State();
                s->Buffers[0].reserve( RetainedBytes );
                s->Buffers[1].reserve( RetainedBytes );
                return s;
            }();
            return *state;
        }

        const std::string& LogFilePath() {
            static std::string path = [] {
                if ( !LOGFILE.empty() ) return LOGFILE; // set by Log::Clear(); share the one Log.txt
                std::string p;
                p.resize( MAX_PATH );
                p.resize( GetModuleFileNameA( nullptr, p.data(), MAX_PATH ) );
                p.erase( p.find_last_of( '\\' ) + 1 );
                p += "Log.txt";
                return p;
            }();
            return path;
        }

        constexpr char LevelTag( Level level ) {
            switch ( level ) {
            case Level::Debug: return 'D';
            case Level::Warn:  return 'W';
            case Level::Error: return 'E';
            default:           return 'I';
            }
        }

        constexpr std::string_view FileName( const char* path ) {
            std::string_view v( path );
            if ( auto slash = v.find_last_of( "\\/" ); slash != std::string_view::npos ) v.remove_prefix( slash + 1 );
            return v;
        }

        /** Output iterator over a fixed buffer that drops - and remembers - anything past the end. */
        struct BoundedOutput {
            using difference_type = std::ptrdiff_t;

            char* Cursor = nullptr;
            char* End = nullptr;
            bool Overflowed = false;

            BoundedOutput& operator*() { return *this; }
            BoundedOutput& operator++() { return *this; }
            // Returns a reference on purpose: a copy would make "*it++ = c" write through a temporary.
            BoundedOutput& operator++( int ) { return *this; }
            BoundedOutput& operator=( char c ) {
                if ( Cursor < End ) *Cursor++ = c; else Overflowed = true;
                return *this;
            }
        };

        template<class Out>
        Out EmitRecord( Out out, Level level, const std::source_location& where,
            std::string_view fmt, std::format_args args ) {
            SYSTEMTIME t;
            GetLocalTime( &t );
            out = std::format_to( out, "[{:02}:{:02}:{:02}.{:03}] {} {:<5} | ",
                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, LevelTag( level ), GetCurrentThreadId() );
            out = std::vformat_to( out, fmt, args );
            if ( level >= Level::Warn ) {
                out = std::format_to( out, "  ({}:{})", FileName( where.file_name() ), where.line() );
            }
            *out++ = '\n';
            return out;
        }

        void Enqueue( State& state, const char* data, size_t size ) {
            bool wake = false;
            {
                std::lock_guard lock( state.QueueMutex );
                auto& buffer = state.Buffers[state.Active];
                if ( buffer.size() + size > MaxBufferBytes ) {
                    ++state.Dropped;
                    return;
                }
                buffer.insert( buffer.end(), data, data + size );
                wake = buffer.size() >= FlushThresholdBytes;
            }
            if ( wake ) state.Wake.notify_one();
        }

        /** Hands the active buffer over to the flusher and starts producers on the other one.
            Returns nullptr when there is nothing to write. Caller holds QueueMutex. */
        std::vector<char>* RetireActive( State& state, uint32_t& outDropped ) {
            if ( state.Buffers[state.Active].empty() && state.Dropped == 0 ) return nullptr;
            std::vector<char>* retired = &state.Buffers[state.Active];
            state.Active ^= 1;
            outDropped = std::exchange( state.Dropped, 0u );
            return retired;
        }

        /** Caller holds FileMutex, which is what makes the retired buffer exclusively ours: producers
            only ever touch Buffers[Active], and the next swap needs FileMutex too. */
        void WriteRetired( State& state, std::vector<char>* retired, uint32_t dropped ) {
            if ( FILE* f = fopen( LogFilePath().c_str(), "a" ) ) {
                if ( dropped ) std::print( f, "[logging] dropped {} message(s): in-memory queue full\n", dropped );
                // The bulk is already formatted; fwrite it instead of letting std::print copy a megabyte.
                if ( !retired->empty() ) fwrite( retired->data(), 1, retired->size(), f );
                fclose( f );
            }

            retired->clear();
            if ( retired->capacity() > RetainedBytes ) {
                retired->shrink_to_fit();
                retired->reserve( RetainedBytes );
            }
        }

        void FlushOnce( State& state ) {
            std::lock_guard fileLock( state.FileMutex );

            std::vector<char>* retired = nullptr;
            uint32_t dropped = 0;
            {
                std::lock_guard lock( state.QueueMutex );
                retired = RetireActive( state, dropped );
            }
            if ( retired ) WriteRetired( state, retired, dropped );
        }

        void WorkerMain( State& state ) {
            for ( ;; ) {
                {
                    std::unique_lock lock( state.QueueMutex );
                    state.Wake.wait_for( lock, FlushInterval, [&state] {
                        return state.Stopping || state.FlushRequested
                            || state.Buffers[state.Active].size() >= FlushThresholdBytes;
                    } );
                    state.FlushRequested = false;
                    if ( state.Stopping ) return;
                }
                FlushOnce( state );
            }
        }

        /** Started from the first record. std::thread's constructor doesn't wait on the new thread,
            so this stays safe under the loader lock - the worker just idles until DllMain returns. */
        void EnsureWorker( State& state ) {
            static std::once_flag once;
            std::call_once( once, [&state] { std::thread( WorkerMain, std::ref( state ) ).detach(); } );
        }

    } // namespace

    namespace Detail {
#if PUBLIC_RELEASE
        std::atomic<Level> MinLevel = Level::Info;
#else
        std::atomic<Level> MinLevel = Level::Debug;
#endif

        void Write( Level level, const std::source_location& where, std::string_view fmt, std::format_args args ) {
            char stack[MessageStackBytes];
            BoundedOutput out{ stack, stack + sizeof( stack ) };
            out = EmitRecord( out, level, where, fmt, args );

            State& state = Get();
            EnsureWorker( state );

            if ( !out.Overflowed ) {
                Enqueue( state, stack, static_cast<size_t>( out.Cursor - stack ) );
                return;
            }

            // Rare: the record outgrew the stack buffer, so build it once on the heap.
            std::string big;
            big.reserve( MessageStackBytes * 2 );
            EmitRecord( std::back_inserter( big ), level, where, fmt, args );
            Enqueue( state, big.data(), big.size() );
        }
    }

    void SetMinLevel( Level level ) noexcept {
        Detail::MinLevel.store( level, std::memory_order_relaxed );
    }

    void Flush() {
        State& state = Get();
        {
            std::lock_guard lock( state.QueueMutex );
            state.FlushRequested = true;
        }
        state.Wake.notify_one();
    }

    void Shutdown() {
        State& state = Get();

        // Drains here rather than joining the worker: this runs from DllMain, where a join would deadlock
        // on the loader lock. Every lock is a try_lock for the same reason - the worker may already have
        // been killed holding one. Losing the last second of records beats hanging the game on exit.
        std::unique_lock fileLock( state.FileMutex, std::try_to_lock );
        std::unique_lock lock( state.QueueMutex, std::try_to_lock );
        if ( !lock.owns_lock() ) return;

        state.Stopping = true;
        std::vector<char>* retired = nullptr;
        uint32_t dropped = 0;
        if ( fileLock.owns_lock() ) retired = RetireActive( state, dropped );
        lock.unlock();

        state.Wake.notify_all();
        if ( retired ) WriteRetired( state, retired, dropped );
    }

} // namespace Logging
