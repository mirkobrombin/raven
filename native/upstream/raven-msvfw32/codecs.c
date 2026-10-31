/* SPDX-License-Identifier: MIT */
#include "vf.h"
#include <wchar.h>

/* The codec manager. A codec is a driver procedure, either handed in by the program or the DriverProc of a DLL named in the
 * registry or in system.ini under a key made of its type and handler; the manager opens it and passes messages along. Which
 * codecs exist is up to the prefix: none is built in. */

typedef LRESULT (CALLBACK *driver_procedure)(DWORD_PTR, HDRVR, UINT, LPARAM, LPARAM);

typedef struct installed
{
	struct installed *next;
	DWORD type;
	DWORD handler;
	driver_procedure procedure;
} installed;

typedef struct opened
{
	struct opened *next;
	HIC handle;
	DWORD type;
	DWORD handler;
	HMODULE module;
	driver_procedure procedure;
	DWORD_PTR driver;
} opened;

static installed *functions;
static opened *open_codecs;
static UINT next_handle = 0x1000;

void *vf_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
}

void vf_free(void *block)
{
	HeapFree(GetProcessHeap(), 0, block);
}

static DWORD lower_fourcc(DWORD code)
{
	DWORD result = 0;
	int shift;
	for (shift = 0; shift < 32; shift += 8)
	{
		BYTE letter = (BYTE)(code >> shift);
		if (letter >= 'A' && letter <= 'Z')
			letter += 32;
		result |= (DWORD)letter << shift;
	}
	return result;
}

static void fourcc_name(DWORD type, DWORD handler, char *name)
{
	memcpy(name, &type, 4);
	name[4] = '.';
	memcpy(name + 5, &handler, 4);
	name[9] = 0;
}

static opened *find_open(HIC handle)
{
	opened *entry;
	for (entry = open_codecs; entry; entry = entry->next)
		if (entry->handle == handle)
			return entry;
	return NULL;
}

/* The DLL of a codec key, from the registry first and from system.ini otherwise. */
static BOOL driver_of_key(const char *key, char *path, DWORD room)
{
	HKEY drivers;
	if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", 0, KEY_READ, &drivers))
	{
		DWORD size = room;
		LONG status = RegQueryValueExA(drivers, key, NULL, NULL, (BYTE *)path, &size);
		RegCloseKey(drivers);
		if (!status)
			return TRUE;
	}
	return GetPrivateProfileStringA("drivers32", key, "", path, room, "system.ini") > 0;
}

static opened *open_procedure(DWORD type, DWORD handler, UINT mode, driver_procedure procedure, HMODULE module)
{
	ICOPEN parameters;
	opened *entry = vf_alloc(sizeof(*entry));
	if (!entry)
		return NULL;
	memset(&parameters, 0, sizeof(parameters));
	parameters.dwSize = sizeof(parameters);
	parameters.fccType = type;
	parameters.fccHandler = handler;
	parameters.dwVersion = ICVERSION;
	parameters.dwFlags = mode;
	entry->handle = (HIC)(ULONG_PTR)next_handle++;
	entry->type = type;
	entry->handler = handler;
	entry->module = module;
	entry->procedure = procedure;
	procedure(0, (HDRVR)entry->handle, DRV_LOAD, 0, 0);
	procedure(0, (HDRVR)entry->handle, DRV_ENABLE, 0, 0);
	entry->driver = procedure(0, (HDRVR)entry->handle, DRV_OPEN, 0, (LPARAM)&parameters);
	if (!entry->driver)
	{
		procedure(0, (HDRVR)entry->handle, DRV_DISABLE, 0, 0);
		procedure(0, (HDRVR)entry->handle, DRV_FREE, 0, 0);
		vf_free(entry);
		return NULL;
	}
	entry->next = open_codecs;
	open_codecs = entry;
	return entry;
}

HIC WINAPI ICOpenFunction(DWORD type, DWORD handler, UINT mode, FARPROC procedure)
{
	opened *entry = open_procedure(type, handler, mode, (driver_procedure)procedure, NULL);
	return entry ? entry->handle : NULL;
}

