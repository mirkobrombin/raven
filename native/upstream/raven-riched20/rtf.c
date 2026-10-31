/* SPDX-License-Identifier: MIT */
#include "re.h"

// ---- reading ----

typedef struct
{
	WCHAR face[LF_FACESIZE];
	BYTE charset, pitch;
	INT number;
} rtf_font;

// What a group inherits and restores when it closes.
typedef struct
{
	CHARFORMAT2W format;
	PARAFORMAT2 para;
	INT skip_count;		// characters still to drop after a \u
	INT unicode_skip;	// \uc
	UINT code_page;
	BOOL skip;		// the destination is not text
	INT destination;
} rtf_state;

enum
{
	DEST_TEXT,
	DEST_FONTS,
	DEST_COLORS,
	DEST_IGNORE,
	DEST_NUMBERING,
	DEST_NUMBERING_TEXT
};

typedef struct
{
	EDITSTREAM *stream;
	BYTE buffer[4096];
	LONG have, at;
	BOOL finished;
	LONG total;

	re_editor *editor;
	rtf_state *states;
	INT depth, capacity;
	rtf_font *fonts;
	INT font_count;
	COLORREF *colors;
	INT color_count;
	COLORREF building;
	INT font_being_read;
	// A paragraph's numbering is described once, and counts only where a \pntext label says so.
	PARAFORMAT2 numbering;
	BOOL numbering_known, numbering_used;
	UINT default_code_page;
	INT default_font;
	WCHAR *text;
	INT text_used, text_capacity;
	CHARFORMAT2W text_format;
	BOOL text_format_set;
	INT position;
	BOOL para_touched;
	INT limit_left;
} rtf_reader;

static INT next_byte(rtf_reader *reader)
{
	if (reader->at >= reader->have) {
		if (reader->finished)
			return -1;
		LONG got = 0;
		DWORD error = reader->stream->pfnCallback(reader->stream->dwCookie, reader->buffer, sizeof(reader->buffer), &got);
		if (error) {
			reader->stream->dwError = error;
			reader->finished = TRUE;
			return -1;
		}
		if (got <= 0) {
			reader->finished = TRUE;
			return -1;
		}
		reader->have = got;
		reader->at = 0;
	}
	reader->total++;
	return reader->buffer[reader->at++];
}

static void put_back(rtf_reader *reader)
{
	reader->at--;
	reader->total--;
}

static void flush_text(rtf_reader *reader)
{
	if (!reader->text_used)
		return;
	re_editor *editor = reader->editor;
	re_doc *doc = &editor->doc;
	INT length = reader->text_used;
	if (length > reader->limit_left)
		length = reader->limit_left;
	if (length > 0) {
		re_doc_replace(doc, reader->position, 0, reader->text, length, &reader->text_format);
		reader->position += length;
		reader->limit_left -= length;
	}
	reader->text_used = 0;
}

static void emit_wide(rtf_reader *reader, WCHAR c)
{
	rtf_state *state = &reader->states[reader->depth];
	if (state->destination == DEST_IGNORE || state->destination == DEST_NUMBERING || state->destination == DEST_NUMBERING_TEXT)
		return;
	if (reader->text_used && memcmp(&reader->text_format, &state->format, sizeof(state->format)))
		flush_text(reader);
	if (!reader->text_used)
		reader->text_format = state->format;
	if (reader->text_used == reader->text_capacity) {
		INT capacity = reader->text_capacity ? reader->text_capacity * 2 : 256;
		WCHAR *grown = re_realloc(reader->text, capacity * sizeof(WCHAR));
		if (!grown)
			return;
		reader->text = grown;
		reader->text_capacity = capacity;
	}
	reader->text[reader->text_used++] = c;
}

static void emit_para_settings(rtf_reader *reader, INT para_index)
{
	reader->editor->doc.paras[para_index].format = reader->states[reader->depth].para;
}

static void emit_para(rtf_reader *reader)
{
	emit_wide(reader, RE_CR);
	flush_text(reader);
	re_doc *doc = &reader->editor->doc;
	emit_para_settings(reader, re_doc_para_at(doc, reader->position - 1));
	reader->para_touched = FALSE;
}

static void group_open(rtf_reader *reader)
{
	if (reader->depth + 1 == reader->capacity) {
		INT capacity = reader->capacity * 2;
		rtf_state *grown = re_realloc(reader->states, capacity * sizeof(rtf_state));
		if (!grown)
			return;
		reader->states = grown;
		reader->capacity = capacity;
	}
	reader->states[reader->depth + 1] = reader->states[reader->depth];
	reader->depth++;
}

static void apply_numbering(rtf_reader *reader, rtf_state *state)
{
	if (!reader->numbering_known || !reader->numbering_used)
		return;
	state->para.wNumbering = reader->numbering.wNumbering;
	state->para.wNumberingStart = reader->numbering.wNumberingStart;
	state->para.wNumberingStyle = reader->numbering.wNumberingStyle;
	state->para.wNumberingTab = reader->numbering.wNumberingTab;
}

static void group_close(rtf_reader *reader)
{
	if (reader->depth > 0) {
		flush_text(reader);
		if (reader->states[reader->depth].destination == DEST_NUMBERING && reader->states[reader->depth - 1].destination != DEST_NUMBERING)
			apply_numbering(reader, &reader->states[reader->depth - 1]);
		reader->depth--;
	}
}

static UINT code_page_of_charset(BYTE charset)
{
	CHARSETINFO info;
	if (charset == DEFAULT_CHARSET || charset == ANSI_CHARSET)
		return 1252;
	if (charset == SYMBOL_CHARSET)
		return 1252;
	if (TranslateCharsetInfo((DWORD *)(ULONG_PTR)charset, &info, TCI_SRCCHARSET))
		return info.ciACP;
	return CP_ACP;
}

