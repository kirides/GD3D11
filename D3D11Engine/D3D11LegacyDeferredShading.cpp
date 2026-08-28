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
    auto plBuf = psPointLight->GetInputIndex( "DS_PointLightConstantBuffer" );
    auto plDynBuf = psPointLightDynShadow->GetInputIndex( "DS_PointLightConstantBuffer" );

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
        // Camera jitter (TAA/FSR) in UV space, subtracted before depth->world reconstruction.
        plcb.PL_JitterOffset = float2( proj._13 * 0.5f, -proj._23 * 0.5f );
    }
    XMStoreFloat4x4( &plcb.PL_InvView, XMMatrixInverse( nullptr, XMLoadFloat4x4( &Engine::GAPI->GetRendererState().TransformState.TransformView ) ) );

    plcb.PL_ViewportSize = Engine::GraphicsEngine->GetResolution();

    color.BindToPixelShader( context.Get(), 0 );
    normals.BindToPixelShader( context.Get(), 1 );
    specular.BindToPixelShader( context.Get(), 7 );
    depthCopy.BindToPixelShader( context.Get(), 2 );

    // Mirrors D3D12 BuildFrameLightBuffer's post-selection range clamp (D3D12Scene.cpp): a light that
    // ends up with no shadow cube shades unshadowed and bleeds through walls. Clamping its range to a
    // small fraction keeps it lighting its own alcove instead of the next room - or, worse, the
    // outside of the building it's sealed inside when the camera is outdoors and nothing can occlude
    // it. Gated on IsStatic() OR IsIndoorVob, not IsStatic() alone: zCVobLight's "static" bit is
    // Gothic's own IsStatic() (colour-animated fine, never repositioned), so a candle or brazier with
    // a colour animation reads as non-static and would otherwise bleed through walls completely
    // unshadowed - same failure mode as an atmospheric fill light. Outdoor dynamic lights (the
    // player's torch, spell effects) are still exempt: open air has no walls to bleed through.
    constexpr float kUnshadowedStaticScale = 0.35f;      // still lights its own alcove
    constexpr float kIndoorSeenFromOutsideScale = 0.15f; // worst bleed case - clamp it much harder
    const zCVob* playerVob = Engine::GAPI->GetPlayerVob();
    const bool cameraIndoors = playerVob && playerVob->IsIndoorVob();

    auto cbPool = graphicsEngine->GetConstantBufferPool();
    for ( auto const& light : lights ) {
        zCVobLight* vob = light->Vob;

        if ( !vob->IsEnabled() ) continue;

        bool hasShadow = false;
        if ( settings.EnablePointlightShadows > 0 ) {
            D3D11PointLight* pl = light->LightShadowBuffers ? static_cast<D3D11PointLight*>(light->LightShadowBuffers.get()) : nullptr;

            hasShadow = pl && pl->IsInited() && pl->HasShadowMap( 0 );
            if ( hasShadow ) {
                if ( graphicsEngine->GetActivePS() != psPointLightDynShadow ) {
                    graphicsEngine->SetActivePS( psPointLightDynShadow )->Apply();
                }
            } else if ( graphicsEngine->GetActivePS() != psPointLight ) {
                graphicsEngine->SetActivePS( psPointLight )->Apply();
            }
        }

        vob->DoAnimation();

        plcb.PL_Color = float4( vob->GetLightColor() );
        plcb.PL_Color.w = settings.PointLightSpecularScale( vob->IsStatic() );
        plcb.PL_Range = vob->GetLightRange();
        if ( !hasShadow && ( vob->IsStatic() || light->IsIndoorVob ) ) {
            const bool leakingOutdoors = light->IsIndoorVob && !cameraIndoors;
            plcb.PL_Range *= leakingOutdoors ? kIndoorSeenFromOutsideScale : kUnshadowedStaticScale;
        }
        plcb.Pl_PositionWorld = vob->GetPositionWorld();
        plcb.PL_Outdoor = light->IsIndoorVob ? 0.0f : 1.0f;

        float dist;
        XMStoreFloat( &dist, XMVector3Length( XMLoadFloat3( &plcb.Pl_PositionWorld ) - Engine::GAPI->GetCameraPositionXM() ) );

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

        XMVECTOR Pl_PositionWorld = XMLoadFloat3( &plcb.Pl_PositionWorld );
        XMStoreFloat3( &plcb.Pl_PositionView,
            XMVector3TransformCoord( Pl_PositionWorld, view ) );

        XMStoreFloat3( &plcb.PL_LightScreenPos,
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
        auto rainBufAllocation = cbPool->Allocate(&plcb, sizeof(plcb));
        cbPool->BindPS(activePlBuf, rainBufAllocation);
        cbPool->BindVS(1, rainBufAllocation);

        if ( settings.EnablePointlightShadows > 0 ) {
            if ( light->LightShadowBuffers )
                static_cast<D3D11PointLight*>(light->LightShadowBuffers.get())->OnRenderLight();
        }

        graphicsEngine->InverseUnitSphereMesh->DrawMesh();

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnLights++;
    }

    return XR_SUCCESS;
}
