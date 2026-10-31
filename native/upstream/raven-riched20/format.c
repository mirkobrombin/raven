/* SPDX-License-Identifier: MIT */
#include "re.h"

// Callers hand CHARFORMAT or CHARFORMAT2, each in narrow or wide form; the first field tells them apart.
BOOL re_format_from_user(CHARFORMAT2W *out, const void *user, BOOL ansi)
{
	(void)ansi;
	UINT size = user ? *(const UINT *)user : 0;
	memset(out, 0, sizeof(*out));
	out->cbSize = sizeof(*out);
	if (size == sizeof(CHARFORMATW) || size == sizeof(CHARFORMAT2W)) {
		memcpy(out, user, size);
		out->cbSize = sizeof(*out);
		if (size == sizeof(CHARFORMATW))
			out->dwMask &= CFM_ALL;
		return TRUE;
	}
	if (size == sizeof(CHARFORMATA) || size == sizeof(CHARFORMAT2A)) {
		const CHARFORMAT2A *narrow = user;
		out->dwMask = narrow->dwMask;
		out->dwEffects = narrow->dwEffects;
		out->yHeight = narrow->yHeight;
		out->yOffset = narrow->yOffset;
		out->crTextColor = narrow->crTextColor;
		out->bCharSet = narrow->bCharSet;
		out->bPitchAndFamily = narrow->bPitchAndFamily;
		MultiByteToWideChar(CP_ACP, 0, narrow->szFaceName, -1, out->szFaceName, LF_FACESIZE);
		if (size == sizeof(CHARFORMAT2A)) {
			out->wWeight = narrow->wWeight;
			out->sSpacing = narrow->sSpacing;
			out->crBackColor = narrow->crBackColor;
			out->lcid = narrow->lcid;
			out->dwReserved = narrow->dwReserved;
			out->sStyle = narrow->sStyle;
			out->wKerning = narrow->wKerning;
			out->bUnderlineType = narrow->bUnderlineType;
			out->bAnimation = narrow->bAnimation;
			out->bRevAuthor = narrow->bRevAuthor;
		} else {
			out->dwMask &= CFM_ALL;
		}
		return TRUE;
	}
	return FALSE;
}

BOOL re_format_to_user(const CHARFORMAT2W *format, void *user, BOOL ansi, DWORD mask)
{
	(void)ansi;
	UINT size = *(const UINT *)user;
	if (size == sizeof(CHARFORMATW) || size == sizeof(CHARFORMAT2W)) {
		memcpy(user, format, size);
		((CHARFORMATW *)user)->cbSize = size;
		((CHARFORMATW *)user)->dwMask = size == sizeof(CHARFORMATW) ? mask & CFM_ALL : mask;
		if (size == sizeof(CHARFORMATW))
			((CHARFORMATW *)user)->dwEffects &= CFM_EFFECTS;
		return TRUE;
	}
	if (size == sizeof(CHARFORMATA) || size == sizeof(CHARFORMAT2A)) {
		CHARFORMAT2A *narrow = user;
		narrow->dwMask = size == sizeof(CHARFORMATA) ? mask & CFM_ALL : mask;
		narrow->dwEffects = size == sizeof(CHARFORMATA) ? format->dwEffects & CFM_EFFECTS : format->dwEffects;
		narrow->yHeight = format->yHeight;
		narrow->yOffset = format->yOffset;
		narrow->crTextColor = format->crTextColor;
		narrow->bCharSet = format->bCharSet;
		narrow->bPitchAndFamily = format->bPitchAndFamily;
		WideCharToMultiByte(CP_ACP, 0, format->szFaceName, -1, narrow->szFaceName, LF_FACESIZE, NULL, NULL);
		if (size == sizeof(CHARFORMAT2A)) {
			narrow->wWeight = format->wWeight;
			narrow->sSpacing = format->sSpacing;
			narrow->crBackColor = format->crBackColor;
			narrow->lcid = format->lcid;
			narrow->dwReserved = format->dwReserved;
			narrow->sStyle = format->sStyle;
			narrow->wKerning = format->wKerning;
			narrow->bUnderlineType = format->bUnderlineType;
			narrow->bAnimation = format->bAnimation;
			narrow->bRevAuthor = format->bRevAuthor;
		}
		return TRUE;
	}
	return FALSE;
}
