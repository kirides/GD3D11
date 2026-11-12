#ifndef WORLD_TESS_CB_HLSL
#define WORLD_TESS_CB_HLSL
// b11: tessellation constants + height-map shading constants. Bound for every lit world draw (D3D12GraphicsEngine WorldTessCBData).

cbuffer WorldTessCB : register(b11)
{
    float3 TessCamPos;      float TessMaxFactor;     // camera world position; HS factor cap
    float  TessUnitsPerPx;  float TessRange;         // world units per pixel at distance 1 * target pixels per segment; smoothing range
    float  TessStrength;    float TessFadeStart;     // roundness scale; distance where smoothing starts to fade
    float  TessDispScale;   float HeightDepth;       // displacement peak-to-peak (0 = tess off); height-map depth in world units
    float  HeightShadowSteps; float HeightShading;   // micro-shadow march steps (0 = off); per-pixel height effects (0 = off)
    float  HeightRange;     float PomSteps;          // POM full to 75 % of this distance, off at 100 %; linear search steps
    float  PomFlags;        float TessActive;        // (uint) bit0 = POM, bit1 = silhouettes; 1 while the tess PSOs draw
};

#endif
