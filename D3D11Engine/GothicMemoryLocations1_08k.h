#pragma once

/** Holds all memorylocations for the selected game */
struct GothicMemoryLocations {
    struct Functions {
        static const unsigned int Alg_Rotation3DNRad = 0x005081A0;
        static const int vidGetFPSRate = 0x004EF790;
        static const unsigned int HandledWinMain = 0x004F3F60;
    };

    struct GlobalObjects {
        static const unsigned int zCResourceManager = 0x008CE9D0;
        static const unsigned int zCCamera = 0x00873240;
        static const unsigned int oCGame = 0x008DA6BC;
        static const unsigned int zCTimer = 0x008CF1E8;
        static const unsigned int DInput7DeviceMouse = 0x0086D2FC;
        static const unsigned int DInput7DeviceKeyboard = 0x0086D2F0;
        static const unsigned int zCInput = 0x0086CCA0;
        static const unsigned int s_globFreePart = 0x00874374;
        static const unsigned int zCOption = 0x00869694;
        static const unsigned int zRenderer = 0x008C5ED0;
        static const unsigned int screen = 0x008DE1BC;
        static const unsigned int sysEvents = 0x004F6AC0;
        static const unsigned int zCParser = 0x008dce08;
    };

    struct CGameManager {
        // CALL sysEvent inside CGameManager::Run()'s ingame do-while loop - the single
        // funnel point every loop iteration passes through (verified via Ghidra against
        // GothicMod.exe, base 1.08k build). Only valid for plain BUILD_GOTHIC_1_08k
        // without BUILD_1_12F - the 1.12f binary hasn't been checked, so this patch is
        // skipped there (see CGameManager.h::Hook()).
        static const unsigned int RunLoopSysEventCallSite = 0x00424f50;

        // Not located in this binary yet - the texture-detail refresh/purge in
        // CGameManager::ApplySomeSettings() stays vanilla here (see CGameManager.h).
        static const unsigned int ApplySettingsTexMaxSizeCheck = 0x0;
        static const unsigned int ApplySettingsTexMaxSizeSkip = 0x0;
    };

    struct zCParser {
        static const unsigned int CallFunc = 0x006e9690;
        static const unsigned int GetIndex = 0x006ea0c0;

        static const unsigned int DefineExternal = 0x006f6840;
        static const unsigned int GetParameter_Int = 0x006F6DF0;
        static const unsigned int GetParameter_Float = 0x006F6E00;
        static const unsigned int GetParameter_String = 0x006F6E40;
        static const unsigned int SetReturn_Int = 0x006F6FB0;
        static const unsigned int SetReturn_Float = 0x006F6FD0;
        static const unsigned int SetReturn_Instance = 0x006F6FF0;
        static const unsigned int SetReturn_String = 0x006F7020;
    };

    struct zCPolyStrip {
        static const unsigned int Offset_Material = 0x34;

        static const unsigned int SetVisibleSegments = 0x0059BE80;
        static const unsigned int AlignToCamera = 0x0059C980;
        static const unsigned int Render = 0x0059BF50;

        static const unsigned int RenderDrawPolyReturn = 0x0059C370;
    };

    struct zCFlash {
        static const unsigned int Destructor = 0x004C1080;
        static const unsigned int SetVisualUsedBy = 0x004C1350;
        static const unsigned int Update = 0x004C2390;

        static const unsigned int Offset_LifeTime = 0x34;
        static const unsigned int Offset_StartPosX = 0x50;
        static const unsigned int Offset_StartPosY = 0x54;
        static const unsigned int Offset_StartPosZ = 0x58;
        static const unsigned int Offset_EndPosX = 0x5C;
        static const unsigned int Offset_EndPosY = 0x60;
        static const unsigned int Offset_EndPosZ = 0x64;
        static const unsigned int Offset_BoltB = 0x4C;
        static const unsigned int Offset_BoltA = 0x48;
        static const unsigned int Offset_ChildrenTable = 0xA8;
        static const unsigned int Offset_ChildrenSize = 0xB0;
    };

    struct zCQuadMark {
        static const unsigned int Constructor = 0x005AB810;
        static const unsigned int Destructor = 0x005ABA60;
        static const unsigned int CreateQuadMark = 0x005ACF20;
        static const unsigned int Offset_QuadMesh = 0x34;
        static const unsigned int Offset_Material = 0x3C;
        static const unsigned int Offset_ConnectedVob = 0x38;
        static const unsigned int Offset_DontRepositionConnectedVob = 0x48;
    };

    struct zCRndD3D {
        static const unsigned int VidSetScreenMode = 0x0071FC70;
        static const unsigned int DrawLineZ = 0x00716D20;
        static const unsigned int DrawLine = 0x00716B00;
        static const unsigned int DrawPoly = 0x00714B60;
        static const unsigned int DrawPolySimple = 0x007143F0;
        static const unsigned int CacheInSurface = 0x0071A3E0;
        static const unsigned int CacheOutSurface = 0x0071A7F0;
        static const unsigned int XD3D_SetRenderState = 0x007185C0;
        static const unsigned int XD3D_SetTexture = 0x00718150;

