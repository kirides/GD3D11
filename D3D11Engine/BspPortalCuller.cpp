#include "pch.h"
#include "BspPortalCuller.h"

#include "GothicAPI.h"
#include "Engine.h"
#include "Logger.h"
#include "zCBspTree.h"
#include "zCMaterial.h"
#include "zCPolygon.h"
#include "zCVob.h"

using namespace DirectX;

namespace {
    /** ZenGin caps sector recursion at 40 (zCBspSector::ActivateSectorRec). Non-convex sectors
        produce cyclic portal chains, so a hard cap is not optional. */
    constexpr int MAX_SECTOR_DEPTH = 40;

    /** Upper bound on sector activations per frame. Reaching it means the world's portal graph is
        pathological; we stop expanding (and log once) rather than truncating silently. */
    constexpr int MAX_SECTOR_VISITS = 4096;

    /** Clip-space w below this is treated as behind the camera. */
    constexpr float CLIP_W_EPSILON = 1e-4f;
}

void BspPortalCuller::Clear() {
    Portals.clear();
    Sectors.clear();
    SectorIdByPtr.clear();
    BspRoot = nullptr;
    OutdoorEntryPortals.clear();
    Stamps.clear();
    Apertures.clear();
    CurrentStamp = 0;
    WarnedBudget = false;
    OutdoorVisible = true;
    LastStats = Stats{};
}

