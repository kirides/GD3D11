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
    }

    template<class... Args>
    using Site = Detail::FormatSite<std::type_identity_t<Args>...>;

    [[nodiscard]] inline bool IsEnabled( Level level ) noexcept {
        return level >= Detail::MinLevel.load( std::memory_order_relaxed );
    }

    /** Messages below this are discarded before they are formatted. */
    void SetMinLevel( Level level ) noexcept;

    /** Asks the worker to write out what is queued; returns without waiting for it. */
    void Flush();

    /** Final synchronous drain. Safe to call from DllMain: it never waits on the worker thread. */
    void Shutdown();

    template<class... Args>
    void Dbg( Site<Args...> site, Args&&... args ) {
        if ( !IsEnabled( Level::Debug ) ) return;
        Detail::Write( Level::Debug, site.Where, site.Fmt.get(), std::make_format_args( args... ) );
    }

    template<class... Args>
    void Inf( Site<Args...> site, Args&&... args ) {
        if ( !IsEnabled( Level::Info ) ) return;
        Detail::Write( Level::Info, site.Where, site.Fmt.get(), std::make_format_args( args... ) );
    }

    template<class... Args>
    void Wrn( Site<Args...> site, Args&&... args ) {
        if ( !IsEnabled( Level::Warn ) ) return;
        Detail::Write( Level::Warn, site.Where, site.Fmt.get(), std::make_format_args( args... ) );
    }

    template<class... Args>
    void Err( Site<Args...> site, Args&&... args ) {
        if ( !IsEnabled( Level::Error ) ) return;
        Detail::Write( Level::Error, site.Where, site.Fmt.get(), std::make_format_args( args... ) );
    }

} // namespace Logging
