/* SPDX-License-Identifier: MIT */
#include "js.h"
#include "../raven-vbscript/regex.h"

/* The lexer and the parser of ECMAScript 3 as JScript reads it. The lexer works one token ahead and decides between a division and
 * a regular expression literal by the token before it. Semicolons the grammar wants may be left out before a line break, a closing
 * brace or the end. The parser is a recursive descent over the grammar; it collects the var names and function declarations of each
 * function as it goes, so the interpreter can make them before the function runs. */

typedef enum { T_EOF, T_NAME, T_NUMBER, T_STRING, T_REGEXP, T_PUNCT } token_kind;

typedef struct {
	token_kind kind;
	const WCHAR *start;
	UINT length;
	UINT line, column;
	BOOL newline_before;
	double number;
	js_string *text;
	DWORD flags;
	WCHAR punct[5];
} token;

/* All a parse allocates, freed with the code. */
typedef struct arena {
	void **blocks;
	UINT count, room;
	js_string **strings;
	UINT string_count, string_room;
} arena;

typedef struct {
	const WCHAR *source, *at;
	UINT line;
	const WCHAR *line_start;
	token token;
	BOOL failed;
	parse_error *error;
	arena *arena;
	function_info *function;
	/* What break and continue may name here. */
	UINT loop_depth, switch_depth;
	js_string **labels;
	BOOL *label_loops;
	UINT label_count;
	BOOL in_function;
} parser;

static void *keep(parser *p, void *block)
{
	arena *a = p->arena;
	if (a->count == a->room) {
		a->room = a->room ? a->room * 2 : 64;
		a->blocks = js_grow(a->blocks, a->room * sizeof(void *));
	}
	a->blocks[a->count++] = block;
	return block;
}

static js_string *keep_string(parser *p, js_string *s)
{
	arena *a = p->arena;
	if (a->string_count == a->string_room) {
		a->string_room = a->string_room ? a->string_room * 2 : 64;
		a->strings = js_grow(a->strings, a->string_room * sizeof(js_string *));
	}
	a->strings[a->string_count++] = s;
	return s;
}

static node *new_node(parser *p, node_kind kind)
{
	node *n = keep(p, js_alloc(sizeof(node)));
	n->kind = kind;
	n->line = p->token.line;
	n->column = p->token.column;
	return n;
}

/* Lists grow while they are parsed and are kept once complete. */
typedef struct {
	void **items;
	UINT count, room;
} vec;

static void vec_add(vec *v, void *item)
{
	if (v->count == v->room) {
		v->room = v->room ? v->room * 2 : 8;
		v->items = js_grow(v->items, v->room * sizeof(void *));
	}
	v->items[v->count++] = item;
}

static void *vec_keep(parser *p, vec *v)
{
	if (!v->items)
		return NULL;
	return keep(p, v->items);
}

static BOOL fail(parser *p, LONG number)
{
	if (!p->failed) {
		p->failed = TRUE;
		p->error->number = number;
		p->error->line = p->token.line;
		p->error->column = p->token.column;
	}
	return FALSE;
}

static BOOL fail_at(parser *p, LONG number, UINT line, UINT column)
{
	if (!p->failed) {
		p->failed = TRUE;
		p->error->number = number;
		p->error->line = line;
		p->error->column = column;
	}
	return FALSE;
}

/* The lexer. */

static BOOL is_line_break(WCHAR c) { return c == L'\n' || c == L'\r' || c == 0x2028 || c == 0x2029; }

static BOOL is_blank(WCHAR c)
{
	return c == L' ' || c == L'\t' || c == L'\v' || c == L'\f' || c == 0xa0 || c == 0xfeff || (c >= 0x2000 && c <= 0x200a) ||
		c == 0x3000 || c == 0x1680 || c == 0x202f || c == 0x205f;
}

static BOOL is_name_start(WCHAR c)
{
	return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || c == L'$' || (c >= 0x80 && IsCharAlphaW(c));
}

static BOOL is_name_part(WCHAR c) { return is_name_start(c) || (c >= L'0' && c <= L'9') || (c >= 0x80 && IsCharAlphaNumericW(c)); }

static int hex_value(WCHAR c)
{
	if (c >= L'0' && c <= L'9')
		return c - L'0';
	if (c >= L'a' && c <= L'f')
		return c - L'a' + 10;
	if (c >= L'A' && c <= L'F')
		return c - L'A' + 10;
	return -1;
}

static void new_line(parser *p, const WCHAR *after)
{
	p->line++;
	p->line_start = after;
}

/* Blanks, line breaks and comments before a token; FALSE for a comment that does not end. */
static BOOL skip_space(parser *p, BOOL *newline)
{
	for (;;) {
		WCHAR c = *p->at;
		if (is_blank(c))
			p->at++;
		else if (is_line_break(c)) {
			*newline = TRUE;
			if (c == L'\r' && p->at[1] == L'\n')
				p->at++;
			p->at++;
			new_line(p, p->at);
		} else if (c == L'/' && p->at[1] == L'/') {
			while (*p->at && !is_line_break(*p->at))
				p->at++;
		} else if (c == L'/' && p->at[1] == L'*') {
			UINT line = p->line, column = (UINT)(p->at - p->line_start) + 1;
			p->at += 2;
			while (*p->at && !(p->at[0] == L'*' && p->at[1] == L'/')) {
				if (is_line_break(*p->at)) {
					*newline = TRUE;
					if (*p->at == L'\r' && p->at[1] == L'\n')
						p->at++;
					new_line(p, p->at + 1);
				}
				p->at++;
			}
			if (!*p->at)
				return fail_at(p, JS_E_UNTERMINATED_COMMENT, line, column);
			p->at += 2;
		} else
			return TRUE;
	}
}

