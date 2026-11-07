/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>

/* The animation control only ever plays small silent clips, uncompressed or RLE8, so it reads the AVI container itself
 * and decodes into a 32 bit canvas. Frames build on each other, which is why a seek replays from the first frame. */

#define PLAY_TIMER 1
#define FOURCC(a, b, c, d) ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))
#define TRANSPARENT_PIXEL 0xff000000u

typedef struct {
	const BYTE *data;
	DWORD size;
} frame_chunk;

typedef struct {
	HWND window, parent;
	HINSTANCE instance;
	BYTE *owned;
	const BYTE *clip;
	DWORD clip_size;
	frame_chunk *frames;
	int frame_count, frame_capacity, width, height, depth, compression;
	DWORD microseconds, stride;
	RGBQUAD palette[256];
	DWORD *canvas;
	int decoded, shown;
	int first, last, repeat;
	BOOL playing;
	DWORD key;
	BOOL has_key;
} animate_state;

static animate_state *state_of(HWND window)
{
	return (animate_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(const animate_state *state)
{
	return (DWORD)GetWindowLongPtrW(state->window, GWL_STYLE);
}

static void *allocate(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void release(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

static DWORD read_u32(const BYTE *at)
{
	return at[0] | ((DWORD)at[1] << 8) | ((DWORD)at[2] << 16) | ((DWORD)at[3] << 24);
}

/* Container */

static void add_frame(animate_state *state, const BYTE *data, DWORD size)
{
	if (state->frame_count == state->frame_capacity) {
		int capacity = state->frame_capacity ? state->frame_capacity * 2 : 32;
		frame_chunk *grown = allocate(capacity * sizeof(*grown));
		if (!grown)
			return;
		if (state->frames)
			CopyMemory(grown, state->frames, state->frame_count * sizeof(*grown));
		release(state->frames);
		state->frames = grown;
		state->frame_capacity = capacity;
	}
	state->frames[state->frame_count].data = data;
	state->frames[state->frame_count].size = size;
	state->frame_count++;
}

static void walk_chunks(animate_state *state, const BYTE *cursor, const BYTE *end, BOOL in_movie)
{
	while (end - cursor >= 8) {
		DWORD tag = read_u32(cursor), size = read_u32(cursor + 4);
		const BYTE *body = cursor + 8;
		const BYTE *next;
		if (size > (DWORD)(end - body))
			size = (DWORD)(end - body);
		next = body + size + (size & 1);
		if (tag == FOURCC('L', 'I', 'S', 'T') && size >= 4) {
			DWORD kind = read_u32(body);
			walk_chunks(state, body + 4, body + size, in_movie || kind == FOURCC('m', 'o', 'v', 'i') || kind == FOURCC('r', 'e', 'c', ' '));
		}
		else if (tag == FOURCC('a', 'v', 'i', 'h') && size >= 4) {
			state->microseconds = read_u32(body);
		}
		else if (tag == FOURCC('s', 't', 'r', 'f') && size >= sizeof(BITMAPINFOHEADER) && !state->width) {
			const BITMAPINFOHEADER *format = (const BITMAPINFOHEADER *)body;
			DWORD colors = format->biClrUsed ? format->biClrUsed : (format->biBitCount <= 8 ? 1u << format->biBitCount : 0);
			DWORD room = (size - format->biSize) / sizeof(RGBQUAD);
			state->width = format->biWidth;
			state->height = format->biHeight < 0 ? -format->biHeight : format->biHeight;
			state->depth = format->biBitCount;
			state->compression = format->biCompression;
			if (colors > 256)
				colors = 256;
			if (colors > room)
				colors = room;
			if (colors)
				CopyMemory(state->palette, body + format->biSize, colors * sizeof(RGBQUAD));
		}
		else if (in_movie && size) {
			/* Skips palette changes (xxpc) and any other stream: only stream 0's video data counts. */
			BYTE first = (BYTE)tag, second = (BYTE)(tag >> 8), third = (BYTE)(tag >> 16);
			if (first == '0' && second == '0' && (third == 'd'))
				add_frame(state, body, size);
		}
		cursor = next;
	}
}

static BOOL parse_clip(animate_state *state)
{
	const BYTE *clip = state->clip;
	if (state->clip_size < 12 || read_u32(clip) != FOURCC('R', 'I', 'F', 'F') || read_u32(clip + 8) != FOURCC('A', 'V', 'I', ' '))
		return FALSE;
	walk_chunks(state, clip + 12, clip + state->clip_size, FALSE);
	if (state->width <= 0 || state->height <= 0 || !state->frame_count)
		return FALSE;
	if (state->compression == BI_RGB) {
		if (state->depth != 8 && state->depth != 16 && state->depth != 24 && state->depth != 32)
			return FALSE;
	}
	else if (state->compression != BI_RLE8) {
		return FALSE;
	}
	state->stride = ((state->width * state->depth + 31) / 32) * 4;
	state->canvas = allocate((SIZE_T)state->width * state->height * sizeof(DWORD));
	return state->canvas != NULL;
}

/* Decoding */

static DWORD palette_pixel(const animate_state *state, BYTE index)
{
	const RGBQUAD *entry = &state->palette[index];
	return entry->rgbBlue | ((DWORD)entry->rgbGreen << 8) | ((DWORD)entry->rgbRed << 16);
}

static void decode_rle(animate_state *state, const frame_chunk *frame)
{
	const BYTE *cursor = frame->data, *end = frame->data + frame->size;
	int x = 0, y = 0;
	while (end - cursor >= 2) {
		BYTE count = cursor[0], value = cursor[1];
		cursor += 2;
		if (count) {
			int run;
			DWORD pixel = palette_pixel(state, value);
			for (run = 0; run < count; run++, x++) {
				if (x < state->width && y < state->height)
					state->canvas[(state->height - 1 - y) * state->width + x] = pixel;
			}
		}
		else if (value == 0) {
			x = 0;
			y++;
		}
		else if (value == 1) {
			return;
		}
		else if (value == 2) {
			if (end - cursor < 2)
				return;
			x += cursor[0];
			y += cursor[1];
			cursor += 2;
		}
		else {
			int run;
			if (end - cursor < value)
				return;
			for (run = 0; run < value; run++, x++) {
				if (x < state->width && y < state->height)
					state->canvas[(state->height - 1 - y) * state->width + x] = palette_pixel(state, cursor[run]);
			}
			cursor += value + (value & 1);
		}
	}
}

static void decode_raw(animate_state *state, const frame_chunk *frame)
{
	int x, y;
	for (y = 0; y < state->height; y++) {
		SIZE_T offset = (SIZE_T)y * state->stride;
		const BYTE *row;
		DWORD *target = state->canvas + (SIZE_T)(state->height - 1 - y) * state->width;
		if (offset + state->stride > frame->size)
			return;
		row = frame->data + offset;
		for (x = 0; x < state->width; x++) {
			switch (state->depth) {
			case 8:
				target[x] = palette_pixel(state, row[x]);
				break;
			case 16: {
				WORD packed = (WORD)(row[x * 2] | (row[x * 2 + 1] << 8));
				BYTE blue = (BYTE)((packed & 0x1f) << 3), green = (BYTE)(((packed >> 5) & 0x1f) << 3), red = (BYTE)(((packed >> 10) & 0x1f) << 3);
				target[x] = blue | ((DWORD)green << 8) | ((DWORD)red << 16);
				break;
			}
			case 24:
				target[x] = row[x * 3] | ((DWORD)row[x * 3 + 1] << 8) | ((DWORD)row[x * 3 + 2] << 16);
				break;
			default:
				target[x] = read_u32(row + x * 4) & 0xffffff;
				break;
			}
		}
	}
}

static void show_frame(animate_state *state, int index)
{
	if (index < state->decoded)
		state->decoded = 0;
	while (state->decoded <= index) {
		const frame_chunk *frame = &state->frames[state->decoded];
		if (state->compression == BI_RLE8)
			decode_rle(state, frame);
		else
			decode_raw(state, frame);
		state->decoded++;
	}
	state->shown = index;
	if ((style_of(state) & ACS_TRANSPARENT) && !state->has_key) {
		/* The colour to see through is whatever the first pixel of the clip holds. */
		state->key = state->canvas[0];
		state->has_key = TRUE;
	}
}

/* Painting */

static HBRUSH background_of(const animate_state *state, HDC dc)
{
	HBRUSH brush = (HBRUSH)SendMessageW(state->parent, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)state->window);
	return brush ? brush : GetSysColorBrush(COLOR_BTNFACE);
}

static void draw_contents(animate_state *state, HDC dc)
{
	RECT client;
	HBRUSH brush;
	GetClientRect(state->window, &client);
	brush = background_of(state, dc);
	FillRect(dc, &client, brush);
	if (state->canvas && state->decoded) {
		BITMAPINFOHEADER header;
		DWORD *pixels = state->canvas;
		DWORD *blended = NULL;
		int x = 0, y = 0;
		if (style_of(state) & ACS_CENTER) {
			x = (client.right - state->width) / 2;
			y = (client.bottom - state->height) / 2;
		}
		ZeroMemory(&header, sizeof(header));
		header.biSize = sizeof(header);
		header.biWidth = state->width;
		header.biHeight = -state->height;
		header.biPlanes = 1;
		header.biBitCount = 32;
		header.biCompression = BI_RGB;
		if ((style_of(state) & ACS_TRANSPARENT) && state->has_key) {
			/* The canvas is kept intact because later frames build on it; only the copy going to screen gets the see through
			 * pixels replaced. */
			COLORREF behind = GetPixel(dc, client.right > 0 ? x > 0 ? x : 0 : 0, y > 0 ? y : 0);
			DWORD fill = GetBValue(behind) | ((DWORD)GetGValue(behind) << 8) | ((DWORD)GetRValue(behind) << 16);
			SIZE_T total = (SIZE_T)state->width * state->height, index;
			blended = allocate(total * sizeof(DWORD));
			if (blended) {
				for (index = 0; index < total; index++)
					blended[index] = pixels[index] == state->key ? fill : pixels[index];
				pixels = blended;
			}
		}
		SetDIBitsToDevice(dc, x, y, state->width, state->height, 0, 0, 0, state->height, pixels, (BITMAPINFO *)&header, DIB_RGB_COLORS);
		release(blended);
	}
}

/* Playback */

static void notify_parent(const animate_state *state, WORD code)
{
	SendMessageW(state->parent, WM_COMMAND, MAKEWPARAM(GetWindowLongPtrW(state->window, GWLP_ID), code), (LPARAM)state->window);
}

static BOOL stop_playback(animate_state *state)
{
	if (!state->playing)
		return FALSE;
	state->playing = FALSE;
	KillTimer(state->window, PLAY_TIMER);
	notify_parent(state, ACN_STOP);
	return TRUE;
}

static void close_clip(animate_state *state)
{
	stop_playback(state);
	release(state->frames);
	release(state->canvas);
	release(state->owned);
	state->frames = NULL;
	state->canvas = NULL;
	state->owned = NULL;
	state->clip = NULL;
	state->clip_size = 0;
	state->frame_count = state->frame_capacity = 0;
	state->width = state->height = state->depth = state->compression = 0;
	state->decoded = state->shown = 0;
	state->has_key = FALSE;
	state->microseconds = 0;
	ZeroMemory(state->palette, sizeof(state->palette));
}

static BOOL start_playback(animate_state *state, int repeat, int first, int last)
{
	UINT delay;
	if (!state->frame_count)
		return FALSE;
	/* The end is -1 for the last frame, and the words of the request arrive as unsigned. */
	if (last == 0xffff || last < 0 || last >= state->frame_count)
		last = state->frame_count - 1;
	if (first < 0 || first > last)
		first = 0;
	stop_playback(state);
	state->first = first;
	state->last = last;
	state->repeat = repeat;
	show_frame(state, first);
	InvalidateRect(state->window, NULL, FALSE);
	if (first == last && repeat != -1 && repeat <= 1)
		return TRUE;
	if (repeat == 0)
		return TRUE;
	state->playing = TRUE;
	notify_parent(state, ACN_START);
	delay = state->microseconds ? state->microseconds / 1000 : 66;
	SetTimer(state->window, PLAY_TIMER, delay ? delay : 1, NULL);
	return TRUE;
}

static void advance(animate_state *state)
{
	if (state->shown < state->last) {
		show_frame(state, state->shown + 1);
	}
	else {
		if (state->repeat > 0 && --state->repeat == 0) {
			stop_playback(state);
			return;
		}
		show_frame(state, state->first);
	}
	InvalidateRect(state->window, NULL, FALSE);
}

/* Opening */

static BOOL load_resource(animate_state *state, HINSTANCE module, const WCHAR *name)
{
	HRSRC found = FindResourceW(module, name, L"AVI");
	HGLOBAL loaded;
	if (!found)
		return FALSE;
	loaded = LoadResource(module, found);
	if (!loaded)
		return FALSE;
	state->clip = LockResource(loaded);
	state->clip_size = SizeofResource(module, found);
	return state->clip != NULL;
}

static BOOL load_file(animate_state *state, const WCHAR *path)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	DWORD size, done = 0;
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	size = GetFileSize(file, NULL);
	if (size != INVALID_FILE_SIZE && size)
		state->owned = allocate(size);
	if (state->owned && ReadFile(file, state->owned, size, &done, NULL) && done == size) {
		state->clip = state->owned;
		state->clip_size = size;
	}
	CloseHandle(file);
	return state->clip != NULL;
}

static LRESULT open_clip(animate_state *state, HINSTANCE module, const WCHAR *name)
{
	close_clip(state);
	if (!name) {
		InvalidateRect(state->window, NULL, TRUE);
		return 0;
	}
	if (!module)
		module = state->instance;
	if (!load_resource(state, module, name)) {
		DWORD failure = GetLastError();
		if (IS_INTRESOURCE(name) || !load_file(state, name)) {
			if (IS_INTRESOURCE(name))
				SetLastError(failure);
			return 0;
		}
	}
	if (!parse_clip(state)) {
		close_clip(state);
		return 0;
	}
	if (!(style_of(state) & ACS_CENTER))
		SetWindowPos(state->window, 0, 0, 0, state->width, state->height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	show_frame(state, 0);
	InvalidateRect(state->window, NULL, TRUE);
	if (style_of(state) & ACS_AUTOPLAY)
		start_playback(state, -1, 0, state->frame_count - 1);
	return 1;
}

static LRESULT CALLBACK animate_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	animate_state *state = state_of(window);

	if (message == WM_NCCREATE) {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = allocate(sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		state->instance = create->hInstance;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	if (!state)
		return DefWindowProcW(window, message, wparam, lparam);

	switch (message) {
	case WM_NCDESTROY:
		SetWindowLongPtrW(window, 0, 0);
		/* A parent going away does not want to hear about the stop. */
		state->playing = FALSE;
		close_clip(state);
		release(state);
		break;
	case ACM_OPENW:
		return open_clip(state, (HINSTANCE)wparam, (const WCHAR *)lparam);
	case ACM_OPENA: {
		const char *name = (const char *)lparam;
		WCHAR *wide = NULL;
		LRESULT result;
		if (!name || IS_INTRESOURCE(name))
			return open_clip(state, (HINSTANCE)wparam, (const WCHAR *)name);
		{
			int size = MultiByteToWideChar(CP_ACP, 0, name, -1, NULL, 0);
			wide = allocate(size * sizeof(WCHAR));
			if (!wide)
				return 0;
			MultiByteToWideChar(CP_ACP, 0, name, -1, wide, size);
		}
		result = open_clip(state, (HINSTANCE)wparam, wide);
		release(wide);
		return result;
	}
	case ACM_PLAY:
		return start_playback(state, (int)wparam, LOWORD(lparam), HIWORD(lparam));
	case ACM_STOP:
		stop_playback(state);
		return TRUE;
	case ACM_ISPLAYING:
		return state->playing;
	case WM_TIMER:
		if (wparam == PLAY_TIMER && state->playing) {
			advance(state);
			return 0;
		}
		break;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT info;
		HDC dc;
		if (wparam) {
			draw_contents(state, (HDC)wparam);
			return 0;
		}
		dc = BeginPaint(window, &info);
		if (dc)
			draw_contents(state, dc);
		EndPaint(window, &info);
		return 0;
	}
	case WM_PRINTCLIENT:
		draw_contents(state, (HDC)wparam);
		return 0;
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	case WM_SIZE:
		if (style_of(state) & ACS_CENTER)
			InvalidateRect(window, NULL, TRUE);
		break;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void ANIMATE_Register(void)
{
	WNDCLASSW info;

	ZeroMemory(&info, sizeof(info));
	info.style = CS_GLOBALCLASS | CS_DBLCLKS;
	info.lpfnWndProc = animate_proc;
	info.cbWndExtra = sizeof(animate_state *);
	info.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	info.lpszClassName = ANIMATE_CLASSW;
	RegisterClassW(&info);
}

void ANIMATE_Unregister(void)
{
	UnregisterClassW(ANIMATE_CLASSW, NULL);
}
