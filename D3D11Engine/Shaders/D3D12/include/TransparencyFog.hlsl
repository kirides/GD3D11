// Self-fog for surfaces drawn after the height-fog pass (port of D3D11's TransparencyFog.h/Heightfog.h):
// each surface fogs from its own position, since the fullscreen pass only saw the depth behind it.
// Everything arrives through one per-frame heap CBV (TransparencyFrameData); no root parameters.
#ifndef D3D12_TRANSPARENCY_FOG_HLSL
#define D3D12_TRANSPARENCY_FOG_HLSL

#define ATMOSPHERE_BINDLESS 1
#include "AtmosphericScattering.hlsl"
#include "GammaSpaceAdd.hlsl"

// What the fog fades toward, picked by the draw's blend mode (D3D11 ETransparencyFog).
#define TF_MODE_OFF      0
#define TF_MODE_BLEND    1   // fog color
#define TF_MODE_ADD      2   // black: additive light is only attenuated
#define TF_MODE_MODULATE 3   // white: a multiply fades to a no-op

struct TransparencyFrameData {
    uint OpaqueSceneIndex;   // this frame's opaque scene copy, 0xFFFFFFFF = unavailable
    uint HeightfogIndex;     // CBV of HeightfogData
    uint AtmosphereIndex;    // CBV of AtmosphereData
    uint FogActive;          // the height-fog pass ran this frame
};

// Mirrors HeightFog.hlsl's PFXBuffer (ConstantBufferStructs.h HeightfogConstantBuffer).
struct HeightfogData {
    float4 ProjParams;
    float4x4 InvView;
    float3 CameraPosition;
    float FogHeight;
    float HeightFalloff;
    float GlobalDensity;
    float WeightZNear;
    float WeightZFar;
    float3 FogColorMod;
    float Pad2;
    float2 ProjAB;
    float2 JitterOffset;
};

TransparencyFrameData LoadTransparencyFrame( uint index )
{
    ConstantBuffer<TransparencyFrameData> cb = ResourceDescriptorHeap[index];
    TransparencyFrameData d;
    d.OpaqueSceneIndex = cb.OpaqueSceneIndex;
    d.HeightfogIndex = cb.HeightfogIndex;
    d.AtmosphereIndex = cb.AtmosphereIndex;
    d.FogActive = cb.FogActive;
    return d;
}

// Same math as HeightFog.hlsl's ComputeHeightFog, evaluated at the surface instead of the depth buffer.
float3 ApplyTransparencyFog( TransparencyFrameData tf, uint mode, float3 color, float3 worldPos )
{
    [branch]
    if ( tf.FogActive == 0 || mode == TF_MODE_OFF )
        return color;

    ConstantBuffer<HeightfogData> hf = ResourceDescriptorHeap[tf.HeightfogIndex];
    float3 rel = worldPos - hf.CameraPosition;
    rel.y -= hf.FogHeight;

    float lenOrig = distance( worldPos, hf.CameraPosition );
    float fogInt = length( rel ) * exp( -hf.HeightFalloff );
    float w = saturate( ( lenOrig - hf.WeightZNear ) / ( hf.WeightZFar - hf.WeightZNear ) );
    if ( abs( rel.y ) > 0.01 )
    {
        float t = hf.HeightFalloff * rel.y * w;
        fogInt *= ( abs( t ) > 0.0001 ? ( ( 1.0 - exp( -t ) ) / t ) : 1.0 );
    }
    float coverage = saturate( 1.0 - exp( -hf.GlobalDensity * w * fogInt ) ) * 0.85;   // HEIGHTFOG_MAX_OPACITY

    [branch]
    if ( mode == TF_MODE_ADD )      return color * ( 1.0 - coverage );
    [branch]
    if ( mode == TF_MODE_MODULATE ) return lerp( color, 1.0, coverage );

    ConstantBuffer<AtmosphereData> atmo = ResourceDescriptorHeap[tf.AtmosphereIndex];
    g_Atmosphere = atmo;
    float3 fogColor = ApplyAtmosphericScatteringGround( rel, hf.FogColorMod, true );
    fogColor = lerp( fogColor, float3( 0.12, 0.18, 0.27 ), saturate( -AC_LightPos.y * 4.0 ) );
    float darkness = 2.5;
    [branch]
    if ( AC_LightPos.y > 0.0 ) darkness -= AC_LightPos.y * 0.8;
    return lerp( color, saturate( fogColor / darkness ), coverage );
}

// Final color of a transparent draw: DX7's gamma-space add for ADD, then self-fog. linearColor is the
// draw's normal output, srgbColor the same color before linearization. Invalid frameIndex = unchanged.
float3 FinishTransparentColor( uint frameIndex, uint mode, float2 pixel, float3 worldPos,
                               float3 srgbColor, float3 linearColor, float alpha )
{
    // Mode OFF also covers callers that leave the index 0: nothing may be loaded through it then.
    [branch]
    if ( frameIndex == 0xFFFFFFFFu || mode == TF_MODE_OFF )
        return linearColor;

    TransparencyFrameData tf = LoadTransparencyFrame( frameIndex );
    float3 rgb = linearColor;
    [branch]
    if ( mode == TF_MODE_ADD && tf.OpaqueSceneIndex != 0xFFFFFFFFu && alpha > ( 1.0 / 255.0 ) )
        rgb = GammaSpaceAddSource( tf.OpaqueSceneIndex, pixel, srgbColor, alpha );
    return ApplyTransparencyFog( tf, mode, rgb, worldPos );
}

#endif // D3D12_TRANSPARENCY_FOG_HLSL
