#include "pch.h"
#include "D3D11GraphicsShader.h"
#include <d3dcompiler.h>

#include "D3D11ConstantBuffer.h"

GraphicsShaderConstantBuffer& GraphicsShaderConstantBuffer::Bind() {
    return Bind(slot);
}

GraphicsShaderConstantBuffer& GraphicsShaderConstantBuffer::Bind(UINT slot) {
    if ( buffer ) {
        shader->BindBuffer(slot, buffer);
    }
    return *this;
}

int32_t D3D11GraphicsShader::GetInputIndex( StringID name )
{
    auto kvp = InputSemanticToIndex.find( name );
    if (kvp != InputSemanticToIndex.end()) {
        return kvp->second;
    }
#ifdef DEBUG_D3D11
    // LogError() << "Tried to find input index for semantic '" << name << "' but it was not found in the shader!";
#endif
    return -1;
}

GraphicsShaderConstantBuffer D3D11GraphicsShader::GetBuffer(StringID name) {
    auto kvp = ConstantBuffersByName.find( name );
    if (kvp != ConstantBuffersByName.end()) {
        return GraphicsShaderConstantBuffer(kvp->second.first, kvp->second.second, this);
    }
#ifdef DEBUG_D3D11
    // LogError() << "Tried to find constant buffer for semantic '" << name << "' but it was not registered!";
#endif
    return GraphicsShaderConstantBuffer(nullptr, 10, nullptr);
}

HRESULT D3D11GraphicsShader::ReflectShaderResources( ID3DBlob* shaderBlob )
{
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> pReflection;
    HRESULT hr = D3DReflect(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        IID_PPV_ARGS( &pReflection )
    );

    if ( SUCCEEDED( hr ) ) {
        D3D11_SHADER_DESC shaderDesc;
        pReflection->GetDesc( &shaderDesc );

        OnReflectShader(shaderBlob, pReflection.Get(), shaderDesc);
    }
    return hr;
}

void D3D11GraphicsShader::OnReflectShader(ID3DBlob* blob, ID3D11ShaderReflection* pReflection, const D3D11_SHADER_DESC& shaderDesc)
{
    // Would be nice to also create all constant buffers, but we need a way to tell the reflection engine "Hey, this is an external per-frame"-Constant Buffer
    ConstantBuffers.clear();
    ConstantBuffers.reserve(shaderDesc.ConstantBuffers);
    ConstantBuffersByName.clear();
    ConstantBuffersByName.reserve(shaderDesc.ConstantBuffers);
    
    // Loop through every resource bound to this shader
    for ( UINT i = 0; i < shaderDesc.BoundResources; ++i ) {
        D3D11_SHADER_INPUT_BIND_DESC resourceDesc;
        if ( SUCCEEDED( pReflection->GetResourceBindingDesc( i, &resourceDesc ) ) ) {
            OnReflectShaderResource(pReflection, shaderDesc, resourceDesc);
        }
        
        if ( resourceDesc.Type == D3D_SHADER_INPUT_TYPE::D3D_SIT_CBUFFER ) {
            auto pCB = pReflection->GetConstantBufferByName( resourceDesc.Name );

            D3D11_SHADER_BUFFER_DESC cbDesc;
            if ( SUCCEEDED( pCB->GetDesc( &cbDesc ) ) ) {
                // cbDesc.Size is the total byte size of the buffer, padded to always be a multiple of 16
                UINT paddedSize = ((cbDesc.Size * resourceDesc.BindCount) + 15) & ~15;

                // Ignore the bind-point here, due to global-per-frame CBs
                ConstantBuffers.emplace_back(std::make_unique<D3D11ConstantBuffer>( paddedSize, nullptr ));
                ConstantBuffersByName[StringID(resourceDesc.Name)] = {ConstantBuffers.back().get(), resourceDesc.BindPoint};
            }
        }
    }    
}

void D3D11GraphicsShader::OnReflectShaderResource(
    ID3D11ShaderReflection* pReflection,
    const D3D11_SHADER_DESC& shaderDesc, 
    const D3D11_SHADER_INPUT_BIND_DESC& resourceDesc)
{
    InputSemanticToIndex[StringID(resourceDesc.Name)] = resourceDesc.BindPoint;
}
