#pragma once

/** Holds all memorylocations for the selected game */
struct GothicMemoryLocations {
    struct Functions {
        static const unsigned int Alg_Rotation3DNRad = 0x00517E90;
        static const unsigned int vidGetFPSRate = 0x004FDCD0;
        static const unsigned int HandledWinMain = 0x00502ED0;
        static const unsigned int WinMain = 0x00502D70;
        //static const unsigned int ExitGameFunc = 0x00425F30;
        static const unsigned int zCExceptionHandler_UnhandledExceptionFilter = 0x004C88C0;
    };

    struct zCQuadMark {
        static const unsigned int Constructor = 0x005D0970;
        static const unsigned int Destructor = 0x005D0B90;
        static const unsigned int CreateQuadMark = 0x005D2030;
        static const unsigned int Offset_QuadMesh = 0x34;
        static const unsigned int Offset_Material = 0x3C;
        static const unsigned int Offset_ConnectedVob = 0x38;
        static const unsigned int Offset_DontRepositionConnectedVob = 0x48;
    };

    struct zCPolyStrip {

        static const unsigned int Offset_Material = 0x34;

        static const unsigned int SetVisibleSegments = 0x005BDB90;
        static const unsigned int AlignToCamera = 0x005BE800;
        static const unsigned int Render = 0x005BDC70;

        static const unsigned int RenderDrawPolyReturn = 0x005BE190;
    };

    struct zCThread {
        static const unsigned int SuspendThread = 0x005F9370;
        static const unsigned int Offset_SuspendCount = 0x0C;
    };

    struct zCActiveSnd {
        static const unsigned int AutoCalcObstruction = 0x004F9830;
    };

    struct zCBinkPlayer {
        static const unsigned int GetPixelFormat = 0x00440790;
        static const unsigned int OpenVideo = 0x0043E0F0;
        static const unsigned int Offset_VideoHandle = 0x30;
    };

    struct CGameManager {
        static const unsigned int ExitGame = 0x00425780;
    };

    struct zCOption {
        static const unsigned int ReadInt = 0x00462390;
        static const unsigned int ReadBool = 0x00462160;
        static const unsigned int ReadDWORD = 0x004624F0;
        static const unsigned int Offset_CommandLine = 0x284;
    };

    struct zCRndD3D {
        static const unsigned int VidSetScreenMode = 0x00658BA0;
        static const unsigned int DrawLineZ = 0x0064DB00;
        static const unsigned int DrawLine = 0x0064d8e0;
        static const unsigned int DrawPoly = 0x0064B260;
        static const unsigned int DrawPolySimple = 0x0064AC30;
        static const unsigned int Vid_GetGammaCorrection = 0x00659610;

        static const unsigned int Offset_RenderState = 0x38;
        static const unsigned int Offset_BoundTexture = 0x82E50;
    };

    struct zERROR {
        // Start/End for problematic SendMessage-Broadcast which causes the game to conflict with other applications
        static const unsigned int BroadcastStart = 0x0044C5D5;
        static const unsigned int BroadcastEnd = 0x0044C5E4;
    };

    struct oCGame {
        static const unsigned int EnterWorld = 0x006C96F0;
        static const unsigned int TestKeys = 0x006FD560;
        static const unsigned int Var_Player = 0x00AB2684;
        static const unsigned int Offset_GameView = 0x30;
        static const unsigned int Offset_SingleStep = 0x11C;
    };

    struct oCNPC {
        static const unsigned int ResetPos = 0x006824D0;
        static const unsigned int InitModel = 0x00738480;
        static const unsigned int Enable = 0x00745D40;
        static const unsigned int Disable = 0x00745A20;
        static const unsigned int IsAPlayer = 0x007425A0;
        static const unsigned int GetName = 0x0072F820;
        static const unsigned int HasFlag = 0x007309E0;
    };
    
    struct zCViewDraw {
        static const unsigned int GetScreen = 0x006905c0;
        static const unsigned int SetVirtualSize = 0x00691f60;
    };

