/* SPDX-License-Identifier: MIT */
#include "decode.h"

static const BYTE bmp_pattern[] = {'B', 'M'};
static const BYTE png_pattern[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
static const BYTE gif_pattern[] = {'G', 'I', 'F', '8'};
static const BYTE ico_pattern[] = {0, 0, 1, 0};

static const BYTE jpeg_pattern[] = {0xff, 0xd8, 0xff};
static const BYTE tiff_pattern[] = {'I', 'I', '*', 0};
static const BYTE tiff_other_pattern[] = {'M', 'M', 0, '*'};

static const wic_codec codecs[] = {
	{&GUID_ContainerFormatBmp, &CLSID_WICBmpDecoder, &CLSID_WICBmpEncoder, L"BMP Decoder", L"BMP Encoder", L".bmp,.dib,.rle", L"image/bmp",
		bmp_pattern, sizeof(bmp_pattern), parse_bmp},
	{&GUID_ContainerFormatPng, &CLSID_WICPngDecoder, &CLSID_WICPngEncoder, L"PNG Decoder", L"PNG Encoder", L".png", L"image/png",
		png_pattern, sizeof(png_pattern), parse_png},
	{&GUID_ContainerFormatIco, &CLSID_WICIcoDecoder, NULL, L"ICO Decoder", NULL, L".ico,.icon", L"image/ico,image/x-icon",
		ico_pattern, sizeof(ico_pattern), parse_ico},
	{&GUID_ContainerFormatGif, &CLSID_WICGifDecoder, &CLSID_WICGifEncoder, L"GIF Decoder", L"GIF Encoder", L".gif", L"image/gif",
		gif_pattern, sizeof(gif_pattern), parse_gif},
	{&GUID_ContainerFormatJpeg, &CLSID_WICJpegDecoder, &CLSID_WICJpegEncoder, L"JPEG Decoder", L"JPEG Encoder", L".jpeg,.jpe,.jpg,.jfif,.exif", L"image/jpeg,image/jpe,image/jpg",
		jpeg_pattern, sizeof(jpeg_pattern), parse_jpeg},
	{&GUID_ContainerFormatTiff, &CLSID_WICTiffDecoder, &CLSID_WICTiffEncoder, L"TIFF Decoder", L"TIFF Encoder", L".tiff,.tif", L"image/tiff,image/tif",
		tiff_pattern, sizeof(tiff_pattern), parse_tiff},
};

const wic_codec *wic_codec_at(UINT index)
{
	return index < ARRAYSIZE(codecs) ? &codecs[index] : NULL;
}

const wic_codec *wic_codec_by_container(const GUID *container)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(codecs); index++)
		if (IsEqualGUID(container, codecs[index].container))
			return &codecs[index];
	return NULL;
}

const wic_codec *wic_codec_by_class(const GUID *clsid, BOOL *is_encoder)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(codecs); index++) {
		const BOOL encoder = codecs[index].encoder_class && IsEqualGUID(clsid, codecs[index].encoder_class);
		if (encoder || IsEqualGUID(clsid, codecs[index].decoder_class)) {
			if (is_encoder)
				*is_encoder = encoder;
			return &codecs[index];
		}
	}
	return NULL;
}

BOOL wic_codec_matches(const wic_codec *codec, const BYTE *data, SIZE_T size)
{
	if (codec->pattern == tiff_pattern && size >= sizeof(tiff_other_pattern) && !memcmp(data, tiff_other_pattern, sizeof(tiff_other_pattern)))
		return TRUE;
	return size >= codec->pattern_size && !memcmp(data, codec->pattern, codec->pattern_size);
}
