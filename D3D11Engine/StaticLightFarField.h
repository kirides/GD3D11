#pragma once
#include <cstdint>
#include <memory>
#include <vector>

struct VobInfo;
struct ExVertexStruct;

/** The static point lights' contribution beyond StaticLightNearRadius, baked at world load: irradiance per
    wrapped world vertex (RGB9E5, GPU vertex order) and per VOB (VobInfo::FarFieldColor), occluded by the world
    mesh and static VOBs. Lights in the bake (VobLightInfo::FarFieldBaked) fade out per pixel as this fades in. */
class StaticLightFarField {
public:
    StaticLightFarField();
    ~StaticLightFarField();

    /** ConvertWorldMesh, after wrapping: the wrapped vertices' normals. Kept only while the setting is on. */
    void CaptureWorldVertices( const std::vector<ExVertexStruct>& wrapped );
    /** World unload: drops everything and clears the lights' flags. */
    void Reset();
    /** End of world load, main thread: bake (or read the cache) for these VOBs. */
    void Bake( const std::vector<VobInfo*>& vobs );
    /** Once per frame, main thread: true when Bake() should run again (setting switched on, or a baked light
        was removed, moved or switched off). Rate-limited. */
    bool NeedsRebake();
    /** Bake again on the next frame, ignoring the cache file. */
    void RequestRebake();

    /** Baked data present and the setting on. */
    bool IsActive() const;
    /** Bumped whenever the baked data changes. */
    uint32_t GetVersion() const { return m_Version; }
    const std::vector<uint32_t>& GetVertexIrradiance() const { return m_VertexIrradiance; }

    /** The lit shaders' crossfade band from the settings; invFade 0 when inactive. */
    static void Crossfade( bool active, float& nearRadius, float& invFade );

    /** Instance color for a VOB: green stays the ground light, the other bytes carry the far field when active. */
    static uint32_t ComposeInstanceColor( uint32_t groundColor, uint32_t farFieldColor, bool active ) {
        return active ? ( groundColor & 0x0000FF00u ) | farFieldColor : groundColor;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    std::vector<uint32_t> m_VertexIrradiance;
    uint32_t m_Version = 0;
};

StaticLightFarField& GetStaticLightFarField();