    struct zCView {
        static const unsigned int Vtbl_ViewText = 0x0083E344;
        static const unsigned int SetMode = 0x007ABDB0;
        static const unsigned int REPL_SetMode_ModechangeStart = 0x007ABDD9;
        static const unsigned int REPL_SetMode_ModechangeEnd = 0x007ABDE8;
        static const unsigned int PrintTimed = 0x007A7D20;
        static const unsigned int PrintChars = 0x007A9B10;
        static const unsigned int CreateText = 0x007AA2B0;
        static const unsigned int BlitText = 0x007A62A0;
        static const unsigned int Print = 0x007A9A40;
        static const unsigned int Blit = 0x007a63c0;
    };

    struct zCObject {
        //static const unsigned int Release = 0x0040C310;
        static const unsigned int GetObjectName = 0x005A9CD0;
        static const unsigned int VTBL_ScalarDestructor = 0x12 / 4;
    };

    struct zCSkyController_Outdoor {
        static const unsigned int OBJ_ActivezCSkyController = 0x0099AC8C;

        static const unsigned int Offset_Sun = 0x5F4; // First of the two planets
        static const unsigned int Offset_MasterTime = 0x80;
        static const unsigned int Offset_MasterState = 0x88;
        static const unsigned int Offset_SkyLayer1 = 0x5A8;
        static const unsigned int Offset_SkyLayer2 = 0x5C4;
        static const unsigned int Offset_SkyLayerState = 0x5A0;
        static const unsigned int Offset_SkyLayerState0 = 0x120;
        static const unsigned int Offset_SkyLayerState1 = 0x124;
        static const unsigned int Offset_OverrideColor = 0x558;
        static const unsigned int Offset_OverrideFlag = 0x564;
        static const unsigned int Interpolate = 0x005E8C20;
        static const unsigned int Offset_InitDone = 0x7C;
        static const unsigned int Init = 0x005E6A00;
        static const unsigned int GetUnderwaterFX = 0x005E0050;
        static const unsigned int Offset_WeatherType = 0x30;
        static const unsigned int SetCameraLocationHint = 0x005E6790;

        static const unsigned int LOC_ProcessRainFXNOPStart = 0x005EB149;
        static const unsigned int LOC_ProcessRainFXNOPEnd = 0x005EB216;

        static const unsigned int LOC_SunVisibleStart = 0x005E7C8F;
        static const unsigned int LOC_SunVisibleEnd = 0x005E7E7F;

        static const unsigned int ProcessRainFX = 0x005EAF30;
        static const unsigned int Offset_OutdoorRainFXWeight = 0x69C;
    };

    struct zCSkyController {
        static const unsigned int VTBL_RenderSkyPre = 27; //0x6C / 4
        static const unsigned int VTBL_RenderSkyPost = 28;
    };

    struct zCParticleEmitter {
        static const unsigned int Offset_VisTexture = 0x318;
        static const unsigned int Offset_VisAlignment = 0x31C;
        static const unsigned int Offset_VisAlphaBlendFunc = 0x340;
        static const unsigned int Offset_AlphaDist = 0x35C;
        static const unsigned int Offset_VisAlphaStart = 0x1F0;
        static const unsigned int Offset_VisTexAniIsLooping = 0x198;
        static const unsigned int Offset_VisIsQuadPoly = 0x190;

    };

    struct zCMesh {
        static const unsigned int Offset_Polygons = 0x44;
        static const unsigned int Offset_NumPolygons = 0x34;
    };

    struct zCParticleFX {
        static const unsigned int Offset_FirstParticle = 0x34;
        static const unsigned int Offset_TimeScale = 0x8C;
        static const unsigned int Offset_LocalFrameTimeF = 0x90; // Offset_TimeScale + 4
        static const unsigned int Offset_PrivateTotalTime = 0x84; // Offset_TimeScale - 8
        static const unsigned int Offset_Emitters = 0x54;
        static const unsigned int Offset_LastTimeRendered = 0x88;

        static const unsigned int Offset_ConnectedVob = 0x70;
        static const unsigned int UpdateParticle = 0x005AF500;
        static const unsigned int OBJ_s_pfxList = 0x008D9214;
        static const unsigned int OBJ_s_partMeshQuad = 0x008D9230;
        static const unsigned int CreateParticlesUpdateDependencies = 0x005AF240;
        static const unsigned int SetVisualUsedBy = 0x005B1DD0;
        static const unsigned int GetNumParticlesThisFrame = 0x005B1A90;
        static const unsigned int UpdateParticleFX = 0x005AF160;
        static const unsigned int CheckDependentEmitter = 0x005B1C30;

