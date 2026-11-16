/* SPDX-License-Identifier: MIT
 *
 * PlaySound and sndPlaySound: one sound at a time, played through a wave output of this component. The sound is found and its RIFF
 * header read before the call returns, so an invalid file or memory sound fails even when it was asked to play asynchronously; only the
 * playing happens on a thread of its own. Compressed sounds would need the ACM and are refused.
 */
#include "winmm_private.h"

#define SOUND_FILE_LIMIT (64u << 20)

typedef struct sound {
	HWAVEOUT device;
	WAVEHDR header;
	HANDLE done, stop, finished;
	BYTE *owned;
	BOOL loop;
} sound;

static CRITICAL_SECTION sound_lock;
static CRITICAL_SECTION play_lock;
static sound *current;

void sound_init(void)
{
	InitializeCriticalSection(&sound_lock);
	InitializeCriticalSection(&play_lock);
}

static BOOL wide_append(WCHAR *target, size_t size, const WCHAR *text)
{
	if ((size_t)lstrlenW(target) + (size_t)lstrlenW(text) + 1 > size)
		return FALSE;
	lstrcatW(target, text);
	return TRUE;
}

static BOOL is_atom(const void *name) { return ((UINT_PTR)name >> 16) == 0; }

/* ----- Finding the sound ----- */

static BYTE *read_file(const WCHAR *path, DWORD *size)
{
	const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	BYTE *data = NULL;
	const DWORD length = GetFileSize(file, NULL);
	if (length != INVALID_FILE_SIZE && length >= 12 && length <= SOUND_FILE_LIMIT && (data = ALLOC(length))) {
		DWORD got = 0;
		if (!ReadFile(file, data, length, &got, NULL) || got != length) {
			FREE(data);
			data = NULL;
		}
	}
	CloseHandle(file);
	*size = length;
	return data;
}

/* A file as named, then in the Windows media folder when the name has no folder of its own. */
static BYTE *find_file(const WCHAR *name, DWORD *size)
{
	BYTE *data = read_file(name, size);
	if (data)
		return data;
	for (const WCHAR *at = name; *at; at++)
		if (*at == L'\\' || *at == L'/' || *at == L':')
			return NULL;
	WCHAR path[MAX_PATH];
	const UINT length = GetWindowsDirectoryW(path, MAX_PATH);
	if (!length || length >= MAX_PATH)
		return NULL;
	if (!wide_append(path, MAX_PATH, L"\\Media\\") || !wide_append(path, MAX_PATH, name))
		return NULL;
	if ((data = read_file(path, size)))
		return data;
	return wide_append(path, MAX_PATH, L".wav") ? read_file(path, size) : NULL;
}

typedef LONG(WINAPI *open_key_function)(HKEY, const WCHAR *, DWORD, REGSAM, HKEY *);
typedef LONG(WINAPI *query_value_function)(HKEY, const WCHAR *, DWORD *, DWORD *, BYTE *, DWORD *);
typedef LONG(WINAPI *close_key_function)(HKEY);

/* The file the user's sound scheme gives an event name such as SystemAsterisk. advapi32 is loaded only for this. */
static BYTE *find_alias(const WCHAR *alias, DWORD *size)
{
	HMODULE advapi = GetModuleHandleW(L"advapi32.dll");
	if (!advapi)
		advapi = LoadLibraryW(L"advapi32.dll");
	if (!advapi)
		return NULL;
	const open_key_function open_key = (open_key_function)GetProcAddress(advapi, "RegOpenKeyExW");
	const query_value_function query_value = (query_value_function)GetProcAddress(advapi, "RegQueryValueExW");
	const close_key_function close_key = (close_key_function)GetProcAddress(advapi, "RegCloseKey");
	if (!open_key || !query_value || !close_key)
		return NULL;
	WCHAR path[256] = L"AppEvents\\Schemes\\Apps\\.Default\\";
	if (!wide_append(path, 256, alias) || !wide_append(path, 256, L"\\.Current"))
		return NULL;
	HKEY key;
	if (open_key(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &key))
		return NULL;
	WCHAR value[MAX_PATH];
	DWORD type = 0, bytes = sizeof value - sizeof(WCHAR);
	const LONG status = query_value(key, NULL, NULL, &type, (BYTE *)value, &bytes);
	close_key(key);
	if (status || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(WCHAR))
		return NULL;
	value[bytes / sizeof(WCHAR)] = 0;
	WCHAR expanded[MAX_PATH];
	const DWORD length = ExpandEnvironmentStringsW(value, expanded, MAX_PATH);
	return length && length <= MAX_PATH ? find_file(expanded, size) : find_file(value, size);
}