HIC WINAPI ICOpen(DWORD type, DWORD handler, UINT mode)
{
	installed *function;
	opened *entry;
	char key[10], path[MAX_PATH];
	HMODULE module;
	driver_procedure procedure;
	type = lower_fourcc(type);
	for (function = functions; function; function = function->next)
		if (function->type == type && function->handler == lower_fourcc(handler))
		{
			entry = open_procedure(type, handler, mode, function->procedure, NULL);
			return entry ? entry->handle : NULL;
		}
	fourcc_name(type, lower_fourcc(handler), key);
	if (!driver_of_key(key, path, sizeof(path)) || !(module = LoadLibraryA(path)))
		return NULL;
	if (!(procedure = (driver_procedure)GetProcAddress(module, "DriverProc")))
	{
		FreeLibrary(module);
		return NULL;
	}
	entry = open_procedure(type, handler, mode, procedure, module);
	if (!entry)
	{
		FreeLibrary(module);
		return NULL;
	}
	return entry->handle;
}

LRESULT WINAPI ICClose(HIC handle)
{
	opened **link;
	for (link = &open_codecs; *link; link = &(*link)->next)
		if ((*link)->handle == handle)
		{
			opened *entry = *link;
			*link = entry->next;
			entry->procedure(entry->driver, (HDRVR)handle, DRV_CLOSE, 0, 0);
			entry->procedure(0, (HDRVR)handle, DRV_DISABLE, 0, 0);
			entry->procedure(0, (HDRVR)handle, DRV_FREE, 0, 0);
			if (entry->module)
				FreeLibrary(entry->module);
			vf_free(entry);
			return ICERR_OK;
		}
	return ICERR_BADHANDLE;
}

LRESULT WINAPI ICSendMessage(HIC handle, UINT message, DWORD_PTR first, DWORD_PTR second)
{
	opened *entry = find_open(handle);
	if (!entry)
		return ICERR_BADHANDLE;
	return entry->procedure(entry->driver, (HDRVR)handle, message, first, second);
}

LRESULT WINAPI ICGetInfo(HIC handle, ICINFO *info, DWORD size)
{
	if (!info || size < sizeof(*info))
		return 0;
	memset(info, 0, sizeof(*info));
	info->dwSize = sizeof(*info);
	info->dwVersionICM = ICVERSION;
	return ICSendMessage(handle, ICM_GETINFO, (DWORD_PTR)info, size);
}

static void blank_info(ICINFO *info, DWORD type, DWORD handler)
{
	memset(info, 0, sizeof(*info));
	info->dwSize = sizeof(*info);
	info->fccType = type;
	info->fccHandler = handler;
	info->dwVersionICM = ICVERSION;
}

static BOOL function_info(installed *function, ICINFO *info)
{
	blank_info(info, 0, 0);
	return function->procedure(0, 0, ICM_GETINFO, (LPARAM)info, sizeof(*info)) != 0;
}

static BOOL key_info(const char *key, const char *path, DWORD type, ICINFO *info)
{
	DWORD found;
	memcpy(&found, key + 5, 4);
	blank_info(info, type, found);
	MultiByteToWideChar(CP_ACP, 0, path, -1, info->szDriver, ARRAYSIZE(info->szDriver));
	return TRUE;
}

/* The next codec of the registry with this type, counting down the index. */
static BOOL enumerate_registry(DWORD type, DWORD *index, ICINFO *info)
{
	HKEY drivers;
	DWORD position = 0;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", 0, KEY_READ, &drivers))
		return FALSE;
	for (;; position++)
	{
		char key[64], path[MAX_PATH];
		DWORD key_size = sizeof(key), path_size = sizeof(path), value_type, found;
		if (RegEnumValueA(drivers, position, key, &key_size, NULL, &value_type, (BYTE *)path, &path_size))
			break;
		if (key_size != 9 || key[4] != '.')
			continue;
		memcpy(&found, key, 4);
		if (lower_fourcc(found) == 0x6e697700 || (type && lower_fourcc(type) != lower_fourcc(found)))
			continue;
		if ((*index)-- == 0)
		{
			key_info(key, path, found, info);
			RegCloseKey(drivers);
			return TRUE;
		}
	}
	RegCloseKey(drivers);
	return FALSE;
}

BOOL WINAPI ICInfo(DWORD type, DWORD handler, ICINFO *info)
{
	installed *function;
	if (!info)
		return FALSE;
	if (handler < 256)
	{
		DWORD index = handler;
		for (function = functions; function; function = function->next)
			if ((!type || function->type == lower_fourcc(type)) && !index--)
				return function_info(function, info);
		blank_info(info, type, 0);
		return enumerate_registry(type, &index, info);
	}
	for (function = functions; function; function = function->next)
		if (function->type == lower_fourcc(type) && function->handler == lower_fourcc(handler))
			return function_info(function, info);
	{
		char key[10], path[MAX_PATH];
		fourcc_name(lower_fourcc(type), lower_fourcc(handler), key);
		if (driver_of_key(key, path, sizeof(path)))
		{
			key_info(key, path, type, info);
			info->fccHandler = handler;
			return TRUE;
		}
	}
	blank_info(info, type, handler);
	return FALSE;
}

