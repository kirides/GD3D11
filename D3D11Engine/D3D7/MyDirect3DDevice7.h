#pragma once
#include "d3d.h"
#include "MyDirect3DVertexBuffer7.h"
#include <stdio.h>
#include "../Engine.h"
#include "../Logger.h"
#include "MyDirectDrawSurface7.h"
#include "../GothicAPI.h"
#include "../HookExceptionFilter.h"

#define GOTHIC_FVF_XYZ_DIF_T1 (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define GOTHIC_FVF_XYZ_DIF_T1_SIZE ((3 + 1 + 2) * 4)

#define GOTHIC_FVF_XYZ_NRM_T1 (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)
#define GOTHIC_FVF_XYZ_NRM_T1_SIZE ((3 + 3 + 2) * 4)

#define GOTHIC_FVF_XYZ_NRM_DIF_T2 (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2)
#define GOTHIC_FVF_XYZ_NRM_DIF_T2_SIZE ((3 + 3 + 1 + 4) * 4)

#define GOTHIC_FVF_XYZ_DIF_T2 (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2)
#define GOTHIC_FVF_XYZ_DIF_T2_SIZE ((3 + 1 + 4) * 4)

#define GOTHIC_FVF_XYZRHW_DIF_T1 (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define GOTHIC_FVF_XYZRHW_DIF_T1_SIZE ((4 + 1 + 2) * 4)

#define GOTHIC_FVF_XYZRHW_DIF_SPEC_T1 (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1)
#define GOTHIC_FVF_XYZRHW_DIF_SPEC_T1_SIZE ((4 + 1 + 1 + 2) * 4)

const int DRAW_PRIM_INDEX_BUFFER_SIZE = 4096 * sizeof( VERTEX_INDEX );

