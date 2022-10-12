#pragma once
#include <Windows.h>
#include <intrin.h>

static void Convert555to8888(unsigned char* dst, unsigned char* src, UINT realDataSize)
{
	for(UINT i = 0; i < realDataSize / 4; ++i)
	{
		unsigned char temp0 = src[2 * i + 0];
		unsigned char temp1 = src[2 * i + 1];
		UINT pixel_data = temp1 << 8 | temp0;

		unsigned char blueComponent = (pixel_data & 31) << 3;
		unsigned char greenComponent = ((pixel_data >> 5) & 31) << 3;
		unsigned char redComponent = ((pixel_data >> 10) & 31) << 3;

		dst[4 * i + 2] = redComponent;
		dst[4 * i + 1] = greenComponent;
		dst[4 * i + 0] = blueComponent;
		dst[4 * i + 3] = 255;
	}
}

static void Convert565to8888(unsigned char* dst, unsigned char* src, UINT realDataSize)
{
	for(UINT i = 0; i < realDataSize / 4; ++i)
	{
		unsigned char temp0 = src[2 * i + 0];
		unsigned char temp1 = src[2 * i + 1];
		UINT pixel_data = temp1 << 8 | temp0;

		unsigned char redComponent = (pixel_data & 31) << 3;
		unsigned char greenComponent = ((pixel_data >> 5) & 63) << 2;
		unsigned char blueComponent = ((pixel_data >> 11) & 31) << 3;

		dst[4 * i + 2] = redComponent;
		dst[4 * i + 1] = greenComponent;
		dst[4 * i + 0] = blueComponent;
		dst[4 * i + 3] = 255;
	}
}

static void Convert1555to8888(unsigned char* dst, unsigned char* src, UINT realDataSize)
{
	for(UINT i = 0; i < realDataSize / 4; ++i)
	{
		unsigned char temp0 = src[2 * i + 0];
		unsigned char temp1 = src[2 * i + 1];
		UINT pixel_data = temp1 << 8 | temp0;

		unsigned char redComponent = (pixel_data & 31) << 3;
		unsigned char greenComponent = ((pixel_data >> 5) & 31) << 3;
		unsigned char blueComponent = ((pixel_data >> 10) & 31) << 3;
		unsigned char alphaComponent = (pixel_data >> 15) * 0xFF;

		dst[4 * i + 2] = redComponent;
		dst[4 * i + 1] = greenComponent;
		dst[4 * i + 0] = blueComponent;
		dst[4 * i + 3] = alphaComponent;
	}
}

static void Convert4444to8888(unsigned char* dst, unsigned char* src, UINT realDataSize)
{
	for(UINT i = 0; i < realDataSize / 4; ++i)
	{
		unsigned char temp0 = src[2 * i + 0];
		unsigned char temp1 = src[2 * i + 1];
		UINT pixel_data = temp1 << 8 | temp0;

		unsigned char redComponent = (pixel_data & 15) << 4;
		unsigned char greenComponent = ((pixel_data >> 4) & 15) << 4;
		unsigned char blueComponent = ((pixel_data >> 8) & 15) << 4;
		unsigned char alphaComponent = ((pixel_data >> 12) & 15) << 4;

		dst[4 * i + 2] = redComponent;
		dst[4 * i + 1] = greenComponent;
		dst[4 * i + 0] = blueComponent;
		dst[4 * i + 3] = alphaComponent;
	}
}

static void ConvertRGBAtoBGRA(unsigned char* dst, unsigned char* src, UINT realDataSize)
{
	__m128i mask = _mm_setr_epi8(-1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0);
	INT textureDataSize = static_cast<INT>(realDataSize) - 32;
	INT i = 0;
	for(; i <= textureDataSize; i += 32)
	{
		__m128i data0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&src[i]));
		__m128i data1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&src[i + 16]));
		__m128i gaComponents0 = _mm_andnot_si128(mask, data0);
		__m128i brComponents0 = _mm_and_si128(data0, mask);
		__m128i gaComponents1 = _mm_andnot_si128(mask, data1);
		__m128i brComponents1 = _mm_and_si128(data1, mask);
		__m128i brSwapped0 = _mm_shufflehi_epi16(_mm_shufflelo_epi16(brComponents0, _MM_SHUFFLE(2, 3, 0, 1)), _MM_SHUFFLE(2, 3, 0, 1));
		__m128i brSwapped1 = _mm_shufflehi_epi16(_mm_shufflelo_epi16(brComponents1, _MM_SHUFFLE(2, 3, 0, 1)), _MM_SHUFFLE(2, 3, 0, 1));
		_mm_storeu_si128(reinterpret_cast<__m128i*>(&dst[i]), _mm_or_si128(gaComponents0, brSwapped0));
		_mm_storeu_si128(reinterpret_cast<__m128i*>(&dst[i + 16]), _mm_or_si128(gaComponents1, brSwapped1));
	}
	textureDataSize += 32;
	for(; i < textureDataSize; i += 4)
	{
		unsigned char R = src[i + 0];
		unsigned char G = src[i + 1];
		unsigned char B = src[i + 2];
		unsigned char A = src[i + 2];
		dst[i + 0] = B;
		dst[i + 1] = G;
		dst[i + 2] = R;
		dst[i + 3] = A;
	}
}
