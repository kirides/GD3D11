/** Atmospheric scattering header */
#ifndef ATMOSPHERIC_SCATTERING_H_
#define ATMOSPHERIC_SCATTERING_H_

static const float NIGHT_BRIGHTNESS = 2.0f;

cbuffer Atmosphere : register( b1 )
{
	float AC_Kr4PI;
	float AC_Km4PI;	
	float AC_g;
	float AC_KrESun;

	float AC_KmESun;
	float AC_InnerRadius;
	float AC_OuterRadius;
	float AC_Scale;

	float3 AC_Wavelength;
	float AC_RayleighScaleDepth;


	float AC_RayleighOverScaleDepth;
	int AC_nSamples;
	float AC_fSamples;
	float AC_CameraHeight;

	float3 AC_CameraPos;
	float AC_Time;
	float3 AC_LightPos;
	float AC_SceneWettness;

	float3 AC_SpherePosition;
	float AC_RainFXWeight;
};

// The scale equation calculated by Vernier's Graphical Analysis
float AC_Escale(float fCos)
{
	float x = 1.0 - fCos;
	return AC_RayleighScaleDepth * exp(-0.00287 + x*(0.459 + x*(3.83 + x*(-6.80 + x*5.25))));
}
// Calculates the Mie phase function
float AC_getMiePhase(float fCos, float fCos2, float g, float g2)
{
	return 1.5 * ((1.0 - g2) / (2.0 + g2)) * (1.0 + fCos2) / pow(abs(1.0 + g2 - 2.0*g*fCos), 1.5);
}
// Calculates the Rayleigh phase function
float AC_getRayleighPhase(float fCos2)
{
	//return 1.0;
	return 0.75 + 0.75*fCos2;
}
// Returns the near intersection point of a line and a sphere
float AC_getNearIntersection(float3 v3Pos, float3 v3Ray, float fDistance2, float fRadius2)
{
	float B = 2.0 * dot(v3Pos, v3Ray);
	float C = fDistance2 - fRadius2;
	float fDet = max(0.0, B*B - 4.0 * C);
	return 0.5 * (-B - sqrt(fDet));
}
// Returns the far intersection point of a line and a sphere
float AC_getFarIntersection(float3 v3Pos, float3 v3Ray, float fDistance2, float fRadius2)
{
	float B = 2.0 * dot(v3Pos, v3Ray);
	float C = fDistance2 - fRadius2;
	float fDet = max(0.0, B*B - 4.0 * C);
	return 0.5 * (-B + sqrt(fDet));
}

float3 GetAtmosphericSunTerm(float3 normal)
{
	return saturate(dot(normal, AC_LightPos));
}



