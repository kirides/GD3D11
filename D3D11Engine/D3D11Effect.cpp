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
#include <d3dcompiler.h>
#include "D3D11_Helpers.h"

// TODO: Remove this!
#include "D3D11GraphicsEngine.h"

D3D11Effect::D3D11Effect() {
    RainBufferDrawFrom = nullptr;
    RainBufferStreamTo = nullptr;
    RainBufferInitial = nullptr;
}

D3D11Effect::~D3D11Effect() {
    delete RainBufferInitial;
    delete RainBufferDrawFrom;
    delete RainBufferStreamTo;
}

/** Loads a texturearray. Use like the following: Put path and prefix as parameter. The files must then be called name_xxxx.dds */
HRESULT LoadTextureArray( Microsoft::WRL::ComPtr<ID3D11Device1> pd3dDevice, Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context, char* sTexturePrefix, int iNumTextures, ID3D11Texture2D** ppTex2D, ID3D11ShaderResourceView** ppSRV );

/** Fills a vector of random raindrop data */
void D3D11Effect::FillRandomRaindropData( std::vector<RainParticleInstanceInfo>& data ) {
    /** Base taken from Nvidias Rain-Sample **/

    float radius = Engine::GAPI->GetRendererState().RendererSettings.RainRadiusRange;
    float height = Engine::GAPI->GetRendererState().RendererSettings.RainHeightRange;

    for ( size_t i = 0; i < data.size(); i++ ) {
        RainParticleInstanceInfo raindrop;
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
        //raindrop.seed = XMFLOAT3(SeedX,SeedY,SeedZ); 

        //add some random speed to the particles, to prevent all the particles from following exactly the same trajectory
        //additionally, random speeds in the vertical direction ensure that temporal aliasing is minimized
        float SpeedX = 40.0f * (Toolbox::frand() / 20.0f);
        float SpeedZ = 40.0f * (Toolbox::frand() / 20.0f);
        float SpeedY = 40.0f * (Toolbox::frand() / 10.0f);
        raindrop.velocity = XMFLOAT3( SpeedX, SpeedY, SpeedZ );

        //move the rain particles to a random positions in a cylinder above the camera
        raindrop.position = float3( SeedX + Engine::GAPI->GetCameraPosition().x, SeedY + Engine::GAPI->GetCameraPosition().y, SeedZ + Engine::GAPI->GetCameraPosition().z );

        //get an integer between 1 and 8 inclusive to decide which of the 8 types of rain textures the particle will use
        short* s = reinterpret_cast<short*>(&raindrop.drawMode);
        s[0] = static_cast<short>( floor( Toolbox::frand() * 8 + 1 ) );
        s[1] = static_cast<short>( floor( Toolbox::frand() * 0xFFFF ) ); // Just a random number

        //this number is used to randomly increase the brightness of some rain particles
        float intensity = 1.0f;
        float randomIncrease = Toolbox::frand();
        if ( randomIncrease > 0.8f )
            intensity += randomIncrease;

        raindrop.color = float4( SeedX, SeedY, SeedZ, randomIncrease );

        float height = 30.0f;
        raindrop.scale = float2( height / 10.0f, height / 2.0f );

        data[i] = raindrop;
    }
}

