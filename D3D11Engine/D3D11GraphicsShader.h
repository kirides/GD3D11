#pragma once
#include "GraphicsShader.h"
#include <d3d11.h>
#include <d3d11shader.h>
#include <memory>

#include "ConstantBufferPool.h"
#include "Types.h"
#include "StringID.h"

typedef size_t ConstantBufferSize;
typedef size_t ConstantBufferSlot;
constexpr size_t MAX_SHADER_CB = 6;
constexpr size_t INVALID_SHADER_CB_SLOT = static_cast<size_t>(static_cast<int32_t>(-1));

class D3D11GraphicsShader 
    : public GraphicsShader
{
public:
    D3D11GraphicsShader();
    ~D3D11GraphicsShader() override;
    /** Returns the input index for the given semantic name */
    int32_t GetInputIndex( StringID name ) override;
    
    const std::array<ConstantBufferSize, MAX_SHADER_CB>& GetConstantBufferSizes() const { return ConstantBuffers; }

    virtual void BindResource(StringID name, ID3D11ShaderResourceView* srv) = 0;
    virtual void BindSampler(StringID name, ID3D11SamplerState* sampler) = 0;
    // `where` defaults to the caller so a pool wrap is logged at the real call site.
    virtual void UpdateBuffer( StringID name, const void* data, size_t size,
        std::source_location where = std::source_location::current() ) = 0;
    virtual void UpdateBuffer( UINT slot, const void* data, size_t size,
        std::source_location where = std::source_location::current() ) = 0;
    
    virtual XRESULT Apply() = 0;
protected:
    // Reflection lookups, defined in the .cpp so includers don't instantiate the maps.
    struct Reflection;
    std::unique_ptr<Reflection> m_Reflection;
    std::array<ConstantBufferSize, MAX_SHADER_CB> ConstantBuffers;
    std::array<byte, MAX_SHADER_CB> ConstantBufferIndexBySlot;

    virtual HRESULT ReflectShaderResources( ID3DBlob* shaderBlob );
    virtual void OnReflectShader( ID3DBlob* blob, ID3D11ShaderReflection* pReflection,  const D3D11_SHADER_DESC& shaderDesc );
    virtual void OnReflectShaderResource( ID3D11ShaderReflection* pReflection, const D3D11_SHADER_DESC& shaderDesc, const D3D11_SHADER_INPUT_BIND_DESC& resourceDesc );
};