class MyDirect3DDevice7 : public IDirect3DDevice7 {
public:
	MyDirect3DDevice7( IDirect3D7* direct3D7, IDirect3DDevice7* direct3DDevice7 ) {
		DebugWrite( "MyDirect3DDevice7::MyDirect3DDevice7" );

		RefCount = 1;

		ZeroMemory(&FakeDeviceDesc, sizeof(D3DDEVICEDESC7));
		FakeDeviceDesc.dwDevCaps = (D3DDEVCAPS_FLOATTLVERTEX|D3DDEVCAPS_EXECUTESYSTEMMEMORY|D3DDEVCAPS_TLVERTEXSYSTEMMEMORY|D3DDEVCAPS_TEXTUREVIDEOMEMORY|D3DDEVCAPS_DRAWPRIMTLVERTEX
			|D3DDEVCAPS_CANRENDERAFTERFLIP|D3DDEVCAPS_DRAWPRIMITIVES2|D3DDEVCAPS_DRAWPRIMITIVES2EX|D3DDEVCAPS_HWTRANSFORMANDLIGHT|D3DDEVCAPS_HWRASTERIZATION);
		FakeDeviceDesc.dpcLineCaps.dwSize = sizeof(D3DPRIMCAPS);
		FakeDeviceDesc.dpcLineCaps.dwMiscCaps = D3DPMISCCAPS_MASKZ;
		FakeDeviceDesc.dpcLineCaps.dwRasterCaps = (D3DPRASTERCAPS_DITHER|D3DPRASTERCAPS_ZTEST|D3DPRASTERCAPS_SUBPIXEL|D3DPRASTERCAPS_FOGVERTEX|D3DPRASTERCAPS_FOGTABLE
			|D3DPRASTERCAPS_MIPMAPLODBIAS|D3DPRASTERCAPS_ZBIAS|D3DPRASTERCAPS_ANISOTROPY|D3DPRASTERCAPS_WFOG|D3DPRASTERCAPS_ZFOG);
		FakeDeviceDesc.dpcLineCaps.dwZCmpCaps = (D3DPCMPCAPS_NEVER|D3DPCMPCAPS_LESS|D3DPCMPCAPS_EQUAL|D3DPCMPCAPS_LESSEQUAL|D3DPCMPCAPS_GREATER|D3DPCMPCAPS_NOTEQUAL
			|D3DPCMPCAPS_GREATEREQUAL|D3DPCMPCAPS_ALWAYS);
		FakeDeviceDesc.dpcLineCaps.dwSrcBlendCaps = (D3DPBLENDCAPS_ZERO|D3DPBLENDCAPS_ONE|D3DPBLENDCAPS_SRCCOLOR|D3DPBLENDCAPS_INVSRCCOLOR|D3DPBLENDCAPS_SRCALPHA
			|D3DPBLENDCAPS_INVSRCALPHA|D3DPBLENDCAPS_DESTALPHA|D3DPBLENDCAPS_INVDESTALPHA|D3DPBLENDCAPS_DESTCOLOR|D3DPBLENDCAPS_INVDESTCOLOR|D3DPBLENDCAPS_SRCALPHASAT
			|D3DPBLENDCAPS_BOTHSRCALPHA|D3DPBLENDCAPS_BOTHINVSRCALPHA);
		FakeDeviceDesc.dpcLineCaps.dwDestBlendCaps = (D3DPBLENDCAPS_ZERO|D3DPBLENDCAPS_ONE|D3DPBLENDCAPS_SRCCOLOR|D3DPBLENDCAPS_INVSRCCOLOR|D3DPBLENDCAPS_SRCALPHA
			|D3DPBLENDCAPS_INVSRCALPHA|D3DPBLENDCAPS_DESTALPHA|D3DPBLENDCAPS_INVDESTALPHA|D3DPBLENDCAPS_DESTCOLOR|D3DPBLENDCAPS_INVDESTCOLOR|D3DPBLENDCAPS_SRCALPHASAT);
		FakeDeviceDesc.dpcLineCaps.dwAlphaCmpCaps = (D3DPCMPCAPS_NEVER|D3DPCMPCAPS_LESS|D3DPCMPCAPS_EQUAL|D3DPCMPCAPS_LESSEQUAL|D3DPCMPCAPS_GREATER|D3DPCMPCAPS_NOTEQUAL
			|D3DPCMPCAPS_GREATEREQUAL|D3DPCMPCAPS_ALWAYS);
		FakeDeviceDesc.dpcLineCaps.dwShadeCaps = (D3DPSHADECAPS_COLORFLATRGB|D3DPSHADECAPS_COLORGOURAUDRGB|D3DPSHADECAPS_SPECULARFLATRGB|D3DPSHADECAPS_SPECULARGOURAUDRGB
			|D3DPSHADECAPS_ALPHAFLATBLEND|D3DPSHADECAPS_ALPHAGOURAUDBLEND|D3DPSHADECAPS_FOGFLAT|D3DPSHADECAPS_FOGGOURAUD);
		FakeDeviceDesc.dpcLineCaps.dwTextureCaps = (D3DPTEXTURECAPS_PERSPECTIVE|D3DPTEXTURECAPS_ALPHA|D3DPTEXTURECAPS_TRANSPARENCY|D3DPTEXTURECAPS_BORDER
			|D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE|D3DPTEXTURECAPS_CUBEMAP|D3DPTEXTURECAPS_COLORKEYBLEND);
		FakeDeviceDesc.dpcLineCaps.dwTextureFilterCaps = (D3DPTFILTERCAPS_NEAREST|D3DPTFILTERCAPS_LINEAR|D3DPTFILTERCAPS_MIPNEAREST|D3DPTFILTERCAPS_MIPLINEAR
			|D3DPTFILTERCAPS_LINEARMIPNEAREST|D3DPTFILTERCAPS_LINEARMIPLINEAR|D3DPTFILTERCAPS_MINFPOINT|D3DPTFILTERCAPS_MINFLINEAR|D3DPTFILTERCAPS_MINFANISOTROPIC
			|D3DPTFILTERCAPS_MIPFPOINT|D3DPTFILTERCAPS_MIPFLINEAR|D3DPTFILTERCAPS_MAGFPOINT|D3DPTFILTERCAPS_MAGFLINEAR|D3DPTFILTERCAPS_MAGFANISOTROPIC);
		FakeDeviceDesc.dpcLineCaps.dwTextureBlendCaps = (D3DPTBLENDCAPS_DECAL|D3DPTBLENDCAPS_MODULATE|D3DPTBLENDCAPS_DECALALPHA|D3DPTBLENDCAPS_MODULATEALPHA|D3DPTBLENDCAPS_DECALMASK
			|D3DPTBLENDCAPS_MODULATEMASK|D3DPTBLENDCAPS_COPY|D3DPTBLENDCAPS_ADD);
		FakeDeviceDesc.dpcLineCaps.dwTextureAddressCaps = (D3DPTADDRESSCAPS_WRAP|D3DPTADDRESSCAPS_MIRROR|D3DPTADDRESSCAPS_CLAMP|D3DPTADDRESSCAPS_BORDER|D3DPTADDRESSCAPS_INDEPENDENTUV);
		FakeDeviceDesc.dpcTriCaps.dwSize = sizeof(D3DPRIMCAPS);
		FakeDeviceDesc.dpcTriCaps.dwMiscCaps = (D3DPMISCCAPS_MASKZ|D3DPMISCCAPS_CULLNONE|D3DPMISCCAPS_CULLCW|D3DPMISCCAPS_CULLCCW);
		FakeDeviceDesc.dpcTriCaps.dwRasterCaps = (D3DPRASTERCAPS_DITHER|D3DPRASTERCAPS_ZTEST|D3DPRASTERCAPS_SUBPIXEL|D3DPRASTERCAPS_FOGVERTEX|D3DPRASTERCAPS_FOGTABLE
			|D3DPRASTERCAPS_MIPMAPLODBIAS|D3DPRASTERCAPS_ZBIAS|D3DPRASTERCAPS_ANISOTROPY|D3DPRASTERCAPS_WFOG|D3DPRASTERCAPS_ZFOG);
		FakeDeviceDesc.dpcTriCaps.dwZCmpCaps = (D3DPCMPCAPS_NEVER|D3DPCMPCAPS_LESS|D3DPCMPCAPS_EQUAL|D3DPCMPCAPS_LESSEQUAL|D3DPCMPCAPS_GREATER|D3DPCMPCAPS_NOTEQUAL
			|D3DPCMPCAPS_GREATEREQUAL|D3DPCMPCAPS_ALWAYS);
		FakeDeviceDesc.dpcTriCaps.dwSrcBlendCaps = (D3DPBLENDCAPS_ZERO|D3DPBLENDCAPS_ONE|D3DPBLENDCAPS_SRCCOLOR|D3DPBLENDCAPS_INVSRCCOLOR|D3DPBLENDCAPS_SRCALPHA
			|D3DPBLENDCAPS_INVSRCALPHA|D3DPBLENDCAPS_DESTALPHA|D3DPBLENDCAPS_INVDESTALPHA|D3DPBLENDCAPS_DESTCOLOR|D3DPBLENDCAPS_INVDESTCOLOR|D3DPBLENDCAPS_SRCALPHASAT
			|D3DPBLENDCAPS_BOTHSRCALPHA|D3DPBLENDCAPS_BOTHINVSRCALPHA);
		FakeDeviceDesc.dpcTriCaps.dwDestBlendCaps = (D3DPBLENDCAPS_ZERO|D3DPBLENDCAPS_ONE|D3DPBLENDCAPS_SRCCOLOR|D3DPBLENDCAPS_INVSRCCOLOR|D3DPBLENDCAPS_SRCALPHA
			|D3DPBLENDCAPS_INVSRCALPHA|D3DPBLENDCAPS_DESTALPHA|D3DPBLENDCAPS_INVDESTALPHA|D3DPBLENDCAPS_DESTCOLOR|D3DPBLENDCAPS_INVDESTCOLOR|D3DPBLENDCAPS_SRCALPHASAT);
		FakeDeviceDesc.dpcTriCaps.dwAlphaCmpCaps = (D3DPCMPCAPS_NEVER|D3DPCMPCAPS_LESS|D3DPCMPCAPS_EQUAL|D3DPCMPCAPS_LESSEQUAL|D3DPCMPCAPS_GREATER|D3DPCMPCAPS_NOTEQUAL
			|D3DPCMPCAPS_GREATEREQUAL|D3DPCMPCAPS_ALWAYS);
		FakeDeviceDesc.dpcTriCaps.dwShadeCaps = (D3DPSHADECAPS_COLORFLATRGB|D3DPSHADECAPS_COLORGOURAUDRGB|D3DPSHADECAPS_SPECULARFLATRGB|D3DPSHADECAPS_SPECULARGOURAUDRGB
			|D3DPSHADECAPS_ALPHAFLATBLEND|D3DPSHADECAPS_ALPHAGOURAUDBLEND|D3DPSHADECAPS_FOGFLAT|D3DPSHADECAPS_FOGGOURAUD);
		FakeDeviceDesc.dpcTriCaps.dwTextureCaps = (D3DPTEXTURECAPS_PERSPECTIVE|D3DPTEXTURECAPS_ALPHA|D3DPTEXTURECAPS_TRANSPARENCY|D3DPTEXTURECAPS_BORDER
			|D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE|D3DPTEXTURECAPS_CUBEMAP|D3DPTEXTURECAPS_COLORKEYBLEND);
		FakeDeviceDesc.dpcTriCaps.dwTextureFilterCaps = (D3DPTFILTERCAPS_NEAREST|D3DPTFILTERCAPS_LINEAR|D3DPTFILTERCAPS_MIPNEAREST|D3DPTFILTERCAPS_MIPLINEAR
			|D3DPTFILTERCAPS_LINEARMIPNEAREST|D3DPTFILTERCAPS_LINEARMIPLINEAR|D3DPTFILTERCAPS_MINFPOINT|D3DPTFILTERCAPS_MINFLINEAR|D3DPTFILTERCAPS_MINFANISOTROPIC
			|D3DPTFILTERCAPS_MIPFPOINT|D3DPTFILTERCAPS_MIPFLINEAR|D3DPTFILTERCAPS_MAGFPOINT|D3DPTFILTERCAPS_MAGFLINEAR|D3DPTFILTERCAPS_MAGFANISOTROPIC);
		FakeDeviceDesc.dpcTriCaps.dwTextureBlendCaps = (D3DPTBLENDCAPS_DECAL|D3DPTBLENDCAPS_MODULATE|D3DPTBLENDCAPS_DECALALPHA|D3DPTBLENDCAPS_MODULATEALPHA|D3DPTBLENDCAPS_DECALMASK
			|D3DPTBLENDCAPS_MODULATEMASK|D3DPTBLENDCAPS_COPY|D3DPTBLENDCAPS_ADD);
		FakeDeviceDesc.dpcTriCaps.dwTextureAddressCaps = (D3DPTADDRESSCAPS_WRAP|D3DPTADDRESSCAPS_MIRROR|D3DPTADDRESSCAPS_CLAMP|D3DPTADDRESSCAPS_BORDER|D3DPTADDRESSCAPS_INDEPENDENTUV);
		FakeDeviceDesc.dwDeviceRenderBitDepth = 1280;
		FakeDeviceDesc.dwDeviceZBufferBitDepth = 1536;
		FakeDeviceDesc.dwMinTextureWidth = 1;
		FakeDeviceDesc.dwMinTextureHeight = 1;
		FakeDeviceDesc.dwMaxTextureWidth = 16384;
		FakeDeviceDesc.dwMaxTextureHeight = 16384;
		FakeDeviceDesc.dwMaxTextureRepeat = 32768;
		FakeDeviceDesc.dwMaxTextureAspectRatio = 32768;
		FakeDeviceDesc.dwMaxAnisotropy = 16;
		FakeDeviceDesc.dvGuardBandLeft = -16384.0f;
		FakeDeviceDesc.dvGuardBandTop = -16384.0f;
		FakeDeviceDesc.dvGuardBandRight = 16384.0f;
		FakeDeviceDesc.dvGuardBandBottom = 16384.0f;
		FakeDeviceDesc.dvExtentsAdjust = 0.0f;
		FakeDeviceDesc.dwStencilCaps = (D3DSTENCILCAPS_KEEP|D3DSTENCILCAPS_ZERO|D3DSTENCILCAPS_REPLACE|D3DSTENCILCAPS_INCRSAT|D3DSTENCILCAPS_DECRSAT|D3DSTENCILCAPS_INVERT
			|D3DSTENCILCAPS_INCR|D3DSTENCILCAPS_DECR);
		FakeDeviceDesc.dwFVFCaps = (D3DFVFCAPS_DONOTSTRIPELEMENTS|8);
		FakeDeviceDesc.dwTextureOpCaps = (D3DTEXOPCAPS_DISABLE|D3DTEXOPCAPS_SELECTARG1|D3DTEXOPCAPS_SELECTARG2|D3DTEXOPCAPS_MODULATE|D3DTEXOPCAPS_MODULATE2X|D3DTEXOPCAPS_MODULATE4X
			|D3DTEXOPCAPS_ADD|D3DTEXOPCAPS_ADDSIGNED|D3DTEXOPCAPS_ADDSIGNED2X|D3DTEXOPCAPS_SUBTRACT|D3DTEXOPCAPS_ADDSMOOTH|D3DTEXOPCAPS_BLENDDIFFUSEALPHA|D3DTEXOPCAPS_BLENDTEXTUREALPHA
			|D3DTEXOPCAPS_BLENDFACTORALPHA|D3DTEXOPCAPS_BLENDTEXTUREALPHAPM|D3DTEXOPCAPS_BLENDCURRENTALPHA|D3DTEXOPCAPS_PREMODULATE|D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR
			|D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA|D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR|D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA|D3DTEXOPCAPS_BUMPENVMAP|D3DTEXOPCAPS_BUMPENVMAPLUMINANCE
			|D3DTEXOPCAPS_DOTPRODUCT3);
		FakeDeviceDesc.wMaxTextureBlendStages = 4;
		FakeDeviceDesc.wMaxSimultaneousTextures = 4;
		FakeDeviceDesc.dwMaxActiveLights = 8;
		FakeDeviceDesc.dvMaxVertexW = 10000000000.0f;
		FakeDeviceDesc.deviceGUID = {0xF5049E78, 0x4861, 0x11D2, {0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8}};
		FakeDeviceDesc.wMaxUserClipPlanes = 6;
		FakeDeviceDesc.wMaxVertexBlendMatrices = 4;
		FakeDeviceDesc.dwVertexProcessingCaps = (D3DVTXPCAPS_TEXGEN|D3DVTXPCAPS_MATERIALSOURCE7|D3DVTXPCAPS_DIRECTIONALLIGHTS|D3DVTXPCAPS_POSITIONALLIGHTS|D3DVTXPCAPS_LOCALVIEWER);
	}

