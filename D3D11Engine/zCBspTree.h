#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCVob.h"
#include "zCVisual.h"
#include "zCArray.h"
#include "zCMesh.h"
#include "zTypes.h"
#include "Logger.h"
#include "BaseGraphicsEngine.h"
#include "BaseLineRenderer.h"

class zCFileBIN;
class zCVob;
class zCBspLeaf;
class zCBspNode;
class zCBspBase;

enum zTBspNodeType {
    zBSP_LEAF = 1,
    zBSP_NODE = 0
};

enum zTBspMode {
    zBSP_MODE_INDOOR = 0,
    zBSP_MODE_OUTDOOR = 1
};

class zCBspBase {
public:
    zCBspNode* Parent;
    zTBBox3D BBox3D;
    zCPolygon** PolyList;
    int	NumPolys;
    zTBspNodeType NodeType;

    bool IsLeaf() {
        return NodeType == zBSP_LEAF;
    }

    bool IsNode() {
        return NodeType == zBSP_NODE;
    }
};

class zCBspNode : public zCBspBase {
public:

    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCBspNodeRender), hooked_zCBspNodeRender );

#ifdef BUILD_GOTHIC_1_08k
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCBspBaseCollectPolysInBBox3D), hooked_zCBspBaseCollectPolysInBBox3D );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCBspBaseCheckRayAgainstPolys), hooked_zCBspBaseCheckRayAgainstPolys );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCBspBaseCheckRayAgainstPolysCache), hooked_zCBspBaseCheckRayAgainstPolysCache );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCBspBaseCheckRayAgainstPolysNearestHit), hooked_zCBspBaseCheckRayAgainstPolysNearestHit );
#endif
    }

