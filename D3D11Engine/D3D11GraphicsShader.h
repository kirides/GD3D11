#pragma once
#include "GraphicsShader.h"
#include <d3d11.h>
#include <d3d11shader.h>
#include <gtl/phmap.hpp>
#include "D3D11ConstantBuffer.h"
#include "Types.h"
#include "StringID.h"

class D3D11GraphicsShader;
class D3D11ConstantBuffer;
constexpr size_t MAX_SHADER_CB = 6;
constexpr size_t INVALID_SHADER_CB_SLOT = 255;

struct GraphicsShaderConstantBuffer
{
public:
    GraphicsShaderConstantBuffer()
        : buffer( nullptr ),
        slot( -1 ),
        shader( nullptr )
    {
    }

    GraphicsShaderConstantBuffer(
        D3D11ConstantBuffer* buffer,
        UINT slot,
        D3D11GraphicsShader* shader)
        : buffer(buffer),
    slot(slot),
    shader(shader)
    {}

    GraphicsShaderConstantBuffer& Update(const void* data) { 
        if ( buffer ) {
            buffer->UpdateBuffer(data);
        }
        return *this; 
    }
    
    GraphicsShaderConstantBuffer& Update(const void* data, UINT size) { 
        if ( buffer ) {
            buffer->UpdateBuffer(data, size);
        }
        return *this; 
    }
    GraphicsShaderConstantBuffer& Bind();
    GraphicsShaderConstantBuffer& Bind(UINT slot);
    constexpr D3D11ConstantBuffer* GetRawBuffer() const { return buffer; }
    constexpr UINT GetSlot() const { return slot; } 
private:
    D3D11ConstantBuffer* buffer;
    D3D11GraphicsShader* shader;
    UINT slot;
};

class D3D11GraphicsShader 
    : public GraphicsShader
{
public:
    D3D11GraphicsShader() = default;
    ~D3D11GraphicsShader() override = default;
    /** Returns the input index for the given semantic name */
    int32_t GetInputIndex( StringID name ) override;
    
    std::array<std::unique_ptr<D3D11ConstantBuffer>, MAX_SHADER_CB>& GetConstantBuffer() { return ConstantBuffers; }

    virtual void BindResource(StringID name, ID3D11ShaderResourceView* srv) = 0;
    virtual void BindSampler(StringID name, ID3D11SamplerState* sampler) = 0;
    virtual void BindBuffer( StringID name, D3D11ConstantBuffer* buffer) = 0;
    virtual void BindBuffer(UINT slot, D3D11ConstantBuffer* buffer) = 0;
    virtual GraphicsShaderConstantBuffer GetBuffer(StringID name);
    virtual GraphicsShaderConstantBuffer GetBuffer(UINT slot);
    
    virtual XRESULT Apply() = 0;
protected:
    gtl::flat_hash_map<StringID, int32_t> InputSemanticToIndex;
    gtl::flat_hash_map<StringID, std::pair<D3D11ConstantBuffer*, int32_t>> ConstantBuffersByName;
    std::array<std::unique_ptr<D3D11ConstantBuffer>, MAX_SHADER_CB> ConstantBuffers;
    std::array<byte, MAX_SHADER_CB> ConstantBufferIndexBySlot;

    virtual HRESULT ReflectShaderResources( ID3DBlob* shaderBlob );
    virtual void OnReflectShader( ID3DBlob* blob, ID3D11ShaderReflection* pReflection,  const D3D11_SHADER_DESC& shaderDesc );
    virtual void OnReflectShaderResource( ID3D11ShaderReflection* pReflection, const D3D11_SHADER_DESC& shaderDesc, const D3D11_SHADER_INPUT_BIND_DESC& resourceDesc );
};
