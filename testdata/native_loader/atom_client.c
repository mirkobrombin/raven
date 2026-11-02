#include <windows.h>

// The decimal form of an integer atom is that integer, and a named atom keeps the case it was added with.
int main(void)
{
	WCHAR name[32];
	if (AddAtomW(L"#215") != 215 || FindAtomW(L"#215") != 215 || GlobalAddAtomW(L"#29295") != 29295)
		return 1;
	if (GetAtomNameW(215, name, 32) != 4 || lstrcmpW(name, L"#215"))
		return 2;
	if (DeleteAtom(215) != 0 || FindAtomW(L"#215") != 215)
		return 3;
	if (AddAtomW(L"#0") || GetLastError() != ERROR_INVALID_PARAMETER || AddAtomW(L"#49152"))
		return 4;
	const ATOM named = AddAtomW(L"#12a");
	if (named < 0xc000 || FindAtomW(L"#12A") != named)
		return 5;
	const ATOM mixed = AddAtomW(L"StandardItems");
	if (mixed < 0xc000 || FindAtomW(L"standarditems") != mixed)
		return 6;
	if (GetAtomNameW(mixed, name, 32) != 13 || lstrcmpW(name, L"StandardItems"))
		return 7;
	if (DeleteAtom(mixed) != 0 || FindAtomW(L"StandardItems"))
		return 8;
	return 0;
}