static BOOL read_number(parser *p, token *t)
{
	const WCHAR *c = p->at;
	double value = 0;
	if (c[0] == L'0' && (c[1] == L'x' || c[1] == L'X')) {
		c += 2;
		if (hex_value(*c) < 0)
			return fail(p, JS_E_SYNTAX);
		while (hex_value(*c) >= 0)
			value = value * 16 + hex_value(*c++);
	} else if (c[0] == L'0' && c[1] >= L'0' && c[1] <= L'9') {
		/* A leading zero writes an octal number, unless a digit is not one. */
		const WCHAR *d = c + 1;
		BOOL octal = TRUE;
		while (*d >= L'0' && *d <= L'9') {
			if (*d >= L'8')
				octal = FALSE;
			d++;
		}
		if (octal) {
			for (c++; c < d; c++)
				value = value * 8 + (*c - L'0');
		} else {
			js_string *digits = js_string_new(c, (UINT)(d - c));
			value = js_string_number(digits);
			js_string_release(digits);
			c = d;
		}
	} else {
		const WCHAR *start = c;
		while (*c >= L'0' && *c <= L'9')
			c++;
		if (*c == L'.') {
			c++;
			while (*c >= L'0' && *c <= L'9')
				c++;
		}
		if ((*c == L'e' || *c == L'E') &&
			((c[1] >= L'0' && c[1] <= L'9') || ((c[1] == L'+' || c[1] == L'-') && c[2] >= L'0' && c[2] <= L'9'))) {
			c += 2;
			while (*c >= L'0' && *c <= L'9')
				c++;
		}
		js_string *digits = js_string_new(start, (UINT)(c - start));
		value = js_string_number(digits);
		js_string_release(digits);
	}
	if (is_name_start(*c))
		return fail(p, JS_E_SYNTAX);
	t->kind = T_NUMBER;
	t->number = value;
	p->at = c;
	return TRUE;
}

static BOOL read_string(parser *p, token *t)
{
	WCHAR quote = *p->at++;
	js_buffer b = { 0 };
	for (;;) {
		WCHAR c = *p->at;
		if (!c || is_line_break(c)) {
			js_free(b.text);
			return fail(p, JS_E_UNTERMINATED_STRING);
		}
		p->at++;
		if (c == quote)
			break;
		if (c != L'\\') {
			js_buffer_char(&b, c);
			continue;
		}
		c = *p->at++;
		int h1, h2, h3, h4;
		switch (c) {
		case L'n': js_buffer_char(&b, L'\n'); break;
		case L't': js_buffer_char(&b, L'\t'); break;
		case L'r': js_buffer_char(&b, L'\r'); break;
		case L'b': js_buffer_char(&b, L'\b'); break;
		case L'f': js_buffer_char(&b, L'\f'); break;
		case L'v': js_buffer_char(&b, L'\v'); break;
		case L'x':
			if ((h1 = hex_value(p->at[0])) < 0 || (h2 = hex_value(p->at[1])) < 0) {
				js_buffer_char(&b, L'x');
				break;
			}
			js_buffer_char(&b, (WCHAR)(h1 * 16 + h2));
			p->at += 2;
			break;
		case L'u':
			if ((h1 = hex_value(p->at[0])) < 0 || (h2 = hex_value(p->at[1])) < 0 || (h3 = hex_value(p->at[2])) < 0 ||
				(h4 = hex_value(p->at[3])) < 0) {
				js_buffer_char(&b, L'u');
				break;
			}
			js_buffer_char(&b, (WCHAR)(((h1 * 16 + h2) * 16 + h3) * 16 + h4));
			p->at += 4;
			break;
		case L'\r':
			if (*p->at == L'\n')
				p->at++;
			new_line(p, p->at);
			break;
		case L'\n':
		case 0x2028:
		case 0x2029:
			new_line(p, p->at);
			break;
		case 0:
			js_free(b.text);
			p->at--;
			return fail(p, JS_E_UNTERMINATED_STRING);
		default:
			if (c >= L'0' && c <= L'7') {
				/* Octal escapes: up to three digits, at most \377. */
				int value = c - L'0';
				if (*p->at >= L'0' && *p->at <= L'7') {
					value = value * 8 + (*p->at++ - L'0');
					if (c <= L'3' && *p->at >= L'0' && *p->at <= L'7')
						value = value * 8 + (*p->at++ - L'0');
				}
				js_buffer_char(&b, (WCHAR)value);
			} else
				js_buffer_char(&b, c);
		}
	}
	t->kind = T_STRING;
	t->text = keep_string(p, b.text ? js_buffer_string(&b) : js_string_new(L"", 0));
	return TRUE;
}

static BOOL read_regexp(parser *p, token *t)
{
	const WCHAR *start = ++p->at;
	BOOL in_class = FALSE;
	for (;;) {
		WCHAR c = *p->at;
		if (!c || is_line_break(c))
			return fail(p, JS_E_UNTERMINATED_REGEXP);
		if (c == L'\\') {
			p->at++;
			if (!*p->at || is_line_break(*p->at))
				return fail(p, JS_E_UNTERMINATED_REGEXP);
		} else if (c == L'[')
			in_class = TRUE;
		else if (c == L']')
			in_class = FALSE;
		else if (c == L'/' && !in_class)
			break;
		p->at++;
	}
	t->text = keep_string(p, js_string_new(start, (UINT)(p->at - start)));
	p->at++;
	t->flags = 0;
	while (is_name_part(*p->at)) {
		WCHAR f = *p->at++;
		if (f == L'g')
			t->flags |= RXF_GLOBAL;
		else if (f == L'i')
			t->flags |= RX_IGNORE_CASE;
		else if (f == L'm')
			t->flags |= RX_MULTILINE;
		else
			return fail(p, JS_E_SYNTAX);
	}
	t->kind = T_REGEXP;
	return TRUE;
}