void BspPortalCuller::BuildFromWorld( zCBspTree* tree ) {
    Clear();

    if ( !tree )
        return;

    // Indoor-mode worlds (dungeons) never classify anything as an indoor VOB in the first place
    // (see GothicAPI::BuildBspVobMapCacheHelper), so there is nothing here to gate.
    if ( tree->GetBspTreeMode() != zBSP_MODE_OUTDOOR )
        return;

    zCArray<zCBspSector*>& sectorList = tree->GetSectorList();
    if ( sectorList.NumInArray <= 0 || !sectorList.Array )
        return;

    const int numSectors = sectorList.NumInArray;
    if ( numSectors > SECTOR_OUTDOOR ) {
        LogWarn() << "BspPortalCuller: world has " << numSectors
            << " sectors, more than the 16-bit sector id space allows - portal culling disabled";
        return;
    }

    Sectors.resize( numSectors );
    BspRoot = tree->GetRootNode();

    // Key sectors by pointer, not by their own sectorIndex field: a disagreement between the two
    // would silently mis-address our arrays and cull the room the player is standing in.
    SectorIdByPtr.reserve( numSectors * 2 );
    for ( int i = 0; i < numSectors; i++ ) {
        if ( sectorList.Array[i] )
            SectorIdByPtr.emplace( sectorList.Array[i], static_cast<uint16_t>(i) );
    }

    auto idOf = [&]( zCBspSector* s ) -> uint16_t {
        if ( !s ) return SECTOR_OUTDOOR;
        auto it = SectorIdByPtr.find( s );
        return it != SectorIdByPtr.end() ? it->second : SECTOR_OUTDOOR;
    };

    int numLeafsTagged = 0;
    for ( int i = 0; i < numSectors; i++ ) {
        zCBspSector* sector = sectorList.Array[i];
        if ( !sector ) continue;
        const uint16_t sectorId = static_cast<uint16_t>(i);

        // --- Leafs of this sector -> BspInfo::SectorIds -------------------------------------
        zCArray<zCBspBase*>& nodes = sector->GetSectorNodes();
        for ( int n = 0; n < nodes.NumInArray; n++ ) {
            zCBspBase* leaf = nodes.Array[n];
            if ( !leaf ) continue;

            BspInfo* info = Engine::GAPI->GetNewBspNode( leaf );
            if ( !info ) continue;

            // A leaf can hold polys of several sectors; keep all of them (visible if ANY is).
            if ( std::find( info->SectorIds.begin(), info->SectorIds.end(), sectorId ) == info->SectorIds.end() ) {
                info->SectorIds.push_back( sectorId );
                numLeafsTagged++;
            }

            Sector& s = Sectors[sectorId];
            s.BoundsMin.x = std::min( s.BoundsMin.x, leaf->BBox3D.Min.x );
            s.BoundsMin.y = std::min( s.BoundsMin.y, leaf->BBox3D.Min.y );
            s.BoundsMin.z = std::min( s.BoundsMin.z, leaf->BBox3D.Min.z );
            s.BoundsMax.x = std::max( s.BoundsMax.x, leaf->BBox3D.Max.x );
            s.BoundsMax.y = std::max( s.BoundsMax.y, leaf->BBox3D.Max.y );
            s.BoundsMax.z = std::max( s.BoundsMax.z, leaf->BBox3D.Max.z );
            s.HasBounds = true;
        }

        // --- Portals of this sector ---------------------------------------------------------
        zCArray<zCPolygon*>& portals = sector->GetSectorPortals();
        for ( int p = 0; p < portals.NumInArray; p++ ) {
            zCPolygon* poly = portals.Array[p];
            if ( !poly ) continue;

            zCMaterial* mat = poly->GetMaterial();
            if ( !mat ) continue;

            const uint8_t numVerts = poly->GetNumPolyVertices();
            zCVertex** verts = poly->getVertices();
            if ( numVerts < 3 || !verts ) continue;

            Portal portal;
            portal.Verts.reserve( numVerts );
            bool vertsOk = true;
            for ( uint8_t v = 0; v < numVerts; v++ ) {
                if ( !verts[v] ) { vertsOk = false; break; }
                portal.Verts.push_back( verts[v]->Position );
            }
            if ( !vertsOk ) continue;

            const zTPlane& plane = poly->GetPolyPlane();
            portal.PlaneNormal = plane.Normal;
            portal.PlaneDistance = plane.Distance;

            zCBspSector* front = mat->GetBspSectorFront();
            zCBspSector* back = mat->GetBspSectorBack();

            if ( !front ) {
                // Outdoor(front) -> indoor(back) door. zCBspBase::RenderNodeOutdoor activates the
                // BACK sector through these; zCBspSector::ActivateSectorRec explicitly skips them,
                // so they are entry points only, never traversed from the inside.
                portal.TargetSector = idOf( back );
                if ( portal.TargetSector == SECTOR_OUTDOOR ) continue;

                const uint32_t entryIdx = static_cast<uint32_t>(Portals.size());
                OutdoorEntryPortals.push_back( entryIdx );
                // Also file it under the room it opens into, so the enclosure walk can find the
                // openings of a room whose doors are all stored from the outdoor side.
                Sectors[portal.TargetSector].IncomingOutdoorPortals.push_back( entryIdx );
                Portals.push_back( std::move( portal ) );
            } else {
                // Traversable from inside `front` (which is this sector) towards `back`.
                // A null back means the portal opens into the outdoor.
                portal.TargetSector = idOf( back );

                const uint32_t portalIdx = static_cast<uint32_t>(Portals.size());
                Portals.push_back( std::move( portal ) );

                // Register on the sector the poly actually belongs to, not blindly on `i`:
                // CreateBspSectors2 files a portal under its FRONT sector, but be defensive.
                const uint16_t owner = idOf( front );
                Sectors[owner != SECTOR_OUTDOOR ? owner : sectorId].OutgoingPortals.push_back( portalIdx );
            }
        }
    }

    Stamps.assign( numSectors, 0 );
    Apertures.assign( numSectors, ScreenBox2D{} );
    CurrentStamp = 0;

    BuildReachability();

    LastStats.NumSectors = numSectors;
    LastStats.NumPortals = static_cast<int>(Portals.size());

    LogInfo() << "BspPortalCuller: " << numSectors << " sectors, " << Portals.size()
        << " portals (" << OutdoorEntryPortals.size() << " outdoor entries), "
        << numLeafsTagged << " leaf/sector links";
    if ( LastStats.UnreachableSectors > 0 ) {
        LogWarn() << "BspPortalCuller: " << LastStats.UnreachableSectors << " of " << numSectors
            << " sectors cannot be reached from the outdoor through any portal chain - those are"
            " never culled. Portal data of this world is incomplete for culling purposes.";
    }
}