        static const unsigned int RenderScreenFade = 0x005376F0;
        static const unsigned int RenderCinemaScope = 0x005377B0;

        static const unsigned int Offset_RenderState = 0x34;
        static const unsigned int Offset_BoundTexture = 0x80E38;
        static const unsigned int Offset_Width = 0x984;
        static const unsigned int Offset_Height = 0x988;

        static const unsigned int SetFog_Offset = 0x28;
        static const unsigned int GetFog_Offset = 0x2C;
        static const unsigned int SetBilerpFilterEnabled_Offset = 0x50;
        static const unsigned int GetBilerpFilterEnabled_Offset = 0x54;
        static const unsigned int GetZBufferWriteEnabled_Offset = 0x68;
        static const unsigned int SetZBufferWriteEnabled_Offset = 0x6C;
        static const unsigned int GetZBufferCompare_Offset = 0x70;
        static const unsigned int SetZBufferCompare_Offset = 0x74;
        static const unsigned int SetAlphaBlendFunc_Offset = 0x88;
        static const unsigned int GetAlphaBlendFunc_Offset = 0x8C;
        static const unsigned int Vid_Blit_Offset = 0xD8;
        static const unsigned int SetViewport_Offset = 0x134;
        static const unsigned int SetTextureStageState_Offset = 0x148;

        static const unsigned int DDRAW7 = 0x00929D54;
        static const unsigned int D3DDevice7 = 0x00929D5C;
    };

    struct zCOption {
        static const unsigned int GetDirectory = 0x0045FC00;
        static const unsigned int ReadInt = 0x0045CDB0;
        static const unsigned int ReadBool = 0x0045CB80;
        static const unsigned int ReadDWORD = 0x0045CEA0;
        static const unsigned int ReadReal = 0x0045cf90;
        static const unsigned int WriteString = 0x0045C9F0;
        static const unsigned int Offset_CommandLine = 0x284;
    };

    struct zFILE {
        static const unsigned int Open = 0x0043F450;
    };

    struct zCMesh {
        static const unsigned int Offset_Polygons = 0x44;
        static const unsigned int Offset_PolyArray = 0x50;
        static const unsigned int Offset_NumPolygons = 0x34;
        static const unsigned int CreateListsFromArrays = 0x00558AB0;
        static const unsigned int CreateTriMesh = 0x00554440;
    };

    struct zCMeshSoftSkin {
        //static const unsigned int Offset_NodeIndexList = 0x38C;
        static const unsigned int Offset_VertWeightStream = 0x0F8;
    };

    struct zCSkyController_Outdoor {
        static const unsigned int OBJ_ActivezCSkyController = 0x0099AC8C;

        // Stock zCSkyControler_Outdoor::RenderSkyPre. Never called - only compared against the live
        // controller's vtable slot to detect a derived controller. See HasDerivedRenderSkyPre().
        static const unsigned int RenderSkyPre = 0x005C0900;

        // zCSkyPlanet planets[2] — [0] sun, [1] moon. 0x5F4 is the GOTHIC 2 offset (see the 2_6_fix header);
        // G1's zCSkyControler_Outdoor lays out differently, verified 0x5DC against
        // ZenGin/Gothic_I_Classic/API/zSky.h (`zCSkyPlanet planets[NUM_PLANETS]; // offset 5DCh`), which also
        // agrees with this header's Offset_MasterTime/Offset_FarZ/Offset_OutdoorRainFXWeight.
        static const unsigned int Offset_Planets = 0x5DC;
        // float resultFogScale — fades the planets out on foggy days (zCSkyControler_Outdoor::RenderPlanets).
        static const unsigned int Offset_ResultFogScale = 0x560;
        static const unsigned int Offset_MasterTime = 0x6C;
        static const unsigned int Offset_LastMasterTime = 0x70;
        static const unsigned int Offset_MasterState = 0x74;
        /*static const unsigned int Offset_SkyLayer1 = 0x5A8;
        static const unsigned int Offset_SkyLayer2 = 0x5C4;
        static const unsigned int Offset_SkyLayerState = 0x5A0;
        static const unsigned int Offset_SkyLayerState0 = 0x120;
        static const unsigned int Offset_SkyLayerState1 = 0x124;
        static const unsigned int Interpolate = 0x005E8C20;*/

        static const unsigned int Offset_InitDone = 0x68;
        static const unsigned int Init = 0x005BCA80;

        static const unsigned int GetUnderwaterFX = 0x5baaa0;
        static const unsigned int Offset_FarZ = 0x56C;
        static const unsigned int Offset_Color = 0x580;

        static const unsigned int SetCameraLocationHint = 0x005BC7D0;

        static const unsigned int LOC_SunVisibleStart = 0x5C0D89;
        static const unsigned int LOC_SunVisibleEnd = 0x5C0D95;

        static const unsigned int LOC_ProcessRainFXNOPStart = 0x005C0FB0;
        static const unsigned int LOC_ProcessRainFXNOPEnd = 0x005C107D;

