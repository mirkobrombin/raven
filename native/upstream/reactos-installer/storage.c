/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windef.h>
#include <winbase.h>
#include <winreg.h>

/* The structured storage sources open class keys through this compobj helper; the registry answers it directly. */
LSTATUS open_classes_key(HKEY key, const WCHAR *name, REGSAM access, HKEY *result)
{
	return RegOpenKeyExW(key, name, 0, access, result);
}
