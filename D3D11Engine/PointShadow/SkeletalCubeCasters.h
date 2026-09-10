#pragma once
// Skeletal casters (NPC bodies, their attachments, skeletal MOBs) for the point-light cubes. Each vob's draw data is
// recorded once per point-light pass; PointShadowBatch draws it into every light that sees the vob.

#include "../pch.h"
#include "../ConstantBufferStructs.h"
#include "../D3D11SkeletalPoseCache.h"
#include "PointShadowBatch.h"

struct SkeletalVobInfo;
class zCVob;

namespace SkeletalCubeCasters {

    struct AttachmentDraw {
        VS_ExConstantBuffer_PerInstanceNode Instance;
        const zCVob* SlotVob = nullptr;        // the inventory item hanging on this node, for self-exclusion
        uint32_t FirstMesh = 0;
        uint32_t NumMeshes = 0;
    };

    struct Record {
        D3D11SkeletalPoseCache::Pose Pose;
        VS_ExConstantBuffer_PerInstanceSkeletal Instance;
        uint32_t FirstBodyMesh = 0;
        uint32_t NumBodyMeshes = 0;
        uint32_t FirstAttachment = 0;
        uint32_t NumAttachments = 0;
    };

    constexpr uint32_t kNoRecord = UINT32_MAX;

    /** Drops the recorded draws. They point at attachment meshes a later pass may release. */
    void BeginPass();

    /** The vob's record for this pass, built on first use; kNoRecord when it has nothing to draw. */
    uint32_t RecordFor( SkeletalVobInfo* vi );

    const Record& GetRecord( uint32_t index );
    std::span<const CasterMeshDraw> BodyMeshes( const Record& record );
    std::span<const AttachmentDraw> Attachments( const Record& record );
    std::span<const CasterMeshDraw> AttachmentMeshes( const AttachmentDraw& attachment );

} // namespace SkeletalCubeCasters