BOOL WINAPI ICInstall(DWORD type, DWORD handler, LPARAM parameter, char *description, UINT flags)
{
	type = lower_fourcc(type);
	handler = lower_fourcc(handler);
	if (flags == ICINSTALL_FUNCTION)
	{
		installed *entry;
		for (entry = functions; entry; entry = entry->next)
			if (entry->type == type && entry->handler == handler)
				return FALSE;
		if (!(entry = vf_alloc(sizeof(*entry))))
			return FALSE;
		entry->type = type;
		entry->handler = handler;
		entry->procedure = (driver_procedure)parameter;
		entry->next = functions;
		functions = entry;
		return TRUE;
	}
	if (flags == ICINSTALL_DRIVER)
	{
		HKEY drivers;
		char key[10];
		LONG status;
		if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", 0, NULL, 0, KEY_WRITE, NULL, &drivers, NULL))
			return FALSE;
		fourcc_name(type, handler, key);
		status = RegSetValueExA(drivers, key, 0, REG_SZ, (const BYTE *)parameter, lstrlenA((const char *)parameter) + 1);
		RegCloseKey(drivers);
		return !status;
	}
	return FALSE;
}

BOOL WINAPI ICRemove(DWORD type, DWORD handler, UINT flags)
{
	installed **link;
	HKEY drivers;
	char key[10];
	type = lower_fourcc(type);
	handler = lower_fourcc(handler);
	for (link = &functions; *link; link = &(*link)->next)
		if ((*link)->type == type && (*link)->handler == handler)
		{
			installed *entry = *link;
			*link = entry->next;
			vf_free(entry);
			return TRUE;
		}
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", 0, KEY_WRITE, &drivers))
		return FALSE;
	fourcc_name(type, handler, key);
	RegDeleteValueA(drivers, key);
	RegCloseKey(drivers);
	return TRUE;
}

static LRESULT query_for(HIC handle, UINT mode, BITMAPINFOHEADER *in, BITMAPINFOHEADER *out)
{
	switch (mode)
	{
	case ICMODE_COMPRESS:
	case ICMODE_FASTCOMPRESS:
		return ICSendMessage(handle, ICM_COMPRESS_QUERY, (DWORD_PTR)in, (DWORD_PTR)out);
	case ICMODE_DECOMPRESS:
	case ICMODE_FASTDECOMPRESS:
		return ICSendMessage(handle, ICM_DECOMPRESS_QUERY, (DWORD_PTR)in, (DWORD_PTR)out);
	case ICMODE_DRAW:
		return ICSendMessage(handle, ICM_DRAW_QUERY, (DWORD_PTR)in, (DWORD_PTR)out);
	default:
		return ICERR_BADFORMAT;
	}
}

static HIC try_codec(DWORD type, DWORD handler, UINT mode, BITMAPINFOHEADER *in, BITMAPINFOHEADER *out)
{
	HIC handle = ICOpen(type, handler, mode);
	if (!handle)
		return NULL;
	if (query_for(handle, mode, in, out) == ICERR_OK)
		return handle;
	ICClose(handle);
	return NULL;
}

/* A named handler, the handler the data names, and then every codec of the type. */
HIC WINAPI ICLocate(DWORD type, DWORD handler, BITMAPINFOHEADER *in, BITMAPINFOHEADER *out, WORD mode)
{
	ICINFO info;
	HIC handle;
	DWORD index;
	if (!in)
		return NULL;
	if (handler && (handle = try_codec(type, handler, mode, in, out)))
		return handle;
	if (in->biCompression > 255 && in->biCompression != handler && (handle = try_codec(type, in->biCompression, mode, in, out)))
		return handle;
	for (index = 0; ICInfo(type, index, &info); index++)
		if (info.fccHandler && info.fccHandler != handler && (handle = try_codec(type, info.fccHandler, mode, in, out)))
			return handle;
	return NULL;
}

static DWORD image_size(LONG width, LONG height, WORD depth)
{
	return ((width * depth + 31) >> 3 & ~3U) * (DWORD)(height < 0 ? -height : height);
}

