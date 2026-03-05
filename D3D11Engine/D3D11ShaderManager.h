#pragma once
#include "ShaderCategory.h"
#include <unordered_map>
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11HDShader.h"
#include "D3D11GShader.h"
#include "D3D11CShader.h"

/** Struct holds initial shader data for load operation*/
struct ShaderInfo {
public:
    std::string name;				//Shader's name, used as key in map
    std::string fileName;			//Shader's filename (without 'system\\GD3D11\\shaders\\')
    ShaderType type;				//Shader's type: 'v' vertexShader, 'p' pixelShader
    std::string entryPoint;				//Shader's type: 'v' vertexShader, 'p' pixelShader
    int layout;						//Shader's input layout
    std::vector<int> cBufferSizes;	//Vector with size for each constant buffer to be created for this shader
    std::vector<D3D_SHADER_MACRO> shaderMakros;
    ShaderCategory contentCategory;	//Content category for selective reloading

    static std::string EntrypointForType(const std::string& t)
    {
        if (t == "v") return "VSMain";
        if (t == "p") return "PSMain";
        if (t == "c") return "CSMain";
        if (t == "hd") return "HSMain";
        if (t == "g") return "GSMain";
        return "main";
    }
    
    static ShaderType ShaderTypeForType(const std::string& t) {
        if (t == "v") return ShaderType::Vertex;
        if (t == "p") return ShaderType::Pixel;
        if (t == "g") return ShaderType::Geometry;
        if (t == "hd") return ShaderType::HullDomain;
        if (t == "c") return ShaderType::Compute;
        return ShaderType::None;
    }
    
    //Constructor
    ShaderInfo( std::string n, std::string fn, const std::string& t, int l, std::vector<D3D_SHADER_MACRO>& makros = std::vector<D3D_SHADER_MACRO>(), ShaderCategory category = ShaderCategory::Other ) {
        name = std::move(n);
        fileName = std::move(fn);
        type = ShaderTypeForType(t);
        layout = l;
        cBufferSizes = std::vector<int>();
        shaderMakros = makros;
        contentCategory = category;
        entryPoint = EntrypointForType(t);
    }

    //Constructor
    ShaderInfo( std::string n, std::string fn, const std::string& t, std::vector<D3D_SHADER_MACRO>& makros = std::vector<D3D_SHADER_MACRO>(), ShaderCategory category = ShaderCategory::Other )
        :ShaderInfo(std::move(n), std::move(fn), t, makros, EntrypointForType(t), category)
    {
    }
    
    ShaderInfo( std::string n, std::string fn, const std::string& t, std::vector<D3D_SHADER_MACRO>& makros, std::string entryPoint, ShaderCategory category = ShaderCategory::Other ) 
        :
        name(std::move(n)),
        fileName(std::move(fn)),
        type(ShaderTypeForType(t)),
        layout(0),
        cBufferSizes(std::vector<int>()),
        shaderMakros(makros),
        contentCategory(category),
        entryPoint(std::move(entryPoint))
    { }
};

class D3D11ShaderManager {
public:
    D3D11ShaderManager();
    ~D3D11ShaderManager();

    /** Compiles the shader from file and outputs error messages if needed */
    static HRESULT CompileShaderFromFile( const CHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros );
    static HRESULT CompileShaderFromFile( const WCHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros );

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
    ShaderInfo GetShaderInfo( const std::string& shader, bool& ok );
    void UpdateShaderInfo( ShaderInfo& shader );

    /** Return a specific shader */
    std::shared_ptr<D3D11VShader> GetVShader( const std::string& shader );
    std::shared_ptr<D3D11PShader> GetPShader( const std::string& shader );
    std::shared_ptr<D3D11HDShader> GetHDShader( const std::string& shader );
    std::shared_ptr<D3D11GShader> GetGShader( const std::string& shader );
    std::shared_ptr<D3D11CShader> GetCShader( const std::string& shader );

private:
    XRESULT CompileShader( const ShaderInfo& si );

    void UpdateVShader( const std::string& name, D3D11VShader* shader ) { std::unique_lock<std::mutex> lock( _VShaderMutex ); VShaders[name].reset( shader ); }
    void UpdatePShader( const std::string& name, D3D11PShader* shader ) { std::unique_lock<std::mutex> lock( _PShaderMutex );  PShaders[name].reset( shader ); }
    void UpdateHDShader( const std::string& name, D3D11HDShader* shader ) { std::unique_lock<std::mutex> lock( _HDShaderMutex );  HDShaders[name].reset( shader ); }
    void UpdateGShader( const std::string& name, D3D11GShader* shader ) { std::unique_lock<std::mutex> lock( _GShaderMutex );  GShaders[name].reset( shader ); }
    void UpdateCShader( const std::string& name, D3D11CShader* shader ) { std::unique_lock<std::mutex> lock( _CShaderMutex );  CShaders[name].reset( shader ); }

    bool IsVShaderKnown( const std::string& name ) { std::unique_lock<std::mutex> lock( _VShaderMutex ); return VShaders.count( name ) > 0; }
    bool IsPShaderKnown( const std::string& name ) { std::unique_lock<std::mutex> lock( _PShaderMutex ); return PShaders.count( name ) > 0; }
    bool IsHDShaderKnown( const std::string& name ) { std::unique_lock<std::mutex> lock( _HDShaderMutex ); return HDShaders.count( name ) > 0; }
    bool IsGShaderKnown( const std::string& name ) { std::unique_lock<std::mutex> lock( _GShaderMutex ); return GShaders.count( name ) > 0; }
    bool IsCShaderKnown( const std::string& name ) { std::unique_lock<std::mutex> lock( _CShaderMutex ); return CShaders.count( name ) > 0; }

private:
    std::vector<ShaderInfo> Shaders;							//Initial shader list for loading
    std::unordered_map<std::string, std::shared_ptr<D3D11VShader>> VShaders;
    std::unordered_map<std::string, std::shared_ptr<D3D11PShader>> PShaders;
    std::unordered_map<std::string, std::shared_ptr<D3D11HDShader>> HDShaders;
    std::unordered_map<std::string, std::shared_ptr<D3D11GShader>> GShaders;
    std::unordered_map<std::string, std::shared_ptr<D3D11CShader>> CShaders;

    std::mutex _VShaderMutex;
    std::mutex _PShaderMutex;
    std::mutex _HDShaderMutex;
    std::mutex _GShaderMutex;
    std::mutex _CShaderMutex;

    /** Shader categories to reload next frame (OR-ed together from multiple calls) */
    ShaderCategory ShaderCategoriesToReloadNextFrame;
};
