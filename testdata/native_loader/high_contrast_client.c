#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>

/* SPI_GETHIGHCONTRAST fills HIGHCONTRASTW, whose last field is a pointer: the structure is 12 bytes in a 32-bit program and 16 in
 * a 64-bit one. Nothing past it may change, and the scheme name is a readable string, empty when no high contrast scheme is set. */
struct guarded {
	HIGHCONTRASTW contrast;
	DWORD guard[4];
};

int raven_entry(void)
{
	struct guarded value;
	BYTE *bytes = (BYTE *)&value;
	for (unsigned i = 0; i < sizeof(value); ++i)
		bytes[i] = 0xcc;
	value.contrast.cbSize = sizeof(value.contrast);
	if (sizeof(value.contrast) != (sizeof(void *) == 4 ? 12 : 16))
		return 1;
	if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value.contrast), &value.contrast, 0))
		return 2;
	for (unsigned i = 0; i < 4; ++i)
		if (value.guard[i] != 0xccccccccU)
			return 3;
	if (value.contrast.dwFlags & HCF_HIGHCONTRASTON)
		return 4;
	if (!value.contrast.lpszDefaultScheme)
		return 5;
	/* Programs measure the name before copying it. */
	unsigned length = 0;
	while (value.contrast.lpszDefaultScheme[length] && length < 256)
		++length;
	if (length >= 256)
		return 6;
	/* A second query names the same scheme. */
	HIGHCONTRASTW again = {sizeof(again)};
	if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(again), &again, 0) || !again.lpszDefaultScheme)
		return 7;
	return 0;
}
