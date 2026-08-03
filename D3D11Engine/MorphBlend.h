#pragma once
#include "pch.h"

class zCMorphMesh;
class zCMorphMeshAni;

/** Reimplementation of zCMorphMesh::CalcVertPositions from captured channel state, so the morph blend
    can be moved off the game thread (and, next, onto the GPU) instead of calling into ZENGIN's own
    deform every animation frame.
 *
 *  What CANNOT move: zCMorphMesh::AdvanceAnis. It is game state, not geometry - it integrates channel
 *  weights against frame time, runs the fade-in/const/fade-out machine, deletes finished channels,
 *  advances frames and rolls zRandF() for discontinuity anis. It keeps being called; only the deform
 *  is reproduced here.
 *
 *  The blend is a SEQUENTIAL FOLD, not a weighted sum:
 *      acc = weight1M * acc + sample * weight     (per channel, in channel order)
 *  Each channel attenuates everything accumulated before it, and a "shape" ani forces weight1M to 1.
 *  So the channel order is load-bearing and the result cannot be precomputed as rest + sum(w*delta).
 *  A channel that does not touch a given vertex is skipped entirely, leaving that vertex's accumulator
 *  untouched - which is what makes the sparse vertIndexList indirection safe to invert. */
namespace MorphBlend {

    /** One channel, snapshotted on the game thread. Everything a consumer needs to fold it, with the
        engine's easing already applied - the arrays it points at are written once at load. */
    struct ChannelState {
        zCMorphMeshAni* Ani = nullptr;
        const float3* FrameA = nullptr;   // vertPosMatrix + actFrameInt * numVert
        const float3* FrameB = nullptr;   // vertPosMatrix + nextFrameInt * numVert (== FrameA if 1 frame)
        const int* VertIndexList = nullptr;
        int NumVert = 0;
        float Frac = 0.0f;                // FrameA -> FrameB lerp, 0 when the ani has a single frame
        float Weight = 0.0f;              // zSinusEase applied twice, as CalcVertPositions does
        float Weight1M = 0.0f;            // 1 - Weight, or exactly 1 for a shape ani
    };

    /** ZENGIN's zSinusEase: (zSinApprox(t*PI - PI/2) + 1) / 2, applied twice by CalcVertPositions.
        zSinApprox is a 1-milliradian quantised sine, not sin() - see the implementation. */
    float SinusEase( float t );

    /** Snapshots this morph mesh's active channels. Must run on the game thread, after AdvanceAnis:
        the entries are owned by aniChannels and can be deleted by the next AdvanceAnis call. */
    void CaptureChannels( zCMorphMesh* mm, std::vector<ChannelState>& outChannels );

    /** Folds the captured channels over 'restPositions' and writes numVert positions to 'outPositions'.
        Reproduces CalcVertPositions exactly, including the trailing add of the rest base. */
    void Apply( const std::vector<ChannelState>& channels, const float3* restPositions, int numVert,
        float3* outPositions );

    /** Runs Apply against ZENGIN's own freshly-computed positions and returns the largest per-component
        deviation, or -1.0f if the comparison could not be made. Diagnostic only - this is the check that
        has to pass before the fold is worth porting to a shader. */
    float CompareAgainstEngine( zCMorphMesh* mm );
}
