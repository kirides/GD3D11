// Screen-space AO lookup for the lit Forward+ pixel shaders (World / Vob / Skeletal / Vegetation / Decal).
//
// The AO mask is produced by D3D12GraphicsEngine::RenderSSAO from THIS frame's DEPTH PREPASS, immediately after
// the prepass and before any lit pass runs. The mask is therefore already in this frame's screen space: the
// lookup is a plain read at the pixel being shaded — no reprojection, no disocclusion test, no one-frame lag.
// (It used to run off a full-frame depth SNAPSHOT of the PREVIOUS frame, which bought coverage of the late
// depth writers at the cost of all of the above; vegetation joining the prepass is what made that trade
// unnecessary — see D3D12Scene.cpp's DrawVegetationDepthPrepass.)
//
// COVERAGE. The prepass contains the world mesh, instanced VOBs, skeletals + node attachments and vegetation
// (the last range-limited to kVegetationPrepassRange). Water, decals, particles and the blended transparencies
// write depth later and are not AO occluders; a pixel of one of those simply reads the mask of the opaque
// surface behind it, which is the right answer for a decal and a harmless one for the rest.
//
// Requires, declared BEFORE this include: `cbuffer AOCB { uint AoMaskIndex; }`, `SamplerState smpAoClamp`,
// and the ShadowCB screen-space-AO block that carries AoInvRes. (AoInvRes rides ShadowCB rather than AOCB
// because the World root signature is at 63 of its 64 DWORDs — two more root constants would not fit.)

// SampleLevel through the clamp sampler rather than Load(): when AO is off or its resources are unavailable the
// host binds the 1x1 WHITE texture here, and a Load() at any real screen coordinate would fall outside that
// single texel and return 0 — fully occluded — instead of 1.
float SampleScreenSpaceAO( float2 svPos )
{
    Texture2D aoTex = ResourceDescriptorHeap[AoMaskIndex];
    return aoTex.SampleLevel( smpAoClamp, svPos * AoInvRes, 0 ).r;
}
