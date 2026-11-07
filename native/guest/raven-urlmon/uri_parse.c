/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "uri.h"

#define FLAG(name) Uri_CREATE_##name

/* A growing wide string; failure is remembered and reported at the end. */
typedef struct
{
	WCHAR *text;
	SIZE_T length, capacity;
	BOOL failed;
} text_builder;

/* How a scheme's text is treated. */
typedef enum
{
	KIND_UNKNOWN,
	KIND_HIERARCHICAL,
	KIND_OPAQUE,
	KIND_FILE,
	KIND_RES,
	KIND_MK,
	KIND_WILDCARD,
	KIND_RAW_OPAQUE
} scheme_kind;

static const struct
{
	const WCHAR *name;
	URL_SCHEME type;
	DWORD port;
	scheme_kind kind;
} known_schemes[] = {
	{L"http", URL_SCHEME_HTTP, 80, KIND_HIERARCHICAL},
	{L"https", URL_SCHEME_HTTPS, 443, KIND_HIERARCHICAL},
	{L"ftp", URL_SCHEME_FTP, 21, KIND_HIERARCHICAL},
	{L"gopher", URL_SCHEME_GOPHER, 70, KIND_HIERARCHICAL},
	{L"nntp", URL_SCHEME_NNTP, 119, KIND_HIERARCHICAL},
	{L"telnet", URL_SCHEME_TELNET, 23, KIND_HIERARCHICAL},
	{L"wais", URL_SCHEME_WAIS, 210, KIND_HIERARCHICAL},
	{L"snews", URL_SCHEME_SNEWS, 0, KIND_OPAQUE},
	{L"news", URL_SCHEME_NEWS, 0, KIND_OPAQUE},
	{L"mailto", URL_SCHEME_MAILTO, 0, KIND_OPAQUE},
	{L"file", URL_SCHEME_FILE, 0, KIND_FILE},
	{L"res", URL_SCHEME_RES, 0, KIND_RES},
	{L"mk", URL_SCHEME_MK, 0, KIND_MK},
	{L"about", URL_SCHEME_ABOUT, 0, KIND_RAW_OPAQUE},
	{L"javascript", URL_SCHEME_JAVASCRIPT, 0, KIND_RAW_OPAQUE},
	{L"vbscript", URL_SCHEME_VBSCRIPT, 0, KIND_RAW_OPAQUE},
	{L"local", URL_SCHEME_LOCAL, 0, KIND_OPAQUE},
	{L"shell", URL_SCHEME_SHELL, 0, KIND_OPAQUE},
};

void *uri_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

void uri_free(void *memory)
{
	if (memory)
		HeapFree(GetProcessHeap(), 0, memory);
}

WCHAR *uri_text_range(const WCHAR *start, const WCHAR *end)
{
	WCHAR *copy = uri_alloc((end - start + 1) * sizeof(WCHAR));
	if (copy)
		memcpy(copy, start, (end - start) * sizeof(WCHAR));
	return copy;
}

WCHAR *uri_text_copy(const WCHAR *text)
{
	return uri_text_range(text, text + wcslen(text));
}

static void put_char(text_builder *out, WCHAR character)
{
	if (out->length + 2 > out->capacity)
	{
		SIZE_T capacity = out->capacity ? out->capacity * 2 : 64;
		WCHAR *grown = uri_alloc(capacity * sizeof(WCHAR));
		if (!grown)
		{
			out->failed = TRUE;
			return;
		}
		if (out->text)
			memcpy(grown, out->text, out->length * sizeof(WCHAR));
		uri_free(out->text);
		out->text = grown;
		out->capacity = capacity;
	}
	out->text[out->length++] = character;
	out->text[out->length] = 0;
}

static void put_text(text_builder *out, const WCHAR *text)
{
	for (; *text; text++)
		put_char(out, *text);
}

static void put_range(text_builder *out, const WCHAR *start, const WCHAR *end)
{
	for (; start < end; start++)
		put_char(out, *start);
}

static int hex_value(WCHAR character)
{
	if (character >= L'0' && character <= L'9')
		return character - L'0';
	if (character >= L'a' && character <= L'f')
		return character - L'a' + 10;
	if (character >= L'A' && character <= L'F')
		return character - L'A' + 10;
	return -1;
}

static void put_hex(text_builder *out, unsigned value, BOOL lower)
{
	const WCHAR *digits = lower ? L"0123456789abcdef" : L"0123456789ABCDEF";
	put_char(out, L'%');
	put_char(out, digits[(value >> 4) & 15]);
	put_char(out, digits[value & 15]);
}

