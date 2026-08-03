#pragma once
#include "pch.h"
#include <DirectXMath.h>
#include <vector>

#include <unordered_map>

class zCBspTree;
class zCBspBase;
class zCBspSector;
class zCPolygon;
class zCVob;
struct BspInfo;

/** Screen-space (NDC, [-1..1]) axis-aligned box. Default-constructed state is EMPTY. */
struct ScreenBox2D {
    float MinX = 1.0f;
    float MinY = 1.0f;
    float MaxX = -1.0f;
    float MaxY = -1.0f;

    bool IsEmpty() const { return MinX > MaxX || MinY > MaxY; }

    static ScreenBox2D FullViewport() { return ScreenBox2D{ -1.0f, -1.0f, 1.0f, 1.0f }; }

    void Add( float x, float y ) {
        MinX = std::min( MinX, x ); MinY = std::min( MinY, y );
        MaxX = std::max( MaxX, x ); MaxY = std::max( MaxY, y );
    }

    /** Union - used when a sector is reached through a second portal chain. */
    void Merge( const ScreenBox2D& o ) {
        if ( o.IsEmpty() ) return;
        MinX = std::min( MinX, o.MinX ); MinY = std::min( MinY, o.MinY );
        MaxX = std::max( MaxX, o.MaxX ); MaxY = std::max( MaxY, o.MaxY );
    }

    /** Intersection. May produce an empty box. */
    ScreenBox2D ClippedTo( const ScreenBox2D& o ) const {
        return ScreenBox2D{ std::max( MinX, o.MinX ), std::max( MinY, o.MinY ),
                            std::min( MaxX, o.MaxX ), std::min( MaxY, o.MaxY ) };
    }

    bool Overlaps( const ScreenBox2D& o ) const {
        return !(MinX > o.MaxX || MaxX < o.MinX || MinY > o.MaxY || MaxY < o.MinY);
    }

    bool Contains( const ScreenBox2D& o ) const {
        return o.IsEmpty() || (MinX <= o.MinX && MinY <= o.MinY && MaxX >= o.MaxX && MaxY >= o.MaxY);
    }
};

/** Sector/portal visibility for portal-compiled worlds.
 *
 *  GD3D11 detours zCBspTree::Render wholesale (see zCBspTree::hooked_zCBspNodeRender), so the
 *  original engine's sector activation never runs and every indoor VOB within IndoorVobDrawRadius
 *  is collected regardless of whether its room can be seen at all. This class reimplements the
 *  parts of that algorithm that matter for culling, reading the same compiled data:
 *
 *   - zCBspTree::sectorList  -> the rooms
 *   - zCBspSector::sectorNodes / sectorPortals
 *   - zCMaterial::bspSectorFront/Back on each portal poly -> what it connects
 *
 *  Per frame it reproduces zCBspSector::ActivateSectorRec: starting either from the camera's own
 *  sector or, when the camera is outdoors, from every outdoor->indoor portal that survives backface
 *  + frustum tests, it flood-fills the sector graph carrying an intersected screen-space aperture.
 *  Only VOBs of sectors reached that way are worth collecting.
 *
 *  Everything here is read-only w.r.t. the game's data and is built once per world load.
 *  Worlds compiled without portals (empty sectorList) leave IsActive() false and the whole
 *  mechanism disables itself. */
class BspPortalCuller {
public:
    /** No sector - i.e. plain outdoor. Matches zSECTOR_INDEX_UNDEF's role, not its value. */
    static constexpr uint16_t SECTOR_OUTDOOR = 0xFFFF;

    /** Extracts the sector/portal graph and stamps BspInfo::SectorIds. Safe to call on any world;
        does nothing for indoor-mode worlds or worlds without sectors.
        Must run AFTER the BspInfo mirror tree exists (GothicAPI::BuildBspVobMapCache). */
    void BuildFromWorld( zCBspTree* tree );

    void Clear();

    /** True when this world has usable sector data and culling is enabled by the user. */
    bool IsActive() const { return Enabled && !Sectors.empty(); }

    void SetEnabled( bool enabled ) { Enabled = enabled; }

    /** Sectors whose bounds are within this distance of the camera are activated unconditionally,
        with a full-screen aperture. Absorbs the cases the portal walk cannot express: standing just
        outside a doorway, doors modelled as several polys, rooms whose entry portal is degenerate.
        Costs almost nothing to raise - nearby rooms are few. In Gothic units (100 = 1m). */
    void SetNearSectorRadius( float units ) { NearSectorRadius = units; }

    /** Runs one visibility solve for the given camera. Cheap (a few hundred quad projections);
        call once per frame before collecting VOBs. */
    void Solve( DirectX::FXMMATRIX worldToClip, const DirectX::XMFLOAT3& cameraPosition, zCVob* cameraVob );

    /** True if any sector this leaf belongs to is visible this frame. Leafs with no sector are
        outdoor and always pass. */
    bool IsLeafVisible( const BspInfo& leaf ) const;

    /** Second-level cut: does this world-space AABB reach the screen aperture of any of the leaf's
        visible sectors? Mirrors zCCamera::ScreenProjectionTouchesPortal. */
    bool IsBoxVisibleInLeafSectors( const BspInfo& leaf, const DirectX::XMFLOAT3& bbMin, const DirectX::XMFLOAT3& bbMax ) const;

    /** Can any OUTDOOR geometry be on screen this frame? This is GD3D11's stand-in for
     *  zCBspSector::IsOutdoorActive(), which the detoured traversal never gets to compute.
     *
     *  False only for a fully enclosed view: the camera stands in a room and no opening to the outdoor
     *  is reachable through a chain of visible portals, so no surface the sun lights is on screen and
     *  the cascades can be skipped. Note this is NOT Stats::CameraOutdoor, which merely says the
     *  camera's BSP leaf touches a sector and is false on an open beach next to a hut.
     *
     *  Conservative: true when culling is inactive or the camera's room cannot be resolved. */
    bool IsOutdoorVisible() const { return !IsActive() || OutdoorVisible; }