	/*** IUnknown methods ***/
	HRESULT STDMETHODCALLTYPE QueryInterface( REFIID riid, void** ppvObj ) {
		DebugWrite( "MyDirect3DDevice7::QueryInterface" );
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef() {
		DebugWrite( "MyDirect3DDevice7::AddRef" );
		return ++RefCount;
	}

	ULONG STDMETHODCALLTYPE Release() {
		DebugWrite( "MyDirect3DDevice7::Release" );
		if ( --RefCount == 0 ) {
			delete this;
			return 0;
		}

		return RefCount;
	}

	/*** IDirect3DDevice7 methods ***/
	HRESULT STDMETHODCALLTYPE GetCaps( LPD3DDEVICEDESC7 lpD3DDevDesc ) {
		DebugWrite( "MyDirect3DDevice7::GetCaps" );

		// Tell Gothic what it wants to hear
		*lpD3DDevDesc = FakeDeviceDesc;

		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetClipPlane( DWORD Index, float* pPlane ) {
		DebugWrite( "MyDirect3DDevice7::GetClipPlane" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetClipPlane( DWORD dwIndex, D3DVALUE* pPlaneEquation ) {
		DebugWrite( "MyDirect3DDevice7::SetClipPlane" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetClipStatus( LPD3DCLIPSTATUS lpD3DClipStatus ) {
		DebugWrite( "MyDirect3DDevice7::GetClipStatus" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetClipStatus( LPD3DCLIPSTATUS lpD3DClipStatus ) {
		DebugWrite( "MyDirect3DDevice7::SetClipStatus" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetDirect3D( IDirect3D7** ppD3D ) {
		DebugWrite( "MyDirect3DDevice7::GetDirect3D" );
		*ppD3D = nullptr;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetInfo( DWORD dwDevInfoID, LPVOID pDevInfoStruct, DWORD dwSize ) {
		DebugWrite( "MyDirect3DDevice7::GetInfo" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetLight( DWORD dwLightIndex, LPD3DLIGHT7 lpLight ) {
		DebugWrite( "MyDirect3DDevice7::GetLight" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetLightEnable( DWORD Index, BOOL* pEnable ) {
		DebugWrite( "MyDirect3DDevice7::GetLightEnable" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetMaterial( LPD3DMATERIAL7 lpMaterial ) {
		DebugWrite( "MyDirect3DDevice7::GetMaterial" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetMaterial( LPD3DMATERIAL7 lpMaterial ) {
		DebugWrite( "MyDirect3DDevice7::SetMaterial" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetRenderState( D3DRENDERSTATETYPE State, DWORD* pValue ) {
		DebugWrite( "MyDirect3DDevice7::GetRenderState" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetRenderState( D3DRENDERSTATETYPE State, DWORD Value ) {
		DebugWrite( "MyDirect3DDevice7::SetRenderState" );

		GothicRendererState& state = Engine::GAPI->GetRendererState();

		// Extract the needed renderstates
		switch ( State ) {
		case D3DRENDERSTATETYPE::D3DRENDERSTATE_FOGENABLE:
			state.GraphicsState.FF_FogWeight = Value != 0 ? 1.0f : 0.0f;
			break;

		case D3DRENDERSTATETYPE::D3DRENDERSTATE_FOGSTART:
			state.GraphicsState.FF_FogNear = *reinterpret_cast<float*>(&Value);
			break;

		case D3DRENDERSTATETYPE::D3DRENDERSTATE_FOGEND:
			state.GraphicsState.FF_FogFar = *reinterpret_cast<float*>(&Value);
			break;

		case D3DRENDERSTATETYPE::D3DRENDERSTATE_FOGCOLOR:
		{
			BYTE a = Value >> 24;
			BYTE r = (Value >> 16) & 0xFF;
			BYTE g = (Value >> 8) & 0xFF;
			BYTE b = Value & 0xFF;
			state.GraphicsState.FF_FogColor = float3( r / 255.0f, g / 255.0f, b / 255.0f );
		}
		break;

		case D3DRENDERSTATETYPE::D3DRENDERSTATE_AMBIENT:
		{
			BYTE a = Value >> 24;
			BYTE r = (Value >> 16) & 0xFF;
			BYTE g = (Value >> 8) & 0xFF;
			BYTE b = Value & 0xFF;
			state.GraphicsState.FF_AmbientLighting = float3( r / 255.0f, g / 255.0f, b / 255.0f );

			// Does this enable the ambientlighting?
			//data->lightEnabled = 1.0f;
		}
		break;

		case D3DRENDERSTATE_ZENABLE: state.DepthState.DepthBufferEnabled = Value != 0; state.DepthState.SetDirty(); break;
		case D3DRENDERSTATE_ALPHATESTENABLE: state.GraphicsState.SetGraphicsSwitch( GSWITCH_ALPHAREF, Value != 0 );	break;
		case D3DRENDERSTATE_SRCBLEND: state.BlendState.SrcBlend = static_cast<GothicBlendStateInfo::EBlendFunc>(Value); state.BlendState.SetDirty(); break;
		case D3DRENDERSTATE_DESTBLEND: state.BlendState.DestBlend = static_cast<GothicBlendStateInfo::EBlendFunc>(Value); state.BlendState.SetDirty(); break;
		//case D3DRENDERSTATE_CULLMODE: state.RasterizerState.CullMode = static_cast<GothicRasterizerStateInfo::ECullMode>(Value); state.RasterizerState.SetDirty(); break;
		case D3DRENDERSTATE_ZFUNC: state.DepthState.DepthBufferCompareFunc = static_cast<GothicDepthBufferStateInfo::ECompareFunc>(Value); state.DepthState.SetDirty(); break;
		case D3DRENDERSTATE_ALPHAREF: state.GraphicsState.FF_AlphaRef = static_cast<float>(Value) / 255.0f; break; // Ref for masked
		case D3DRENDERSTATE_ALPHABLENDENABLE: state.BlendState.BlendEnabled = Value != 0; state.BlendState.SetDirty(); break;
		case D3DRENDERSTATE_ZBIAS: state.RasterizerState.ZBias = Value; state.DepthState.SetDirty(); break;
		case D3DRENDERSTATE_TEXTUREFACTOR: state.GraphicsState.FF_TextureFactor = float4( Value ); break;
		case D3DRENDERSTATE_LIGHTING: state.GraphicsState.SetGraphicsSwitch( GSWITCH_LIGHING, Value != 0 ); break;
		}

		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetRenderTarget( LPDIRECTDRAWSURFACE7* lplpRenderTarget ) {
		DebugWrite( "MyDirect3DDevice7::GetRenderTarget" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetRenderTarget( LPDIRECTDRAWSURFACE7 lpNewRenderTarget, DWORD dwFlags ) {
		DebugWrite( "MyDirect3DDevice7::SetRenderTarget" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetTexture( DWORD dwStage, LPDIRECTDRAWSURFACE7* lplpTexture ) {
		DebugWrite( "MyDirect3DDevice7::GetTexture" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetTexture( DWORD dwStage, LPDIRECTDRAWSURFACE7 lplpTexture ) {
		DebugWrite( "MyDirect3DDevice7::SetTexture" );

		// Bind the texture
		MyDirectDrawSurface7* surface = static_cast<MyDirectDrawSurface7*>(lplpTexture);
		if ( surface ) {
			surface->BindToSlot( dwStage );
		}

		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetTextureStageState( DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue ) {
		DebugWrite( "MyDirect3DDevice7::GetTextureStageState" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetTextureStageState( DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value ) {
		DebugWrite( "MyDirect3DDevice7::SetTextureStageState" );

		GothicRendererState& state = Engine::GAPI->GetRendererState();
		switch ( Type ) {
		case D3DTSS_COLOROP:
			if ( Stage < 2 )
				state.GraphicsState.FF_Stages[Stage].ColorOp = static_cast<FixedFunctionStage::EColorOp>(Value);
			else
				LogWarn() << "Gothic uses more than 2 TextureStages!";
			break;

		case D3DTSS_COLORARG1:
			if ( Stage < 2 )
				state.GraphicsState.FF_Stages[Stage].ColorArg1 = static_cast<FixedFunctionStage::ETextureArg>(Value);
			break;

		case D3DTSS_COLORARG2:
			if ( Stage < 2 )
				state.GraphicsState.FF_Stages[Stage].ColorArg2 = static_cast<FixedFunctionStage::ETextureArg>(Value);
			break;

		case D3DTSS_ALPHAOP:
			if ( Stage < 2 )
				state.GraphicsState.FF_Stages[Stage].AlphaOp = static_cast<FixedFunctionStage::EColorOp>(Value);
			break;

		case D3DTSS_ALPHAARG1:
			if ( Stage < 2 )
				state.GraphicsState.FF_Stages[Stage].ColorArg1 = static_cast<FixedFunctionStage::ETextureArg>(Value);
			break;

		case D3DTSS_ALPHAARG2:
			if ( Stage < 2 )
				state.GraphicsState.FF_Stages[Stage].ColorArg2 = static_cast<FixedFunctionStage::ETextureArg>(Value);
			break;

		case D3DTSS_BUMPENVMAT00: break;
		case D3DTSS_BUMPENVMAT01: break;
		case D3DTSS_BUMPENVMAT10: break;
		case D3DTSS_BUMPENVMAT11: break;
		case D3DTSS_TEXCOORDINDEX:
			if ( Value > 7 ) // This means that some other flag was set, and the only case that happens is for reflections
			{
				state.GraphicsState.SetGraphicsSwitch( GSWITCH_REFLECTIONS, true );
			} else {
				state.GraphicsState.SetGraphicsSwitch( GSWITCH_REFLECTIONS, false );
			}
			break;

		case D3DTSS_ADDRESS: state.SamplerState.AddressU = static_cast<GothicSamplerStateInfo::ETextureAddress>(Value);
			state.SamplerState.AddressV = static_cast<GothicSamplerStateInfo::ETextureAddress>(Value);
			state.SamplerState.SetDirty();
			break;

		case D3DTSS_ADDRESSU: state.SamplerState.AddressU = static_cast<GothicSamplerStateInfo::ETextureAddress>(Value);
			state.SamplerState.SetDirty();
			break;

		case D3DTSS_ADDRESSV: state.SamplerState.AddressV = static_cast<GothicSamplerStateInfo::ETextureAddress>(Value);
			state.SamplerState.SetDirty();
			break;

		case D3DTSS_BORDERCOLOR: break;
		case D3DTSS_MAGFILTER: break;
		case D3DTSS_MINFILTER: break;
		case D3DTSS_MIPFILTER: break;
		case D3DTSS_MIPMAPLODBIAS: break;
		case D3DTSS_MAXMIPLEVEL: break;
		case D3DTSS_MAXANISOTROPY: break;
		case D3DTSS_BUMPENVLSCALE: break;
		case D3DTSS_BUMPENVLOFFSET: break;
		case D3DTSS_TEXTURETRANSFORMFLAGS:
			break;
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetTransform( D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix ) {
		DebugWrite( "MyDirect3DDevice7::GetTransform" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetTransform( D3DTRANSFORMSTATETYPE dtstTransformStateType, LPD3DMATRIX lpD3DMatrix ) {
		DebugWrite( "MyDirect3DDevice7::SetTransform" );

		GothicRendererState& state = Engine::GAPI->GetRendererState();
		switch ( dtstTransformStateType ) {
		case D3DTRANSFORMSTATE_WORLD:
			XMMATRIX matrixWorld = XMLoadFloat4x4( reinterpret_cast<XMFLOAT4X4*>(lpD3DMatrix) );
			XMStoreFloat4x4( &state.TransformState.TransformWorld, XMMatrixTranspose( matrixWorld ) );
			break;

		case D3DTRANSFORMSTATE_VIEW:
			XMMATRIX matrixView = XMLoadFloat4x4( reinterpret_cast<XMFLOAT4X4*>(lpD3DMatrix) );
			XMStoreFloat4x4( &state.TransformState.TransformView, XMMatrixTranspose( matrixView ) );
			break;

		case D3DTRANSFORMSTATE_PROJECTION:
			XMMATRIX matrixProj = XMLoadFloat4x4( reinterpret_cast<XMFLOAT4X4*>(lpD3DMatrix) );
			XMStoreFloat4x4( &state.TransformState.TransformProj, XMMatrixTranspose( matrixProj ) );
			break;
		}

		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetViewport( LPD3DVIEWPORT7 lpViewport ) {
		DebugWrite( "MyDirect3DDevice7::GetViewport" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetViewport( LPD3DVIEWPORT7 lpViewport ) {
		DebugWrite( "MyDirect3DDevice7::SetViewport" );

		float scale = std::max( 0.1f, Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale );

		ViewportInfo vp;
		vp.TopLeftX = static_cast<UINT>(lpViewport->dwX * scale);
		vp.TopLeftY = static_cast<UINT>(lpViewport->dwY * scale);
		vp.Height = static_cast<UINT>(lpViewport->dwHeight * scale);
		vp.Width = static_cast<UINT>(lpViewport->dwWidth * scale);
		vp.MinZ = lpViewport->dvMinZ;
		vp.MaxZ = lpViewport->dvMaxZ;

		Engine::GraphicsEngine->SetViewport( vp );

		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE ApplyStateBlock( DWORD dwBlockHandle ) {
		DebugWrite( "MyDirect3DDevice7::ApplyStateBlock" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE BeginScene() {
		DebugWrite( "MyDirect3DDevice7::BeginScene" );

		Engine::GraphicsEngine->OnBeginFrame();
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE BeginStateBlock() {
		DebugWrite( "MyDirect3DDevice7::BeginStateBlock" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE CaptureStateBlock( DWORD dwBlockHandle ) {
		DebugWrite( "MyDirect3DDevice7::CaptureStateBlock" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Clear( DWORD dwCount, LPD3DRECT lpRects, DWORD dwFlags, D3DCOLOR dwColor, D3DVALUE dvZ, DWORD dwStencil ) {
		DebugWrite( "MyDirect3DDevice7::Clear" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE ComputeSphereVisibility( LPD3DVECTOR lpCenters, LPD3DVALUE lpRadii, DWORD dwNumSpheres, DWORD dwFlags, LPDWORD lpdwReturnValues ) {
		DebugWrite( "MyDirect3DDevice7::ComputeSphereVisibility" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE CreateStateBlock( D3DSTATEBLOCKTYPE d3dsbType, LPDWORD lpdwBlockHandle ) {
		DebugWrite( "MyDirect3DDevice7::CreateStateBlock" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DeleteStateBlock( DWORD dwBlockHandle ) {
		DebugWrite( "MyDirect3DDevice7::DeleteStateBlock" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive( D3DPRIMITIVETYPE dptPrimitiveType, DWORD dwVertexTypeDesc, LPVOID lpvVertices, DWORD dwVertexCount, LPWORD lpwIndices, DWORD dwIndexCount, DWORD dwFlags ) {
		DebugWrite( "MyDirect3DDevice7::DrawIndexedPrimitive" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveStrided( D3DPRIMITIVETYPE dptPrimitiveType, DWORD dwVertexTypeDesc, LPD3DDRAWPRIMITIVESTRIDEDDATA lpVertexArray, DWORD dwVertexCount, LPWORD lpwIndices, DWORD dwIndexCount, DWORD dwFlags ) {
		DebugWrite( "MyDirect3DDevice7::DrawIndexedPrimitiveStrided" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveVB( D3DPRIMITIVETYPE d3dptPrimitiveType, LPDIRECT3DVERTEXBUFFER7 lpd3dVertexBuffer, DWORD dwStartVertex, DWORD dwNumVertices, LPWORD lpwIndices, DWORD dwIndexCount, DWORD dwFlags ) {
		DebugWrite( "MyDirect3DDevice7::DrawIndexedPrimitiveVB" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DrawPrimitive( D3DPRIMITIVETYPE dptPrimitiveType, DWORD dwVertexTypeDesc, LPVOID lpvVertices, DWORD dwVertexCount, DWORD dwFlags ) {
		DebugWrite( "MyDirect3DDevice7::DrawPrimitive" );

		// Convert them into ExVertices
		static std::vector<ExVertexStruct> exv;
		exv.resize( dwVertexCount );

		switch ( dwVertexTypeDesc ) {
		case GOTHIC_FVF_XYZRHW_DIF_T1:
			//return S_OK; 
			for ( unsigned int i = 0; i < dwVertexCount; i++ ) {
				Gothic_XYZRHW_DIF_T1_Vertex* rhw = reinterpret_cast<Gothic_XYZRHW_DIF_T1_Vertex*>(lpvVertices);

				exv[i].Position = rhw[i].xyz;
				exv[i].Normal.x = rhw[i].rhw;
				exv[i].TexCoord = rhw[i].texCoord;
				exv[i].Color = rhw[i].color;
			}

			// Gothic wants that for the sky
			Engine::GAPI->GetRendererState().RasterizerState.FrontCounterClockwise = true;
			Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
			Engine::GraphicsEngine->SetActiveVertexShader( "VS_TransformedEx" );
			Engine::GraphicsEngine->BindViewportInformation( "VS_TransformedEx", 0 );
			break;

		case GOTHIC_FVF_XYZRHW_DIF_SPEC_T1:
			for ( unsigned int i = 0; i < dwVertexCount; i++ ) {
				Gothic_XYZRHW_DIF_SPEC_T1_Vertex* rhw = reinterpret_cast<Gothic_XYZRHW_DIF_SPEC_T1_Vertex*>(lpvVertices);

				exv[i].Position = rhw[i].xyz;
				exv[i].Normal.x = rhw[i].rhw;
				exv[i].TexCoord = rhw[i].texCoord;
				exv[i].Color = rhw[i].color;
			}

			Engine::GraphicsEngine->SetActiveVertexShader( "VS_TransformedEx" );
			Engine::GraphicsEngine->BindViewportInformation( "VS_TransformedEx", 0 );
			break;

		default:
			return S_OK;
		}

		Engine::GraphicsEngine->SetActivePixelShader( "PS_FixedFunctionPipe" );
		if ( dptPrimitiveType == D3DPT_TRIANGLEFAN ) {
			static std::vector<ExVertexStruct> vertexList;
			vertexList.clear();
			WorldConverter::TriangleFanToList( &exv[0], dwVertexCount, &vertexList );

			Engine::GraphicsEngine->DrawVertexArray( &vertexList[0], vertexList.size() );
		} else {
			if ( dptPrimitiveType == D3DPT_TRIANGLELIST )
				Engine::GraphicsEngine->DrawVertexArray( &exv[0], dwVertexCount );
		}

		exv.clear(); // static, keep the memory allocated

		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DrawPrimitiveStrided( D3DPRIMITIVETYPE dptPrimitiveType, DWORD dwVertexTypeDesc, LPD3DDRAWPRIMITIVESTRIDEDDATA lpVertexArray, DWORD dwVertexCount, DWORD dwFlags ) {
		DebugWrite( "MyDirect3DDevice7::DrawPrimitiveStrided" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DrawPrimitiveVB( D3DPRIMITIVETYPE d3dptPrimitiveType, LPDIRECT3DVERTEXBUFFER7 lpd3dVertexBuffer, DWORD dwStartVertex, DWORD dwNumVertices, DWORD dwFlags ) {
		DebugWrite( "MyDirect3DDevice7::DrawPrimitiveVB" );
		if ( d3dptPrimitiveType < 4 )
		{
			return S_OK;
		}

		D3DVERTEXBUFFERDESC desc;
		lpd3dVertexBuffer->GetVertexBufferDesc( &desc );

		switch ( desc.dwFVF ) {
		case GOTHIC_FVF_XYZRHW_DIF_T1:
			Engine::GraphicsEngine->SetActiveVertexShader( "VS_XYZRHW_DIF_T1" );
			Engine::GraphicsEngine->SetActivePixelShader( "PS_FixedFunctionPipe" );

			Engine::GraphicsEngine->BindViewportInformation( "VS_XYZRHW_DIF_T1", 0 );

			// Gothic wants that for the sky
			Engine::GAPI->GetRendererState().RasterizerState.FrontCounterClockwise = true;
			Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
			Engine::GraphicsEngine->DrawVertexBufferFF( static_cast<MyDirect3DVertexBuffer7*>(lpd3dVertexBuffer)->GetVertexBuffer(), dwNumVertices, dwStartVertex, sizeof( Gothic_XYZRHW_DIF_T1_Vertex ) );
			break;

		default:
			return S_OK;
		}

		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE EndScene() {
		DebugWrite( "MyDirect3DDevice7::EndScene" );

		hook_infunc

			Engine::GraphicsEngine->OnEndFrame();

		hook_outfunc

		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE EndStateBlock( LPDWORD lpdwBlockHandle ) {
		DebugWrite( "MyDirect3DDevice7::EndStateBlock" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE EnumTextureFormats( LPD3DENUMPIXELFORMATSCALLBACK lpd3dEnumPixelProc, LPVOID lpArg ) {
		DebugWrite( "MyDirect3DDevice7::EnumTextureFormats" );

        static std::array<DDPIXELFORMAT, 19> tformats = { {
            {32, DDPF_ALPHA, 0, 8, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_LUMINANCE, 0, 8, 0xFF, 0x00, 0x00, 0x00},
            {32, DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 0, 8, 0x0F, 0x00, 0x00, 0xF0},
            {32, DDPF_RGB, 0, 16, 0xF800, 0x7E0, 0x1F, 0x00},
            {32, DDPF_RGB | DDPF_ALPHAPIXELS, 0, 16, 0x7C00, 0x3E0, 0x1F, 0x8000},
            {32, DDPF_RGB | DDPF_ALPHAPIXELS, 0, 16, 0xF00, 0xF0, 0x0F, 0xF000},
            {32, DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 0, 16, 0xFF, 0x00, 0x00, 0xFF00},
            {32, DDPF_BUMPDUDV, 0, 16, 0xFF, 0xFF00, 0x00, 0x00},
            {32, DDPF_BUMPDUDV | DDPF_BUMPLUMINANCE, 0, 16, 0x1F, 0x3E0, 0xFC00, 0x00},
            {32, DDPF_RGB, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0x00},
            {32, DDPF_RGB | DDPF_ALPHAPIXELS, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000},
            {32, DDPF_FOURCC, MAKEFOURCC( 'Y','U','Y','2' ), 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, MAKEFOURCC( 'U','Y','V','Y' ), 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, MAKEFOURCC( 'A','Y','U','V' ), 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, FOURCC_DXT1, 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, FOURCC_DXT2, 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, FOURCC_DXT3, 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, FOURCC_DXT4, 0, 0x00, 0x00, 0x00, 0x00},
            {32, DDPF_FOURCC, FOURCC_DXT5, 0, 0x00, 0x00, 0x00, 0x00},
        } };

        for ( DDPIXELFORMAT& ppf : tformats )
            (*lpd3dEnumPixelProc)(&ppf, lpArg);

		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Load( LPDIRECTDRAWSURFACE7 lpDestTex, LPPOINT lpDestPoint, LPDIRECTDRAWSURFACE7 lpSrcTex, LPRECT lprcSrcRect, DWORD dwFlags ) {
		DebugWrite( "MyDirect3DDevice7::Load" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE MultiplyTransform( D3DTRANSFORMSTATETYPE dtstTransformStateType, LPD3DMATRIX lpD3DMatrix ) {
		DebugWrite( "MyDirect3DDevice7::MultiplyTransform" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE PreLoad( LPDIRECTDRAWSURFACE7 lpddsTexture ) {
		DebugWrite( "MyDirect3DDevice7::PreLoad" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE ValidateDevice( DWORD* pNumPasses ) {
		DebugWrite( "MyDirect3DDevice7::ValidateDevice" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE LightEnable( DWORD Index, BOOL Enable ) {
		DebugWrite( "MyDirect3DDevice7::LightEnable" );
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetLight( DWORD dwLightIndex, LPD3DLIGHT7 lpLight ) {
		DebugWrite( "MyDirect3DDevice7::SetLight" );
		return S_OK;
	}

private:
	D3DDEVICEDESC7 FakeDeviceDesc;
	int RefCount;
};
