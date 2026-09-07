#pragma once

/** Creation-time audit trail for the D3D11 lighting/shadow resources. Every texture, view, buffer and
    sampler the lit passes depend on reports through here, so a missing one is named in Log.txt instead
    of silently shading black. */

#include "Logging.h"

#include <format>
#include <source_location>
#include <string_view>

namespace LightingLog {

    namespace Detail {
        inline void Fail( HRESULT hr, const void* resource, std::string_view what,
            const std::source_location& where ) {
            if ( !Logging::IsEnabled( Logging::Level::Error ) ) return;
            uint32_t code = static_cast<uint32_t>( hr );
            Logging::Detail::Write( Logging::Level::Error, where,
                "Lighting resource NOT created: {} (hr 0x{:08X}, pointer {})",
                std::make_format_args( what, code, resource ) );
        }
    }

    /** Logs one D3D11 creation call. `what` names the resource and its shape, e.g.
        std::format( "CSM DSV cascade {} ({}x{})", i, size, size ). Returns true when usable. */
    inline bool Check( HRESULT hr, const void* resource, std::string_view what,
        std::source_location where = std::source_location::current() ) {
        if ( FAILED( hr ) || !resource ) {
            Detail::Fail( hr, resource, what, where );
            return false;
        }
        if ( Logging::IsEnabled( Logging::Level::Info ) )
            Logging::Detail::Write( Logging::Level::Info, where, "Lighting resource created: {}",
                std::make_format_args( what ) );
        return true;
    }

    /** Same test, but silent on success - for the per-slot loops that would otherwise write thousands
        of lines. Report those with one Logging::Inf summary instead. */
    inline bool CheckQuiet( HRESULT hr, const void* resource, std::string_view what,
        std::source_location where = std::source_location::current() ) {
        if ( FAILED( hr ) || !resource ) {
            Detail::Fail( hr, resource, what, where );
            return false;
        }
        return true;
    }

    /** Per-frame call sites: reports the hole once, and re-arms if it ever comes back, so a broken
        frame path names itself without filling Log.txt. `reported` is a call-site static. */
    inline bool RequireOnce( const void* resource, bool& reported, std::string_view what,
        std::source_location where = std::source_location::current() ) {
        if ( resource ) {
            reported = false;
            return true;
        }
        if ( reported ) return false;
        reported = true;
        if ( Logging::IsEnabled( Logging::Level::Error ) )
            Logging::Detail::Write( Logging::Level::Error, where, "Lighting resource MISSING: {}",
                std::make_format_args( what ) );
        return false;
    }

    /** As CheckQuiet, but for a call made every frame - see RequireOnce. */
    inline bool CheckOnce( HRESULT hr, const void* resource, bool& reported, std::string_view what,
        std::source_location where = std::source_location::current() ) {
        if ( SUCCEEDED( hr ) && resource ) {
            reported = false;
            return true;
        }
        if ( reported ) return false;
        reported = true;
        Detail::Fail( hr, resource, what, where );
        return false;
    }

    /** Null check for something that should already exist (a pooled buffer, a view handed out by a
        slot table). Silent on success - only the hole is worth a line. */
    inline bool Require( const void* resource, std::string_view what,
        std::source_location where = std::source_location::current() ) {
        if ( resource ) return true;
        if ( Logging::IsEnabled( Logging::Level::Error ) )
            Logging::Detail::Write( Logging::Level::Error, where, "Lighting resource MISSING: {}",
                std::make_format_args( what ) );
        return false;
    }

    /** A resource the renderer can do without: says what is lost, not that something broke. */
    inline void Degraded( std::string_view what, std::string_view consequence,
        std::source_location where = std::source_location::current() ) {
        if ( !Logging::IsEnabled( Logging::Level::Warn ) ) return;
        Logging::Detail::Write( Logging::Level::Warn, where, "Lighting resource unavailable: {} - {}",
            std::make_format_args( what, consequence ) );
    }

} // namespace LightingLog
