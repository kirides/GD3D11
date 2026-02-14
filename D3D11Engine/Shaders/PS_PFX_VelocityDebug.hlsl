//--------------------------------------------------------------------------------------
// Velocity Buffer Debug Visualization Shader
// Amplifies small velocity values to make them visible
//--------------------------------------------------------------------------------------

cbuffer VelocityDebugCB : register(b0)
{
    float VD_Amplification;     // Multiplier for velocity values (e.g., 10-100)
    float VD_ShowMagnitude;     // 0 = show direction as RG, 1 = show magnitude as grayscale
    float VD_AbsoluteMode;      // 0 = signed (-1 to 1), 1 = absolute values
    float VD_Padding;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register(s0);
Texture2D TX_Velocity : register(t0);

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
    float2 vTexcoord		: TEXCOORD0;
    float3 vEyeRay			: TEXCOORD1;
    float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain(PS_INPUT Input) : SV_TARGET
{
    float2 velocity = TX_Velocity.Sample(SS_Linear, Input.vTexcoord).rg;
    
    // Amplify the velocity
    velocity *= VD_Amplification;
    
    float4 output;
    
    if (VD_ShowMagnitude > 0.5)
    {
        // Magnitude mode: show velocity length as grayscale
        float magnitude = length(velocity);
        
        // Color code by magnitude
        // Green = slow, Yellow = medium, Red = fast
        if (magnitude < 0.5)
        {
            output = float4(0, magnitude * 2.0, 0, 1); // Green
        }
        else if (magnitude < 1.0)
        {
            float t = (magnitude - 0.5) * 2.0;
            output = float4(t, 1.0, 0, 1); // Green to Yellow
        }
        else
        {
            float t = saturate(magnitude - 1.0);
            output = float4(1.0, 1.0 - t, 0, 1); // Yellow to Red
        }
    }
    else
    {
        // Direction mode: show velocity as color
        if (VD_AbsoluteMode > 0.5)
        {
            // Absolute mode: R = |velocity.x|, G = |velocity.y|
            output = float4(abs(velocity.x), abs(velocity.y), 0, 1);
        }
        else
        {
            // Signed mode: remap -1..1 to 0..1 for visualization
            // Neutral (0,0) = gray (0.5, 0.5)
            // Positive = brighter, Negative = darker
            float2 remapped = velocity * 0.5 + 0.5;
            output = float4(remapped.x, remapped.y, 0.5, 1);
        }
    }
    
    return output;
}
