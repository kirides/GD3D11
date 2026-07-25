#pragma once
#include <d3dcommon.h>   // ID3DBlob, D3D_SHADER_MACRO
#include <string>
#include <vector>

// Loads and compiles the D3D12 backend's HLSL shaders from disk (Shaders\D3D12\*.hlsl, shipped to
// system\GD3D11\shaders\D3D12\). Sources are read via zFILE_VDFS (with a physical filesystem fallback
// for dev/hot-reload) and compiled to DXIL at runtime with the DXC COM API (SM6.6). This keeps the
// D3D11-style "edit .hlsl, reload" workflow instead of baking shader blobs into the engine binary.
class D3D12ShaderBackend {
public:
    // Compile one entrypoint from Shaders\D3D12\<fileName> to a DXIL ID3DBlob.
    // Returns false (and logs) if the file cannot be found or fails to compile.
    bool CompileFromFile( const std::string& fileName, const char* entryPoint,
                          const char* target, ID3DBlob** ppCode,
                          const D3D_SHADER_MACRO* defines = nullptr );

    // Reads raw HLSL text for a shader file: zFILE_VDFS first, physical fallback second.
    static bool LoadShaderSource( const std::string& fileName, std::string& outSource );

    // Appends the backend-wide configuration macros (currently the normal-map convention pair,
    // mirroring D3D11's ShaderRegistry normalmappingConfigurationBuilder) that CompileFromFile adds to
    // every compile. The Definition pointers are static literals, so the vector may outlive this call.
    // NOT null-terminated — the caller adds the {nullptr,nullptr} sentinel.
    static void AppendGlobalMacros( std::vector<D3D_SHADER_MACRO>& list );
};
