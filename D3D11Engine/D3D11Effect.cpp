#include "pch.h"
#include "D3D11Effect.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "D3D11ShaderManager.h"
#include "GothicAPI.h"
#include "D3D11VertexBuffer.h"
#include "D3D11VShader.h"
#include "D3D11GShader.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "GSky.h"
#include <DDSTextureLoader.h>
#include "RenderToTextureBuffer.h"
#include "D3D11_Helpers.h"

// TODO: Remove this!
#include "D3D11GraphicsEngine.h"
#include "oCGame.h"

constexpr float snowSpeedFactor = 0.15f;

namespace {
    const float2 snowScale( 3.0f, 3.0f );
    const float2 rainScale( 30.0f / 10.0f, 30.0f / 2.0f );
}

D3D11Effect::D3D11Effect() {
    RainBufferStatic = nullptr;
    RainBufferDrawFrom = nullptr;
    RainBufferStreamTo = nullptr;
    RainBufferInitial = nullptr;
}

D3D11Effect::~D3D11Effect() {
    delete RainBufferStatic;
    delete RainBufferInitial;
    delete RainBufferDrawFrom;
    delete RainBufferStreamTo;
}

/** Loads a texturearray. Use like the following: Put path and prefix as parameter. The files must then be called name_xxxx.dds */
HRESULT LoadTextureArray( Microsoft::WRL::ComPtr<ID3D11Device1> pd3dDevice, Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context, char* sTexturePrefix, int iNumTextures, ID3D11Texture2D** ppTex2D, ID3D11ShaderResourceView** ppSRV );

/** Fills vectors of random raindrop data, split into mutable and immutable parts */
void D3D11Effect::FillRandomRaindropData( std::vector<RainParticleDynamic>& dynamicData, std::vector<RainParticleStatic>& staticData ) {
    /** Base taken from Nvidias Rain-Sample **/

    float radius = Engine::GAPI->GetRendererState().RendererSettings.RainRadiusRange;
    float height = Engine::GAPI->GetRendererState().RendererSettings.RainHeightRange;

    for ( size_t i = 0; i < dynamicData.size(); i++ ) {
        //use rejection sampling to generate random points inside a circle of radius 1 centered at 0, 0
        float SeedX;
        float SeedZ;
        bool pointIsInside = false;
        while ( !pointIsInside ) {
            SeedX = Toolbox::frand() - 0.5f;
            SeedZ = Toolbox::frand() - 0.5f;
            if ( sqrt( SeedX * SeedX + SeedZ * SeedZ ) <= 0.5f )
                pointIsInside = true;
        }
        //save these random locations for reinitializing rain particles that have fallen out of bounds
        SeedX *= radius;
        SeedZ *= radius;
        float SeedY = Toolbox::frand() * height;

        //add some random speed to the particles, to prevent all the particles from following exactly the same trajectory
        //additionally, random speeds in the vertical direction ensure that temporal aliasing is minimized
        float SpeedX = 40.0f * (Toolbox::frand() / 20.0f);
        float SpeedZ = 40.0f * (Toolbox::frand() / 20.0f);
        float SpeedY = 40.0f * (Toolbox::frand() / 10.0f);

        // Mutable data
        RainParticleDynamic& dynamic = dynamicData[i];
        dynamic.position = float3( SeedX + Engine::GAPI->GetCameraPosition().x, SeedY + Engine::GAPI->GetCameraPosition().y, SeedZ + Engine::GAPI->GetCameraPosition().z );
        dynamic.velocity = XMFLOAT3( SpeedX, SpeedY, SpeedZ );

        // Immutable data
        RainParticleStatic& immutable = staticData[i];
        immutable.seed = float3( SeedX, SeedY, SeedZ );
        immutable.randomBrightness = Toolbox::frand();

        //get an integer between 1 and 8 inclusive to decide which of the 8 types of rain textures the particle will use
        short* s = reinterpret_cast<short*>(&immutable.drawMode);
        s[0] = static_cast<short>( floor( Toolbox::frand() * 8 + 1 ) );
        s[1] = static_cast<short>( floor( Toolbox::frand() * 0xFFFF ) ); // Just a random number
    }
}

