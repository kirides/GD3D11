#pragma once

/** Holds all memorylocations for the selected game */
struct GothicMemoryLocations {
    struct Functions {
        static const unsigned int Alg_Rotation3DNRad = 0x006A3A40;
        static const unsigned int vidGetFPSRate = 0x0068AD40;
        //static const unsigned int HandledWinMain = 0x00502ED0XXX;
        //static const unsigned int ExitGameFunc = 0x00425F30;
        static const unsigned int zCExceptionHandler_UnhandledExceptionFilter = 0x00655A10;
    };

    struct zCQuadMark {
        static const unsigned int Constructor = 0x0075B830;
        static const unsigned int Destructor = 0x0075BA50;
        static const unsigned int CreateQuadMark = 0x0075CEF0;
        static const unsigned int Offset_QuadMesh = 0x34;
        static const unsigned int Offset_Material = 0x3C;
        static const unsigned int Offset_ConnectedVob = 0x38;
        static const unsigned int Offset_DontRepositionConnectedVob = 0x48;
    };

    struct zCPolyStrip {
        static const unsigned int Offset_PolyList = 0x3C;
        static const unsigned int Offset_NumPolys = 0x40;
    };

    struct zCThread {
        static const unsigned int SuspendThread = 0x00784070;
        static const unsigned int Offset_SuspendCount = 0x0C;
    };

    struct zCActiveSnd {
        static const unsigned int AutoCalcObstruction = 0x006868A0;
    };

    struct zCBinkPlayer {
        static const unsigned int GetPixelFormat = 0x005FB240;
        static const unsigned int OpenVideo = 0x005F8C50;
        static const unsigned int Offset_VideoHandle = 0x30;
    };

    struct CGameManager {
        static const unsigned int ExitGame = 0x004899E0;
    };

    struct zCOption {
        static const unsigned int ReadInt = 0x0061C4F0;
        static const unsigned int ReadBool = 0x0061C2C0;
        static const unsigned int ReadDWORD = 0x0061C650;
        static const unsigned int Offset_CommandLine = 0x284;
    };

    struct zCRndD3D {
        static const unsigned int VidSetScreenMode = 0x007E1260;
        static const unsigned int DrawLineZ = 0x007D61C0;
        static const unsigned int Vid_GetGammaCorrection = 0x007E1CD0;

        static const unsigned int Offset_RenderState = 0x38;
        static const unsigned int Offset_BoundTexture = 0x82E50;
    };

    struct zERROR {
        // Start/End for problematic SendMessage-Broadcast which causes the game to conflict with other applications
        //static const unsigned int BroadcastStart = 0x0044C5D5;
        //static const unsigned int BroadcastEnd = 0x0044C5E4;
    };

    struct oCGame {
        static const unsigned int EnterWorld = 0x00533E10;
        static const unsigned int TestKeys = 0x00557180;
        static const unsigned int Var_Player = 0x009A81DC;
        static const unsigned int Offset_GameView = 0x30;
    };

    struct oCNPC {
        static const unsigned int ResetPos = 0x004F4F20;
        static const unsigned int InitModel = 0x00592330;
        static const unsigned int Enable = 0x0059FBF0;
        static const unsigned int Disable = 0x0059F8D0;
        static const unsigned int IsAPlayer = 0x0059C450;
    };

    struct zCView {
        static const unsigned int SetMode = 0x008186D0;
        static const unsigned int REPL_SetMode_ModechangeStart = SetMode + 0x29;
        static const unsigned int REPL_SetMode_ModechangeEnd = SetMode + 0x38;
        static const unsigned int PrintTimed = 0x00814640;
    };

    struct zCObject {
        //static const unsigned int Release = 0x0040C310;
        static const unsigned int GetObjectName = 0x00734DC0;
        static const unsigned int VTBL_ScalarDestructor = 0x12 / 4;
    };

    struct zCSkyController_Outdoor {
        static const unsigned int OBJ_ActivezCSkyController = 0x00B186AC;

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
        //static const unsigned int Interpolate = 0x005E8C20;
        static const unsigned int Offset_InitDone = 0x7C;
        //static const unsigned int Init = 0x005E6A00;
        static const unsigned int GetUnderwaterFX = 0x0076AE40;

        static const unsigned int SetCameraLocationHint = 0x00526860;

        static const unsigned int Offset_WeatherType = 0x30;
        static const unsigned int LOC_ProcessRainFXNOPStart = 0x00775E3F;
        static const unsigned int LOC_ProcessRainFXNOPEnd = 0x00775F06;

