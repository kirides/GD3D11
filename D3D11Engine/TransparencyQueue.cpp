#include "pch.h"
#include "TransparencyQueue.h"
#include "GothicAPI.h"

#include <algorithm>

namespace {
    /** Pre-queue pass order, for the categoryMajor fallback. Approximate: quad marks used to be
        split over two passes by blend mode; they are one category here. */
    constexpr uint8_t LegacyRank( ETransparentKind kind ) {
        switch ( kind ) {
        case ETransparentKind::AlphaVob:  return 0;
        case ETransparentKind::Ghost:     return 1;
        case ETransparentKind::Decal:     return 2;
        case ETransparentKind::QuadMark:  return 3;
        case ETransparentKind::PolyStrip: return 4;
        case ETransparentKind::WorldMesh: return 5;
        default:                          return 6;
        }
    }

    /** Back to front, then grouped by material so equidistant items still batch. */
    bool BackToFront( const TransparentItem& a, const TransparentItem& b ) {
        if ( a.DistanceSq != b.DistanceSq )
            return a.DistanceSq > b.DistanceSq;
        if ( a.Kind != b.Kind )
            return a.Kind < b.Kind;
        if ( a.BatchKey != b.BatchKey )
            return a.BatchKey < b.BatchKey;
        return a.PayloadIndex < b.PayloadIndex;
    }
}

void TransparencyQueue::BeginFrame() {
    if ( Items.capacity() == 0 ) {
        Items.reserve( 2048 );
        WorldMeshes.reserve( 512 );
        AlphaVobs.reserve( 1024 );
        GhostIndices.reserve( 32 );
        Decals.reserve( 256 );
        QuadMarks.reserve( 128 );
        PolyStrips.reserve( 32 );
    }

    Items.clear();
    WorldMeshes.clear();
    AlphaVobs.clear();
    GhostIndices.clear();
    Decals.clear();
    QuadMarks.clear();
    PolyStrips.clear();
}

void TransparencyQueue::AddWorldMesh( float distanceSq, EWorldTransparencyVariant variant, const MeshKey& key, MeshInfo* mesh ) {
    Items.push_back( { distanceSq, MakeBatchKey( key.Material ), static_cast<uint32_t>(WorldMeshes.size()),
        ETransparentKind::WorldMesh, static_cast<uint8_t>(variant) } );
    WorldMeshes.push_back( { key, mesh } );
}

void TransparencyQueue::AddAlphaVob( float distanceSq, uint32_t batchIndex, uint32_t instanceIndex, uint32_t batchKey ) {
    AddIndexed( distanceSq, ETransparentKind::AlphaVob, 0, batchIndex, instanceIndex, batchKey );
}

void TransparencyQueue::AddIndexed( float distanceSq, ETransparentKind kind, uint8_t subKind,
    uint32_t index0, uint32_t index1, uint32_t batchKey ) {
    Items.push_back( { distanceSq, batchKey, static_cast<uint32_t>(AlphaVobs.size()), kind, subKind } );
    AlphaVobs.push_back( { index0, index1 } );
}

void TransparencyQueue::AddGhost( float distanceSq, uint32_t transparencyVobIndex ) {
    Items.push_back( { distanceSq, transparencyVobIndex, static_cast<uint32_t>(GhostIndices.size()),
        ETransparentKind::Ghost, 0 } );
    GhostIndices.push_back( transparencyVobIndex );
}

void TransparencyQueue::AddDecal( float distanceSq, zCVob* vob ) {
    Items.push_back( { distanceSq, MakeBatchKey( vob ), static_cast<uint32_t>(Decals.size()),
        ETransparentKind::Decal, 0 } );
    Decals.push_back( vob );
}

void TransparencyQueue::AddQuadMark( float distanceSq, zCQuadMark* mark, const QuadMarkInfo* info ) {
    Items.push_back( { distanceSq, MakeBatchKey( mark ), static_cast<uint32_t>(QuadMarks.size()),
        ETransparentKind::QuadMark, 0 } );
    QuadMarks.push_back( { mark, info } );
}

void TransparencyQueue::AddPolyStrip( float distanceSq, zCTexture* texture, const PolyStripInfo* info ) {
    Items.push_back( { distanceSq, MakeBatchKey( texture ), static_cast<uint32_t>(PolyStrips.size()),
        ETransparentKind::PolyStrip, 0 } );
    PolyStrips.push_back( { texture, info } );
}

void TransparencyQueue::Sort( bool categoryMajor ) {
    if ( categoryMajor ) {
        std::sort( Items.begin(), Items.end(), []( const TransparentItem& a, const TransparentItem& b ) {
            const uint8_t ra = LegacyRank( a.Kind );
            const uint8_t rb = LegacyRank( b.Kind );
            if ( ra != rb )
                return ra < rb;
            return BackToFront( a, b );
            } );
        return;
    }

    std::sort( Items.begin(), Items.end(), BackToFront );
}

float TransparencyQueue::DistanceSqFromCamera( const XMFLOAT3& worldPosition ) {
    float distanceSq;
    XMStoreFloat( &distanceSq,
        XMVector3LengthSq( XMLoadFloat3( &worldPosition ) - Engine::GAPI->GetCameraPositionXM() ) );
    return distanceSq;
}