static const WCHAR *alias_of_id(UINT_PTR id)
{
	switch (id) {
	case SND_ALIAS_SYSTEMASTERISK:
		return L"SystemAsterisk";
	case SND_ALIAS_SYSTEMQUESTION:
		return L"SystemQuestion";
	case SND_ALIAS_SYSTEMHAND:
		return L"SystemHand";
	case SND_ALIAS_SYSTEMEXIT:
		return L"SystemExit";
	case SND_ALIAS_SYSTEMSTART:
		return L"SystemStart";
	case SND_ALIAS_SYSTEMWELCOME:
		return L"SystemWelcome";
	case SND_ALIAS_SYSTEMEXCLAMATION:
		return L"SystemExclamation";
	case SND_ALIAS_SYSTEMDEFAULT:
		return L".Default";
	default:
		return NULL;
	}
}

/* The sound's bytes: owned ones are set in *owned, to be freed once played; a memory or resource sound is the program's. */
static const BYTE *find_sound(const WCHAR *name, HMODULE module, DWORD flags, BYTE **owned, DWORD *size)
{
	*owned = NULL;
	*size = 0;
	if ((flags & SND_RESOURCE) == SND_RESOURCE) {
		const HRSRC found = FindResourceW(module, name, L"WAVE");
		const HGLOBAL loaded = found ? LoadResource(module, found) : NULL;
		*size = found ? SizeofResource(module, found) : 0;
		return loaded ? LockResource(loaded) : NULL;
	}
	if (flags & SND_MEMORY) {
		const BYTE *data = (const BYTE *)name;
		/* The program gives no size; the RIFF header has it. */
		if (data && !memcmp(data, "RIFF", 4))
			*size = 8 + (data[4] | data[5] << 8 | data[6] << 16 | (DWORD)data[7] << 24);
		return data;
	}
	if ((flags & SND_ALIAS_ID) == SND_ALIAS_ID) {
		const WCHAR *alias = alias_of_id((UINT_PTR)name);
		return alias ? (*owned = find_alias(alias, size)) : NULL;
	}
	if (is_atom(name))
		return NULL;
	if (flags & SND_ALIAS)
		return *owned = find_alias(name, size);
	if (flags & SND_FILENAME)
		return *owned = find_file(name, size);
	if ((*owned = find_alias(name, size)))
		return *owned;
	return *owned = find_file(name, size);
}

/* The format and the samples of a RIFF WAVE image. */
static BOOL parse_wave(const BYTE *data, DWORD size, WAVEFORMATEXTENSIBLE *format, const BYTE **samples, DWORD *length)
{
	if (size < 12 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4))
		return FALSE;
	BOOL have_format = FALSE;
	*samples = NULL;
	*length = 0;
	DWORD at = 12;
	while (at + 8 <= size) {
		const BYTE *chunk = data + at;
		const DWORD chunk_size = chunk[4] | chunk[5] << 8 | chunk[6] << 16 | (DWORD)chunk[7] << 24;
		const DWORD available = size - at - 8;
		const DWORD body = chunk_size < available ? chunk_size : available;
		if (!memcmp(chunk, "fmt ", 4)) {
			if (body < 16)
				return FALSE;
			memset(format, 0, sizeof *format);
			memcpy(format, chunk + 8, body < sizeof *format ? body : sizeof *format);
			if (body < sizeof(WAVEFORMATEX))
				format->Format.cbSize = 0;
			if (format->Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE && format->Format.cbSize > 0)
				format->Format.cbSize = 0;
			have_format = TRUE;
		} else if (!memcmp(chunk, "data", 4)) {
			*samples = chunk + 8;
			*length = body;
		}
		if (have_format && *samples)
			break;
		if (chunk_size > available)
			break;
		at += 8 + chunk_size + (chunk_size & 1);
	}
	if (!have_format || !*samples || !format->Format.nBlockAlign)
		return FALSE;
	*length -= *length % format->Format.nBlockAlign;
	return *length != 0;
}

