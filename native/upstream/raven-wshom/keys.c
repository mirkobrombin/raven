/* SPDX-License-Identifier: MIT */
#include "wshom.h"

/* SendKeys: characters stand for themselves; + ^ and % hold Shift, Ctrl and Alt down for the key after them or for a group in
 * parentheses; ~ is Enter; braces name a key ({TAB}, {F5}), repeat one ({LEFT 3}) or stand for a character that is otherwise special
 * ({+}, {{}). The keys go to the window with the focus as SendInput types them. */

typedef struct {
	INPUT *inputs;
	UINT count, room;
} typing;

static void add(typing *t, WORD key, WCHAR character, BOOL up)
{
	if (t->count == t->room) {
		t->room = t->room ? t->room * 2 : 32;
		t->inputs = sr_grow(t->inputs, t->room * sizeof(INPUT));
	}
	INPUT *input = &t->inputs[t->count++];
	ZeroMemory(input, sizeof(*input));
	input->type = INPUT_KEYBOARD;
	input->ki.wVk = key;
	input->ki.wScan = character;
	input->ki.dwFlags = (character ? KEYEVENTF_UNICODE : 0) | (up ? KEYEVENTF_KEYUP : 0);
}

static void press(typing *t, WORD key)
{
	add(t, key, 0, FALSE);
	add(t, key, 0, TRUE);
}

/* A character through the key that types it on the keyboard layout, with Shift, Ctrl or Alt as it needs, or as itself. */
static void type_character(typing *t, WCHAR c)
{
	SHORT scan = VkKeyScanW(c);
	if (scan == -1) {
		add(t, 0, c, FALSE);
		add(t, 0, c, TRUE);
		return;
	}
	BYTE shifts = HIBYTE(scan);
	if (shifts & 1)
		add(t, VK_SHIFT, 0, FALSE);
	if (shifts & 2)
		add(t, VK_CONTROL, 0, FALSE);
	if (shifts & 4)
		add(t, VK_MENU, 0, FALSE);
	press(t, LOBYTE(scan));
	if (shifts & 4)
		add(t, VK_MENU, 0, TRUE);
	if (shifts & 2)
		add(t, VK_CONTROL, 0, TRUE);
	if (shifts & 1)
		add(t, VK_SHIFT, 0, TRUE);
}

static const struct {
	const WCHAR *name;
	WORD key;
} named[] = {
	{ L"BACKSPACE", VK_BACK }, { L"BS", VK_BACK }, { L"BKSP", VK_BACK }, { L"BREAK", VK_CANCEL }, { L"CAPSLOCK", VK_CAPITAL },
	{ L"DELETE", VK_DELETE }, { L"DEL", VK_DELETE }, { L"DOWN", VK_DOWN }, { L"END", VK_END }, { L"ENTER", VK_RETURN }, { L"ESC", VK_ESCAPE },
	{ L"HELP", VK_HELP }, { L"HOME", VK_HOME }, { L"INSERT", VK_INSERT }, { L"INS", VK_INSERT }, { L"LEFT", VK_LEFT },
	{ L"NUMLOCK", VK_NUMLOCK }, { L"PGDN", VK_NEXT }, { L"PGUP", VK_PRIOR }, { L"PRTSC", VK_SNAPSHOT }, { L"RIGHT", VK_RIGHT },
	{ L"SCROLLLOCK", VK_SCROLL }, { L"TAB", VK_TAB }, { L"UP", VK_UP }, { L"ADD", VK_ADD }, { L"SUBTRACT", VK_SUBTRACT },
	{ L"MULTIPLY", VK_MULTIPLY }, { L"DIVIDE", VK_DIVIDE },
};

/* The key a brace names: a name above, F1 to F16, or a single character. */
static BOOL key_of(const WCHAR *name, UINT length, WORD *key, WCHAR *character)
{
	*key = 0;
	*character = 0;
	if (length == 1) {
		*character = name[0];
		return TRUE;
	}
	for (UINT i = 0; i < ARRAYSIZE(named); i++)
		if ((UINT)lstrlenW(named[i].name) == length &&
			CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, named[i].name, -1, name, (int)length) == CSTR_EQUAL) {
			*key = named[i].key;
			return TRUE;
		}
	if ((name[0] == L'F' || name[0] == L'f') && length <= 3) {
		UINT number = 0;
		for (UINT i = 1; i < length; i++) {
			if (name[i] < L'0' || name[i] > L'9')
				return FALSE;
			number = number * 10 + (name[i] - L'0');
		}
		if (number >= 1 && number <= 16) {
			*key = (WORD)(VK_F1 + number - 1);
			return TRUE;
		}
	}
	return FALSE;
}

/* Parses keys from *at up to the end or, inside a group, its closing parenthesis. */
static BOOL parse(typing *t, const WCHAR **at, BOOL group)
{
	BYTE held = 0;
	while (**at) {
		WCHAR c = *(*at)++;
		switch (c) {
		case L'+':
			held |= 1;
			continue;
		case L'^':
			held |= 2;
			continue;
		case L'%':
			held |= 4;
			continue;
		case L')':
			if (!group)
				return FALSE;
			return held == 0;
		}
		if (held & 1)
			add(t, VK_SHIFT, 0, FALSE);
		if (held & 2)
			add(t, VK_CONTROL, 0, FALSE);
		if (held & 4)
			add(t, VK_MENU, 0, FALSE);
		if (c == L'(') {
			if (!parse(t, at, TRUE))
				return FALSE;
		} else if (c == L'~')
			press(t, VK_RETURN);
		else if (c == L'{') {
			/* The name runs to the first closing brace after its first character, so {}} is the brace itself. */
			const WCHAR *name = *at, *end = name + (*name ? 1 : 0);
			while (*end && *end != L'}')
				end++;
			if (*end != L'}')
				return FALSE;
			const WCHAR *space = name + 1;
			while (space < end && *space != L' ')
				space++;
			UINT repeat = 1;
			if (space < end) {
				repeat = 0;
				for (const WCHAR *d = space + 1; d < end; d++) {
					if (*d < L'0' || *d > L'9')
						return FALSE;
					repeat = repeat * 10 + (*d - L'0');
				}
			}
			WORD key;
			WCHAR character;
			if (!key_of(name, (UINT)(space - name), &key, &character))
				return FALSE;
			for (UINT i = 0; i < repeat; i++)
				if (key)
					press(t, key);
				else
					type_character(t, character);
			*at = end + 1;
		} else if (c == L'}')
			return FALSE;
		else
			type_character(t, c);
		if (held & 4)
			add(t, VK_MENU, 0, TRUE);
		if (held & 2)
			add(t, VK_CONTROL, 0, TRUE);
		if (held & 1)
			add(t, VK_SHIFT, 0, TRUE);
		held = 0;
	}
	return !group && !held;
}

HRESULT wsh_send_keys(const WCHAR *keys)
{
	typing t = { 0 };
	const WCHAR *at = keys;
	HRESULT hr = S_OK;
	if (!parse(&t, &at, FALSE))
		hr = sr_error(5);
	else if (t.count && SendInput(t.count, t.inputs, sizeof(INPUT)) != t.count)
		hr = sr_error(70);
	sr_free(t.inputs);
	return hr;
}
