// TAA Pixel Shader - Improved with Velocity Buffer Support
// Features: Depth discontinuity detection, edge-aware blending, variance clipping,
//           alpha-edge detection, integrated sharpening
cbuffer TAAConstants : register(b0) {
    float4x4 InvViewProj;      // Current frame's UNJITTERED inverse view-projection
    float4x4 PrevViewProj;     // Previous frame's UNJITTERED view-projection
    float2 JitterOffset;       // Current jitter in UV space
    float2 Resolution;
    float BlendFactor;
    float MotionScale;
    float2 Padding;
};

SamplerState SS_Linear : register(s0);
SamplerState SS_Point : register(s1);

Texture2D TX_Texture0 : register(t0);  // Current frame
Texture2D TX_Texture1 : register(t1);  // History buffer
Texture2D TX_Texture2 : register(t2);  // Depth buffer
Texture2D TX_Texture3 : register(t3);  // Velocity buffer (RG16F)

struct PS_INPUT
{
    float2 vTexcoord   : TEXCOORD0;
    float3 vEyeRay     : TEXCOORD1;
    float4 vPosition   : SV_POSITION;
};

// ============================================================================
// Color space conversions for better blending
// ============================================================================

float3 RGB_YCoCg(float3 rgb) {
    float Y = dot(rgb, float3(0.25, 0.5, 0.25));
    float Co = dot(rgb, float3(0.5, 0.0, -0.5));
    float Cg = dot(rgb, float3(-0.25, 0.5, -0.25));
    return float3(Y, Co, Cg);
}

