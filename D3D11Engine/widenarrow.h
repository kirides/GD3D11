#pragma once

struct WideNarrowChars {
    const wchar_t* wide;
    const char* narrow;

    constexpr WideNarrowChars( const wchar_t* wide, const char* narrow )
        : wide( wide ), narrow( narrow ) {
    }

    constexpr WideNarrowChars( const WideNarrowChars& chars )
        : wide( chars.wide ), narrow( chars.narrow ) {
    }
};
