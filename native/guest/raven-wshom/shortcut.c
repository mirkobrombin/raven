/* SPDX-License-Identifier: MIT */
#include "wshom.h"
#include <shlobj.h>

/* WshShortcut and WshURLShortcut: a shortcut file named by CreateShortcut, a shell link (.lnk) or an Internet shortcut (.url). Its
 * properties are read from the file when it is there, and Save writes them back. */

enum {
	LINK_ARGUMENTS = 1, LINK_DESCRIPTION, LINK_FULLNAME, LINK_HOTKEY, LINK_ICONLOCATION, LINK_RELATIVEPATH, LINK_TARGETPATH, LINK_WINDOWSTYLE,
	LINK_WORKINGDIRECTORY, LINK_SAVE, LINK_LOAD,
};

/* The texts a shortcut keeps, by property. */
enum { TEXT_ARGUMENTS, TEXT_DESCRIPTION, TEXT_HOTKEY, TEXT_ICON, TEXT_RELATIVE, TEXT_TARGET, TEXT_DIRECTORY, TEXT_COUNT };

typedef struct {
	sr_object base;
	WCHAR *path;
	BOOL url;
	WCHAR *text[TEXT_COUNT];
	LONG window_style;
} shortcut;

static const sr_member link_members[] = {
	{ L"Arguments", LINK_ARGUMENTS, 0 }, { L"Description", LINK_DESCRIPTION, 0 }, { L"FullName", LINK_FULLNAME, 0 }, { L"Hotkey", LINK_HOTKEY, 0 },
	{ L"IconLocation", LINK_ICONLOCATION, 0 }, { L"RelativePath", LINK_RELATIVEPATH, 0 }, { L"TargetPath", LINK_TARGETPATH, 0 },
	{ L"WindowStyle", LINK_WINDOWSTYLE, 0 }, { L"WorkingDirectory", LINK_WORKINGDIRECTORY, 0 }, { L"Save", LINK_SAVE, 0 }, { L"Load", LINK_LOAD, 1 },
	{ NULL },
};

static const sr_member url_members[] = {
	{ L"FullName", LINK_FULLNAME, 0 }, { L"TargetPath", LINK_TARGETPATH, 0 }, { L"Save", LINK_SAVE, 0 }, { L"Load", LINK_LOAD, 1 }, { NULL },
};

static void set_text(shortcut *s, UINT which, const WCHAR *text)
{
	sr_free(s->text[which]);
	s->text[which] = sr_copy(text ? text : L"");
}

/* Hotkeys as text, "CTRL+ALT+F", and as the shell link keeps them, a virtual key with its modifiers in the high byte. */
static WORD hotkey_of(const WCHAR *text)
{
	static const struct {
		const WCHAR *name;
		BYTE modifier;
	} modifiers[] = { { L"SHIFT", HOTKEYF_SHIFT }, { L"CTRL", HOTKEYF_CONTROL }, { L"ALT", HOTKEYF_ALT }, { L"EXT", HOTKEYF_EXT } };
	BYTE held = 0, key = 0;
	const WCHAR *part = text;
	while (*part) {
		const WCHAR *end = part;
		while (*end && *end != L'+')
			end++;
		UINT length = (UINT)(end - part), i;
		for (i = 0; i < ARRAYSIZE(modifiers); i++)
			if ((UINT)lstrlenW(modifiers[i].name) == length &&
				CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, modifiers[i].name, -1, part, (int)length) == CSTR_EQUAL)
				break;
		if (i < ARRAYSIZE(modifiers))
			held |= modifiers[i].modifier;
		else if (length == 1)
			key = (BYTE)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)part[0]);
		else if ((part[0] == L'F' || part[0] == L'f') && length <= 3) {
			UINT number = 0;
			for (UINT d = 1; d < length; d++)
				number = number * 10 + (part[d] - L'0');
			if (number >= 1 && number <= 24)
				key = (BYTE)(VK_F1 + number - 1);
		}
		part = *end ? end + 1 : end;
	}
	return key ? MAKEWORD(key, held) : 0;
}