/** Draws GPU-Based rain */
XRESULT D3D11Effect::DrawRain() {
    D3D11GraphicsEngineBase* e = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    GothicRendererState& state = Engine::GAPI->GetRendererState();

    // Get shaders
    auto streamOutGS = e->GetShaderManager().GetGShader( "GS_ParticleStreamOut" );
    auto particleAdvanceVS = e->GetShaderManager().GetVShader( "VS_AdvanceRain" );
    auto particleVS = e->GetShaderManager().GetVShader( "VS_ParticlePointShaded" );
    
    bool isSnow = oCGame::GetGame()
        && oCGame::GetGame()->_zCSession_world
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetWeatherType() == zTWEATHER_SNOW;

    auto rainPS = e->GetShaderManager().GetPShader( isSnow ? "PS_Rain_Snow" : "PS_Rain" );

    // artificially increase the number of particles for snow, to make it look better.
    // Snowflakes are bigger and slower than raindrops, so we can get away with less particles for rain, but for snow we need more to make it look good.
    UINT numParticles = state.RendererSettings.RainNumParticles;

    static float lastRadius = state.RendererSettings.RainRadiusRange;
    static float lastHeight = state.RendererSettings.RainHeightRange;
    static UINT lastNumParticles = numParticles;
    static bool firstFrame = true;

    // Create resources if not already done
    if ( !RainBufferDrawFrom || lastHeight != state.RendererSettings.RainHeightRange
        || lastRadius != state.RendererSettings.RainRadiusRange ||
        lastNumParticles != numParticles ) {
        delete RainBufferStatic;
        delete RainBufferDrawFrom;
        delete RainBufferStreamTo;
        delete RainBufferInitial;

        e->CreateVertexBuffer( &RainBufferStatic );
        e->CreateVertexBuffer( &RainBufferDrawFrom );
        e->CreateVertexBuffer( &RainBufferStreamTo );
        e->CreateVertexBuffer( &RainBufferInitial );

        // Fill the vectors with random raindrop data
        std::vector<RainParticleDynamic> dynamicParticles( numParticles );
        std::vector<RainParticleStatic> staticParticles( numParticles );
        FillRandomRaindropData( dynamicParticles, staticParticles );

        // Create immutable structured buffer (SRV only, for StructuredBuffer<> access in shaders)
        RainBufferStatic->Init( &staticParticles[0], staticParticles.size() * sizeof( RainParticleStatic ), D3D11VertexBuffer::B_SHADER_RESOURCE, D3D11VertexBuffer::U_IMMUTABLE, D3D11VertexBuffer::CA_NONE, "D3D11Effect::DrawRain::RainBufferStatic", sizeof( RainParticleStatic ) );

        // Create mutable vertexbuffers (position + velocity only)
        RainBufferInitial->Init( &dynamicParticles[0], dynamicParticles.size() * sizeof( RainParticleDynamic ), (D3D11VertexBuffer::EBindFlags)(D3D11VertexBuffer::B_VERTEXBUFFER), D3D11VertexBuffer::U_DEFAULT, D3D11VertexBuffer::CA_NONE, "D3D11Effect::DrawRain::RainBufferInitial" );
        RainBufferDrawFrom->Init( &dynamicParticles[0], dynamicParticles.size() * sizeof( RainParticleDynamic ), (D3D11VertexBuffer::EBindFlags)(D3D11VertexBuffer::B_VERTEXBUFFER | D3D11VertexBuffer::B_STREAM_OUT), D3D11VertexBuffer::U_DEFAULT, D3D11VertexBuffer::CA_NONE, "D3D11Effect::DrawRain::RainBufferDrawFrom" );
        RainBufferStreamTo->Init( &dynamicParticles[0], dynamicParticles.size() * sizeof( RainParticleDynamic ), (D3D11VertexBuffer::EBindFlags)(D3D11VertexBuffer::B_VERTEXBUFFER | D3D11VertexBuffer::B_STREAM_OUT), D3D11VertexBuffer::U_DEFAULT, D3D11VertexBuffer::CA_NONE, "D3D11Effect::DrawRain::RainBufferStreamTo" );

        firstFrame = true;

        LoadRainResources();
    }

    lastHeight = state.RendererSettings.RainHeightRange;
    lastRadius = state.RendererSettings.RainRadiusRange;
    lastNumParticles = numParticles;

    auto velocity = state.RendererSettings.RainGlobalVelocity;
    if ( isSnow ) {
        // make snow a lot slower
        velocity = XMFLOAT3( velocity.x * snowSpeedFactor, velocity.y * snowSpeedFactor, velocity.z * snowSpeedFactor );
    }

    // Update constantbuffer for the advance-VS
    AdvanceRainConstantBuffer acb;
    XMFLOAT3 LightPosition_XMFloat3;
    XMStoreFloat3( &LightPosition_XMFloat3, XMLoadFloat3( &Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection ) * Engine::GAPI->GetSky()->GetAtmoshpereSettings().OuterRadius + Engine::GAPI->GetCameraPositionXM() );
    acb.AR_LightPosition = LightPosition_XMFloat3;
    acb.AR_FPS = state.RendererInfo.FPS;
    acb.AR_Radius = state.RendererSettings.RainRadiusRange;
    acb.AR_Height = state.RendererSettings.RainHeightRange;
    acb.AR_CameraPosition = Engine::GAPI->GetCameraPosition();
    acb.AR_GlobalVelocity = velocity;
    acb.AR_MoveRainParticles = state.RendererSettings.RainMoveParticles ? 1 : 0;
    particleAdvanceVS->GetConstantBuffer()[0]->UpdateBuffer( &acb );
    particleAdvanceVS->GetConstantBuffer()[0]->BindToVertexShader( 1 );
    particleAdvanceVS->GetConstantBuffer()[0]->BindToPixelShader( 1 );

    if ( firstFrame || (state.RendererSettings.RainMoveParticles && !Engine::GAPI->IsGamePaused()) ) {
        D3D11VertexBuffer* b = nullptr;

        // Use initial-data if we don't have something in the stream-buffers yet
        if ( firstFrame )
            b = RainBufferInitial;
        else
            b = RainBufferDrawFrom;

        firstFrame = false;

        UINT stride = sizeof( RainParticleDynamic );
        UINT offset = 0;

        // Bind buffer to draw from last frame
        e->GetContext()->IASetVertexBuffers( 0, 1, b->GetVertexBuffer().GetAddressOf(), &stride, &offset );

        // Bind immutable particle data as StructuredBuffer SRV for the advance VS
        e->GetContext()->VSSetShaderResources( 1, 1, RainBufferStatic->GetShaderResourceView().GetAddressOf() );

        // Set stream target
        e->GetContext()->SOSetTargets( 1, RainBufferStreamTo->GetVertexBuffer().GetAddressOf(), &offset );

        // Apply shaders
        e->GetContext()->PSSetShader( nullptr, nullptr, 0 );
        particleAdvanceVS->Apply();
        streamOutGS->Apply();

        // Rendering points only
        e->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_POINTLIST );
        e->SetDefaultStates();
        e->UpdateRenderStates();

        // Advance particle system in VS and stream out the data
        e->GetContext()->DrawInstanced( 1, numParticles, 0, 0 );

        // Unset streamout target
        Microsoft::WRL::ComPtr<ID3D11Buffer> bobjStream;
        e->GetContext()->SOSetTargets( 1, bobjStream.ReleaseAndGetAddressOf(), 0 );

        // Swap buffers
        std::swap( RainBufferDrawFrom, RainBufferStreamTo );
    }

    // ---- Draw the rain ----
    // Set alphablending

    state.BlendState.SetAlphaBlending();
    state.BlendState.SetDirty();

    // Disable depth-write
    state.DepthState.DepthWriteEnabled = false;
    state.DepthState.SetDirty();

    // Disable culling
    state.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    state.RasterizerState.SetDirty();

    // Rendering instances only
    e->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
    e->UpdateRenderStates();

    // Apply particle shaders
    e->GetContext()->GSSetShader( nullptr, 0, 0 );
    particleVS->Apply();
    rainPS->Apply();

    // Setup constantbuffers
    ParticleGSInfoConstantBuffer gcb = {};
    gcb.CameraPosition = Engine::GAPI->GetCameraPosition();
    gcb.PGS_RainFxWeight = Engine::GAPI->GetRainFXWeight();
    gcb.PGS_RainHeight = state.RendererSettings.RainHeightRange;
    gcb.PGS_RainScale = isSnow ? snowScale : rainScale;

    particleVS->GetConstantBuffer()[2]->UpdateBuffer( &gcb );
    particleVS->GetConstantBuffer()[2]->BindToVertexShader( 2 );

    ParticlePointShadingConstantBuffer scb = {};
    scb.View = GetRainShadowmapCameraRepl().ViewReplacement;
    scb.Projection = GetRainShadowmapCameraRepl().ProjectionReplacement;
    particleVS->GetConstantBuffer()[1]->UpdateBuffer( &scb );
    particleVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

    RainShadowmap->BindToVertexShader( e->GetContext().Get(), 0 );

    // Bind immutable particle data as StructuredBuffer SRV
    e->GetContext()->VSSetShaderResources( 1, 1, RainBufferStatic->GetShaderResourceView().GetAddressOf() );

    // Bind the shadow comparison sampler to the vertex shader at slot 2 (SS_Comp in shader)
    e->GetContext()->VSSetSamplers( 2, 1, m_RainDropShadowSamplerState.GetAddressOf() );

    // Bind view/proj
    e->SetupVS_ExConstantBuffer();

    // Bind droplets
    e->GetContext()->PSSetShaderResources( 0, 1, isSnow
        ? SnowTextureArraySRV.GetAddressOf()
        : RainTextureArraySRV.GetAddressOf() );

    // Draw the vertexbuffer
    {
        UINT stride = sizeof( RainParticleDynamic );
        UINT offset = 0;
        e->GetContext()->IASetVertexBuffers( 0, 1, RainBufferDrawFrom->GetVertexBuffer().GetAddressOf(), &stride, &offset );
        e->GetContext()->DrawInstanced( 4, numParticles, 0, 0 );
    }

    // Reset this
    e->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    e->GetContext()->GSSetShader( nullptr, 0, 0 );
    return XR_SUCCESS;
}

