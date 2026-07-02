#pragma once
#include "pch.h"
#include "Detours/detours.h"
#include "GothicMemoryLocations.h"
#include "zTypes.h"
#include "HookExceptionFilter.h"
#include <cstring>

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
class zCParser;

template <class T>
class zCTree;

class zCVisual;

template<typename T>
struct HookFunc {
private:
    PVOID func_ptr = nullptr;

    // Helper to cast internal PVOID back to the usable function pointer type
    T get_func() const { return reinterpret_cast<T>(func_ptr); }

public:
    static_assert(std::is_pointer<T>::value&& std::is_function<typename std::remove_pointer<T>::type>::value,
                  "HookFunc requires a function pointer type.");

    HookFunc() = delete;
    HookFunc(const HookFunc&) = delete;
    HookFunc& operator=(const HookFunc&) = delete;

    HookFunc( HookFunc&& ) = delete;
    HookFunc& operator=(HookFunc&&) = delete;

    HookFunc(DWORD_PTR p) : func_ptr(reinterpret_cast<PVOID>(p)) {}
    HookFunc(T f = nullptr) : func_ptr(reinterpret_cast<PVOID>(f)) {}

    template <typename THook>
    LONG Detour( THook hookFn ) {
        static_assert(std::is_pointer<THook>::value&& std::is_function<typename std::remove_pointer<THook>::type>::value,
                  "Detour THook requires a function pointer type.");

        return DetourAttach( &func_ptr, reinterpret_cast<PVOID>(hookFn) );
    }

    // perfect forward any arguments matching what T expects
    template<typename... Args>
    auto operator()( Args&&... args ) -> decltype(std::declval<T>()( std::forward<Args>( args )... )) {
        return get_func()( std::forward<Args>( args )... );
    }

    explicit operator bool() { return func_ptr != nullptr; }
};

typedef int( __thiscall* zCBspTreeLoadBIN )(void*, zCFileBIN&, int);
typedef void( __thiscall* zCWorldRender )(void*, zCCamera&);
typedef void( __thiscall* zCWorldVobAddedToWorld )(void*, zCVob*);
#ifdef BUILD_SPACER_NET
typedef void( __thiscall* zCWorldCompileWorld )(void*, int&, float, int, int, void*);
typedef void( __thiscall* zCWorldGenerateStaticWorldLighting )(void*, int&, void*);
#endif
typedef void( __thiscall* oCNPCEnable )(void*, XMFLOAT3&);
typedef void( __thiscall* zCBspTreeAddVob )(void*, zCVob*);
typedef void( __thiscall* zCWorldLoadWorld )(void*, const zSTRING& fileName, const int loadMode);
typedef void( __thiscall* oCGameEnterWorld )(void*, oCNPC* playerVob, int changePlayerPos, const zSTRING& startpoint);
#if defined(BUILD_GOTHIC_2_6_fix) || defined(BUILD_GOTHIC_1_CLASSIC)
typedef void( __thiscall* oCGameDefineExternals_Ulfi )(void*, zCParser* parser);
#endif
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
typedef void( __thiscall* zCFlashSetVisualUsedBy )(void*, zCVob*);
typedef void( __thiscall* oCWorldEnableVob )(void*, zCVob*, zCVob*);
typedef void( __thiscall* oCWorldRemoveVob )(void*, zCVob*);
typedef void( __thiscall* oCWorldDisableVob )(void*, zCVob*);
typedef void( __fastcall* oCWorldRemoveFromLists )(void*, zCVob*);
typedef int( __thiscall* zCModelPrototypeLoadModelASC )(void*, class zSTRING const&);
typedef int( __thiscall* zCModelPrototypeReadMeshAndTreeMSB )(void*, int&, class zCFileBIN&);

typedef int( __thiscall* zCModelGetLowestLODNumPolys )(void*);
typedef float3*( __thiscall* zCModelGetLowestLODPoly )(void*, const int, float3*&);

typedef DWORD( __cdecl* GetInformationManagerProc )();

typedef uint16_t( __thiscall* zCInput_Win32__GetKey )(void*, int repeat, int delayed);

#ifdef BUILD_GOTHIC_1_08k
typedef void( __thiscall* zCVobEndMovement )(void*);
#else
typedef void( __thiscall* zCVobEndMovement )(void*, int);
#endif

