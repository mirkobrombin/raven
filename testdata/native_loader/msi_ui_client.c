#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msi.h>
#include <msiquery.h>

/* A package with its own dialogs, installed at the full user interface level as msiexec runs it without /q. Another thread plays the
   user: it cancels the wizard once, which ends the installation without the product, then goes through it, typing a name, ticking a
   check box that enables the Install button, picking a radio button, going back and forth, and installs; what it chose is what the
   package writes to the registry. */

static const WCHAR product[] = L"{7B1E3C52-9A4D-4F60-8C2E-1D5A6B7C8D90}";

static void beside(WCHAR *target, const WCHAR *name)
{
	unsigned cut = GetModuleFileNameW(NULL, target, MAX_PATH);
	while (cut && target[cut - 1] != L'\\')
		--cut;
	for (unsigned i = 0; name[i]; ++i)
		target[cut++] = name[i];
	target[cut] = 0;
}

static BOOL run_sql(MSIHANDLE database, const WCHAR *query)
{
	MSIHANDLE view = 0;
	BOOL done = !MsiDatabaseOpenViewW(database, query, &view) && !MsiViewExecute(view, 0);
	if (view)
		MsiCloseHandle(view);
	return done;
}

static const WCHAR *const tables[] = {
	L"CREATE TABLE `Dialog` (`Dialog` CHAR(72) NOT NULL, `HCentering` SHORT NOT NULL, `VCentering` SHORT NOT NULL, `Width` SHORT NOT NULL, "
	L"`Height` SHORT NOT NULL, `Attributes` LONG, `Title` CHAR(128) LOCALIZABLE, `Control_First` CHAR(50) NOT NULL, `Control_Default` CHAR(50), "
	L"`Control_Cancel` CHAR(50) PRIMARY KEY `Dialog`)",
	L"CREATE TABLE `Control` (`Dialog_` CHAR(72) NOT NULL, `Control` CHAR(50) NOT NULL, `Type` CHAR(20) NOT NULL, `X` SHORT NOT NULL, "
	L"`Y` SHORT NOT NULL, `Width` SHORT NOT NULL, `Height` SHORT NOT NULL, `Attributes` LONG, `Property` CHAR(72), `Text` LONGCHAR LOCALIZABLE, "
	L"`Control_Next` CHAR(50), `Help` CHAR(255) LOCALIZABLE PRIMARY KEY `Dialog_`, `Control`)",
	L"CREATE TABLE `ControlEvent` (`Dialog_` CHAR(72) NOT NULL, `Control_` CHAR(50) NOT NULL, `Event` CHAR(50) NOT NULL, `Argument` CHAR(255) NOT NULL, "
	L"`Condition` CHAR(255), `Ordering` SHORT PRIMARY KEY `Dialog_`, `Control_`, `Event`, `Argument`, `Condition`)",
	L"CREATE TABLE `ControlCondition` (`Dialog_` CHAR(72) NOT NULL, `Control_` CHAR(50) NOT NULL, `Action` CHAR(50) NOT NULL, "
	L"`Condition` CHAR(255) NOT NULL PRIMARY KEY `Dialog_`, `Control_`, `Action`, `Condition`)",
	L"CREATE TABLE `EventMapping` (`Dialog_` CHAR(72) NOT NULL, `Control_` CHAR(50) NOT NULL, `Event` CHAR(50) NOT NULL, "
	L"`Attribute` CHAR(50) NOT NULL PRIMARY KEY `Dialog_`, `Control_`, `Event`)",
	L"CREATE TABLE `RadioButton` (`Property` CHAR(72) NOT NULL, `Order` SHORT NOT NULL, `Value` CHAR(64) NOT NULL, `X` SHORT NOT NULL, "
	L"`Y` SHORT NOT NULL, `Width` SHORT NOT NULL, `Height` SHORT NOT NULL, `Text` CHAR(64) LOCALIZABLE, `Help` CHAR(50) LOCALIZABLE PRIMARY KEY `Property`, `Order`)",
	L"CREATE TABLE `CheckBox` (`Property` CHAR(72) NOT NULL, `Value` CHAR(64) PRIMARY KEY `Property`)",
	L"CREATE TABLE `TextStyle` (`TextStyle` CHAR(72) NOT NULL, `FaceName` CHAR(32) NOT NULL, `Size` SHORT NOT NULL, `Color` LONG, `StyleBits` SHORT "
	L"PRIMARY KEY `TextStyle`)",
};