/** A sector the outdoor cannot reach through portals would be invisible forever once the camera
    steps outside it, because nothing would ever activate it. Rather than guess why the world's
    portal data says so, mark those rooms uncullable - the cost is bounded and correctness is not. */
void BspPortalCuller::BuildReachability() {
    std::vector<uint16_t> queue;
    std::vector<bool> reached( Sectors.size(), false );

    for ( uint32_t portalIdx : OutdoorEntryPortals ) {
        const uint16_t target = Portals[portalIdx].TargetSector;
        if ( target < Sectors.size() && !reached[target] ) {
            reached[target] = true;
            queue.push_back( target );
        }
    }

    while ( !queue.empty() ) {
        const uint16_t s = queue.back();
        queue.pop_back();
        for ( uint32_t portalIdx : Sectors[s].OutgoingPortals ) {
            const uint16_t target = Portals[portalIdx].TargetSector;
            if ( target < Sectors.size() && !reached[target] ) {
                reached[target] = true;
                queue.push_back( target );
            }
        }
    }

    int unreachable = 0;
    for ( size_t i = 0; i < Sectors.size(); i++ ) {
        if ( !reached[i] ) {
            Sectors[i].AlwaysActive = true;
            unreachable++;
        }
    }
    LastStats.UnreachableSectors = unreachable;
}

zCBspBase* BspPortalCuller::FindLeaf( const XMFLOAT3& position ) const {
    zCBspBase* node = BspRoot;
    int guard = 256; // the tree is balanced; this only protects against corrupt data
    while ( node && !node->IsLeaf() && --guard > 0 ) {
        zCBspNode* n = static_cast<zCBspNode*>(node);
        const float side = n->Plane.Normal.x * position.x
                         + n->Plane.Normal.y * position.y
                         + n->Plane.Normal.z * position.z
                         - n->Plane.Distance;
        zCBspBase* next = side > 0.0f ? n->Front : n->Back;
        if ( !next ) break;
        node = next;
    }
    return (node && node->IsLeaf()) ? node : nullptr;
}

/** Nearest sector-flagged polygon straight below `position`. Every node is tested, not just leafs -
    zCBspBase::PolyList is on the base and inner nodes carry polys too. */
void BspPortalCuller::TraceSectorPolyDown( zCBspBase* node, const XMFLOAT3& position,
    float& bestY, zCPolygon*& bestPoly ) const {
    if ( !node )
        return;

    // Vertical ray: a subtree matters only if it spans XZ, reaches below us, and beats the best hit.
    const zTBBox3D& box = node->BBox3D;
    if ( position.x < box.Min.x || position.x > box.Max.x ) return;
    if ( position.z < box.Min.z || position.z > box.Max.z ) return;
    if ( box.Min.y > position.y ) return;
    if ( box.Max.y < bestY ) return;

    for ( int i = 0; i < node->NumPolys; i++ ) {
        zCPolygon* poly = node->PolyList[i];
        if ( !poly ) continue;

        // zCBspTree::Render's GetSectorFlag test. Portals stay in - a doorway underfoot must resolve.
        if ( !poly->GetPolyFlags()->SectorPoly ) continue;

        const uint8_t numVerts = poly->GetNumPolyVertices();
        zCVertex** verts = poly->getVertices();
        if ( numVerts < 3 || !verts ) continue;

        // A near-vertical poly is a wall, never the floor under the camera - and would divide by ~0.
        const zTPlane& plane = poly->GetPolyPlane();
        if ( std::fabs( plane.Normal.y ) < 1e-4f ) continue;

        // Point-in-polygon in XZ (crossing test), then the plane's height at that XZ.
        bool inside = false;
        for ( uint8_t a = 0, b = numVerts - 1; a < numVerts; b = a++ ) {
            if ( !verts[a] || !verts[b] ) { inside = false; break; }
            const float az = verts[a]->Position.z, bz = verts[b]->Position.z;
            const float ax = verts[a]->Position.x, bx = verts[b]->Position.x;
            if ( (az > position.z) != (bz > position.z)
                && position.x < ax + (bx - ax) * (position.z - az) / (bz - az) ) {
                inside = !inside;
            }
        }
        if ( !inside ) continue;

        const float y = (plane.Distance - plane.Normal.x * position.x - plane.Normal.z * position.z)
            / plane.Normal.y;
        if ( y <= position.y && y > bestY ) {
            bestY = y;
            bestPoly = poly;
        }
    }

    if ( node->IsNode() ) {
        zCBspNode* n = static_cast<zCBspNode*>(node);
        TraceSectorPolyDown( n->Front, position, bestY, bestPoly );
        TraceSectorPolyDown( n->Back, position, bestY, bestPoly );
    }
}