        static const unsigned int ProcessRainFX = 0x00775C20;
        static const unsigned int Offset_OutdoorRainFXWeight = 0x69C;
    };

    struct zCSkyController {
        static const unsigned int VTBL_RenderSkyPre = 27; //0x6C / 4
        static const unsigned int VTBL_RenderSkyPost = (0x78 / 4);
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
        static const unsigned int UpdateParticle = 0x0073A3F0;
        static const unsigned int OBJ_s_pfxList = 0x009BFA34;
        static const unsigned int OBJ_s_partMeshQuad = 0x009BFA50;
        static const unsigned int CreateParticlesUpdateDependencies = 0x0073A130;
        static const unsigned int SetVisualUsedBy = 0x00738B70;
        static const unsigned int GetNumParticlesThisFrame = 0x0073C980;
        static const unsigned int UpdateParticleFX = 0x0073A050;
        static const unsigned int CheckDependentEmitter = 0x0073CB20;

        static const unsigned int Offset_PrevPFX = 0x80; // PrivateTotalTime - 4
        static const unsigned int Offset_NextPFX = 0x7C; // PrivateTotalTime - 8
        static const unsigned int Destructor = 0x00737FD0;
    };

    struct zCStaticPfxList {
        static const unsigned int TouchPFX = 0x00738220;
    };

    struct zCInput {
        static const unsigned int GetDeviceEnabled = 0x00662460;
        static const unsigned int SetDeviceEnabled = 0x00662400;
    };

    struct GlobalObjects {
        static const unsigned int zCResourceManager = 0x00B18550;
        static const unsigned int zCCamera = 0x009BDDF4;
        static const unsigned int oCGame = 0x009A63DC;
        static const unsigned int zCTimer = 0x00B18DF8;
        static const unsigned int s_globFreePart = 0x009BFA48;
        //static const unsigned int DInput7DeviceMouse = 0x008D1D70;
        //static const unsigned int DInput7DeviceKeyboard = 0x008D1D64;
        static const unsigned int zCInput = 0x009B74A5;
        static const unsigned int zCOption = 0x009B4350;
        //static const unsigned int zCParser = 0xAB40C0;
        static const unsigned int zRenderer = 0x00AF9728;
        static const unsigned int zSound = 0x00B18A5C;
    };

    struct zCSoundSystem {
        static const unsigned int VTBL_SetGlobalReverbPreset = 0x58 / 4;
    };

    struct zCParser {
        //static const unsigned int CallFunc = 0x7929F0;
    };

    struct zCMorphMesh {
        static const unsigned int Offset_MorphMesh = 0x38;
        static const unsigned int Offset_TexAniState = 0x40;
        static const unsigned int AdvanceAnis = 0x00731920;
        static const unsigned int CalcVertexPositions = 0x00731570;
    };

    struct zCModelAni {
        static const unsigned int Offset_NumFrames = 0xD8;
        static const unsigned int Offset_Flags = 0xE0;
        static const unsigned int Mask_FlagIdle = 0x10;
    };

    struct zCMeshSoftSkin {
        //static const unsigned int Offset_NodeIndexList = 0x38C;
        static const unsigned int Offset_VertWeightStream = 0x0FC;
    };

    struct zCModelPrototype {
        static const unsigned int Offset_NodeList = 0x64;
        static const unsigned int Offset_MeshSoftSkinList = 0x64 + 16;
        static const unsigned int LoadModelASC = 0x007298B0;
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
        static const unsigned int Mask_LightEnabled = 0x20;
        static const unsigned int DoAnimation = 0x007932D0;
    };

    struct zFILE {
        static const unsigned int Open = 0x005FDD00;
    };

    struct zCBspNode {
        static const unsigned int RenderIndoor = 0x006BA900;
        static const unsigned int RenderOutdoor = 0x006BACC0;

        //static const unsigned int REPL_RenderIndoorEnd = 0x0052F153;

        //static const unsigned int REPL_RenderOutdoorStart = 0x0052F495;
        //static const unsigned int REPL_RenderOutdoorEnd = 0x0052F69E;
        //static const unsigned int SIZE_RenderOutdoor = 0x211;

    };

    struct zCModelTexAniState {
        static const unsigned int UpdateTexList = 0x00702080;
    };