    /** Diagnostics for the ImGui overlay / Tracy. */
    struct Stats {
        int NumSectors = 0;
        int NumPortals = 0;
        int ActiveSectors = 0;
        /** Sectors unreachable from the outdoor - permanently uncullable. A non-zero count means
            the world's portal data does not fully describe how its rooms are entered. */
        int UnreachableSectors = 0;
        bool CameraOutdoor = true;
        uint16_t CameraSector = SECTOR_OUTDOOR;
        /** Mirrors IsOutdoorVisible() for this frame. False means the sun cascades were skippable. */
        bool OutdoorVisible = true;

        // --- Enclosure-test diagnostics: why the sun cascades were or were not skipped -------------
        /** Room FindSectorBelow resolved, or SECTOR_OUTDOOR if none. */
        uint16_t EnclosedInSector = SECTOR_OUTDOOR;
        /** Room whose opening to the outdoor ended the walk. Set only when OutdoorVisible is true. */
        uint16_t OutdoorSeenFromSector = SECTOR_OUTDOOR;
        /** Rooms the enclosure walk reached before deciding. */
        int EnclosureWalkSectors = 0;
    };
    const Stats& GetStats() const { return LastStats; }

private:
    /** One traversable portal poly, pre-baked at world load. */
    struct Portal {
        std::vector<DirectX::XMFLOAT3> Verts;
        DirectX::XMFLOAT3 PlaneNormal{};
        float PlaneDistance = 0.0f;
        /** Sector this portal leads INTO, or SECTOR_OUTDOOR. */
        uint16_t TargetSector = SECTOR_OUTDOOR;
    };

    struct Sector {
        /** Indices into Portals of the portals traversable when standing inside this sector. */
        std::vector<uint32_t> OutgoingPortals;
        /** The subset of OutdoorEntryPortals targeting this sector. Their material has a null front,
            so they appear in no sector's OutgoingPortals and a room whose doors are all stored that
            way would look sealed from the inside. */
        std::vector<uint32_t> IncomingOutdoorPortals;
        /** Union of this sector's leaf AABBs - used for the near-camera grace radius. */
        DirectX::XMFLOAT3 BoundsMin{ FLT_MAX, FLT_MAX, FLT_MAX };
        DirectX::XMFLOAT3 BoundsMax{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
        bool HasBounds = false;
        /** Set for sectors that no chain of portals can reach from the outdoor. Such a room could
            never be activated correctly, so it is never culled either. See BuildReachability. */
        bool AlwaysActive = false;
    };

    /** Projects a portal/box polygon and returns its NDC bounding box, clipped to the viewport.
        Returns an empty box when the polygon is fully behind the camera. */
    static ScreenBox2D ProjectPolygon( DirectX::FXMMATRIX worldToClip, const DirectX::XMFLOAT3* verts, size_t numVerts );

    void ActivateSector( uint16_t sector, const ScreenBox2D& aperture, uint16_t cameFrom, int depth );

    /** Descends the BSP by position, like zCBspBase::FindLeaf. */
    zCBspBase* FindLeaf( const DirectX::XMFLOAT3& position ) const;

    /** The room the camera stands in, or SECTOR_OUTDOOR - reproducing what zCBspTree::Render does to
        pick its start sector: cast a ray straight down (portals included) and look at the nearest poly
        below that carries the sector flag. See FindSectorBelow for why nothing cheaper works. */
    uint16_t FindSectorBelow( const DirectX::XMFLOAT3& position ) const;

    /** Recursive half of FindSectorBelow: nearest sector-flagged poly under `position`. */
    void TraceSectorPolyDown( zCBspBase* node, const DirectX::XMFLOAT3& position,
        float& bestY, zCPolygon*& bestPoly ) const;

    /** Marks every sector no chain of portals can reach from the outdoor as AlwaysActive. */
    void BuildReachability();

    /** Backs IsOutdoorVisible(); evaluated once per Solve(). */
    bool ComputeOutdoorVisible( uint16_t fromSector );

    bool IsSectorActive( uint16_t s ) const {
        return s < Stamps.size() && (Stamps[s] == CurrentStamp || Sectors[s].AlwaysActive);
    }

    std::vector<Portal> Portals;
    std::vector<Sector> Sectors;
    /** zCBspSector* -> our index. Authoritative: we never trust zCBspSector::sectorIndex to agree
        with the position in zCBspTree::sectorList. */
    std::unordered_map<zCBspSector*, uint16_t> SectorIdByPtr;
    /** Root of the world BSP, for FindLeaf. Valid between BuildFromWorld and Clear. */
    zCBspBase* BspRoot = nullptr;
    /** Portals leading from the outdoor into a sector - the "doors" seen from outside. */
    std::vector<uint32_t> OutdoorEntryPortals;

    /** Frame-stamped activation state, avoids clearing per frame. */
    std::vector<uint32_t> Stamps;
    std::vector<ScreenBox2D> Apertures;
    uint32_t CurrentStamp = 0;

    /** Set for the duration of one Solve(). */
    DirectX::XMMATRIX SolveWorldToClip{};
    DirectX::XMFLOAT3 SolveCameraPos{};
    int VisitBudget = 0;
    bool WarnedBudget = false;

    bool Enabled = true;
    /** Result of the last Solve(). Starts true so a world that never solves is never treated as
        enclosed. */
    bool OutdoorVisible = true;
    float NearSectorRadius = 2500.0f; // ~25m
    Stats LastStats;
};