/** Mirrors the start-sector block of zCBspTree::Render. The cheaper substitutes all fail: ZenGin
    warns against GetGroundPoly() here (portals are filtered out of groundPolys, and it is stale on the
    camera vob), while BspInfo::SectorIds names a room from open sand because outdoor leafs straddle
    walls - as do sector bounds, being a union of those leafs' AABBs. */
uint16_t BspPortalCuller::FindSectorBelow( const XMFLOAT3& position ) const {
    float bestY = -FLT_MAX;
    zCPolygon* hitPoly = nullptr;
    TraceSectorPolyDown( BspRoot, position, bestY, hitPoly );

    if ( !hitPoly )
        return SECTOR_OUTDOOR;

    zCMaterial* mat = hitPoly->GetMaterial();
    if ( !mat )
        return SECTOR_OUTDOOR;

    zCBspSector* front = mat->GetBspSectorFront();

    // An indoor=>outdoor portal underfoot means we are standing in a doorway from the outdoor side;
    // ZenGin treats that as outdoor (its front is the outdoor).
    if ( hitPoly->IsPortal() && !front )
        return SECTOR_OUTDOOR;

    if ( !front )
        return SECTOR_OUTDOOR;

    auto it = SectorIdByPtr.find( front );
    return it != SectorIdByPtr.end() ? it->second : SECTOR_OUTDOOR;
}

ScreenBox2D BspPortalCuller::ProjectPolygon( FXMMATRIX worldToClip, const XMFLOAT3* verts, size_t numVerts ) {
    ScreenBox2D box;
    if ( numVerts < 3 ) return box;

    // Sutherland-Hodgman against the w>0 plane only. Vertices outside the side planes still
    // project to valid (out-of-range) NDC, and the final clamp to the viewport handles those;
    // vertices at or behind the eye would produce garbage, so those edges must be split.
    XMVECTOR prev = XMVector4Transform( XMVectorSetW( XMLoadFloat3( &verts[numVerts - 1] ), 1.0f ), worldToClip );
    float prevW = XMVectorGetW( prev );
    bool prevIn = prevW > CLIP_W_EPSILON;

    for ( size_t i = 0; i < numVerts; i++ ) {
        XMVECTOR cur = XMVector4Transform( XMVectorSetW( XMLoadFloat3( &verts[i] ), 1.0f ), worldToClip );
        const float curW = XMVectorGetW( cur );
        const bool curIn = curW > CLIP_W_EPSILON;

        if ( curIn != prevIn ) {
            // Split the edge where w crosses the epsilon plane
            const float t = (CLIP_W_EPSILON - prevW) / (curW - prevW);
            XMVECTOR mid = XMVectorLerp( prev, cur, t );
            const float midW = std::max( XMVectorGetW( mid ), CLIP_W_EPSILON );
            box.Add( XMVectorGetX( mid ) / midW, XMVectorGetY( mid ) / midW );
        }

        if ( curIn ) {
            box.Add( XMVectorGetX( cur ) / curW, XMVectorGetY( cur ) / curW );
        }

        prev = cur;
        prevW = curW;
        prevIn = curIn;
    }

    if ( box.IsEmpty() )
        return box;

    return box.ClippedTo( ScreenBox2D::FullViewport() );
}

