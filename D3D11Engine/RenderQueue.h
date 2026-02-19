#pragma once
#include "WorldObjects.h"

class RenderQueue {
public:
    RenderQueue() = default;
    virtual ~RenderQueue() = default;

    virtual void PushStaticVob( VobInfo* vobInfo ) PURE;
    virtual void PushSkeletalVob( SkeletalVobInfo* vobInfo ) PURE;
    virtual void PushTransparencyVob( const TransparencyVobInfo& vobInfo ) PURE;
    virtual void PushLightVob( VobLightInfo* vobInfo ) PURE;

    virtual XRESULT Init() PURE;
    virtual void Reset() PURE;
    virtual XRESULT ProcessQueue() PURE;
};

// Only used for legacy render paths where we need to manipulate render state while collecting them
class LegacyRenderQueueProxy: public RenderQueue {
public:
    LegacyRenderQueueProxy( 
        std::vector<VobInfo*>& vobs,
        std::vector<VobLightInfo*>& lights,
        std::vector<SkeletalVobInfo*>& skeltalVobs
    ):
        vobs( vobs ),
        lights( lights ),
        skeltalVobs( skeltalVobs )
    {
    }

    virtual ~LegacyRenderQueueProxy() = default;

    void PushStaticVob( VobInfo* vobInfo ) {
        vobs.push_back( vobInfo );
    }
    void PushSkeletalVob( SkeletalVobInfo* vobInfo ) {
        skeltalVobs.push_back( vobInfo );
    }
    void PushTransparencyVob( const TransparencyVobInfo& vobInfo ) {
        transparent.push_back( vobInfo );
    }
    void PushLightVob( VobLightInfo* vobInfo ) {
        lights.push_back( vobInfo );
    }

    XRESULT Init() { return XR_SUCCESS; }

    void Reset() { }
    XRESULT ProcessQueue() { return XR_SUCCESS; }

    std::vector<VobInfo*>& vobs;
    std::vector<VobLightInfo*>& lights;
    std::vector<SkeletalVobInfo*>& skeltalVobs;
    std::vector<TransparencyVobInfo> transparent;
};


// Marks any visited vob 
class BspTreeVobVisitor {
public:
    void Visit( VobInfo* vobInfo ) {
        vobInfo->VisibleInRenderPass = true;
        vobs.push_back( vobInfo );
    }
    void Visit( SkeletalVobInfo* vobInfo ) {
        vobInfo->VisibleInRenderPass = true;
        skeltalVobs.push_back( vobInfo );
    }
    void Visit( VobLightInfo* vobInfo ) {
        vobInfo->VisibleInRenderPass = true;
        lights.push_back( vobInfo );
    }

    void ClearForReuse() {
        for ( auto it : vobs ) { it->VisibleInRenderPass = false; }
        for ( auto it : skeltalVobs ) { it->VisibleInRenderPass = false; }
        for ( auto it : lights ) { it->VisibleInRenderPass = false; }

        vobs.clear();
        lights.clear();
        skeltalVobs.clear();
    }
private:
    std::vector<VobInfo*> vobs;
    std::vector<VobLightInfo*> lights;
    std::vector<SkeletalVobInfo*> skeltalVobs;
};