XRESULT D3D11Effect::DrawRain_CS() {
    D3D11GraphicsEngineBase* e = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    GothicRendererState& state = Engine::GAPI->GetRendererState();

    // Get shaders
    auto advanceRainCS = e->GetShaderManager().GetCShader( "CS_AdvanceRain" );
    auto particleVS = e->GetShaderManager().GetVShader( "VS_ParticlePointShaded" );

    bool isSnow = oCGame::GetGame()
        && oCGame::GetGame()->_zCSession_world
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetWeatherType() == zTWEATHER_SNOW;
    
    auto rainPS = e->GetShaderManager().GetPShader( isSnow ? "PS_Rain_Snow" : "PS_Rain" );

    // artificially increase the number of particles for snow, to make it look better.
    // Snowflakes are bigger and slower than raindrops, so we can get away with less particles for rain, but for snow we need more to make it look good.
    UINT numParticles = state.RendererSettings.RainNumParticles;

    static float lastRadius = state.RendererSettings.RainRadiusRange;
    static float lastHeight = state.RendererSettings.RainHeightRange;
    static UINT lastNumParticles = numParticles;

    if ( !RainBufferDrawFrom || lastHeight != state.RendererSettings.RainHeightRange
        || lastRadius != state.RendererSettings.RainRadiusRange ||
        (lastNumParticles + 127) / 128 != (numParticles + 127) / 128 ) {
        delete RainBufferStatic;
        delete RainBufferDrawFrom;

        e->CreateVertexBuffer( &RainBufferStatic );
        e->CreateVertexBuffer( &RainBufferDrawFrom );

        UINT alignedCount = ((numParticles + 127) / 128) * 128;

        // Fill the vectors with random raindrop data
        std::vector<RainParticleDynamic> dynamicParticles( alignedCount );
        std::vector<RainParticleStatic> staticParticles( alignedCount );
        FillRandomRaindropData( dynamicParticles, staticParticles );

        // Create immutable structured buffer (SRV only, for StructuredBuffer<> access in shaders)
        RainBufferStatic->Init( &staticParticles[0], staticParticles.size() * sizeof( RainParticleStatic ), D3D11VertexBuffer::B_SHADER_RESOURCE, D3D11VertexBuffer::U_DEFAULT, D3D11VertexBuffer::CA_NONE, "D3D11Effect::DrawRain_CS::RainBufferStatic", sizeof( RainParticleStatic ) );

        // Create mutable vertexbuffer (position + velocity, with UAV for compute shader access)
        RainBufferDrawFrom->Init( &dynamicParticles[0], dynamicParticles.size() * sizeof( RainParticleDynamic ), (D3D11VertexBuffer::EBindFlags)(D3D11VertexBuffer::B_VERTEXBUFFER | D3D11VertexBuffer::B_UNORDERED_ACCESS), D3D11VertexBuffer::U_DEFAULT, D3D11VertexBuffer::CA_NONE, "D3D11Effect::DrawRain_CS::RainBufferDrawFrom", sizeof( float ) );

        LoadRainResources();
    }

    lastHeight = state.RendererSettings.RainHeightRange;
    lastRadius = state.RendererSettings.RainRadiusRange;
    lastNumParticles = numParticles;

    auto velocity = state.RendererSettings.RainGlobalVelocity;
    if ( isSnow ) {
        // make snow a lot slower
        velocity = XMFLOAT3(velocity.x * snowSpeedFactor, velocity.y * snowSpeedFactor, velocity.z * snowSpeedFactor );
    }

    // Update constantbuffer for the advance-CS
    AdvanceRainConstantBuffer acb;
    XMFLOAT3 LightPosition_XMFloat3;
    XMStoreFloat3( &LightPosition_XMFloat3, XMLoadFloat3( &Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection ) * Engine::GAPI->GetSky()->GetAtmoshpereSettings().OuterRadius + Engine::GAPI->GetCameraPositionXM() );
    acb.AR_LightPosition = LightPosition_XMFloat3;
    acb.AR_FPS = state.RendererInfo.FPS;
    acb.AR_Radius = state.RendererSettings.RainRadiusRange;
    acb.AR_Height = state.RendererSettings.RainHeightRange;
    acb.AR_CameraPosition = Engine::GAPI->GetCameraPosition();
    acb.AR_GlobalVelocity = velocity;
    acb.AR_MoveRainParticles = numParticles;

    advanceRainCS->GetConstantBuffer()[0]->UpdateBuffer( &acb );
    advanceRainCS->GetConstantBuffer()[0]->BindToPixelShader( 1 );
    if ( state.RendererSettings.RainMoveParticles && !Engine::GAPI->IsGamePaused() ) {
        advanceRainCS->Apply();
        advanceRainCS->GetConstantBuffer()[0]->BindToComputeShader( 0 );

        e->GetContext()->CSSetShaderResources( 0, 1, RainBufferStatic->GetShaderResourceView().GetAddressOf() );
        e->GetContext()->CSSetUnorderedAccessViews( 0, 1, RainBufferDrawFrom->GetUnorderedAccessView().GetAddressOf(), nullptr );
        e->GetContext()->Dispatch( (numParticles + 127) / 128, 1, 1 );

        // Unbind compute shader elements
        Microsoft::WRL::ComPtr<ID3D11Buffer> emptyBuf;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> emptyUAV;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> emptySRV;
        e->GetContext()->CSSetConstantBuffers( 0, 1, emptyBuf.GetAddressOf() );
        e->GetContext()->CSSetShaderResources( 0, 1, emptySRV.GetAddressOf() );
        e->GetContext()->CSSetUnorderedAccessViews( 0, 1, emptyUAV.GetAddressOf(), nullptr );
        e->GetContext()->CSSetShader( nullptr, nullptr, 0 );
    }

    // ---- Draw the rain ----
    // Set alphablending

    state.BlendState.SetAlphaBlending();
    state.BlendState.SetDirty();

    // Disable depth-write
    state.DepthState.DepthWriteEnabled = false;
    state.DepthState.SetDirty();

    // Disable culling
    state.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    state.RasterizerState.SetDirty();

    // Rendering instances only
    e->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
    e->UpdateRenderStates();

    // Apply particle shaders
    particleVS->Apply();
    rainPS->Apply();

    // Setup constantbuffers
    ParticleGSInfoConstantBuffer gcb = {};
    gcb.CameraPosition = Engine::GAPI->GetCameraPosition();
    gcb.PGS_RainFxWeight = Engine::GAPI->GetRainFXWeight();
    gcb.PGS_RainHeight = state.RendererSettings.RainHeightRange;
    gcb.PGS_RainScale = isSnow ? snowScale : rainScale;

    particleVS->GetConstantBuffer()[2]->UpdateBuffer( &gcb );
    particleVS->GetConstantBuffer()[2]->BindToVertexShader( 2 );

    ParticlePointShadingConstantBuffer scb = {};
    scb.View = GetRainShadowmapCameraRepl().ViewReplacement;
    scb.Projection = GetRainShadowmapCameraRepl().ProjectionReplacement;
    particleVS->GetConstantBuffer()[1]->UpdateBuffer( &scb );
    particleVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

    RainShadowmap->BindToVertexShader( e->GetContext().Get(), 0 );

    // Bind immutable particle data as StructuredBuffer SRV
    e->GetContext()->VSSetShaderResources( 1, 1, RainBufferStatic->GetShaderResourceView().GetAddressOf() );

    // Bind the shadow comparison sampler to the vertex shader at slot 2 (SS_Comp in shader)
    e->GetContext()->VSSetSamplers( 2, 1, m_RainDropShadowSamplerState.GetAddressOf() );

    // Bind view/proj
    e->SetupVS_ExConstantBuffer();

    // Bind droplets
    e->GetContext()->PSSetShaderResources( 0, 1, isSnow
        ? SnowTextureArraySRV.GetAddressOf()
        : RainTextureArraySRV.GetAddressOf() );

    // Draw the vertexbuffer
    {
        UINT stride = sizeof( RainParticleDynamic );
        UINT offset = 0;
        e->GetContext()->IASetVertexBuffers( 0, 1, RainBufferDrawFrom->GetVertexBuffer().GetAddressOf(), &stride, &offset );
        e->GetContext()->DrawInstanced( 4, numParticles, 0, 0 );
    }

    // Reset primitive topology
    e->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    return XR_SUCCESS;
}

