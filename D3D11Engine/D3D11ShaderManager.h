#pragma once
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
    std::string type;				//Shader's type: 'v' vertexShader, 'p' pixelShader
    int layout;						//Shader's input layout
    std::vector<int> cBufferSizes;	//Vector with size for each constant buffer to be created for this shader
    std::vector<D3D_SHADER_MACRO> shaderMakros;

    //Constructor
    ShaderInfo( std::string n, std::string fn, std::string t, int l, std::vector<D3D_SHADER_MACRO>& makros = std::vector<D3D_SHADER_MACRO>() ) {
        name = n;
        fileName = fn;
        type = t;
        layout = l;
        cBufferSizes = std::vector<int>();

        shaderMakros = makros;
    }

    //Constructor
    ShaderInfo( std::string n, std::string fn, std::string t, std::vector<D3D_SHADER_MACRO>& makros = std::vector<D3D_SHADER_MACRO>() ) {
        name = n;
        fileName = fn;
        type = t;
        layout = 0;
        cBufferSizes = std::vector<int>();

        shaderMakros = makros;
    }
};

class D3D11ShaderManager {
public:
    D3D11ShaderManager();
    ~D3D11ShaderManager();

    /** Compiles the shader from file and outputs error messages if needed */
    static HRESULT CompileShaderFromFile( const CHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros );

    /** Creates list with ShaderInfos */
    XRESULT Init();

    /** Loads/Compiles Shaderes from list */
    XRESULT LoadShaders();

    /** Deletes all shaders and loads them again */
    XRESULT ReloadShaders();

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

    /** Whether we need to reload the shaders next frame or not */
    bool ReloadShadersNextFrame;
};
