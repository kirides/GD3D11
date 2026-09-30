#pragma once

/** Asynchronous logging. Callers only format into a stack buffer and memcpy the bytes into a
    double-buffered in-memory queue; a background worker swaps the queue and writes it to Log.txt
    once a second, or earlier once enough bytes piled up. */

#include <atomic>
#include <concepts>
#include <cstdint>
#include <format>
#include <source_location>
#include <string_view>
#include <type_traits>

namespace Logging {

    enum class Level : uint8_t { Debug = 0, Info, Warn, Error, Off };

    namespace Detail {
        extern std::atomic<Level> MinLevel;

        /** Carries the call site alongside the format string, so I/D/W/E can stay variadic
            function templates instead of macros. */
        template<class... Args>
        struct FormatSite {
            std::format_string<Args...> Fmt;
            std::source_location Where;

            template<class T> requires std::convertible_to<const T&, std::string_view>
            consteval FormatSite( const T& fmt, std::source_location where = std::source_location::current() )
                : Fmt( fmt ), Where( where ) {}
        };

        /** Type-erased sink. Defined in Logging.cpp so <format>'s codegen isn't duplicated per call site. */
        void Write( Level level, const std::source_location& where, std::string_view fmt, std::format_args args );

        /** Write, then show the message in a blocking MessageBox. */
        void WriteBox( Level level, const std::source_location& where, std::string_view fmt, std::format_args args );

        /** char[N] arguments (string literals, LE's #x) format as string_view, so each literal length doesn't
            instantiate its own formatter checks. Output is identical: both are null-terminated strings. */
        template<class T> struct FmtArg { using type = T; };
        template<std::size_t N> struct FmtArg<const char (&)[N]> { using type = std::string_view; };
        template<std::size_t N> struct FmtArg<char (&)[N]> { using type = std::string_view; };

        template<class T>
        decltype(auto) AsFmtArg( T& v ) {
            if constexpr ( std::is_array_v<T> ) return std::string_view( v );
            else return ( v );
        }

        template<class... T>
        void WriteArgs( Level level, const std::source_location& where, std::string_view fmt, T&&... args ) {
            Write( level, where, fmt, std::make_format_args( args... ) );
        }

        template<class... T>
        void WriteBoxArgs( Level level, const std::source_location& where, std::string_view fmt, T&&... args ) {
            WriteBox( level, where, fmt, std::make_format_args( args... ) );
        }
    }

    template<class... Args>
    using Site = Detail::FormatSite<typename Detail::FmtArg<Args>::type...>;

    [[nodiscard]] inline bool IsEnabled( Level level ) noexcept {
        return level >= Detail::MinLevel.load( std::memory_order_relaxed );
    }

    /** Messages below this are discarded before they are formatted. */
    void SetMinLevel( Level level ) noexcept;

    /** Truncates Log.txt. Call once at startup, before the first record. */
    void ClearFile();

    /** Asks the worker to write out what is queued; returns without waiting for it. */
    void Flush();

    /** Final synchronous drain. Safe to call from DllMain: it never waits on the worker thread. */
    void Shutdown();

    template<class... Args>
    void Dbg( Site<Args...> site, Args&&... args ) {
        if ( !IsEnabled( Level::Debug ) ) return;
        Detail::WriteArgs( Level::Debug, site.Where, site.Fmt.get(), Detail::AsFmtArg( args )... );
    }

    template<class... Args>
    void Inf( Site<Args...> site, Args&&... args ) {
        if ( !IsEnabled( Level::Info ) ) return;
        Detail::WriteArgs( Level::Info, site.Where, site.Fmt.get(), Detail::AsFmtArg( args )... );
    }

    template<class... Args>
    void Wrn( Site<Args...> site, Args&&... args ) {
        if ( !IsEnabled( Level::Warn ) ) return;
        Detail::WriteArgs( Level::Warn, site.Where, site.Fmt.get(), Detail::AsFmtArg( args )... );
    }

    /** Warn attributed to an explicit call site, for helpers that forward their caller's location. */
    template<class... Args>
    void WrnAt( const std::source_location& where, std::format_string<typename Detail::FmtArg<Args>::type...> fmt, Args&&... args ) {
        if ( !IsEnabled( Level::Warn ) ) return;
        Detail::WriteArgs( Level::Warn, where, fmt.get(), Detail::AsFmtArg( args )... );
    }

    template<class... Args>
    void Err( Site<Args...> site, Args&&... args ) {
        if ( !IsEnabled( Level::Error ) ) return;
        Detail::WriteArgs( Level::Error, site.Where, site.Fmt.get(), Detail::AsFmtArg( args )... );
    }

    /** Like Inf/Wrn/Err, plus a blocking MessageBox. Never filtered by the min level. */
    template<class... Args>
    void InfBox( Site<Args...> site, Args&&... args ) {
        Detail::WriteBoxArgs( Level::Info, site.Where, site.Fmt.get(), Detail::AsFmtArg( args )... );
    }

    template<class... Args>
    void WrnBox( Site<Args...> site, Args&&... args ) {
        Detail::WriteBoxArgs( Level::Warn, site.Where, site.Fmt.get(), Detail::AsFmtArg( args )... );
    }

    template<class... Args>
    void ErrBox( Site<Args...> site, Args&&... args ) {
        Detail::WriteBoxArgs( Level::Error, site.Where, site.Fmt.get(), Detail::AsFmtArg( args )... );
    }

} // namespace Logging
