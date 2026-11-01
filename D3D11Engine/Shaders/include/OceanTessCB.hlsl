// Constants of the tessellated FFT ocean, shared by the hull / domain (b0) and pixel (b4) stages. C++ mirror: OceanTessConstants
// in D3D11GraphicsEngine.cpp.
#ifndef GD_OCEAN_TESS_CB_HLSL
#define GD_OCEAN_TESS_CB_HLSL

struct OceanTessData
{
	matrix ViewProj;
	matrix View;
	float4 Camera;     // xyz camera position
	float4 Tile;       // cascade tile sizes in metres, 0 = unused
	float4 Params;     // x tile texels N, y world units per metre, z shore field valid (1), w unused
	float4 ShoreMap;   // xy field corner (world xz), zw 1 / extent
};

#endif
