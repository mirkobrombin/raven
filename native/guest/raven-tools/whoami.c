/* whoami.exe: WHOAMI [/USER] [/ALL] [/FO format] [/NH] — the user this program runs as, as computer\user, and with /USER its security
   identifier. */

#include "tool.h"

typedef BOOL(WINAPI *LookupName)(LPCWSTR, LPCWSTR, PSID, LPDWORD, LPWSTR, LPDWORD, PSID_NAME_USE);
typedef BOOL(WINAPI *SidText)(PSID, LPWSTR *);

static void lower(WCHAR *text)
{
	for (; *text; ++text)
		*text = lowered(*text);
}

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	BOOL user = FALSE, no_header = FALSE;
	const WCHAR *format = L"TABLE";
	for (DWORD index = 0; index < arguments.count; ++index) {
		const WCHAR *argument = arguments.items[index];
		if (switch_is(argument, L"/USER") || switch_is(argument, L"/ALL"))
			user = TRUE;
		else if (switch_is(argument, L"/NH"))
			no_header = TRUE;
		else if (switch_is(argument, L"/FO") && index + 1 < arguments.count)
			format = arguments.items[++index];
		else if (switch_is(argument, L"/?")) {
			say(L"WHOAMI [/USER] [/ALL] [/FO format] [/NH]\r\n\r\n"
				L"Description:\r\n    This utility can be used to get user name and\r\n    security identifier (SID) of the current user.\r\n");
			ExitProcess(0);
		} else {
			complain(L"ERROR: Invalid argument/option - '");
			complain(argument);
			complain(L"'.\r\nType \"WHOAMI /?\" for usage.\r\n");
			ExitProcess(1);
		}
	}
	WCHAR computer[256], name[256];
	DWORD size = 256;
	if (!GetComputerNameW(computer, &size))
		lstrcpynW(computer, L"localhost", 256);
	size = 256;
	if (!GetUserNameW(name, &size))
		lstrcpynW(name, L"user", 256);
	struct Text account = {0};
	text_put(&account, computer);
	text_char(&account, L'\\');
	text_put(&account, name);
	lower(account.data);
	if (!user) {
		say(account.data);
		say(L"\r\n");
		ExitProcess(0);
	}
	/* The SID from the account's name, through advapi32, found as it is needed. */
	struct Text sid_text = {0};
	HMODULE advapi = LoadLibraryW(L"advapi32.dll");
	LookupName lookup = advapi ? (LookupName)(void (*)(void))GetProcAddress(advapi, "LookupAccountNameW") : NULL;
	SidText to_text = advapi ? (SidText)(void (*)(void))GetProcAddress(advapi, "ConvertSidToStringSidW") : NULL;
	BYTE sid[256];
	WCHAR domain[256];
	DWORD sid_size = sizeof sid, domain_size = 256;
	SID_NAME_USE use;
	WCHAR *converted = NULL;
	if (lookup && to_text && lookup(NULL, name, sid, &sid_size, domain, &domain_size, &use) && to_text(sid, &converted) && converted)
		text_put(&sid_text, converted);
	else
		text_put(&sid_text, L"S-1-5-21-0-0-0-1000");
	struct Text out = {0};
	if (equal(format, L"CSV")) {
		if (!no_header)
			text_put(&out, L"\"User Name\",\"SID\"\r\n");
		text_char(&out, L'"');
		text_put(&out, account.data);
		text_put(&out, L"\",\"");
		text_put(&out, sid_text.data);
		text_put(&out, L"\"\r\n");
	} else if (equal(format, L"LIST")) {
		text_put(&out, L"\r\nUSER INFORMATION\r\n----------------\r\n\r\nUser Name: ");
		text_put(&out, account.data);
		text_put(&out, L"\r\nSID:       ");
		text_put(&out, sid_text.data);
		text_put(&out, L"\r\n");
	} else {
		DWORD name_width = account.size > 9 ? account.size : 9, sid_width = sid_text.size > 3 ? sid_text.size : 3;
		text_put(&out, L"\r\nUSER INFORMATION\r\n----------------\r\n\r\n");
		if (!no_header) {
			put_field(&out, L"User Name", name_width + 1, TRUE);
			text_put(&out, L"SID\r\n");
			for (DWORD index = 0; index < name_width; ++index)
				text_char(&out, L'=');
			text_char(&out, L' ');
			for (DWORD index = 0; index < sid_width; ++index)
				text_char(&out, L'=');
			text_put(&out, L"\r\n");
		}
		put_field(&out, account.data, name_width + 1, TRUE);
		text_put(&out, sid_text.data);
		text_put(&out, L"\r\n");
	}
	say_text(&out);
	ExitProcess(0);
	return 0;
}