/* After these a slash divides; elsewhere it starts a regular expression. */
static BOOL slash_divides(const token *previous)
{
	if (previous->kind == T_NUMBER || previous->kind == T_STRING || previous->kind == T_REGEXP)
		return TRUE;
	if (previous->kind == T_NAME) {
		static const WCHAR *const operators[] = { L"return", L"typeof", L"instanceof", L"in", L"of", L"new", L"delete", L"void",
			L"throw", L"case", L"do", L"else" };
		for (UINT i = 0; i < ARRAYSIZE(operators); i++)
			if ((UINT)lstrlenW(operators[i]) == previous->length && !memcmp(previous->start, operators[i], previous->length * sizeof(WCHAR)))
				return FALSE;
		return TRUE;
	}
	if (previous->kind == T_PUNCT)
		return !lstrcmpW(previous->punct, L")") || !lstrcmpW(previous->punct, L"]") || !lstrcmpW(previous->punct, L"}");
	return FALSE;
}

static BOOL next(parser *p)
{
	token previous = p->token;
	token *t = &p->token;
	BOOL newline = FALSE;
	if (p->failed)
		return FALSE;
	if (!skip_space(p, &newline))
		return FALSE;
	ZeroMemory(t, sizeof(*t));
	t->newline_before = newline;
	t->start = p->at;
	t->line = p->line;
	t->column = (UINT)(p->at - p->line_start) + 1;
	WCHAR c = *p->at;
	if (!c) {
		t->kind = T_EOF;
		return TRUE;
	}
	if (is_name_start(c)) {
		while (is_name_part(*p->at))
			p->at++;
		t->kind = T_NAME;
		t->length = (UINT)(p->at - t->start);
		return TRUE;
	}
	if ((c >= L'0' && c <= L'9') || (c == L'.' && p->at[1] >= L'0' && p->at[1] <= L'9')) {
		if (!read_number(p, t))
			return FALSE;
		t->length = (UINT)(p->at - t->start);
		return TRUE;
	}
	if (c == L'"' || c == L'\'') {
		if (!read_string(p, t))
			return FALSE;
		t->length = (UINT)(p->at - t->start);
		return TRUE;
	}
	if (c == L'/' && !slash_divides(&previous)) {
		if (!read_regexp(p, t))
			return FALSE;
		t->length = (UINT)(p->at - t->start);
		return TRUE;
	}
	static const WCHAR *const puncts[] = {
		L">>>=", L"===", L"!==", L">>>", L"<<=", L">>=", L"==", L"!=", L"<=", L">=", L"&&", L"||", L"++", L"--", L"+=", L"-=", L"*=",
		L"/=", L"%=", L"&=", L"|=", L"^=", L"<<", L">>", L"{", L"}", L"(", L")", L"[", L"]", L".", L";", L",", L"<", L">", L"+", L"-",
		L"*", L"/", L"%", L"&", L"|", L"^", L"!", L"~", L"?", L":", L"=",
	};
	for (UINT i = 0; i < ARRAYSIZE(puncts); i++) {
		UINT length = lstrlenW(puncts[i]);
		if (!memcmp(p->at, puncts[i], length * sizeof(WCHAR))) {
			t->kind = T_PUNCT;
			lstrcpyW(t->punct, puncts[i]);
			p->at += length;
			t->length = length;
			return TRUE;
		}
	}
	return fail(p, JS_E_INVALID_CHAR);
}

static BOOL is(parser *p, const WCHAR *punct) { return p->token.kind == T_PUNCT && !lstrcmpW(p->token.punct, punct); }

static BOOL is_word(parser *p, const WCHAR *word)
{
	UINT length = lstrlenW(word);
	return p->token.kind == T_NAME && p->token.length == length && !memcmp(p->token.start, word, length * sizeof(WCHAR));
}

static BOOL is_keyword_token(const token *t)
{
	static const WCHAR *const keywords[] = {
		L"break", L"case", L"catch", L"continue", L"default", L"delete", L"do", L"else", L"finally", L"for", L"function", L"if", L"in",
		L"instanceof", L"new", L"return", L"switch", L"this", L"throw", L"try", L"typeof", L"var", L"void", L"while", L"with", L"null",
		L"true", L"false",
	};
	if (t->kind != T_NAME)
		return FALSE;
	for (UINT i = 0; i < ARRAYSIZE(keywords); i++)
		if ((UINT)lstrlenW(keywords[i]) == t->length && !memcmp(t->start, keywords[i], t->length * sizeof(WCHAR)))
			return TRUE;
	return FALSE;
}

static BOOL expect(parser *p, const WCHAR *punct, LONG number)
{
	if (!is(p, punct))
		return fail(p, number);
	return next(p);
}

static js_string *token_name(parser *p) { return keep_string(p, js_string_new(p->token.start, p->token.length)); }

/* A name that is not a keyword. */
static js_string *identifier(parser *p)
{
	if (p->token.kind != T_NAME || is_keyword_token(&p->token)) {
		fail(p, JS_E_EXPECTED_IDENTIFIER);
		return NULL;
	}
	js_string *name = token_name(p);
	return next(p) ? name : NULL;
}

/* The semicolon a statement ends with, or the place one may be left out. */
static BOOL semicolon(parser *p)
{
	if (is(p, L";"))
		return next(p);
	if (is(p, L"}") || p->token.kind == T_EOF || p->token.newline_before)
		return TRUE;
	return fail(p, JS_E_EXPECTED_SEMICOLON);
}

/* Expressions. */

static node *assignment(parser *p, BOOL no_in);
static node *expression(parser *p, BOOL no_in);
static node *statement(parser *p);
static function_info *function_rest(parser *p, js_string *name, const WCHAR *start, BOOL expression);

static void note_name(parser *p, const js_string *name)
{
	if (js_string_is(name, L"arguments"))
		p->function->uses_arguments = TRUE;
}

