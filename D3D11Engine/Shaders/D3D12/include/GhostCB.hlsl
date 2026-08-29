// Ghost/transparency fade (D3D12PipelineState::CreateGhost): a per-vob fade alpha multiplied onto the
// unlit diffuse sample, no alpha-clip (a fading ghost should smoothly disappear, not pop). Shared by
// Preview.hlsl and Skeletal.hlsl's PSGhost. Register differs per shader — #define GHOSTCB_REGISTER
// before including this file.
#ifndef D3D12_GHOSTCB_HLSL
#define D3D12_GHOSTCB_HLSL

#ifndef GHOSTCB_REGISTER
#define GHOSTCB_REGISTER b2
#endif

cbuffer GhostCB : register(GHOSTCB_REGISTER) { float GhostAlpha; float3 _GhostPad; };

#endif // D3D12_GHOSTCB_HLSL
