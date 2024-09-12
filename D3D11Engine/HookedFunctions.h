#pragma once
#include "pch.h"
#include "Detours/detours.h"
#include "GothicMemoryLocations.h"
#include "zTypes.h"
#include "HookExceptionFilter.h"

/** This file stores the original versions of the hooked functions and the function declerations */

class zCFileBIN;
class zCCamera;
class zCVob;
class zSTRING;
class zCBspBase;
class oCNPC;
class zCPolygon;
class zCTexture;
class zCViewDraw;

template <class T>
class zCTree;

class zCVisual;

typedef int( __thiscall* zCBspTreeLoadBIN )(void*, zCFileBIN&, int);
typedef void( __thiscall* zCWorldRender )(void*, zCCamera&);
typedef void( __thiscall* zCWorldVobAddedToWorld )(void*, zCVob*);
typedef void( __thiscall* oCNPCEnable )(void*, XMFLOAT3&);
typedef void( __thiscall* zCBspTreeAddVob )(void*, zCVob*);
typedef void( __thiscall* zCWorldLoadWorld )(void*, const zSTRING& fileName, const int loadMode);
typedef void( __thiscall* oCGameEnterWorld )(void*, oCNPC* playerVob, int changePlayerPos, const zSTRING& startpoint);
typedef void( __thiscall* zCWorldVobRemovedFromWorld )(void*, zCVob*);
typedef XMFLOAT4X4( __cdecl* Alg_Rotation3DNRad )(const XMFLOAT3& axis, const float angle);
typedef int( __cdecl* vidGetFPSRate )();
typedef void( __thiscall* GenericDestructor )(void*);
typedef void( __thiscall* GenericThiscall )(void*);
typedef void( __thiscall* zCMaterialConstruktor )(void*);
typedef void( __thiscall* zCMaterialInitValues )(void*);
typedef void( __fastcall* zCBspNodeRenderIndoor )(void*, int);
typedef void( __fastcall* zCBspNodeRenderOutdoor )(void*, zCBspBase*, zTBBox3D, int, int);

typedef int( __fastcall* zCBspBaseCollectPolysInBBox3D )(void*, const zTBBox3D&, zCPolygon**&, int&);

typedef int( __fastcall* zCBspBaseCheckRayAgainstPolys )(void*, const XMFLOAT3&, const XMFLOAT3&, XMFLOAT3&);

typedef int( __thiscall* zFILEOpen )(void*, zSTRING&, bool);
typedef void( __thiscall* zCRnd_D3D_DrawPoly )(void*, zCPolygon*);
typedef void( __thiscall* zCRnd_D3D_DrawPolySimple )(void*, zCTexture*, void*, int);
typedef int( __thiscall* zCOptionReadInt )(void*, zSTRING const&, char const*, int);
typedef int( __thiscall* zCOptionReadBool )(void*, zSTRING const&, char const*, int);
typedef unsigned long( __thiscall* zCOptionReadDWORD )(void*, zSTRING const&, char const*, unsigned long);
typedef void( __thiscall* zCViewBlitText )(void*);
typedef void( __thiscall* zCViewPrint )(void*, int, int, const zSTRING&);
typedef int( __thiscall* CGameManagerExitGame )(void*);
typedef const zSTRING* (__thiscall* zCVisualGetFileExtension)(void*, int);
typedef void( __thiscall* zCWorldDisposeVobs )(void*, zCTree<zCVob>*);
typedef void( __thiscall* oCSpawnManagerSpawnNpc )(void*, oCNPC*, const XMFLOAT3&, float);
typedef int( __thiscall* oCSpawnManagerCheckRemoveNpc )(void*, oCNPC*);
typedef void( __thiscall* oCSpawnManagerCheckInsertNpc )(void*);
typedef void( __thiscall* zCVobSetVisual )(void*, zCVisual*);

typedef int( __thiscall* zCTex_D3DXTEX_BuildSurfaces )(void*, int);
typedef int( __thiscall* zCTextureLoadResourceData )(void*);
typedef int( __thiscall* zCThreadSuspendThread )(void*);
typedef void( __thiscall* zCResourceManagerCacheOut )(void*, class zCResource*);
typedef void( __thiscall* zCQuadMarkCreateQuadMark )(void*, zCPolygon*, const float3&, const float2&, struct zTEffectParams*);
typedef void( __thiscall* oCWorldEnableVob )(void*, zCVob*, zCVob*);
typedef void( __thiscall* oCWorldRemoveVob )(void*, zCVob*);
typedef void( __thiscall* oCWorldDisableVob )(void*, zCVob*);
typedef void( __fastcall* oCWorldRemoveFromLists )(void*, zCVob*);
typedef int( __thiscall* zCModelPrototypeLoadModelASC )(void*, class zSTRING const&);
typedef int( __thiscall* zCModelPrototypeReadMeshAndTreeMSB )(void*, int&, class zCFileBIN&);

