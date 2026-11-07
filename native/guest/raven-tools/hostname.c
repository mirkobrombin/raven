/* hostname.exe: HOSTNAME — the name of this computer. */

#include "tool.h"

int raven_tool_entry(void)
{
	struct Arguments arguments;
	start_tool();
	read_arguments(&arguments);
	if (arguments.count) {
		if (switch_is(arguments.items[0], L"/?")) {
			say(L"Prints the name of the current host.\r\n\r\nhostname\r\n");
			ExitProcess(0);
		}
		complain(L"sethostname: Use the Network Control Panel Applet to set hostname.\r\nhostname -s is not supported.\r\n");
		ExitProcess(1);
	}
	WCHAR name[256];
	DWORD size = 256;
	/* The DNS host name keeps its case; the older call's name is the one Windows gives in capitals. */
	typedef BOOL(WINAPI * NameEx)(COMPUTER_NAME_FORMAT, LPWSTR, LPDWORD);
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	NameEx name_ex = kernel ? (NameEx)(void (*)(void))GetProcAddress(kernel, "GetComputerNameExW") : NULL;
	if (!name_ex || !name_ex(ComputerNameDnsHostname, name, &size)) {
		size = 256;
		if (!GetComputerNameW(name, &size))
			ExitProcess(1);
	}
	say(name);
	say(L"\r\n");
	ExitProcess(0);
	return 0;
}
