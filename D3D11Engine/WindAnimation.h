#pragma once

struct VS_ExConstantBuffer_Wind;

// Advances the global wind direction / time / strength state used by the instanced-VOB wind-sway
// vertex shader (flags, foliage, leaves). Backend-neutral: shared by D3D11GraphicsEngine::ApplyWindProps
// and D3D12GraphicsEngine's per-frame update, so both renderers see identical wind behavior. Writes
// windDir/globalTime into windBuff and updates the extern vobAnimation_WindStrength (consumed by
// GothicAPI::ProcessVobAnimation when filling each vob instance's per-instance windStrenth). Callers are
// responsible for filling windBuff.minHeight/maxHeight/playerPos themselves (those are per-draw/per-visual,
// not global state) and for only calling this while the game isn't paused.
void UpdateWindAnimation( VS_ExConstantBuffer_Wind& windBuff );