typedef void( __cdecl* oCItemContainer__Container_Draw )();
typedef void( __thiscall* zCCamera__Activate )(void*);
typedef void( __thiscall* zCCamera__UpdateViewport )(void*);

typedef void( __thiscall* zCSkyControler_ClearBackground )(void*, zColor);

struct zTRndSurfaceDesc;

struct HookedFunctionInfo {

    /** Init all hooks here */
    void InitHooks();

    HookFunc<zCBspTreeLoadBIN> original_zCBspTreeLoadBIN = GothicMemoryLocations::zCBspTree::LoadBIN;
    HookFunc<zCWorldRender> original_zCWorldRender = GothicMemoryLocations::zCWorld::Render;
    HookFunc<oCItemContainer__Container_Draw> original_ContainerDraw = GothicMemoryLocations::oCItemContainer::s_Container_Draw;
    HookFunc<zCWorldVobAddedToWorld> original_zCWorldVobAddedToWorld = GothicMemoryLocations::zCWorld::VobAddedToWorld;
#ifdef BUILD_SPACER_NET
    HookFunc<zCWorldCompileWorld> original_zCWorldCompileWorld = GothicMemoryLocations::zCWorld::CompileWorld;
    HookFunc<zCWorldGenerateStaticWorldLighting> original_zCWorldGenerateStaticWorldLighting = GothicMemoryLocations::zCWorld::GenerateStaticWorldLighting;
#endif
    HookFunc<zCBspTreeAddVob> original_zCBspTreeAddVob = GothicMemoryLocations::zCBspTree::AddVob;
    HookFunc<zCWorldLoadWorld> original_zCWorldLoadWorld = GothicMemoryLocations::zCWorld::LoadWorld;
    HookFunc<oCGameEnterWorld> original_oCGameEnterWorld = GothicMemoryLocations::oCGame::EnterWorld;
#if defined(BUILD_GOTHIC_2_6_fix) || defined(BUILD_GOTHIC_1_CLASSIC)
    HookFunc<oCGameDefineExternals_Ulfi> original_oCGameDefineExternals_Ulfi = GothicMemoryLocations::oCGame::DefineExternals_Ulfi;
#endif
    HookFunc<zCWorldVobRemovedFromWorld> original_zCWorldVobRemovedFromWorld = GothicMemoryLocations::zCWorld::VobRemovedFromWorld;
    HookFunc<Alg_Rotation3DNRad> original_Alg_Rotation3DNRad = GothicMemoryLocations::Functions::Alg_Rotation3DNRad;
    HookFunc<GenericDestructor> original_zCMaterialDestructor = GothicMemoryLocations::zCMaterial::Destructor;
    HookFunc<GenericDestructor> original_zCParticleFXDestructor = GothicMemoryLocations::zCParticleFX::Destructor;
    HookFunc<GenericDestructor> original_zCVisualDestructor = GothicMemoryLocations::zCVisual::Destructor;
    HookFunc<zCMaterialConstruktor> original_zCMaterialConstruktor = GothicMemoryLocations::zCMaterial::Constructor;
    HookFunc<zCMaterialInitValues> original_zCMaterialInitValues = GothicMemoryLocations::zCMaterial::InitValues;
    HookFunc<zFILEOpen> original_zFILEOpen = GothicMemoryLocations::zFILE::Open;
    HookFunc<GenericThiscall> original_zCRnd_D3D_DrawLineZ = GothicMemoryLocations::zCRndD3D::DrawLineZ; // Not usable - only for hooking
    HookFunc<GenericThiscall> original_zCRnd_D3D_DrawLine = GothicMemoryLocations::zCRndD3D::DrawLine; // Not usable - only for hooking
    HookFunc<zCRnd_D3D_DrawPoly> original_zCRnd_D3D_DrawPoly = GothicMemoryLocations::zCRndD3D::DrawPoly;
    HookFunc<zCRnd_D3D_DrawPolySimple> original_zCRnd_D3D_DrawPolySimple = GothicMemoryLocations::zCRndD3D::DrawPolySimple;
    HookFunc<GenericThiscall> original_zCRnd_D3D_CacheInSurface = GothicMemoryLocations::zCRndD3D::CacheInSurface; // Not usable - only for hooking
    HookFunc<GenericThiscall> original_zCRnd_D3D_CacheOutSurface = GothicMemoryLocations::zCRndD3D::CacheOutSurface; // Not usable - only for hooking
    HookFunc<GenericThiscall> original_zCRnd_D3D_RenderScreenFade = GothicMemoryLocations::zCRndD3D::RenderScreenFade; // Not usable - only for hooking
    HookFunc<GenericThiscall> original_zCRnd_D3D_RenderCinemaScope = GothicMemoryLocations::zCRndD3D::RenderCinemaScope; // Not usable - only for hooking
    HookFunc<zCOptionReadInt> original_zCOptionReadInt = GothicMemoryLocations::zCOption::ReadInt;
    HookFunc<zCOptionReadBool> original_zCOptionReadBool = GothicMemoryLocations::zCOption::ReadBool;
    HookFunc<zCOptionReadDWORD> original_zCOptionReadDWORD = GothicMemoryLocations::zCOption::ReadDWORD;
#if defined(BUILD_GOTHIC_1_CLASSIC) || defined(BUILD_GOTHIC_2_6_fix)
    HookFunc<zCViewBlitText> original_zCViewBlit = GothicMemoryLocations::zCView::Blit;
    HookFunc<zCViewBlitText> original_zCViewBlitText = GothicMemoryLocations::zCView::BlitText;
    HookFunc<zCViewPrint> original_zCViewPrint = GothicMemoryLocations::zCView::Print;
    HookFunc<zCViewPrint> original_zCViewPrintChars = GothicMemoryLocations::zCView::PrintChars;
#endif
    HookFunc<zCCamera__Activate> original_zCCamera__Activate = GothicMemoryLocations::zCCamera::Activate;
    HookFunc<zCCamera__UpdateViewport> original_zCCamera__UpdateViewport = GothicMemoryLocations::zCCamera::UpdateViewport;
    //CGameManagerExitGame original_CGameManagerExitGame = reinterpret_cast<CGameManagerExitGame>(GothicMemoryLocations::CGameManager::ExitGame);
    //GenericThiscall original_zCWorldDisposeWorld = reinterpret_cast<GenericThiscall>(GothicMemoryLocations::zCWorld::DisposeWorld);
    HookFunc<zCWorldDisposeVobs> original_zCWorldDisposeVobs = GothicMemoryLocations::zCWorld::DisposeVobs;
    HookFunc<oCSpawnManagerSpawnNpc> original_oCSpawnManagerSpawnNpc = GothicMemoryLocations::oCSpawnManager::SpawnNpc;
    HookFunc<oCSpawnManagerCheckRemoveNpc> original_oCSpawnManagerCheckRemoveNpc = GothicMemoryLocations::oCSpawnManager::CheckRemoveNpc;
    HookFunc<oCSpawnManagerCheckInsertNpc> original_oCSpawnManagerCheckInsertNpc = GothicMemoryLocations::oCSpawnManager::CheckInsertNpc;
    HookFunc<zCVobSetVisual> original_zCVobSetVisual = GothicMemoryLocations::zCVob::SetVisual;
    HookFunc<GenericDestructor> original_zCVobDestructor = GothicMemoryLocations::zCVob::Destructor;
    //zCTex_D3DXTEX_BuildSurfaces original_zCTex_D3DXTEX_BuildSurfaces = GothicMemoryLocations::zCTexture::XTEX_BuildSurfaces);
    HookFunc<zCTextureLoadResourceData> ofiginal_zCTextureLoadResourceData = GothicMemoryLocations::zCTexture::LoadResourceData;
    HookFunc<zCThreadSuspendThread> original_zCThreadSuspendThread = GothicMemoryLocations::zCThread::SuspendThread;
    //zCResourceManagerCacheOut original_zCResourceManagerCacheOut = GothicMemoryLocations::zCResourceManager::CacheOut);
    HookFunc<zCQuadMarkCreateQuadMark> original_zCQuadMarkCreateQuadMark = GothicMemoryLocations::zCQuadMark::CreateQuadMark;
    HookFunc<GenericDestructor> original_zCQuadMarkDestructor = GothicMemoryLocations::zCQuadMark::Destructor;
    HookFunc<GenericThiscall> original_zCQuadMarkConstructor = GothicMemoryLocations::zCQuadMark::Constructor;
    HookFunc<zCFlashSetVisualUsedBy> original_zCFlashSetVisualUsedBy = GothicMemoryLocations::zCFlash::SetVisualUsedBy;
    HookFunc<GenericDestructor> original_zCFlashDestructor = GothicMemoryLocations::zCFlash::Destructor;
    HookFunc<oCNPCEnable> original_oCNPCEnable = GothicMemoryLocations::oCNPC::Enable;
    HookFunc<GenericThiscall> original_oCNPCDisable = GothicMemoryLocations::oCNPC::Disable;
    HookFunc<GenericThiscall> original_oCNPCInitModel = GothicMemoryLocations::oCNPC::InitModel;
    HookFunc<oCWorldDisableVob> original_oCWorldDisableVob = GothicMemoryLocations::oCWorld::DisableVob;
    HookFunc<oCWorldEnableVob> original_oCWorldEnableVob = GothicMemoryLocations::oCWorld::EnableVob;
    HookFunc<oCWorldRemoveVob> original_oCWorldRemoveVob = GothicMemoryLocations::oCWorld::RemoveVob;
    HookFunc<oCWorldRemoveFromLists> original_oCWorldRemoveFromLists = GothicMemoryLocations::oCWorld::RemoveFromLists;
    HookFunc<zCVobEndMovement> original_zCVobEndMovement = GothicMemoryLocations::zCVob::EndMovement;
    HookFunc<GenericThiscall> original_zCBspNodeRender = GothicMemoryLocations::zCBspTree::Render; // Not usable - only for hooking
#ifdef BUILD_GOTHIC_1_08k
    HookFunc<zCBspBaseCollectPolysInBBox3D> original_zCBspBaseCollectPolysInBBox3D = GothicMemoryLocations::zCBspBase::CollectPolysInBBox3D;
    HookFunc<zCBspBaseCheckRayAgainstPolys> original_zCBspBaseCheckRayAgainstPolys = GothicMemoryLocations::zCBspBase::CheckRayAgainstPolys;
    HookFunc<zCBspBaseCheckRayAgainstPolys> original_zCBspBaseCheckRayAgainstPolysCache = GothicMemoryLocations::zCBspBase::CheckRayAgainstPolysCache;
    HookFunc<zCBspBaseCheckRayAgainstPolys> original_zCBspBaseCheckRayAgainstPolysNearestHit = GothicMemoryLocations::zCBspBase::CheckRayAgainstPolysNearestHit;
#endif
#ifdef BUILD_GOTHIC_2_6_fix
    HookFunc<void( __thiscall* )(void*, int)> original_zCActiveSndAutoCalcObstruction = GothicMemoryLocations::zCActiveSnd::AutoCalcObstruction; // Not usable - only for hooking
    HookFunc<zCModelGetLowestLODNumPolys> original_zCModelGetLowestLODNumPolys = GothicMemoryLocations::zCModel::GetLowestLODNumPolys;
    HookFunc<zCModelGetLowestLODPoly> original_zCModelGetLowestLODPoly = GothicMemoryLocations::zCModel::GetLowestLODPoly;
#endif
    HookFunc<zCInput_Win32__GetKey> original_zCInput_Win32__GetKey = GothicMemoryLocations::zCInput_Win32::GetKey;
    HookFunc<zCSkyControler_ClearBackground> original_zCSkyControler_ClearBackground = GothicMemoryLocations::zCSkyController::ClearBackground;
    //zCModelPrototypeLoadModelASC original_zCModelPrototypeLoadModelASC = reinterpret_cast<zCModelPrototypeLoadModelASC>(GothicMemoryLocations::zCModelPrototype::LoadModelASC);
    //zCModelPrototypeReadMeshAndTreeMSB original_zCModelPrototypeReadMeshAndTreeMSB = reinterpret_cast<zCModelPrototypeReadMeshAndTreeMSB>(GothicMemoryLocations::zCModelPrototype::ReadMeshAndTreeMSB);

    /** Function hooks */
    static void __fastcall hooked_zCActiveSndAutoCalcObstruction( void* thisptr, void* unknwn, int i );

    static int __cdecl hooked_GetNumDevices();
    static void __fastcall hooked_SetLightmap( void* polygonPtr );

    static FARPROC WINAPI hooked_GetProcAddress( HMODULE mod, const char* procName );

#if defined(BUILD_GOTHIC_1_CLASSIC)
    void InitAnimatedInventoryHooks();
    static void __fastcall hooked_RotateInInventory( DWORD oCItem );
#endif
};

namespace HookedFunctions {
    /** Holds all the original functions */
    inline HookedFunctionInfo OriginalFunctions = {};
};