        static const unsigned int ProcessRainFX = 0x005C0DC0;
        static const unsigned int Offset_OutdoorRainFXWeight = 0x66C;
        static const unsigned int Offset_TimeStartRain = 0x678;
        static const unsigned int Offset_TimeStopRain = 0x67C;
    };

    struct zCSkyController {
        static const unsigned int VTBL_RenderSkyPre = 20; //0x50 / 4
        static const unsigned int VTBL_RenderSkyPost = 21;
        static const unsigned int static_skyEffectsEnabled = 0x8422A0; // int*
        static const unsigned int ClearBackground = 0x005ba2f0;
    };

    struct zCParticleEmitter {
        static const unsigned int Offset_VisIsQuadPoly = 0x190;
        static const unsigned int Offset_VisTexture = 0x2DC;
        static const unsigned int Offset_VisAlignment = 0x2E4;
        static const unsigned int Offset_VisAlphaBlendFunc = 0x308;
        static const unsigned int Offset_VisTexAniIsLooping = 0x198;

        static const unsigned int Offset_VisShpRender = 0xBC;
        static const unsigned int Offset_VisShpType = 0x258;
        static const unsigned int Offset_VisShpMesh = 0x26C;
    };

    struct zCStaticPfxList {
        static const unsigned int TouchPFX = 0x0058D590;
    };

    struct zERROR {
        // Start/End for problematic SendMessage-Broadcast which causes the game to conflict with other applications
        //static const unsigned int BroadcastStart = 0x0044CB2D;
        //static const unsigned int BroadcastEnd = 0x0044CB3C;
    };

    struct zCPathSearch {
        // Not located in this binary yet - stays vanilla here (see zCPathSearch.h).
        static const unsigned int CorrectPosForNearClip = 0x0;
    };

    struct zCCamera {
        static const unsigned int GetTransform = 0x00536460;
        static const unsigned int SetTransform = 0x00536300;
        static const unsigned int UpdateViewport = 0x00536850;
        static const unsigned int Activate = 0x005364C0;
        static const unsigned int SetFarPlane = 0x00536D30;
        static const unsigned int BBox3DInFrustum = 0x00536EF0;
        static const unsigned int SetFOV = 0x00536720;
        static const unsigned int GetFOV_f2 = 0x005366B0;

        static const unsigned int Offset_FarPlane = 0x8E0;
        static const unsigned int Offset_NearPlane = 0x8E4;
        static const unsigned int Offset_ScreenFadeEnabled = 0x8C0;
        static const unsigned int Offset_ScreenFadeColor = 0x8C4;
        static const unsigned int Offset_CinemaScopeEnabled = 0x8C8;
        static const unsigned int Offset_CinemaScopeColor = 0x8CC;
        static const unsigned int Offset_PolyMaterial = 0x8BC;
    };

    struct zCVob {
        static const unsigned int Offset_WorldMatrixPtr = 0x3C;
        //static const unsigned int Offset_BoundingBoxWS = 0x40;

        static const unsigned int SetCollDetStat = 0x005EF8D0;
        static const unsigned int s_ShowHelperVisuals = 0x008D75F4;
        static const unsigned int GetClassHelperVisual = 0x005D5D60;
        static const unsigned int s_renderVobs = 0x00843734;

        static const unsigned int GetVisual = 0x005E9A70;
        static const unsigned int SetVisual = 0x005D6E10;
        static const unsigned int GetPositionWorld = 0x005EE380;
        static const unsigned int Offset_GroundPoly = 0x0AC;
        static const unsigned int DoFrameActivity = 0x006DCC80;
        static const unsigned int GetBBoxLocal = 0x005EDCF0;
        static const unsigned int Offset_HomeWorld = 0x0A8;
        static const unsigned int Offset_Type = 0x0A0;

        static const unsigned int Offset_WindAniMode = 0xCC; // Hack into lightdirection variable unused in GD3D11 rendering pipeline
        static const unsigned int Offset_WindAniModeStrength = 0xD0; // Hack into lightdirection variable unused in GD3D11 rendering pipeline

        static const unsigned int Offset_Flags = 0xE4;
        static const unsigned int Offset_VobTree = 0x24;
        static const unsigned int Offset_VobAlpha = 0xBC;
        static const unsigned int MASK_ShowVisual = 0x1;
        static const unsigned int MASK_VisualAlpha = 0x4;
        static const unsigned int MASK_DynColl = 0x80; // (1 << 7) collDetectionDynamic
        static const unsigned int Offset_CameraAlignment = 0xF0;
        static const unsigned int SHIFTLR_CameraAlignment = 0x1E;

        static const unsigned int Destructor = 0x005D32A0;

        static const unsigned int Offset_WorldPosX = 0x48;
        static const unsigned int Offset_WorldPosY = 0x58;
        static const unsigned int Offset_WorldPosZ = 0x68;

        static const unsigned int Offset_WorldBBOX = 0x7C;
        static const unsigned int Offset_SleepingMode = 0xEC;
        static const unsigned int MASK_SkeepingMode = 3;

        static const unsigned int EndMovement = 0x005F0B60;
        static const unsigned int SetSleeping = 0x005D7250;
    };

    struct oCMob {
        static const unsigned int GetName = 0x0067aa50;
    };

