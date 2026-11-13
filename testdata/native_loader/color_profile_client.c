#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <icm.h>

/* A monitor profile ("mntr") of RGB data ("RGB "), signed "acsp". */
static BYTE icc[256];

static void put_big(BYTE *at, DWORD value)
{
	at[0] = (BYTE)(value >> 24);
	at[1] = (BYTE)(value >> 16);
	at[2] = (BYTE)(value >> 8);
	at[3] = (BYTE)value;
}

/* Color profiles as an image decoder reads them: opened from memory or from a file, valid with an ICC header, whose fields come back
 * in the processor's order, whose elements are read by tag, and whose bytes come back whole. */
int raven_entry(void)
{
	put_big(icc, sizeof(icc));
	put_big(icc + 12, 0x6d6e7472);
	put_big(icc + 16, 0x52474220);
	put_big(icc + 36, 0x61637370);
	/* One element after the header: a description tag of twelve bytes at offset 144. */
	put_big(icc + 128, 1);
	put_big(icc + 132, 0x64657363);
	put_big(icc + 136, 144);
	put_big(icc + 140, 12);
	for (int index = 0; index < 12; ++index)
		icc[144 + index] = (BYTE)('a' + index);
	PROFILE memory = {PROFILE_MEMBUFFER, icc, sizeof(icc)};
	HPROFILE profile = OpenColorProfileW(&memory, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
	if (!profile)
		return 1;
	BOOL valid = FALSE;
	if (!IsColorProfileValid(profile, &valid) || !valid)
		return 2;
	PROFILEHEADER header;
	if (!GetColorProfileHeader(profile, &header) || header.phSize != sizeof(icc) || header.phClass != 0x6d6e7472U ||
		header.phDataColorSpace != 0x52474220U || header.phSignature != 0x61637370U)
		return 3;
	static BYTE copy[256];
	DWORD size = 0;
	if (GetColorProfileFromHandle(profile, NULL, &size) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || size != sizeof(icc))
		return 4;
	if (!GetColorProfileFromHandle(profile, copy, &size) || copy[36] != 'a' || copy[39] != 'p')
		return 5;
	/* Its elements, as paint.net reads a display's profile: counted, named by a 1-based index, looked for by tag, and read whole or from
	   an offset, the size of one asked for without a buffer. */
	DWORD count = 0, tag = 0;
	BOOL present = FALSE, reference = TRUE;
	if (!GetCountColorProfileElements(profile, &count) || count != 1)
		return 12;
	if (!GetColorProfileElementTag(profile, 1, &tag) || tag != 0x64657363U || GetColorProfileElementTag(profile, 2, &tag))
		return 13;
	if (!IsColorProfileTagPresent(profile, 0x64657363U, &present) || !present || !IsColorProfileTagPresent(profile, 0x77747074U, &present) || present)
		return 14;
	BYTE element[16] = {0};
	size = 0;
	if (!GetColorProfileElement(profile, 0x64657363U, 0, &size, NULL, &reference) || size != 12)
		return 15;
	size = 4;
	if (!GetColorProfileElement(profile, 0x64657363U, 2, &size, element, &reference) || size != 4 || element[0] != 'c' || element[3] != 'f' || reference)
		return 16;
	if (!CloseColorProfile(profile))
		return 6;

	WCHAR path[MAX_PATH];
	DWORD length = GetTempPathW(MAX_PATH - 16, path);
	if (!length)
		return 7;
	lstrcatW(path, L"raven-test.icc");
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written = 0;
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, icc, 128, &written, NULL) || written != 128)
		return 8;
	CloseHandle(file);
	/* The header says 256 bytes and the file holds 128: the profile opens but is not valid. */
	PROFILE named = {PROFILE_FILENAME, path, (DWORD)((lstrlenW(path) + 1) * sizeof(WCHAR))};
	profile = OpenColorProfileW(&named, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
	if (!profile)
		return 9;
	if (!IsColorProfileValid(profile, &valid) || valid)
		return 10;
	CloseColorProfile(profile);
	DeleteFileW(path);
	PROFILE missing = {PROFILE_FILENAME, L"no-such-profile.icc", 40};
	if (OpenColorProfileW(&missing, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING))
		return 11;
	return 0;
}
