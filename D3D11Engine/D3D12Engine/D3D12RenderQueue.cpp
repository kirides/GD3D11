#include "D3D12RenderQueue.h"
#include "InstancingUtils.h"
#include "../GothicAPI.h"
#include "../zCVob.h"

D3D12RenderQueue::D3D12RenderQueue(RenderView* vobInstances,
    std::vector<SkeletalVobInfo*>* mobs,
    std::vector<TransparencyVobInfo>* tranparencyVobInfos,
    std::vector<VobLightInfo*>* lights
    )
        : vobInstances(vobInstances), mobs(mobs), tranparency_vob_infos(tranparencyVobInfos), lights(lights)
{ }

void D3D12RenderQueue::PushStaticVob(VobInfo* vobInfo)
{
    const auto it = vobInfo;
    VobInstanceInfo vii = {};
    vii.world = it->WorldMatrix;
    vii.prevWorld = it->HasValidPrevMatrix ? it->PrevWorldMatrix : it->WorldMatrix;
    vii.color = it->GroundColor;
    vii.windStrenth = 0.0f;
    vii.canBeAffectedByPlayer = 0;

    zTAnimationMode aniMode = it->Vob->GetVisualAniMode();
    if ( aniMode != zVISUAL_ANIMODE_NONE ) {
        vii.canBeAffectedByPlayer = (!it->Vob->GetDynColl() ? 1.0f : 0.0f);
        GothicAPI::ProcessVobAnimation( it->Vob, aniMode, vii );
    }
    vobInstances->buckets[vobInfo->VisualIndex].instances.push_back(vii);
}

void D3D12RenderQueue::PushSkeletalVob(SkeletalVobInfo* vobInfo)
{
    mobs->push_back(vobInfo);
}

void D3D12RenderQueue::PushTransparencyVob(TransparencyVobInfo vobInfo)
{
    tranparency_vob_infos->push_back(std::move(vobInfo));
}

void D3D12RenderQueue::PushLightVob(VobLightInfo* vobInfo)
{
    lights->push_back(vobInfo);
}
