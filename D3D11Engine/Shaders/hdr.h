#ifndef _HDR_H
#define _HDR_H

//static const float3 LUM_CONVERT = float3(0.299f, 0.587f, 0.114f); 
//static const float3 LUM_CONVERT  = float3(0.2125f, 0.7154f, 0.0721f);
static const float3 LUM_CONVERT  = float3(0.333f, 0.333f, 0.333f);

cbuffer HDR_Settings : register(b0)
{
	float HDR_MiddleGray;
	float HDR_LumWhite;
	float HDR_Threshold;
	float HDR_BloomStrength;
};

float3 ToneMap_Reinhard(float3 vColor, Texture2D lumTex, SamplerState samplerState)
{
	// Get the calculated average luminance
	float fLumAvg = lumTex.SampleLevel(samplerState, float2(0.5f, 0.5f), 9).r;

	// Calculate the luminance of the current pixel
	float fLumPixel = dot(vColor, LUM_CONVERT); //?.
	
	vColor.rgb *= (HDR_MiddleGray / (fLumAvg.r + 0.001f));
   	 vColor.rgb *= (1.0f + vColor/HDR_LumWhite);
  	  vColor.rgb /= (1.0f + vColor);

	//return vColor;
	//return pow(vColor, 1 / 2.2f);
	return pow( vColor, 2.2f );
} 

float3 ToneMap_jafEq4(float3 vColor, Texture2D lumTex, SamplerState samplerState)
{
	// Get the calculated average luminance
	float fLumAvg = lumTex.SampleLevel(samplerState, float2(0.5f, 0.5f), 9).r;

	// Calculate the luminance of the current pixel
	float fLumPixel = dot(vColor, LUM_CONVERT);
	
	// Apply the modified operator (Eq. 4)
	float fLumScaled = (fLumPixel * HDR_MiddleGray) / fLumAvg;
	float fLumCompressed = (fLumScaled * (1 + (fLumScaled / (HDR_LumWhite * HDR_LumWhite)))) / (1 + fLumScaled);
	return pow(max(0.0f, fLumCompressed * vColor), 1/2.2f);
	//return pow( vColor, 2.2f );
} 

float3 Uncharted2TonemapOperator(float3 x)
{
	float A = 0.22; // shoulder strength
	float B = 0.3;	// linear strength
	float C = 0.10;	// linear angle
	float D = 0.20;	// toe strength
	float E = 0.01;	// toe numerator
	float F = 0.30;	// toe denominator
	float W = 11.2;	// linear white point   

	return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}

float3 Uncharted2Tonemap(float3 vColor, Texture2D lumTex, SamplerState samplerState) : COLOR
{
	float fLumAvg = lumTex.SampleLevel(samplerState, float2(0.5f, 0.5f), 9).r;
	
	vColor *= (HDR_MiddleGray / fLumAvg);  // Exposure Adjustment

	float ExposureBias = 2.0f;
	float3 curr = Uncharted2TonemapOperator(ExposureBias*vColor);	
	
	float3 whiteScale = 1.0f/Uncharted2TonemapOperator(HDR_LumWhite);
	float3 color = curr*whiteScale;

	float3 retColor = color;//pow(color,1/2.2f);
	return retColor;
}

// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
float3 ACESFilmTonemapOperator(float3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    
	return saturate( ( x * ( a * x + b ) ) / ( x * ( c * x + d ) + e ) );
}

float3 ACESFilmTonemap(float3 vColor, Texture2D lumTex, SamplerState samplerState) : COLOR // needs adaptation
{
	float3 LUM_CONVERT  = float3(0.2125f, 0.7154f, 0.0721f);
	
	float fLumAvg = lumTex.SampleLevel(samplerState, float2(0.5f, 0.5f), 9).r;
	
	// Calculate the luminance of the current pixel
	float fLumPixel = dot(vColor, LUM_CONVERT);
	
	// Apply the modified operator (Eq. 4)
	float fLumScaled = (fLumPixel * HDR_MiddleGray) / fLumAvg;	

	vColor *= HDR_MiddleGray / fLumAvg;  // Exposure Adjustment	

	float3 curr = ACESFilmTonemapOperator(fLumScaled*vColor);	
	
	float3 whiteScale = ACESFilmTonemapOperator(HDR_LumWhite);
	float3 color = curr*whiteScale;

	float3 retColor = color;//pow(color,1/2.2f);
	return retColor;
}