static node *arguments(parser *p, node *n)
{
	vec args = { 0 };
	if (!next(p))
		return NULL;
	if (!is(p, L")"))
		for (;;) {
			node *arg = assignment(p, FALSE);
			if (!arg) {
				js_free(args.items);
				return NULL;
			}
			vec_add(&args, arg);
			if (!is(p, L","))
				break;
			if (!next(p)) {
				js_free(args.items);
				return NULL;
			}
		}
	if (!is(p, L")")) {
		js_free(args.items);
		fail(p, JS_E_EXPECTED_RPAREN);
		return NULL;
	}
	n->count = args.count;
	n->list = vec_keep(p, &args);
	return next(p) ? n : NULL;
}

static node *primary(parser *p)
{
	node *n;
	token *t = &p->token;
	if (t->kind == T_NUMBER) {
		n = new_node(p, N_NUMBER);
		n->number = t->number;
		return next(p) ? n : NULL;
	}
	if (t->kind == T_STRING) {
		n = new_node(p, N_STRING);
		n->name = t->text;
		return next(p) ? n : NULL;
	}
	if (t->kind == T_REGEXP) {
		n = new_node(p, N_REGEXP);
		n->name = t->text;
		n->flags = t->flags;
		return next(p) ? n : NULL;
	}
	if (is(p, L"(")) {
		if (!next(p) || !(n = expression(p, FALSE)))
			return NULL;
		if (!expect(p, L")", JS_E_EXPECTED_RPAREN))
			return NULL;
		return n;
	}
	if (is(p, L"[")) {
		vec elements = { 0 };
		n = new_node(p, N_ARRAY);
		if (!next(p))
			return NULL;
		/* Elisions leave holes; a comma before the closing bracket ends the list. */
		while (!is(p, L"]")) {
			if (is(p, L",")) {
				vec_add(&elements, NULL);
				if (!next(p))
					goto array_failed;
				continue;
			}
			node *element = assignment(p, FALSE);
			if (!element)
				goto array_failed;
			vec_add(&elements, element);
			if (is(p, L",")) {
				if (!next(p))
					goto array_failed;
			} else if (!is(p, L"]")) {
				fail(p, JS_E_EXPECTED_RBRACKET);
				goto array_failed;
			}
		}
		n->count = elements.count;
		n->list = vec_keep(p, &elements);
		return next(p) ? n : NULL;
	array_failed:
		js_free(elements.items);
		return NULL;
	}
	if (is(p, L"{")) {
		vec names = { 0 }, values = { 0 };
		n = new_node(p, N_OBJECT);
		if (!next(p))
			return NULL;
		while (!is(p, L"}")) {
			js_string *name;
			if (t->kind == T_NAME)
				name = token_name(p);
			else if (t->kind == T_STRING)
				name = t->text;
			else if (t->kind == T_NUMBER)
				name = keep_string(p, js_number_string(t->number));
			else {
				fail(p, JS_E_EXPECTED_MEMBER);
				goto object_failed;
			}
			if (!next(p) || !expect(p, L":", JS_E_EXPECTED_COLON))
				goto object_failed;
			node *value = assignment(p, FALSE);
			if (!value)
				goto object_failed;
			vec_add(&names, name);
			vec_add(&values, value);
			if (is(p, L",")) {
				if (!next(p))
					goto object_failed;
			} else if (!is(p, L"}")) {
				fail(p, JS_E_EXPECTED_RBRACE);
				goto object_failed;
			}
		}
		n->count = values.count;
		n->list = vec_keep(p, &values);
		n->names = vec_keep(p, &names);
		return next(p) ? n : NULL;
	object_failed:
		js_free(names.items);
		js_free(values.items);
		return NULL;
	}
	if (t->kind == T_NAME) {
		if (is_word(p, L"function")) {
			n = new_node(p, N_FUNCTION);
			const WCHAR *start = t->start;
			if (!next(p))
				return NULL;
			js_string *name = NULL;
			if (t->kind == T_NAME && (name = identifier(p)) == NULL)
				return NULL;
			n->function = function_rest(p, name, start, TRUE);
			return n->function ? n : NULL;
		}
		node_kind kind = is_word(p, L"this") ? N_THIS : is_word(p, L"null") ? N_NULL : is_word(p, L"true") ? N_TRUE :
			is_word(p, L"false") ? N_FALSE : N_NAME;
		if (kind == N_NAME && is_keyword_token(t)) {
			fail(p, JS_E_SYNTAX);
			return NULL;
		}
		n = new_node(p, kind);
		if (kind == N_NAME) {
			n->name = token_name(p);
			note_name(p, n->name);
		}
		return next(p) ? n : NULL;
	}
	fail(p, JS_E_SYNTAX);
	return NULL;
}

/* Member and call expressions, and new with or without its arguments. */
static node *member(parser *p, BOOL calls)
{
	node *n;
	if (is_word(p, L"new")) {
		n = new_node(p, N_NEW);
		if (!next(p) || !(n->a = member(p, FALSE)))
			return NULL;
		if (is(p, L"(") && !arguments(p, n))
			return NULL;
	} else if (!(n = primary(p)))
		return NULL;
	for (;;) {
		if (is(p, L".")) {
			node *m = new_node(p, N_MEMBER);
			if (!next(p))
				return NULL;
			/* After a dot any name will do, keywords too. */
			if (p->token.kind != T_NAME) {
				fail(p, JS_E_EXPECTED_IDENTIFIER);
				return NULL;
			}
			m->a = n;
			m->name = token_name(p);
			if (!next(p))
				return NULL;
			n = m;
		} else if (is(p, L"[")) {
			node *m = new_node(p, N_INDEX);
			m->a = n;
			if (!next(p) || !(m->b = expression(p, FALSE)) || !expect(p, L"]", JS_E_EXPECTED_RBRACKET))
				return NULL;
			n = m;
		} else if (calls && is(p, L"(")) {
			node *c = new_node(p, N_CALL);
			c->a = n;
			if (n->kind == N_NAME && js_string_is(n->name, L"eval"))
				p->function->uses_eval = TRUE;
			if (!arguments(p, c))
				return NULL;
			n = c;
		} else
			return n;
	}
}

