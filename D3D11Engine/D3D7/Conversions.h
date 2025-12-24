#pragma once

static void Convert565to8888(unsigned char* dst, unsigned char* src, unsigned int realDataSize)
{
	for(unsigned int i = 0; i < realDataSize / 4; ++i)
	{
		unsigned char temp0 = src[2 * i + 0];
		unsigned char temp1 = src[2 * i + 1];
		unsigned int pixel_data = temp1 << 8 | temp0;

		unsigned char redComponent = (pixel_data & 31) << 3;
		unsigned char greenComponent = ((pixel_data >> 5) & 63) << 2;
		unsigned char blueComponent = ((pixel_data >> 11) & 31) << 3;

		dst[4 * i + 2] = blueComponent;
		dst[4 * i + 1] = greenComponent;
		dst[4 * i + 0] = redComponent;
		dst[4 * i + 3] = 255;
	}
}

static void Convert1555to8888(unsigned char* dst, unsigned char* src, unsigned int realDataSize)
{
	for(unsigned int i = 0; i < realDataSize / 4; ++i)
	{
		unsigned char temp0 = src[2 * i + 0];
		unsigned char temp1 = src[2 * i + 1];
		unsigned int pixel_data = temp1 << 8 | temp0;

		unsigned char redComponent = (pixel_data & 31) << 3;
		unsigned char greenComponent = ((pixel_data >> 5) & 31) << 3;
		unsigned char blueComponent = ((pixel_data >> 10) & 31) << 3;
		unsigned char alphaComponent = (pixel_data >> 15) * 0xFF;

		dst[4 * i + 2] = blueComponent;
		dst[4 * i + 1] = greenComponent;
		dst[4 * i + 0] = redComponent;
		dst[4 * i + 3] = alphaComponent;
	}
}

static void Convert4444to8888(unsigned char* dst, unsigned char* src, unsigned int realDataSize)
{
	for(unsigned int i = 0; i < realDataSize / 4; ++i)
	{
		unsigned char temp0 = src[2 * i + 0];
		unsigned char temp1 = src[2 * i + 1];
		unsigned int pixel_data = temp1 << 8 | temp0;

		unsigned char redComponent = (pixel_data & 15) << 4;
		unsigned char greenComponent = ((pixel_data >> 4) & 15) << 4;
		unsigned char blueComponent = ((pixel_data >> 8) & 15) << 4;
		unsigned char alphaComponent = ((pixel_data >> 12) & 15) << 4;

		dst[4 * i + 2] = blueComponent;
		dst[4 * i + 1] = greenComponent;
		dst[4 * i + 0] = redComponent;
		dst[4 * i + 3] = alphaComponent;
	}
}
