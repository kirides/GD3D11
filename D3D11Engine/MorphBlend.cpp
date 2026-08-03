#include "pch.h"
#include "MorphBlend.h"
#include "zCMorphMesh.h"

namespace MorphBlend {

    /** ZENGIN's zSinApprox is a 1-milliradian lookup table, NOT sin(): it quantises the argument to
        1/1000 rad (zFloat2Int == fistp, round-to-nearest-even), wraps it into [-3.142, +3.142] and
        reads a precomputed sine. Reproducing the quantisation matters - using a true sin() here leaves
        a ~0.0025-unit disagreement with the engine on a head, which is invisible but would sit as a
        noise floor under VerifyMorphBlend and hide small regressions. Index 3142 is a == 0, so the
        table entry is sin((index - 3142) / 1000). */
    static float SinApprox( float a ) {
        const int lookup = static_cast<int>( lrintf( 1000.0f * a ) );
        const int index = (lookup + 3142 + 6284 * 1000) % 6284;
        return sinf( static_cast<float>( index - 3142 ) * 0.001f );
    }

    float SinusEase( float t ) {
        constexpr float kPi = 3.14159265358979323846f;
        return (SinApprox( t * kPi - kPi * 0.5f ) + 1.0f) * 0.5f;
    }

    void CaptureChannels( zCMorphMesh* mm, std::vector<ChannelState>& outChannels ) {
        outChannels.clear();
        if ( !mm ) {
            return;
        }

        const int numChannels = mm->GetNumAniChannels();
        if ( numChannels <= 0 ) {
            return;
        }

        outChannels.reserve( numChannels );
        for ( int i = 0; i < numChannels; i++ ) {
            zTMorphAniEntry* entry = mm->GetAniChannel( i );
            if ( !entry ) {
                continue;
            }
            zCMorphMeshAni* ani = entry->GetAni();
            if ( !ani ) {
                continue;
            }

            const int aniNumVert = ani->GetNumVert();
            const float3* vertPos = ani->GetVertPosMatrix();
            const int* vertIndexList = ani->GetVertIndexList();
            if ( aniNumVert <= 0 || !vertPos || !vertIndexList ) {
                continue;
            }

            ChannelState& c = outChannels.emplace_back();
            c.Ani = ani;
            c.NumVert = aniNumVert;
            c.VertIndexList = vertIndexList;

            const float weight = SinusEase( SinusEase( entry->GetWeight() ) );
            c.Weight = weight;
            c.Weight1M = ani->IsShape() ? 1.0f : (1.0f - weight);

            const int numFrames = ani->GetNumFrames();
            if ( numFrames > 1 ) {
                c.FrameA = vertPos + static_cast<ptrdiff_t>(entry->GetActFrameInt()) * aniNumVert;
                c.FrameB = vertPos + static_cast<ptrdiff_t>(entry->GetNextFrameInt()) * aniNumVert;
                c.Frac = entry->GetFrac();
            } else {
                // Single-frame anis take CalcVertPositions' non-lerping branch.
                c.FrameA = vertPos;
                c.FrameB = vertPos;
                c.Frac = 0.0f;
            }
        }
    }

    void Apply( const std::vector<ChannelState>& channels, const float3* restPositions, int numVert,
        float3* outPositions ) {
        if ( !outPositions || numVert <= 0 ) {
            return;
        }

        for ( int i = 0; i < numVert; i++ ) {
            outPositions[i] = float3( 0.0f, 0.0f, 0.0f );
        }

        for ( const ChannelState& c : channels ) {
            for ( int j = 0; j < c.NumVert; j++ ) {
                const int v = c.VertIndexList[j];
                if ( v < 0 || v >= numVert ) {
                    continue;   // an ani wider than the mesh we are folding onto - skip rather than scribble
                }

                float3 sample = c.FrameA[j];
                if ( c.Frac != 0.0f ) {
                    const float3& next = c.FrameB[j];
                    sample.x += c.Frac * (next.x - sample.x);
                    sample.y += c.Frac * (next.y - sample.y);
                    sample.z += c.Frac * (next.z - sample.z);
                }

                float3& acc = outPositions[v];
                acc.x = c.Weight1M * acc.x + sample.x * c.Weight;
                acc.y = c.Weight1M * acc.y + sample.y * c.Weight;
                acc.z = c.Weight1M * acc.z + sample.z * c.Weight;
            }
        }

        // CalcVertPositions' trailing add of the rest base.
        if ( restPositions ) {
            for ( int i = 0; i < numVert; i++ ) {
                outPositions[i].x += restPositions[i].x;
                outPositions[i].y += restPositions[i].y;
                outPositions[i].z += restPositions[i].z;
            }
        }
    }