static void emit_byte(rtf_reader *reader, BYTE value)
{
	rtf_state *state = &reader->states[reader->depth];
	if (state->skip_count > 0) {
		state->skip_count--;
		return;
	}
	if (state->destination == DEST_IGNORE)
		return;
	if (state->destination == DEST_FONTS) {
		if (reader->font_being_read >= 0) {
			rtf_font *font = &reader->fonts[reader->font_being_read];
			INT used = lstrlenW(font->face);
			if (value != ';' && used + 1 < LF_FACESIZE) {
				font->face[used] = value;
				font->face[used + 1] = 0;
			}
		}
		return;
	}
	if (state->destination == DEST_NUMBERING_TEXT) {
		if (value == '.')
			reader->numbering.wNumberingStyle = PFNS_PERIOD;
		else if (value == ')' && reader->numbering.wNumberingStyle != PFNS_PARENS)
			reader->numbering.wNumberingStyle = PFNS_PAREN;
		else if (value == '(')
			reader->numbering.wNumberingStyle = PFNS_PARENS;
		return;
	}
	if (state->destination != DEST_TEXT)
		return;
	WCHAR wide = value;
	if (value >= 0x80) {
		char narrow = (char)value;
		MultiByteToWideChar(state->code_page, 0, &narrow, 1, &wide, 1);
	}
	emit_wide(reader, wide);
}

typedef struct
{
	const char *name;
	WCHAR value;
} rtf_symbol;

static const rtf_symbol symbols[] = {
	{"emdash", 0x2014}, {"endash", 0x2013}, {"bullet", 0x2022}, {"lquote", 0x2018}, {"rquote", 0x2019},
	{"ldblquote", 0x201c}, {"rdblquote", 0x201d}, {"emspace", 0x2003}, {"enspace", 0x2002}, {"qmspace", 0x2005},
	{"zwj", 0x200d}, {"zwnj", 0x200c}, {"ltrmark", 0x200e}, {"rtlmark", 0x200f}, {"tab", RE_TAB}, {"line", RE_VT},
};

static const char *const ignored_destinations[] = {
	"stylesheet", "info", "pict", "object", "header", "headerl", "headerr", "headerf", "footer", "footerl", "footerr", "footerf",
	"footnote", "annotation", "fldinst", "listtable", "listoverridetable", "rsidtbl", "xmlnstbl",
	"themedata", "colorschememapping", "latentstyles", "datastore", "generator", "mmathPr", "nonesttables", "filetbl", "revtbl",
};

static void set_effect(CHARFORMAT2W *format, DWORD effect, BOOL on)
{
	format->dwEffects = on ? format->dwEffects | effect : format->dwEffects & ~effect;
}

static void plain_format(rtf_reader *reader, rtf_state *state)
{
	CHARFORMAT2W base;
	re_default_char_format(&base, reader->editor->set_font);
	if (reader->default_font < reader->font_count && reader->font_count) {
		lstrcpyW(base.szFaceName, reader->fonts[reader->default_font].face);
		base.bCharSet = reader->fonts[reader->default_font].charset;
		base.bPitchAndFamily = reader->fonts[reader->default_font].pitch;
	}
	base.yHeight = 24 * 10;
	state->format = base;
}

static rtf_font *font_numbered(rtf_reader *reader, INT number)
{
	for (INT i = 0; i < reader->font_count; i++) {
		if (reader->fonts[i].number == number)
			return &reader->fonts[i];
	}
	return NULL;
}

static COLORREF color_at(rtf_reader *reader, INT index, BOOL *automatic)
{
	if (index < 0 || index >= reader->color_count || !reader->colors) {
		*automatic = TRUE;
		return 0;
	}
	*automatic = reader->colors[index] == (COLORREF)-1;
	return *automatic ? 0 : reader->colors[index];
}

