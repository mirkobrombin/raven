/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msi.h>
#include <msiquery.h>

/* The installer's command line, as its documentation gives it: /i /x /f /a package, /q levels, /l log, PROPERTY=value.
 * /raven-action is how the engine starts a custom action host. */

UINT WINAPI RavenActionHost(const WCHAR *pipe, const WCHAR *library, const WCHAR *entry);

static const WCHAR *next_argument(const WCHAR *at, WCHAR *out, UINT room)
{
	BOOL quoted = FALSE;
	UINT length = 0;
	while (*at == ' ' || *at == '\t')
		at++;
	for (; *at && (quoted || (*at != ' ' && *at != '\t')); at++) {
		if (*at == '"')
			quoted = !quoted;
		else if (length + 1 < room)
			out[length++] = *at;
	}
	out[length] = 0;
	return at;
}

static WCHAR lower(WCHAR c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static void append(WCHAR **line, UINT *room, const WCHAR *start, const WCHAR *end)
{
	UINT used = *line ? lstrlenW(*line) : 0, size = (UINT)(end - start);
	if (used + size + 2 > *room) {
		*room = (used + size + 2) * 2;
		WCHAR *grown = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *room * sizeof(WCHAR));
		if (*line)
			lstrcpyW(grown, *line);
		HeapFree(GetProcessHeap(), 0, *line);
		*line = grown;
	}
	(*line)[used] = ' ';
	CopyMemory(*line + used + 1, start, size * sizeof(WCHAR));
	(*line)[used + 1 + size] = 0;
}

static BOOL is_guid(const WCHAR *text)
{
	return text[0] == '{' && lstrlenW(text) == 38;
}

// /x and /f take a product code or a package; the package says which product it is.
static UINT product_named_by(const WCHAR *package, WCHAR product[39])
{
	if (is_guid(package)) {
		lstrcpyW(product, package);
		return ERROR_SUCCESS;
	}
	MSIHANDLE handle = 0;
	if (MsiOpenPackageExW(package, MSIOPENPACKAGEFLAGS_IGNOREMACHINESTATE, &handle))
		return ERROR_INSTALL_PACKAGE_OPEN_FAILED;
	DWORD size = 39;
	UINT status = MsiGetProductPropertyW(handle, L"ProductCode", product, &size);
	MsiCloseHandle(handle);
	return status ? ERROR_INSTALL_PACKAGE_INVALID : ERROR_SUCCESS;
}

void WINAPI raven_msiexec(void)
{
	const WCHAR *at = GetCommandLineW();
	static WCHAR argument[32768], package[32768], log_path[32768];
	at = next_argument(at, argument, ARRAYSIZE(argument));
	WCHAR *properties = NULL;
	UINT room = 0;
	WCHAR command = 0;
	BOOL advertise_user = FALSE;
	INSTALLUILEVEL level = INSTALLUILEVEL_FULL;	// without /q the package shows its own dialogs
	while (*at) {
		const WCHAR *start = at;
		while (*start == ' ' || *start == '\t')
			start++;
		at = next_argument(at, argument, ARRAYSIZE(argument));
		if (!argument[0])
			break;
		if (argument[0] != '/' && argument[0] != '-') {
			append(&properties, &room, start, at);
			continue;
		}
		if (!lstrcmpiW(argument, L"/raven-action")) {
			static WCHAR pipe[512], library[32768], entry[512];
			at = next_argument(at, pipe, ARRAYSIZE(pipe));
			at = next_argument(at, library, ARRAYSIZE(library));
			next_argument(at, entry, ARRAYSIZE(entry));
			ExitProcess(RavenActionHost(pipe, library, entry));
		}
		// The long spellings of the same commands.
		if (!lstrcmpiW(argument + 1, L"update"))
			lstrcpyW(argument + 1, L"p");
		else if (!lstrcmpiW(argument + 1, L"package"))
			lstrcpyW(argument + 1, L"i");
		else if (!lstrcmpiW(argument + 1, L"uninstall"))
			lstrcpyW(argument + 1, L"x");
		else if (!lstrcmpiW(argument + 1, L"quiet"))
			lstrcpyW(argument + 1, L"qn");
		else if (!lstrcmpiW(argument + 1, L"passive"))
			lstrcpyW(argument + 1, L"qb");
		else if (!lstrcmpiW(argument + 1, L"norestart") || !lstrcmpiW(argument + 1, L"promptrestart") || !lstrcmpiW(argument + 1, L"forcerestart"))
			continue;
		WCHAR option = lower(argument[1]);
		if (option == 'i' || option == 'x' || option == 'f' || option == 'a' || option == 'j' || option == 'p') {
			command = option;
			// /jm and /ju name the assignment; the package follows.
			if (option == 'j') {
				advertise_user = lower(argument[2]) == 'u';
				at = next_argument(at, package, ARRAYSIZE(package));
			}
			// /x{guid} and /Xpackage may be joined.
			else if (argument[2] && option != 'f')
				lstrcpyW(package, argument + 2);
			else
				at = next_argument(at, package, ARRAYSIZE(package));
		} else if (option == 'q') {
			WCHAR kind = lower(argument[2]);
			level = kind == 'b' ? INSTALLUILEVEL_BASIC : kind == 'r' ? INSTALLUILEVEL_REDUCED : kind == 'f' ? INSTALLUILEVEL_FULL : INSTALLUILEVEL_NONE;
		} else if (option == 'l') {
			at = next_argument(at, log_path, ARRAYSIZE(log_path));
		}
	}
	if (!command)
		ExitProcess(ERROR_INVALID_COMMAND_LINE);
	MsiSetInternalUI(level, NULL);
	if (log_path[0])
		MsiEnableLogW(INSTALLLOGMODE_VERBOSE | INSTALLLOGMODE_ERROR | INSTALLLOGMODE_WARNING | INSTALLLOGMODE_INFO, log_path, 0);
	UINT status;
	const WCHAR *line = properties ? properties : L"";
	if (command == 'x' || command == 'f') {
		WCHAR product[39];
		status = product_named_by(package, product);
		if (!status)
			status = MsiConfigureProductExW(product, INSTALLLEVEL_DEFAULT, command == 'x' ? INSTALLSTATE_ABSENT : INSTALLSTATE_DEFAULT, line);
	}
	else if (command == 'i' && is_guid(package))
		status = MsiConfigureProductExW(package, INSTALLLEVEL_DEFAULT, INSTALLSTATE_DEFAULT, line);
	else if (command == 'j')
		status = MsiAdvertiseProductW(package, advertise_user ? (LPCWSTR)1 : (LPCWSTR)0, NULL, 0);
	else if (command == 'p')
		status = MsiApplyPatchW(package, NULL, INSTALLTYPE_DEFAULT, line);
	else if (command == 'a') {
		WCHAR *admin = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(line) + 16) * sizeof(WCHAR));
		lstrcpyW(admin, L"ACTION=ADMIN ");
		lstrcatW(admin, line);
		status = MsiInstallProductW(package, admin);
		if (status == ERROR_FILE_NOT_FOUND)
			status = ERROR_INSTALL_PACKAGE_OPEN_FAILED;
	} else if (command == 'i') {
		status = MsiInstallProductW(package, line);
		if (status == ERROR_FILE_NOT_FOUND)
			status = ERROR_INSTALL_PACKAGE_OPEN_FAILED;
	} else
		status = ERROR_INVALID_COMMAND_LINE;
	ExitProcess(status);
}
