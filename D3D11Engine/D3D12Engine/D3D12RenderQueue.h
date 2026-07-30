#pragma once
#include "../RenderQueue.h"

struct RenderView;

class D3D12RenderQueue: public RenderQueue {
public:
    explicit D3D12RenderQueue(RenderView* vobInstances,
    std::vector<SkeletalVobInfo*>* mobs,
    std::vector<TransparencyVobInfo>* tranparencyVobInfos,
    std::vector<VobLightInfo*>* lights
    ); 

    void PushStaticVob(VobInfo* vobInfo) override;
    void PushSkeletalVob(SkeletalVobInfo* vobInfo) override;
    void PushTransparencyVob(TransparencyVobInfo vobInfo) override;
    void PushLightVob(VobLightInfo* vobInfo) override;
private:
    RenderView* vobInstances;
    std::vector<SkeletalVobInfo*>* mobs;
    std::vector<TransparencyVobInfo>* tranparency_vob_infos;
    std::vector<VobLightInfo*>* lights;
};