static void control_word(rtf_reader *reader, const char *word, BOOL has_value, INT value)
{
	rtf_state *state = &reader->states[reader->depth];
#define IS(name) (!lstrcmpA(word, name))
	if (state->destination == DEST_FONTS) {
		if (IS("f")) {
			reader->font_being_read = -1;
			rtf_font *grown = re_realloc(reader->fonts, (reader->font_count + 1) * sizeof(rtf_font));
			if (grown) {
				reader->fonts = grown;
				memset(&grown[reader->font_count], 0, sizeof(rtf_font));
				grown[reader->font_count].number = value;
				grown[reader->font_count].charset = DEFAULT_CHARSET;
				reader->font_being_read = reader->font_count++;
			}
		} else if (reader->font_being_read >= 0) {
			rtf_font *font = &reader->fonts[reader->font_being_read];
			if (IS("fcharset"))
				font->charset = (BYTE)value;
			else if (IS("fnil"))
				font->pitch = (font->pitch & 3) | FF_DONTCARE;
			else if (IS("froman"))
				font->pitch = (font->pitch & 3) | FF_ROMAN;
			else if (IS("fswiss"))
				font->pitch = (font->pitch & 3) | FF_SWISS;
			else if (IS("fmodern"))
				font->pitch = (font->pitch & 3) | FF_MODERN;
			else if (IS("fscript"))
				font->pitch = (font->pitch & 3) | FF_SCRIPT;
			else if (IS("fdecor"))
				font->pitch = (font->pitch & 3) | FF_DECORATIVE;
			else if (IS("fprq"))
				font->pitch = (font->pitch & ~3) | (value == 1 ? FIXED_PITCH : value == 2 ? VARIABLE_PITCH : DEFAULT_PITCH);
		}
		return;
	}
	if (state->destination == DEST_COLORS) {
		if (IS("red"))
			reader->building = (reader->building & 0xffff00) | (value & 255);
		else if (IS("green"))
			reader->building = (reader->building & 0xff00ff) | (value & 255) << 8;
		else if (IS("blue"))
			reader->building = (reader->building & 0x00ffff) | (value & 255) << 16;
		return;
	}
	if (state->destination == DEST_IGNORE)
		return;
	if (state->destination == DEST_NUMBERING) {
		PARAFORMAT2 *numbering = &reader->numbering;
		if (IS("pnlvlbody") || IS("pndec"))
			numbering->wNumbering = PFN_ARABIC;
		else if (IS("pnlvlblt"))
			numbering->wNumbering = PFN_BULLET;
		else if (IS("pnlcltr"))
			numbering->wNumbering = PFN_LCLETTER;
		else if (IS("pnucltr"))
			numbering->wNumbering = PFN_UCLETTER;
		else if (IS("pnlcrm"))
			numbering->wNumbering = PFN_LCROMAN;
		else if (IS("pnucrm"))
			numbering->wNumbering = PFN_UCROMAN;
		else if (IS("pnstart"))
			numbering->wNumberingStart = (WORD)value;
		else if (IS("pnindent"))
			numbering->wNumberingTab = (WORD)value;
		else if (IS("pntxta") || IS("pntxtb"))
			state->destination = DEST_NUMBERING_TEXT;
		return;
	}
	if (IS("rtf")) {
		return;
	} else if (IS("ansi")) {
		reader->default_code_page = 1252;
	} else if (IS("mac")) {
		reader->default_code_page = 10000;
	} else if (IS("pc")) {
		reader->default_code_page = 437;
	} else if (IS("pca")) {
		reader->default_code_page = 850;
	} else if (IS("ansicpg")) {
		reader->default_code_page = value;
		state->code_page = value;
	} else if (IS("deff")) {
		reader->default_font = value;
	} else if (IS("fonttbl")) {
		state->destination = DEST_FONTS;
		reader->font_being_read = -1;
	} else if (IS("colortbl")) {
		state->destination = DEST_COLORS;
		reader->building = 0;
	} else if (IS("par") || IS("sect") || IS("page")) {
		emit_para(reader);
	} else if (IS("cell")) {
		emit_wide(reader, RE_TAB);
	} else if (IS("row")) {
		emit_para(reader);
	} else if (IS("pard")) {
		re_default_para_format(&state->para);
	} else if (IS("plain")) {
		plain_format(reader, state);
	} else if (IS("b")) {
		set_effect(&state->format, CFE_BOLD, !has_value || value);
		state->format.wWeight = !has_value || value ? FW_BOLD : FW_NORMAL;
	} else if (IS("i")) {
		set_effect(&state->format, CFE_ITALIC, !has_value || value);
	} else if (IS("ul")) {
		set_effect(&state->format, CFE_UNDERLINE, !has_value || value);
		state->format.bUnderlineType = CFU_UNDERLINE;
	} else if (IS("uld") || IS("uldb") || IS("ulw") || IS("uldash") || IS("uldashd") || IS("uldashdd") || IS("ulwave") || IS("ulth")) {
		set_effect(&state->format, CFE_UNDERLINE, TRUE);
		state->format.bUnderlineType = IS("uld") ? CFU_UNDERLINEDOTTED : IS("uldb") ? CFU_UNDERLINEDOUBLE : IS("ulw") ? CFU_UNDERLINEWORD : IS("uldash") ? CFU_UNDERLINEDASH
			: IS("uldashd") ? CFU_UNDERLINEDASHDOT : IS("uldashdd") ? CFU_UNDERLINEDASHDOTDOT : IS("ulwave") ? CFU_UNDERLINEWAVE : CFU_UNDERLINETHICK;
	} else if (IS("ulnone")) {
		set_effect(&state->format, CFE_UNDERLINE, FALSE);
	} else if (IS("strike")) {
		set_effect(&state->format, CFE_STRIKEOUT, !has_value || value);
	} else if (IS("protect")) {
		set_effect(&state->format, CFE_PROTECTED, !has_value || value);
	} else if (IS("v")) {
		set_effect(&state->format, CFE_HIDDEN, !has_value || value);
	} else if (IS("sub")) {
		state->format.dwEffects = (state->format.dwEffects & ~CFE_SUPERSCRIPT) | CFE_SUBSCRIPT;
	} else if (IS("super")) {
		state->format.dwEffects = (state->format.dwEffects & ~CFE_SUBSCRIPT) | CFE_SUPERSCRIPT;
	} else if (IS("nosupersub")) {
		state->format.dwEffects &= ~(CFE_SUBSCRIPT | CFE_SUPERSCRIPT);
	} else if (IS("up")) {
		state->format.yOffset = (has_value ? value : 6) * 10;
	} else if (IS("dn")) {
		state->format.yOffset = -(has_value ? value : 6) * 10;
	} else if (IS("fs")) {
		state->format.yHeight = (has_value ? value : 24) * 10;
	} else if (IS("f")) {
		rtf_font *font = font_numbered(reader, value);
		if (font) {
			lstrcpyW(state->format.szFaceName, font->face);
			state->format.bCharSet = font->charset;
			state->format.bPitchAndFamily = font->pitch;
			if (font->charset != DEFAULT_CHARSET)
				state->code_page = code_page_of_charset(font->charset);
		}
	} else if (IS("cf")) {
		BOOL automatic;
		state->format.crTextColor = color_at(reader, value, &automatic);
		set_effect(&state->format, CFE_AUTOCOLOR, automatic);
	} else if (IS("cb") || IS("highlight")) {
		BOOL automatic;
		state->format.crBackColor = color_at(reader, value, &automatic);
		set_effect(&state->format, CFE_AUTOBACKCOLOR, automatic);
	} else if (IS("li")) {
		state->para.dxStartIndent = value - state->para.dxOffset;
	} else if (IS("ri")) {
		state->para.dxRightIndent = value;
	} else if (IS("fi")) {
		state->para.dxStartIndent += state->para.dxOffset + value;
		state->para.dxOffset = -value;
	} else if (IS("ql") || IS("rtlpar") || IS("ltrpar")) {
		if (IS("ql"))
			state->para.wAlignment = PFA_LEFT;
		if (IS("rtlpar"))
			state->para.wEffects |= PFE_RTLPARA;
		if (IS("ltrpar"))
			state->para.wEffects &= ~PFE_RTLPARA;
	} else if (IS("qr")) {
		state->para.wAlignment = PFA_RIGHT;
	} else if (IS("qc")) {
		state->para.wAlignment = PFA_CENTER;
	} else if (IS("qj")) {
		state->para.wAlignment = PFA_JUSTIFY;
	} else if (IS("sa")) {
		state->para.dySpaceAfter = value;
	} else if (IS("sb")) {
		state->para.dySpaceBefore = value;
	} else if (IS("sl")) {
		state->para.dyLineSpacing = value < 0 ? -value : value;
		state->para.bLineSpacingRule = value < 0 ? 4 : value ? 3 : 0;
	} else if (IS("slmult")) {
		if (value && state->para.bLineSpacingRule == 3)
			state->para.bLineSpacingRule = 5;
	} else if (IS("tx")) {
		if (state->para.cTabCount < MAX_TAB_STOPS)
			state->para.rgxTabs[state->para.cTabCount++] = value;
	} else if (IS("pn")) {
		memset(&reader->numbering, 0, sizeof(reader->numbering));
		reader->numbering_known = TRUE;
		state->destination = DEST_NUMBERING;
	} else if (IS("pntext")) {
		reader->numbering_used = TRUE;
		if (reader->depth)
			apply_numbering(reader, &reader->states[reader->depth - 1]);
		state->destination = DEST_IGNORE;
	} else if (IS("emspace") || IS("enspace")) {
		emit_wide(reader, ' ');
	} else if (IS("bullet")) {
		emit_wide(reader, 0x2022);
	} else if (IS("lquote")) {
		emit_wide(reader, 0x2018);
	} else if (IS("rquote")) {
		emit_wide(reader, 0x2019);
	} else if (IS("ldblquote")) {
		emit_wide(reader, 0x201c);
	} else if (IS("rdblquote")) {
		emit_wide(reader, 0x201d);
	} else if (IS("ltrmark")) {
		emit_wide(reader, 0x200e);
	} else if (IS("rtlmark")) {
		emit_wide(reader, 0x200f);
	} else if (IS("zwj")) {
		emit_wide(reader, 0x200d);
	} else if (IS("zwnj")) {
		emit_wide(reader, 0x200c);
	} else if (IS("u")) {
		INT code = value < 0 ? value + 65536 : value;
		emit_wide(reader, (WCHAR)code);
		state->skip_count = state->unicode_skip;
	} else if (IS("uc")) {
		state->unicode_skip = value;
	} else {
		for (size_t i = 0; i < ARRAYSIZE(symbols); i++) {
			if (IS(symbols[i].name)) {
				emit_wide(reader, symbols[i].value);
				return;
			}
		}
		for (size_t i = 0; i < ARRAYSIZE(ignored_destinations); i++) {
			if (IS(ignored_destinations[i])) {
				state->destination = DEST_IGNORE;
				return;
			}
		}
	}
#undef IS
}

