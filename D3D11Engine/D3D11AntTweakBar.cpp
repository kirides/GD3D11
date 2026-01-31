#include "pch.h"
#include "D3D11AntTweakBar.h"
#include "AntTweakBar.h"
#include "Engine.h"
#include "D3D11GraphicsEngineBase.h"

D3D11AntTweakBar::D3D11AntTweakBar() {}

D3D11AntTweakBar::~D3D11AntTweakBar() {}

/** Creates the resources */
XRESULT D3D11AntTweakBar::Init() {
    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);

    LogInfo() << "Initializing AntTweakBar";
    if ( !TwInit( TW_DIRECT3D11, engine->GetDevice().Get() ) )
        return XR_FAILED;

    TwWindowSize( engine->GetBackbufferResolution().x, engine->GetBackbufferResolution().y );

    return BaseAntTweakBar::Init();
}