#ifdef BUILD_GOTHIC_1_08k
    static int _fastcall hooked_zCBspBaseCheckRayAgainstPolysNearestHit( void* thisptr, const XMFLOAT3& start, const XMFLOAT3& end, XMFLOAT3& intersection ) {
        // Get our version of this node
        //Engine::GAPI->Get

#ifdef DEBUG_SHOW_COLLISION
        Engine::GraphicsEngine->GetLineRenderer()->AddLine( LineVertex( start, 0xFF0000FF ), LineVertex( end, 0xFFFFFFFF ) );
#endif

        if ( Engine::GAPI->GetLoadedWorldInfo()->CustomWorldLoaded ) {
            zCBspBase* base = reinterpret_cast<zCBspBase*>(thisptr);
            BspInfo* newNode = Engine::GAPI->GetNewBspNode( base );

            zCPolygon** polysOld = base->PolyList;
            int numPolysOld = base->NumPolys;

            base->PolyList = &newNode->NodePolygons[0];
            base->NumPolys = newNode->NodePolygons.size();

            // Call original function
            int r = HookedFunctions::OriginalFunctions.original_zCBspBaseCheckRayAgainstPolysNearestHit( thisptr, start, end, intersection );

            base->PolyList = polysOld;
            base->NumPolys = numPolysOld;

#ifdef DEBUG_SHOW_COLLISION
            Engine::GraphicsEngine->GetLineRenderer()->AddPointLocator( intersection, 25.0f );
#endif

            return r;
        } else {
            return HookedFunctions::OriginalFunctions.original_zCBspBaseCheckRayAgainstPolysNearestHit( thisptr, start, end, intersection );
        }
    }

    static int _fastcall hooked_zCBspBaseCheckRayAgainstPolysCache( void* thisptr, const XMFLOAT3& start, const XMFLOAT3& end, XMFLOAT3& intersection ) {
        // Get our version of this node
        //Engine::GAPI->Get

        if ( Engine::GAPI->GetLoadedWorldInfo()->CustomWorldLoaded ) {

#ifdef DEBUG_SHOW_COLLISION
            Engine::GraphicsEngine->GetLineRenderer()->AddLine( LineVertex( start, 0xFF0000FF ), LineVertex( end, 0xFFFFFFFF ) );
#endif

            zCBspBase* base = reinterpret_cast<zCBspBase*>(thisptr);
            BspInfo* newNode = Engine::GAPI->GetNewBspNode( base );

            zCPolygon** polysOld = base->PolyList;
            int numPolysOld = base->NumPolys;

            base->PolyList = &newNode->NodePolygons[0];
            base->NumPolys = newNode->NodePolygons.size();

            // Call original function
            //int r = HookedFunctions::OriginalFunctions.original_zCBspBaseCheckRayAgainstPolysCache(thisptr, start, end, intersection);
            int r = HookedFunctions::OriginalFunctions.original_zCBspBaseCheckRayAgainstPolysNearestHit( thisptr, start, end, intersection ); // Not supporting cache ATM

            base->PolyList = polysOld;
            base->NumPolys = numPolysOld;

#ifdef DEBUG_SHOW_COLLISION
            Engine::GraphicsEngine->GetLineRenderer()->AddPointLocator( intersection, 25.0f );
#endif
            return r;
        } else {
            return HookedFunctions::OriginalFunctions.original_zCBspBaseCheckRayAgainstPolysCache( thisptr, start, end, intersection );
        }
    }

    static int _fastcall hooked_zCBspBaseCheckRayAgainstPolys( void* thisptr, const XMFLOAT3& start, const XMFLOAT3& end, XMFLOAT3& intersection ) {
#ifdef DEBUG_SHOW_COLLISION
        Engine::GraphicsEngine->GetLineRenderer()->AddLine( LineVertex( start, 0xFF0000FF ), LineVertex( end, 0xFFFFFFFF ) );
#endif

        if ( Engine::GAPI->GetLoadedWorldInfo()->CustomWorldLoaded ) {
            zCBspBase* base = reinterpret_cast<zCBspBase*>(thisptr);
            BspInfo* newNode = Engine::GAPI->GetNewBspNode( base );

            zCPolygon** polysOld = base->PolyList;
            int numPolysOld = base->NumPolys;

            base->PolyList = &newNode->NodePolygons[0];
            base->NumPolys = newNode->NodePolygons.size();

            // Call original function
            int r = HookedFunctions::OriginalFunctions.original_zCBspBaseCheckRayAgainstPolys( thisptr, start, end, intersection );

            base->PolyList = polysOld;
            base->NumPolys = numPolysOld;

#ifdef DEBUG_SHOW_COLLISION
            Engine::GraphicsEngine->GetLineRenderer()->AddPointLocator( intersection, 25.0f );
#endif
            return r;
        } else {
            return HookedFunctions::OriginalFunctions.original_zCBspBaseCheckRayAgainstPolys( thisptr, start, end, intersection );
        }
    }

    static int __fastcall hooked_zCBspBaseCollectPolysInBBox3D( void* thisptr, const zTBBox3D& bbox, zCPolygon**& polyList, int& numFound ) {
        if ( Engine::GAPI->GetLoadedWorldInfo()->CustomWorldLoaded ) {
            Engine::GAPI->CollectPolygonsInAABB( bbox, polyList, numFound );
            //HookedFunctions::OriginalFunctions.original_zCBspBaseCollectPolysInBBox3D(thisptr, bbox, polyList, numFound);

#ifdef DEBUG_SHOW_COLLISION
            for ( int i = 0; i < numFound; i++ ) {
                Engine::GraphicsEngine->GetLineRenderer()->AddTriangle( *polyList[i]->getVertices()[0]->Position.toXMFLOAT3(),
                    *polyList[i]->getVertices()[1]->Position.toXMFLOAT3(),
                    *polyList[i]->getVertices()[2]->Position.toXMFLOAT3() );
            }


            Engine::GraphicsEngine->GetLineRenderer()->AddAABBMinMax( bbox.Min, bbox.Max, XMFLOAT4( 1, 0, 0, 1 ) );
#endif

            return numFound != 0;
        } else {
            return HookedFunctions::OriginalFunctions.original_zCBspBaseCollectPolysInBBox3D( thisptr, bbox, polyList, numFound );
        }
    }