static BOOL is_reference(const node *n) { return n->kind == N_NAME || n->kind == N_MEMBER || n->kind == N_INDEX || n->kind == N_CALL; }

static node *postfix(parser *p)
{
	node *n = member(p, TRUE);
	if (!n)
		return NULL;
	if ((is(p, L"++") || is(p, L"--")) && !p->token.newline_before) {
		if (!is_reference(n)) {
			fail(p, JS_E_ILLEGAL_ASSIGN);
			return NULL;
		}
		node *u = new_node(p, N_POSTFIX);
		u->op = is(p, L"++") ? OP_INC : OP_DEC;
		u->a = n;
		return next(p) ? u : NULL;
	}
	return n;
}

static node *unary(parser *p)
{
	js_op op = OP_NONE;
	node_kind kind = N_UNARY;
	if (is(p, L"!"))
		op = OP_NOT;
	else if (is(p, L"~"))
		op = OP_BITNOT;
	else if (is(p, L"-"))
		op = OP_NEG;
	else if (is(p, L"+"))
		op = OP_PLUS;
	else if (is_word(p, L"typeof"))
		op = OP_TYPEOF;
	else if (is_word(p, L"void"))
		op = OP_VOID;
	else if (is_word(p, L"delete"))
		op = OP_DELETE;
	else if (is(p, L"++") || is(p, L"--")) {
		op = is(p, L"++") ? OP_INC : OP_DEC;
		kind = N_PREFIX;
	}
	if (op == OP_NONE)
		return postfix(p);
	node *n = new_node(p, kind);
	n->op = op;
	if (!next(p) || !(n->a = unary(p)))
		return NULL;
	if (kind == N_PREFIX && !is_reference(n->a)) {
		fail(p, JS_E_ILLEGAL_ASSIGN);
		return NULL;
	}
	return n;
}

/* Binary operators by precedence, lowest first. */
typedef struct {
	const WCHAR *text;
	js_op op;
	int level;
} binary_op;

static const binary_op binary_ops[] = {
	{ L"||", OP_OR, 1 }, { L"&&", OP_AND, 2 }, { L"|", OP_BITOR, 3 }, { L"^", OP_BITXOR, 4 }, { L"&", OP_BITAND, 5 },
	{ L"==", OP_EQ, 6 }, { L"!=", OP_NE, 6 }, { L"===", OP_SEQ, 6 }, { L"!==", OP_SNE, 6 }, { L"<", OP_LT, 7 }, { L">", OP_GT, 7 },
	{ L"<=", OP_LE, 7 }, { L">=", OP_GE, 7 }, { L"instanceof", OP_INSTANCEOF, 7 }, { L"in", OP_IN, 7 }, { L"<<", OP_SHL, 8 },
	{ L">>", OP_SAR, 8 }, { L">>>", OP_SHR, 8 }, { L"+", OP_ADD, 9 }, { L"-", OP_SUB, 9 }, { L"*", OP_MUL, 10 }, { L"/", OP_DIV, 10 },
	{ L"%", OP_MOD, 10 },
};

static const binary_op *binary_at(parser *p, BOOL no_in)
{
	for (UINT i = 0; i < ARRAYSIZE(binary_ops); i++) {
		const binary_op *b = &binary_ops[i];
		if (b->op == OP_INSTANCEOF || b->op == OP_IN) {
			if (is_word(p, b->text) && !(no_in && b->op == OP_IN))
				return b;
		} else if (is(p, b->text))
			return b;
	}
	return NULL;
}

static node *binary(parser *p, int level, BOOL no_in)
{
	node *left = unary(p);
	if (!left)
		return NULL;
	for (;;) {
		const binary_op *b = binary_at(p, no_in);
		if (!b || b->level < level)
			return left;
		node *n = new_node(p, b->op == OP_AND || b->op == OP_OR ? N_LOGICAL : N_BINARY);
		n->op = b->op;
		n->a = left;
		if (!next(p) || !(n->b = binary(p, b->level + 1, no_in)))
			return NULL;
		left = n;
	}
}

static node *conditional(parser *p, BOOL no_in)
{
	node *n = binary(p, 1, no_in);
	if (!n || !is(p, L"?"))
		return n;
	node *c = new_node(p, N_CONDITIONAL);
	c->a = n;
	if (!next(p) || !(c->b = assignment(p, FALSE)) || !expect(p, L":", JS_E_EXPECTED_COLON) || !(c->c = assignment(p, no_in)))
		return NULL;
	return c;
}

static node *assignment(parser *p, BOOL no_in)
{
	static const struct {
		const WCHAR *text;
		js_op op;
	} ops[] = {
		{ L"=", OP_NONE }, { L"+=", OP_ADD }, { L"-=", OP_SUB }, { L"*=", OP_MUL }, { L"/=", OP_DIV }, { L"%=", OP_MOD },
		{ L"<<=", OP_SHL }, { L">>=", OP_SAR }, { L">>>=", OP_SHR }, { L"&=", OP_BITAND }, { L"|=", OP_BITOR }, { L"^=", OP_BITXOR },
	};
	node *target = conditional(p, no_in);
	if (!target)
		return NULL;
	for (UINT i = 0; i < ARRAYSIZE(ops); i++)
		if (is(p, ops[i].text)) {
			if (!is_reference(target)) {
				fail(p, JS_E_ILLEGAL_ASSIGN);
				return NULL;
			}
			node *n = new_node(p, N_ASSIGN);
			n->op = ops[i].op;
			n->a = target;
			if (!next(p) || !(n->b = assignment(p, no_in)))
				return NULL;
			return n;
		}
	return target;
}

