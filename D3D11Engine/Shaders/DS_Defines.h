struct DEFERRED_PS_OUTPUT
{
	float4 vDiffuse : SV_TARGET0;
	float4 vNrm : SV_TARGET1; 
	float2 vSI_SP : SV_TARGET2;
	float2 vVelocity : SV_TARGET3;  // Screen-space velocity for motion vectors
};

struct DEFERRED_PS_OUTPUT_ALPHA_TO_COVERAGE
{
	float4 vDiffuse : SV_TARGET0;
	float4 vNrm_SI_SP : SV_TARGET1; 
	uint fCoverage	: SV_Coverage;
};



float2 EncodeNormal(float3 n)
{
    float f = sqrt(8*n.z+8);
    return n.xy / f + 0.5;
}
float3 DecodeNormal(float2 enc)
{
    float2 fenc = enc.xy*4-2;
    float f = dot(fenc,fenc);
    float g = sqrt(1-f/4);
    float3 n;
    n.xy = fenc*g;
    n.z = 1-f/2;
    return n;
}