    struct zCModel {
        static const unsigned int RenderNodeList = 0x00704840;
        static const unsigned int UpdateAttachedVobs = 0x0070BBE0;
        static const unsigned int Offset_HomeVob = 0x60;
        static const unsigned int Offset_ModelProtoList = 0x64;
        static const unsigned int Offset_NodeList = 0x70;
        static const unsigned int Offset_MeshSoftSkinList = 0x7C;
        static const unsigned int Offset_MeshLibList = 0xBC;
        static const unsigned int Offset_AttachedVobList = 0x98;
        static const unsigned int AdvanceAnis = 0x00707D70;
        static const unsigned int SIZE_AdvanceAnis = 0x119E;
        static const unsigned int RPL_AniQuality = 0x0057CCC1;
        static const unsigned int Offset_DrawHandVisualsOnly = 0x174;
        static const unsigned int Offset_ModelFatness = 0x128;
        static const unsigned int Offset_ModelScale = 0x12C;
        static const unsigned int Offset_Flags = 0x1F8;
        static const unsigned int Offset_DistanceModelToCamera = 0x120;
        static const unsigned int Offset_NumActiveAnis = 0x34;
        static const unsigned int GetVisualName = 0x00709240;
        static const unsigned int Offset_AniChannels = 0x38;
    };

    struct zCCamera {
        static const unsigned int GetTransform = 0x006D5D90;
        static const unsigned int SetTransform = 0x006D5C30;
        static const unsigned int UpdateViewport = 0x006D6180;
        static const unsigned int Activate = 0x006D5DF0;
        static const unsigned int Offset_NearPlane = 0x900;
        static const unsigned int Offset_FarPlane = 0x8FC;
        static const unsigned int SetFarPlane = 0x006D68F0;
        static const unsigned int BBox3DInFrustum = 0x006D6B00;
        static const unsigned int Var_FreeLook = 0x009A1C0C;
        static const unsigned int SetFOV = 0x006D6050;
        static const unsigned int GetFOV_f2 = 0x006D5FE0;
    };

    struct zCProgMeshProto {
        static const unsigned int Offset_PositionList = 0x34;
        static const unsigned int Offset_NormalsList = 0x3C;
        static const unsigned int Offset_Submeshes = 0xA4;
        static const unsigned int Offset_NumSubmeshes = 0xA8;
    };

    struct zCVob {
        static const unsigned int SetCollDetStat = 0x007A7CE0;
        //static const unsigned int SetCollDetDyn = 0x;

        static const unsigned int s_ShowHelperVisuals = 0x00B21214;
        static const unsigned int GetClassHelperVisual = 0x0078C100;

        static const unsigned int Offset_WorldMatrixPtr = 0x3C;
        //static const unsigned int Offset_BoundingBoxWS = 0x40;
        static const unsigned int GetVisual = 0x007A1AE0;
        static const unsigned int SetVisual = 0x0078D660;
        static const unsigned int GetPositionWorld = 0x004E72A0;
        static const unsigned int SetPositionWorld = 0x007A6A00;
        static const unsigned int GetBBoxLocal = 0x007A6080;
        static const unsigned int Offset_HomeWorld = 0x0B8;
        static const unsigned int Offset_GroundPoly = 0x0BC;

        static const unsigned int Offset_Flags = 0x104;
		static const unsigned int Offset_VobAlpha = 0xCC;
        static const unsigned int MASK_ShowVisual = 0x1;
		static const unsigned int MASK_VisualAlpha = 0x4;
        static const unsigned int Offset_CameraAlignment = 0x110;
        static const unsigned int SHIFTLR_CameraAlignment = 0x1E;

        static const unsigned int Destructor = 0x00789070;

        static const unsigned int Offset_WorldPosX = 0x48;
        static const unsigned int Offset_WorldPosY = 0x58;
        static const unsigned int Offset_WorldPosZ = 0x68;

        static const unsigned int Offset_SleepingMode = 0x10C;
        static const unsigned int MASK_SkeepingMode = 3;

        static const unsigned int EndMovement = 0x007A8F60;

    };

    struct zCVisual {
        static const unsigned int VTBL_GetFileExtension = 17;
        static const unsigned int Destructor = 0x00791970;
    };

    struct oCVisualFX {
        static const unsigned int Offset_emAdjustShpToOrigin = 0x2D0;
        static const unsigned int AdjustShapeToOrigin = 0x004c7fa0; //vVvVv starts as
        // if (((*(int *)(param_1 + 0x2d0) != 0) && (*(int *)(param_1 + 0x4a8) != 0))
        // which is:
        // if (this->emAdjustShpToOrigin && this->origin)
    };

    struct zCDecal {
        static const unsigned int Offset_DecalSettings = 0x34;
        static const unsigned int GetAlphaTestEnabled = 0x006E21E0;
    };

    struct oCSpawnManager {
        static const unsigned int SpawnNpc = 0x005D2410;
    };