        static const unsigned int Offset_PrevPFX = 0x80; // PrivateTotalTime - 4
        static const unsigned int Offset_NextPFX = 0x7C; // PrivateTotalTime - 8
        static const unsigned int Destructor = 0x005AD0E0;
        static const unsigned int GetVisualDied = 0x005AD090;
    };

    struct zCStaticPfxList {
        static const unsigned int TouchPFX = 0x005AD330;
    };

    struct zCInput {
        static const unsigned int GetDeviceEnabled = 0x004D5160;
        static const unsigned int SetDeviceEnabled = 0x004D5100;
    };

    struct GlobalObjects {
        static const unsigned int zCResourceManager = 0x0099AB30;
        static const unsigned int zCCamera = 0x008D7F94;
        static const unsigned int oCGame = 0x00AB0884;
        static const unsigned int zCTimer = 0x0099B3D4;
        static const unsigned int s_globFreePart = 0x008D9228;
        static const unsigned int DInput7DeviceMouse = 0x008D1D70;
        static const unsigned int DInput7DeviceKeyboard = 0x008D1D64;
        static const unsigned int zCInput = 0x008D1650;
        static const unsigned int zCOption = 0x008CD988;
        static const unsigned int zCParser = 0xAB40C0;
        static const unsigned int zRenderer = 0x00982F08;
        static const unsigned int zSound = 0x0099B03C;
        static const unsigned int screen = 0x00ab6468;

        static const unsigned int NOP_FreelookWindowedCheckStart = 0x004816D7;
        static const unsigned int NOP_FreelookWindowedCheckEnd = 0x004816DB;
    };

    struct zCSoundSystem {
        static const unsigned int VTBL_SetGlobalReverbPreset = 0x58 / 4;
    };

    struct zCParser {
        static const unsigned int CallFunc = 0x7929F0;
    };

    struct zCMorphMesh {
        static const unsigned int Offset_MorphMesh = 0x38;
        static const unsigned int Offset_TexAniState = 0x40;
        static const unsigned int AdvanceAnis = 0x005A6830;
        static const unsigned int CalcVertexPositions = 0x005A6480;
    };

    struct zCMeshSoftSkin {
        //static const unsigned int Offset_NodeIndexList = 0x38C;
        static const unsigned int Offset_VertWeightStream = 0x0FC;
    };

    struct zCModelPrototype {
        static const unsigned int Offset_NodeList = 0x64;
        static const unsigned int Offset_MeshSoftSkinList = 0x64 + 16;
        static const unsigned int LoadModelASC = 0x0059E760;
        static const unsigned int ReadMeshAndTreeMSB = 0x00593180;

    };

    struct zCModelMeshLib {
        static const unsigned int Offset_NodeList = 0xB * 4;
        static const unsigned int Offset_MeshSoftSkinList = 0xC * 4;
    };

    struct zCBspLeaf {
        static const unsigned int Size = 0x5C;
    };

    struct zCVobLight {
        static const unsigned int Offset_LightColor = 0x140; // Right before Range
        static const unsigned int Offset_Range = 0x144;
        static const unsigned int Offset_LightInfo = 0x164;
        static const unsigned int Offset_IsStatic = 0x164;
        static const unsigned int Mask_LightEnabled = 0x20;
        static const unsigned int DoAnimation = 0x006081C0;
    };

    struct zFILE {
        static const unsigned int Open = 0x00448D30;
    };

    struct zCBspNode {
        static const unsigned int RenderIndoor = 0x0052F0E0;
        static const unsigned int RenderOutdoor = 0x0052F490;

        static const unsigned int REPL_RenderIndoorEnd = 0x0052F153;

        static const unsigned int REPL_RenderOutdoorStart = 0x0052F495;
        static const unsigned int REPL_RenderOutdoorEnd = 0x0052F69E;
        static const unsigned int SIZE_RenderOutdoor = 0x211;

    };

    struct zCModelTexAniState {
        static const unsigned int UpdateTexList = 0x00576DA0;
    };