static const WCHAR *const rows[] = {
	L"INSERT INTO `TextStyle` (`TextStyle`, `FaceName`, `Size`) VALUES ('DefaultFont', 'Tahoma', 8)",
	L"INSERT INTO `TextStyle` (`TextStyle`, `FaceName`, `Size`, `Color`, `StyleBits`) VALUES ('TitleFont', 'Tahoma', 12, 8388608, 1)",
	L"INSERT INTO `Property` (`Property`, `Value`) VALUES ('DefaultUIFont', 'DefaultFont')",
	L"INSERT INTO `Property` (`Property`, `Value`) VALUES ('MODE', 'full')",
	L"INSERT INTO `Dialog` (`Dialog`, `HCentering`, `VCentering`, `Width`, `Height`, `Attributes`, `Title`, `Control_First`, `Control_Default`, `Control_Cancel`) "
	L"VALUES ('WelcomeDlg', 50, 50, 370, 270, 3, 'Welcome', 'Next', 'Next', 'Cancel')",
	L"INSERT INTO `Dialog` (`Dialog`, `HCentering`, `VCentering`, `Width`, `Height`, `Attributes`, `Title`, `Control_First`, `Control_Default`, `Control_Cancel`) "
	L"VALUES ('OptionsDlg', 50, 50, 370, 270, 3, 'Options', 'Name', 'Install', 'Cancel')",
	L"INSERT INTO `Dialog` (`Dialog`, `HCentering`, `VCentering`, `Width`, `Height`, `Attributes`, `Title`, `Control_First`, `Control_Default`, `Control_Cancel`) "
	L"VALUES ('CancelDlg', 50, 50, 260, 85, 3, 'Cancel?', 'No', 'No', 'No')",
	L"INSERT INTO `Dialog` (`Dialog`, `HCentering`, `VCentering`, `Width`, `Height`, `Attributes`, `Title`, `Control_First`) "
	L"VALUES ('ProgressDlg', 50, 50, 370, 270, 1, 'Installing', 'Bar')",
	L"INSERT INTO `Dialog` (`Dialog`, `HCentering`, `VCentering`, `Width`, `Height`, `Attributes`, `Title`, `Control_First`, `Control_Default`, `Control_Cancel`) "
	L"VALUES ('DoneDlg', 50, 50, 370, 270, 3, 'Done', 'Finish', 'Finish', 'Finish')",
	L"INSERT INTO `Dialog` (`Dialog`, `HCentering`, `VCentering`, `Width`, `Height`, `Attributes`, `Title`, `Control_First`, `Control_Default`, `Control_Cancel`) "
	L"VALUES ('StoppedDlg', 50, 50, 370, 270, 3, 'Stopped', 'Finish', 'Finish', 'Finish')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`) "
	L"VALUES ('WelcomeDlg', 'Banner', 'Bitmap', 0, 0, 370, 234, 1, 'Banner')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`) "
	L"VALUES ('WelcomeDlg', 'Title', 'Text', 135, 20, 220, 60, 196611, '{\\TitleFont}Welcome to [ProductName]')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`, `Control_Next`) "
	L"VALUES ('WelcomeDlg', 'Next', 'PushButton', 236, 243, 56, 17, 3, 'Next', 'Cancel')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`, `Control_Next`) "
	L"VALUES ('WelcomeDlg', 'Cancel', 'PushButton', 304, 243, 56, 17, 3, 'Cancel', 'Next')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Property`, `Text`, `Control_Next`) "
	L"VALUES ('OptionsDlg', 'Name', 'Edit', 20, 60, 200, 18, 7, 'RAVENUSER', '{40}', 'Accept')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Property`, `Text`, `Control_Next`) "
	L"VALUES ('OptionsDlg', 'Accept', 'CheckBox', 20, 90, 200, 18, 3, 'ACCEPT', 'I accept', 'Mode')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Property`, `Control_Next`) "
	L"VALUES ('OptionsDlg', 'Mode', 'RadioButtonGroup', 20, 120, 200, 40, 3, 'MODE', 'License')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`, `Control_Next`) "
	L"VALUES ('OptionsDlg', 'License', 'ScrollableText', 20, 170, 330, 60, 3, '{\\rtf1\\ansi Terms of use}', 'Back')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`, `Control_Next`) "
	L"VALUES ('OptionsDlg', 'Back', 'PushButton', 180, 243, 56, 17, 3, 'Back', 'Install')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`, `Control_Next`) "
	L"VALUES ('OptionsDlg', 'Install', 'PushButton', 236, 243, 56, 17, 1, 'Install', 'Cancel')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`, `Control_Next`) "
	L"VALUES ('OptionsDlg', 'Cancel', 'PushButton', 304, 243, 56, 17, 3, 'Cancel', 'Name')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`, `Control_Next`) "
	L"VALUES ('CancelDlg', 'Yes', 'PushButton', 72, 57, 56, 17, 3, 'Yes', 'No')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`, `Control_Next`) "
	L"VALUES ('CancelDlg', 'No', 'PushButton', 132, 57, 56, 17, 3, 'No', 'Yes')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Control_Next`) "
	L"VALUES ('ProgressDlg', 'Bar', 'ProgressBar', 35, 115, 300, 10, 65537, 'Status')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Control_Next`) "
	L"VALUES ('ProgressDlg', 'Status', 'Text', 35, 100, 300, 12, 3, 'Bar')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`) "
	L"VALUES ('DoneDlg', 'Thanks', 'Text', 20, 60, 330, 20, 3, 'Thanks, [RAVENUSER]')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`) "
	L"VALUES ('DoneDlg', 'Finish', 'PushButton', 304, 243, 56, 17, 3, 'Finish')",
	L"INSERT INTO `Control` (`Dialog_`, `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Text`) "
	L"VALUES ('StoppedDlg', 'Finish', 'PushButton', 304, 243, 56, 17, 3, 'Finish')",
	L"INSERT INTO `ControlEvent` (`Dialog_`, `Control_`, `Event`, `Argument`, `Condition`, `Ordering`) VALUES ('WelcomeDlg', 'Next', 'NewDialog', 'OptionsDlg', '1', 1)",
	L"INSERT INTO `ControlEvent` (`Dialog_`, `Control_`, `Event`, `Argument`, `Condition`, `Ordering`) VALUES ('WelcomeDlg', 'Cancel', 'SpawnDialog', 'CancelDlg', '1', 1)",
	L"INSERT INTO `ControlEvent` (`Dialog_`, `Control_`, `Event`, `Argument`, `Condition`, `Ordering`) VALUES ('OptionsDlg', 'Back', 'NewDialog', 'WelcomeDlg', '1', 1)",
	L"INSERT INTO `ControlEvent` (`Dialog_`, `Control_`, `Event`, `Argument`, `Condition`, `Ordering`) VALUES ('OptionsDlg', 'Install', '[CHOSEN]', '[MODE]', '1', 1)",
	L"INSERT INTO `ControlEvent` (`Dialog_`, `Control_`, `Event`, `Argument`, `Condition`, `Ordering`) VALUES ('OptionsDlg', 'Install', 'EndDialog', 'Return', '1', 2)",
	L"INSERT INTO `ControlEvent` (`Dialog_`, `Control_`, `Event`, `Argument`, `Condition`, `Ordering`) VALUES ('OptionsDlg', 'Cancel', 'SpawnDialog', 'CancelDlg', '1', 1)",
	L"INSERT INTO `ControlEvent` (`Dialog_`, `Control_`, `Event`, `Argument`, `Condition`, `Ordering`) VALUES ('CancelDlg', 'Yes', 'EndDialog', 'Exit', '1', 1)",
	L"INSERT INTO `ControlEvent` (`Dialog_`, `Control_`, `Event`, `Argument`, `Condition`, `Ordering`) VALUES ('CancelDlg', 'No', 'EndDialog', 'Return', '1', 1)",
	L"INSERT INTO `ControlEvent` (`Dialog_`, `Control_`, `Event`, `Argument`, `Condition`, `Ordering`) VALUES ('DoneDlg', 'Finish', 'EndDialog', 'Return', '1', 1)",
	L"INSERT INTO `ControlEvent` (`Dialog_`, `Control_`, `Event`, `Argument`, `Condition`, `Ordering`) VALUES ('StoppedDlg', 'Finish', 'EndDialog', 'Return', '1', 1)",
	L"INSERT INTO `ControlCondition` (`Dialog_`, `Control_`, `Action`, `Condition`) VALUES ('OptionsDlg', 'Install', 'Enable', 'ACCEPT = \"yes\"')",
	L"INSERT INTO `ControlCondition` (`Dialog_`, `Control_`, `Action`, `Condition`) VALUES ('OptionsDlg', 'Install', 'Disable', 'NOT ACCEPT')",
	L"INSERT INTO `EventMapping` (`Dialog_`, `Control_`, `Event`, `Attribute`) VALUES ('ProgressDlg', 'Bar', 'SetProgress', 'Progress')",
	L"INSERT INTO `EventMapping` (`Dialog_`, `Control_`, `Event`, `Attribute`) VALUES ('ProgressDlg', 'Status', 'ActionText', 'Text')",
	L"INSERT INTO `RadioButton` (`Property`, `Order`, `Value`, `X`, `Y`, `Width`, `Height`, `Text`) VALUES ('MODE', 1, 'full', 0, 0, 100, 15, 'Full')",
	L"INSERT INTO `RadioButton` (`Property`, `Order`, `Value`, `X`, `Y`, `Width`, `Height`, `Text`) VALUES ('MODE', 2, 'portable', 0, 20, 100, 15, 'Portable')",
	L"INSERT INTO `CheckBox` (`Property`, `Value`) VALUES ('ACCEPT', 'yes')",
	L"INSERT INTO `InstallUISequence` (`Action`, `Sequence`) VALUES ('WelcomeDlg', 1100)",
	L"INSERT INTO `InstallUISequence` (`Action`, `Sequence`) VALUES ('ProgressDlg', 1200)",
	L"INSERT INTO `InstallUISequence` (`Action`, `Sequence`) VALUES ('DoneDlg', -1)",
	L"INSERT INTO `InstallUISequence` (`Action`, `Sequence`) VALUES ('StoppedDlg', -2)",
	L"INSERT INTO `Registry` (`Registry`, `Root`, `Key`, `Name`, `Value`, `Component_`) VALUES ('uiname', 2, 'Software\\RavenSample', 'Name', '[RAVENUSER]', 'Payload')",
	L"INSERT INTO `Registry` (`Registry`, `Root`, `Key`, `Name`, `Value`, `Component_`) VALUES ('uimode', 2, 'Software\\RavenSample', 'Mode', '[CHOSEN]', 'Payload')",
};