void BspPortalCuller::ActivateSector( uint16_t sector, const ScreenBox2D& aperture, uint16_t cameFrom, int depth ) {
    if ( sector >= Sectors.size() || aperture.IsEmpty() )
        return;

    if ( depth > MAX_SECTOR_DEPTH )
        return;

    if ( --VisitBudget < 0 ) {
        if ( !WarnedBudget ) {
            WarnedBudget = true;
            LogWarn() << "BspPortalCuller: sector activation budget (" << MAX_SECTOR_VISITS
                << ") exhausted - portal graph may be cyclic. Culling stays conservative.";
        }
        return;
    }

    const bool firstVisit = Stamps[sector] != CurrentStamp;
    if ( firstVisit ) {
        Stamps[sector] = CurrentStamp;
        Apertures[sector] = aperture;
    } else {
        // Reached again through a different chain. Only re-expand when this chain actually widens
        // the aperture, otherwise we would re-walk the whole subgraph for nothing.
        if ( Apertures[sector].Contains( aperture ) )
            return;
        Apertures[sector].Merge( aperture );
    }

    for ( uint32_t portalIdx : Sectors[sector].OutgoingPortals ) {
        const Portal& portal = Portals[portalIdx];

        // Backface cull: the portal's front normal points into the sector we are standing in,
        // so a visible portal has the camera on its front side (zCPolygon::IsBackfacing).
        const float side = portal.PlaneNormal.x * SolveCameraPos.x
                         + portal.PlaneNormal.y * SolveCameraPos.y
                         + portal.PlaneNormal.z * SolveCameraPos.z
                         - portal.PlaneDistance;
        if ( side < 0.0f )
            continue;

        ScreenBox2D portalBox = ProjectPolygon( SolveWorldToClip, portal.Verts.data(), portal.Verts.size() );
        if ( portalBox.IsEmpty() )
            continue;

        portalBox = portalBox.ClippedTo( aperture );
        if ( portalBox.IsEmpty() )
            continue;

        if ( portal.TargetSector == SECTOR_OUTDOOR )
            continue; // Opens into the outdoor - outdoor VOBs are never sector-gated.

        // Same cycle guard the original uses: never step straight back into the sector we came from.
        if ( portal.TargetSector == cameFrom )
            continue;

        ActivateSector( portal.TargetSector, portalBox, sector, depth + 1 );
    }
}