static void parse_rtf(rtf_reader *reader)
{
	BOOL star = FALSE;
	for (;;) {
		INT c = next_byte(reader);
		if (c < 0)
			break;
		// A stray NUL in the stream reads as a blank.
		if (!c)
			c = ' ';
		rtf_state *state = &reader->states[reader->depth];
		if (c == '{') {
			flush_text(reader);
			group_open(reader);
			star = FALSE;
		} else if (c == '}') {
			if (state->destination == DEST_FONTS && reader->depth > 0 && reader->states[reader->depth - 1].destination != DEST_FONTS)
				reader->font_being_read = -1;
			group_close(reader);
			if (reader->depth == 0)
				break;
			star = FALSE;
		} else if (c == '\\') {
			c = next_byte(reader);
			if (c < 0)
				break;
			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
				char word[32];
				INT used = 0;
				BOOL has_value = FALSE, negative = FALSE;
				INT value = 0;
				while ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
					if (used < (INT)sizeof(word) - 1)
						word[used++] = (char)c;
					c = next_byte(reader);
				}
				word[used] = 0;
				if (c == '-') {
					negative = TRUE;
					c = next_byte(reader);
				}
				while (c >= '0' && c <= '9') {
					has_value = TRUE;
					value = value * 10 + (c - '0');
					c = next_byte(reader);
				}
				if (negative)
					value = -value;
				if (c >= 0 && c != ' ')
					put_back(reader);
				// A destination marked as ignorable is dropped unless it is one read here.
				if (star && reader->states[reader->depth].destination == DEST_TEXT && lstrcmpA(word, "pn")) {
					reader->states[reader->depth].destination = DEST_IGNORE;
				} else {
					control_word(reader, word, has_value, value);
				}
				star = FALSE;
			} else if (c == '\'') {
				INT high = next_byte(reader), low = next_byte(reader);
				if (high < 0 || low < 0)
					break;
				BYTE value = 0;
				for (INT i = 0; i < 2; i++) {
					INT digit = i ? low : high;
					value <<= 4;
					if (digit >= '0' && digit <= '9')
						value |= digit - '0';
					else if (digit >= 'a' && digit <= 'f')
						value |= digit - 'a' + 10;
					else if (digit >= 'A' && digit <= 'F')
						value |= digit - 'A' + 10;
				}
				emit_byte(reader, value);
			} else if (c == '*') {
				star = TRUE;
			} else if (c == '\\' || c == '{' || c == '}') {
				emit_byte(reader, (BYTE)c);
			} else if (c == '~') {
				if (state->destination == DEST_TEXT)
					emit_wide(reader, 0xa0);
			} else if (c == '-') {
				if (state->destination == DEST_TEXT)
					emit_wide(reader, 0xad);
			} else if (c == '_') {
				if (state->destination == DEST_TEXT)
					emit_wide(reader, 0x2011);
			} else if (c == RE_CR || c == RE_LF) {
				if (state->destination == DEST_TEXT)
					emit_para(reader);
			}
		} else if (c == RE_CR || c == RE_LF) {
			continue;
		} else if (state->destination == DEST_COLORS) {
			if (c == ';') {
				COLORREF *grown = re_realloc(reader->colors, (reader->color_count + 1) * sizeof(COLORREF));
				if (grown) {
					reader->colors = grown;
					// The first, empty entry stands for the automatic colour and is not in the numbering.
					grown[reader->color_count++] = reader->building;
				}
				reader->building = 0;
			}
		} else {
			emit_byte(reader, (BYTE)c);
		}
	}
	flush_text(reader);
}

