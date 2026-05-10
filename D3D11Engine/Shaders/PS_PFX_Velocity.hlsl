// Velocity Buffer Pixel Shader with Dilation
// Generates screen-space motion vectors from depth buffer reprojection
// Includes velocity dilation to propagate motion to object edges

cbuffer VelocityConstants : register(b0) {
    float4x4 InvViewProj;      // Current frame's UNJITTERED inverse view-projection
    float4x4 PrevViewProj;     // Previous frame's UNJITTERED view-projection
    float2 JitterOffset;       // Current jitter in UV space
    float2 PrevJitterOffset;   // Previous jitter in UV space
    float2 Resolution;
    float2 Padding;
};

SamplerState SS_Linear : register(s0);
SamplerState SS_Point : register(s1);

Texture2D TX_Depth : register(t0);  // Current frame depth

struct PS_INPUT
{
    float2 vTexcoord   : TEXCOORD0;
    float3 vEyeRay     : TEXCOORD1;
    float4 vPosition   : SV_POSITION;
};

// Reconstruct world position from depth
// Note: This engine uses REVERSED-Z: depth 1 = near, depth 0 = far (sky)
float3 ReconstructWorldPosition(float2 uv, float depth) {
    // Convert UV to NDC (clip space)
    // UV (0,0) is top-left, NDC (-1,-1) is bottom-left
    float4 clipPos = float4(
        uv.x * 2.0 - 1.0,           // X: [0,1] -> [-1,1]
        (1.0 - uv.y) * 2.0 - 1.0,   // Y: [0,1] -> [1,-1] (flip Y for NDC)
        depth,                       // Z: use depth directly (reversed-Z)
        1.0
    );
    
    // Transform to world space
    float4 worldPos = mul(clipPos, InvViewProj);
    return worldPos.xyz / worldPos.w;
}

// Project world position to previous frame's clip space
float2 ProjectToPreviousFrame(float3 worldPos) {
    float4 prevClipPos = mul(float4(worldPos, 1.0), PrevViewProj);
    
    // Perspective divide
    float2 prevNDC = prevClipPos.xy / prevClipPos.w;
    
    // Convert NDC to UV
    float2 prevUV = float2(
        prevNDC.x * 0.5 + 0.5,          // X: [-1,1] -> [0,1]
        1.0 - (prevNDC.y * 0.5 + 0.5)   // Y: [-1,1] -> [1,0] (flip Y back)
    );
    
    return prevUV;
}

// Calculate velocity for a single sample point
float2 CalculateVelocity(float2 texCoord, float depth) {
    // Skip sky pixels (depth = 0 in reversed-Z means far plane/sky)
    if (!(depth > 0.0f)) {
        return float2(0.0, 0.0);
    }

    // Remove current jitter from UV to get the unjittered sample position
    float2 currentUV = texCoord - JitterOffset;

    // Reconstruct world position at this pixel
    float3 worldPos = ReconstructWorldPosition(currentUV, depth);

    // Project to previous frame (unjittered)
    float2 prevUV = ProjectToPreviousFrame(worldPos);

    // Calculate velocity in unjittered space
    return currentUV - prevUV;
}

// 3x3 velocity dilation kernel
// Takes the velocity from the sample with the largest motion magnitude
// This propagates motion from moving objects into their edges
float2 DilateVelocity3x3(float2 texCoord, float2 pixelSize) {
    float2 bestVelocity = float2(0, 0);
    float bestMagnitudeSq = 0.0;
    float closestDepth = 0.0;  // For reversed-Z, start at far (0)
    
    // Sample 3x3 neighborhood
    [unroll]
    for (int y = -1; y <= 1; y++) {
        [unroll]
        for (int x = -1; x <= 1; x++) {
            float2 offset = float2(x, y) * pixelSize;
            float2 sampleUV = texCoord + offset;
            
            // Clamp to valid UV range
            sampleUV = clamp(sampleUV, pixelSize, 1.0 - pixelSize);
            
            float depth = TX_Depth.SampleLevel(SS_Point, sampleUV, 0).r;
            float2 velocity = CalculateVelocity(sampleUV, depth);
            float magnitudeSq = dot(velocity, velocity);
            
            // Strategy: Take velocity from the closest (highest depth in reversed-Z)
            // sample that has significant motion
            // This helps propagate motion from foreground objects to their edges
            
            // If this sample is closer to camera (higher depth in reversed-Z)
            // and has meaningful velocity, prefer it
            bool isCloser = depth > closestDepth + 0.001;
            bool hasMoreMotion = magnitudeSq > bestMagnitudeSq * 1.5; // 50% more motion threshold
            
            if (isCloser || (abs(depth - closestDepth) < 0.01 && hasMoreMotion)) {
                if (magnitudeSq > 0.0000001 || isCloser) { // Has some velocity or is closer
                    closestDepth = depth;
                    bestVelocity = velocity;
                    bestMagnitudeSq = max(bestMagnitudeSq, magnitudeSq);
                }
            }
        }
    }
    
    // If we found no significant velocity, fall back to center sample
    if (bestMagnitudeSq < 0.0000001) {
        float centerDepth = TX_Depth.SampleLevel(SS_Point, texCoord, 0).r;
        bestVelocity = CalculateVelocity(texCoord, centerDepth);
    }
    
    return bestVelocity;
}

float2 PSMain(PS_INPUT Input) : SV_TARGET {
    float2 texCoord = Input.vTexcoord;
    float2 pixelSize = 1.0 / Resolution;
    
    // Use dilated velocity for better edge handling
    float2 velocity = DilateVelocity3x3(texCoord, pixelSize);
    
    return velocity;
}
