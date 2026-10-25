/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windef.h>
#include <winbase.h>
#include <wine/debug.h>

/* The components trace only when RAVEN_COMPONENT_DEBUG names their channels, comma separated, or says "all"; the messages go to the
 * debugger, which RAVEN_TRACE_DEBUG makes the runtime, as "class:channel:function message". The formatting is the runtime's own, on kernel32 alone, as the components
 * that link this have no C runtime of their own. */

static char channels[256];
static LONG loaded;

static int channel_enabled(const char *name)
{
	if (!InterlockedCompareExchange(&loaded, 1, 0)) {
		const DWORD length = GetEnvironmentVariableA("RAVEN_COMPONENT_DEBUG", channels, sizeof channels);
		if (!length || length >= sizeof channels)
			channels[0] = 0;
	}
	if (!channels[0])
		return 0;
	for (const char *at = channels; *at;) {
		const char *end = at;
		while (*end && *end != ',')
			++end;
		const size_t size = (size_t)(end - at);
		if (size == 3 && at[0] == 'a' && at[1] == 'l' && at[2] == 'l')
			return 1;
		size_t index = 0;
		while (index < size && name[index] && name[index] == at[index])
			++index;
		if (index == size && !name[index])
			return 1;
		at = *end ? end + 1 : end;
	}
	return 0;
}

unsigned char __cdecl __wine_dbg_get_channel_flags(struct __wine_debug_channel *channel)
{
	return channel_enabled(channel->name) ? 0x0f : 0;
}

/* A small printf: flags, width, precision, the h, l and ll sizes and the conversions d i u x X p c s S and %. */
typedef struct {
	char *to;
	size_t size, used;
} Out;

static void put(Out *out, char character)
{
	if (out->used + 1 < out->size)
		out->to[out->used] = character;
	++out->used;
}

static void put_number(Out *out, unsigned long long value, int negative, unsigned base, int upper, int width, int zero)
{
	char digits[24];
	int count = 0;
	do {
		const unsigned digit = (unsigned)(value % base);
		digits[count++] = (char)(digit < 10 ? '0' + digit : (upper ? 'A' : 'a') + digit - 10);
		value /= base;
	} while (value);
	if (negative)
		digits[count++] = '-';
	for (int pad = count; pad < width; ++pad)
		put(out, zero ? '0' : ' ');
	while (count)
		put(out, digits[--count]);
}

static void format(Out *out, const char *text, va_list arguments)
{
	for (; *text; ++text) {
		if (*text != '%') {
			put(out, *text);
			continue;
		}
		++text;
		int zero = 0, width = 0, precision = -1, size = 0;
		while (*text == '-' || *text == '+' || *text == ' ' || *text == '#' || *text == '0')
			zero |= *text++ == '0';
		if (*text == '*') {
			width = va_arg(arguments, int);
			++text;
		}
		while (*text >= '0' && *text <= '9')
			width = width * 10 + *text++ - '0';
		if (*text == '.') {
			++text;
			precision = 0;
			if (*text == '*') {
				precision = va_arg(arguments, int);
				++text;
			}
			while (*text >= '0' && *text <= '9')
				precision = precision * 10 + *text++ - '0';
		}
		while (*text == 'h' || *text == 'l' || *text == 'I' || *text == 'z' || *text == 'w') {
			if (*text == 'l' || *text == 'w')
				++size;
			if (*text == 'I' && text[1] == '6' && text[2] == '4') {
				size = 2;
				text += 2;
			} else if (*text == 'z' || (*text == 'I' && !(text[1] == '3' && text[2] == '2'))) {
				size = sizeof(size_t) == 8 ? 2 : 0;
			} else if (*text == 'I') {
				text += 2;
			}
			++text;
		}
		switch (*text) {
		case 'd':
		case 'i': {
			const long long value = size >= 2 ? va_arg(arguments, long long) : va_arg(arguments, int);
			put_number(out, value < 0 ? 0ULL - (unsigned long long)value : (unsigned long long)value, value < 0, 10, 0, width, zero);
			break;
		}
		case 'u':
		case 'x':
		case 'X': {
			const unsigned long long value = size >= 2 ? va_arg(arguments, unsigned long long) : va_arg(arguments, unsigned int);
			put_number(out, value, 0, *text == 'u' ? 10 : 16, *text == 'X', width, zero);
			break;
		}
		case 'p':
			put_number(out, (ULONG_PTR)va_arg(arguments, void *), 0, 16, 0, sizeof(void *) * 2, 1);
			break;
		case 'f':
		case 'g':
		case 'e': {
			/* Two decimals, which is what the traces need of coordinates and sizes; beyond the range of an int only the sign and digits fit. */
			double value = va_arg(arguments, double);
			const int negative = value < 0;
			if (negative)
				value = -value;
			if (value > 2000000000.0)
				value = 2000000000.0;
			const int whole = (int)value, hundredths = (int)((value - whole) * 100.0 + 0.5);
			put_number(out, (unsigned)(whole + hundredths / 100), negative, 10, 0, 0, 0);
			put(out, '.');
			put_number(out, (unsigned)(hundredths % 100), 0, 10, 0, 2, 1);
			break;
		}
		case 'c':
			put(out, (char)va_arg(arguments, int));
			break;
		case 's':
		case 'S': {
			if (size || *text == 'S') {
				const WCHAR *string = va_arg(arguments, const WCHAR *);
				for (int index = 0; string && string[index] && (precision < 0 || index < precision); ++index)
					put(out, string[index] < 128 ? (char)string[index] : '?');
			} else {
				const char *string = va_arg(arguments, const char *);
				for (int index = 0; string && string[index] && (precision < 0 || index < precision); ++index)
					put(out, string[index]);
			}
			break;
		}
		case '%':
			put(out, '%');
			break;
		default:
			put(out, '%');
			put(out, *text);
			if (!*text)
				--text;
		}
	}
}