typedef int( __thiscall* zCModelGetLowestLODNumPolys )(void*);
typedef float3*( __thiscall* zCModelGetLowestLODPoly )(void*, const int, float3*&);

typedef DWORD( __cdecl* GetInformationManagerProc )();

#ifdef BUILD_GOTHIC_1_08k
typedef void( __thiscall* zCVobEndMovement )(void*);
#else
typedef void( __thiscall* zCVobEndMovement )(void*, int);
#endif
struct zTRndSurfaceDesc;
struct HookedFunctionInfo {

    /** Init all hooks here */
    void InitHooks();

    zCBspTreeLoadBIN original_zCBspTreeLoadBIN = reinterpret_cast<zCBspTreeLoadBIN>(GothicMemoryLocations::zCBspTree::LoadBIN);
    zCWorldRender original_zCWorldRender = reinterpret_cast<zCWorldRender>(GothicMemoryLocations::zCWorld::Render);
    zCWorldVobAddedToWorld original_zCWorldVobAddedToWorld = reinterpret_cast<zCWorldVobAddedToWorld>(GothicMemoryLocations::zCWorld::VobAddedToWorld);
    zCBspTreeAddVob original_zCBspTreeAddVob = reinterpret_cast<zCBspTreeAddVob>(GothicMemoryLocations::zCBspTree::AddVob);
    zCWorldLoadWorld original_zCWorldLoadWorld = reinterpret_cast<zCWorldLoadWorld>(GothicMemoryLocations::zCWorld::LoadWorld);
    oCGameEnterWorld original_oCGameEnterWorld = reinterpret_cast<oCGameEnterWorld>(GothicMemoryLocations::oCGame::EnterWorld);
    zCWorldVobRemovedFromWorld original_zCWorldVobRemovedFromWorld = reinterpret_cast<zCWorldVobRemovedFromWorld>(GothicMemoryLocations::zCWorld::VobRemovedFromWorld);
    Alg_Rotation3DNRad original_Alg_Rotation3DNRad = reinterpret_cast<Alg_Rotation3DNRad>(GothicMemoryLocations::Functions::Alg_Rotation3DNRad);
    GenericDestructor original_zCMaterialDestructor = reinterpret_cast<GenericDestructor>(GothicMemoryLocations::zCMaterial::Destructor);
    GenericDestructor original_zCParticleFXDestructor = reinterpret_cast<GenericDestructor>(GothicMemoryLocations::zCParticleFX::Destructor);
    GenericDestructor original_zCVisualDestructor = reinterpret_cast<GenericDestructor>(GothicMemoryLocations::zCVisual::Destructor);
    zCMaterialConstruktor original_zCMaterialConstruktor = reinterpret_cast<zCMaterialConstruktor>(GothicMemoryLocations::zCMaterial::Constructor);
    zCMaterialInitValues original_zCMaterialInitValues = reinterpret_cast<zCMaterialInitValues>(GothicMemoryLocations::zCMaterial::InitValues);
    zFILEOpen original_zFILEOpen = reinterpret_cast<zFILEOpen>(GothicMemoryLocations::zFILE::Open);
    GenericThiscall original_zCRnd_D3D_DrawLineZ = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::zCRndD3D::DrawLineZ); // Not usable - only for hooking
    GenericThiscall original_zCRnd_D3D_DrawLine = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::zCRndD3D::DrawLine); // Not usable - only for hooking
    zCRnd_D3D_DrawPoly original_zCRnd_D3D_DrawPoly = reinterpret_cast<zCRnd_D3D_DrawPoly>(GothicMemoryLocations::zCRndD3D::DrawPoly);
    zCRnd_D3D_DrawPolySimple original_zCRnd_D3D_DrawPolySimple = reinterpret_cast<zCRnd_D3D_DrawPolySimple>(GothicMemoryLocations::zCRndD3D::DrawPolySimple);
    GenericThiscall original_zCRnd_D3D_CacheInSurface = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::zCRndD3D::CacheInSurface); // Not usable - only for hooking
    GenericThiscall original_zCRnd_D3D_CacheOutSurface = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::zCRndD3D::CacheOutSurface); // Not usable - only for hooking
    GenericThiscall original_zCRnd_D3D_RenderScreenFade = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::zCRndD3D::RenderScreenFade); // Not usable - only for hooking
    GenericThiscall original_zCRnd_D3D_RenderCinemaScope = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::zCRndD3D::RenderCinemaScope); // Not usable - only for hooking
    zCOptionReadInt original_zCOptionReadInt = reinterpret_cast<zCOptionReadInt>(GothicMemoryLocations::zCOption::ReadInt);
    zCOptionReadBool original_zCOptionReadBool = reinterpret_cast<zCOptionReadBool>(GothicMemoryLocations::zCOption::ReadBool);
    zCOptionReadDWORD original_zCOptionReadDWORD = reinterpret_cast<zCOptionReadDWORD>(GothicMemoryLocations::zCOption::ReadDWORD);