    struct oCMobInter {
        static const unsigned int IsInteractingWith = 0x0067fc70;
    };

    struct zCInput {
        static const unsigned int GetDeviceEnabled = 0x004C8760;
        static const unsigned int SetDeviceEnabled = 0x004C8710;
        static const unsigned int ClearKeyBuffer = 0x004C8AE0;

        static const unsigned int GetKey_Offset = 0x2C;
        static const unsigned int ProcessInputEvents_Offset = 0x70;
    };

    struct zCInput_Win32 {
        static const unsigned int GetKey = 0x004c88a0;
    };

    struct zCVisual {
        static const unsigned int VTBL_GetFileExtension = 17;
        static const unsigned int Destructor = 0x005D9F10;
    };

    struct zCBspTree {
        static const unsigned int Offset_SectorList = 0x40;
        static const unsigned int Offset_PortalList = 0x4C;
        static const unsigned int AddVob = 0x0051E6F0;
        static const unsigned int LoadBIN = 0x00525330;
        static const unsigned int Offset_NumPolys = 0x24;
        static const unsigned int Offset_PolyArray = 0x10;
        static const unsigned int Offset_RootNode = 0x8;
        static const unsigned int Offset_WorldMesh = 0xC;
        static const unsigned int Offset_LeafList = 0x18;
        static const unsigned int Offset_NumLeafes = 0x20;
        static const unsigned int Offset_ArrPolygon = 0x4C;
        static const unsigned int Offset_BspTreeMode = 0x58;
        static const unsigned int Render = 0x0051D840;
    };

    /** Layout is identical across G1 1.08k, G1 1.12f and G2 2.6 - plain data, no code addresses. */
    struct zCBspSector {
        static const unsigned int Offset_SectorNodes = 0x14;
        static const unsigned int Offset_SectorIndex = 0x20;
        static const unsigned int Offset_SectorPortals = 0x24;
    };

    struct zCBspBase {
        static const unsigned int CollectPolysInBBox3D = 0x005200C0;
        static const unsigned int CheckRayAgainstPolys = 0x0051F180;
        static const unsigned int CheckRayAgainstPolysCache = 0x0051F440;
        static const unsigned int CheckRayAgainstPolysNearestHit = 0x0051F2A0;

    };

    struct zCPolygon {
        static const unsigned int Offset_PolyPlane = 0x08;
        static const unsigned int Offset_VerticesArray = 0x00;
        static const unsigned int Offset_FeaturesArray = 0x2C;
        static const unsigned int Offset_NumPolyVertices = 0x30;
        static const unsigned int Offset_PolyFlags = 0x31;
        static const unsigned int Offset_Material = 0x18;
        static const unsigned int Offset_Lightmap = 0x1C;

        static const unsigned int GetLightStatAtPos = 0x00597950;
        static const unsigned int CheckRayPolyIntersection = 0x00598060;
        static const unsigned int CheckRayPolyIntersection2Sided = 0x005983F0;
        static const unsigned int AllocVerts = 0x00599840;
        static const unsigned int CalcNormal = 0x00596540;

        static const unsigned int Constructor = 0x00595B00;
        static const unsigned int Destructor = 0x00595B30;

        static const unsigned int Size = 0x38;
    };

    struct zCThread {
        static const unsigned int SuspendThread = 0x005CE220;
        static const unsigned int Offset_SuspendCount = 0x0C;
    };

    struct zCBspLeaf {
        static const unsigned int Size = 0x5C;
    };

    struct zSTRING {
        static const unsigned int ToChar = 0x08;
        static const unsigned int ConstructorEmptyPtr = 0x00402B30;
        static const unsigned int ConstructorCharPtr = 0x004013A0;
        static const unsigned int DestructorCharPtr = 0x00401260;
        static const unsigned int OperatorAssignCharPtr = 0x004C5820;
        static const unsigned int OperatorAssignString = 0x0057E0C0;
    };

    struct zCModelPrototype {
        // zSTRING modelProtoFileName. Same offset in G1 and G2, classic and addon.
        static const unsigned int Offset_ModelProtoFileName = 0x20;
        static const unsigned int Offset_NodeList = 0x74;
    };

    struct zCModelTexAniState {
        static const unsigned int UpdateTexList = 0x0055D610;
    };

    struct zCMaterial {
        static const unsigned int Offset_BspSectorFront = 0x44;
        static const unsigned int Offset_BspSectorBack = 0x48;
        static const unsigned int Offset_Color = 0x38;
        static const unsigned int Offset_Texture = 0x34;
        static const unsigned int Offset_AlphaFunc = 0x70;
        static const unsigned int Offset_MatGroup = 0x40;
        static const unsigned int Offset_TexAniCtrl = 0x4C;

        static const unsigned int Offset_Flags = 0x6C;
        static const unsigned int Offset_TexAniMapDelta = 0x7C;
        static const unsigned int Mask_FlagTexAniMap = 0x4;

        static const unsigned int InitValues = 0x0054D3C0;
        static const unsigned int Constructor = 0x0054CFC0;
        static const unsigned int Destructor = 0x0054D230;
        static const unsigned int GetAniTexture = 0x007154C0;
        static const unsigned int AdvanceAni = 0x0054E9C0;

    };