    struct zCModel {
        static const unsigned int RenderNodeList = 0x00579560;
        static const unsigned int UpdateAttachedVobs = 0x00580900;
        static const unsigned int Offset_ModelProtoList = 0x64;
        static const unsigned int Offset_NodeList = 0x70;
        static const unsigned int Offset_MeshSoftSkinList = 0x7C;
        static const unsigned int Offset_MeshLibList = 0xBC;
        static const unsigned int Offset_AttachedVobList = 0x98;
        static const unsigned int AdvanceAnis = 0x0057CA90;
        static const unsigned int SIZE_AdvanceAnis = 0x119E;
        static const unsigned int RPL_AniQuality = 0x0057CCC1;
        static const unsigned int Offset_DrawHandVisualsOnly = 0x174;
        static const unsigned int Offset_ModelFatness = 0x128;
        static const unsigned int Offset_ModelScale = 0x12C;
        static const unsigned int Offset_Flags = 0x1F8;
        static const unsigned int Offset_DistanceModelToCamera = 0x120;
        static const unsigned int Offset_NumActiveAnis = 0x34;
        static const unsigned int Offset_AniChannels = 0x38;
        static const unsigned int GetVisualName = 0x0057DF60;
    };

    struct zCModelAni {
        static const unsigned int Offset_NumFrames = 0xD8;
        static const unsigned int Offset_Flags = 0xE0;
        static const unsigned int Mask_FlagIdle = 0x10;
    };

    struct zCCamera {
        static const unsigned int GetTransform = 0x0054A6A0;
        static const unsigned int SetTransform = 0x0054A540;
        static const unsigned int UpdateViewport = 0x0054AA90;
        static const unsigned int Activate = 0x0054A700;
        static const unsigned int Offset_NearPlane = 0x900;
        static const unsigned int Offset_FarPlane = 0x8FC;
        static const unsigned int SetFarPlane = 0x0054B200;
        static const unsigned int BBox3DInFrustum = 0x0054B410;
        static const unsigned int Var_FreeLook = 0x008CE42C;
        static const unsigned int SetFOV = 0x0054A960;
        static const unsigned int GetFOV_f2 = 0x0054A8F0;
    };

    struct zCProgMeshProto {
        static const unsigned int Offset_PositionList = 0x34;
        static const unsigned int Offset_NormalsList = 0x38;
        static const unsigned int Offset_Submeshes = 0xA4;
        static const unsigned int Offset_NumSubmeshes = 0xA8;
    };

    struct zCVob {
        static const unsigned int Offset_WorldMatrixPtr = 0x3C;
        //static const unsigned int Offset_BoundingBoxWS = 0x40;

        static const unsigned int SetCollDetStat = 0x0061CE50;
        static const unsigned int s_ShowHelperVisuals = 0x009A37F4;
        static const unsigned int GetClassHelperVisual = 0x006011E0;

        static const unsigned int GetVisual = 0x00616B20;
        static const unsigned int SetVisual = 0x006024F0;
        static const unsigned int GetPositionWorld = 0x0052DC90;
        static const unsigned int GetBBoxLocal = 0x0061B1F0;
        static const unsigned int Offset_HomeWorld = 0x0B8;
        static const unsigned int Offset_GroundPoly = 0x0BC;
        static const unsigned int Offset_Flags = 0x104;
        static const unsigned int MASK_ShowVisual = 0x1;
        static const unsigned int Offset_CameraAlignment = 0x110;
        static const unsigned int SHIFTLR_CameraAlignment = 0x1E;

        static const unsigned int Destructor = 0x005FE470;

        static const unsigned int Offset_WorldPosX = 0x48;
        static const unsigned int Offset_WorldPosY = 0x58;
        static const unsigned int Offset_WorldPosZ = 0x68;

        static const unsigned int Offset_SleepingMode = 0x10C;
        static const unsigned int MASK_SkeepingMode = 3;

        static const unsigned int EndMovement = 0x0061E0D0;
        static const unsigned int SetSleeping = 0x00602930;
    };

    struct zCVisual {
        static const unsigned int VTBL_GetFileExtension = 17;
        static const unsigned int Destructor = 0x00606800;
    };



    struct zCDecal {
        static const unsigned int Offset_DecalSettings = 0x34;
        static const unsigned int GetAlphaTestEnabled = 0x00556BE0;
    };

