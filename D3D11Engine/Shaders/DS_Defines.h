struct DEFERRED_PS_OUTPUT
{
	float4 vDiffuse : SV_TARGET0;
	float2 vNrm : SV_TARGET1; 
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

struct FORWARD_PLUS_PS_OUTPUT
{
	float4 vColor : SV_TARGET0;
	float2 vNrm : SV_TARGET1;
	float2 vSI_SP : SV_TARGET2;
	float2 vVelocity : SV_TARGET3;
	float vReactiveMask : SV_TARGET4;
};


// Octahedral encoding: map a unit normal to [-1,1]^2 for R16G16_SNORM storage
// Reference: "A Survey of Efficient Representations for Independent Unit Vectors" (Cigolle et al. 2014)
float2 OctWrap(float2 v)
{
    return (1.0 - abs(v.yx)) * (v.xy >= 0.0 ? 1.0 : -1.0);
}

float2 EncodeNormalGBuffer(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : OctWrap(n.xy);
    return n.xy;
}

// Decode octahedral [-1,1]^2 back to a unit normal
float3 DecodeNormalGBuffer(float2 encoded)
{
    float3 n;
    n.z = 1.0 - abs(encoded.x) - abs(encoded.y);
    n.xy = n.z >= 0.0 ? encoded.xy : OctWrap(encoded.xy);
    return normalize(n);
}
