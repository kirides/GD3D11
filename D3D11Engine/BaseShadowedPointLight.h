#pragma once

struct BaseVobInfo;
class BaseShadowedPointLight {
public:
    BaseShadowedPointLight();
    virtual ~BaseShadowedPointLight();

    /** Called when a vob got removed from the world */
    virtual void OnVobRemovedFromWorld( BaseVobInfo* vob ) {};

    /** True once this light has a valid, up-to-date baked static shadow it can just keep reusing (no
        camera-proximity nudge needed). Backend-neutral so callers outside the D3D11-specific classes (e.g.
        GothicAPI's per-frame light collection) can ask "does this still need a bake" without downcasting.
        Default false so a backend that never overrides this (nothing to bake, or not implemented yet) is
        conservatively treated as always needing one. */
    virtual bool IsShadowReady() const { return false; }
};