float3 ApplyAtmosphericScatteringGround(float3 worldPosition, float3 in_color, bool applyNightshade=true)
{
	float3 camPos = AC_CameraPos;
	float3 v3Pos = worldPosition - AC_SpherePosition;
	float3 v3Ray = v3Pos - camPos;

	float nightWeight = saturate(((-AC_LightPos.y) + 0.2f) * 10.0f);
		
	float innerRadius = AC_InnerRadius;
				
	const int iSamples = 1;
	const int fSamples = iSamples;
				
	// Get the ray from the camera to the vertex, and its length (which is the far point of the ray passing through the atmosphere)
	float fFar = length(v3Ray);
	v3Ray /= fFar;

	//if(AC_CameraHeight > AC_OuterRadius)
	//	return in_color;
	
	// Calculate the ray's starting position, then calculate its scattering offset
	float3 v3Start = camPos;
	float fDepth = exp((innerRadius - AC_CameraHeight) / AC_RayleighScaleDepth);
	float fCameraAngle = max(1.0f, dot(-v3Ray, v3Pos) / length(v3Pos));
	float fLightAngle = dot(AC_LightPos, v3Pos) / length(v3Pos);
	float fCameraScale = AC_Escale(fCameraAngle);
	float fLightScale = AC_Escale(fLightAngle);
	float fCameraOffset = fDepth*fCameraScale;
	float fTemp = (fLightScale + fCameraScale);

	// Initialize the scattering loop variables
	float fSampleLength = fFar / fSamples;
	float fScaledLength = fSampleLength * AC_Scale;
	float3 v3SampleRay = v3Ray * fSampleLength;
	float3 v3SamplePoint = v3Start + v3SampleRay * 0.5;

	float3 vInvWavelength = 1.0f / pow(AC_Wavelength, 4.0f);
	
	// Now loop through the sample rays
	float3 v3FrontColor = float3(0.0, 0.0, 0.0);
	float3 v3Attenuate;
	for(int i=0; i<iSamples; i++)
	{
		float fHeight = length(v3SamplePoint);
		float fDepth = exp(AC_RayleighOverScaleDepth * (innerRadius - fHeight));
		float fScatter = fDepth*fTemp - fCameraOffset;
		v3Attenuate = exp(-fScatter * (vInvWavelength * AC_Kr4PI + AC_Km4PI));
		v3FrontColor += v3Attenuate * (fDepth * fScaledLength);
		v3SamplePoint += v3SampleRay;
	}
	
	// Shut off blue tint of geometry when raining
	v3FrontColor =  lerp(v3FrontColor, 0.0f, AC_SceneWettness);
	
	// Finally, scale the Mie and Rayleigh colors and set up the varying variables for the pixel shader
	float3 c0 = v3FrontColor * (vInvWavelength * AC_KrESun + AC_KmESun);
	//c0 = lerp(dot(float3(0.333f,0.333f,0.333f), c0), c0, 0.5f);
	float3 c1 = v3Attenuate;
	
	float3 dayColor = c0 + in_color * c1;
	float3 nightColor = float3(0.20,0.20,0.4) * NIGHT_BRIGHTNESS;
	nightColor = lerp(nightColor, float3(0.24,0.24,0.24) * NIGHT_BRIGHTNESS * 0.6f, AC_SceneWettness); // Grey fog when raining
	float3 outColor;

	if(applyNightshade)
		outColor = dayColor + in_color * nightColor * nightWeight;
	else
		outColor = dayColor + nightColor * nightWeight;
		
	return outColor;
}

float3 ApplyAtmosphericScatteringSky(float3 worldPosition)
{
	float3 camPos = AC_CameraPos;
	float3 vPos = (worldPosition) - AC_SpherePosition;
	float3 vRay = vPos - camPos;
				
	float fFar = length(vRay);
	vRay /= fFar;
	
	//return float4(abs(AC_SpherePosition), 1);
	
	//if(AC_CameraHeight < AC_InnerRadius)
	//	return float4(1,0,0,1);
	
	// Calculate the closest intersection of the ray with the outer atmosphere (which is the near point of the ray passing through the atmosphere)
	float fNear = AC_getNearIntersection(camPos, vRay, AC_CameraHeight * AC_CameraHeight, AC_OuterRadius * AC_OuterRadius);

	// Calculate the ray's starting position, then calculate its scattering offset
	float3 vStart = camPos;

	float fHeight = length(vStart);
	float fDepth = exp(AC_RayleighOverScaleDepth * (AC_InnerRadius - AC_CameraHeight));
	float fStartAngle = dot(vRay, vStart) / fHeight;
	float fStartOffset = fDepth*AC_Escale(fStartAngle);
	
	// Initialize the scattering loop variables
	float fSampleLength = fFar / AC_fSamples;
	float fScaledLength = fSampleLength * AC_Scale;
	float3 vSampleRay = vRay * fSampleLength;
	float3 vSamplePoint = vStart + vSampleRay * 0.5;
	
	float3 vInvWavelength = 1.0f / pow(AC_Wavelength, 4.0f);
	
	//return retF(AC_InnerRadius - length(vSamplePoint));
	
	// Now loop through the sample rays
	float3 vFrontColor = float3(0.0, 0.0, 0.0);
	for(int i=0; i<AC_nSamples; i++)
	{
		float fHeight = length(vSamplePoint);
		float fDepth = exp(AC_RayleighOverScaleDepth * (AC_InnerRadius - fHeight));
		float fLightAngle = dot(AC_LightPos, vSamplePoint) / fHeight;
		float fCameraAngle = dot(vRay, vSamplePoint) / fHeight;
		float fScatter = (fStartOffset + fDepth*(AC_Escale(fLightAngle) - AC_Escale(fCameraAngle)));
		
		float3 vAttenuate = exp(-fScatter * (vInvWavelength * AC_Kr4PI + AC_Km4PI));
		
		vFrontColor += vAttenuate * (fDepth * fScaledLength);
		vSamplePoint += vSampleRay;
	}
	
	float yCLip = saturate(1-pow(1-vPos.y, 20.0f));
	
	// Finally, scale the Mie and Rayleigh colors and set up the varying variables for the pixel shader
	float3 c0 = vFrontColor * (vInvWavelength * AC_KrESun);
	float3 c1 = vFrontColor * AC_KmESun;	
	
	
	
	float3 vDirection = camPos - vPos;
	
	float fCos = dot(AC_LightPos, vDirection) / length(vDirection);
	
	float fCos2 = fCos*fCos;

	float3 color = AC_getRayleighPhase(fCos2) * c0 + AC_getMiePhase(fCos, fCos2, AC_g, AC_g * AC_g) * c1 * 2.0f;
	
	return color;
}