XRESULT D3D11Effect::LoadRainResources()
{
    D3D11GraphicsEngineBase* e = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    if ( !RainTextureArray.Get() ) {
        HRESULT hr = S_OK;
        // Load textures...
        LogInfo() << "Loading rain-drop textures";
        BASIC_TIMING( t );
        LE( LoadTextureArray( e->GetDevice().Get(), e->GetContext().Get(), "system\\GD3D11\\Textures\\Raindrops\\cv0_vPositive_", 370, &RainTextureArray, &RainTextureArraySRV ) );
        t.Update();
        LogInfo() << "Loading rain drops took " << static_cast<int>(t.GetDelta() * 1000.0f) << "ms";
    }

    if ( !SnowTextureArray.Get() ) {
        HRESULT hr = S_OK;
        // Load textures...
        LogInfo() << "Loading snow flake textures";
        BASIC_TIMING( t );
        LE( LoadTextureArray( e->GetDevice().Get(), e->GetContext().Get(), "system\\GD3D11\\Textures\\Snowflakes\\Snow_", 256, &SnowTextureArray, &SnowTextureArraySRV ) );
        t.Update();
        LogInfo() << "Loading snow flakes took " << static_cast<int>(t.GetDelta() * 1000.0f) << "ms";
    }

    if ( !RainShadowmap.get() ) {
        const int s = 2048;
        RainShadowmap = std::make_unique<RenderToDepthStencilBuffer>( e->GetDevice().Get(), s, s, DXGI_FORMAT_R16_TYPELESS, nullptr, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM );
        SetDebugName( RainShadowmap->GetDepthStencilView().Get(), "RainShadowmap->DepthStencilView" );
        SetDebugName( RainShadowmap->GetShaderResView().Get(), "RainShadowmap->ShaderResView" );
        SetDebugName( RainShadowmap->GetTexture().Get(), "RainShadowmap->Texture" );
    }

    if ( !m_RainDropShadowSamplerState ) {
        // same as in D3D11ShadowMap.cpp
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.MipLODBias = 0;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        samplerDesc.MinLOD = -FLT_MAX;
        samplerDesc.MaxLOD = FLT_MAX;

        HRESULT hr;
        LE( e->GetDevice()->CreateSamplerState(&samplerDesc, m_RainDropShadowSamplerState.GetAddressOf()));
        SetDebugName( m_RainDropShadowSamplerState.Get(), "RainDropSamplerState" );
    }

    return XR_SUCCESS;
}

