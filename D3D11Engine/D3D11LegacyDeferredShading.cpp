#include "pch.h"
#include "D3D11LegacyDeferredShading.h"

#include "D3D11GraphicsEngine.h"
#include "D3D11PointLight.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "ConstantBufferStructs.h"
#include "D3D11_Helpers.h"
#include "zCVobLight.h"
#include "GMesh.h"

XRESULT D3D11LegacyDeferredShading::DrawPointlightLights(
    std::vector<VobLightInfo*>& lights,
    RenderToTextureBuffer& color,
    RenderToTextureBuffer& normals,
    RenderToTextureBuffer& specular,
    RenderToTextureBuffer& depthCopy ) {
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "LegacyPointlightLights" ) );
    auto& context = graphicsEngine->GetContext();
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    view = XMMatrixTranspose( view );

    graphicsEngine->SetActiveVertexShader( VShaderID::VS_ExPointLight );
    graphicsEngine->SetActivePixelShader( PShaderID::PS_DS_PointLight );

    auto psPointLight = graphicsEngine->GetShaderManager().GetPShader( PShaderID::PS_DS_PointLight );
    auto psPointLightDynShadow = graphicsEngine->GetShaderManager().GetPShader( PShaderID::PS_DS_PointLightDynShadow );
    auto plBuf = psPointLight->GetBuffer( "DS_PointLightConstantBuffer" );
    auto plDynBuf = psPointLightDynShadow->GetBuffer( "DS_PointLightConstantBuffer" );

    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
    if ( settings.LimitLightIntesity ) {
        Engine::GAPI->GetRendererState().BlendState.BlendOp = GothicBlendStateInfo::BO_BLEND_OP_MAX;
    }
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    graphicsEngine->SetupVS_ExMeshDrawCall();
    graphicsEngine->SetupVS_ExConstantBuffer();

    context->OMSetRenderTargets( 1, graphicsEngine->GetHDRBackBuffer().GetRenderTargetView().GetAddressOf(), graphicsEngine->GetDepthBuffer()->GetDepthStencilView().Get() );

    DS_PointLightConstantBuffer plcb = {};

    {
        auto& proj = Engine::GAPI->GetProjectionMatrix();
        plcb.PL_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
    }
    XMStoreFloat4x4( &plcb.PL_InvView, XMMatrixInverse( nullptr, XMLoadFloat4x4( &Engine::GAPI->GetRendererState().TransformState.TransformView ) ) );

    plcb.PL_ViewportSize = Engine::GraphicsEngine->GetResolution();

    color.BindToPixelShader( context.Get(), 0 );
    normals.BindToPixelShader( context.Get(), 1 );
    specular.BindToPixelShader( context.Get(), 7 );
    depthCopy.BindToPixelShader( context.Get(), 2 );

    for ( auto const& light : lights ) {
        zCVobLight* vob = light->Vob;

        if ( !vob->IsEnabled() ) continue;

        if ( settings.EnablePointlightShadows > 0 ) {
            D3D11PointLight* pl = light->LightShadowBuffers ? static_cast<D3D11PointLight*>(light->LightShadowBuffers.get()) : nullptr;

            if ( pl && pl->IsInited() && pl->HasShadowMap( 0 ) ) {
                if ( graphicsEngine->GetActivePS() != psPointLightDynShadow ) {
                    graphicsEngine->SetActivePS( psPointLightDynShadow )->Apply();
                }
            } else if ( graphicsEngine->GetActivePS() != psPointLight ) {
                graphicsEngine->SetActivePS( psPointLight )->Apply();
            }
        }

        vob->DoAnimation();

        plcb.PL_Color = float4( vob->GetLightColor() );
        plcb.PL_Range = vob->GetLightRange();
        plcb.Pl_PositionWorld = vob->GetPositionWorld();
        plcb.PL_Outdoor = light->IsIndoorVob ? 0.0f : 1.0f;

        float dist;
        XMStoreFloat( &dist, XMVector3Length( XMLoadFloat3( plcb.Pl_PositionWorld.toXMFLOAT3() ) - Engine::GAPI->GetCameraPositionXM() ) );

        if ( dist + plcb.PL_Range <
            settings.VisualFXDrawRadius ) {
            float fadeEnd =
                settings.VisualFXDrawRadius;

            float fadeFactor = std::min(
                1.0f,
                std::max( 0.0f, ((fadeEnd - (dist + plcb.PL_Range)) / plcb.PL_Range) ) );
            plcb.PL_Color.x *= fadeFactor;
            plcb.PL_Color.y *= fadeFactor;
            plcb.PL_Color.z *= fadeFactor;
        }

        float lightFactor = 1.2f;

        plcb.PL_Color.x *= lightFactor;
        plcb.PL_Color.y *= lightFactor;
        plcb.PL_Color.z *= lightFactor;

        FXMVECTOR Pl_PositionWorld = XMLoadFloat3( plcb.Pl_PositionWorld.toXMFLOAT3() );
        XMStoreFloat3( plcb.Pl_PositionView.toXMFLOAT3(),
            XMVector3TransformCoord( Pl_PositionWorld, view ) );

        XMStoreFloat3( plcb.PL_LightScreenPos.toXMFLOAT3(),
            XMVector3TransformCoord( Pl_PositionWorld, XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() ) ) );

        if ( dist < plcb.PL_Range ) {
            if ( Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled ) {
                Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled = false;
                Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_FRONT;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();
                Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
                graphicsEngine->UpdateRenderStates();
            }
        } else {
            if ( !Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled ) {
                Engine::GAPI->GetRendererState().DepthState.DepthBufferEnabled = true;
                Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();
                Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
                graphicsEngine->UpdateRenderStates();
            }
        }

        plcb.PL_LightScreenPos.x = plcb.PL_LightScreenPos.x / 2.0f + 0.5f;
        plcb.PL_LightScreenPos.y = plcb.PL_LightScreenPos.y / -2.0f + 0.5f;

        auto& activePlBuf = (graphicsEngine->GetActivePS() == psPointLightDynShadow) ? plDynBuf : plBuf;
        activePlBuf.Update( &plcb ).Bind();
        activePlBuf.GetRawBuffer()->BindToVertexShader( 1 );

        if ( settings.EnablePointlightShadows > 0 ) {
            if ( light->LightShadowBuffers )
                static_cast<D3D11PointLight*>(light->LightShadowBuffers.get())->OnRenderLight();
        }

        graphicsEngine->InverseUnitSphereMesh->DrawMesh();

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnLights++;
    }

    return XR_SUCCESS;
}