static void read_plain(rtf_reader *reader, WPARAM flags)
{
	rtf_state *state = &reader->states[0];
	BOOL unicode = (flags & SF_UNICODE) != 0;
	INT pending = -1;
	UINT code_page = (flags & SF_USECODEPAGE) ? HIWORD(flags) : CP_ACP;
	// A UTF-8 mark names the code page; it may straddle two reads of the stream.
	INT held[3], held_count = 0, held_at = 0;
	if (!unicode) {
		static const BYTE mark[3] = {0xef, 0xbb, 0xbf};
		while (held_count < 3) {
			INT c = next_byte(reader);
			if (c < 0)
				break;
			held[held_count++] = c;
			if (c != mark[held_count - 1])
				break;
		}
		if (held_count == 3 && held[2] == 0xbf) {
			held_count = 0;
			code_page = CP_UTF8;
		}
	}
	UINT sequence = 0;
	INT sequence_left = 0;
	WCHAR low = 0;
	BYTE lead = 0;
	BOOL has_lead = FALSE;
	WCHAR last = 0;
	for (;;) {
		INT c = low ? 0 : held_at < held_count ? held[held_at++] : next_byte(reader);
		if (c < 0)
			break;
		WCHAR wide;
		if (low) {
			wide = low;
			low = 0;
		} else if (code_page == CP_UTF8 && !unicode) {
			if (c >= 0xc0 && c < 0xf8) {
				sequence_left = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
				sequence = c & (0x3f >> sequence_left);
				continue;
			}
			if (c >= 0x80 && sequence_left) {
				sequence = sequence << 6 | (c & 0x3f);
				if (--sequence_left)
					continue;
				c = 0x110000;
			}
			if (c == 0x110000 && sequence >= 0x10000) {
				wide = (WCHAR)(0xd800 + ((sequence - 0x10000) >> 10));
				low = (WCHAR)(0xdc00 + ((sequence - 0x10000) & 0x3ff));
			} else {
				wide = c == 0x110000 ? (WCHAR)sequence : (WCHAR)c;
			}
		} else if (unicode) {
			if (pending < 0) {
				pending = c;
				continue;
			}
			wide = (WCHAR)(pending | c << 8);
			pending = -1;
		} else if (code_page == CP_UTF8 || code_page != 1252) {
			char bytes[4];
			if (has_lead) {
				bytes[0] = lead;
				bytes[1] = (char)c;
				has_lead = FALSE;
				MultiByteToWideChar(code_page, 0, bytes, 2, &wide, 1);
			} else if (c >= 0x80 && IsDBCSLeadByteEx(code_page, (BYTE)c)) {
				lead = (BYTE)c;
				has_lead = TRUE;
				continue;
			} else {
				bytes[0] = (char)c;
				MultiByteToWideChar(code_page, 0, bytes, 1, &wide, 1);
			}
		} else {
			char byte = (char)c;
			MultiByteToWideChar(code_page, 0, &byte, 1, &wide, 1);
		}
		// Every kind of line ending makes one mark.
		if (wide == RE_LF) {
			if (last != RE_CR)
				emit_para(reader);
			last = wide;
			continue;
		}
		if (wide == RE_CR)
			emit_para(reader);
		else
			emit_wide(reader, wide);
		last = wide;
	}
	(void)state;
	flush_text(reader);
}

