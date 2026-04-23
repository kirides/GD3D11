#include "pch.h"
#include "D3D11PFX_FSR1.h"

#include "D3D11PfxRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11ShaderManager.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "RenderToTextureBuffer.h"
#include "ConstantBufferStructs.h"
#include "Engine.h"

// Include AMD FidelityFX FSR1 header for CPU setup functions
#define A_CPU
#include "Shaders/FidelityFX/fsr1/ffx_a.h"
#include "Shaders/FidelityFX/fsr1/ffx_fsr1.h"

D3D11PFX_FSR1::D3D11PFX_FSR1( D3D11PfxRenderer* renderer )
    : Renderer( renderer )
    , Sharpness( 0.2f )
    , CurrentInputSize( 0, 0 )
    , CurrentOutputSize( 0, 0 )
    , Initialized( false ) {
}

D3D11PFX_FSR1::~D3D11PFX_FSR1() {
}

bool D3D11PFX_FSR1::Init() {
    if ( Initialized ) {
        return true;
    }

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto device = engine->GetDevice();

    // Create point sampler for Gather operations (FSR1 requires point sampling)
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = device->CreateSamplerState( &samplerDesc, PointSampler.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create FSR1 point sampler";
        return false;
    }

    Initialized = true;
    return true;
}

void D3D11PFX_FSR1::OnResize( const INT2& inputSize, const INT2& outputSize ) {
    // Only recreate intermediate buffer if output size changed
    if ( CurrentOutputSize.x != outputSize.x || CurrentOutputSize.y != outputSize.y ) {
        CurrentOutputSize = outputSize;
    }
    
    CurrentInputSize = inputSize;
}

void D3D11PFX_FSR1::SetSharpness( float sharpness ) {
    // FSR1 RCAS uses "stops" where 0.0 = maximum sharpness
    // Higher values reduce sharpness (each +1.0 halves the sharpness)
    Sharpness = std::max( 0.0f, sharpness );
}

void D3D11PFX_FSR1::ReleaseResources() {
    PointSampler.Reset();
}

XRESULT D3D11PFX_FSR1::ApplyEASU(
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input,
    ID3D11RenderTargetView* output,
    const INT2& inputSize,
    const INT2& outputSize ) {
    
    if ( !Initialized && !Init() ) {
        return XR_FAILED;
    }

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto context = engine->GetContext();

    engine->SetDefaultStates();
    engine->UpdateRenderStates();

    // Get EASU shader
    auto easuPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_FSR1_EASU );
    if ( !easuPS ) {
        LogError() << "FSR1 EASU shader not found";
        return XR_FAILED;
    }

    // Setup EASU constants
    FSR1EASUConstantBuffer cb = {};
    
    FsrEasuCon(
        cb.Const0,
        cb.Const1,
        cb.Const2,
        cb.Const3,
        static_cast<AF1>(inputSize.x),   // Input viewport width
        static_cast<AF1>(inputSize.y),   // Input viewport height
        static_cast<AF1>(inputSize.x),   // Input texture width
        static_cast<AF1>(inputSize.y),   // Input texture height
        static_cast<AF1>(outputSize.x),  // Output width
        static_cast<AF1>(outputSize.y)   // Output height
    );

    // Update constant buffer
    easuPS->GetBuffer( "FSR1Constants" ).Update( &cb ).Bind();

    // Save old render targets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    // Set output render target
    context->OMSetRenderTargets( 1, &output, nullptr );

    // Set viewport to output size
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(outputSize.x);
    vp.Height = static_cast<float>(outputSize.y);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports( 1, &vp );

    // Bind input texture and point sampler
    context->PSSetShaderResources( 0, 1, input.GetAddressOf() );
    context->PSSetSamplers( 0, 1, PointSampler.GetAddressOf() );

    // Apply shader
    easuPS->Apply();

    // Draw fullscreen quad
    Renderer->DrawFullScreenQuad();

    // Unbind resources
    ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
    context->PSSetShaderResources( 0, 1, nullSRV );

    // Restore old render targets
    context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

    return XR_SUCCESS;
}

XRESULT D3D11PFX_FSR1::ApplyRCAS(
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input,
    ID3D11RenderTargetView* output,
    float sharpness ) {
    
    if ( !Initialized && !Init() ) {
        return XR_FAILED;
    }

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto context = engine->GetContext();

    engine->SetDefaultStates();
    engine->UpdateRenderStates();

    // Get RCAS shader
    auto rcasPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_FSR1_RCAS );
    if ( !rcasPS ) {
        LogError() << "FSR1 RCAS shader not found";
        return XR_FAILED;
    }

    // Setup RCAS constants
    FSR1RCASConstantBuffer cb = {};
    
    FsrRcasCon( cb.RCASConst, sharpness );

    // Update constant buffer
    rcasPS->GetBuffer( "FSR1RCASConstants" ).Update( &cb ).Bind();

    // Save old render targets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    // Set output render target
    context->OMSetRenderTargets( 1, &output, nullptr );

    // Bind input texture and point sampler
    context->PSSetShaderResources( 0, 1, input.GetAddressOf() );
    context->PSSetSamplers( 0, 1, PointSampler.GetAddressOf() );

    // Apply shader
    rcasPS->Apply();

    // Draw fullscreen quad
    Renderer->DrawFullScreenQuad();

    // Unbind resources
    ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
    context->PSSetShaderResources( 0, 1, nullSRV );

    // Restore old render targets
    context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

    return XR_SUCCESS;
}

XRESULT D3D11PFX_FSR1::Apply(
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input,
    ID3D11RenderTargetView* output,
    const INT2& inputSize,
    const INT2& outputSize,
    bool enableRCAS,
    float sharpness ) {
    
    if ( !Initialized && !Init() ) {
        return XR_FAILED;
    }

    // Ensure intermediate buffer is sized correctly
    OnResize( inputSize, outputSize );

    if ( enableRCAS ) {
        // TODO: this assumes that the backbuffer is always the correct size as outputSize
        auto tempBuffer = Renderer->GetBackbufferTempBuffer();

        // Two-pass: EASU to intermediate buffer, then RCAS to final output
        XRESULT result = ApplyEASU( input, tempBuffer->GetRenderTargetView().Get(), inputSize, outputSize);
        if ( result != XR_SUCCESS ) {
            return result;
        }

        return ApplyRCAS( tempBuffer->GetShaderResView(), output, sharpness );
    } else {
        // Single-pass: EASU directly to output
        return ApplyEASU( input, output, inputSize, outputSize );
    }
}
