struct DEFERRED_PS_OUTPUT
{
	float4 vDiffuse : SV_TARGET0;
	float4 vNrm : SV_TARGET1; 
	float2 vSI_SP : SV_TARGET2;
	float2 vVelocity : SV_TARGET3;  // Screen-space velocity for motion vectors
	float vReactiveMask : SV_TARGET4;  // Screen-space velocity for motion vectors
};

struct DEFERRED_PS_OUTPUT_ALPHA_TO_COVERAGE
{
	float4 vDiffuse : SV_TARGET0;
	float4 vNrm_SI_SP : SV_TARGET1; 
	uint fCoverage	: SV_Coverage;
};



// Encode a normalized view-space normal [-1,1] to UNORM [0,1] for R10G10B10A2_UNORM storage
float4 EncodeNormalGBuffer(float3 n, float alpha)
{
    return float4(n * 0.5 + 0.5, alpha);
}

// Decode a UNORM [0,1] sample back to a normalized view-space normal [-1,1]
float3 DecodeNormalGBuffer(float3 encoded)
{
    return normalize(encoded * 2.0 - 1.0);
}
