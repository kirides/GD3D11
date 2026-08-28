#pragma once

struct WideNarrowChars {
    const wchar_t* wide;
    const char* narrow;
    size_t len_wide;
    size_t len_narrow;

    template<std::size_t NWide, std::size_t NNarrow>
    constexpr WideNarrowChars( const wchar_t(&wide)[NWide], const char(&narrow)[NNarrow]) :
        wide( wide ), narrow( narrow ),
        len_wide(NWide - 1), len_narrow(NNarrow - 1) {
    }

    constexpr WideNarrowChars( const WideNarrowChars& ) noexcept = default;
    constexpr WideNarrowChars& operator=( const WideNarrowChars& ) noexcept = default;

    [[nodiscard]] constexpr std::wstring_view sv_wide() const noexcept {
        return { wide, len_wide };
    }

    [[nodiscard]] constexpr std::string_view sv_narrow() const noexcept {
        return { narrow, len_narrow };
    }
};