/* ----- Playing it ----- */

static void sound_free(sound *sound)
{
	waveOutUnprepareHeader(sound->device, &sound->header, sizeof sound->header);
	waveOutClose(sound->device);
	CloseHandle(sound->done);
	CloseHandle(sound->stop);
	CloseHandle(sound->finished);
	if (sound->owned)
		FREE(sound->owned);
	FREE(sound);
}

/* Queues the sound while the caller serializes replacement of `current`. */
static BOOL sound_start(sound *sound)
{
	return waveOutWrite(sound->device, &sound->header, sizeof sound->header) == MMSYSERR_NOERROR;
}

/* Whoever took it off `current` to stop it waits for `finished` and frees it; otherwise it is freed here. */
static void sound_wait(sound *sound)
{
	const HANDLE events[2] = {sound->done, sound->stop};
	while (!(sound->header.dwFlags & WHDR_DONE))
		if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
			waveOutReset(sound->device);
			break;
		}
	EnterCriticalSection(&sound_lock);
	const BOOL mine = current == sound;
	if (mine)
		current = NULL;
	LeaveCriticalSection(&sound_lock);
	if (mine)
		sound_free(sound);
	else
		SetEvent(sound->finished);
}

static DWORD WINAPI sound_main(void *parameter)
{
	sound_wait(parameter);
	return 0;
}

/* Stops the sound playing, if any, and waits for it to be done with. */
static void sound_stop(void)
{
	EnterCriticalSection(&sound_lock);
	sound *playing = current;
	current = NULL;
	LeaveCriticalSection(&sound_lock);
	if (!playing)
		return;
	SetEvent(playing->stop);
	WaitForSingleObject(playing->finished, INFINITE);
	sound_free(playing);
}