static void hotkey_text(WORD hotkey, WCHAR *text)
{
	text[0] = 0;
	BYTE key = LOBYTE(hotkey), held = HIBYTE(hotkey);
	if (!key)
		return;
	if (held & HOTKEYF_ALT)
		lstrcatW(text, L"Alt+");
	if (held & HOTKEYF_CONTROL)
		lstrcatW(text, L"Ctrl+");
	if (held & HOTKEYF_SHIFT)
		lstrcatW(text, L"Shift+");
	if (held & HOTKEYF_EXT)
		lstrcatW(text, L"Ext+");
	WCHAR name[8];
	if (key >= VK_F1 && key <= VK_F24)
		wsprintfW(name, L"F%u", key - VK_F1 + 1);
	else
		wsprintfW(name, L"%c", key);
	lstrcatW(text, name);
}

static HRESULT open_link(IShellLinkW **link, IPersistFile **file)
{
	HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)link);
	if (FAILED(hr))
		return hr;
	hr = IShellLinkW_QueryInterface(*link, &IID_IPersistFile, (void **)file);
	if (FAILED(hr))
		IShellLinkW_Release(*link);
	return hr;
}

static HRESULT load(shortcut *s, const WCHAR *path)
{
	WCHAR text[2084];
	if (s->url) {
		GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", text, ARRAYSIZE(text), path);
		set_text(s, TEXT_TARGET, text);
		return S_OK;
	}
	IShellLinkW *link;
	IPersistFile *file;
	HRESULT hr = open_link(&link, &file);
	if (FAILED(hr))
		return hr;
	if (SUCCEEDED(hr = IPersistFile_Load(file, path, STGM_READ))) {
		int index = 0, style = SW_SHOWNORMAL;
		WORD hotkey = 0;
		text[0] = 0;
		IShellLinkW_GetPath(link, text, MAX_PATH, NULL, 0);
		set_text(s, TEXT_TARGET, text);
		text[0] = 0;
		IShellLinkW_GetArguments(link, text, ARRAYSIZE(text));
		set_text(s, TEXT_ARGUMENTS, text);
		text[0] = 0;
		IShellLinkW_GetDescription(link, text, ARRAYSIZE(text));
		set_text(s, TEXT_DESCRIPTION, text);
		text[0] = 0;
		IShellLinkW_GetWorkingDirectory(link, text, MAX_PATH);
		set_text(s, TEXT_DIRECTORY, text);
		text[0] = 0;
		IShellLinkW_GetIconLocation(link, text, MAX_PATH, &index);
		WCHAR *icon = sr_alloc((lstrlenW(text) + 16) * sizeof(WCHAR));
		wsprintfW(icon, L"%s,%d", text, index);
		set_text(s, TEXT_ICON, icon);
		sr_free(icon);
		IShellLinkW_GetHotkey(link, &hotkey);
		hotkey_text(hotkey, text);
		set_text(s, TEXT_HOTKEY, text);
		IShellLinkW_GetShowCmd(link, &style);
		s->window_style = style;
	}
	IPersistFile_Release(file);
	IShellLinkW_Release(link);
	return hr;
}

static HRESULT save(shortcut *s)
{
	if (s->url)
		return WritePrivateProfileStringW(L"InternetShortcut", L"URL", s->text[TEXT_TARGET], s->path) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
	IShellLinkW *link;
	IPersistFile *file;
	HRESULT hr = open_link(&link, &file);
	if (FAILED(hr))
		return hr;
	IShellLinkW_SetPath(link, s->text[TEXT_TARGET]);
	IShellLinkW_SetArguments(link, s->text[TEXT_ARGUMENTS]);
	IShellLinkW_SetDescription(link, s->text[TEXT_DESCRIPTION]);
	IShellLinkW_SetWorkingDirectory(link, s->text[TEXT_DIRECTORY]);
	if (*s->text[TEXT_RELATIVE])
		IShellLinkW_SetRelativePath(link, s->text[TEXT_RELATIVE], 0);
	IShellLinkW_SetHotkey(link, hotkey_of(s->text[TEXT_HOTKEY]));
	IShellLinkW_SetShowCmd(link, s->window_style);
	/* The icon is "path,index"; the index is the number after the last comma. */
	WCHAR *icon = sr_copy(s->text[TEXT_ICON]), *comma = NULL;
	for (WCHAR *c = icon; *c; c++)
		if (*c == L',')
			comma = c;
	int index = 0;
	if (comma) {
		*comma = 0;
		BOOL negative = comma[1] == L'-';
		for (WCHAR *d = comma + 1 + negative; *d >= L'0' && *d <= L'9'; d++)
			index = index * 10 + (*d - L'0');
		if (negative)
			index = -index;
	}
	if (*icon)
		IShellLinkW_SetIconLocation(link, icon, index);
	sr_free(icon);
	hr = IPersistFile_Save(file, s->path, TRUE);
	IPersistFile_Release(file);
	IShellLinkW_Release(link);
	return hr;
}

