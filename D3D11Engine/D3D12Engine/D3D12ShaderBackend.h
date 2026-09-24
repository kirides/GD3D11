#pragma once
#include <d3dcommon.h>   // ID3DBlob, D3D_SHADER_MACRO
#include <string>
#include <vector>

struct ID3D12ShaderReflection;   // <d3d12shader.h>; only the reflection entry point below needs it

// Which intermediate language DXC emits: DXIL for D3D12, SPIR-V (Vulkan 1.3) for Vulkan.
enum class ShaderIL { DXIL, SPIRV };

// Loads and compiles the D3D12 backend's HLSL shaders from disk (Shaders\D3D12\*.hlsl, shipped to
// system\GD3D11\shaders\D3D12\). Sources are read via zFILE_VDFS (with a physical filesystem fallback
// for dev/hot-reload) and compiled at runtime with the DXC COM API (SM6.6), to DXIL or SPIR-V. This keeps
// the D3D11-style "edit .hlsl, reload" workflow instead of baking shader blobs into the engine binary.
class D3D12ShaderBackend {
public:
    // Compile one entrypoint from Shaders\D3D12\<fileName> to a DXIL (or SPIR-V) ID3DBlob.
    // Returns false (and logs) if the file cannot be found or fails to compile.
    //
    // Results are cached per IL in system\GD3D11\cache\ and re-used on later runs. The entry hashes the
    // source AND every file it transitively #include'd, so editing a shared header invalidates exactly the
    // shaders that pull it in.
    bool CompileFromFile( const std::string& fileName, const char* entryPoint,
                          const char* target, ID3DBlob** ppCode,
                          const D3D_SHADER_MACRO* defines = nullptr,
                          ShaderIL il = ShaderIL::DXIL );

    // True when dxcompiler.dll loads and was built with SPIR-V codegen (compiles a trivial shader once).
    static bool IsSpirvCodegenAvailable( std::string* outReason = nullptr );

    // Logs one "N reused from disk, M compiled" line for the compiles since the last call, then zeroes it.
    static void LogAndResetCacheStats( const char* context );

    // Reads raw HLSL text for a shader file: zFILE_VDFS first, physical fallback second.
    static bool LoadShaderSource( const std::string& fileName, std::string& outSource );

    // Creates a DXC reflection interface over a DXIL container produced by CompileFromFile, so the
    // shader's actual resource bindings can be checked against the C++ root signature
    // (D3D12RootLayout::ValidateShaders). Returns false — quietly, this is a diagnostic path — if
    // dxcompiler.dll is unavailable or the container carries no reflection part.
    static bool Reflect( ID3DBlob* code, struct ID3D12ShaderReflection** ppReflection );

    // Appends the backend-wide configuration macros (currently the normal-map convention pair,
    // mirroring D3D11's ShaderRegistry normalmappingConfigurationBuilder) that CompileFromFile adds to
    // every compile. The Definition pointers are static literals, so the vector may outlive this call.
    // NOT null-terminated — the caller adds the {nullptr,nullptr} sentinel.
    static void AppendGlobalMacros( std::vector<D3D_SHADER_MACRO>& list );
};