static BOOL play_sound(const WCHAR *name, HMODULE module, DWORD flags)
{
	EnterCriticalSection(&play_lock);
	if (!name || (flags & SND_PURGE)) {
		sound_stop();
		LeaveCriticalSection(&play_lock);
		return TRUE;
	}
	if (flags & SND_NOSTOP) {
		EnterCriticalSection(&sound_lock);
		const BOOL busy = current != NULL;
		LeaveCriticalSection(&sound_lock);
		if (busy) {
			LeaveCriticalSection(&play_lock);
			return FALSE;
		}
	}
	BYTE *owned;
	DWORD size;
	const BYTE *data = find_sound(name, module, flags, &owned, &size);
	if (!data && !(flags & SND_NODEFAULT) && !(flags & SND_MEMORY))
		data = owned = find_alias(L".Default", &size);
	/* Preserve the runtime's silent system sounds when no scheme resolves the alias, unless the caller asks for failure. */
	if (!data && (flags & SND_ALIAS) && !(flags & SND_NODEFAULT)) {
		LeaveCriticalSection(&play_lock);
		return TRUE;
	}
	WAVEFORMATEXTENSIBLE format;
	const BYTE *samples;
	DWORD length;
	if (!data || !parse_wave(data, size, &format, &samples, &length)) {
		if (owned)
			FREE(owned);
		LeaveCriticalSection(&play_lock);
		return FALSE;
	}
	sound_stop();
	sound *made = ALLOC(sizeof *made);
	if (!made) {
		if (owned)
			FREE(owned);
		LeaveCriticalSection(&play_lock);
		return FALSE;
	}
	made->owned = owned;
	made->loop = (flags & SND_LOOP) != 0;
	made->done = CreateEventW(NULL, FALSE, FALSE, NULL);
	made->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	made->finished = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!made->done || !made->stop || !made->finished ||
		waveOutOpen(&made->device, WAVE_MAPPER, &format.Format, (DWORD_PTR)made->done, 0, CALLBACK_EVENT)) {
		if (made->done)
			CloseHandle(made->done);
		if (made->stop)
			CloseHandle(made->stop);
		if (made->finished)
			CloseHandle(made->finished);
		if (owned)
			FREE(owned);
		FREE(made);
		LeaveCriticalSection(&play_lock);
		return FALSE;
	}
	made->header.lpData = (LPSTR)samples;
	made->header.dwBufferLength = length;
	if (made->loop) {
		made->header.dwFlags = WHDR_BEGINLOOP | WHDR_ENDLOOP;
		made->header.dwLoops = 0xffffffff;
	}
	if (waveOutPrepareHeader(made->device, &made->header, sizeof made->header)) {
		sound_free(made);
		LeaveCriticalSection(&play_lock);
		return FALSE;
	}
	EnterCriticalSection(&sound_lock);
	current = made;
	LeaveCriticalSection(&sound_lock);
	if (!sound_start(made)) {
		EnterCriticalSection(&sound_lock);
		if (current == made)
			current = NULL;
		LeaveCriticalSection(&sound_lock);
		sound_free(made);
		LeaveCriticalSection(&play_lock);
		return FALSE;
	}
	/* A looping sound would never return, so it plays asynchronously whatever the flags say, as on Windows. */
	if (!(flags & SND_ASYNC) && !made->loop) {
		LeaveCriticalSection(&play_lock);
		sound_wait(made);
		return TRUE;
	}
	const HANDLE thread = CreateThread(NULL, 64 * 1024, sound_main, made, 0, NULL);
	if (!thread) {
		EnterCriticalSection(&sound_lock);
		if (current == made)
			current = NULL;
		LeaveCriticalSection(&sound_lock);
		waveOutReset(made->device);
		sound_free(made);
		LeaveCriticalSection(&play_lock);
		return FALSE;
	}
	CloseHandle(thread);
	LeaveCriticalSection(&play_lock);
	return TRUE;
}

static WCHAR *wide_from_ascii(const char *text)
{
	const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	if (length <= 0)
		return NULL;
	WCHAR *wide = ALLOC((size_t)length * sizeof(WCHAR));
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
	return wide;
}

EXPORT BOOL WINAPI PlaySoundW(LPCWSTR name, HMODULE module, DWORD flags) { return play_sound(name, module, flags); }

EXPORT BOOL WINAPI PlaySoundA(LPCSTR name, HMODULE module, DWORD flags)
{
	/* Memory, an id, and an atom name are passed through as they are. SND_RESOURCE holds the SND_MEMORY bit and names a resource. */
	if (!name || (flags & SND_RESOURCE) == SND_MEMORY || (flags & SND_ALIAS_ID) == SND_ALIAS_ID || is_atom(name) || (flags & SND_PURGE))
		return play_sound((const WCHAR *)name, module, flags);
	WCHAR *wide = wide_from_ascii(name);
	if (!wide)
		return FALSE;
	const BOOL played = play_sound(wide, module, flags);
	FREE(wide);
	return played;
}

#undef PlaySound
EXPORT BOOL WINAPI PlaySound(LPCSTR name, HMODULE module, DWORD flags) { return PlaySoundA(name, module, flags); }

#define SND_PLAY_FLAGS (SND_ASYNC | SND_NODEFAULT | SND_MEMORY | SND_LOOP | SND_NOSTOP)

EXPORT BOOL WINAPI sndPlaySoundW(LPCWSTR name, UINT flags) { return play_sound(name, NULL, flags & SND_PLAY_FLAGS); }

EXPORT BOOL WINAPI sndPlaySoundA(LPCSTR name, UINT flags) { return PlaySoundA(name, NULL, flags & SND_PLAY_FLAGS); }
