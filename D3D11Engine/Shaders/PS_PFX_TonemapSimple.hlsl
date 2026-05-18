//--------------------------------------------------------------------------------------
// Simple non-adaptive tonemap pass for SDR/HDR-disabled mode
//--------------------------------------------------------------------------------------

SamplerState SS_Linear : register( s0 );
Texture2D TX_Scene : register( t0 );

cbuffer HDR_Settings : register( b0 ) {
    float HDR_MiddleGray;
    float HDR_LumWhite;
    float HDR_Threshold;
    float HDR_BloomStrength;
};

struct PS_INPUT {
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float4 PSMain( PS_INPUT Input ) : SV_TARGET {
    float3 color = TX_Scene.Sample( SS_Linear, Input.vTexcoord ).rgb;

    // Stable non-adaptive exposure from user middle-gray; no temporal eye adaptation.
    float exposure = max( HDR_MiddleGray, 0.001f ) / 0.18f;
    float3 exposed = color * exposure;

    // Modified Reinhard with user white point for brighter mids/highlights than plain Reinhard.
    float white = max( HDR_LumWhite, 0.001f );
    float whiteSq = white * white;
    float3 toneMapped = (exposed * (1.0f + exposed / whiteSq)) / (1.0f + exposed);

    return float4( pow( saturate( toneMapped ), 2.2f ), 1.0f );
}