    struct oCSpawnManager {
        static const unsigned int SpawnNpc = 0x00778E70;
        static const unsigned int CheckInsertNpc = 0x007780B0;
        static const unsigned int CheckRemoveNpc = 0x007792E0;
    };


    struct zCBspTree {
        static const unsigned int AddVob = 0x00531040;
        static const unsigned int LoadBIN = 0x00538E10;
        static const unsigned int Offset_NumPolys = 0x24;
        static const unsigned int Offset_PolyArray = 0x10;
        static const unsigned int Offset_WorldMesh = 0xC;

        static const unsigned int Offset_RootNode = 0x8;
        static const unsigned int Offset_LeafList = 0x18;
        static const unsigned int Offset_NumLeafes = 0x20;
        static const unsigned int Offset_BspTreeMode = 0x58;

        static const unsigned int Render = 0x00530080;
        static const unsigned int SIZE_Render = 0xA48;



        static const unsigned int CALL_RenderOutdoor = 0x005307E9;
        static const unsigned int CALL_RenderOutdoor2 = 0x005308C1;
        static const unsigned int CALL_RenderIndoor = 0x0053041C;
        static const unsigned int CALL_RenderIndoor2 = 0x00530432;
        static const unsigned int CALL_RenderIndoor3 = 0x00530490;
        static const unsigned int CALL_RenderIndoor4 = 0x005304A2;
        static const unsigned int CALL_RenderTrivIndoor = 0x005304AB;
    };

    struct zCPolygon {
        static const unsigned int Offset_VerticesArray = 0x00;
        static const unsigned int Offset_FeaturesArray = 0x2C;
        static const unsigned int Offset_NumPolyVertices = 0x30;
        static const unsigned int Offset_PolyFlags = 0x31;
        static const unsigned int Offset_Material = 0x18;
        static const unsigned int Offset_Lightmap = 0x1C;
    };

    struct zSTRING {
        static const unsigned int ToChar = 0x004639D0;
        static const unsigned int ConstructorCharPtr = 0x004010C0;
    };



    struct zCMaterial {
        static const unsigned int Offset_Color = 0x38;
        static const unsigned int Offset_Texture = 0x34;
        static const unsigned int Offset_AlphaFunc = 0x74;
        static const unsigned int Offset_MatGroup = 0x40;
        static const unsigned int Offset_TexAniCtrl = 0x4C;

        static const unsigned int InitValues = 0x00564260;
        static const unsigned int Constructor = 0x00563E00;
        static const unsigned int Destructor = 0x00564070;
        static const unsigned int GetAniTexture = 0x0064BA20;

    };

    struct zCTexture {
        static const unsigned int zCTex_D3DInsertTexture = 0x00656120;
        static const unsigned int LoadResourceData = 0x005F54D0;
        static const unsigned int GetName = 0x005A9CD0;
        static const unsigned int Offset_CacheState = 0x4C;
        static const unsigned int Mask_CacheState = 3;

        static const unsigned int Offset_Flags = 0x88;
        static const unsigned int Mask_FlagHasAlpha = 0x1;
        static const unsigned int Mask_FlagIsAnimated = 0x2;

        static const unsigned int zCResourceTouchTimeStamp = 0x005DC810;
        static const unsigned int zCResourceTouchTimeStampLocal = 0x005DC890;

        static const unsigned int Load = 0x005F36A0;

        static const unsigned int Offset_Surface = 0xD4;
        static const unsigned int XTEX_BuildSurfaces = 0x006552A0;
        //static const unsigned int Load = 0x005F4360;

        static const unsigned int PrecacheTexAniFrames = 0x005F3660;
    };

    struct zCResourceManager {
        static const unsigned int CacheIn = 0x005DD040;
        static const unsigned int CacheOut = 0x005DD350;
        static const unsigned int PurgeCaches = 0x005DCA30;
    };

    struct oCWorld {
        //static const unsigned int InsertVobInWorld = 0x006D7120;
        static const unsigned int EnableVob = 0x00780340;
        static const unsigned int DisableVob = 0x00780460;
        static const unsigned int RemoveFromLists = 0x00780990;
        static const unsigned int RemoveVob = 0x007800C0;
    };