static BOOL display_format_fits(HIC handle, BITMAPINFOHEADER *in, BITMAPINFOHEADER *out, LONG width, LONG height, WORD depth, DWORD compression)
{
	out->biWidth = width;
	out->biHeight = height;
	out->biBitCount = depth;
	out->biCompression = compression;
	out->biSizeImage = image_size(width, height, depth);
	return ICDecompressQuery(handle, in, out) == ICERR_OK;
}

/* Tries the wanted depth and then the depths a display offers, at the wanted size and then at the source's. */
HIC WINAPI ICGetDisplayFormat(HIC handle, BITMAPINFOHEADER *in, BITMAPINFOHEADER *out, int depth, int width, int height)
{
	static const struct
	{
		WORD depth;
		DWORD compression;
	} formats[] = {
		{16, BI_RGB},
		{16, BI_BITFIELDS},
		{24, BI_RGB},
		{32, BI_RGB},
	};
	BOOL owned = FALSE;
	UINT index;
	if (width <= 0 || height <= 0)
	{
		width = in->biWidth;
		height = in->biHeight;
	}
	if (!depth)
		depth = 32;
	if (!handle)
	{
		if (!(handle = ICLocate(ICTYPE_VIDEO, 0, in, NULL, ICMODE_DECOMPRESS)))
			return NULL;
		owned = TRUE;
	}
	if (ICDecompressQuery(handle, in, NULL) != ICERR_OK)
		goto failed;
	if (ICDecompressGetFormatSize(handle, in) < 0)
		goto failed;
	*out = *in;
	out->biSize = sizeof(*out);
	if (display_format_fits(handle, in, out, width, height, (WORD)depth, BI_RGB))
		goto found;
	for (index = 0; index < ARRAYSIZE(formats); index++)
		if (formats[index].depth >= depth && display_format_fits(handle, in, out, width, height, formats[index].depth, formats[index].compression))
			goto found;
	if (depth <= 32 && display_format_fits(handle, in, out, in->biWidth, in->biHeight, 32, BI_RGB))
		goto found;
	if (ICDecompressGetFormat(handle, in, out) == ICERR_OK)
		return handle;
failed:
	if (owned)
		ICClose(handle);
	return NULL;
found:
	if (out->biBitCount <= 8)
		ICSendMessage(handle, ICM_DECOMPRESS_GET_PALETTE, (DWORD_PTR)in, (DWORD_PTR)out);
	return handle;
}

DWORD WINAPIV ICCompress(HIC handle, DWORD flags, BITMAPINFOHEADER *out_format, void *out, BITMAPINFOHEADER *in_format, void *in, DWORD *chunk, DWORD *key_flags, LONG frame, DWORD size, DWORD quality, BITMAPINFOHEADER *previous_format, void *previous)
{
	ICCOMPRESS request;
	request.dwFlags = flags;
	request.lpbiOutput = out_format;
	request.lpOutput = out;
	request.lpbiInput = in_format;
	request.lpInput = in;
	request.lpckid = chunk;
	request.lpdwFlags = key_flags;
	request.lFrameNum = frame;
	request.dwFrameSize = size;
	request.dwQuality = quality;
	request.lpbiPrev = previous_format;
	request.lpPrev = previous;
	return (DWORD)ICSendMessage(handle, ICM_COMPRESS, (DWORD_PTR)&request, sizeof(request));
}

DWORD WINAPIV ICDecompress(HIC handle, DWORD flags, BITMAPINFOHEADER *in_format, void *in, BITMAPINFOHEADER *out_format, void *out)
{
	ICDECOMPRESS request;
	request.dwFlags = flags;
	request.lpbiInput = in_format;
	request.lpInput = in;
	request.lpbiOutput = out_format;
	request.lpOutput = out;
	request.ckid = 0;
	return (DWORD)ICSendMessage(handle, ICM_DECOMPRESS, (DWORD_PTR)&request, sizeof(request));
}

DWORD WINAPIV ICDraw(HIC handle, DWORD flags, void *format, void *data, DWORD size, LONG time)
{
	ICDRAW request;
	request.dwFlags = flags;
	request.lpFormat = format;
	request.lpData = data;
	request.cbData = size;
	request.lTime = time;
	return (DWORD)ICSendMessage(handle, ICM_DRAW, (DWORD_PTR)&request, sizeof(request));
}

