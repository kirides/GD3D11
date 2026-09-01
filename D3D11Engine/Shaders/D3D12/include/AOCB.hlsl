// Simple-SSAO mask slot (bindless, set once per frame — see D3D12GraphicsEngine::RenderSSAO/
// m_ActiveAOMaskSrvSlot). Points at the white 1x1 texture (mask = no occlusion) when SSAO is
// disabled/unavailable. Register differs per shader — #define AOCB_REGISTER before including this file.
#ifndef D3D12_AOCB_HLSL
#define D3D12_AOCB_HLSL

#ifndef AOCB_REGISTER
#define AOCB_REGISTER b7
#endif

cbuffer AOCB : register(AOCB_REGISTER) { uint AoMaskIndex; };

#endif // D3D12_AOCB_HLSL