LRESULT re_stream_in(re_editor *editor, WPARAM flags, EDITSTREAM *stream)
{
	if (!stream || !stream->pfnCallback)
		return 0;
	re_doc *doc = &editor->doc;
	stream->dwError = 0;
	DWORD format = (DWORD)(flags & (SF_TEXT | SF_RTF));
	if (format == 0)
		return 0;
	rtf_reader *reader = re_calloc(sizeof(*reader));
	if (!reader)
		return 0;
	reader->stream = stream;
	reader->editor = editor;
	reader->capacity = 16;
	reader->states = re_alloc(reader->capacity * sizeof(rtf_state));
	if (!reader->states) {
		re_free(reader);
		return 0;
	}
	reader->default_code_page = 1252;
	reader->font_being_read = -1;
	rtf_state *state = &reader->states[0];
	memset(state, 0, sizeof(*state));
	state->code_page = 1252;
	state->unicode_skip = 1;
	BOOL selection = (flags & SFF_SELECTION) != 0;
	INT start = 0, end = 0;
	if (selection) {
		start = editor->sel_start < editor->sel_end ? editor->sel_start : editor->sel_end;
		end = editor->sel_start < editor->sel_end ? editor->sel_end : editor->sel_start;
		if (end > re_text_length(editor))
			end = re_text_length(editor);
	}
	CHARFORMAT2W look;
	if (selection) {
		DWORD differing;
		re_get_selection_format(editor, &look, &differing);
		look.dwMask = CFM_ALL2;
		state->para = doc->paras[re_doc_para_at(doc, start)].format;
	} else {
		look = editor->default_format;
		re_default_para_format(&state->para);
	}
	state->format = look;
	re_doc saved_doc;
	BOOL replaced_doc = FALSE;
	memset(&saved_doc, 0, sizeof(saved_doc));
	INT first_byte = next_byte(reader);
	BOOL looks_rtf = FALSE;
	if (first_byte >= 0) {
		put_back(reader);
		// Peek at the magic; RTF that does not start with it falls back to text, or is refused when text was not asked for.
		looks_rtf = reader->have - reader->at >= 5 && !memcmp(reader->buffer + reader->at, "{\\rtf", 5);
		if (!looks_rtf && reader->have - reader->at >= 5 && !memcmp(reader->buffer + reader->at, "{\\urtf", 6))
			looks_rtf = TRUE;
	}
	if ((format & SF_RTF) && !looks_rtf && !(format & SF_TEXT)) {
		BOOL any_data = first_byte >= 0;
		if (any_data)
			stream->dwError = (DWORD)-16;
		// The old document is gone even so.
		if (!selection) {
			re_ole_text_reset(editor);
			re_doc_clear(doc);
			re_doc_init(doc, &look);
			editor->sel_start = editor->sel_end = 0;
		}
		re_free(reader->states);
		re_free(reader);
		return 0;
	}
	if (!selection) {
		// A fresh document: the old one is gone without an undo.
		re_undo_clear(&editor->undo);
		re_undo_clear(&editor->redo);
		saved_doc = *doc;
		memset(doc, 0, sizeof(*doc));
		re_doc_init(doc, &look);
		replaced_doc = TRUE;
		editor->sel_start = editor->sel_end = 0;
		editor->scroll_x = editor->scroll_y = 0;
		reader->position = 0;
	} else {
		re_undo_begin_step(editor);
		if (start != end) {
			CHARFORMAT2W kept;
			re_doc_char_format(doc, start, &kept);
			re_replace(editor, start, end - start, NULL, 0, &kept, TRUE);
		}
		reader->position = start;
		re_undo_format(editor);
	}
	reader->limit_left = editor->limit - re_text_length(editor);
	if (reader->limit_left < 0)
		reader->limit_left = 0;
	if (looks_rtf)
		parse_rtf(reader);
	else
		read_plain(reader, flags);
	if (replaced_doc)
		re_doc_clear(&saved_doc);
	if (looks_rtf && !selection) {
		// The settings still open at the end belong to the last paragraph, the one that has no \par of its own.
		rtf_state *last = &reader->states[0];
		// The last \\par of a whole document is the final mark itself, and it brings its settings along.
		if (doc->length >= 2 && doc->text[doc->length - 2] == RE_CR) {
			CHARFORMAT2W kept;
			PARAFORMAT2 settings = doc->paras[doc->para_count - 2].format;
			re_doc_char_format(doc, doc->length - 2, &kept);
			re_doc_replace(doc, doc->length - 2, 1, NULL, 0, &kept);
			doc->paras[doc->para_count - 1].format = settings;
			kept.dwMask = CFM_ALL2;
			re_doc_set_format(doc, doc->length - 1, doc->length, &kept);
			reader->position--;
		} else {
			doc->paras[doc->para_count - 1].format = last->para;
			last->format.dwMask = CFM_ALL2;
			re_doc_set_format(doc, doc->length - 1, doc->length, &last->format);
		}
	}
	// A control of one line keeps its first paragraph.
	if (!(editor->style & ES_MULTILINE) && !editor->host) {
		for (INT i = 0; i < doc->length - 1; i++)
			if (doc->text[i] == RE_CR) {
				CHARFORMAT2W kept;
				re_doc_char_format(doc, i, &kept);
				reader->position = i;
				re_doc_replace(doc, i, doc->length - 1 - i, NULL, 0, &kept);
				break;
			}
	}
	INT inserted = reader->position - start;
	if (selection) {
		editor->sel_start = editor->sel_end = start + inserted;
	} else {
		editor->sel_start = editor->sel_end = 0;
	}
	editor->has_insert_format = FALSE;
	LRESULT result = looks_rtf ? inserted : reader->total;
	re_free(reader->states);
	re_free(reader->fonts);
	re_free(reader->colors);
	re_free(reader->text);
	re_free(reader);
	return result;
}

// ---- writing ----

typedef struct
{
	EDITSTREAM *stream;
	char buffer[4096];
	INT used;
	BOOL failed;
	LONG total;
	const WCHAR **faces;
	const CHARFORMAT2W **font_formats;
	INT face_count;
	COLORREF *colors;
	INT color_count;
} rtf_writer;

static void write_flush(rtf_writer *writer)
{
	INT at = 0;
	while (at < writer->used && !writer->failed) {
		LONG wrote = 0;
		DWORD error = writer->stream->pfnCallback(writer->stream->dwCookie, (BYTE *)writer->buffer + at, writer->used - at, &wrote);
		if (error) {
			writer->stream->dwError = error;
			writer->failed = TRUE;
			break;
		}
		// A callback that took less than it was given ends the stream.
		if (wrote < writer->used - at) {
			writer->failed = TRUE;
			break;
		}
		at += wrote;
		writer->total += wrote;
	}
	writer->used = 0;
}

static void write_bytes(rtf_writer *writer, const char *data, INT length)
{
	for (INT i = 0; i < length; i++) {
		if (writer->used == (INT)sizeof(writer->buffer))
			write_flush(writer);
		writer->buffer[writer->used++] = data[i];
	}
}

static void write_string(rtf_writer *writer, const char *text)
{
	write_bytes(writer, text, lstrlenA(text));
}

static void write_number(rtf_writer *writer, const char *word, INT value)
{
	char text[48];
	wsprintfA(text, "%s%d", word, value);
	write_string(writer, text);
}