    struct zCObject {
        //static const unsigned int Release = 0x0040C310;
        static const unsigned int GetObjectName = 0x0058A160;
    };

    struct zCTexture {
        static const unsigned int zCTex_D3DInsertTexture = 0x0071D7D0;
        static const unsigned int LoadResourceData = 0x005CA5E0;
        static const unsigned int Offset_CacheState = 0x4C;
        static const unsigned int Offset_NextFrame = 0x58;
        static const unsigned int Offset_ActAniFrame = 0x70;
        static const unsigned int Offset_AniFrames = 0x7C;
        static const unsigned int Mask_CacheState = 3;

        static const unsigned int Offset_Flags = 0x88;
        static const unsigned int Mask_FlagHasAlpha = 0x1;
        static const unsigned int Mask_FlagIsAnimated = 0x2;

        static const unsigned int zCResourceTouchTimeStamp = 0x005B5500;
        static const unsigned int zCResourceTouchTimeStampLocal = 0x005B5580;

        static const unsigned int Offset_Surface = 0xD4;

        static const unsigned int PrecacheTexAniFrames = 0x005C8AB0;
    };

    struct zCBinkPlayer {
        static const unsigned int PlayInit = 0x00469600;
        static const unsigned int PlayDeinit = 0x00469650;
        static const unsigned int OpenVideo = 0x00469280;
        static const unsigned int CloseVideo = 0x004693F0;
        static const unsigned int Offset_IsLooping = 0x18;
        static const unsigned int Offset_IsPaused = 0x1C;
        static const unsigned int Offset_IsPlaying = 0x20;
        static const unsigned int Offset_SoundOn = 0x24;
        static const unsigned int Offset_VideoHandle = 0x30;
        static const unsigned int Offset_DoHandleEvents = 0x40;
        static const unsigned int Offset_DisallowInputHandling = 0x84;

        static const unsigned int Stop_Offset = 0x20;

        static const unsigned int PlayHandleEvents_Func = 0x004223D0;
        static const unsigned int PlayHandleEvents_Vtable = 0x007D1014;
        static const unsigned int SetSoundVolume_Func = 0x0043CB80;
        static const unsigned int SetSoundVolume_Vtable = 0x007D1008;
        static const unsigned int ToggleSound_Func = 0x0043CB30;
        static const unsigned int ToggleSound_Vtable = 0x007D1004;
        static const unsigned int Pause_Func = 0x0043C960;
        static const unsigned int Pause_Vtable = 0x007D0FF4;
        static const unsigned int Unpause_Func = 0x0043C980;
        static const unsigned int Unpause_Vtable = 0x007D0FF8;
        static const unsigned int IsPlaying_Func = 0x0043C9B0;
        static const unsigned int IsPlaying_Vtable = 0x007D1000;
        static const unsigned int PlayGotoNextFrame_Func = 0x0043C760;
        static const unsigned int PlayGotoNextFrame_Vtable = 0x007D100C;
        static const unsigned int PlayWaitNextFrame_Func = 0x0043C770;
        static const unsigned int PlayWaitNextFrame_Vtable = 0x007D1010;
        static const unsigned int PlayFrame_Func = 0x0043C7B0;
        static const unsigned int PlayFrame_Vtable = 0x007D14C4;
        static const unsigned int PlayInit_Func = 0x0043B460;
        static const unsigned int PlayInit_Vtable = 0x007D14C0;
        static const unsigned int PlayDeinit_Func = 0x0043BFB0;
        static const unsigned int PlayDeinit_Vtable = 0x007D14C8;
        static const unsigned int OpenVideo_Func = 0x0043A660;
        static const unsigned int OpenVideo_Vtable = 0x007D14B8;
        static const unsigned int CloseVideo_Func = 0x0043B1D0;
        static const unsigned int CloseVideo_Vtable = 0x007D0FE4;
    };

    struct oCSpawnManager {
        static const unsigned int SpawnNpc = 0x006D0710;
        static const unsigned int CheckInsertNpc = 0x006CFDE0;
        static const unsigned int CheckRemoveNpc = 0x006D0A80;
    };

    struct zCDecal {
        static const unsigned int Offset_DecalSettings = 0x34;
        //static const unsigned int GetAlphaTestEnabled = 0x00556BE0;
    };

    struct zCResourceManager {
        static const unsigned int CacheIn = 0x005B5CB0;
        static const unsigned int PurgeCaches = 0x005B5720;
        static const unsigned int RefreshTexMaxSize = 0x005C9F70;
        static const unsigned int SetThreadingEnabled = 0x005B58A0;
        // zCResourceManager::zCClassCache::InsertRes/TouchRes/RemoveRes - hooked to serialize class-cache
        // list mutation, see zCResourceManager.h.
        static const unsigned int InsertRes = 0x005B6E40;
        static const unsigned int TouchRes = 0x005B6F10;
        static const unsigned int RemoveRes = 0x005B6EA0;