static node *expression(parser *p, BOOL no_in)
{
	node *n = assignment(p, no_in);
	while (n && is(p, L",")) {
		node *c = new_node(p, N_COMMA);
		c->a = n;
		if (!next(p) || !(c->b = assignment(p, no_in)))
			return NULL;
		n = c;
	}
	return n;
}

/* Statements. */

static void add_var(parser *p, js_string *name)
{
	function_info *f = p->function;
	for (UINT i = 0; i < f->var_count; i++)
		if (js_string_equal(f->vars[i], name))
			return;
	if (!(f->var_count & (f->var_count - 1)) || !f->var_count)
		f->vars = js_grow(f->vars, (f->var_count ? f->var_count * 2 : 4) * sizeof(js_string *));
	f->vars[f->var_count++] = name;
}

/* var a = 1, b: the names, and each one's initializer or NULL. */
static node *var_list(parser *p, BOOL no_in)
{
	node *n = new_node(p, N_VAR);
	vec names = { 0 }, values = { 0 };
	for (;;) {
		js_string *name = identifier(p);
		if (!name)
			goto failed;
		note_name(p, name);
		add_var(p, name);
		node *value = NULL;
		if (is(p, L"=")) {
			if (!next(p) || !(value = assignment(p, no_in)))
				goto failed;
		}
		vec_add(&names, name);
		vec_add(&values, value);
		if (!is(p, L","))
			break;
		if (!next(p))
			goto failed;
	}
	n->count = names.count;
	n->names = vec_keep(p, &names);
	n->list = vec_keep(p, &values);
	return n;
failed:
	js_free(names.items);
	js_free(values.items);
	return NULL;
}

static node *block(parser *p)
{
	node *n = new_node(p, N_BLOCK);
	vec items = { 0 };
	if (!expect(p, L"{", JS_E_EXPECTED_LBRACE))
		return NULL;
	while (!is(p, L"}")) {
		if (p->token.kind == T_EOF) {
			fail(p, JS_E_EXPECTED_RBRACE);
			js_free(items.items);
			return NULL;
		}
		node *s = statement(p);
		if (!s) {
			js_free(items.items);
			return NULL;
		}
		vec_add(&items, s);
	}
	n->count = items.count;
	n->list = vec_keep(p, &items);
	return next(p) ? n : NULL;
}

static node *loop_body(parser *p)
{
	p->loop_depth++;
	node *body = statement(p);
	p->loop_depth--;
	return body;
}

static node *for_statement(parser *p)
{
	node *n = new_node(p, N_FOR);
	if (!next(p) || !expect(p, L"(", JS_E_EXPECTED_LPAREN))
		return NULL;
	node *init = NULL;
	if (is_word(p, L"var")) {
		if (!next(p) || !(init = var_list(p, TRUE)))
			return NULL;
	} else if (!is(p, L";") && !(init = expression(p, TRUE)))
		return NULL;
	if (init && is_word(p, L"in")) {
		/* for (var name in object) and for (target in object). */
		if (init->kind == N_VAR ? init->count != 1 : !is_reference(init)) {
			fail(p, JS_E_SYNTAX);
			return NULL;
		}
		n->kind = N_FOR_IN;
		n->a = init;
		if (!next(p) || !(n->b = expression(p, FALSE)) || !expect(p, L")", JS_E_EXPECTED_RPAREN) || !(n->d = loop_body(p)))
			return NULL;
		return n;
	}
	n->a = init;
	if (!expect(p, L";", JS_E_EXPECTED_SEMICOLON))
		return NULL;
	if (!is(p, L";") && !(n->b = expression(p, FALSE)))
		return NULL;
	if (!expect(p, L";", JS_E_EXPECTED_SEMICOLON))
		return NULL;
	if (!is(p, L")") && !(n->c = expression(p, FALSE)))
		return NULL;
	if (!expect(p, L")", JS_E_EXPECTED_RPAREN) || !(n->d = loop_body(p)))
		return NULL;
	return n;
}

static node *switch_statement(parser *p)
{
	node *n = new_node(p, N_SWITCH);
	vec cases = { 0 };
	BOOL seen_default = FALSE;
	if (!next(p) || !expect(p, L"(", JS_E_EXPECTED_LPAREN) || !(n->a = expression(p, FALSE)) || !expect(p, L")", JS_E_EXPECTED_RPAREN) ||
		!expect(p, L"{", JS_E_EXPECTED_LBRACE))
		return NULL;
	p->switch_depth++;
	while (!is(p, L"}")) {
		node *c = new_node(p, N_BLOCK);
		if (is_word(p, L"case")) {
			if (!next(p) || !(c->a = expression(p, FALSE)))
				goto failed;
		} else if (is_word(p, L"default")) {
			if (seen_default) {
				fail(p, JS_E_SYNTAX);
				goto failed;
			}
			seen_default = TRUE;
			c->flags = 1;
			if (!next(p))
				goto failed;
		} else {
			fail(p, JS_E_EXPECTED_RBRACE);
			goto failed;
		}
		if (!expect(p, L":", JS_E_EXPECTED_COLON))
			goto failed;
		vec items = { 0 };
		while (!is(p, L"}") && !is_word(p, L"case") && !is_word(p, L"default")) {
			if (p->token.kind == T_EOF) {
				fail(p, JS_E_EXPECTED_RBRACE);
				js_free(items.items);
				goto failed;
			}
			node *s = statement(p);
			if (!s) {
				js_free(items.items);
				goto failed;
			}
			vec_add(&items, s);
		}
		c->count = items.count;
		c->list = vec_keep(p, &items);
		vec_add(&cases, c);
	}
	p->switch_depth--;
	n->count = cases.count;
	n->list = vec_keep(p, &cases);
	return next(p) ? n : NULL;
failed:
	p->switch_depth--;
	js_free(cases.items);
	return NULL;
}