#if (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)) || defined(BUILD_GOTHIC_2_6_fix)
    zCViewBlitText original_zCViewBlit = reinterpret_cast<zCViewBlitText>(GothicMemoryLocations::zCView::Blit);
    zCViewBlitText original_zCViewBlitText = reinterpret_cast<zCViewBlitText>(GothicMemoryLocations::zCView::BlitText);
    zCViewPrint original_zCViewPrint = reinterpret_cast<zCViewPrint>(GothicMemoryLocations::zCView::Print);
    zCViewPrint original_zCViewPrintChars = reinterpret_cast<zCViewPrint>(GothicMemoryLocations::zCView::PrintChars);
#endif
    //CGameManagerExitGame original_CGameManagerExitGame = reinterpret_cast<CGameManagerExitGame>(GothicMemoryLocations::CGameManager::ExitGame);
    //GenericThiscall original_zCWorldDisposeWorld = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::zCWorld::DisposeWorld);
    zCWorldDisposeVobs original_zCWorldDisposeVobs = reinterpret_cast<zCWorldDisposeVobs>(GothicMemoryLocations::zCWorld::DisposeVobs);
    oCSpawnManagerSpawnNpc original_oCSpawnManagerSpawnNpc = reinterpret_cast<oCSpawnManagerSpawnNpc>(GothicMemoryLocations::oCSpawnManager::SpawnNpc);
    oCSpawnManagerCheckRemoveNpc original_oCSpawnManagerCheckRemoveNpc = reinterpret_cast<oCSpawnManagerCheckRemoveNpc>(GothicMemoryLocations::oCSpawnManager::CheckRemoveNpc);
    oCSpawnManagerCheckInsertNpc original_oCSpawnManagerCheckInsertNpc = reinterpret_cast<oCSpawnManagerCheckInsertNpc>(GothicMemoryLocations::oCSpawnManager::CheckInsertNpc);
    zCVobSetVisual original_zCVobSetVisual = reinterpret_cast<zCVobSetVisual>(GothicMemoryLocations::zCVob::SetVisual);
    GenericDestructor original_zCVobDestructor = reinterpret_cast<GenericDestructor>(GothicMemoryLocations::zCVob::Destructor);
    //zCTex_D3DXTEX_BuildSurfaces original_zCTex_D3DXTEX_BuildSurfaces = reinterpret_cast<zCTex_D3DXTEX_BuildSurfaces>(GothicMemoryLocations::zCTexture::XTEX_BuildSurfaces);
    zCTextureLoadResourceData ofiginal_zCTextureLoadResourceData = reinterpret_cast<zCTextureLoadResourceData>(GothicMemoryLocations::zCTexture::LoadResourceData);
    zCThreadSuspendThread original_zCThreadSuspendThread = reinterpret_cast<zCThreadSuspendThread>(GothicMemoryLocations::zCThread::SuspendThread);
    //zCResourceManagerCacheOut original_zCResourceManagerCacheOut = reinterpret_cast<zCResourceManagerCacheOut>(GothicMemoryLocations::zCResourceManager::CacheOut);
    zCQuadMarkCreateQuadMark original_zCQuadMarkCreateQuadMark = reinterpret_cast<zCQuadMarkCreateQuadMark>(GothicMemoryLocations::zCQuadMark::CreateQuadMark);
    GenericDestructor original_zCQuadMarkDestructor = reinterpret_cast<GenericDestructor>(GothicMemoryLocations::zCQuadMark::Destructor);
    GenericThiscall original_zCQuadMarkConstructor = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::zCQuadMark::Constructor);
    oCNPCEnable original_oCNPCEnable = reinterpret_cast<oCNPCEnable>(GothicMemoryLocations::oCNPC::Enable);
    GenericThiscall original_oCNPCDisable = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::oCNPC::Disable);
    GenericThiscall original_oCNPCInitModel = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::oCNPC::InitModel);
    oCWorldDisableVob original_oCWorldDisableVob = reinterpret_cast<oCWorldDisableVob>(GothicMemoryLocations::oCWorld::DisableVob);
    oCWorldEnableVob original_oCWorldEnableVob = reinterpret_cast<oCWorldEnableVob>(GothicMemoryLocations::oCWorld::EnableVob);
    oCWorldRemoveVob original_oCWorldRemoveVob = reinterpret_cast<oCWorldRemoveVob>(GothicMemoryLocations::oCWorld::RemoveVob);
    oCWorldRemoveFromLists original_oCWorldRemoveFromLists = reinterpret_cast<oCWorldRemoveFromLists>(GothicMemoryLocations::oCWorld::RemoveFromLists);
    zCVobEndMovement original_zCVobEndMovement = reinterpret_cast<zCVobEndMovement>(GothicMemoryLocations::zCVob::EndMovement);
    GenericThiscall original_zCBspNodeRender = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::zCBspTree::Render); // Not usable - only for hooking