    struct zCBspTree {
        static const unsigned int AddVob = 0x006BC840;
        static const unsigned int LoadBIN = 0x006C4640;
        static const unsigned int Offset_NumPolys = 0x24;
        static const unsigned int Offset_PolyArray = 0x10;
        static const unsigned int Offset_WorldMesh = 0xC;

        static const unsigned int Offset_RootNode = 0x8;
        static const unsigned int Offset_LeafList = 0x18;
        static const unsigned int Offset_NumLeafes = 0x20;
        static const unsigned int Offset_BspTreeMode = 0x58;

        static const unsigned int Render = 0x006BB880;
        //static const unsigned int SIZE_Render = 0xA48;



        /*static const unsigned int CALL_RenderOutdoor = 0x005307E9;
        static const unsigned int CALL_RenderOutdoor2 = 0x005308C1;
        static const unsigned int CALL_RenderIndoor = 0x0053041C;
        static const unsigned int CALL_RenderIndoor2 = 0x00530432;
        static const unsigned int CALL_RenderIndoor3 = 0x00530490;
        static const unsigned int CALL_RenderIndoor4 = 0x005304A2;
        static const unsigned int CALL_RenderTrivIndoor = 0x005304AB;*/
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
        static const unsigned int ToChar = 0x08;
        static const unsigned int ConstructorCharPtr = 0x004010C0;
        static const unsigned int DestructorCharPtr = 0x00401160;
    };



    struct zCMaterial {

        static const unsigned int Offset_Color = 0x38;
        static const unsigned int Offset_Texture = 0x34;
        static const unsigned int Offset_AlphaFunc = 0x74;
        static const unsigned int Offset_MatGroup = 0x40;
        static const unsigned int Offset_TexAniCtrl = 0x4C;

        static const unsigned int InitValues = 0x006EF890;
        static const unsigned int Constructor = 0x006EF430;
        static const unsigned int Destructor = 0x006EF6A0;
        static const unsigned int GetAniTexture = 0x007D4110;

    };

    struct zCTexture {
        static const unsigned int zCTex_D3DInsertTexture = 0x007DE7E0;
        static const unsigned int LoadResourceData = 0x007801D0;
        static const unsigned int GetName = 0x00734DC0;
        static const unsigned int Offset_CacheState = 0x4C;
        static const unsigned int Mask_CacheState = 3;

        static const unsigned int Offset_Flags = 0x88;
        static const unsigned int Mask_FlagHasAlpha = 0x1;
        static const unsigned int Mask_FlagIsAnimated = 0x2;

        static const unsigned int zCResourceTouchTimeStamp = 0x007676C0;
        static const unsigned int zCResourceTouchTimeStampLocal = 0x00767740;

        static const unsigned int Load = 0x0077E390;

        static const unsigned int Offset_Surface = 0xD4;
        static const unsigned int XTEX_BuildSurfaces = 0x007DD960;
        //static const unsigned int Load = 0x005F4360;

        static const unsigned int PrecacheTexAniFrames = 0x0077E350;
    };

    struct zCResourceManager {
        static const unsigned int CacheIn = 0x00767EF0;
        static const unsigned int CacheOut = 0x00768200;
        static const unsigned int PurgeCaches = 0x007678E0;
        static const unsigned int RefreshTexMaxSize = 0x0077FBA0;
        static const unsigned int SetThreadingEnabled = 0x00767AE0;
    };

    struct oCWorld {
        static const unsigned int EnableVob = 0x005D9920;
        static const unsigned int DisableVob = 0x005D9A40;
        static const unsigned int RemoveFromLists = 0x005D9F70;
    };

    struct zCWorld {
        static const unsigned int Render = 0x007AC5F0;
        static const unsigned int VobAddedToWorld = 0x007AF6C0;
        //static const unsigned int Call_Render_zCBspTreeRender = 0x00621830;
        static const unsigned int Offset_GlobalVobTree = 0x24;
        static const unsigned int LoadWorld = 0x005D9100;
        static const unsigned int VobRemovedFromWorld = 0x007AF800;
        static const unsigned int Offset_SkyControllerOutdoor = 0x0E4;
        static const unsigned int GetActiveSkyController = 0x007AB240;
        static const unsigned int DisposeWorld = 0x007AEC20;
        static const unsigned int DisposeVobs = 0x007AE850;
        static const unsigned int Offset_BspTree = 0x1AC;
    };

    struct zCClassDef {
        static const unsigned int oCNpc = 0x009A7978;
        static const unsigned int zCTexture = 0x00B18D18;
        static const unsigned int oCVisualFX = 0x0096f654;
    };
};