static node *try_statement(parser *p)
{
	node *n = new_node(p, N_TRY);
	if (!next(p) || !(n->a = block(p)))
		return NULL;
	if (is_word(p, L"catch")) {
		if (!next(p) || !expect(p, L"(", JS_E_EXPECTED_LPAREN) || !(n->name = identifier(p)) || !expect(p, L")", JS_E_EXPECTED_RPAREN) ||
			!(n->b = block(p)))
			return NULL;
	}
	if (is_word(p, L"finally")) {
		if (!next(p) || !(n->c = block(p)))
			return NULL;
	}
	if (!n->b && !n->c) {
		fail(p, JS_E_EXPECTED_CATCH);
		return NULL;
	}
	return n;
}

static BOOL label_known(parser *p, const js_string *name, BOOL loops_only)
{
	for (UINT i = 0; i < p->label_count; i++)
		if (js_string_equal(p->labels[i], name))
			return !loops_only || p->label_loops[i];
	return FALSE;
}

static node *jump(parser *p, node_kind kind)
{
	node *n = new_node(p, kind);
	if (!next(p))
		return NULL;
	if (p->token.kind == T_NAME && !p->token.newline_before && !is_keyword_token(&p->token)) {
		n->name = token_name(p);
		if (!label_known(p, n->name, kind == N_CONTINUE)) {
			fail(p, JS_E_LABEL_NOT_FOUND);
			return NULL;
		}
		if (!next(p))
			return NULL;
	} else if (kind == N_BREAK ? !p->loop_depth && !p->switch_depth : !p->loop_depth) {
		fail_at(p, kind == N_BREAK ? JS_E_BREAK_OUTSIDE : JS_E_CONTINUE_OUTSIDE, n->line, n->column);
		return NULL;
	}
	return semicolon(p) ? n : NULL;
}

static BOOL starts_loop(parser *p) { return is_word(p, L"for") || is_word(p, L"while") || is_word(p, L"do"); }

static node *statement(parser *p)
{
	node *n;
	token *t = &p->token;
	if (is(p, L"{"))
		return block(p);
	if (is(p, L";")) {
		n = new_node(p, N_EMPTY);
		return next(p) ? n : NULL;
	}
	if (t->kind == T_NAME) {
		if (is_word(p, L"var")) {
			if (!next(p) || !(n = var_list(p, FALSE)))
				return NULL;
			return semicolon(p) ? n : NULL;
		}
		if (is_word(p, L"if")) {
			n = new_node(p, N_IF);
			if (!next(p) || !expect(p, L"(", JS_E_EXPECTED_LPAREN) || !(n->a = expression(p, FALSE)) || !expect(p, L")", JS_E_EXPECTED_RPAREN) ||
				!(n->b = statement(p)))
				return NULL;
			if (is_word(p, L"else") && (!next(p) || !(n->c = statement(p))))
				return NULL;
			return n;
		}
		if (is_word(p, L"do")) {
			n = new_node(p, N_DO);
			if (!next(p) || !(n->a = loop_body(p)))
				return NULL;
			if (!is_word(p, L"while")) {
				fail(p, JS_E_EXPECTED_WHILE);
				return NULL;
			}
			if (!next(p) || !expect(p, L"(", JS_E_EXPECTED_LPAREN) || !(n->b = expression(p, FALSE)) || !expect(p, L")", JS_E_EXPECTED_RPAREN))
				return NULL;
			/* The semicolon after do-while may always be left out. */
			if (is(p, L";") && !next(p))
				return NULL;
			return n;
		}
		if (is_word(p, L"while")) {
			n = new_node(p, N_WHILE);
			if (!next(p) || !expect(p, L"(", JS_E_EXPECTED_LPAREN) || !(n->a = expression(p, FALSE)) || !expect(p, L")", JS_E_EXPECTED_RPAREN) ||
				!(n->b = loop_body(p)))
				return NULL;
			return n;
		}
		if (is_word(p, L"for"))
			return for_statement(p);
		if (is_word(p, L"continue"))
			return jump(p, N_CONTINUE);
		if (is_word(p, L"break"))
			return jump(p, N_BREAK);
		if (is_word(p, L"return")) {
			n = new_node(p, N_RETURN);
			if (!p->in_function) {
				fail(p, JS_E_RETURN_OUTSIDE);
				return NULL;
			}
			if (!next(p))
				return NULL;
			if (!is(p, L";") && !is(p, L"}") && t->kind != T_EOF && !t->newline_before && !(n->a = expression(p, FALSE)))
				return NULL;
			return semicolon(p) ? n : NULL;
		}
		if (is_word(p, L"with")) {
			n = new_node(p, N_WITH);
			if (!next(p) || !expect(p, L"(", JS_E_EXPECTED_LPAREN) || !(n->a = expression(p, FALSE)) || !expect(p, L")", JS_E_EXPECTED_RPAREN) ||
				!(n->b = statement(p)))
				return NULL;
			return n;
		}
		if (is_word(p, L"switch"))
			return switch_statement(p);
		if (is_word(p, L"throw")) {
			n = new_node(p, N_THROW);
			if (!next(p))
				return NULL;
			if (t->newline_before) {
				fail(p, JS_E_SYNTAX);
				return NULL;
			}
			if (!(n->a = expression(p, FALSE)))
				return NULL;
			return semicolon(p) ? n : NULL;
		}
		if (is_word(p, L"try"))
			return try_statement(p);
		if (is_word(p, L"function")) {
			/* A declaration, wherever it is: JScript makes it when the function it is in starts. */
			n = new_node(p, N_FUNCTION_DECLARATION);
			const WCHAR *start = t->start;
			js_string *name;
			if (!next(p) || !(name = identifier(p)))
				return NULL;
			if (!(n->function = function_rest(p, name, start, FALSE)))
				return NULL;
			function_info *f = p->function;
			f->functions = js_grow(f->functions, (f->function_count + 1) * sizeof(node *));
			f->functions[f->function_count++] = n;
			return n;
		}
		if (!is_keyword_token(t)) {
			/* A label: a name and a colon. */
			const WCHAR *save_at = p->at, *save_line_start = p->line_start;
			UINT save_line = p->line;
			token save = *t;
			js_string *name = token_name(p);
			if (!next(p))
				return NULL;
			if (is(p, L":")) {
				if (label_known(p, name, FALSE)) {
					fail(p, JS_E_LABEL_REDEFINED);
					return NULL;
				}
				n = new_node(p, N_LABEL);
				n->name = name;
				if (!next(p))
					return NULL;
				p->labels = js_grow(p->labels, (p->label_count + 1) * sizeof(js_string *));
				p->label_loops = js_grow(p->label_loops, (p->label_count + 1) * sizeof(BOOL));
				p->labels[p->label_count] = name;
				p->label_loops[p->label_count] = starts_loop(p);
				p->label_count++;
				/* A label lets break leave any statement it names. */
				p->switch_depth++;
				n->a = statement(p);
				p->switch_depth--;
				p->label_count--;
				return n->a ? n : NULL;
			}
			/* Not a label: read the name again as the start of an expression. */
			p->at = save_at;
			p->line_start = save_line_start;
			p->line = save_line;
			*t = save;
		}
	}
	n = new_node(p, N_EXPRESSION);
	if (!(n->a = expression(p, FALSE)))
		return NULL;
	return semicolon(p) ? n : NULL;
}