float3 YCoCg_RGB(float3 ycocg) {
    float Y = ycocg.x;
    float Co = ycocg.y;
    float Cg = ycocg.z;
    return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

// Tonemap and inverse for stable blending in HDR
float3 Tonemap(float3 color) {
    return color / (1.0 + max(color.r, max(color.g, color.b)));
}

float3 TonemapInvert(float3 color) {
    return color / max(1.0 - max(color.r, max(color.g, color.b)), 0.001);
}

// ============================================================================
// Soft neighborhood clamping using clip towards center
// ============================================================================

float3 ClipAABB(float3 aabbMin, float3 aabbMax, float3 prevSample, float3 center) {
    float3 extents = 0.5 * (aabbMax - aabbMin) + 0.0001;
    
    float3 offset = prevSample - center;
    float3 ts = abs(extents / (offset + 0.0001));
    float t = saturate(min(min(ts.x, ts.y), ts.z));
    
    return center + offset * t;
}

// ============================================================================
// Utility functions
// ============================================================================

float Luminance(float3 color) {
    return dot(color, float3(0.299, 0.587, 0.114));
}

// ============================================================================
// Depth discontinuity detection
// Detects edges where depth changes sharply (object silhouettes)
// Returns a value 0-1 where higher values indicate stronger discontinuity
// ============================================================================

float DetectDepthDiscontinuity(float2 texCoord, float2 pixelSize, out float centerDepth) {
    // Sample depth in a cross pattern for edge detection
    centerDepth = TX_Texture2.SampleLevel(SS_Point, texCoord, 0).r;
    
    float depthLeft = TX_Texture2.SampleLevel(SS_Point, texCoord + float2(-pixelSize.x, 0), 0).r;
    float depthRight = TX_Texture2.SampleLevel(SS_Point, texCoord + float2(pixelSize.x, 0), 0).r;
    float depthUp = TX_Texture2.SampleLevel(SS_Point, texCoord + float2(0, -pixelSize.y), 0).r;
    float depthDown = TX_Texture2.SampleLevel(SS_Point, texCoord + float2(0, pixelSize.y), 0).r;
    
    // Calculate depth gradients
    float gradX = abs(depthRight - depthLeft);
    float gradY = abs(depthDown - depthUp);
    
    // Combined gradient magnitude
    float gradientMagnitude = sqrt(gradX * gradX + gradY * gradY);
    
    // Normalize by depth to make detection distance-independent
    // In reversed-Z, larger depth = closer to camera
    // We want the threshold to be relative to the depth
    float depthThreshold = max(centerDepth, 0.001) * 0.05; // 5% of current depth
    
    // Calculate discontinuity factor
    float discontinuity = saturate(gradientMagnitude / depthThreshold);
    
    // Also check for diagonal discontinuities for better corner detection
    float depthTL = TX_Texture2.SampleLevel(SS_Point, texCoord + float2(-pixelSize.x, -pixelSize.y), 0).r;
    float depthBR = TX_Texture2.SampleLevel(SS_Point, texCoord + float2(pixelSize.x, pixelSize.y), 0).r;
    float depthTR = TX_Texture2.SampleLevel(SS_Point, texCoord + float2(pixelSize.x, -pixelSize.y), 0).r;
    float depthBL = TX_Texture2.SampleLevel(SS_Point, texCoord + float2(-pixelSize.x, pixelSize.y), 0).r;
    
    float diagGrad1 = abs(depthBR - depthTL);
    float diagGrad2 = abs(depthBL - depthTR);
    float diagGradient = max(diagGrad1, diagGrad2) * 0.707; // Scale by 1/sqrt(2) for diagonal
    
    float diagDiscontinuity = saturate(diagGradient / depthThreshold);
    
    return max(discontinuity, diagDiscontinuity);
}

// ============================================================================
// Detect high-frequency color edges (alpha-tested geometry like vegetation)
// These areas need special handling to prevent thickening
// ============================================================================

float DetectColorEdge(float2 texCoord, float2 pixelSize) {
    // Sample luminance in cross pattern
    float3 center = TX_Texture0.SampleLevel(SS_Point, texCoord, 0).rgb;
    float3 left = TX_Texture0.SampleLevel(SS_Point, texCoord + float2(-pixelSize.x, 0), 0).rgb;
    float3 right = TX_Texture0.SampleLevel(SS_Point, texCoord + float2(pixelSize.x, 0), 0).rgb;
    float3 up = TX_Texture0.SampleLevel(SS_Point, texCoord + float2(0, -pixelSize.y), 0).rgb;
    float3 down = TX_Texture0.SampleLevel(SS_Point, texCoord + float2(0, pixelSize.y), 0).rgb;
    
    float lumCenter = Luminance(center);
    float lumLeft = Luminance(left);
    float lumRight = Luminance(right);
    float lumUp = Luminance(up);
    float lumDown = Luminance(down);
    
    // Calculate luminance gradients
    float gradX = abs(lumRight - lumLeft);
    float gradY = abs(lumDown - lumUp);
    float gradient = sqrt(gradX * gradX + gradY * gradY);
    
    // Vegetation/alpha edges typically have high luminance contrast
    // (bright leaves against dark background or sky)
    float maxLum = max(max(max(lumLeft, lumRight), max(lumUp, lumDown)), lumCenter);
    float minLum = min(min(min(lumLeft, lumRight), min(lumUp, lumDown)), lumCenter);
    float contrast = (maxLum - minLum) / (maxLum + 0.01);
    
    // Combine gradient and contrast for edge detection
    // High values indicate likely alpha-tested geometry edges
    return saturate(gradient * 2.0 + contrast * 0.5);
}

// ============================================================================
// Get the closest depth in a 3x3 neighborhood (for better motion vector sampling)
// Note: This engine uses REVERSED-Z: depth 0 = far (sky), depth 1 = near
// ============================================================================

float2 GetClosestDepthOffset(float2 texCoord, float2 pixelSize) {
    float closestDepth = 0.0;  // Start at far (0 in reversed-Z)
    float2 closestOffset = float2(0, 0);
    
    [unroll]
    for (int x = -1; x <= 1; x++) {
        [unroll]
        for (int y = -1; y <= 1; y++) {
            float2 offset = float2(x, y) * pixelSize;
            float depth = TX_Texture2.SampleLevel(SS_Point, texCoord + offset, 0).r;
            // In reversed-Z, larger depth = closer to camera
            if (depth > closestDepth) {
                closestDepth = depth;
                closestOffset = offset;
            }
        }
    }
    
    return closestOffset;
}

// ============================================================================
// Catmull-Rom filtering for sharper history sampling
// ============================================================================

float3 SampleHistoryCatmullRom(float2 uv) {
    float2 texSize = Resolution;
    float2 invTexSize = 1.0 / texSize;
    
    // Compute sample position
    float2 position = uv * texSize;
    float2 centerPosition = floor(position - 0.5) + 0.5;
    float2 f = position - centerPosition;
    float2 f2 = f * f;
    float2 f3 = f * f2;
    
    // Catmull-Rom weights
    float2 w0 = -0.5 * f3 + f2 - 0.5 * f;
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    float2 w3 = 0.5 * f3 - 0.5 * f2;
    
    // Optimized bilinear samples
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / (w12 + 0.0001);
    
    float2 tc0 = (centerPosition - 1.0) * invTexSize;
    float2 tc3 = (centerPosition + 2.0) * invTexSize;
    float2 tc12 = (centerPosition + offset12) * invTexSize;
    
    // Clamp to valid UV range to prevent sampling outside texture
    tc0 = clamp(tc0, 0.0, 1.0);
    tc3 = clamp(tc3, 0.0, 1.0);
    tc12 = clamp(tc12, 0.0, 1.0);
    
    // Sample using bilinear filtering
    float3 result = float3(0, 0, 0);
    result += TX_Texture1.SampleLevel(SS_Linear, float2(tc12.x, tc0.y), 0).rgb * (w12.x * w0.y);
    result += TX_Texture1.SampleLevel(SS_Linear, float2(tc0.x, tc12.y), 0).rgb * (w0.x * w12.y);
    result += TX_Texture1.SampleLevel(SS_Linear, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y);
    result += TX_Texture1.SampleLevel(SS_Linear, float2(tc3.x, tc12.y), 0).rgb * (w3.x * w12.y);
    result += TX_Texture1.SampleLevel(SS_Linear, float2(tc12.x, tc3.y), 0).rgb * (w12.x * w3.y);
    
    return max(result, 0.0);
}

// ============================================================================
// Gather neighborhood statistics using 3x3 with cross weighting
// ============================================================================

void GatherNeighborhood(float2 texCoord, float2 pixelSize, 
    out float3 m1, out float3 m2, out float3 neighborMin, out float3 neighborMax, out float3 centerColor) {
    
    m1 = float3(0, 0, 0);
    m2 = float3(0, 0, 0);
    neighborMin = float3(1e10, 1e10, 1e10);
    neighborMax = float3(-1e10, -1e10, -1e10);
    
    // Weights: higher weight for cross pattern (direct neighbors)
    static const float weights[9] = { 0.5, 1.0, 0.5, 1.0, 1.5, 1.0, 0.5, 1.0, 0.5 };
    float totalWeight = 0.0;
    int idx = 0;
    
    [unroll]
    for (int y = -1; y <= 1; y++) {
        [unroll]
        for (int x = -1; x <= 1; x++) {
            float2 offset = float2(x, y) * pixelSize;
            float3 neighbor = TX_Texture0.SampleLevel(SS_Linear, texCoord + offset, 0).rgb;
            float3 tonemapped = Tonemap(neighbor);
            
            float w = weights[idx++];
            m1 += tonemapped * w;
            m2 += tonemapped * tonemapped * w;
            totalWeight += w;
            
            neighborMin = min(neighborMin, tonemapped);
            neighborMax = max(neighborMax, tonemapped);
            
            if (x == 0 && y == 0) {
                centerColor = neighbor;
            }
        }
    }
    
    m1 /= totalWeight;
    m2 /= totalWeight;
}

// ============================================================================
// TAA-integrated sharpening to counteract temporal blur
// Uses a modified unsharp mask that's edge-aware
// ============================================================================

float3 ApplyTAASharpening(float3 color, float2 texCoord, float2 pixelSize, float strength) {
    // Sample neighbors for sharpening
    float3 left = TX_Texture0.SampleLevel(SS_Linear, texCoord + float2(-pixelSize.x, 0), 0).rgb;
    float3 right = TX_Texture0.SampleLevel(SS_Linear, texCoord + float2(pixelSize.x, 0), 0).rgb;
    float3 up = TX_Texture0.SampleLevel(SS_Linear, texCoord + float2(0, -pixelSize.y), 0).rgb;
    float3 down = TX_Texture0.SampleLevel(SS_Linear, texCoord + float2(0, pixelSize.y), 0).rgb;
    
    // Simple sharpening kernel: center - average of neighbors
    float3 blur = (left + right + up + down) * 0.25;
    float3 sharpened = color + (color - blur) * strength;
    
    // Prevent negative values and excessive brightening
    return max(sharpened, 0.0);
}

// ============================================================================
// Main TAA resolve
// ============================================================================

float4 PSMain(PS_INPUT Input) : SV_TARGET {
    float2 texCoord = Input.vTexcoord;
    float2 pixelSize = 1.0 / Resolution;
    
    // Detect depth discontinuity for edge-aware blending
    float centerDepth;
    float depthDiscontinuity = DetectDepthDiscontinuity(texCoord, pixelSize, centerDepth);
    
    // Detect color edges (helps identify alpha-tested geometry like vegetation)
    float colorEdge = DetectColorEdge(texCoord, pixelSize);
    
    // Combined edge factor - both depth and color discontinuities
    float edgeFactor = max(depthDiscontinuity, colorEdge * 0.7);
    
    // Sample current frame at jitter-corrected position for sharper output
    float2 unjitteredUV = texCoord - JitterOffset;
    float3 currentColor = TX_Texture0.SampleLevel(SS_Linear, unjitteredUV, 0).rgb;
    
    // Find the closest depth in neighborhood for motion vector sampling
    // This helps reduce ghosting on edges of moving objects
    float2 closestOffset = GetClosestDepthOffset(texCoord, pixelSize);
    
    // Sample velocity from velocity buffer at closest depth location
    float2 velocity = TX_Texture3.SampleLevel(SS_Point, texCoord + closestOffset, 0).rg;
    
    // Scale velocity if needed
    velocity *= MotionScale;
    
    // Velocity magnitude in pixels
    float velocityLengthPixels = length(velocity * Resolution);
    
    // Calculate reprojected UV using velocity
    float2 prevUV = texCoord - velocity;
    
    // Check if reprojected position is outside screen
    bool offScreen = prevUV.x < 0.0 || prevUV.x > 1.0 || prevUV.y < 0.0 || prevUV.y > 1.0;
    
    // Sample history with Catmull-Rom for sharper result
    float3 historyColor = offScreen ? currentColor : SampleHistoryCatmullRom(prevUV);
    
    // Gather neighborhood statistics
    float3 m1, m2, neighborMin, neighborMax, centerColor;
    GatherNeighborhood(texCoord, pixelSize, m1, m2, neighborMin, neighborMax, centerColor);
    
    // Calculate standard deviation
    float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
    
    // Adaptive gamma based on velocity, depth discontinuity, and color edges
    // Tighter clipping for fast motion and at all types of edges
    float velocityFactor = saturate(velocityLengthPixels * 0.1);
    float combinedFactor = max(velocityFactor, edgeFactor);
    
    // For vegetation/alpha edges, use even tighter clipping to prevent thickening
    float gamma = lerp(1.0, 0.3, combinedFactor);
    
    // Variance-based clipping bounds in tonemapped space
    float3 clipMin = m1 - gamma * sigma;
    float3 clipMax = m1 + gamma * sigma;
    
    // Also constrain to neighborhood min/max for robustness
    clipMin = max(clipMin, neighborMin);
    clipMax = min(clipMax, neighborMax);
    
    // Tonemap history for stable clipping
    float3 tonemappedHistory = Tonemap(historyColor);
    
    // Convert to YCoCg for perceptually better clamping
    float3 historyYCoCg = RGB_YCoCg(tonemappedHistory);
    float3 clipMinYCoCg = RGB_YCoCg(clipMin);
    float3 clipMaxYCoCg = RGB_YCoCg(clipMax);
    float3 centerYCoCg = RGB_YCoCg(m1);
    
    // Clip history to neighborhood bounds
    float3 clampedHistoryYCoCg = ClipAABB(clipMinYCoCg, clipMaxYCoCg, historyYCoCg, centerYCoCg);
    float3 clampedHistory = YCoCg_RGB(clampedHistoryYCoCg);
    
    // Inverse tonemap back to linear
    clampedHistory = TonemapInvert(clampedHistory);
    
    // Calculate how much history was clipped (for adaptive blending)
    float clipDistance = length(tonemappedHistory - YCoCg_RGB(clampedHistoryYCoCg));
    float clipAmount = saturate(clipDistance * 5.0);
    
    if (offScreen) {
        clampedHistory = currentColor;
        clipAmount = 1.0;
    }
    
    // ========================================================================
    // Adaptive blend factor calculation with edge awareness
    // ========================================================================
    
    // Base blend factor
    float adaptiveBlend = BlendFactor;
    
    // Increase blend for high motion to reduce smearing
    float motionBlend = saturate(velocityLengthPixels * 0.025);
    adaptiveBlend = max(adaptiveBlend, motionBlend);
    
    // Increase blend when history was heavily clipped
    adaptiveBlend = max(adaptiveBlend, clipAmount * 0.35);
    
    // IMPORTANT: Increase blend at depth discontinuities (object edges)
    // This reduces ghosting at silhouettes where reprojection is unreliable
    float edgeBlend = depthDiscontinuity * 0.5;
    adaptiveBlend = max(adaptiveBlend, edgeBlend);
    
    // IMPORTANT: Increase blend at color edges (vegetation/alpha geometry)
    // This prevents temporal accumulation from making thin geometry appear thicker
    float alphaEdgeBlend = colorEdge * 0.6;
    adaptiveBlend = max(adaptiveBlend, alphaEdgeBlend);
    
    // For very high velocity, blend even more towards current frame
    if (velocityLengthPixels > 8.0) {
        adaptiveBlend = lerp(adaptiveBlend, 0.7, saturate((velocityLengthPixels - 8.0) * 0.06));
    }
    
    // Anti-flicker: reduce temporal weight on high-contrast edges
    float lumCurrent = Luminance(Tonemap(currentColor));
    float lumHistory = Luminance(Tonemap(clampedHistory));
    float lumContrast = abs(lumCurrent - lumHistory) / max(max(lumCurrent, lumHistory), 0.1);
    
    // Only increase blend for very high contrast to avoid losing temporal stability
    if (lumContrast > 0.5) {
        adaptiveBlend = max(adaptiveBlend, lerp(adaptiveBlend, 0.4, saturate((lumContrast - 0.5) * 2.0)));
    }
    
    // Clamp final blend factor
    adaptiveBlend = clamp(adaptiveBlend, 0.04, 1.0);
    
    // ========================================================================
    // Final blend with luminance weighting for reduced flickering
    // ========================================================================
    
    // Weight by luminance for reduced flickering
    float weightCurrent = 1.0 / (1.0 + lumCurrent);
    float weightHistory = 1.0 / (1.0 + lumHistory);
    
    // Blend with luminance weighting
    float3 result = (currentColor * weightCurrent * adaptiveBlend + 
                     clampedHistory * weightHistory * (1.0 - adaptiveBlend)) /
                    (weightCurrent * adaptiveBlend + weightHistory * (1.0 - adaptiveBlend) + 0.0001);
    
    // ========================================================================
    // Apply subtle integrated sharpening to counteract TAA blur
    // The sharpening is adaptive: stronger in non-edge areas, subtle at edges
    // ========================================================================
    
    // Sharpen amount: reduce at edges to prevent artifacts, increase in flat areas
    // Base sharpening of 0.15 is subtle but helps with clarity
    float sharpenStrength = lerp(0.15, 0.05, edgeFactor);
    result = ApplyTAASharpening(result, texCoord, pixelSize, sharpenStrength);
    
    return float4(result, 1.0);
}