static INT face_index(rtf_writer *writer, const CHARFORMAT2W *format)
{
	for (INT i = 0; i < writer->face_count; i++) {
		if (!lstrcmpW(writer->font_formats[i]->szFaceName, format->szFaceName) && writer->font_formats[i]->bCharSet == format->bCharSet)
			return i;
	}
	return 0;
}

static INT color_index(rtf_writer *writer, COLORREF color)
{
	for (INT i = 0; i < writer->color_count; i++) {
		if (writer->colors[i] == color)
			return i + 1;
	}
	return 0;
}

static void collect_tables(rtf_writer *writer, const re_doc *doc, INT start, INT end)
{
	writer->font_formats = re_alloc(sizeof(*writer->font_formats) * (doc->run_count + 1));
	writer->colors = re_alloc(sizeof(COLORREF) * (doc->run_count * 2 + 1));
	if (!writer->font_formats || !writer->colors)
		return;
	for (INT i = re_doc_run_at(doc, start); i < doc->run_count && doc->runs[i].start <= end; i++) {
		const CHARFORMAT2W *format = &doc->runs[i].format;
		BOOL found = FALSE;
		for (INT f = 0; f < writer->face_count && !found; f++)
			found = !lstrcmpW(writer->font_formats[f]->szFaceName, format->szFaceName) && writer->font_formats[f]->bCharSet == format->bCharSet;
		if (!found)
			writer->font_formats[writer->face_count++] = format;
		if (!(format->dwEffects & CFE_AUTOCOLOR) && !color_index(writer, format->crTextColor))
			writer->colors[writer->color_count++] = format->crTextColor;
		if (!(format->dwEffects & CFE_AUTOBACKCOLOR) && !color_index(writer, format->crBackColor))
			writer->colors[writer->color_count++] = format->crBackColor;
	}
}

static void write_header(rtf_writer *writer)
{
	write_string(writer, "{\\rtf1\\ansi");
	write_number(writer, "\\ansicpg", (INT)GetACP());
	write_string(writer, "\\deff0\\deflang1033{\\fonttbl");
	for (INT i = 0; i < writer->face_count; i++) {
		const CHARFORMAT2W *format = writer->font_formats[i];
		write_number(writer, "{\\f", i);
		BYTE family = format->bPitchAndFamily & 0xf0;
		write_string(writer, family == FF_ROMAN ? "\\froman" : family == FF_SWISS ? "\\fswiss" : family == FF_MODERN ? "\\fmodern"
			: family == FF_SCRIPT ? "\\fscript" : family == FF_DECORATIVE ? "\\fdecor" : "\\fnil");
		if (format->bCharSet != DEFAULT_CHARSET)
			write_number(writer, "\\fcharset", format->bCharSet);
		write_string(writer, " ");
		char face[LF_FACESIZE * 2];
		INT made = WideCharToMultiByte(CP_ACP, 0, format->szFaceName, -1, face, sizeof(face), NULL, NULL);
		if (made > 0)
			write_bytes(writer, face, made - 1);
		write_string(writer, ";}");
	}
	write_string(writer, "}\r\n");
	if (writer->color_count) {
		write_string(writer, "{\\colortbl ;");
		for (INT i = 0; i < writer->color_count; i++) {
			write_number(writer, "\\red", GetRValue(writer->colors[i]));
			write_number(writer, "\\green", GetGValue(writer->colors[i]));
			write_number(writer, "\\blue", GetBValue(writer->colors[i]));
			write_string(writer, ";");
		}
		write_string(writer, "}\r\n");
	}
}

static void write_para_settings(rtf_writer *writer, const PARAFORMAT2 *para)
{
	write_string(writer, "\\pard");
	if (para->wAlignment == PFA_RIGHT)
		write_string(writer, "\\qr");
	else if (para->wAlignment == PFA_CENTER)
		write_string(writer, "\\qc");
	else if (para->wAlignment == PFA_JUSTIFY)
		write_string(writer, "\\qj");
	if (para->dxStartIndent + para->dxOffset)
		write_number(writer, "\\li", para->dxStartIndent + para->dxOffset);
	if (para->dxRightIndent)
		write_number(writer, "\\ri", para->dxRightIndent);
	if (para->dxOffset)
		write_number(writer, "\\fi", -para->dxOffset);
	if (para->dySpaceBefore)
		write_number(writer, "\\sb", para->dySpaceBefore);
	if (para->dySpaceAfter)
		write_number(writer, "\\sa", para->dySpaceAfter);
	for (INT i = 0; i < para->cTabCount && i < MAX_TAB_STOPS; i++)
		write_number(writer, "\\tx", para->rgxTabs[i]);
}