/* Statements up to a closing brace, or the end for a program. */
static BOOL source_elements(parser *p, function_info *f, BOOL program)
{
	vec items = { 0 };
	while (program ? p->token.kind != T_EOF : !is(p, L"}")) {
		if (p->token.kind == T_EOF) {
			fail(p, JS_E_EXPECTED_RBRACE);
			js_free(items.items);
			return FALSE;
		}
		node *s = statement(p);
		if (!s) {
			js_free(items.items);
			return FALSE;
		}
		vec_add(&items, s);
	}
	f->body_count = items.count;
	f->body = vec_keep(p, &items);
	return TRUE;
}

static void keep_function(parser *p, function_info *f)
{
	if (f->vars)
		keep(p, f->vars);
	if (f->functions)
		keep(p, f->functions);
}

/* After the name: the parameters and the body, in a function of its own. */
static function_info *function_rest(parser *p, js_string *name, const WCHAR *start, BOOL is_expression)
{
	function_info *f = keep(p, js_alloc(sizeof(function_info)));
	f->name = name;
	f->is_expression = is_expression;
	vec params = { 0 };
	if (!expect(p, L"(", JS_E_EXPECTED_LPAREN))
		return NULL;
	if (!is(p, L")"))
		for (;;) {
			js_string *param = identifier(p);
			if (!param) {
				js_free(params.items);
				return NULL;
			}
			vec_add(&params, param);
			if (!is(p, L","))
				break;
			if (!next(p)) {
				js_free(params.items);
				return NULL;
			}
		}
	f->param_count = params.count;
	f->params = vec_keep(p, &params);
	if (!expect(p, L")", JS_E_EXPECTED_RPAREN))
		return NULL;
	if (!is(p, L"{")) {
		fail(p, JS_E_EXPECTED_LBRACE);
		return NULL;
	}
	function_info *outer = p->function;
	UINT loops = p->loop_depth, switches = p->switch_depth, labels = p->label_count;
	BOOL in_function = p->in_function;
	p->function = f;
	p->loop_depth = p->switch_depth = p->label_count = 0;
	p->in_function = TRUE;
	BOOL fine = next(p) && source_elements(p, f, FALSE);
	keep_function(p, f);
	p->function = outer;
	p->loop_depth = loops;
	p->switch_depth = switches;
	p->label_count = labels;
	p->in_function = in_function;
	if (!fine)
		return NULL;
	/* The text runs from "function" to the closing brace. */
	f->source = keep_string(p, js_string_new(start, (UINT)(p->token.start + 1 - start)));
	return next(p) ? f : NULL;
}

script_code *js_parse(const WCHAR *source, BOOL expression_only, parse_error *error)
{
	script_code *code = js_alloc(sizeof(script_code));
	arena *a = js_alloc(sizeof(arena));
	code->arena = a;
	code->source = js_wcsdup(source);
	parser p = { 0 };
	p.source = p.at = p.line_start = code->source;
	p.line = 1;
	p.error = error;
	p.arena = a;
	function_info *program = keep(&p, js_alloc(sizeof(function_info)));
	p.function = program;
	code->program = program;
	BOOL fine = next(&p);
	if (fine && expression_only) {
		/* An expression the host wants the value of: one expression statement, with an optional semicolon. */
		node *n = new_node(&p, N_EXPRESSION);
		if ((n->a = expression(&p, FALSE)) != NULL && (!is(&p, L";") || next(&p)) && p.token.kind == T_EOF) {
			program->body = keep(&p, js_alloc(sizeof(node *)));
			program->body[0] = n;
			program->body_count = 1;
		} else
			fine = fail(&p, JS_E_SYNTAX);
	} else if (fine)
		fine = source_elements(&p, program, TRUE);
	keep_function(&p, program);
	js_free(p.labels);
	js_free(p.label_loops);
	if (!fine || p.failed) {
		if (!p.failed)
			fail(&p, JS_E_SYNTAX);
		js_code_free(code);
		return NULL;
	}
	return code;
}

void js_code_free(script_code *code)
{
	if (!code)
		return;
	arena *a = code->arena;
	for (UINT i = 0; i < a->count; i++)
		js_free(a->blocks[i]);
	for (UINT i = 0; i < a->string_count; i++)
		js_string_release(a->strings[i]);
	js_free(a->blocks);
	js_free(a->strings);
	js_free(a);
	js_free(code->source);
	js_free(code);
}