DWORD WINAPIV ICDrawBegin(HIC handle, DWORD flags, HPALETTE palette, HWND window, HDC context, int x, int y, int width, int height, BITMAPINFOHEADER *format, int source_x, int source_y, int source_width, int source_height, DWORD rate, DWORD scale)
{
	ICDRAWBEGIN request;
	request.dwFlags = flags;
	request.hpal = palette;
	request.hwnd = window;
	request.hdc = context;
	request.xDst = x;
	request.yDst = y;
	request.dxDst = width;
	request.dyDst = height;
	request.lpbi = format;
	request.xSrc = source_x;
	request.ySrc = source_y;
	request.dxSrc = source_width;
	request.dySrc = source_height;
	request.dwRate = rate;
	request.dwScale = scale;
	return (DWORD)ICSendMessage(handle, ICM_DRAW_BEGIN, (DWORD_PTR)&request, sizeof(request));
}

static DWORD header_size(const BITMAPINFOHEADER *format)
{
	DWORD colors = format->biClrUsed;
	if (!colors && format->biBitCount <= 8)
		colors = 1U << format->biBitCount;
	return format->biSize + colors * sizeof(RGBQUAD) + (format->biCompression == BI_BITFIELDS ? 12 : 0);
}

HANDLE WINAPI ICImageDecompress(HIC handle, UINT flags, BITMAPINFO *in, void *bits, BITMAPINFO *out)
{
	BITMAPINFO *format = out;
	BITMAPINFOHEADER wanted;
	BOOL owned = FALSE;
	HGLOBAL result = NULL;
	if (!in || !bits)
		return NULL;
	if (!handle)
	{
		if (!(handle = ICLocate(ICTYPE_VIDEO, 0, &in->bmiHeader, out ? &out->bmiHeader : NULL, ICMODE_DECOMPRESS)))
			return NULL;
		owned = TRUE;
	}
	if (!format)
	{
		format = (BITMAPINFO *)&wanted;
		if (ICDecompressGetFormat(handle, in, format) != ICERR_OK)
			goto done;
	}
	if (ICDecompressBegin(handle, in, format) == ICERR_OK)
	{
		DWORD head = header_size(&format->bmiHeader), body = image_size(format->bmiHeader.biWidth, format->bmiHeader.biHeight, format->bmiHeader.biBitCount);
		if ((result = GlobalAlloc(GMEM_MOVEABLE, head + body)))
		{
			BYTE *block = GlobalLock(result);
			memcpy(block, format, head);
			((BITMAPINFOHEADER *)block)->biSizeImage = body;
			if (ICDecompress(handle, 0, &in->bmiHeader, bits, (BITMAPINFOHEADER *)block, block + head) != ICERR_OK)
			{
				GlobalUnlock(result);
				GlobalFree(result);
				result = NULL;
			}
			else
				GlobalUnlock(result);
		}
		ICDecompressEnd(handle);
	}
done:
	if (owned)
		ICClose(handle);
	return result;
}

HANDLE WINAPI ICImageCompress(HIC handle, UINT flags, BITMAPINFO *in, void *bits, BITMAPINFO *out, LONG quality, LONG *size)
{
	BYTE buffer[sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD)];
	BITMAPINFO *format = out ? out : (BITMAPINFO *)buffer;
	BOOL owned = FALSE;
	HGLOBAL result = NULL;
	if (!in || !bits)
		return NULL;
	if (!handle)
	{
		if (!(handle = ICLocate(ICTYPE_VIDEO, 0, &in->bmiHeader, out ? &out->bmiHeader : NULL, ICMODE_COMPRESS)))
			return NULL;
		owned = TRUE;
	}
	if (!out && ICCompressGetFormat(handle, in, format) != ICERR_OK)
		goto done;
	if (ICCompressBegin(handle, in, format) == ICERR_OK)
	{
		DWORD head = header_size(&format->bmiHeader), body = (DWORD)ICCompressGetSize(handle, in, format), chunk = 0, keys = 0;
		if ((result = GlobalAlloc(GMEM_MOVEABLE, head + body)))
		{
			BYTE *block = GlobalLock(result);
			memcpy(block, format, head);
			if (ICCompress(handle, 0, (BITMAPINFOHEADER *)block, block + head, &in->bmiHeader, bits, &chunk, &keys, 0, size ? *size : 0, quality, NULL, NULL) != ICERR_OK)
			{
				GlobalUnlock(result);
				GlobalFree(result);
				result = NULL;
			}
			else
			{
				if (size)
					*size = ((BITMAPINFOHEADER *)block)->biSizeImage;
				GlobalUnlock(result);
			}
		}
		ICCompressEnd(handle);
	}