float3 ApplyAtmosphericScatteringOuter(float3 worldPosition)
{
	float3 camPos = AC_CameraPos;
	float3 vPos = (worldPosition) - AC_SpherePosition;
	float3 vRay = vPos - camPos;
				
	float fFar = length(vRay);
	vRay /= fFar;
	
	//return float4(abs(AC_SpherePosition), 1);
	
	//if(AC_CameraHeight < AC_InnerRadius)
	//	return float4(1,0,0,1);
	
	// Calculate the closest intersection of the ray with the outer atmosphere (which is the near point of the ray passing through the atmosphere)
	float fNear = AC_getNearIntersection(camPos, vRay, AC_CameraHeight * AC_CameraHeight, AC_OuterRadius * AC_OuterRadius);

	// Calculate the ray's starting position, then calculate its scattering offset
	float3 vStart = camPos + vRay * fNear;
	fFar -= fNear;

	float fStartAngle = dot(vRay, vStart) / AC_OuterRadius;
	float fStartDepth = exp(-1.0 / AC_RayleighScaleDepth);
	float fStartOffset = fStartDepth*AC_Escale(fStartAngle);
	
	// Initialize the scattering loop variables
	float fSampleLength = fFar / AC_fSamples;
	float fScaledLength = fSampleLength * AC_Scale;
	float3 vSampleRay = vRay * fSampleLength;
	float3 vSamplePoint = vStart + vSampleRay * 0.5;
	
	float3 vInvWavelength = 1.0f / pow(AC_Wavelength, 4.0f);
	
	//return retF(AC_InnerRadius - length(vSamplePoint));
	
	// Now loop through the sample rays
	float3 vFrontColor = float3(0.0, 0.0, 0.0);
	for(int i=0; i<AC_nSamples; i++)
	{
		float fHeight = length(vSamplePoint);
		float fDepth = exp(AC_RayleighOverScaleDepth * (AC_InnerRadius - fHeight));
		float fLightAngle = dot(AC_LightPos, vSamplePoint) / fHeight;
		float fCameraAngle = dot(vRay, vSamplePoint) / fHeight;
		float fScatter = (fStartOffset + fDepth*(AC_Escale(fLightAngle) - AC_Escale(fCameraAngle)));
		
		float3 vAttenuate = exp(-fScatter * (vInvWavelength * AC_Kr4PI + AC_Km4PI));
		
		vFrontColor += vAttenuate * (fDepth * fScaledLength);
		vSamplePoint += vSampleRay;
	}
	
	// Finally, scale the Mie and Rayleigh colors and set up the varying variables for the pixel shader
	float3 c0 = vFrontColor * (vInvWavelength * AC_KrESun) * 2.0f;
	
	float3 c1 = vFrontColor * AC_KmESun;	
	float3 vDirection = camPos - vPos;
	
	float fCos = dot(AC_LightPos, vDirection) / length(vDirection);
	
	float fCos2 = fCos*fCos;

	float3 color = AC_getRayleighPhase(fCos2) * c0 + AC_getMiePhase(fCos, fCos2, AC_g, AC_g * AC_g)* c1;
	
	return color;
}

#endif