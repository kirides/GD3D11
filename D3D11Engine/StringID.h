#pragma once
#include <string_view>

class StringID {
public:
    // This allows: StringID myId = "u_Texture"; 
    // Computed at compile-time!
    constexpr StringID(std::string_view str) : m_hash(SID(str)) {
    }
    
    constexpr StringID(const char* str) : m_hash(SID(str)) {
    }

    constexpr operator uint32_t() const { return m_hash; }

private:
    uint32_t m_hash;
    
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