float3 PerceptualQuantizerTonemapOperator(float3 x) //broken with gray color
{
	float m1 = 0.1593017578125;
	float m2 = 78.84375;
	float c1 = 0.8359375;
	float c2 = 18.8515625;
	float c3 = 18.6875;
	float p = pow(x, m1);

	return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

float3 PerceptualQuantizerTonemap(float3 vColor, Texture2D lumTex, SamplerState samplerState) : COLOR //broken with gray color
{
	float fLumAvg = lumTex.SampleLevel(samplerState, float2(0.5f, 0.5f), 9).r;
	
	vColor *= (HDR_MiddleGray / fLumAvg);  // Exposure Adjustment

	float3 curr = PerceptualQuantizerTonemapOperator(vColor);	
	
	float3 whiteScale = 1.0f/PerceptualQuantizerTonemapOperator(HDR_LumWhite);
	float3 color = curr*whiteScale;

	float3 retColor = color;//pow(color,1/2.2f);
	return retColor;
}

float3 ToneMap_Simple(float3 vColor, Texture2D lumTex, SamplerState samplerState)
{
	// Get the calculated average luminance
	float fLumAvg = lumTex.SampleLevel(samplerState, float2(0.5f, 0.5f), 9).r;
	
	vColor *= HDR_MiddleGray/(fLumAvg + 0.001f);
	vColor /= (1.0f + vColor);
	
	return vColor;
}

// The code in this file was originally written by Stephen Hill (@self_shadow), who deserves all
// credit for coming up with this fit and implementing it. Buy him a beer next time you see him. :)

// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
static const float3x3 ACESInputMat =
{
    {0.59719, 0.35458, 0.04823},
    {0.07600, 0.90834, 0.01566},
    {0.02840, 0.13383, 0.83777}
};

// ODT_SAT => XYZ => D60_2_D65 => sRGB
static const float3x3 ACESOutputMat =
{
    { 1.60475, -0.53108, -0.07367},
    {-0.10208,  1.10813, -0.00605},
    {-0.00327, -0.07276,  1.07602}
};

float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

float3 ACESFittedTonemapOperator(float3 color)
{
    color = mul(ACESInputMat, color);

    // Apply RRT and ODT
    color = RRTAndODTFit(color);

    color = mul(ACESOutputMat, color);

    // Clamp to [0, 1]
    color = saturate(color);

    return color;
}

float3 ACESFittedTonemap(float3 vColor, Texture2D lumTex, SamplerState samplerState) : COLOR
{	
	float fLumAvg = lumTex.SampleLevel(samplerState, float2(0.5f, 0.5f), 9).r;
	
	vColor *= (HDR_MiddleGray / fLumAvg);  // Exposure Adjustment

	float3 curr = ACESFittedTonemapOperator(vColor);	
	
	float3 whiteScale = ACESFittedTonemapOperator(HDR_LumWhite);
	float3 color = curr*whiteScale;

	float3 retColor = color;//pow(color,1/2.2f);
	return retColor;
}

// AgX (Troy Sobotka's display transform, polynomial contrast fit as in three.js): Rec.2020 inset, log2 encode over
// [-12.47, 4.03] EV, sigmoid, outset. Hue-stable highlights, no ACES-style orange/red skew.
static const float3x3 AgXToRec2020 = { { 0.6274, 0.3293, 0.0433 }, { 0.0691, 0.9195, 0.0113 }, { 0.0164, 0.0880, 0.8956 } };
static const float3x3 AgXFromRec2020 = { { 1.6605, -0.5876, -0.0728 }, { -0.1246, 1.1329, -0.0083 }, { -0.0182, -0.1006, 1.1187 } };
static const float3x3 AgXInsetMat = { { 0.856627153315983, 0.0951212405381588, 0.0482516061458583 }, { 0.137318972929847, 0.761241990602591, 0.101439036467562 }, { 0.11189821299995, 0.0767994186031903, 0.811302368396859 } };
static const float3x3 AgXOutsetMat = { { 1.1271005818144368, -0.11060664309660323, -0.016493938717834573 }, { -0.1413297634984383, 1.157823702216272, -0.016493938717834257 }, { -0.14132976349843826, -0.11060664309660294, 1.2519364065950405 } };

float3 AgXContrastApprox(float3 x)
{
	float3 x2 = x * x;
	float3 x4 = x2 * x2;
	return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// Takes exposed linear sRGB, returns LINEAR sRGB in [0,1].
float3 AgXTonemapOperator(float3 color)
{
	const float minEv = -12.47393;
	const float maxEv = 4.026069;

	color = mul(AgXInsetMat, mul(AgXToRec2020, max(color, 0.0f)));
	color = log2(max(color, 1e-10f));
	color = saturate((color - minEv) / (maxEv - minEv));
	color = AgXContrastApprox(color);
	color = mul(AgXOutsetMat, color);
	color = pow(max(color, 0.0f), 2.2f);
	return saturate(mul(AgXFromRec2020, color));
}

// Returns display-encoded (gamma 1/2.2) like jafEq4; PS_PFX_HDR re-linearizes with pow(2.2).
float3 AgXTonemap(float3 vColor, Texture2D lumTex, SamplerState samplerState)
{
	float fLumAvg = lumTex.SampleLevel(samplerState, float2(0.5f, 0.5f), 9).r;

	vColor *= (HDR_MiddleGray / fLumAvg);  // Exposure Adjustment

	return pow(AgXTonemapOperator(vColor), 1 / 2.2f);
}

#endif