    struct zCWorld {
        static const unsigned int Render = 0x00621700;
        static const unsigned int InsertVobInWorld = 0x00780330;
        static const unsigned int VobAddedToWorld = 0x00624830;
        static const unsigned int Call_Render_zCBspTreeRender = 0x00621830;
        static const unsigned int Offset_GlobalVobTree = 0x24;
        static const unsigned int LoadWorld = 0x006270D0;
        static const unsigned int VobRemovedFromWorld = 0x00624970;
        static const unsigned int Offset_SkyControllerOutdoor = 0x0E4;
        static const unsigned int GetActiveSkyController = 0x006203A0;
        static const unsigned int DisposeWorld = 0x00623D30;
        static const unsigned int DisposeVobs = 0x00623960;
        static const unsigned int Offset_BspTree = 0x1AC;
        static const unsigned int RemoveVob = 0x00624B70;
    };

    struct zCClassDef {
        static const unsigned int oCTouchDamage = 0x00ab3590;
        static const unsigned int oCMobSwitch = 0x00ab17c0;
        static const unsigned int zCVobStartpoint = 0x00ab6568;
        static const unsigned int zCVobLight = 0x009a3810;
        static const unsigned int oCTriggerChangeLevel = 0x008c2f40;
        static const unsigned int zCObject = 0x008d8cd8;
        static const unsigned int zCVob = 0x009a34d8;
        static const unsigned int zCZone = 0x009a45a8;
        static const unsigned int zCVobAnimate = 0x009a3e50;
        static const unsigned int zCEffect = 0x009a3de0;
        static const unsigned int oCItem = 0x00ab1168;
        static const unsigned int zCVobSound = 0x009a44c8;
        static const unsigned int zCTouchDamage = 0x009a3d00;
        static const unsigned int zCMover = 0x009a3a60;
        static const unsigned int zCTrigger = 0x009a3d70;
        static const unsigned int zCTriggerBase = 0x009a3c90;
        static const unsigned int oCMobDoor = 0x00ab1518;
        static const unsigned int zCCamTrj_KeyFrame = 0x008d1028;
        static const unsigned int oCMobLockable = 0x00ab1598;
        static const unsigned int zCVobSoundDaytime = 0x009a4688;
        static const unsigned int oCMobInter = 0x00ab19a0;
        static const unsigned int oCMOB = 0x00ab1a10;
        static const unsigned int oCVob = 0x00ab3510;
        static const unsigned int zCMessageFilter = 0x009a3ec0;
        static const unsigned int oCMobContainer = 0x00ab18b0;
        static const unsigned int zCVobSpot = 0x00ab6648;
        static const unsigned int oCNpc = 0x00ab1e20;
        static const unsigned int oCTriggerScript = 0x008c2e48;
        static const unsigned int zCPFXControler = 0x009a3b40;
        static const unsigned int zCTriggerList = 0x009a4160;
        static const unsigned int oCMobFire = 0x00ab1698;
        static const unsigned int zCEarthquake = 0x009a3ad0;
        static const unsigned int zCCSCamera = 0x008d0fb8;
        static const unsigned int zCCodeMaster = 0x009a40f0;
        static const unsigned int oCVisualFX = 0x008ce658;
        static const unsigned int oCZoneMusic = 0x009a49b0;
        static const unsigned int oCMobBed = 0x00ab14a0;
        static const unsigned int zCZoneZFog = 0x009a4538;
        static const unsigned int oCMobWheel = 0x00ab1430;
        static const unsigned int zCZoneVobFarPlane = 0x009a48b8;
        static const unsigned int zCTriggerUntouch = 0x009a4010;
        static const unsigned int zCTriggerWorldStart = 0x009a4240;
        static const unsigned int zCZoneVobFarPlaneDefault = 0x009a4768;
        static const unsigned int zCZoneZFogDefault = 0x009a4618;
        static const unsigned int oCZoneMusicDefault = 0x009a4938;
    };

    class VobTypes // vftables
    {
    public:
        static const unsigned int
            Npc = 0x83D724,
            Mob = 0x83D4D4,
            Item = 0x83C804,
            Mover = 0x83A47C,
            MobFire = 0x83D19C,
            VobLight = 0x839A74;
    protected:
        VobTypes() {}
    };
};