void BspPortalCuller::Solve( FXMMATRIX worldToClip, const XMFLOAT3& cameraPosition, zCVob* cameraVob ) {
    if ( !IsActive() )
        return;

    ZoneScopedN( "BspPortalCuller::Solve" );

    CurrentStamp++;
    if ( CurrentStamp == 0 ) { // wrapped - stale stamps would read as active
        std::fill( Stamps.begin(), Stamps.end(), 0u );
        CurrentStamp = 1;
    }

    SolveWorldToClip = worldToClip;
    SolveCameraPos = cameraPosition;
    VisitBudget = MAX_SECTOR_VISITS;

    // --- Where is the camera? ----------------------------------------------------------------
    // zCBspTree::Render raycasts straight down for this; the ground poly is the same answer for
    // every case except standing exactly on a portal, which we handle conservatively below.
    uint16_t cameraSector = SECTOR_OUTDOOR;
    bool cameraOutdoor = true;
    bool ambiguous = false;
    /** Strictly resolved room the camera stands in - see the block that fills it. Only the enclosure
        test (ComputeOutdoorVisible) uses this; VOB culling keeps the looser camSectors. */
    uint16_t cameraEnclosedIn = SECTOR_OUTDOOR;

    // Under-culling here is harmless, over-culling empties the room the player is standing in,
    // so the camera's sector is resolved from two independent sources and both are activated.
    static thread_local std::vector<uint16_t> camSectors;
    camSectors.clear();

    if ( !cameraVob ) {
        // No camera vob yet (menu / level transition) - do not cull anything.
        ambiguous = true;
    } else {
        if ( zCPolygon* ground = cameraVob->GetGroundPoly() ) {
            if ( ground->IsPortal() ) {
                // Standing in a doorway: the ground poly cannot name one room. Fall through to
                // the leaf lookup below, which returns every sector touching this spot.
            } else if ( zCMaterial* mat = ground->GetMaterial() ) {
                if ( zCBspSector* front = mat->GetBspSectorFront() ) {
                    auto it = SectorIdByPtr.find( front );
                    if ( it != SectorIdByPtr.end() )
                        camSectors.push_back( it->second );
                }
            }
        }

        // Second source: every sector owning a poly in the camera's own BSP leaf. Catches vobs
        // standing on non-sector geometry (rugs, crates) inside a room, and doorways, where the
        // ground poly alone would wrongly report "outdoor" and cull the room away.
        if ( zCBspBase* leaf = FindLeaf( cameraPosition ) ) {
            if ( BspInfo* info = Engine::GAPI->GetNewBspNode( leaf ) ) {
                for ( uint16_t s : info->SectorIds ) {
                    if ( std::find( camSectors.begin(), camSectors.end(), s ) == camSectors.end() )
                        camSectors.push_back( s );
                }
            }
        }

        if ( !camSectors.empty() ) {
            cameraOutdoor = false;
            cameraSector = camSectors[0];
        }

        // The room the camera actually stands in - enclosure test only, camSectors above stays
        // over-inclusive for VOB culling.
        cameraEnclosedIn = FindSectorBelow( cameraPosition );
    }

    const ScreenBox2D fullScreen = ScreenBox2D::FullViewport();

    // Any room close enough to walk into is seeded outright, in addition to whatever the portal
    // walk finds. The screen aperture of a doorway you are standing next to is a poor predictor of
    // what you can see of the room behind it (you can lean past the frame, doors are split across
    // several polys, and the aperture collapses as soon as a door edge leaves the screen). Seeding
    // rather than stamping matters: these sectors must still propagate into their neighbours.
    if ( !ambiguous && NearSectorRadius > 0.0f ) {
        const float radiusSq = NearSectorRadius * NearSectorRadius;
        for ( size_t i = 0; i < Sectors.size(); i++ ) {
            const Sector& s = Sectors[i];
            if ( !s.HasBounds ) continue;

            const float dx = std::max( 0.0f, std::max( s.BoundsMin.x - cameraPosition.x, cameraPosition.x - s.BoundsMax.x ) );
            const float dy = std::max( 0.0f, std::max( s.BoundsMin.y - cameraPosition.y, cameraPosition.y - s.BoundsMax.y ) );
            const float dz = std::max( 0.0f, std::max( s.BoundsMin.z - cameraPosition.z, cameraPosition.z - s.BoundsMax.z ) );
            if ( dx * dx + dy * dy + dz * dz < radiusSq ) {
                ActivateSector( static_cast<uint16_t>(i), fullScreen, SECTOR_OUTDOOR, 0 );
            }
        }
    }

    if ( ambiguous ) {
        // Conservative fallback, mirrors ZenGin's "CamPos ist keinem Sektor zuordbar" branch:
        // mark every sector active for this frame.
        std::fill( Stamps.begin(), Stamps.end(), CurrentStamp );
        std::fill( Apertures.begin(), Apertures.end(), fullScreen );
    } else if ( !cameraOutdoor ) {
        for ( uint16_t s : camSectors )
            ActivateSector( s, fullScreen, SECTOR_OUTDOOR, 0 );
    } else {
        // Outdoors: seed from every outdoor->indoor door that is facing us and on screen.
        // This is zCBspBase::RenderNodeOutdoor's portal branch, minus the fade handling.
        for ( uint32_t portalIdx : OutdoorEntryPortals ) {
            const Portal& portal = Portals[portalIdx];

            const float side = portal.PlaneNormal.x * cameraPosition.x
                             + portal.PlaneNormal.y * cameraPosition.y
                             + portal.PlaneNormal.z * cameraPosition.z
                             - portal.PlaneDistance;
            if ( side < 0.0f )
                continue;

            ScreenBox2D portalBox = ProjectPolygon( worldToClip, portal.Verts.data(), portal.Verts.size() );
            if ( portalBox.IsEmpty() )
                continue;

            ActivateSector( portal.TargetSector, portalBox, SECTOR_OUTDOOR, 0 );
        }
    }

    // Uncullable rooms get a fresh full-screen aperture every frame, so the per-VOB aperture test
    // can never reject them against a box left over from an earlier frame.
    for ( size_t i = 0; i < Sectors.size(); i++ ) {
        if ( Sectors[i].AlwaysActive ) {
            Stamps[i] = CurrentStamp;
            Apertures[i] = fullScreen;
        }
    }

    // Enclosure test for the sun cascades. Reset the walk's stats so short-circuited frames don't
    // report stale ones.
    LastStats.EnclosedInSector = cameraEnclosedIn;
    LastStats.OutdoorSeenFromSector = SECTOR_OUTDOOR;
    LastStats.EnclosureWalkSectors = 0;
    OutdoorVisible = ambiguous || cameraEnclosedIn == SECTOR_OUTDOOR
        || ComputeOutdoorVisible( cameraEnclosedIn );

    int active = 0;
    for ( uint32_t s : Stamps ) {
        if ( s == CurrentStamp ) active++;
    }
    LastStats.ActiveSectors = active;
    LastStats.CameraOutdoor = cameraOutdoor;
    LastStats.CameraSector = cameraSector;
    LastStats.OutdoorVisible = OutdoorVisible;

    ZoneText( "activeSectors", std::size( "activeSectors" ) - 1 );
    ZoneValue( active );
}

