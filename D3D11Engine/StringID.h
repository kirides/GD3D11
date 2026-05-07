#pragma once
#include <string_view>

class StringID {
public:

#ifdef DEBUG_D3D11
    // This allows: StringID myId = "u_Texture"; 
    // Computed at compile-time!
    consteval StringID( std::string_view str ) 
        : m_hash( SID( str ) ),
        m_strView( str )
    { }

    template <size_t N>
    consteval StringID( const char( &str )[N] ) 
        : m_hash( SID( std::string_view( str, N - 1 ) ) ),
        m_strView(str)
    {}

    static StringID make( std::string_view str ) {
        StringID id = {};
        id.m_hash = SID( str );
        id.m_str = str;
        id.m_strView = id.m_str;
        return id;
    }
#else
    // This allows: StringID myId = "u_Texture"; 
    // Computed at compile-time!
    consteval StringID(std::string_view str) : m_hash(SID(str)) {
    }
    
    template <size_t N>
    consteval StringID( const char( &str )[N] ) : m_hash( SID( std::string_view( str, N - 1 ) ) ) {}

    static StringID make( std::string_view str ) {
        StringID id;
        id.m_hash = SID( str );
        return id;
    }
#endif

    constexpr operator uint32_t() const { return m_hash; }
private:    
    StringID() = default;
    uint32_t m_hash;
#ifdef DEBUG_D3D11
    std::string_view m_strView;
    std::string m_str;
#endif
    
    static constexpr uint32_t SID(std::string_view str) {
        static_assert(sizeof(uint32_t) == sizeof(size_t), "StringID requires size_t to be 32 bits");

        uint32_t hash = 2166136261u;
        for (char c : str) {
            hash ^= static_cast<uint32_t>(static_cast<unsigned char>(c));
            hash *= 16777619u;
        }
        return hash;
    }
    
};

namespace std
{
    template<> struct hash<StringID>
    {
        constexpr std::size_t operator()(StringID const &p) const noexcept {
            return p;
        }
    };
}