        // zCResourceManager derives from zCThread (0x18 bytes), classCacheList is the zCArray that
        // follows it: [+0x18] = array, [+0x1C] = numAlloc, [+0x20] = numInArray.
        static const unsigned int Offset_ClassCacheList = 0x18;
        static const unsigned int Offset_ClassCacheCount = 0x20;
        // volatile zBOOL goToSuspend - the flag PurgeCaches uses to park the loader thread
        static const unsigned int Offset_GoToSuspend = 0x60;
        static const unsigned int VTBL_IsThreadRunning = 0x0C;

        // zCResourceManager::zCClassCache
        static const unsigned int SizeOf_ClassCache = 0x1C;
        static const unsigned int Offset_ClassCache_ResClassDef = 0x00;
        static const unsigned int Offset_ClassCache_ResListHead = 0x04;
    };

    struct zCResource {
        // zCObject base, then nextRes/prevRes/timeStamp, then the zCCriticalSection stateChangeGuard
        // (vtbl + CRITICAL_SECTION = 0x1C bytes), then the bitfield.
        static const unsigned int Offset_RefCtr = 0x04;
        static const unsigned int Offset_NextRes = 0x24;
        static const unsigned int Offset_StateChangeGuard = 0x30;
        // bits 0-1: cacheState (zTResourceCacheState), bit 2: cacheOutLock
        static const unsigned int Offset_Flags = 0x4C;
        // bit 0: managedByResMan
        static const unsigned int Offset_Flags_ManagedByResMan = 0x4E;
    };

    struct zCProgMeshProto {
        static const unsigned int Offset_PositionList = 0x34;
        static const unsigned int Offset_NormalsList = 0x3C;
        static const unsigned int Offset_Submeshes = 0xA4;
        static const unsigned int Offset_NumSubmeshes = 0xA8;
    };

    struct zCWorld {
        static const unsigned int Render = 0x005F3EC0;
        static const unsigned int LoadWorld = 0x005F8A00;
        static const unsigned int VobRemovedFromWorld = 0x005F64C0;
        static const unsigned int VobAddedToWorld = 0x005F6360;
#ifdef BUILD_SPACER_NET
        static const unsigned int CompileWorld = 0x005F4F90;
        static const unsigned int GenerateStaticWorldLighting = 0x00600B60;
#endif
        static const unsigned int Offset_GlobalVobTree = 0x24;
        static const unsigned int Call_Render_zCBspTreeRender = 0x005F3F95;
        //static const unsigned int GetActiveSkyController = 0x006060A0;
        static const unsigned int Offset_SkyControllerOutdoor = 0x0D0;
        static const unsigned int DisposeVobs = 0x005F55F0;
        static const unsigned int Offset_BspTree = 0x198;
        static const unsigned int RemoveVob = 0x005F66C0;
    };

    struct oCWorld {
        static const unsigned int EnableVob = 0x006D7130;
        static const unsigned int DisableVob = 0x006D7250;
        static const unsigned int RemoveFromLists = 0x006D7750;
        static const unsigned int RemoveVob = 0x006D6EF0;
    };

    struct zCBspNode {
        /*static const unsigned int RenderIndoor = 0x0052A3E0;
        static const unsigned int RenderOutdoor = 0x0052A7B0;
        static const unsigned int REPL_RenderIndoorEnd = 0x0052A47D;

        static const unsigned int REPL_RenderOutdoorStart = 0x0052A7B0;
        static const unsigned int REPL_RenderOutdoorEnd = 0x0052A9BA;
        static const unsigned int SIZE_RenderOutdoor = 0x252;*/

    };

    struct oCNPC {
        static const unsigned int ResetPos = 0x0074CED0;
        static const unsigned int Enable = 0x006A2000;
        static const unsigned int Disable = 0x006A1D20;
        static const unsigned int InitModel = 0x00695020;
        static const unsigned int IsAPlayer = 0x0069E9D0;
        static const unsigned int GetName = 0x0068D0B0;
        static const unsigned int HasFlag = 0x0068E150;
        static const unsigned int Offset_focus_vob = 0x9e4;
        static const unsigned int Offset_states = 0x470;
        static const unsigned int GetInvSlot_zString = 0x006a5770;
    };
    
    struct oCNpc_States {
        static const unsigned int IsInState = 0x006c4c90;
    };

    struct oCGame {
        static const unsigned int EnterWorld = 0x0063EAD0;
        static const unsigned int TestKeys = 0x00660000;
        static const unsigned int Var_Player = 0x008DBBB0;
        static const unsigned int Offset_GameView = 0x30;
        static const unsigned int Offset_SingleStep = 0x118;
        static const unsigned int DefineExternals_Ulfi = 0x006495b0;
    };

    struct zCViewDraw {
        static const unsigned int GetScreen = 0x00753A60;
        static const unsigned int SetVirtualSize = 0x00755410;
    };