/* The formatters return text from a ring of buffers that the message they are part of uses before they come round again. */
static char *ring(void)
{
	static char buffers[16][512];
	static LONG next;
	return buffers[(InterlockedIncrement(&next) & 15)];
}

const char *__cdecl wine_dbg_sprintf(const char *text, ...)
{
	char *buffer = ring();
	Out out = {buffer, 512, 0};
	va_list arguments;
	va_start(arguments, text);
	format(&out, text, arguments);
	va_end(arguments);
	buffer[out.used < 512 ? out.used : 511] = 0;
	return buffer;
}

static const char *quoted(const void *string, int wide, int count)
{
	if (!string)
		return "(null)";
	if ((ULONG_PTR)string < 0x10000)
		return wine_dbg_sprintf("#%04x", (unsigned)(ULONG_PTR)string);
	char *buffer = ring();
	Out out = {buffer, 512, 0};
	put(&out, wide ? 'L' : '"');
	if (wide)
		put(&out, '"');
	for (int index = 0; count < 0 || index < count; ++index) {
		const unsigned character = wide ? ((const WCHAR *)string)[index] : ((const unsigned char *)string)[index];
		if (!character && count < 0)
			break;
		if (character >= 32 && character < 127 && character != '"' && character != '\\')
			put(&out, (char)character);
		else {
			char escape[8];
			Out small = {escape, sizeof escape, 0};
			put(&small, '\\');
			put(&small, 'x');
			put_number(&small, character, 0, 16, 0, wide ? 4 : 2, 1);
			for (size_t at = 0; at < small.used; ++at)
				put(&out, escape[at]);
		}
		if (out.used > 480)
			break;
	}
	put(&out, '"');
	buffer[out.used < 512 ? out.used : 511] = 0;
	return buffer;
}

const char *wine_dbgstr_an(const char *string, int count)
{
	return quoted(string, 0, count);
}

const char *wine_dbgstr_wn(const WCHAR *string, int count)
{
	return quoted(string, 1, count);
}

int __cdecl ros_dbg_log(enum __wine_debug_class class, struct __wine_debug_channel *channel, const char *file,
	const char *function, const int line, const char *text, ...)
{
	static const char *const classes[] = {"fixme", "err", "warn", "trace"};
	/* ReactOS's macros only look at the channel's static flags before they get here: the channels RAVEN_COMPONENT_DEBUG names are the
	 * only ones that print, so a component left alone costs nothing. */
	if (!channel_enabled(channel->name))
		return 0;
	char buffer[1024];
	Out out = {buffer, sizeof buffer, 0};
	const char *prefix = wine_dbg_sprintf("%s:%s:%s ", (unsigned)class < 4 ? classes[class] : "?", channel->name, function);
	for (; *prefix; ++prefix)
		put(&out, *prefix);
	va_list arguments;
	va_start(arguments, text);
	format(&out, text, arguments);
	va_end(arguments);
	const size_t length = out.used < sizeof buffer ? out.used : sizeof buffer - 1;
	buffer[length] = 0;
	OutputDebugStringA(buffer);
	return (int)length;
}
