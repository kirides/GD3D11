#include "D3D11RenderQueue.h"

XRESULT D3D11RenderQueue::Init()
{
    // TODO: Create any necessery resources, like resource managers or other things.
    return XR_SUCCESS;
}

void D3D11RenderQueue::Reset()
{
    vobs.clear();
    skeltalVobs.clear();
    lights.clear();
    transparent.clear();
}

XRESULT D3D11RenderQueue::ProcessQueue()
{
    // std::sort() vobs etc.
    return XR_SUCCESS;
}


void D3D11RenderQueue::PushStaticVob( VobInfo* vobInfo )
{
    vobs.push_back( vobInfo );
    // TODO: actually create renderItem in here, ready for indexed/instanced drawing.
}

void D3D11RenderQueue::PushSkeletalVob( SkeletalVobInfo* vobInfo )
{
    skeltalVobs.push_back( vobInfo );
    // TODO: actually create renderItem in here, ready for indexed/instanced drawing.
}

void D3D11RenderQueue::PushTransparencyVob( const TransparencyVobInfo& vobInfo )
{
    transparent.push_back( vobInfo );
    // TODO: actually create renderItem in here, ready for indexed/instanced drawing.
}

void D3D11RenderQueue::PushLightVob( VobLightInfo* vobInfo )
{
    lights.push_back( vobInfo );
    // TODO: actually create renderItem in here, ready for indexed/instanced drawing.
}