/** Reproduces what zCBspSector::ActivateSectorRec computes for zCBspSector::IsOutdoorActive(): walks
    out from the camera's room propagating apertures, and returns true once an opening to the outdoor
    is reachable through the chain. ZenGin skips RenderOutdoor and the sky when this is false.

    Needs its own walk, not a scan of the sectors Solve() activated: activation seeds every room
    within NearSectorRadius with a full-screen aperture, and ProjectPolygon knows nothing about
    occlusion, so a flat scan called every indoor spot in a dense complex open to the sky.

    Stricter than ZenGin in one place: it also checks IncomingOutdoorPortals, which ActivateSectorRec
    skips. That can only decline a skip, never cause a wrong one. */
bool BspPortalCuller::ComputeOutdoorVisible( uint16_t fromSector ) {
    LastStats.OutdoorSeenFromSector = SECTOR_OUTDOOR;
    LastStats.EnclosureWalkSectors = 0;
    if ( fromSector >= Sectors.size() )
        return true;

    // Own visited/aperture state - must not disturb the Stamps/Apertures the VOB cull reads.
    static thread_local std::vector<uint8_t> visited;
    static thread_local std::vector<ScreenBox2D> apertures;
    static thread_local std::vector<std::pair<uint16_t, uint16_t>> stack;   // sector, cameFrom
    visited.assign( Sectors.size(), 0 );
    apertures.assign( Sectors.size(), ScreenBox2D{} );
    stack.clear();

    const auto reachesOutdoor = [this]( uint32_t portalIdx, const ScreenBox2D& aperture ) {
        const Portal& portal = Portals[portalIdx];
        ScreenBox2D box = ProjectPolygon( SolveWorldToClip, portal.Verts.data(), portal.Verts.size() );
        return !box.IsEmpty() && !box.ClippedTo( aperture ).IsEmpty();
    };

    visited[fromSector] = 1;
    apertures[fromSector] = ScreenBox2D::FullViewport();
    stack.push_back( { fromSector, SECTOR_OUTDOOR } );

    int budget = MAX_SECTOR_VISITS;
    while ( !stack.empty() ) {
        if ( --budget < 0 )
            return true;   // pathological graph - assume the outdoor is reachable rather than guess

        const auto [sector, cameFrom] = stack.back();
        stack.pop_back();
        const ScreenBox2D aperture = apertures[sector];
        LastStats.EnclosureWalkSectors++;

        // Doorways into this room that are stored from the outdoor side.
        for ( uint32_t portalIdx : Sectors[sector].IncomingOutdoorPortals ) {
            if ( reachesOutdoor( portalIdx, aperture ) ) {
                LastStats.OutdoorSeenFromSector = sector;
                return true;
            }
        }

        for ( uint32_t portalIdx : Sectors[sector].OutgoingPortals ) {
            const Portal& portal = Portals[portalIdx];

            if ( portal.TargetSector == SECTOR_OUTDOOR ) {
                if ( reachesOutdoor( portalIdx, aperture ) ) {
                    LastStats.OutdoorSeenFromSector = sector;
                    return true;
                }
                continue;
            }

            // --- traversal into the next room: identical rules to ActivateSector ---
            const float side = portal.PlaneNormal.x * SolveCameraPos.x
                             + portal.PlaneNormal.y * SolveCameraPos.y
                             + portal.PlaneNormal.z * SolveCameraPos.z
                             - portal.PlaneDistance;
            if ( side < 0.0f )
                continue;

            ScreenBox2D box = ProjectPolygon( SolveWorldToClip, portal.Verts.data(), portal.Verts.size() );
            if ( box.IsEmpty() )
                continue;
            box = box.ClippedTo( aperture );
            if ( box.IsEmpty() )
                continue;

            if ( portal.TargetSector == cameFrom )
                continue;

            const uint16_t next = portal.TargetSector;
            if ( next >= Sectors.size() )
                continue;

            if ( !visited[next] ) {
                visited[next] = 1;
                apertures[next] = box;
                stack.push_back( { next, sector } );
            } else if ( !apertures[next].Contains( box ) ) {
                // Reached again through a wider chain - re-expand, like ActivateSector does.
                apertures[next].Merge( box );
                stack.push_back( { next, sector } );
            }
        }
    }

    return false;
}