done:
	if (owned)
		ICClose(handle);
	return result;
}

/* Sequential compression: the state of a frame run lives in the caller's variables. */
BOOL WINAPI ICSeqCompressFrameStart(COMPVARS *state, BITMAPINFO *in)
{
	DWORD format_size, input_size;
	LRESULT frames;
	if (!state || state->cbSize != sizeof(*state))
		return FALSE;
	input_size = header_size(&in->bmiHeader);
	if (!(state->lpbiIn = vf_alloc(input_size)))
		return FALSE;
	memcpy(state->lpbiIn, in, input_size);
	format_size = (DWORD)ICCompressGetFormatSize(state->hic, in);
	if ((LONG)format_size <= 0 || !(state->lpbiOut = vf_alloc(format_size)))
		goto failed;
	if (ICCompressGetFormat(state->hic, in, state->lpbiOut) != ICERR_OK)
		goto failed;
	if (ICCompressQuery(state->hic, in, state->lpbiOut) != ICERR_OK || ICCompressBegin(state->hic, in, state->lpbiOut) != ICERR_OK)
		goto failed;
	frames = ICCompressGetSize(state->hic, in, state->lpbiOut);
	if (!(state->lpBitsOut = vf_alloc(frames)))
		goto failed;
	if (!(state->lpBitsPrev = vf_alloc(image_size(in->bmiHeader.biWidth, in->bmiHeader.biHeight, in->bmiHeader.biBitCount))))
		goto failed;
	state->lFrame = 0;
	state->lKeyCount = 0;
	state->dwFlags |= 0x4000;
	return TRUE;
failed:
	vf_free(state->lpbiIn);
	vf_free(state->lpbiOut);
	vf_free(state->lpBitsOut);
	state->lpbiIn = NULL;
	state->lpbiOut = NULL;
	state->lpBitsOut = NULL;
	return FALSE;
}

LPVOID WINAPI ICSeqCompressFrame(COMPVARS *state, UINT flags, void *bits, BOOL *key_frame, LONG *size)
{
	BOOL want_key = state->lKey && !state->lKeyCount, wanted_size = size && *size;
	DWORD keys = 0, chunk = 0, status, bytes;
	status = ICCompress(state->hic, want_key ? ICCOMPRESS_KEYFRAME : 0, state->lpbiOut, state->lpBitsOut, &state->lpbiIn->bmiHeader, bits, &chunk, &keys, state->lFrame,
		wanted_size ? *size : 0, state->lQ, want_key ? NULL : &state->lpbiIn->bmiHeader, want_key ? NULL : state->lpBitsPrev);
	if (status != ICERR_OK)
		return NULL;
	bytes = image_size(state->lpbiIn->bmiHeader.biWidth, state->lpbiIn->bmiHeader.biHeight, state->lpbiIn->bmiHeader.biBitCount);
	memcpy(state->lpBitsPrev, bits, bytes);
	state->lFrame++;
	state->lKeyCount = state->lKey ? (state->lKeyCount + 1) % state->lKey : 0;
	if (key_frame)
		*key_frame = (keys & AVIIF_KEYFRAME) != 0;
	if (size)
		*size = state->lpbiOut->bmiHeader.biSizeImage;
	return state->lpBitsOut;
}

void WINAPI ICSeqCompressFrameEnd(COMPVARS *state)
{
	if (!state || !(state->dwFlags & 0x4000))
		return;
	ICCompressEnd(state->hic);
	state->dwFlags &= ~0x4000;
	vf_free(state->lpbiIn);
	vf_free(state->lpbiOut);
	vf_free(state->lpBitsOut);
	vf_free(state->lpBitsPrev);
	state->lpbiIn = NULL;
	state->lpbiOut = NULL;
	state->lpBitsOut = NULL;
	state->lpBitsPrev = NULL;
}

void WINAPI ICCompressorFree(COMPVARS *state)
{
	if (!state || !(state->dwFlags & ICMF_COMPVARS_VALID))
		return;
	if (state->hic)
	{
		ICClose(state->hic);
		state->hic = NULL;
	}
	state->dwFlags = 0;
}

/* The chooser is a dialog nothing has asked for; saying the user cancelled is what its callers already handle. */
BOOL WINAPI ICCompressorChoose(HWND parent, UINT flags, void *in, void *state, COMPVARS *variables, char *title)
{
	return FALSE;
}

DWORD WINAPI VideoForWindowsVersion(void)
{
	return 0x010a0000;
}