    struct zCView {
        static const unsigned int SetMode = 0x00702180;
        static const unsigned int Vid_SetMode = 0x005AE970;
        static const unsigned int REPL_SetMode_ModechangeStart = 0x007021A9;
        static const unsigned int REPL_SetMode_ModechangeEnd = 0x0007021B8;
        static const unsigned int PrintTimed = 0x006FE1A0;
        static const unsigned int PrintChars = 0x006FFF80;
        static const unsigned int CreateText = 0x007006E0;
        static const unsigned int BlitText = 0x006FC7B0;
        static const unsigned int Print = 0x006FFEB0;
        static const unsigned int Blit = 0x006FC8C0;
    };

    struct zCVobLight {
        static const unsigned int Offset_LightColor = 0x120; // Right before Range
        static const unsigned int Offset_Range = 0x124;
        static const unsigned int Offset_LightInfo = 0x144;
        static const unsigned int Mask_LightEnabled = 0x20;
        static const unsigned int Offset_IsStatic = 0x164;
        static const unsigned int DoAnimation = 0x005DB820;
    };

    struct zCMorphMesh {
        static const unsigned int Offset_MorphMesh = 0x38;
        static const unsigned int Offset_TexAniState = 0x40;
        // Shared morph prototype + optional refShape ani. CalcVertPositions zeroes posList, sums the
        // active ani deltas, then adds refShapeAni->vertPosMatrix (or morphRefMeshVertPos) as the base -
        // with no active channels that base IS the rest pose, readable without touching engine state.
        static const unsigned int Offset_MorphProto = 0x34;
        static const unsigned int Offset_RefShapeAni = 0x3C;
        // zCMorphMeshProto members (the per-.MMS shared prototype).
        static const unsigned int Proto_Offset_Name = 0x0C;
        static const unsigned int Proto_Offset_MorphRefMesh = 0x20;
        static const unsigned int Proto_Offset_MorphRefMeshVertPos = 0x24;
        // zCArraySort<zCMorphMeshAni*> - every ani this .MMS can play, written once at load. Same
        // ArraySort_Offset_* layout as the channel array below.
        static const unsigned int Proto_Offset_AniList = 0x28;
        // zCMorphMeshAni members (entries of zCMorphMeshProto::aniList, also shared).
        static const unsigned int Ani_Offset_NumVert = 0x40;
        static const unsigned int Ani_Offset_VertPosMatrix = 0x4C;
        static const unsigned int Ani_Offset_NumFrames = 0x48;
        // zCMorphMeshAni::flags bitfield byte: bit0 discontinuity, bit1 looping, bit2 shape, bit3 refShape.
        static const unsigned int Ani_Offset_Flags = 0x3C;
        static const unsigned int Ani_Mask_Shape = 0x04;
        static const unsigned int Ani_Offset_VertIndexList = 0x44;
        // Active blend channels: zCArraySort<zTMorphAniEntry*>. NOTE the layout is
        // { T* array; int numAlloc; int numInArray; } - the COUNT is the third dword, not the second.
        // zCArrayAdapt reads the second, which is fine for file-loaded arrays (numAlloc == numInArray)
        // but wrong here: this array grows and shrinks as anis start and finish.
        static const unsigned int Offset_AniChannels = 0x6C;
        static const unsigned int ArraySort_Offset_Array = 0x00;
        static const unsigned int ArraySort_Offset_NumInArray = 0x08;
        // zTMorphAniEntry (sizeof 0x2C)
        static const unsigned int Entry_Offset_Ani = 0x00;
        static const unsigned int Entry_Offset_Weight = 0x04;
        static const unsigned int Entry_Offset_ActFrameInt = 0x10;
        static const unsigned int Entry_Offset_NextFrameInt = 0x14;
        static const unsigned int Entry_Offset_Frac = 0x18;
        static const unsigned int AdvanceAnis = 0x00586E90;
        static const unsigned int CalcVertexPositions = 0x00586AE0;

    };

    struct zCParticleFX {
        static const unsigned int Offset_FirstParticle = 0x34;
        static const unsigned int Offset_TimeScale = 0x8C;
        static const unsigned int Offset_LocalFrameTimeF = 0x90; // Offset_TimeScale + 4
        static const unsigned int Offset_PrivateTotalTime = 0x84; // Offset_TimeScale - 8
        static const unsigned int Offset_LastTimeRendered = 0x88;
        static const unsigned int Offset_Emitters = 0x54;
        static const unsigned int Offset_ConnectedVob = 0x70;

        static const unsigned int OBJ_s_pfxList = 0x00874360;
        static const unsigned int OBJ_s_partMeshQuad = 0x0087437C;
        static const unsigned int CheckDependentEmitter = 0x005913C0;
        static const unsigned int UpdateParticle = 0x0058F4C0;

        //static const unsigned int OBJ_s_pfxList = 0x008D9214;

        static const unsigned int CreateParticlesUpdateDependencies = 0x0058F210;
        static const unsigned int SetVisualUsedBy = 0x0058DD60;
        static const unsigned int Destructor = 0x0058D340;
        static const unsigned int UpdateParticleFX = 0x0058F130;
        static const unsigned int GetVisualDied = 0x0058D2F0;
    };