/** Draws GPU-Based rain */
XRESULT D3D11Effect::DrawRain() {
    D3D11GraphicsEngineBase* e = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    GothicRendererState& state = Engine::GAPI->GetRendererState();

    // Get shaders
    auto streamOutGS = e->GetShaderManager().GetGShader( "GS_ParticleStreamOut" );
    auto particleGS = e->GetShaderManager().GetGShader( "GS_Raindrops" );
    auto particleAdvanceVS = e->GetShaderManager().GetVShader( "VS_AdvanceRain" );
    auto particleVS = e->GetShaderManager().GetVShader( "VS_ParticlePointShaded" );
    auto rainPS = e->GetShaderManager().GetPShader( "PS_Rain" );

    UINT numParticles = Engine::GAPI->GetRendererState().RendererSettings.RainNumParticles;

    static float lastRadius = state.RendererSettings.RainRadiusRange;
    static float lastHeight = state.RendererSettings.RainHeightRange;
    static bool firstFrame = true;

    // Create resources if not already done
    if ( !RainBufferDrawFrom || lastHeight != state.RendererSettings.RainHeightRange
        || lastRadius != state.RendererSettings.RainRadiusRange ) {
        delete RainBufferDrawFrom;
        delete RainBufferStreamTo;
        delete RainBufferInitial;

        e->CreateVertexBuffer( &RainBufferDrawFrom );
        e->CreateVertexBuffer( &RainBufferStreamTo );
        e->CreateVertexBuffer( &RainBufferInitial );

        UINT numParticles = Engine::GAPI->GetRendererState().RendererSettings.RainNumParticles;
        std::vector<RainParticleInstanceInfo> particles( numParticles );

        // Fill the vector with random raindrop data
        FillRandomRaindropData( particles );

        // Create vertexbuffers
        RainBufferInitial->Init( &particles[0], particles.size() * sizeof( RainParticleInstanceInfo ), (D3D11VertexBuffer::EBindFlags)(D3D11VertexBuffer::B_VERTEXBUFFER), D3D11VertexBuffer::U_DEFAULT, D3D11VertexBuffer::CA_NONE, "D3D11Effect::DrawRain::RainBufferInitial" );
        RainBufferDrawFrom->Init( &particles[0], particles.size() * sizeof( RainParticleInstanceInfo ), (D3D11VertexBuffer::EBindFlags)(D3D11VertexBuffer::B_VERTEXBUFFER | D3D11VertexBuffer::B_STREAM_OUT), D3D11VertexBuffer::U_DEFAULT, D3D11VertexBuffer::CA_NONE, "D3D11Effect::DrawRain::RainBufferDrawFrom" );
        RainBufferStreamTo->Init( &particles[0], particles.size() * sizeof( RainParticleInstanceInfo ), (D3D11VertexBuffer::EBindFlags)(D3D11VertexBuffer::B_VERTEXBUFFER | D3D11VertexBuffer::B_STREAM_OUT), D3D11VertexBuffer::U_DEFAULT, D3D11VertexBuffer::CA_NONE, "D3D11Effect::DrawRain::RainBufferStreamTo" );

        firstFrame = true;

        LoadRainResources();
    }

    lastHeight = state.RendererSettings.RainHeightRange;
    lastRadius = state.RendererSettings.RainRadiusRange;

    D3D11VertexBuffer* b = nullptr;

    // Use initial-data if we don't have something in the stream-buffers yet
    if ( firstFrame || state.RendererSettings.RainUseInitialSet || Engine::GAPI->IsGamePaused() )
        b = RainBufferInitial;
    else
        b = RainBufferDrawFrom;

    firstFrame = false;

    UINT stride = sizeof( RainParticleInstanceInfo );
    UINT offset = 0;

    // Bind buffer to draw from last frame
    e->GetContext()->IASetVertexBuffers( 0, 1, b->GetVertexBuffer().GetAddressOf(), &stride, &offset );

    // Set stream target
    e->GetContext()->SOSetTargets( 1, RainBufferStreamTo->GetVertexBuffer().GetAddressOf(), &offset );

    // Apply shaders
    e->GetContext()->PSSetShader( nullptr, nullptr, 0 );
    particleAdvanceVS->Apply();
    streamOutGS->Apply();

    // Rendering points only
    e->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_POINTLIST );

    // Update constantbuffer for the advance-VS
    AdvanceRainConstantBuffer acb;
    XMFLOAT3 LightPosition_XMFloat3;
    XMStoreFloat3( &LightPosition_XMFloat3, XMLoadFloat3( &Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection ) * Engine::GAPI->GetSky()->GetAtmoshpereSettings().OuterRadius + Engine::GAPI->GetCameraPositionXM() );
    acb.AR_LightPosition = LightPosition_XMFloat3;
    acb.AR_FPS = state.RendererInfo.FPS;
    acb.AR_Radius = state.RendererSettings.RainRadiusRange;
    acb.AR_Height = state.RendererSettings.RainHeightRange;
    acb.AR_CameraPosition = Engine::GAPI->GetCameraPosition();
    acb.AR_GlobalVelocity = state.RendererSettings.RainGlobalVelocity;
    acb.AR_MoveRainParticles = state.RendererSettings.RainMoveParticles ? 1 : 0;

    particleAdvanceVS->GetConstantBuffer()[0]->UpdateBuffer( &acb );
    particleAdvanceVS->GetConstantBuffer()[0]->BindToVertexShader( 1 );
    particleAdvanceVS->GetConstantBuffer()[0]->BindToPixelShader( 1 );

    e->SetDefaultStates();
    e->UpdateRenderStates();

    // Advance particle system in VS and stream out the data
    e->GetContext()->Draw( numParticles, 0 );

    // Unset streamout target
    Microsoft::WRL::ComPtr<ID3D11Buffer> bobjStream;
    e->GetContext()->SOSetTargets( 1, bobjStream.ReleaseAndGetAddressOf(), 0 );

    // Swap buffers
    std::swap( RainBufferDrawFrom, RainBufferStreamTo );

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

    e->UpdateRenderStates();

    // Apply particle shaders
    particleVS->Apply();
    rainPS->Apply();
    particleGS->Apply();

    // Setup constantbuffers
    ParticleGSInfoConstantBuffer gcb = {};
    gcb.CameraPosition = Engine::GAPI->GetCameraPosition();
    gcb.PGS_RainFxWeight = Engine::GAPI->GetRainFXWeight();
    gcb.PGS_RainHeight = state.RendererSettings.RainHeightRange;

    particleGS->GetConstantBuffer()[0]->UpdateBuffer( &gcb );
    particleGS->GetConstantBuffer()[0]->BindToGeometryShader( 2 );

    ParticlePointShadingConstantBuffer scb = {};
    scb.View = GetRainShadowmapCameraRepl().ViewReplacement;
    scb.Projection = GetRainShadowmapCameraRepl().ProjectionReplacement;
    particleVS->GetConstantBuffer()[1]->UpdateBuffer( &scb );
    particleVS->GetConstantBuffer()[1]->BindToVertexShader( 1 );

    RainShadowmap->BindToVertexShader( e->GetContext().Get(), 0 );

    // Bind view/proj
    e->SetupVS_ExConstantBuffer();

    // Bind droplets
    e->GetContext()->PSSetShaderResources( 0, 1, RainTextureArraySRV.GetAddressOf() );

    // Draw the vertexbuffer
    e->DrawVertexBuffer( RainBufferDrawFrom, numParticles, sizeof( RainParticleInstanceInfo ) );

    // Reset this
    e->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    e->GetContext()->GSSetShader( nullptr, 0, 0 );
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

    if ( !RainShadowmap.get() ) {
        const int s = 2048;
        RainShadowmap = std::make_unique<RenderToDepthStencilBuffer>( e->GetDevice().Get(), s, s, DXGI_FORMAT_R32_TYPELESS, nullptr, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT );
        SetDebugName( RainShadowmap->GetDepthStencilView().Get(), "RainShadowmap->DepthStencilView" );
        SetDebugName( RainShadowmap->GetShaderResView().Get(), "RainShadowmap->ShaderResView" );
        SetDebugName( RainShadowmap->GetTexture().Get(), "RainShadowmap->Texture" );
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

    XMMATRIX crProjectionReplacement = XMMatrixOrthographicLH(
        RainShadowmap->GetSizeX() * Engine::GAPI->GetRendererState().RendererSettings.WorldShadowRangeScale,
        RainShadowmap->GetSizeX() * Engine::GAPI->GetRendererState().RendererSettings.WorldShadowRangeScale,
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
    e->RenderShadowmaps( p, RainShadowmap.get(), true, false );

    // Restore old settings
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

    //	CHAR szTextureName[MAX_PATH];
    CHAR str[MAX_PATH];
    for ( int i = 0; i < iNumTextures; i++ ) {
        sprintf( str, "%s%.4d.dds", sTexturePrefix, i );

        Microsoft::WRL::ComPtr<ID3D11Resource> pRes;
        LE( CreateDDSTextureFromFileEx( pd3dDevice.Get(), Toolbox::ToWideChar( str ).c_str(), 0, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, 0, DDS_LOADER_DEFAULT, pRes.GetAddressOf(), nullptr ) );
        if ( pRes.Get() ) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> pTemp;
            pRes.As( &pTemp );
            if ( !pTemp.Get() ) {
                LogError() << "Could not get ID3D11Texture2D!";
                return E_FAIL;
            }
            pTemp->GetDesc( &desc );

            if ( DXGI_FORMAT_R8_UNORM != desc.Format )
                return E_FAIL;

            if ( !(*ppTex2D) ) {
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = 0;
                desc.ArraySize = iNumTextures;
                LE( pd3dDevice->CreateTexture2D( &desc, nullptr, ppTex2D ) );
                if ( !(*ppTex2D) )
                    return E_FAIL;
            }

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
