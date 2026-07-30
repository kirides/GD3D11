// Screen-space AO lookup for the lit Forward+ pixel shaders (World.hlsl / Vob.hlsl / Skeletal.hlsl).
//
// The AO mask is produced by D3D12GraphicsEngine::RenderSSAO from the PREVIOUS frame's COMPLETE depth buffer
// (m_PrevDepth, snapshotted by CopyDepthForAO after every depth-writing pass) rather than from this frame's
// depth prepass. That is what lets grass/foliage, decals and the other late depth writers both cast AO and be
// correctly shaded by it — the prepass never contained them.
//
// The price is that the mask lives in the PREVIOUS frame's screen space, so this reprojects into it per-pixel:
// the surface's world position is pushed through AoPrevViewProj (the exact camera that rasterized the
// snapshot) to get the UV it occupied last frame. For static geometry that is exact — not a camera-motion
// approximation — and it only lags on objects that themselves moved. Two rejections fall back to "unoccluded":
//   * the reprojected UV leaving the screen (the surface simply wasn't visible last frame), and
//   * a depth mismatch at that UV (disocclusion — last frame something ELSE was in front here, so its AO
//     has nothing to do with this surface).
//
// Requires, declared BEFORE this include: the ShadowCB reprojection tail (AoPrevViewProj/AoPrevDepthIndex/
// AoPrevProjZ*/AoReprojValid), `cbuffer AOCB { uint AoMaskIndex; }` and `SamplerState smpAoClamp`.

// Relative view-depth tolerance for the disocclusion test. Generous enough to survive the sub-pixel
// reprojection error on a steeply-slanted surface, tight enough to reject a genuinely different surface.
static const float kAoReprojDepthTolerance = 0.05;

float SampleScreenSpaceAO( float3 wpos )
{
    // No snapshot yet (first frame of a world, or right after a resize) — nothing to reproject into.
    if ( AoReprojValid < 0.5 ) return 1.0;

    float4 prevClip = mul( float4( wpos, 1.0 ), AoPrevViewProj );
    if ( prevClip.w <= 0.0 ) return 1.0;                      // behind last frame's camera
    float invW = 1.0 / prevClip.w;
    float2 uv = prevClip.xy * invW * float2( 0.5, -0.5 ) + 0.5;
    if ( any( uv < 0.0 ) || any( uv > 1.0 ) ) return 1.0;     // off-screen last frame

    // Disocclusion test: what the snapshot actually stored at that UV must be this same surface.
    Texture2D prevDepthTex = ResourceDescriptorHeap[AoPrevDepthIndex];
    float storedDepth = prevDepthTex.SampleLevel( smpAoClamp, uv, 0 ).r;
    if ( storedDepth <= 0.0 ) return 1.0;                     // reversed-Z: cleared == sky, no occluder data

    // Compare in linear view Z (reversed-Z depth is wildly non-uniform, so a raw depth epsilon is meaningless).
    float thisDepth = prevClip.z * invW;
    float zStored = AoPrevProjZY / ( storedDepth - AoPrevProjZX );
    float zThis   = AoPrevProjZY / ( thisDepth   - AoPrevProjZX );
    if ( abs( zStored - zThis ) > kAoReprojDepthTolerance * abs( zThis ) ) return 1.0;

    Texture2D aoTex = ResourceDescriptorHeap[AoMaskIndex];
    return aoTex.SampleLevel( smpAoClamp, uv, 0 ).r;
}
