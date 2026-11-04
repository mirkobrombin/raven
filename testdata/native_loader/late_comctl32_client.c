// A program that imports neither the common controls nor user32 loads comctl32 later and finds the whole library there.
#include <windows.h>

typedef void *(WINAPI *create_function)(int);
typedef int (WINAPI *insert_function)(void *, int, void *);
typedef void *(WINAPI *get_function)(void *, INT_PTR);
typedef BOOL (WINAPI *destroy_function)(void *);

int raven_entry(void)
{
	if (GetModuleHandleA("comctl32.dll"))
		return 1;
	HMODULE controls = LoadLibraryA("comctl32.dll");
	if (!controls || GetModuleHandleA("comctl32.dll") != controls)
		return 2;
	create_function create = (create_function)GetProcAddress(controls, "DPA_Create");
	insert_function insert = (insert_function)GetProcAddress(controls, "DPA_InsertPtr");
	get_function get = (get_function)GetProcAddress(controls, "DPA_GetPtr");
	destroy_function destroy = (destroy_function)GetProcAddress(controls, "DPA_Destroy");
	if (!create || !insert || !get || !destroy)
		return 3;
	void *list = create(4);
	static int first, second;
	if (!list || insert(list, 0, &second) != 0 || insert(list, 0, &first) != 0 || get(list, 0) != &first || get(list, 1) != &second)
		return 4;
	if (!destroy(list))
		return 5;
	return FreeLibrary(controls) ? 0 : 6;
}
