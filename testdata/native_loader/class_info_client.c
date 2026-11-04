// GetClassInfoA and GetClassInfoExA describe a class registered through RegisterClassExA, found by name or by atom.
#include <windows.h>

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefWindowProcA(window, message, wparam, lparam);
}

int raven_entry(void)
{
	HINSTANCE instance = GetModuleHandleA(NULL);
	WNDCLASSEXA definition = {sizeof(definition)};
	definition.style = CS_HREDRAW | CS_DBLCLKS;
	definition.lpfnWndProc = procedure;
	definition.cbClsExtra = 8;
	definition.cbWndExtra = 16;
	definition.hInstance = instance;
	definition.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	definition.lpszClassName = "RavenClassInfo";
	const ATOM atom = RegisterClassExA(&definition);
	if (!atom)
		return 1;

	WNDCLASSA plain;
	if (GetClassInfoA(instance, "RavenClassInfo", &plain) != atom)
		return 2;
	if (plain.style != definition.style || plain.lpfnWndProc != procedure || plain.cbClsExtra != 8 || plain.cbWndExtra != 16 ||
		plain.hInstance != instance || plain.hbrBackground != definition.hbrBackground)
		return 3;

	WNDCLASSEXA extended = {sizeof(extended)};
	if (GetClassInfoExA(instance, "RavenClassInfo", &extended) != atom)
		return 4;
	if (extended.cbSize != sizeof(extended) || extended.style != definition.style || extended.lpfnWndProc != procedure ||
		extended.cbClsExtra != 8 || extended.cbWndExtra != 16 || extended.hbrBackground != definition.hbrBackground)
		return 5;

	WNDCLASSEXA by_atom = {sizeof(by_atom)};
	if (GetClassInfoExA(instance, MAKEINTATOM(atom), &by_atom) != atom || by_atom.lpfnWndProc != procedure)
		return 6;

	// A class nobody registered is reported as such.
	if (GetClassInfoA(instance, "RavenNoSuchClass", &plain) || GetLastError() != ERROR_CLASS_DOES_NOT_EXIST)
		return 7;
	return 0;
}