bool BspPortalCuller::IsLeafVisible( const BspInfo& leaf ) const {
    // No sector -> plain outdoor leaf, never gated.
    if ( leaf.SectorIds.empty() )
        return true;

    for ( uint16_t s : leaf.SectorIds ) {
        if ( IsSectorActive( s ) )
            return true;
    }
    return false;
}

bool BspPortalCuller::IsBoxVisibleInLeafSectors( const BspInfo& leaf, const XMFLOAT3& bbMin, const XMFLOAT3& bbMax ) const {
    if ( leaf.SectorIds.empty() )
        return true;

    // Widest aperture among the leaf's active sectors, then one projection of the box against it.
    ScreenBox2D aperture;
    for ( uint16_t s : leaf.SectorIds ) {
        if ( IsSectorActive( s ) )
            aperture.Merge( Apertures[s] );
    }
    if ( aperture.IsEmpty() )
        return false;

    // A full-viewport aperture can never reject anything - skip the projection entirely.
    if ( aperture.MinX <= -1.0f && aperture.MinY <= -1.0f && aperture.MaxX >= 1.0f && aperture.MaxY >= 1.0f )
        return true;

    const XMFLOAT3 corners[8] = {
        { bbMin.x, bbMin.y, bbMin.z }, { bbMax.x, bbMin.y, bbMin.z },
        { bbMin.x, bbMax.y, bbMin.z }, { bbMax.x, bbMax.y, bbMin.z },
        { bbMin.x, bbMin.y, bbMax.z }, { bbMax.x, bbMin.y, bbMax.z },
        { bbMin.x, bbMax.y, bbMax.z }, { bbMax.x, bbMax.y, bbMax.z },
    };

    ScreenBox2D boxOnScreen;
    bool anyInFront = false;
    for ( const XMFLOAT3& c : corners ) {
        XMVECTOR clip = XMVector4Transform( XMVectorSetW( XMLoadFloat3( &c ), 1.0f ), SolveWorldToClip );
        const float w = XMVectorGetW( clip );
        if ( w <= CLIP_W_EPSILON ) {
            // Box straddles/contains the eye - cannot bound it safely on screen, keep it.
            return true;
        }
        anyInFront = true;
        boxOnScreen.Add( XMVectorGetX( clip ) / w, XMVectorGetY( clip ) / w );
    }

    if ( !anyInFront || boxOnScreen.IsEmpty() )
        return false;

    return boxOnScreen.Overlaps( aperture );
}