// The banner: a two by two bitmap file, kept in the Binary table.
static BOOL banner_add(MSIHANDLE database, const WCHAR *path)
{
	static const BYTE file[70] = { 'B', 'M', 70, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0,
		16, 0, 0, 0, 0x13, 0x0b, 0, 0, 0x13, 0x0b, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255, 0, 0 };
	HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD put = 0;
	if (out == INVALID_HANDLE_VALUE || !WriteFile(out, file, sizeof file, &put, NULL))
		return FALSE;
	CloseHandle(out);
	MSIHANDLE view = 0, record = MsiCreateRecord(2);
	BOOL done = !MsiDatabaseOpenViewW(database, L"INSERT INTO `Binary` (`Name`, `Data`) VALUES (?, ?)", &view) &&
		!MsiRecordSetStringW(record, 1, L"Banner") && !MsiRecordSetStreamW(record, 2, path) && !MsiViewExecute(view, record);
	MsiCloseHandle(record);
	if (view)
		MsiCloseHandle(view);
	return done;
}

static DWORD WINAPI install(void *path)
{
	return MsiInstallProductW(path, NULL);
}

static HWND wait_dialog(const WCHAR *title)
{
	for (int i = 0; i < 500; i++) {
		HWND window = FindWindowW(L"RavenMsiDialog", title);
		if (window && IsWindowVisible(window))
			return window;
		Sleep(20);
	}
	return NULL;
}

