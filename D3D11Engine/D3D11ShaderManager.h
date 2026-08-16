#pragma once
#include "ShaderRegistry.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11HDShader.h"
#include "D3D11GShader.h"
#include "D3D11CShader.h"
#include <cstdint>
#include <string>
#include <utility>

class D3D11ShaderManager {
public:
    D3D11ShaderManager();
    ~D3D11ShaderManager();

    // Compiles the shader from file and outputs error messages if needed. Cached on disk in
    // system\GD3D11\shadercache\D3D11\; delete that directory to force a full recompile.
    static HRESULT CompileShaderFromFile( const CHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros );
    static HRESULT CompileShaderFromFile( const WCHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros );

    static void LogAndResetCacheStats( const char* context );

    using ShaderDeps = std::vector<std::pair<std::string, uint64_t>>;

    /** Creates list with ShaderInfos */
    XRESULT Init();

    /** Loads/Compiles Shaderes from list */
    XRESULT LoadShaders( ShaderCategory categories = ShaderCategory::All );

    /** Deletes all shaders and loads them again */
    XRESULT ReloadShaders( ShaderCategory categories = ShaderCategory::All );

    /** Called on frame start */
    XRESULT OnFrameStart();

    /** Deletes all shaders */
    XRESULT DeleteShaders();
    void UpdateShaderInfo( ShaderInfo& shader );

    /** Return a specific shader */
    std::shared_ptr<D3D11VShader>& GetVShader( VShaderID id ) { return VShaders[static_cast<size_t>(id)]; }
    std::shared_ptr<D3D11PShader>& GetPShader( PShaderID id ) { return PShaders[static_cast<size_t>(id)]; }
    std::shared_ptr<D3D11HDShader>& GetHDShader( HDShaderID id ) { return HDShaders[static_cast<size_t>(id)]; }
    std::shared_ptr<D3D11GShader>& GetGShader( GShaderID id ) { return GShaders[static_cast<size_t>(id)]; }
    std::shared_ptr<D3D11CShader>& GetCShader( CShaderID id ) { return CShaders[static_cast<size_t>(id)]; }

private:
    // Uncached compile; outDeps, when non-null, receives every #include resolved.
    static HRESULT CompileShaderFromFileRaw( const WCHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel,
        ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros, ShaderDeps* outDeps );

    XRESULT CompileShader( ShaderInfo& si );

    void UpdateVShader( size_t index, D3D11VShader* shader ) { std::unique_lock<std::mutex> lock( _VShaderMutex ); VShaders[index].reset( shader ); }
    void UpdatePShader( size_t index, D3D11PShader* shader ) { std::unique_lock<std::mutex> lock( _PShaderMutex );  PShaders[index].reset( shader ); }
    void UpdateHDShader( size_t index, D3D11HDShader* shader ) { std::unique_lock<std::mutex> lock( _HDShaderMutex );  HDShaders[index].reset( shader ); }
    void UpdateGShader( size_t index, D3D11GShader* shader ) { std::unique_lock<std::mutex> lock( _GShaderMutex );  GShaders[index].reset( shader ); }
    void UpdateCShader( size_t index, D3D11CShader* shader ) { std::unique_lock<std::mutex> lock( _CShaderMutex );  CShaders[index].reset( shader ); }

    bool IsVShaderKnown( size_t index ) { std::unique_lock<std::mutex> lock( _VShaderMutex ); return VShaders[index] != nullptr; }
    bool IsPShaderKnown( size_t index ) { std::unique_lock<std::mutex> lock( _PShaderMutex ); return PShaders[index] != nullptr; }
    bool IsHDShaderKnown( size_t index ) { std::unique_lock<std::mutex> lock( _HDShaderMutex ); return HDShaders[index] != nullptr; }
    bool IsGShaderKnown( size_t index ) { std::unique_lock<std::mutex> lock( _GShaderMutex ); return GShaders[index] != nullptr; }
    bool IsCShaderKnown( size_t index ) { std::unique_lock<std::mutex> lock( _CShaderMutex ); return CShaders[index] != nullptr; }

private:
    ShaderRegistry m_Registry;									//Backend-neutral shader declaration table + hashing
    std::vector<std::shared_ptr<D3D11VShader>> VShaders;
    std::vector<std::shared_ptr<D3D11PShader>> PShaders;
    std::vector<std::shared_ptr<D3D11HDShader>> HDShaders;
    std::vector<std::shared_ptr<D3D11GShader>> GShaders;
    std::vector<std::shared_ptr<D3D11CShader>> CShaders;

    std::mutex _VShaderMutex;
    std::mutex _PShaderMutex;
    std::mutex _HDShaderMutex;
    std::mutex _GShaderMutex;
    std::mutex _CShaderMutex;

    /** Shader categories to reload next frame (OR-ed together from multiple calls) */
    ShaderCategory ShaderCategoriesToReloadNextFrame;
};
