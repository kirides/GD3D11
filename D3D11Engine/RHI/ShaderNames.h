#pragma once
#include <cstddef>
#include <string>

/** Shader bytecode -> "File.hlsl:Entry", so backends can name pipelines in validation messages and breadcrumbs. */
namespace ShaderNames {
    void Register( const void* code, size_t size, const std::string& name );
    /** Empty when the bytecode was never registered. */
    std::string Lookup( const void* code, size_t size );
    /** The known names of both stages joined by " | " (vertex first); empty when neither is known. */
    std::string Describe( const void* vs, size_t vsSize, const void* ps, size_t psSize );
}
