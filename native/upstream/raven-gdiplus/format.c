/* SPDX-License-Identifier: MIT */
#include "gdip.h"

static GpStringFormat generic_default, generic_typographic;

static BOOL is_generic(const GpStringFormat *format)
{
	return format == &generic_default || format == &generic_typographic;
}

static GpStringFormat *new_format(INT flags, LANGID language)
{
	GpStringFormat *format = gp_alloc(sizeof(GpStringFormat));
	if (format) {
		format->flags = flags;
		format->language = language;
		format->digit_substitute = StringDigitSubstituteUser;
		format->trimming = StringTrimmingCharacter;
	}
	return format;
}

GpStatus GDIPAPI GdipCreateStringFormat(INT flags, LANGID language, GpStringFormat **out)
{
	if (!out)
		return InvalidParameter;
	return (*out = new_format(flags, language)) ? Ok : OutOfMemory;
}

GpStatus GDIPAPI GdipDeleteStringFormat(GpStringFormat *format)
{
	if (!format)
		return InvalidParameter;
	if (is_generic(format))
		return Ok;
	gp_free(format->tabs);
	gp_free(format->ranges);
	gp_free(format);
	return Ok;
}

GpStatus GDIPAPI GdipCloneStringFormat(const GpStringFormat *format, GpStringFormat **out)
{
	if (!format || !out)
		return InvalidParameter;
	GpStringFormat *copy = gp_dup(format, sizeof(GpStringFormat));
	if (!copy)
		return OutOfMemory;
	copy->tabs = NULL;
	copy->ranges = NULL;
	if ((format->tab_count && !(copy->tabs = gp_dup(format->tabs, format->tab_count * sizeof(REAL))))
		|| (format->range_count && !(copy->ranges = gp_dup(format->ranges, format->range_count * sizeof(CharacterRange))))) {
		GdipDeleteStringFormat(copy);
		return OutOfMemory;
	}
	*out = copy;
	return Ok;
}

// The two generic formats are one object each for the life of the process: changes stay and deleting them does nothing.
static LONG generics_ready;

static void make_generics(void)
{
	if (InterlockedExchange(&generics_ready, 1))
		return;
	generic_default.trimming = generic_typographic.trimming = StringTrimmingCharacter;
	generic_default.digit_substitute = generic_typographic.digit_substitute = StringDigitSubstituteUser;
	generic_typographic.flags = StringFormatFlagsNoFitBlackBox | StringFormatFlagsLineLimit | StringFormatFlagsNoClip;
	generic_typographic.trimming = StringTrimmingNone;
	generic_typographic.typographic = TRUE;
}

GpStatus GDIPAPI GdipStringFormatGetGenericDefault(GpStringFormat **out)
{
	if (!out)
		return InvalidParameter;
	make_generics();
	*out = &generic_default;
	return Ok;
}

GpStatus GDIPAPI GdipStringFormatGetGenericTypographic(GpStringFormat **out)
{
	if (!out)
		return InvalidParameter;
	make_generics();
	*out = &generic_typographic;
	return Ok;
}

#define FORMAT_FIELD(get_name, set_name, type, field) \
	GpStatus GDIPAPI get_name(const GpStringFormat *format, type *out) \
	{ \
		if (!format || !out) \
			return InvalidParameter; \
		*out = format->field; \
		return Ok; \
	} \
	GpStatus GDIPAPI set_name(GpStringFormat *format, type value) \
	{ \
		if (!format) \
			return InvalidParameter; \
		format->field = value; \
		return Ok; \
	}

FORMAT_FIELD(GdipGetStringFormatAlign, GdipSetStringFormatAlign, StringAlignment, alignment)
FORMAT_FIELD(GdipGetStringFormatLineAlign, GdipSetStringFormatLineAlign, StringAlignment, line_alignment)
FORMAT_FIELD(GdipGetStringFormatTrimming, GdipSetStringFormatTrimming, StringTrimming, trimming)
FORMAT_FIELD(GdipGetStringFormatFlags, GdipSetStringFormatFlags, INT, flags)

GpStatus GDIPAPI GdipGetStringFormatHotkeyPrefix(const GpStringFormat *format, INT *out)
{
	if (!format || !out)
		return InvalidParameter;
	*out = format->hotkey;
	return Ok;
}

GpStatus GDIPAPI GdipSetStringFormatHotkeyPrefix(GpStringFormat *format, INT prefix)
{
	if (!format)
		return InvalidParameter;
	format->hotkey = prefix;
	return Ok;
}

GpStatus GDIPAPI GdipGetStringFormatDigitSubstitution(const GpStringFormat *format, LANGID *language, StringDigitSubstitute *substitute)
{
	if (!format)
		return InvalidParameter;
	if (language)
		*language = format->digit_language;
	if (substitute)
		*substitute = format->digit_substitute;
	return Ok;
}

GpStatus GDIPAPI GdipSetStringFormatDigitSubstitution(GpStringFormat *format, LANGID language, StringDigitSubstitute substitute)
{
	if (!format)
		return InvalidParameter;
	format->digit_language = language;
	format->digit_substitute = substitute;
	return Ok;
}

GpStatus GDIPAPI GdipGetStringFormatMeasurableCharacterRangeCount(const GpStringFormat *format, INT *out)
{
	if (!format || !out)
		return InvalidParameter;
	*out = format->range_count;
	return Ok;
}

GpStatus GDIPAPI GdipSetStringFormatMeasurableCharacterRanges(GpStringFormat *format, INT count, const CharacterRange *ranges)
{
	if (!format || !ranges)
		return InvalidParameter;
	if (count > 32)
		return ValueOverflow;
	CharacterRange *copy = count ? gp_dup(ranges, count * sizeof(CharacterRange)) : NULL;
	if (count && !copy)
		return OutOfMemory;
	gp_free(format->ranges);
	format->ranges = copy;
	format->range_count = count;
	return Ok;
}

GpStatus GDIPAPI GdipGetStringFormatTabStopCount(const GpStringFormat *format, INT *out)
{
	if (!format || !out)
		return InvalidParameter;
	*out = format->tab_count;
	return Ok;
}

GpStatus GDIPAPI GdipGetStringFormatTabStops(const GpStringFormat *format, INT count, REAL *first, REAL *tabs)
{
	if (!format || !first || !tabs)
		return InvalidParameter;
	*first = format->first_tab;
	if (count > format->tab_count) {
		for (INT i = format->tab_count; i < count; i++)
			tabs[i] = 0.0f;
		count = format->tab_count;
	}
	if (count > 0)
		memcpy(tabs, format->tabs, count * sizeof(REAL));
	return Ok;
}

GpStatus GDIPAPI GdipSetStringFormatTabStops(GpStringFormat *format, REAL first, INT count, const REAL *tabs)
{
	if (!format || !tabs)
		return InvalidParameter;
	if (count <= 0)
		return Ok;
	if (first < 0.0f)
		return NotImplemented;
	REAL *copy = gp_dup(tabs, count * sizeof(REAL));
	if (!copy)
		return OutOfMemory;
	gp_free(format->tabs);
	format->tabs = copy;
	format->tab_count = count;
	format->first_tab = first;
	return Ok;
}