#endif

    static void __fastcall hooked_zCBspNodeRender( void* thisptr, void* unkwn ) {
        // Start world rendering here
        Engine::GraphicsEngine->OnStartWorldRendering();
    }

    zTPlane	Plane;
    zCBspBase* Front;
    zCBspBase* Back;
    zCBspLeaf* LeafList;
    int NumLeafs;
    char PlaneSignbits;
};

class zCVobLight;
class zCBspLeaf : public zCBspBase {
public:
    int LastTimeLighted;
    zCArray<zCVob*>	LeafVobList;
    zCArray<zCVobLight*> LightVobList;

    UINT				lastTimeActivated;		// last time activated by portal
    short					sectorIndex;			// sector this leaf was activated by
};

/** BspTree-Object which holds the world */
class zCBspTree {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCBspTreeLoadBIN), hooked_LoadBIN );
        //DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCBspTreeAddVob), hooked_AddVob );
    }

    /** Called when a vob gets added to a bsp-tree */
    static void __fastcall hooked_AddVob( void* thisptr, void* unknwn, zCVob* vob ) {
        HookedFunctions::OriginalFunctions.original_zCBspTreeAddVob( thisptr, vob );
    }

    /** Called on level load. */
    static int __fastcall hooked_LoadBIN( void* thisptr, void* vtbl, zCFileBIN& file, int skip ) {
        LogInfo() << "Loading world: " << Engine::GAPI->GetLoadedWorldInfo()->WorldName;
        int r = HookedFunctions::OriginalFunctions.original_zCBspTreeLoadBIN( thisptr, file, skip );
                // Save pointer to this
        Engine::GAPI->GetLoadedWorldInfo()->BspTree = reinterpret_cast<zCBspTree*>(thisptr);
        LoadLevelGeometry( Engine::GAPI->GetLoadedWorldInfo()->BspTree );
        return r;
    }

    /** Loads the world geometry of this BSP-Tree */
    static void LoadLevelGeometry( zCBspTree* tree ) {
        LogInfo() << "World loaded, getting Levelmesh now!";
        LogInfo() << " - Found " << tree->GetNumPolys() << " polygons";

        std::vector<zCPolygon*> polys;
        tree->GetLOD0Polygons( polys );

        Engine::GAPI->OnGeometryLoaded( &polys[0], polys.size() );
    }

    /** Returns only the polygons used in LOD0 of the world */
    void GetLOD0Polygons( std::vector<zCPolygon*>& target ) {
        int num = GetNumLeafes();
        target.reserve( num * 3 ); // preallocate a little space

        for ( int i = 0; i < num; i++ ) {
            zCBspLeaf* leaf = GetLeaf( i );

            for ( int j = 0; j < leaf->NumPolys; j++ ) {
                target.push_back( leaf->PolyList[j] );
            }
        }
    }

    int GetNumLeafes() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCBspTree::Offset_NumLeafes ));
    }

    zTBspMode GetBspTreeMode() {
        return *reinterpret_cast<zTBspMode*>(THISPTR_OFFSET( GothicMemoryLocations::zCBspTree::Offset_BspTreeMode ));
    }

    zCBspLeaf* GetLeaf( int i ) {
        char* list = *reinterpret_cast<char**>(THISPTR_OFFSET( GothicMemoryLocations::zCBspTree::Offset_LeafList ));
        return reinterpret_cast<zCBspLeaf*>(list + GothicMemoryLocations::zCBspLeaf::Size * i);
    }

    zCBspBase* GetRootNode() {
        return *reinterpret_cast<zCBspBase**>(THISPTR_OFFSET( GothicMemoryLocations::zCBspTree::Offset_RootNode ));
    }

    int GetNumPolys() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCBspTree::Offset_NumPolys ));
    }

    zCPolygon** GetPolygons() {
        return *reinterpret_cast<zCPolygon***>(THISPTR_OFFSET( GothicMemoryLocations::zCBspTree::Offset_PolyArray ));
    }

    zCMesh* GetMesh() {
        return *reinterpret_cast<zCMesh**>(THISPTR_OFFSET( GothicMemoryLocations::zCBspTree::Offset_WorldMesh ));
    }

private:

};