static BOOL wait_gone(HWND window)
{
	for (int i = 0; i < 500 && IsWindow(window); i++)
		Sleep(20);
	return !IsWindow(window);
}

static BOOL click(HWND dialog, HWND control)
{
	return control && PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(control), BN_CLICKED), (LPARAM)control);
}

static BOOL press(HWND dialog, const WCHAR *text)
{
	HWND button = dialog ? FindWindowExW(dialog, NULL, L"Button", text) : NULL;
	return click(dialog, button) && wait_gone(dialog);
}

static BOOL enabled_soon(HWND control, BOOL wanted)
{
	for (int i = 0; i < 100; i++) {
		if (!IsWindowEnabled(control) == !wanted)
			return TRUE;
		Sleep(20);
	}
	return FALSE;
}

static DWORD finish(HANDLE thread)
{
	DWORD code = 0xffffffff;
	if (WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0)
		GetExitCodeThread(thread, &code);
	CloseHandle(thread);
	return code;
}

static BOOL value_is(HKEY key, const WCHAR *name, const WCHAR *expected)
{
	WCHAR text[64];
	DWORD size = sizeof text, type = 0;
	return !RegQueryValueExW(key, name, NULL, &type, (BYTE *)text, &size) && type == REG_SZ && !lstrcmpW(text, expected);
}

