#pragma once
#include <unordered_map>
#include <vector>

#include "RenderQueue.h"
#include "WorldObjects.h"

class zCVisual;
class zCMaterial;
struct MeshVisualInfo;
struct VobInstanceInfo;
struct SkeletalVobInfo;
struct VobLightInfo;
struct VobInfo;

struct RenderItem
{
    union
    {
        uint64_t value;

        struct
        {
            unsigned int layer : 4;
            unsigned int shaderId : 12;
            unsigned int meshId : 16;
            unsigned int transformId : 32;
        } opaque;

        struct
        {
        private:
            unsigned int _padding;
        public:
            float depth;
        } shadowmap;
    } sortKey;

    uint32_t meshIndex;
    uint32_t materialIndex;
    uint32_t transformIndex;
    uint32_t instanceId;
};

class D3D11RenderQueue: public RenderQueue {
public:
    D3D11RenderQueue( ID3D11Device* device, ID3D11DeviceContext* context ):
        m_Device( device ),
        m_Context( context )
    { }

    ~D3D11RenderQueue() override = default;

    XRESULT Init() override ;
    void Reset() override;
    XRESULT ProcessQueue() override;

    void PushStaticVob( VobInfo* vobInfo ) override;
    void PushSkeletalVob( SkeletalVobInfo* vobInfo ) override;
    void PushTransparencyVob( TransparencyVobInfo vobInfo ) override;
    void PushLightVob( VobLightInfo* vobInfo ) override;

    std::vector<VobInfo*>& GetVobs() { return vobs; }

private:
    std::vector<VobInfo*> vobs;
    std::vector<VobLightInfo*> lights;
    std::vector<SkeletalVobInfo*> skeltalVobs;
    std::vector<TransparencyVobInfo> transparent;

    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
};
