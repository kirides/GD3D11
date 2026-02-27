#pragma once
#include <cstdint>

/** Enum flags for shader categories to enable selective reloading */
enum class ShaderType : byte
{
    None = 0,
    Vertex = 1,
    Pixel = 2,
    Geometry = 3,
    HullDomain = 4,
    Compute = 5,
};
enum class ShaderCategory : uint32_t {
    None = 0,

    // Shader type categories
    Vertex = 1 << 0,
    Pixel = 1 << 1,
    Geometry = 1 << 2,
    HullDomain = 1 << 3,
    Compute = 1 << 4,
    AllTypes = Vertex | Pixel | Geometry | HullDomain | Compute,

    // Shader content categories
    LightsAndShadows = 1 << 8,
    Water = 1 << 9,
    Other = 1 << 10,
    AllContent = LightsAndShadows | Water | Other,

    // Combined: All types and all content
    All = AllTypes | AllContent
};

inline ShaderCategory operator|( ShaderCategory a, ShaderCategory b ) {
    return static_cast<ShaderCategory>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline ShaderCategory operator&( ShaderCategory a, ShaderCategory b ) {
    return static_cast<ShaderCategory>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline ShaderCategory& operator|=( ShaderCategory& a, ShaderCategory b ) {
    a = a | b;
    return a;
}

inline bool HasCategory( ShaderCategory flags, ShaderCategory category ) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(category)) != 0;
}