/** Renders the rain-shadowmap */
XRESULT D3D11Effect::DrawRainShadowmap() {
    if ( !RainShadowmap.get() )
        return XR_SUCCESS;

    D3D11GraphicsEngine* e = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine); // TODO: This has to be a cast to D3D11GraphicsEngineBase!
    //D3D11GraphicsEngineBase* e = (D3D11GraphicsEngineBase*)Engine::GraphicsEngine; //RenderShadowmaps to be moved then to D3D11GraphicsEngineBase
    GothicRendererState& state = Engine::GAPI->GetRendererState();

    CameraReplacement& cr = RainShadowmapCameraRepl;

    // Get the section we are currently in
    XMVECTOR p = Engine::GAPI->GetCameraPositionXM();
    FXMVECTOR dir = XMVector3Normalize( XMLoadFloat3( &Engine::GAPI->GetRendererState().RendererSettings.RainGlobalVelocity ) * -1 ); //check was previous XMVector3NormalizeEst
    // Set the camera height to the highest point in this section
    //p.y = 0;
    p += dir * 6000.0f;

    FXMVECTOR lookAt = p - dir;

    // Create shadowmap view-matrix
    XMMATRIX crViewReplacement = XMMatrixLookAtLH( p, lookAt, XMVectorSet( 0, 1, 0, 0 ) );

    const auto size = RainShadowmap->GetSizeX();
    const auto legacySingleShadowMapScaleFactor = Toolbox::GetRecommendedWorldShadowRangeScaleForSize( size );

    XMMATRIX crProjectionReplacement = XMMatrixOrthographicLH(
        size * legacySingleShadowMapScaleFactor,
        size * legacySingleShadowMapScaleFactor,
        1,
        20000.0f
    );

    XMStoreFloat4x4( &cr.ViewReplacement, XMMatrixTranspose( crViewReplacement ) );
    XMStoreFloat4x4( &cr.ProjectionReplacement, XMMatrixTranspose( crProjectionReplacement ) );
    XMStoreFloat3( &cr.PositionReplacement, p );
    XMStoreFloat3( &cr.LookAtReplacement, lookAt );

    // Replace gothics camera
    Engine::GAPI->SetCameraReplacementPtr( &cr );

    // Make alpharef a bit more aggressive, to make trees less rain-proof

    float oldAlphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;

    Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef = -1.0f;

    // Bind the FF-Info to the first PS slot
    auto PS_Diffuse = e->GetShaderManager().GetPShader( "PS_Diffuse" );
    if ( PS_Diffuse ) {
        PS_Diffuse->GetConstantBuffer()[0]->UpdateBuffer( &Engine::GAPI->GetRendererState().GraphicsState );
        PS_Diffuse->GetConstantBuffer()[0]->BindToPixelShader( 0 );
    }

    // Disable stuff like NPCs and usable things as they don't need to cast rain-shadows
    bool oldDrawSkel = Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes;
    Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes = false;

    // Draw rain-shadowmap

    // Save old rendertargets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    e->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    e->RenderShadowmaps( p, RainShadowmap.get(), true, false );

    // Restore old settings
    e->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
    Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes = oldDrawSkel;
    Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef = oldAlphaRef;
    if ( PS_Diffuse ) {
        PS_Diffuse->GetConstantBuffer()[0]->UpdateBuffer( &Engine::GAPI->GetRendererState().GraphicsState );
    }

    e->SetDefaultStates();

    // Restore gothics camera
    Engine::GAPI->SetCameraReplacementPtr( nullptr );

    return XR_SUCCESS;
}

