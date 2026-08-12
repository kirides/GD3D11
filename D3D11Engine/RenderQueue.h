#pragma once
#include "WorldObjects.h"

class RenderQueue {
public:
    RenderQueue() = default;
    virtual ~RenderQueue() = default;

    virtual void PushStaticVob( VobInfo* vobInfo ) PURE;
    virtual void PushSkeletalVob( SkeletalVobInfo* vobInfo ) PURE;
    virtual void PushTransparencyVob( TransparencyVobInfo vobInfo ) PURE;
    virtual void PushLightVob( VobLightInfo* vobInfo ) PURE;

    virtual XRESULT Init() { return XR_SUCCESS; }
    virtual void Reset() {}
    virtual XRESULT ProcessQueue() { return XR_SUCCESS; }
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

    ~LegacyRenderQueueProxy() override = default;

    void PushStaticVob( VobInfo* vobInfo ) override {
        vobs.emplace_back( vobInfo );
    }
    void PushSkeletalVob( SkeletalVobInfo* vobInfo ) override {
        skeltalVobs.emplace_back( vobInfo );
    }
    void PushTransparencyVob( TransparencyVobInfo vobInfo ) override {
        transparent.emplace_back( std::move( vobInfo ) );
    }
    void PushLightVob( VobLightInfo* vobInfo ) override {
        lights.emplace_back( vobInfo );
    }

    XRESULT Init() override { return XR_SUCCESS; }

    void Reset() override { }
    XRESULT ProcessQueue() override { return XR_SUCCESS; }

    std::vector<VobInfo*>& vobs;
    std::vector<VobLightInfo*>& lights;
    std::vector<SkeletalVobInfo*>& skeltalVobs;
    std::vector<TransparencyVobInfo> transparent;
};

static std::atomic<size_t> g_nextSeenId = 0;
// Marks any visited vob 
class BspTreeVobVisitor {
public:
    BspTreeVobVisitor(): seen_flag_id( 1 << g_nextSeenId.fetch_add(1, std::memory_order_relaxed ) ) {
    }

    // A vob is registered in every BSP leaf its box touches, so Visit() is called several times per
    // vob per pass while only the FIRST call does anything. The plain relaxed load short-circuits the
    // repeats: a locked read-modify-write (which takes exclusive ownership of the cache line) is then
    // paid once per vob per pass instead of once per (vob, leaf) pair. Correct because only this
    // visitor's own thread ever sets seen_flag_id - other threads own other bits of the same word, so
    // losing the race is impossible; the fetch_or is what keeps their bits intact.
    bool Visit( VobInfo* vobInfo ) {
        if ( (vobInfo->VisibleInRenderPass.load( std::memory_order_relaxed ) & seen_flag_id) != 0 )
            return false;

        vobInfo->VisibleInRenderPass.fetch_or( seen_flag_id, std::memory_order_relaxed );
        vobs.emplace_back( vobInfo );
        return true;
    }
    bool Visit( SkeletalVobInfo* vobInfo ) {
        if ( (vobInfo->VisibleInRenderPass.load( std::memory_order_relaxed ) & seen_flag_id) != 0 )
            return false;

        vobInfo->VisibleInRenderPass.fetch_or( seen_flag_id, std::memory_order_relaxed );
        skeltalVobs.emplace_back( vobInfo );
        return true;
    }
    bool Visit( VobLightInfo* vobInfo ) {
        if ( (vobInfo->VisibleInRenderPass.load( std::memory_order_relaxed ) & seen_flag_id) != 0 )
            return false;

        vobInfo->VisibleInRenderPass.fetch_or( seen_flag_id, std::memory_order_relaxed );
        lights.emplace_back( vobInfo );
        return true;
    }

    void ClearForReuse() {
        for ( auto it : vobs ) { it->VisibleInRenderPass.fetch_xor( seen_flag_id, std::memory_order_relaxed ); }
        for ( auto it : skeltalVobs ) { it->VisibleInRenderPass.fetch_xor( seen_flag_id, std::memory_order_relaxed ); }
        for ( auto it : lights ) { it->VisibleInRenderPass.fetch_xor( seen_flag_id, std::memory_order_relaxed ); }

        vobs.clear();
        lights.clear();
        skeltalVobs.clear();
    }

    size_t GetSeenLights() const { return lights.size(); }
    size_t GetSeenVobs() const { return vobs.size(); }
    size_t GetSeenMobs() const { return skeltalVobs.size(); }
private:
    size_t seen_flag_id;
    std::vector<VobInfo*> vobs;
    std::vector<VobLightInfo*> lights;
    std::vector<SkeletalVobInfo*> skeltalVobs;
};