    void LogPrototypeBudget( zCMorphMesh* mm ) {
        if ( !mm ) {
            return;
        }
        zCMorphMeshProto* proto = mm->GetMorphProto();
        if ( !proto ) {
            return;
        }

        // Once per .MMS, on the game thread (same context as the rest of UpdateMorphMeshVisual), so no
        // lock. Bounded by the number of distinct morph prototypes the world loads - a handful of head
        // meshes - which is why this does not need a settings switch.
        static std::unordered_set<void*> s_seen;
        if ( !s_seen.insert( proto ).second ) {
            return;
        }

        zCProgMeshProto* ref = proto->GetMorphRefMesh();
        zCArrayAdapt<float3>* refPos = ref ? ref->GetPositionList() : nullptr;
        const int meshNumVert = refPos ? refPos->NumInArray : 0;

        const int numAnis = proto->GetNumAnis();
        size_t vertPosBytes = 0;    // what a GPU port would upload: all anis' vertPosMatrix concatenated
        size_t indexBytes = 0;      // the sparse vertIndexList we would upload alongside it
        size_t totalFrames = 0;
        size_t widestAniBytes = 0;
        for ( int i = 0; i < numAnis; i++ ) {
            zCMorphMeshAni* ani = proto->GetAni( i );
            if ( !ani ) {
                continue;
            }
            const int aniNumVert = ani->GetNumVert();
            const int numFrames = ani->GetNumFrames();
            if ( aniNumVert <= 0 || numFrames <= 0 ) {
                continue;
            }
            const size_t bytes = static_cast<size_t>(numFrames) * aniNumVert * sizeof( float3 );
            vertPosBytes += bytes;
            indexBytes += static_cast<size_t>(aniNumVert) * sizeof( int );
            totalFrames += numFrames;
            widestAniBytes = std::max( widestAniBytes, bytes );
        }

        // The gather-friendly inversion of vertIndexList the compute fold needs: one dense
        // vertex -> slot table per ani, meshNumVert uints wide.
        const size_t inverseTableBytes = static_cast<size_t>(numAnis) * meshNumVert * sizeof( uint32_t );

        static size_t s_totalVertPosBytes = 0;
        static size_t s_totalInverseBytes = 0;
        s_totalVertPosBytes += vertPosBytes;
        s_totalInverseBytes += inverseTableBytes;

        LogInfo() << "MorphBlend budget: " << (proto->GetName() ? proto->GetName()->ToChar() : "?")
            << " " << numAnis << " anis, " << totalFrames << " frames, " << meshNumVert << " mesh verts"
            << " | vertPosMatrix " << (vertPosBytes / 1024) << " KB (widest ani "
            << (widestAniBytes / 1024) << " KB), vertIndexList " << (indexBytes / 1024) << " KB"
            << ", inverse tables " << (inverseTableBytes / 1024) << " KB"
            << " | running total: vertPosMatrix " << (s_totalVertPosBytes / 1024) << " KB + inverse "
            << (s_totalInverseBytes / 1024) << " KB over " << s_seen.size() << " prototypes";
    }

    float CompareAgainstEngine( zCMorphMesh* mm ) {
        if ( !mm ) {
            return -1.0f;
        }

        int restCount = 0;
        const float3* rest = mm->GetRestPositions( restCount );
        zCProgMeshProto* pm = mm->GetMorphMesh();
        if ( !rest || restCount <= 0 || !pm ) {
            return -1.0f;
        }

        zCArrayAdapt<float3>* posList = pm->GetPositionList();
        if ( !posList || !posList->Array || posList->NumInArray <= 0 ) {
            return -1.0f;
        }
        const int numVert = std::min( restCount, posList->NumInArray );

        // Capture first, then let the engine deform into its own (shared) position list. Order matters:
        // CalcVertexPositions does not touch channel state, but capturing after keeps the two reads of
        // that state adjacent and makes the comparison independent of any future reordering here.
        static std::vector<ChannelState> channels;
        CaptureChannels( mm, channels );

        mm->CalcVertexPositions();

        static std::vector<float3> ours;
        ours.resize( numVert );
        Apply( channels, rest, numVert, ours.data() );

        const float3* theirs = posList->Array;
        float worst = 0.0f;
        for ( int i = 0; i < numVert; i++ ) {
            worst = std::max( worst, std::abs( ours[i].x - theirs[i].x ) );
            worst = std::max( worst, std::abs( ours[i].y - theirs[i].y ) );
            worst = std::max( worst, std::abs( ours[i].z - theirs[i].z ) );
        }
        return worst;
    }
}