static HRESULT shortcut_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	static const UINT texts[] = {
		[LINK_ARGUMENTS] = TEXT_ARGUMENTS, [LINK_DESCRIPTION] = TEXT_DESCRIPTION, [LINK_HOTKEY] = TEXT_HOTKEY, [LINK_ICONLOCATION] = TEXT_ICON,
		[LINK_RELATIVEPATH] = TEXT_RELATIVE, [LINK_TARGETPATH] = TEXT_TARGET, [LINK_WORKINGDIRECTORY] = TEXT_DIRECTORY,
	};
	shortcut *s = (shortcut *)self;
	BOOL put = sr_is_put(flags);
	BSTR text = NULL;
	HRESULT hr;
	switch (id) {
	case LINK_FULLNAME:
		return put ? DISP_E_MEMBERNOTFOUND : sr_return_text(result, s->path);
	case LINK_RELATIVEPATH:
		/* The relative path can be set and not read. */
		if (!put)
			return DISP_E_MEMBERNOTFOUND;
		/* fall through */
	case LINK_ARGUMENTS:
	case LINK_DESCRIPTION:
	case LINK_HOTKEY:
	case LINK_ICONLOCATION:
	case LINK_TARGETPATH:
	case LINK_WORKINGDIRECTORY:
		if (!put)
			return sr_return_text(result, s->text[texts[id]]);
		if (FAILED(hr = sr_text(sr_put_value(params), &text)))
			return hr;
		set_text(s, texts[id], text);
		SysFreeString(text);
		return S_OK;
	case LINK_WINDOWSTYLE:
		if (!put)
			return sr_return_long(result, s->window_style);
		return sr_long(sr_put_value(params), &s->window_style);
	case LINK_SAVE:
		return put ? DISP_E_MEMBERNOTFOUND : save(s);
	case LINK_LOAD:
		if (put)
			return DISP_E_MEMBERNOTFOUND;
		if (FAILED(hr = sr_text(sr_arg(params, flags, 0), &text)))
			return hr;
		hr = load(s, text);
		if (SUCCEEDED(hr)) {
			sr_free(s->path);
			s->path = sr_copy(text);
		}
		SysFreeString(text);
		return hr;
	}
	return DISP_E_MEMBERNOTFOUND;
}

static void shortcut_destroy(sr_object *self)
{
	shortcut *s = (shortcut *)self;
	sr_free(s->path);
	for (UINT i = 0; i < TEXT_COUNT; i++)
		sr_free(s->text[i]);
}

static const sr_class link_class = { link_members, shortcut_invoke, shortcut_destroy };
static const sr_class url_class = { url_members, shortcut_invoke, shortcut_destroy };

HRESULT wsh_shortcut_new(const WCHAR *path, VARIANT *result)
{
	UINT length = lstrlenW(path);
	BOOL lnk = length >= 4 && !lstrcmpiW(path + length - 4, L".lnk"), url = length >= 4 && !lstrcmpiW(path + length - 4, L".url");
	if (!lnk && !url)
		return sr_error(5);
	shortcut *s = sr_object_new(url ? &url_class : &link_class, sizeof(shortcut));
	s->url = url;
	DWORD size = GetFullPathNameW(path, 0, NULL, NULL);
	s->path = sr_alloc((size + 1) * sizeof(WCHAR));
	if (!size || !GetFullPathNameW(path, size, s->path, NULL))
		lstrcpyW(s->path, path);
	s->window_style = SW_SHOWNORMAL;
	for (UINT i = 0; i < TEXT_COUNT; i++)
		set_text(s, i, L"");
	if (GetFileAttributesW(s->path) != INVALID_FILE_ATTRIBUTES)
		load(s, s->path);
	return sr_return_object(result, sr_dispatch(s));
}
