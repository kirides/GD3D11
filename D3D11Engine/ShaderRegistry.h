#pragma once
#include "ShaderCategory.h"
#include "ShaderIDs.h"
#include <d3dcommon.h>      // D3D_SHADER_MACRO
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

/** Backend-neutral shader declaration table.

    This is the single source of truth for *which* shaders exist and *how* they are built
    (source file, entry point, input layout, static + settings-driven dynamic macros, content
    category) plus the compile-input hash used for dirty-flagged hot reload. It contains no
    D3D11/D3D12 objects: each graphics backend consumes this registry to compile the DXBC and
    create its own shader / PSO objects (D3D11ShaderManager today; a future D3D12 backend later).

    Input-layout declarations (EVERTEX_INPUT_LAYOUT) stay here because both APIs need identical
    element descriptions. Macros use D3D_SHADER_MACRO / DXBC, the shared shader currency. */

/** Struct holds initial shader data for load operation*/
struct ShaderInfo {
public:
    std::string name;				//Shader's name, used for debug logging
    std::string fileName;			//Shader's filename (without 'system\\GD3D11\\shaders\\')
    ShaderType type;				//Shader's type: Vertex, Pixel, Geometry, HullDomain, Compute
    std::string entryPoint;			//Shader's entry point function name
    size_t shaderIndex;				//Per-type enum index (e.g. VShaderID/PShaderID cast to size_t)
    EVERTEX_INPUT_LAYOUT layout;						//Shader's input layout
    std::vector<D3D_SHADER_MACRO> shaderMakros;
    ShaderCategory contentCategory;	//Content category for selective reloading
    size_t compiledHash = 0;			//Hash of last successful compilation (file timestamp + macros)

    // Optional: builds per-shader dynamic macros (renderer-settings-dependent) at compile/hash time.
    // Only macros this shader actually uses should be emitted — keeps hashing precise.
    using MacroBuilder = std::function<void( std::vector<D3D_SHADER_MACRO>& )>;
    MacroBuilder macroBuilder;

    /** Builder-style factory: infers name, type, and entrypoint from enum template parameter */
    template<auto ID>
    static ShaderInfo make( std::string fn ) {
        using EnumT = decltype(ID);
        ShaderInfo si{};

        si.shaderIndex = static_cast<size_t>(ID);
        si.name = magic_enum::enum_name( ID );
        si.fileName = std::move( fn );
        si.type = shader_type_for<EnumT>();
        si.entryPoint = entrypoint_for<EnumT>();

        return si;
    }

    /** Chainable setters for builder pattern */
    ShaderInfo& with_layout( EVERTEX_INPUT_LAYOUT l ) { layout = l; return *this; }
    ShaderInfo& with_macros( std::vector<D3D_SHADER_MACRO> m ) { shaderMakros = std::move(m); return *this; }
    ShaderInfo& with_macros( MacroBuilder b ) {
        if (macroBuilder) {
            macroBuilder = [previous = std::move(macroBuilder), current = std::move( b )](std::vector<D3D_SHADER_MACRO>& m)
            {
               previous(m);
                current(m);
            };
        } else {
            macroBuilder = std::move( b );
        }
        return *this;
    }
    ShaderInfo& with_category( ShaderCategory c ) { contentCategory = c; return *this; }
    ShaderInfo& with_entrypoint( std::string ep ) { entryPoint = std::move( ep ); return *this; }

private:
    ShaderInfo() : type( ShaderType::None ), shaderIndex( 0 ), layout( VERTEX_INPUT_LAYOUT_NONE ), contentCategory( ShaderCategory::Other ) {}

    template<typename EnumT>
    static constexpr ShaderType shader_type_for() {
        if constexpr ( std::is_same_v<EnumT, VShaderID> )  return ShaderType::Vertex;
        if constexpr ( std::is_same_v<EnumT, PShaderID> )  return ShaderType::Pixel;
        if constexpr ( std::is_same_v<EnumT, GShaderID> )  return ShaderType::Geometry;
        if constexpr ( std::is_same_v<EnumT, HDShaderID> ) return ShaderType::HullDomain;
        if constexpr ( std::is_same_v<EnumT, CShaderID> )  return ShaderType::Compute;
    }

    template<typename EnumT>
    static constexpr const char* entrypoint_for() {
        if constexpr ( std::is_same_v<EnumT, VShaderID> )  return "VSMain";
        if constexpr ( std::is_same_v<EnumT, PShaderID> )  return "PSMain";
        if constexpr ( std::is_same_v<EnumT, GShaderID> )  return "GSMain";
        if constexpr ( std::is_same_v<EnumT, HDShaderID> ) return "HSMain";
        if constexpr ( std::is_same_v<EnumT, CShaderID> )  return "CSMain";
    }
};

/** Backend-neutral registry that owns the shader declaration table and the compile-input hashing.
    Populate it once via Build(); each backend's shader manager then iterates Shaders() to compile. */
class ShaderRegistry {
public:
    /** Populates the declaration table (which shaders exist, their files, layouts, macros, categories).
        Settings-/feature-dependent macro builders are captured as closures and evaluated lazily at
        compile/hash time, so Build() itself is a pure declaration step. */
    void Build();

    /** The declaration table. Consumers iterate and compile these; compiledHash is updated in place. */
    std::vector<ShaderInfo>& Shaders() noexcept { return m_Shaders; }
    const std::vector<ShaderInfo>& Shaders() const noexcept { return m_Shaders; }

    /** Hash of a shader's compile inputs (source file timestamp + static + dynamic macros).
        Used by consumers to skip recompilation when nothing relevant changed. */
    static size_t ComputeShaderHash( const ShaderInfo& si );

private:
    std::vector<ShaderInfo> m_Shaders;   // Initial shader declaration list for loading
};