//--------------------------------------------------------------------------------------
// LoadTextureArray loads a texture array and associated view from a series
// of textures on disk.
//--------------------------------------------------------------------------------------
HRESULT LoadTextureArray( Microsoft::WRL::ComPtr<ID3D11Device1> pd3dDevice, Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context, char* sTexturePrefix, int iNumTextures, ID3D11Texture2D** ppTex2D, ID3D11ShaderResourceView** ppSRV ) {
    if ( !ppTex2D ) {
        LogError() << "invalid argument: ppTex2D. should not be null";
        return E_FAIL;
    }

    HRESULT hr = S_OK;
    D3D11_TEXTURE2D_DESC desc = {};
    DXGI_FORMAT texFormat = DXGI_FORMAT_UNKNOWN;

    //	CHAR szTextureName[MAX_PATH];
    CHAR str[MAX_PATH];
    for ( int i = 0; i < iNumTextures; i++ ) {
        sprintf( str, "%s%.4d.dds", sTexturePrefix, i );

        Microsoft::WRL::ComPtr<ID3D11Resource> pRes;
        LE( CreateDDSTextureFromFileEx( pd3dDevice.Get(), Toolbox::ToWideChar( str ).c_str(), 0, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_WRITE, 0, DDS_LOADER_DEFAULT, pRes.GetAddressOf(), nullptr ) );
        if ( pRes.Get() ) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> pTemp;
            pRes.As( &pTemp );
            if ( !pTemp.Get() ) {
                LogError() << "Could not get ID3D11Texture2D!";
                return E_FAIL;
            }
            pTemp->GetDesc( &desc );

            if ( !(*ppTex2D) ) {
                if ( desc.Format == DXGI_FORMAT_BC4_UNORM || desc.Format == DXGI_FORMAT_R8_UNORM ) {
                    texFormat = desc.Format;
                } else {
                    return E_FAIL;
                }

                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = 0;
                desc.ArraySize = iNumTextures;
                LE( pd3dDevice->CreateTexture2D( &desc, nullptr, ppTex2D ) );
                if ( !(*ppTex2D) )
                    return E_FAIL;
            }

            if ( texFormat != desc.Format )
                return E_FAIL;

            for ( UINT iMip = 0; iMip < desc.MipLevels; iMip++ ) {
                context->CopySubresourceRegion( (*ppTex2D),
                    D3D11CalcSubresource( iMip, i, desc.MipLevels ),
                    0,
                    0,
                    0,
                    pTemp.Get(),
                    iMip,
                    nullptr );
            }

            pRes.Reset();
            pTemp.Reset();
        } else {
            return E_FAIL;
        }
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
    SRVDesc.Format = desc.Format;
    SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    SRVDesc.Texture2DArray.MipLevels = desc.MipLevels;
    SRVDesc.Texture2DArray.ArraySize = iNumTextures;
    LE( pd3dDevice->CreateShaderResourceView( *ppTex2D, &SRVDesc, ppSRV ) );

    return hr;
}
