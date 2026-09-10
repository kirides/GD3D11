#pragma once
#include "pch.h"

struct SkeletalVobInfo;
class zCModel;
class D3D11VertexBuffer;

/** Model-space bone poses packed once per frame for every pass that skins a skeletal vob (prepass, main,
    CSM, point-light cubes). Uploads only what was added since the last flush, into grow-only buffers. */
class D3D11SkeletalPoseCache {
public:
    struct Pose {
        uint32_t Offset = 0;   ///< into the packed buffers; the previous-frame pose sits at the same offset
        uint32_t Count = 0;    ///< 0 when the model has no nodes
    };

    D3D11SkeletalPoseCache();
    ~D3D11SkeletalPoseCache();

    /** Forgets the previous frame's poses; the GPU buffers stay allocated. */
    void BeginFrame();

    /** This frame's pose of the vob, gathered on first use. */
    Pose Acquire( SkeletalVobInfo* vi, zCModel* model );

    /** Uploads the poses acquired since the last flush. False when the structured buffers are unavailable. */
    bool Flush();

    /** Only valid until the next Acquire, which may grow the storage. */
    std::span<const XMFLOAT4X4> Bones( const Pose& pose ) const {
        return std::span<const XMFLOAT4X4>( m_Bones.data() + pose.Offset, pose.Count );
    }

    ID3D11ShaderResourceView* GetBonesSRV() const;
    ID3D11ShaderResourceView* GetPrevBonesSRV() const;

private:
    struct Entry {
        zCModel* Model = nullptr;
        Pose Pose;
    };

    bool Grow( uint32_t required );

    gtl::flat_hash_map<SkeletalVobInfo*, Entry> m_Entries;
    std::vector<XMFLOAT4X4> m_Bones;
    std::vector<XMFLOAT4X4> m_PrevBones;
    size_t m_Frame = static_cast<size_t>( -1 );
    uint32_t m_UploadedCount = 0;
    uint32_t m_Capacity = 0;
    bool m_BufferFailed = false;
    std::unique_ptr<D3D11VertexBuffer> m_BoneBuffer;
    std::unique_ptr<D3D11VertexBuffer> m_PrevBoneBuffer;
};