#ifdef BUILD_GOTHIC_1_08k
    zCBspBaseCollectPolysInBBox3D original_zCBspBaseCollectPolysInBBox3D = reinterpret_cast<zCBspBaseCollectPolysInBBox3D>(GothicMemoryLocations::zCBspBase::CollectPolysInBBox3D);
    zCBspBaseCheckRayAgainstPolys original_zCBspBaseCheckRayAgainstPolys = reinterpret_cast<zCBspBaseCheckRayAgainstPolys>(GothicMemoryLocations::zCBspBase::CheckRayAgainstPolys);
    zCBspBaseCheckRayAgainstPolys original_zCBspBaseCheckRayAgainstPolysCache = reinterpret_cast<zCBspBaseCheckRayAgainstPolys>(GothicMemoryLocations::zCBspBase::CheckRayAgainstPolysCache);
    zCBspBaseCheckRayAgainstPolys original_zCBspBaseCheckRayAgainstPolysNearestHit = reinterpret_cast<zCBspBaseCheckRayAgainstPolys>(GothicMemoryLocations::zCBspBase::CheckRayAgainstPolysNearestHit);
#endif
#ifdef BUILD_GOTHIC_2_6_fix
    GenericThiscall original_zCActiveSndAutoCalcObstruction = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::zCActiveSnd::AutoCalcObstruction); // Not usable - only for hooking
    zCModelGetLowestLODNumPolys original_zCModelGetLowestLODNumPolys = reinterpret_cast<zCModelGetLowestLODNumPolys>(GothicMemoryLocations::zCModel::GetLowestLODNumPolys);
    zCModelGetLowestLODPoly original_zCModelGetLowestLODPoly = reinterpret_cast<zCModelGetLowestLODPoly>(GothicMemoryLocations::zCModel::GetLowestLODPoly);
#endif
    //zCModelPrototypeLoadModelASC original_zCModelPrototypeLoadModelASC = reinterpret_cast<zCModelPrototypeLoadModelASC>(GothicMemoryLocations::zCModelPrototype::LoadModelASC);
    //zCModelPrototypeReadMeshAndTreeMSB original_zCModelPrototypeReadMeshAndTreeMSB = reinterpret_cast<zCModelPrototypeReadMeshAndTreeMSB>(GothicMemoryLocations::zCModelPrototype::ReadMeshAndTreeMSB);

    /** Function hooks */
    static void __fastcall hooked_zCActiveSndAutoCalcObstruction( void* thisptr, void* unknwn, int i );

    static int __cdecl hooked_GetNumDevices();
    static void __fastcall hooked_SetLightmap( void* polygonPtr );

    static FARPROC WINAPI hooked_GetProcAddress( HMODULE mod, const char* procName );

#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    void InitAnimatedInventoryHooks();
    static void __fastcall hooked_RotateInInventory( DWORD oCItem );
#endif
};

namespace HookedFunctions {
    /** Holds all the original functions */
    __declspec(selectany) HookedFunctionInfo OriginalFunctions;
};
