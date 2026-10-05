// D3D12GraphicsEngine — low clouds (Shaders/D3D12/LowClouds.hlsl + Shaders/include/LowClouds.hlsl).
//
// Mirrors D3D11's "Generate Low Clouds" / "Composite Low Clouds" passes: the half-resolution layer is marched
// before water, which reflects it through m_LowCloudLayerSrvSlot, and blended onto the scene after the fog.
#include "../pch.h"
#include "D3D12GraphicsEngine.h"
#include "D3D12PipelineState.h"
#include "D3D12RenderGraph.h"
#include "../Engine.h"
#include "../GothicAPI.h"
#include "../GSky.h"
#include "../ConstantBufferStructs.h"

#include "D3D12EngineCommon.h"

bool D3D12GraphicsEngine::CreateLowCloudConstantBuffers() {
    // One persistently-mapped UPLOAD buffer per frame-in-flight: [0,256) LowCloudCB (b2), [256,512) atmosphere (b1).
    static_assert( sizeof( LowCloudConstantBuffer ) <= kLowCloudAtmosphereCbOffset, "LowCloudCB must fit the first block" );
    static_assert( sizeof( AtmosphereConstantBuffer ) <= 512 - kLowCloudAtmosphereCbOffset, "atmosphere must fit the second block" );

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = 512;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for ( UINT i = 0; i < kBackBufferCount; ++i ) {
        if ( FAILED( m_Rhi->CreateResource( DefaultUploadHeapType, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                m_LowCloudCB[i].ReleaseAndGetAddressOf() ) ) ) {
            Logging::Wrn( "D3D12: failed to create the low cloud constant buffer." );
            return false;
        }
        m_LowCloudCB[i]->SetName( L"LowCloudCB" );
        D3D12_RANGE noRead = { 0, 0 };
        void* mapped = nullptr;
        if ( FAILED( m_LowCloudCB[i]->Map( 0, &noRead, &mapped ) ) ) return false;
        m_LowCloudCBMapped[i] = static_cast<uint8_t*>( mapped );
        m_LowCloudCBGpu[i] = m_LowCloudCB[i]->GetGPUVirtualAddress();
    }

    // Created in UNORDERED_ACCESS: GenerateLowClouds fills it once, then it stays a shader resource
    constexpr UINT kNoiseSize = 64;
    D3D12_RESOURCE_DESC noiseDesc = {};
    noiseDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    noiseDesc.Width = kNoiseSize;
    noiseDesc.Height = kNoiseSize;
    noiseDesc.DepthOrArraySize = kNoiseSize;
    noiseDesc.MipLevels = 1;
    noiseDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    noiseDesc.SampleDesc.Count = 1;
    noiseDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    noiseDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if ( FAILED( m_Rhi->CreateResource( D3D12_HEAP_TYPE_DEFAULT, &noiseDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            m_LowCloudNoise.ReleaseAndGetAddressOf(), Rhi::RESOURCE_FLAG_TRACK_LAYOUT ) ) ) {
        Logging::Wrn( "D3D12: failed to create the low cloud noise texture." );
        return false;
    }
    m_LowCloudNoise->SetName( L"LowCloudNoise" );
    m_LowCloudNoiseReady = false;
    if ( m_LowCloudNoiseSrvSlot == UINT_MAX ) m_LowCloudNoiseSrvSlot = AllocateSrvSlot();
    if ( m_LowCloudNoiseUavSlot == UINT_MAX ) m_LowCloudNoiseUavSlot = AllocateSrvSlot();
    if ( m_LowCloudNoiseSrvSlot == UINT_MAX || m_LowCloudNoiseUavSlot == UINT_MAX ) {
        m_LowCloudNoise.Reset();
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture3D.MipLevels = 1;
    m_Rhi->CreateShaderResourceView( m_LowCloudNoise.Get(), &srv, GetSrvCpuHandle( m_LowCloudNoiseSrvSlot ) );

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uav.Texture3D.WSize = kNoiseSize;
    m_Rhi->CreateUnorderedAccessView( m_LowCloudNoise.Get(), nullptr, &uav, GetSrvCpuHandle( m_LowCloudNoiseUavSlot ) );
    return true;
}


void D3D12GraphicsEngine::GenerateLowClouds() {
    m_LowCloudLayerSrvSlot = UINT_MAX;
    m_LowCloudDepthSrvSlot = UINT_MAX;
    m_SkyLowCloudSrvSlot = UINT_MAX;

    GSky* sky = Engine::GAPI->GetSky();
    if ( !m_FrameOpen || !m_CmdList || !sky || !sky->AreLowCloudsVisible() ) return;
    if ( !m_Pipelines.LowClouds.GeneratePSO || !m_Pipelines.LowClouds.CompositePSO || !m_LowCloudCBMapped[m_FrameIndex] ) return;
    if ( !m_Pipelines.LowClouds.NoisePSO || !m_LowCloudNoise ) return;
    if ( !m_DepthBuffer || m_DepthSrvSlot == UINT_MAX ) return;

    LowCloudConstantBuffer cb;
    sky->FillLowCloudConstants( cb );
    cb.LC_NoiseIndex = m_LowCloudNoiseSrvSlot;
    memcpy( m_LowCloudCBMapped[m_FrameIndex], &cb, sizeof( cb ) );
    const auto& atmo = sky->GetAtmosphereCB();
    memcpy( m_LowCloudCBMapped[m_FrameIndex] + kLowCloudAtmosphereCbOffset, &atmo, sizeof( atmo ) );

    if ( !m_LowCloudNoiseReady ) {
        DX_ZONE( m_CmdList.Get(), "Low cloud noise" );
        const UINT consts[4] = { m_LowCloudNoiseUavSlot, 0, 0, 0 };
        m_CmdList->SetComputeRootSignature( m_Pipelines.LowClouds.GenerateRootSig.Get() );
        m_CmdList->SetPipelineState( m_Pipelines.LowClouds.NoisePSO.Get() );
        m_CmdList->SetComputeRoot32BitConstants( 0, 4, consts, 0 );
        m_CmdList->SetComputeRootConstantBufferView( 1, m_LowCloudCBGpu[m_FrameIndex] );
        m_CmdList->SetComputeRootConstantBufferView( 2, m_LowCloudCBGpu[m_FrameIndex] + kLowCloudAtmosphereCbOffset );
        m_CmdList->Dispatch( 64 / 4, 64 / 4, 64 / 4 );
        m_CmdList->TransitionBarriers( { { m_LowCloudNoise.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE } } );
        m_LowCloudNoiseReady = true;
    }

    const uint32_t width = static_cast<uint32_t>( std::max( 1, ( m_Resolution.x + 1 ) / 2 ) );
    const uint32_t height = static_cast<uint32_t>( std::max( 1, ( m_Resolution.y + 1 ) / 2 ) );

    // Local graph: the textures stay placed in the arena (name-keyed) for the water pass and the composite.
    D3D12RenderGraph graph( &m_AliasArena );
    RGResourceHandle layerHandle = RG_INVALID_HANDLE;
    RGResourceHandle depthHandle = RG_INVALID_HANDLE;
    RGResourceHandle skyHandle = RG_INVALID_HANDLE;
    graph.AddPass( RG_PASS_NAME( "Low Clouds" ), [&]( D3D12RGBuilder& builder, D3D12RenderPass& pass ) {
        layerHandle = builder.CreateTexture( { width, height, static_cast<int>( DXGI_FORMAT_R16G16B16A16_FLOAT ), L"LowCloudLayer", 1u },
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        depthHandle = builder.CreateTexture( { width, height, static_cast<int>( DXGI_FORMAT_R32_FLOAT ), L"LowCloudDepth", 1u },
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        skyHandle = builder.CreateTexture( { width, height, static_cast<int>( DXGI_FORMAT_R16G16B16A16_FLOAT ), L"SkyLowCloudLayer", 1u },
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        builder.MarkExternalEffect();

        pass.m_executeCallback = [this, width, height, layerHandle, depthHandle, skyHandle]( const D3D12RenderGraph& g, D3D12CmdList& cmdList ) {
            D3D12RenderTarget* layer = g.GetPhysicalTexture( layerHandle );
            D3D12RenderTarget* depth = g.GetPhysicalTexture( depthHandle );
            D3D12RenderTarget* skyLayer = g.GetPhysicalTexture( skyHandle );
            if ( !layer || !depth || !skyLayer ) return;
            DX_ZONE( cmdList.Get(), "Low clouds" );

            // The depth buffer leaves its DSV binding to be read by the march
            cmdList.OMSetRenderTargets( 0, nullptr, FALSE, nullptr );
            TransitionSceneDepth( cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );

            const UINT consts[4] = { m_DepthSrvSlot, layer->GetUavSlot(), depth->GetUavSlot(), skyLayer->GetUavSlot() };
            cmdList.SetComputeRootSignature( m_Pipelines.LowClouds.GenerateRootSig.Get() );
            cmdList.SetPipelineState( m_Pipelines.LowClouds.GeneratePSO.Get() );
            cmdList.SetComputeRoot32BitConstants( 0, 4, consts, 0 );
            cmdList.SetComputeRootConstantBufferView( 1, m_LowCloudCBGpu[m_FrameIndex] );
            cmdList.SetComputeRootConstantBufferView( 2, m_LowCloudCBGpu[m_FrameIndex] + kLowCloudAtmosphereCbOffset );
            cmdList.Dispatch( ( width + 7 ) / 8, ( height + 7 ) / 8, 1 );

            cmdList.TransitionBarriers( {
                { layer->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
                { depth->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
                { skyLayer->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
                } );
            layer->State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            depth->State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            skyLayer->State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            };
        } );
    graph.Compile();
    graph.Execute( m_CmdList );

    D3D12RenderTarget* layer = graph.GetPhysicalTexture( layerHandle );
    D3D12RenderTarget* depth = graph.GetPhysicalTexture( depthHandle );
    D3D12RenderTarget* skyLayer = graph.GetPhysicalTexture( skyHandle );
    if ( layer && depth && skyLayer ) {
        m_LowCloudLayerSrvSlot = layer->GetSrvSlot();
        m_LowCloudDepthSrvSlot = depth->GetSrvSlot();
        m_SkyLowCloudSrvSlot = skyLayer->GetSrvSlot();
    }
    BindSceneColorTarget();   // the march dropped the scene target and its DSV
}


void D3D12GraphicsEngine::AddLowCloudCompositePass( D3D12RenderGraph& graph ) {
    if ( m_LowCloudLayerSrvSlot == UINT_MAX || m_LowCloudDepthSrvSlot == UINT_MAX || m_SkyLowCloudSrvSlot == UINT_MAX ) return;
    if ( !m_Pipelines.LowClouds.CompositePSO || !m_SceneColor || !m_DepthBuffer || m_DepthSrvSlot == UINT_MAX ) return;

    const std::array<UINT, 4> consts = { m_LowCloudLayerSrvSlot, m_LowCloudDepthSrvSlot, m_SkyLowCloudSrvSlot, m_DepthSrvSlot };
    graph.AddPass( RG_PASS_NAME( "Low Cloud Composite" ), [&]( D3D12RGBuilder&, D3D12RenderPass& pass ) {
        pass.m_executeCallback = [this, consts]( const D3D12RenderGraph&, D3D12CmdList& cmdList ) {
            DX_ZONE( cmdList.Get(), "Low cloud composite" );
            if ( m_SceneColorInPixelState ) {
                cmdList.TransitionBarrier( m_SceneColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET );
                m_SceneColorInPixelState = false;
            }

            const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>( m_Resolution.x ), static_cast<float>( m_Resolution.y ), 0.0f, 1.0f };
            const D3D12_RECT     sc = { 0, 0, m_Resolution.x, m_Resolution.y };
            cmdList.RSSetViewports( 1, &vp );
            cmdList.RSSetScissorRects( 1, &sc );
            cmdList.OMSetRenderTargets( 1, &m_SceneColorRtv, FALSE, nullptr );   // depth is read as an SRV
            TransitionSceneDepth( cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
            cmdList.IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
            cmdList.IASetVertexBuffers( 0, 0, nullptr );

            cmdList.SetPipelineState( m_Pipelines.LowClouds.CompositePSO.Get() );
            cmdList.SetGraphicsRootSignature( m_Pipelines.LowClouds.CompositeRootSig.Get() );
            cmdList.SetGraphicsRoot32BitConstants( 0, 4, consts.data(), 0 );
            cmdList.SetGraphicsRootConstantBufferView( 1, m_LowCloudCBGpu[m_FrameIndex] );
            cmdList.SetGraphicsRootConstantBufferView( 2, m_LowCloudCBGpu[m_FrameIndex] + kLowCloudAtmosphereCbOffset );
            cmdList.DrawInstanced( 3, 1, 0, 0 );
            };
        } );
}