    struct zCModel {
        static const unsigned int RenderNodeList = 0x0055FA50;
        static const unsigned int UpdateAttachedVobs = 0x005667F0;
        static const unsigned int SearchNode = 0x00563F80;
        static const unsigned int Offset_HomeVob = 0x54;
        static const unsigned int Offset_ModelProtoList = 0x58;
        static const unsigned int Offset_NodeList = 0x64;
        static const unsigned int Offset_MeshSoftSkinList = 0x70;
        static const unsigned int Offset_MeshLibList = 0xB0;
        static const unsigned int Offset_AttachedVobList = 0x8C;
        static const unsigned int Offset_Flags = 0x1CC; // was previously 0x1F8
        //static const unsigned int Offset_DrawHandVisualsOnly = 0x174;
        static const unsigned int Offset_ModelFatness = 0x118;
        static const unsigned int Offset_ModelScale = 0x11C;
        static const unsigned int Offset_DistanceModelToCamera = 0x114;
        static const unsigned int Offset_NumActiveAnis = 0x34;
        static const unsigned int GetVisualName = 0x00563EF0;
    };

    struct zCClassDef {
        static const unsigned int zCVobSound = 0x008d82a8;
        static const unsigned int oCItem = 0x008daa80;
        static const unsigned int zCZone = 0x008d8388;
        static const unsigned int oCTriggerChangeLevel = 0x0085ed88;
        static const unsigned int zCObject = 0x00873e70;
        static const unsigned int zCVob = 0x008d72d8;
        static const unsigned int oCCSTrigger = 0x0085e210;
        static const unsigned int zCVobSoundDaytime = 0x008d8468;
        static const unsigned int oCMobInter = 0x008db060;
        static const unsigned int zCVobLensFlare = 0x008d79b8;
        static const unsigned int zCVobStartpoint = 0x008de2c0;
        static const unsigned int oCMobSwitch = 0x008daea0;
        static const unsigned int zCVobLight = 0x008d7610;
        static const unsigned int zCEffect = 0x008d7be8;
        static const unsigned int oCVob = 0x008dc418;
        static const unsigned int zCVobSpot = 0x008de3a0;
        static const unsigned int zCPFXControler = 0x008d7948;
        static const unsigned int oCMobContainer = 0x008daf80;
        static const unsigned int oCMobLockable = 0x008dacb0;
        static const unsigned int oCMOB = 0x008db0d0;
        static const unsigned int zCVobAnimate = 0x008d7c58;
        static const unsigned int oCMobFire = 0x008dada0;
        static const unsigned int oCTriggerScript = 0x0085ed08;
        static const unsigned int oCNpc = 0x008db408;
        static const unsigned int oCMobLadder = 0x008daf10;
        static const unsigned int zCTrigger = 0x008d7b78;
        static const unsigned int zCTriggerBase = 0x008d7a98;
        static const unsigned int oCVisualFX = 0x00869e00;
        static const unsigned int oCZoneMusic = 0x008de498;
        static const unsigned int zCMover = 0x008d7868;
        static const unsigned int oCMobDoor = 0x008dac40;
        static const unsigned int oCZoneMusicDefault = 0x008de420;
        static const unsigned int zCVobScreenFX = 0x008d7d38;
        static const unsigned int zCZoneZFogDefault = 0x008d83f8;
        static const unsigned int oCMobBed = 0x008dabc8;
        static const unsigned int zCZoneZFog = 0x008d8318;
        static const unsigned int zCZoneVobFarPlaneDefault = 0x008d8548;
        static const unsigned int oCMobWheel = 0x008dab58;
        static const unsigned int zCZoneVobFarPlane = 0x008d8698;
        static const unsigned int zCTriggerList = 0x008d7f68;
        static const unsigned int zCVobStair = 0x008d7708;
        static const unsigned int zCTexture = 0x008CF110;
    };

    struct oCInformationManager
    {
        static const unsigned int GetInformationManager = 0x0072ABD0;
        static const unsigned int IsDoneOffset = 0x2C;
    };

    struct oCItemContainer {
        static const unsigned int s_Container_Draw = 0x00666250;
    };

    class VobTypes // vftables
    {
    public:
        static const unsigned int
            Npc = 0x7DDF34,
            Mob = 0x7DDCFC,
            Item = 0x7DD0CC,
            Mover = 0x7DBFAC,
            MobFire = 0x7DD9E4,
            VobLight = 0x7DB534;
    protected:
        VobTypes() {}
    };
    
    
    struct zFILE_VDFS
    {
        static const unsigned int StructSize = 0x2a24;

        static const unsigned int Constructor2 = 0x00444d90;
        static const unsigned int Destructor = 0x00444e40;
        static const unsigned int zCObjectFactory__CreateZFile = 0x0058bd60;
    };

    struct oCVisualFX
    {
        static const unsigned int Offset_origin = 0x458;
        static const unsigned int Offset_orgNode = 0x44c;
        static const unsigned int Offset_emTrjOriginNode_S = 0x15c;
        static const unsigned int Offset_emAdjustShpToOrigin = 0x29c;
    };
    
#define zALLOCATOR_SUPPORTED
    struct zAllocator
    {
        static const unsigned int Malloc = 0x007d02f8;
        static const unsigned int Free = 0x007d02ec;
    };
};