static BOOL is_alpha(WCHAR character)
{
	return (character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z');
}

static BOOL is_digit(WCHAR character)
{
	return character >= L'0' && character <= L'9';
}

static WCHAR lower_char(WCHAR character)
{
	return character >= L'A' && character <= L'Z' ? character + 32 : character;
}

static void lower_text(WCHAR *text)
{
	for (; *text; text++)
		*text = lower_char(*text);
}

static BOOL is_unreserved(WCHAR character)
{
	return is_alpha(character) || is_digit(character) || character == L'-' || character == L'.' || character == L'_' || character == L'~';
}

/* What a canonical URI never carries in the open. */
static BOOL is_forbidden(WCHAR character)
{
	return character <= 0x20 || character == 0x7f || character == L'"' || character == L'<' || character == L'>' || character == L'^' || character == L'`' ||
		character == L'{' || character == L'|' || character == L'}';
}

static BOOL is_valid_escape(const WCHAR *at)
{
	return at[0] == L'%' && hex_value(at[1]) >= 0 && hex_value(at[2]) >= 0;
}

/* Canonical text of a component: escapes of plain characters open up, forbidden ones close.
   Returns FALSE on an escape that is not one. */
static BOOL put_canonical(text_builder *out, const WCHAR *start, const WCHAR *end, BOOL encode)
{
	const WCHAR *at;
	for (at = start; at < end; at++)
	{
		if (*at == L'%')
		{
			int decoded;
			if (end - at < 3 || !is_valid_escape(at))
				return FALSE;
			decoded = hex_value(at[1]) * 16 + hex_value(at[2]);
			if (is_unreserved((WCHAR)decoded))
				put_char(out, (WCHAR)decoded);
			else
				put_range(out, at, at + 3);
			at += 2;
		}
		else if (encode && is_forbidden(*at))
			put_hex(out, *at, FALSE);
		else
			put_char(out, *at);
	}
	return TRUE;
}

BOOL uri_flags_valid(DWORD flags)
{
	if ((flags & FLAG(DECODE_EXTRA_INFO)) && (flags & FLAG(NO_DECODE_EXTRA_INFO)))
		return FALSE;
	if ((flags & FLAG(CANONICALIZE)) && (flags & FLAG(NO_CANONICALIZE)))
		return FALSE;
	if ((flags & FLAG(CRACK_UNKNOWN_SCHEMES)) && (flags & FLAG(NO_CRACK_UNKNOWN_SCHEMES)))
		return FALSE;
	if ((flags & FLAG(PRE_PROCESS_HTML_URI)) && (flags & FLAG(NO_PRE_PROCESS_HTML_URI)))
		return FALSE;
	if ((flags & FLAG(IE_SETTINGS)) && (flags & FLAG(NO_IE_SETTINGS)))
		return FALSE;
	return TRUE;
}

/* Flags as they act, so asking for a default reads the same as not asking. */
DWORD uri_effective_flags(DWORD flags)
{
	if (!(flags & Uri_CREATE_NO_CANONICALIZE))
		flags |= Uri_CREATE_CANONICALIZE;
	if (!(flags & Uri_CREATE_NO_DECODE_EXTRA_INFO))
		flags |= Uri_CREATE_DECODE_EXTRA_INFO;
	if (!(flags & Uri_CREATE_NO_CRACK_UNKNOWN_SCHEMES))
		flags |= Uri_CREATE_CRACK_UNKNOWN_SCHEMES;
	if (!(flags & Uri_CREATE_NO_PRE_PROCESS_HTML_URI))
		flags |= Uri_CREATE_PRE_PROCESS_HTML_URI;
	if (!(flags & Uri_CREATE_IE_SETTINGS))
		flags |= Uri_CREATE_NO_IE_SETTINGS;
	return flags;
}

static int find_scheme(const WCHAR *name)
{
	int index;
	for (index = 0; index < (int)ARRAYSIZE(known_schemes); index++)
		if (!lstrcmpiW(known_schemes[index].name, name))
			return index;
	return -1;
}

URL_SCHEME uri_scheme_of(const WCHAR *name)
{
	int index = find_scheme(name);
	return index < 0 ? URL_SCHEME_UNKNOWN : known_schemes[index].type;
}

/* Drops surrounding blanks and every tab and line break, as HTML attribute values need. */
static WCHAR *preprocess(const WCHAR *text)
{
	const WCHAR *start = text, *end = text + wcslen(text), *at;
	WCHAR *copy, *write;
	while (start < end && *start <= 0x20)
		start++;
	while (end > start && end[-1] <= 0x20)
		end--;
	copy = uri_alloc((end - start + 1) * sizeof(WCHAR));
	if (!copy)
		return NULL;
	write = copy;
	for (at = start; at < end; at++)
		if (*at != L'\t' && *at != L'\r' && *at != L'\n')
			*write++ = *at;
	return copy;
}

/* The scheme at the start of the text: its end, or NULL without one. */
static const WCHAR *scheme_end(const WCHAR *text)
{
	const WCHAR *at = text;
	if (text[0] == L'*' && text[1] == L':')
		return text + 1;
	while (is_alpha(*at) || is_digit(*at) || *at == L'+' || *at == L'-' || *at == L'.')
		at++;
	/* One letter before a colon is a drive. */
	return *at == L':' && at - text >= 2 ? at : NULL;
}

static BOOL is_slash(WCHAR character)
{
	return character == L'/' || character == L'\\';
}

static BOOL is_drive(const WCHAR *text)
{
	return is_alpha(text[0]) && (text[1] == L':' || text[1] == L'|');
}

/* Parts of an address written in numbers: each part is decimal and the last fills what is left. */
static BOOL parse_ipv4(const WCHAR *start, const WCHAR *end, BYTE bytes[4])
{
	DWORD parts[4], count = 0, value, index, limit;
	const WCHAR *at = start;
	if (start == end)
		return FALSE;
	for (;;)
	{
		const WCHAR *first = at;
		unsigned long long number = 0;
		while (at < end && is_digit(*at))
		{
			number = number * 10 + (*at++ - L'0');
			if (number > 0xFFFFFFFFull)
				return FALSE;
		}
		if (at == first || count == 4)
			return FALSE;
		parts[count++] = (DWORD)number;
		if (at == end)
			break;
		if (*at++ != L'.' || at == end)
			return FALSE;
	}
	value = 0;
	for (index = 0; index + 1 < count; index++)
	{
		if (parts[index] > 255)
			return FALSE;
		bytes[index] = (BYTE)parts[index];
	}
	limit = 4 - (count - 1);
	if (limit < 4 && parts[count - 1] >> (limit * 8))
		return FALSE;
	value = parts[count - 1];
	for (index = 0; index < limit; index++)
		bytes[3 - index] = (BYTE)(value >> (index * 8));
	return TRUE;
}

/* The eight words of an address, with whether its tail was written as dotted numbers. */
static BOOL parse_ipv6(const WCHAR *start, const WCHAR *end, WORD words[8], BOOL *dotted)
{
	const WCHAR *at = start;
	int count = 0, gap = -1, index;
	*dotted = FALSE;
	if (end - start < 2)
		return FALSE;
	if (at[0] == L':')
	{
		if (at[1] != L':')
			return FALSE;
		gap = 0;
		at += 2;
	}
	while (at < end)
	{
		const WCHAR *first = at, *dot;
		unsigned value = 0;
		if (count == 8)
			return FALSE;
		for (dot = at; dot < end && *dot != L':' && *dot != L'.'; dot++);
		if (dot < end && *dot == L'.')
		{
			const WCHAR *stop = at;
			BYTE bytes[4];
			int parts = 1;
			while (stop < end && *stop != L':')
			{
				if (*stop == L'.')
					parts++;
				stop++;
			}
			if (stop != end || parts != 4 || count > 6 || !parse_ipv4(at, end, bytes))
				return FALSE;
			words[count++] = (WORD)((bytes[0] << 8) | bytes[1]);
			words[count++] = (WORD)((bytes[2] << 8) | bytes[3]);
			*dotted = TRUE;
			at = end;
			break;
		}
		while (at < end && hex_value(*at) >= 0 && at - first < 4)
			value = value * 16 + hex_value(*at++);
		if (at == first)
			return FALSE;
		words[count++] = (WORD)value;
		if (at == end)
			break;
		if (*at != L':')
			return FALSE;
		at++;
		if (at == end)
			return FALSE;
		if (*at == L':')
		{
			if (gap >= 0)
				return FALSE;
			gap = count;
			at++;
		}
	}
	if (gap < 0)
		return count == 8;
	if (count > 7)
		return FALSE;
	for (index = 7; index >= 8 - (count - gap); index--)
		words[index] = words[index - (8 - count)];
	for (index = gap; index < gap + 8 - count; index++)
		words[index] = 0;
	return TRUE;
}

static void put_number(text_builder *out, unsigned value)
{
	WCHAR digits[12];
	int used = 0;
	do
	{
		digits[used++] = (WCHAR)(L'0' + value % 10);
		value /= 10;
	}
	while (value);
	while (used)
		put_char(out, digits[--used]);
}

static void put_hex_word(text_builder *out, unsigned value)
{
	static const WCHAR digits[] = L"0123456789abcdef";
	BOOL started = FALSE;
	int shift;
	for (shift = 12; shift >= 0; shift -= 4)
	{
		unsigned digit = (value >> shift) & 15;
		if (digit || started || !shift)
		{
			put_char(out, digits[digit]);
			started = TRUE;
		}
	}
}

/* The shortest usual form; a compatible or mapped address keeps its dotted tail. */
static void put_ipv6(text_builder *out, const WORD words[8], BOOL dotted)
{
	int best = -1, best_length = 1, run = 0, index, last = 8;
	BOOL keep_dotted = dotted && !words[0] && !words[1] && !words[2] && !words[3] && !words[4] && (!words[5] || words[5] == 0xffff || words[5] == 0x5efe);
	if (keep_dotted)
		last = 6;
	for (index = 0; index < last; index++)
	{
		if (words[index])
		{
			run = 0;
			continue;
		}
		run++;
		if (run > best_length)
		{
			best_length = run;
			best = index - run + 1;
		}
	}
	for (index = 0; index < last; index++)
	{
		if (index == best)
		{
			put_text(out, L"::");
			index += best_length - 1;
			continue;
		}
		if (index && !(best >= 0 && index == best + best_length))
			put_char(out, L':');
		put_hex_word(out, words[index]);
	}
	if (keep_dotted)
	{
		if (best < 0 || best + best_length < last)
			put_char(out, L':');
		put_number(out, words[6] >> 8);
		put_char(out, L'.');
		put_number(out, words[6] & 255);
		put_char(out, L'.');
		put_number(out, words[7] >> 8);
		put_char(out, L'.');
		put_number(out, words[7] & 255);
	}
}

static const WCHAR *find_any(const WCHAR *text, const WCHAR *set)
{
	while (*text && !wcschr(set, *text))
		text++;
	return text;
}

static BOOL is_all_ascii(const WCHAR *start, const WCHAR *end)
{
	for (; start < end; start++)
		if (*start >= 0x80)
			return FALSE;
	return TRUE;
}

/* Bootstring with the parameters IDNA gives it (RFC 3492). */
static BOOL put_punycode(text_builder *out, const WCHAR *start, const WCHAR *end)
{
	DWORD code = 128, delta = 0, bias = 72, handled = 0, basic = 0, length = (DWORD)(end - start);
	const WCHAR *at;
	put_text(out, L"xn--");
	for (at = start; at < end; at++)
		if (*at < 0x80)
		{
			put_char(out, lower_char(*at));
			basic++;
		}
	handled = basic;
	if (basic)
		put_char(out, L'-');
	while (handled < length)
	{
		DWORD next = 0x110000, value;
		for (at = start; at < end; at++)
			if (*at >= code && *at < next)
				next = *at;
		delta += (next - code) * (handled + 1);
		code = next;
		for (at = start; at < end; at++)
		{
			if (*at < code)
				delta++;
			if (*at != code)
				continue;
			value = delta;
			{
				DWORD step;
				for (step = 36;; step += 36)
				{
					DWORD threshold = step <= bias ? 1 : step >= bias + 26 ? 26 : step - bias;
					if (value < threshold)
						break;
					{
						DWORD digit = threshold + (value - threshold) % (36 - threshold);
						put_char(out, (WCHAR)(digit < 26 ? L'a' + digit : L'0' + digit - 26));
					}
					value = (value - threshold) / (36 - threshold);
				}
				put_char(out, (WCHAR)(value < 26 ? L'a' + value : L'0' + value - 26));
			}
			{
				DWORD adapted = handled == basic ? delta / 700 : delta / 2, scale = 0;
				adapted += adapted / (handled + 1);
				while (adapted > 455)
				{
					adapted /= 35;
					scale += 36;
				}
				bias = scale + 36 * adapted / (adapted + 38);
			}
			delta = 0;
			handled++;
		}
		delta++;
		code++;
	}
	return !out->failed;
}

static int decode_punycode_digit(WCHAR character)
{
	if (character >= L'a' && character <= L'z')
		return character - L'a';
	if (character >= L'A' && character <= L'Z')
		return character - L'A';
	if (is_digit(character))
		return character - L'0' + 26;
	return -1;
}

/* One xn-- label back to its Unicode form; FALSE leaves the label as it stands. */
static BOOL put_unicode_label(text_builder *out, const WCHAR *start, const WCHAR *end)
{
	WCHAR decoded[128];
	DWORD code = 128, bias = 72, index = 0, length = 0;
	const WCHAR *at = start + 4, *dash = NULL, *scan;
	for (scan = at; scan < end; scan++)
		if (*scan == L'-')
			dash = scan;
	if (dash)
	{
		for (scan = at; scan < dash && length < ARRAYSIZE(decoded); scan++)
			decoded[length++] = *scan;
		at = dash + 1;
	}
	while (at < end)
	{
		DWORD old = index, weight = 1, step;
		for (step = 36;; step += 36)
		{
			DWORD threshold;
			int digit;
			if (at >= end)
				return FALSE;
			digit = decode_punycode_digit(*at++);
			if (digit < 0)
				return FALSE;
			index += digit * weight;
			threshold = step <= bias ? 1 : step >= bias + 26 ? 26 : step - bias;
			if ((DWORD)digit < threshold)
				break;
			weight *= 36 - threshold;
		}
		{
			DWORD adapted = old ? (index - old) / 2 : (index - old) / 700, scale = 0;
			adapted += adapted / (length + 1);
			while (adapted > 455)
			{
				adapted /= 35;
				scale += 36;
			}
			bias = scale + 36 * adapted / (adapted + 38);
		}
		code += index / (length + 1);
		index %= length + 1;
		if (length + 1 >= ARRAYSIZE(decoded) || code >= 0x10000)
			return FALSE;
		memmove(decoded + index + 1, decoded + index, (length - index) * sizeof(WCHAR));
		decoded[index++] = (WCHAR)code;
		length++;
	}
	put_range(out, decoded, decoded + length);
	return TRUE;
}

/* The host as an International name reads, for the display form. */
static WCHAR *host_for_display(const WCHAR *host)
{
	text_builder out = {0};
	const WCHAR *label = host, *end;
	for (;;)
	{
		for (end = label; *end && *end != L'.'; end++);
		if (end - label <= 4 || _wcsnicmp(label, L"xn--", 4) || !put_unicode_label(&out, label, end))
			put_range(&out, label, end);
		if (!*end)
			break;
		put_char(&out, L'.');
		label = end + 1;
	}
	if (!out.text)
		return uri_text_copy(L"");
	return out.text;
}

static BOOL put_host_labels(text_builder *out, const WCHAR *start, const WCHAR *end)
{
	const WCHAR *label = start, *stop;
	for (;;)
	{
		for (stop = label; stop < end && *stop != L'.'; stop++);
		if (is_all_ascii(label, stop))
			put_range(out, label, stop);
		else if (!put_punycode(out, label, stop))
			return FALSE;
		if (stop == end)
			break;
		put_char(out, L'.');
		label = stop + 1;
	}
	return TRUE;
}

static BOOL put_host_name(text_builder *out, const WCHAR *start, const WCHAR *end)
{
	const WCHAR *at;
	text_builder plain = {0};
	BOOL done;
	for (at = start; at < end; at++)
	{
		if (*at == L'%')
		{
			int decoded;
			if (end - at < 3 || !is_valid_escape(at))
			{
				uri_free(plain.text);
				return FALSE;
			}
			decoded = hex_value(at[1]) * 16 + hex_value(at[2]);
			if (is_unreserved((WCHAR)decoded))
				put_char(&plain, lower_char((WCHAR)decoded));
			else
				put_hex(&plain, decoded, TRUE);
			at += 2;
		}
		else if (is_forbidden(*at))
			put_hex(&plain, *at, TRUE);
		else
			put_char(&plain, lower_char(*at));
	}
	done = !plain.failed;
	if (plain.text)
		put_range(out, plain.text, plain.text + plain.length);
	uri_free(plain.text);
	return done;
}

/* What the text between the slashes and the path says about host and port. */
static HRESULT parse_host(uri_data *uri, const WCHAR *start, const WCHAR *end, BOOL known, BOOL canonicalize, DWORD default_port)
{
	text_builder out = {0};
	const WCHAR *host_end = end, *port_text = NULL, *at_port;
	WORD words[8];
	BYTE bytes[4];
	BOOL dotted;
	uri->port = default_port;
	uri->has_port = known;
	if (start[0] == L'[' && start < end)
	{
		const WCHAR *close = start;
		while (close < end && *close != L']')
			close++;
		if (close < end)
		{
			const WCHAR *after = close + 1;
			BOOL port_follows = after < end && *after == L':';
			BOOL valid_port = after == end || port_follows;
			if (port_follows)
			{
				const WCHAR *digit = after + 1;
				unsigned long value = 0;
				for (; digit < end; digit++)
				{
					if (!is_digit(*digit) || value > 65535)
					{
						valid_port = FALSE;
						break;
					}
					value = value * 10 + (*digit - L'0');
				}
				if (value > 65535)
					valid_port = FALSE;
			}
			if (start[1] == L'v' || start[1] == L'V')
			{
				if (!known && !valid_port)
					goto registered;
				if (!valid_port)
					return E_INVALIDARG;
				uri->host = uri_text_range(start, close + 1);
				if (known)
					lower_text(uri->host);
				uri->host_type = Uri_HOST_UNKNOWN;
				port_text = port_follows ? after + 1 : NULL;
				goto port;
			}
			if (valid_port && parse_ipv6(start + 1, close, words, &dotted))
			{
				if (known && canonicalize)
				{
					put_ipv6(&out, words, dotted);
					uri->host = out.text;
				}
				else
					uri->host = uri_text_range(start + 1, close);
				uri->host_type = Uri_HOST_IPV6;
				uri->host_in_brackets = TRUE;
				port_text = port_follows ? after + 1 : NULL;
				goto port;
			}
			if (known)
				return E_INVALIDARG;
		}
		else
		{
			/* No closing bracket: the whole of it is a name. */
			goto registered_whole;
		}
	}
registered:
	if (known)
	{
		const WCHAR *colon = start;
		while (colon < end && *colon != L':')
			colon++;
		host_end = colon;
		if (colon < end)
			port_text = colon + 1;
		if (uri->flags & URI_INTERNAL_COMBINED)
			for (at_port = colon + 1; at_port < end; at_port++)
				if (!is_digit(*at_port))
				{
					host_end = end;
					port_text = NULL;
					break;
				}
	}
	else
	{
		/* Unknown schemes take a port only if it reads as one. */
		const WCHAR *colon = NULL, *at;
		for (at = start; at < end; at++)
			if (*at == L':')
			{
				colon = at;
				break;
			}
		if (colon && colon + 1 < end)
		{
			unsigned long value = 0;
			BOOL digits = TRUE;
			for (at = colon + 1; at < end; at++)
			{
				if (!is_digit(*at) || value > 65535)
				{
					digits = FALSE;
					break;
				}
				value = value * 10 + (*at - L'0');
			}
			if (digits && value <= 65535)
			{
				host_end = colon;
				port_text = colon + 1;
			}
		}
	}
	if (0)
	{
registered_whole:
		host_end = end;
	}
	if (parse_ipv4(start, host_end, bytes))
	{
		if (known && canonicalize)
		{
			int index;
			for (index = 0; index < 4; index++)
			{
				if (index)
					put_char(&out, L'.');
				put_number(&out, bytes[index]);
			}
			uri->host = out.text;
		}
		else
			uri->host = uri_text_range(start, host_end);
		uri->host_type = Uri_HOST_IPV4;
	}
	else if (known)
	{
		if (canonicalize)
		{
			if (!put_host_name(&out, start, host_end))
			{
				uri_free(out.text);
				return E_INVALIDARG;
			}
			uri->host = out.text ? out.text : uri_text_copy(L"");
		}
		else
		{
			const WCHAR *at;
			for (at = start; at < host_end; at++)
				if (*at == L'%' && (host_end - at < 3 || !is_valid_escape(at)))
					return E_INVALIDARG;
			uri->host = uri_text_range(start, host_end);
		}
		uri->host_type = host_end > start ? Uri_HOST_DNS : Uri_HOST_UNKNOWN;
	}
	else
	{
		uri->host = uri_text_range(start, host_end);
		uri->host_type = host_end > start ? Uri_HOST_DNS : Uri_HOST_UNKNOWN;
	}
port:
	if (!uri->host)
		return E_OUTOFMEMORY;
	if (port_text)
	{
		unsigned long value = 0;
		const WCHAR *digit;
		for (digit = port_text; digit < end; digit++)
		{
			if (!is_digit(*digit))
				return E_INVALIDARG;
			value = value * 10 + (*digit - L'0');
			/* A port the builder was told to use is taken as it is. */
			if (value > 65535 && !(uri->flags & URI_INTERNAL_COMBINED))
				return E_INVALIDARG;
		}
		if (port_text < end)
		{
			uri->port = value;
			uri->has_port = TRUE;
			if (!(canonicalize && known && value == default_port && default_port))
			{
				uri->port_text = uri_text_range(port_text, end);
				if (canonicalize && known)
				{
					text_builder number = {0};
					put_number(&number, value);
					uri_free(uri->port_text);
					uri->port_text = number.text;
				}
			}
		}
		else if (!canonicalize && !known)
			uri->port_text = uri_text_range(port_text, end);
	}
	return S_OK;
}

/* Removes "." and ".." from a path whose separators are in the given set. */
static void remove_dots(WCHAR *path, const WCHAR *separators, BOOL rooted)
{
	WCHAR *read = path, *write = path;
	if (rooted && wcschr(separators, *read) && *read)
		write = ++read;
	while (*read)
	{
		WCHAR *stop = read;
		SIZE_T length;
		while (*stop && !wcschr(separators, *stop))
			stop++;
		length = stop - read;
		if (length == 1 && read[0] == L'.')
		{
			read = *stop ? stop + 1 : stop;
			continue;
		}
		if (length == 2 && read[0] == L'.' && read[1] == L'.')
		{
			WCHAR *base = rooted ? path + 1 : path;
			if (write > base)
			{
				write--;
				while (write > base && !wcschr(separators, write[-1]))
					write--;
			}
			read = *stop ? stop + 1 : stop;
			continue;
		}
		memmove(write, read, length * sizeof(WCHAR));
		write += length;
		if (*stop)
			*write++ = *stop++;
		read = stop;
	}
	*write = 0;
}

/* File paths also open escapes of '#' and carry stray percents. */
static void put_file_path(text_builder *out, const WCHAR *start, const WCHAR *end, BOOL escape_all)
{
	const WCHAR *at;
	for (at = start; at < end; at++)
	{
		if (*at == L'\\')
			put_char(out, L'/');
		else if (*at == L'%')
		{
			if (!escape_all && end - at >= 3 && is_valid_escape(at))
			{
				int decoded = hex_value(at[1]) * 16 + hex_value(at[2]);
				if (is_unreserved((WCHAR)decoded) || decoded == L'#')
					put_char(out, (WCHAR)decoded);
				else
					put_range(out, at, at + 3);
				at += 2;
			}
			else
				put_text(out, L"%25");
		}
		else if (is_forbidden(*at))
			put_hex(out, *at, FALSE);
		else
			put_char(out, *at);
	}
}

/* A DOS path opens every escape; a caret quotes a pipe and angle brackets are not allowed. */
static BOOL put_dos_path(text_builder *out, const WCHAR *start, const WCHAR *end, BOOL decode, BOOL lenient)
{
	const WCHAR *at;
	for (at = start; at < end; at++)
	{
		if (!lenient && (*at == L'<' || *at == L'>' || *at == L'"'))
			return FALSE;
		if (*at == L'/')
			put_char(out, L'\\');
		else if (decode && *at == L'%' && end - at >= 3 && is_valid_escape(at))
		{
			put_char(out, (WCHAR)(hex_value(at[1]) * 16 + hex_value(at[2])));
			at += 2;
		}
		else
			put_char(out, *at);
	}
	return TRUE;
}

/* Splits off "?query" and "#fragment" into the URI, leaving the path's end. */
static HRESULT parse_tail(uri_data *uri, const WCHAR *start, const WCHAR **path_end, BOOL known, BOOL split_fragment_first)
{
	const WCHAR *end = start + wcslen(start), *query = NULL, *fragment = NULL, *at;
	BOOL no_fragment = (uri->flags & FLAG(NOFRAG)) != 0;
	BOOL no_decode = (uri->flags & FLAG(NO_DECODE_EXTRA_INFO)) != 0;
	BOOL encode = !(uri->flags & FLAG(NO_ENCODE_FORBIDDEN_CHARACTERS));
	for (at = start; at < end; at++)
	{
		if (*at == L'?' && !query)
			query = at;
		else if (*at == L'#' && !no_fragment && (query || split_fragment_first))
		{
			fragment = at;
			break;
		}
	}
	*path_end = query ? query : fragment ? fragment : end;
	if (query)
	{
		const WCHAR *stop = fragment ? fragment : end;
		if (known && !no_decode)
		{
			text_builder out = {0};
			if (!put_canonical(&out, query, stop, encode))
			{
				uri_free(out.text);
				return E_INVALIDARG;
			}
			uri->query = out.text;
		}
		else
			uri->query = uri_text_range(query, stop);
		if (!uri->query)
			return E_OUTOFMEMORY;
	}
	if (fragment)
	{
		if (known && !no_decode)
		{
			text_builder out = {0};
			if (!put_canonical(&out, fragment, end, encode))
			{
				uri_free(out.text);
				return E_INVALIDARG;
			}
			uri->fragment = out.text;
		}
		else
			uri->fragment = uri_text_range(fragment, end);
		if (!uri->fragment)
			return E_OUTOFMEMORY;
	}
	return S_OK;
}

/* "user:password@" in front of a host; known schemes clean both up. */
static HRESULT parse_user_info(uri_data *uri, const WCHAR *start, const WCHAR *at_sign, BOOL known)
{
	const WCHAR *colon = start;
	BOOL encode = !(uri->flags & FLAG(NO_ENCODE_FORBIDDEN_CHARACTERS));
	while (colon < at_sign && *colon != L':')
		colon++;
	if (known)
	{
		text_builder user = {0}, password = {0};
		if (!put_canonical(&user, start, colon, encode) || (colon < at_sign && !put_canonical(&password, colon + 1, at_sign, encode)))
		{
			uri_free(user.text);
			uri_free(password.text);
			return E_INVALIDARG;
		}
		uri->user = user.text ? user.text : uri_text_copy(L"");
		if (colon < at_sign)
			uri->password = password.text ? password.text : uri_text_copy(L"");
	}
	else
	{
		uri->user = uri_text_range(start, colon);
		if (colon < at_sign)
			uri->password = uri_text_range(colon + 1, at_sign);
	}
	return uri->user ? S_OK : E_OUTOFMEMORY;
}

static HRESULT parse_authority(uri_data *uri, const WCHAR *start, const WCHAR **after, BOOL known, BOOL canonicalize, DWORD default_port)
{
	const WCHAR *stops = known ? L"/\\?#" : L"/?#";
	const WCHAR *end = find_any(start, stops), *at_sign = NULL, *at;
	HRESULT result;
	for (at = start; at < end; at++)
		if (*at == L'@')
			at_sign = at;
	if (at_sign)
	{
		result = parse_user_info(uri, start, at_sign, known);
		if (FAILED(result))
			return result;
		start = at_sign + 1;
	}
	if (known && !canonicalize && *end == L'\\')
		return E_INVALIDARG;
	*after = end;
	return parse_host(uri, start, end, known, canonicalize, default_port);
}

static HRESULT finish_generic_path(uri_data *uri, const WCHAR *start, const WCHAR *end, BOOL canonicalize)
{
	text_builder out = {0};
	const WCHAR *at;
	BOOL encode = !(uri->flags & FLAG(NO_ENCODE_FORBIDDEN_CHARACTERS));
	text_builder clean = {0};
	for (at = start; at < end; at++)
		put_char(&clean, *at == L'\\' ? L'/' : *at);
	if (clean.text && !put_canonical(&out, clean.text, clean.text + clean.length, encode))
	{
		uri_free(clean.text);
		uri_free(out.text);
		return E_INVALIDARG;
	}
	uri_free(clean.text);
	if (canonicalize)
	{
		if (!out.text)
			put_char(&out, L'/');
		remove_dots(out.text, L"/", out.text[0] == L'/');
		if (!out.text[0])
			put_char(&out, L'/');
	}
	uri->path = out.text ? out.text : uri_text_copy(L"");
	return uri->path ? S_OK : E_OUTOFMEMORY;
}

static HRESULT parse_file(uri_data *uri, const WCHAR *start, BOOL implicit, BOOL canonicalize)
{
	BOOL dos = (uri->flags & FLAG(FILE_USE_DOS_PATH)) != 0;
	BOOL authority = implicit || (is_slash(start[0]) && is_slash(start[1]));
	const WCHAR *after = start, *end, *path_start;
	text_builder out = {0};
	BOOL decode = !implicit, drive_in_authority = FALSE;
	HRESULT result;
	uri->host_type = Uri_HOST_UNKNOWN;
	uri->has_port = FALSE;
	uri->port = 0;
	if (authority)
	{
		if (implicit && is_drive(start))
		{
			drive_in_authority = TRUE;
		}
		else
		{
			after = start + 2;
			if (is_drive(after))
			{
				start = after;
				drive_in_authority = TRUE;
			}
			else if (is_slash(*after))
			{
				start = after + 1;
				if (!is_drive(start))
					start = after;
			}
			else
			{
				const WCHAR *stop = find_any(after, L"/\\?#");
				if (stop - after != 9 || _wcsnicmp(after, L"localhost", 9))
				{
					text_builder host = {0};
					put_range(&host, after, stop);
					if (host.text)
					{
						lower_text(host.text);
						uri->host = host.text;
						uri->host_type = Uri_HOST_DNS;
					}
				}
				start = stop;
			}
		}
	}
	result = parse_tail(uri, start, &end, FALSE, FALSE);
	if (FAILED(result))
		return result;
	path_start = start;
	uri->has_authority = authority;
	if (dos)
	{
		BOOL drive = is_drive(path_start);
		if (authority && uri->host)
			put_dos_path(&out, path_start, end, decode, (uri->flags & URI_INTERNAL_COMBINED) != 0);
		else
		{
			if (drive)
			{
				put_char(&out, path_start[0]);
				put_char(&out, L':');
				path_start += 2;
			}
			if (!put_dos_path(&out, path_start, end, decode, (uri->flags & URI_INTERNAL_COMBINED) != 0))
			{
				uri_free(out.text);
				return E_INVALIDARG;
			}
		}
		uri->path = out.text ? out.text : uri_text_copy(L"");
		return uri->path ? S_OK : E_OUTOFMEMORY;
	}
	if (!authority)
	{
		/* Without slashes the text stays relative to nothing: only separators change. */
		const WCHAR *at;
		for (at = path_start; at < end; at++)
			put_char(&out, *at == L'\\' ? L'/' : *at);
		uri->path = out.text ? out.text : uri_text_copy(L"");
		return uri->path ? S_OK : E_OUTOFMEMORY;
	}
	if (is_drive(path_start))
	{
		put_char(&out, L'/');
		put_char(&out, path_start[0]);
		put_char(&out, L':');
		path_start += 2;
	}
	put_file_path(&out, path_start, end, drive_in_authority);
	if (canonicalize && out.text)
		remove_dots(out.text, L"/", out.text[0] == L'/');
	else if (!out.text)
		put_char(&out, L'/');
	uri->path = out.text;
	return uri->path ? S_OK : E_OUTOFMEMORY;
}

/* The moniker scheme's path has both separators and "::" inside one segment. */
static HRESULT parse_mk(uri_data *uri, const WCHAR *start, BOOL canonicalize)
{
	text_builder out = {0};
	const WCHAR *at;
	for (at = start; *at; at++)
	{
		if (*at == L' ')
			put_text(&out, L"%20");
		else
			put_char(&out, *at);
	}
	if (!out.text)
		put_char(&out, 0);
	if (canonicalize && out.text)
		remove_dots(out.text, L"/\\", FALSE);
	uri->path = out.text ? out.text : uri_text_copy(L"");
	return uri->path ? S_OK : E_OUTOFMEMORY;
}

static HRESULT parse_res(uri_data *uri, const WCHAR *start)
{
	const WCHAR *stop, *end, *at;
	HRESULT result;
	stop = find_any(start, L"/?#");
	for (at = start; at < stop; at++)
		if (*at == L'<' || *at == L'>' || *at == L'"')
			return E_INVALIDARG;
	uri->host = uri_text_range(start, stop);
	uri->host_type = Uri_HOST_UNKNOWN;
	result = parse_tail(uri, stop, &end, FALSE, TRUE);
	if (FAILED(result))
		return result;
	for (at = stop; at < end; at++)
		if (*at == L'%' && (end - at < 3 || !is_valid_escape(at)))
			return E_INVALIDARG;
	uri->path = uri_text_range(stop, end);
	uri->has_authority = TRUE;
	return uri->path && uri->host ? S_OK : E_OUTOFMEMORY;
}

/* The composed forms of the parts. */
static HRESULT compose(uri_data *uri, BOOL canonicalize, BOOL known)
{
	text_builder absolute = {0}, display = {0}, *targets[2] = {&absolute, &display};
	BOOL dos = (uri->flags & FLAG(FILE_USE_DOS_PATH)) && uri->scheme_type == URL_SCHEME_FILE;
	int index;
	if (uri->host && uri->host_type == Uri_HOST_DNS)
	{
		const WCHAR *label;
		BOOL idn = !is_all_ascii(uri->host, uri->host + wcslen(uri->host)) || !_wcsnicmp(uri->host, L"xn--", 4);
		for (label = uri->host; !idn && (label = wcschr(label, L'.')) != NULL; label++)
			idn = !_wcsnicmp(label + 1, L"xn--", 4);
		if (idn)
			uri->host_type = Uri_HOST_IDN;
	}
	for (index = 0; index < 2; index++)
	{
		text_builder *out = targets[index];
		const WCHAR *host = uri->host;
		if (uri->scheme)
		{
			put_text(out, uri->scheme);
			put_char(out, L':');
		}
		if (uri->host_after_scheme)
		{
			/* The wildcard scheme writes its host straight after the colon. */
			if (host)
				put_text(out, host);
		}
		else if (uri->has_authority)
		{
			put_text(out, L"//");
			if (dos && host && host[0])
				put_text(out, L"\\\\");
			if (uri->user && uri->show_user_info && (!known || !index))
			{
				BOOL hide = known && !uri->user[0] && (!uri->password || !uri->password[0]);
				if (!hide)
				{
					put_text(out, uri->user);
					if (uri->password)
					{
						put_char(out, L':');
						put_text(out, uri->password);
					}
					put_char(out, L'@');
				}
			}
			if (host)
			{
				if (uri->host_in_brackets)
					put_char(out, L'[');
				put_text(out, host);
				if (uri->host_in_brackets)
					put_char(out, L']');
			}
			if (uri->port_text)
			{
				put_char(out, L':');
				put_text(out, uri->port_text);
			}
		}
		if (uri->path)
			put_text(out, uri->path);
		if (uri->query)
			put_text(out, uri->query);
		if (uri->fragment)
			put_text(out, uri->fragment);
	}
	if (absolute.failed || display.failed)
		return E_OUTOFMEMORY;
	uri->absolute = absolute.text ? absolute.text : uri_text_copy(L"");
	uri->display = display.text ? display.text : uri_text_copy(L"");
	/* A known hierarchical or file scheme without its slashes names nothing absolute yet. */
	return S_OK;
}

void uri_clear(uri_data *uri)
{
	uri_free(uri->raw);
	uri_free(uri->scheme);
	uri_free(uri->user);
	uri_free(uri->password);
	uri_free(uri->host);
	uri_free(uri->port_text);
	uri_free(uri->path);
	uri_free(uri->query);
	uri_free(uri->fragment);
	uri_free(uri->absolute);
	uri_free(uri->display);
	memset(uri, 0, sizeof(*uri));
}

HRESULT uri_parse(const WCHAR *text, DWORD flags, uri_data *uri)
{
	BOOL canonicalize;
	BOOL implicit_file = FALSE, implicit_wildcard = FALSE, relative = FALSE, known = FALSE;
	const WCHAR *cursor, *end_of_scheme, *path_end;
	scheme_kind kind = KIND_UNKNOWN;
	DWORD default_port = 0;
	HRESULT result = S_OK;
	canonicalize = !(flags & FLAG(NO_CANONICALIZE));
	memset(uri, 0, sizeof(*uri));
	uri->flags = flags;
	uri->raw = flags & FLAG(NO_PRE_PROCESS_HTML_URI) ? uri_text_copy(text) : preprocess(text);
	if (!uri->raw)
		return E_OUTOFMEMORY;
	cursor = uri->raw;
	uri->host_type = Uri_HOST_UNKNOWN;
	uri->show_user_info = TRUE;
	end_of_scheme = scheme_end(cursor);
	if (end_of_scheme)
	{
		WCHAR *name = uri_text_range(cursor, end_of_scheme);
		int index;
		if (!name)
			return E_OUTOFMEMORY;
		lower_text(name);
		uri->scheme = name;
		if (!wcscmp(name, L"*"))
		{
			uri->scheme_type = URL_SCHEME_WILDCARD;
			kind = KIND_WILDCARD;
		}
		else
		{
			index = find_scheme(name);
			uri->scheme_type = index < 0 ? URL_SCHEME_UNKNOWN : known_schemes[index].type;
			if (index >= 0)
			{
				kind = known_schemes[index].kind;
				default_port = known_schemes[index].port;
			}
		}
		cursor = end_of_scheme + 1;
	}
	else if ((flags & FLAG(ALLOW_IMPLICIT_FILE_SCHEME)) && ((is_drive(cursor) && is_slash(cursor[2])) || (is_slash(cursor[0]) && is_slash(cursor[1]) && cursor[0] == L'\\')))
	{
		uri->scheme = uri_text_copy(L"file");
		uri->scheme_type = URL_SCHEME_FILE;
		kind = KIND_FILE;
		implicit_file = TRUE;
	}
	else if ((flags & FLAG(ALLOW_IMPLICIT_WILDCARD_SCHEME)) && !is_drive(cursor) && cursor[0] != L'\\' && cursor[0])
	{
		uri->scheme = uri_text_copy(L"*");
		uri->scheme_type = URL_SCHEME_WILDCARD;
		kind = KIND_WILDCARD;
		implicit_wildcard = TRUE;
	}
	else if ((flags & FLAG(ALLOW_RELATIVE)) && cursor[0] != L'\\' && !(cursor[0] == L':' && cursor[1] == L'/'))
	{
		relative = TRUE;
		uri->scheme_type = URL_SCHEME_UNKNOWN;
	}
	else
		return E_INVALIDARG;
	if (uri->scheme == NULL && !relative)
		return E_OUTOFMEMORY;
	known = kind == KIND_HIERARCHICAL || kind == KIND_OPAQUE;
	switch (kind)
	{
	case KIND_FILE:
		result = parse_file(uri, cursor, implicit_file, canonicalize);
		break;
	case KIND_RES:
		if (cursor[0] == L'/' && cursor[1] == L'/')
			result = parse_res(uri, cursor + 2);
		else
		{
			result = parse_tail(uri, cursor, &path_end, FALSE, TRUE);
			if (SUCCEEDED(result))
				uri->path = uri_text_range(cursor, path_end);
		}
		break;
	case KIND_MK:
		result = parse_mk(uri, cursor, canonicalize);
		uri->has_authority = FALSE;
		break;
	case KIND_RAW_OPAQUE:
		uri->path = uri_text_copy(cursor);
		break;
	case KIND_OPAQUE:
		if (cursor[0] == L'/' && cursor[1] == L'/')
			cursor += 2;
		result = parse_tail(uri, cursor, &path_end, TRUE, TRUE);
		if (SUCCEEDED(result))
		{
			text_builder out = {0};
			BOOL encode = !(flags & FLAG(NO_ENCODE_FORBIDDEN_CHARACTERS));
			if (!put_canonical(&out, cursor, path_end, encode))
				result = E_INVALIDARG;
			uri->path = out.text ? out.text : uri_text_copy(L"");
			if (FAILED(result))
			{
				uri_free(uri->path);
				uri->path = NULL;
			}
		}
		break;
	case KIND_WILDCARD:
		if (implicit_wildcard && cursor[0] == L'/' && cursor[1] == L'/')
		{
			/* A network path with no scheme of its own keeps its text as a path. */
			text_builder out = {0};
			result = parse_tail(uri, cursor, &path_end, TRUE, TRUE);
			if (SUCCEEDED(result))
			{
				put_canonical(&out, cursor, path_end, TRUE);
				uri->path = out.text;
				uri->host = uri_text_copy(L"");
			}
			uri->host_after_scheme = TRUE;
			break;
		}
		if (cursor[0] == L'/' && cursor[1] == L'/')
			cursor += 2;
		{
			const WCHAR *after = find_any(cursor, L"/?#");
			result = parse_host(uri, cursor, after, FALSE, FALSE, 0);
			uri->has_port = FALSE;
			if (SUCCEEDED(result))
				result = parse_tail(uri, after, &path_end, FALSE, TRUE);
			if (SUCCEEDED(result))
				uri->path = path_end > after ? uri_text_range(after, path_end) : uri_text_copy(L"/");
			uri->host_after_scheme = TRUE;
		}
		break;
	case KIND_HIERARCHICAL:
		if (is_slash(cursor[0]) && is_slash(cursor[1]))
		{
			const WCHAR *after;
			uri->has_authority = TRUE;
			result = parse_authority(uri, cursor + 2, &after, TRUE, canonicalize, default_port);
			if (SUCCEEDED(result))
				result = parse_tail(uri, after, &path_end, TRUE, TRUE);
			if (SUCCEEDED(result))
				result = finish_generic_path(uri, after, path_end, canonicalize);
		}
		else
		{
			/* "http:name" has no host: only the path's separators settle. */
			text_builder out = {0};
			const WCHAR *at;
			uri->port = default_port;
			uri->has_port = TRUE;
			result = parse_tail(uri, cursor, &path_end, TRUE, TRUE);
			for (at = cursor; SUCCEEDED(result) && at < path_end; at++)
				put_char(&out, *at == L'\\' ? L'/' : *at);
			uri->path = out.text ? out.text : uri_text_copy(L"");
		}
		break;
	case KIND_UNKNOWN:
		if (relative && cursor[0] != L'/' && !cursor[0])
		{
			uri->path = uri_text_copy(L"");
			break;
		}
		if (cursor[0] == L'/' && cursor[1] == L'/' && (flags & FLAG(NO_CRACK_UNKNOWN_SCHEMES)) && !relative)
		{
			/* Left uncracked, the slashes must not read as an authority. */
			text_builder out = {0};
			result = parse_tail(uri, cursor, &path_end, FALSE, TRUE);
			put_text(&out, L"/.");
			put_range(&out, cursor, path_end);
			uri->path = out.text;
			break;
		}
		if (cursor[0] == L'/' && cursor[1] == L'/')
		{
			const WCHAR *after;
			uri->has_authority = TRUE;
			result = parse_authority(uri, cursor + 2, &after, FALSE, FALSE, 0);
			if (SUCCEEDED(result))
				result = parse_tail(uri, after, &path_end, FALSE, TRUE);
			if (SUCCEEDED(result))
			{
				uri->path = uri_text_range(after, path_end);
				if (uri->path && canonicalize)
				{
					text_builder out = {0};
					put_text(&out, uri->path[0] ? uri->path : L"/");
					remove_dots(out.text, L"/", out.text[0] == L'/');
					if (!out.text[0])
						put_char(&out, L'/');
					uri_free(uri->path);
					uri->path = out.text;
				}
			}
			break;
		}
		result = parse_tail(uri, cursor, &path_end, FALSE, TRUE);
		if (SUCCEEDED(result))
			uri->path = uri_text_range(cursor, path_end);
		break;
	}
	if (FAILED(result))
		return result;
	if (!uri->path)
		uri->path = uri_text_copy(L"");
	if (!uri->path)
		return E_OUTOFMEMORY;
	result = compose(uri, canonicalize, known);
	uri->flags &= ~URI_INTERNAL_COMBINED;
	return result;
}

WCHAR *uri_host_as_ascii(const WCHAR *host)
{
	text_builder out = {0};
	if (!put_host_labels(&out, host, host + wcslen(host)))
	{
		uri_free(out.text);
		return NULL;
	}
	return out.text ? out.text : uri_text_copy(L"");
}

WCHAR *uri_host_as_unicode(const WCHAR *host)
{
	return host_for_display(host);
}

WCHAR *uri_domain_of(const WCHAR *host)
{
	static const WCHAR *const second_level[] = {L"com", L"edu", L"gov", L"mil", L"net", L"org", L"co", L"ac"};
	const WCHAR *last = wcsrchr(host, L'.'), *second = NULL, *third = NULL, *at;
	SIZE_T index;
	if (!last)
		return NULL;
	for (at = last - 1; at >= host; at--)
		if (*at == L'.')
		{
			second = at;
			break;
		}
	if (wcslen(last + 1) == 2)
	{
		const WCHAR *start = second ? second + 1 : host;
		SIZE_T length = last - start;
		for (index = 0; index < ARRAYSIZE(second_level); index++)
		{
			if (wcslen(second_level[index]) != length || _wcsnicmp(start, second_level[index], length))
				continue;
			/* A registry's own name has nothing registered in it. */
			if (!second)
				return NULL;
			for (at = second - 1; at >= host; at--)
				if (*at == L'.')
				{
					third = at;
					break;
				}
			return uri_text_copy(third ? third + 1 : host);
		}
	}
	return uri_text_copy(second ? second + 1 : host);
}