static void write_char_settings(rtf_writer *writer, const CHARFORMAT2W *format, const CHARFORMAT2W *before)
{
	struct { DWORD effect; const char *on, *off; } effects[] = {
		{CFE_BOLD, "\\b", "\\b0"}, {CFE_ITALIC, "\\i", "\\i0"}, {CFE_UNDERLINE, "\\ul", "\\ulnone"}, {CFE_STRIKEOUT, "\\strike", "\\strike0"},
		{CFE_PROTECTED, "\\protect", "\\protect0"}, {CFE_HIDDEN, "\\v", "\\v0"},
	};
	for (size_t i = 0; i < ARRAYSIZE(effects); i++) {
		BOOL on = (format->dwEffects & effects[i].effect) != 0;
		BOOL was = before && (before->dwEffects & effects[i].effect);
		if (on != was)
			write_string(writer, on ? effects[i].on : effects[i].off);
	}
	DWORD script = format->dwEffects & (CFE_SUBSCRIPT | CFE_SUPERSCRIPT);
	DWORD script_before = before ? before->dwEffects & (CFE_SUBSCRIPT | CFE_SUPERSCRIPT) : 0;
	if (script != script_before)
		write_string(writer, script & CFE_SUBSCRIPT ? "\\sub" : script & CFE_SUPERSCRIPT ? "\\super" : "\\nosupersub");
	if (!before || format->yOffset != before->yOffset) {
		if (format->yOffset > 0)
			write_number(writer, "\\up", format->yOffset / 10);
		else if (format->yOffset < 0)
			write_number(writer, "\\dn", -format->yOffset / 10);
	}
	INT color = format->dwEffects & CFE_AUTOCOLOR ? 0 : color_index(writer, format->crTextColor);
	INT color_before = !before || before->dwEffects & CFE_AUTOCOLOR ? 0 : color_index(writer, before->crTextColor);
	if (color != color_before)
		write_number(writer, "\\cf", color);
	INT back = format->dwEffects & CFE_AUTOBACKCOLOR ? 0 : color_index(writer, format->crBackColor);
	INT back_before = !before || before->dwEffects & CFE_AUTOBACKCOLOR ? 0 : color_index(writer, before->crBackColor);
	if (back != back_before)
		write_number(writer, "\\highlight", back);
	if (!before || face_index(writer, format) != face_index(writer, before))
		write_number(writer, "\\f", face_index(writer, format));
	if (!before || format->yHeight != before->yHeight)
		write_number(writer, "\\fs", format->yHeight / 10);
}

static void write_wide(rtf_writer *writer, WCHAR c)
{
	char byte[8];
	if (c == '\\' || c == '{' || c == '}') {
		byte[0] = '\\';
		byte[1] = (char)c;
		write_bytes(writer, byte, 2);
	} else if (c == RE_TAB) {
		write_string(writer, "\\tab ");
	} else if (c == RE_VT) {
		write_string(writer, "\\line ");
	} else if (c >= 0x20 && c < 0x80) {
		byte[0] = (char)c;
		write_bytes(writer, byte, 1);
	} else {
		char narrow[4];
		BOOL lost = FALSE;
		INT made = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, &c, 1, narrow, sizeof(narrow), NULL, &lost);
		if (made > 0 && !lost) {
			for (INT i = 0; i < made; i++) {
				wsprintfA(byte, "\\'%02x", (BYTE)narrow[i]);
				write_string(writer, byte);
			}
		} else {
			write_number(writer, "\\u", (SHORT)c);
			write_string(writer, "?");
		}
	}
}

LRESULT re_stream_out(re_editor *editor, WPARAM flags, EDITSTREAM *stream)
{
	if (!stream || !stream->pfnCallback)
		return 0;
	stream->dwError = 0;
	const re_doc *doc = &editor->doc;
	INT start = 0, end = flags & (SF_RTF | SF_RTFNOOBJS) ? doc->length : re_text_length(editor);
	if (flags & SFF_SELECTION) {
		start = editor->sel_start < editor->sel_end ? editor->sel_start : editor->sel_end;
		end = editor->sel_start < editor->sel_end ? editor->sel_end : editor->sel_start;
		if (end > doc->length)
			end = doc->length;
	}
	rtf_writer *writer = re_calloc(sizeof(*writer));
	if (!writer)
		return 0;
	writer->stream = stream;
	LRESULT result = 0;
	DWORD format = (DWORD)(flags & (SF_TEXT | SF_RTF | SF_RTFNOOBJS | SF_TEXTIZED));
	if (format == SF_TEXT || !(format & (SF_RTF | SF_RTFNOOBJS))) {
		UINT code_page = (flags & SF_USECODEPAGE) ? HIWORD(flags) : CP_ACP;
		for (INT i = start; i < end && i < re_text_length(editor); i++) {
			WCHAR c = doc->text[i];
			WCHAR pair[2] = {RE_CR, RE_LF};
			INT count = c == RE_CR ? 2 : 1;
			const WCHAR *send = c == RE_CR ? pair : &c;
			if (flags & SF_UNICODE) {
				write_bytes(writer, (const char *)send, count * sizeof(WCHAR));
			} else {
				char narrow[16];
				INT made = WideCharToMultiByte(code_page, 0, send, count, narrow, sizeof(narrow), NULL, NULL);
				write_bytes(writer, narrow, made);
			}
		}
		write_flush(writer);
		result = writer->failed ? 0 : writer->total;
	} else {
		collect_tables(writer, doc, start, end);
		write_header(writer);
		const CHARFORMAT2W *before = NULL;
		INT para = re_doc_para_at(doc, start);
		write_para_settings(writer, &doc->paras[para].format);
		write_string(writer, "\\plain");
		INT at = start;
		BOOL spaced = FALSE;
		while (at < end) {
			INT run = re_doc_run_at(doc, at);
			const CHARFORMAT2W *look = &doc->runs[run].format;
			if (look != before) {
				write_char_settings(writer, look, before);
				before = look;
				spaced = FALSE;
			}
			INT stop = run + 1 < doc->run_count ? doc->runs[run + 1].start : doc->length;
			if (stop > end)
				stop = end;
			for (INT i = at; i < stop; i++) {
				WCHAR c = doc->text[i];
				if (c == RE_CR) {
					write_string(writer, "\\par\r\n");
					spaced = TRUE;
					if (i + 1 < end) {
						INT next = re_doc_para_at(doc, i + 1);
						write_para_settings(writer, &doc->paras[next].format);
						spaced = FALSE;
					}
					continue;
				}
				if (!spaced) {
					write_string(writer, " ");
					spaced = TRUE;
				}
				write_wide(writer, c);
			}
			at = stop;
		}
		write_string(writer, "}\r\n");
		write_flush(writer);
		result = writer->failed ? 0 : writer->total;
	}
	re_free(writer->font_formats);
	re_free(writer->colors);
	re_free(writer);
	return result;
}