int raven_entry(void)
{
	WCHAR base[MAX_PATH], package[MAX_PATH], banner[MAX_PATH];
	beside(base, L"basic.msi");
	beside(package, L"dialogs.msi");
	beside(banner, L"banner.bmp");
	MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);
	if (MsiQueryProductStateW(product) != INSTALLSTATE_UNKNOWN)
		MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT);
	if (!CopyFileW(base, package, FALSE))
		return 1;
	SetFileAttributesW(package, FILE_ATTRIBUTE_NORMAL);
	MSIHANDLE database = 0;
	if (MsiOpenDatabaseW(package, (LPCWSTR)MSIDBOPEN_TRANSACT, &database))
		return 2;
	for (unsigned i = 0; i < ARRAYSIZE(tables); i++)
		if (!run_sql(database, tables[i]))
			return 10 + i;
	for (unsigned i = 0; i < ARRAYSIZE(rows); i++)
		if (!run_sql(database, rows[i]))
			return 100 + i;
	if (!banner_add(database, banner) || MsiDatabaseCommit(database))
		return 3;
	MsiCloseHandle(database);
	MsiSetInternalUI(INSTALLUILEVEL_FULL, NULL);

	/* Cancelled from the first page: the confirmation, then the page the package shows when the user stopped it. */
	HANDLE thread = CreateThread(NULL, 0, install, package, 0, NULL);
	HWND welcome = wait_dialog(L"Welcome");
	if (!welcome || !FindWindowExW(welcome, NULL, L"Static", L"Welcome to Raven Sample"))
		return 4;
	if (!click(welcome, FindWindowExW(welcome, NULL, L"Button", L"Cancel")))
		return 5;
	HWND confirm = wait_dialog(L"Cancel?");
	if (!confirm || !press(confirm, L"Yes"))
		return 6;
	if (!wait_gone(welcome))
		return 7;
	HWND stopped = wait_dialog(L"Stopped");
	if (!stopped || !press(stopped, L"Finish"))
		return 8;
	if (finish(thread) != ERROR_INSTALL_USEREXIT || MsiQueryProductStateW(product) != INSTALLSTATE_UNKNOWN)
		return 9;

	/* Through the wizard: Install waits for the check box, the edit and the radio buttons set their properties, and they survive a
	   step back. */
	thread = CreateThread(NULL, 0, install, package, 0, NULL);
	welcome = wait_dialog(L"Welcome");
	if (!welcome || !press(welcome, L"Next"))
		return 20;
	HWND options = wait_dialog(L"Options");
	HWND name = options ? FindWindowExW(options, NULL, L"Edit", NULL) : NULL;
	HWND accept = options ? FindWindowExW(options, NULL, L"Button", L"I accept") : NULL;
	HWND portable = options ? FindWindowExW(options, NULL, L"Button", L"Portable") : NULL;
	HWND go = options ? FindWindowExW(options, NULL, L"Button", L"Install") : NULL;
	if (!name || !accept || !portable || !go)
		return 21;
	if (IsWindowEnabled(go) || SendMessageW(FindWindowExW(options, NULL, L"Button", L"Full"), BM_GETCHECK, 0, 0) != BST_CHECKED)
		return 22;
	SendMessageW(name, WM_SETTEXT, 0, (LPARAM)L"Mirko");
	SendMessageW(accept, BM_SETCHECK, BST_CHECKED, 0);
	click(options, accept);
	if (!enabled_soon(go, TRUE))
		return 23;
	SendMessageW(portable, BM_SETCHECK, BST_CHECKED, 0);
	click(options, portable);
	if (!press(options, L"Back"))
		return 24;
	welcome = wait_dialog(L"Welcome");
	if (!welcome || !press(welcome, L"Next"))
		return 25;
	options = wait_dialog(L"Options");
	WCHAR typed[16] = {0};
	name = options ? FindWindowExW(options, NULL, L"Edit", NULL) : NULL;
	go = options ? FindWindowExW(options, NULL, L"Button", L"Install") : NULL;
	if (!name || !GetWindowTextW(name, typed, ARRAYSIZE(typed)) || lstrcmpW(typed, L"Mirko"))
		return 26;
	if (!go || !IsWindowEnabled(go) || SendMessageW(FindWindowExW(options, NULL, L"Button", L"Portable"), BM_GETCHECK, 0, 0) != BST_CHECKED)
		return 27;
	if (!press(options, L"Install"))
		return 28;
	HWND done = wait_dialog(L"Done");
	if (!done || !FindWindowExW(done, NULL, L"Static", L"Thanks, Mirko"))
		return 29;
	if (FindWindowW(L"RavenMsiDialog", L"Installing"))
		return 30;
	if (!press(done, L"Finish"))
		return 31;
	if (finish(thread) != ERROR_SUCCESS || MsiQueryProductStateW(product) != INSTALLSTATE_DEFAULT)
		return 32;
	HKEY key;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\RavenSample", 0, KEY_READ, &key))
		return 33;
	const BOOL written = value_is(key, L"Name", L"Mirko") && value_is(key, L"Mode", L"portable");
	RegCloseKey(key);
	if (!written)
		return 34;

	MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);
	if (MsiConfigureProductW(product, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT) || MsiQueryProductStateW(product) != INSTALLSTATE_UNKNOWN)
		return 35;
	DeleteFileW(package);
	DeleteFileW(banner);
	return 0;
}
