/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

/* VBScript custom actions, run by an interpreter of the language's statements and expressions in the installing process. The script
 * sees the installer's Session object with its properties, records, messages, conditions, paths and feature and component states,
 * the Err object and the common string and conversion functions. Absent: COM objects (CreateObject fails), classes, Execute,
 * regular expressions, dates and arguments passed by reference, which are passed by value. */

typedef enum { V_EMPTY, V_NULL, V_BOOL, V_NUMBER, V_TEXT, V_OBJECT, V_NOTHING, V_ARRAY } value_kind;
typedef enum { O_SESSION, O_INSTALLER, O_RECORD, O_ERR } script_object_kind;

typedef struct script_object {
	LONG refs;
	script_object_kind kind;
	MSIHANDLE handle;
} script_object;

typedef struct script_array script_array;

typedef struct {
	value_kind kind;
	double number;
	WCHAR *text;
	script_object *object;
	script_array *array;
} value;

struct script_array {
	UINT count;
	value *items;
};

static value value_copy(const value *from);

static void value_clear(value *v)
{
	msi_free(v->text);
	if (v->object && !--v->object->refs) {
		if (v->object->kind == O_RECORD)
			MsiCloseHandle(v->object->handle);
		msi_free(v->object);
	}
	if (v->array) {
		for (UINT i = 0; i < v->array->count; i++)
			value_clear(&v->array->items[i]);
		msi_free(v->array->items);
		msi_free(v->array);
	}
	*v = (value){V_EMPTY};
}

static value value_copy(const value *from)
{
	value v = *from;
	if (from->text)
		v.text = msi_copy(from->text);
	if (from->object)
		from->object->refs++;
	if (from->array) {
		v.array = msi_alloc(sizeof(script_array));
		v.array->count = from->array->count;
		v.array->items = msi_alloc((from->array->count + 1) * sizeof(value));
		for (UINT i = 0; i < from->array->count; i++)
			v.array->items[i] = value_copy(&from->array->items[i]);
	}
	return v;
}

static value number_value(double number) { return (value){V_NUMBER, number}; }
static value bool_value(BOOL yes) { return (value){V_BOOL, yes ? -1 : 0}; }

static value text_value(const WCHAR *text)
{
	value v = {V_TEXT};
	v.text = msi_copy(text ? text : L"");
	return v;
}

static value text_taken(WCHAR *text)
{
	value v = {V_TEXT};
	v.text = text ? text : msi_copy(L"");
	return v;
}

static value object_value(script_object_kind kind, MSIHANDLE handle)
{
	value v = {V_OBJECT};
	v.object = msi_alloc(sizeof(script_object));
	v.object->refs = 1;
	v.object->kind = kind;
	v.object->handle = handle;
	return v;
}

/* Numbers. The library has no C runtime, so text and numbers convert here: up to fifteen significant digits, as VBScript shows. */

static double whole(double x)
{
	if (x >= 9.2e18 || x <= -9.2e18)
		return x;
	double w = (double)(LONGLONG)x;
	return w > x ? w - 1 : w;
}

static BOOL parse_number(const WCHAR *text, double *out)
{
	const WCHAR *at = text;
	while (*at == L' ' || *at == L'\t')
		at++;
	BOOL negative = FALSE, digits = FALSE;
	if (*at == L'-' || *at == L'+')
		negative = *at++ == L'-';
	double number = 0;
	if (at[0] == L'&' && (at[1] == L'h' || at[1] == L'H')) {
		at += 2;
		for (; (*at >= L'0' && *at <= L'9') || (*at >= L'a' && *at <= L'f') || (*at >= L'A' && *at <= L'F'); at++, digits = TRUE)
			number = number * 16 + (*at <= L'9' ? *at - L'0' : (*at | 0x20) - L'a' + 10);
	} else {
		for (; *at >= L'0' && *at <= L'9'; at++, digits = TRUE)
			number = number * 10 + (*at - L'0');
		if (*at == L'.') {
			double scale = 0.1;
			for (at++; *at >= L'0' && *at <= L'9'; at++, digits = TRUE, scale /= 10)
				number += (*at - L'0') * scale;
		}
		if (digits && (*at == L'e' || *at == L'E')) {
			BOOL down = FALSE;
			int exponent = 0;
			at++;
			if (*at == L'-' || *at == L'+')
				down = *at++ == L'-';
			while (*at >= L'0' && *at <= L'9' && exponent < 400)
				exponent = exponent * 10 + (*at++ - L'0');
			while (exponent--)
				number = down ? number / 10 : number * 10;
		}
	}
	while (*at == L' ' || *at == L'\t')
		at++;
	if (!digits || *at)
		return FALSE;
	*out = negative ? -number : number;
	return TRUE;
}

static WCHAR *number_text(double number)
{
	WCHAR buffer[64], *at = buffer;
	if (number < 0) {
		*at++ = L'-';
		number = -number;
	}
	if (number == whole(number) && number < 1e15) {
		WCHAR digits[24];
		int count = 0;
		ULONGLONG n = (ULONGLONG)number;
		do
			digits[count++] = (WCHAR)(L'0' + n % 10);
		while (n /= 10);
		while (count)
			*at++ = digits[--count];
		*at = 0;
		return msi_copy(buffer);
	}
	int exponent = 0;
	while (number >= 10) {
		number /= 10;
		exponent++;
	}
	while (number < 1 && number > 0) {
		number *= 10;
		exponent--;
	}
	int digits[15];
	for (int i = 0; i < 15; i++) {
		digits[i] = (int)number;
		number = (number - digits[i]) * 10;
	}
	if (number >= 5)
		for (int i = 14; i >= 0 && ++digits[i] == 10; i--) {
			digits[i] = 0;
			if (!i) {
				digits[0] = 1;
				exponent++;
			}
		}
	int last = 14;
	while (last > 0 && !digits[last])
		last--;
	if (exponent >= 15 || exponent < -5) {
		*at++ = (WCHAR)(L'0' + digits[0]);
		if (last) {
			*at++ = L'.';
			for (int i = 1; i <= last; i++)
				*at++ = (WCHAR)(L'0' + digits[i]);
		}
		wsprintfW(at, L"E%c%02d", exponent < 0 ? L'-' : L'+', exponent < 0 ? -exponent : exponent);
		return msi_copy(buffer);
	}
	if (exponent < 0) {
		*at++ = L'0';
		*at++ = L'.';
		for (int i = -1; i > exponent; i--)
			*at++ = L'0';
		for (int i = 0; i <= last; i++)
			*at++ = (WCHAR)(L'0' + digits[i]);
	} else {
		for (int i = 0; i <= exponent; i++)
			*at++ = (WCHAR)(L'0' + (i <= last ? digits[i] : 0));
		if (last > exponent) {
			*at++ = L'.';
			for (int i = exponent + 1; i <= last; i++)
				*at++ = (WCHAR)(L'0' + digits[i]);
		}
	}
	*at = 0;
	return msi_copy(buffer);
}

/* The lexer: identifiers and keywords, numbers, strings with doubled quotes, operators, and line ends, which a colon also makes.
 * A comment runs to the end of its line; an underscore at the end of a line continues the statement on the next. */

typedef enum { T_END, T_LINE, T_NAME, T_NUMBER, T_TEXT, T_SYMBOL } token_kind;

typedef struct {
	token_kind kind;
	WCHAR *text;
	double number;
	WCHAR symbol[3];
	UINT line;
} token;

typedef struct {
	token *tokens;
	UINT count, room, at;
} token_list;

static void push_token(token_list *list, token t)
{
	if (list->count == list->room) {
		list->room = list->room ? list->room * 2 : 256;
		list->tokens = msi_grow(list->tokens, list->room * sizeof(token));
	}
	list->tokens[list->count++] = t;
}

static BOOL is_name_char(WCHAR c, BOOL first)
{
	return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || (!first && c >= L'0' && c <= L'9');
}

static WCHAR lower_char(WCHAR c)
{
	return c >= L'A' && c <= L'Z' ? c + 32 : c;
}

static BOOL same_word(const WCHAR *a, const WCHAR *b)
{
	if (!a)
		return FALSE;
	while (*a && lower_char(*a) == lower_char(*b))
		a++, b++;
	return !*a && !*b;
}

static BOOL starts_with_word(const WCHAR *at, const WCHAR *word, SIZE_T length)
{
	for (SIZE_T i = 0; i < length; i++)
		if (lower_char(at[i]) != lower_char(word[i]))
			return FALSE;
	return TRUE;
}

static BOOL has_char(const WCHAR *set, WCHAR c)
{
	for (; *set; set++)
		if (*set == c)
			return TRUE;
	return FALSE;
}

static BOOL tokenize(const WCHAR *code, token_list *list)
{
	UINT line = 1;
	const WCHAR *at = code;
	while (*at) {
		WCHAR c = *at;
		if (c == L' ' || c == L'\t' || c == L'\r') {
			at++;
		} else if (c == L'\n' || c == L':') {
			if (c == L'\n')
				line++;
			// A colon separates statements as a line end does; it stays known as one for the one-line If.
			push_token(list, (token){T_LINE, .symbol = {c == L':' ? L':' : 0}, .line = line});
			at++;
		} else if (c == L'\'' || ((c == L'r' || c == L'R') && starts_with_word(at, L"rem", 3) && !is_name_char(at[3], FALSE) &&
			(at == code || !is_name_char(at[-1], FALSE)))) {
			while (*at && *at != L'\n')
				at++;
		} else if (c == L'_' && !is_name_char(at[1], FALSE)) {
			// A continuation: the rest of the line is blank and the statement goes on.
			const WCHAR *next = at + 1;
			while (*next == L' ' || *next == L'\t' || *next == L'\r')
				next++;
			if (*next != L'\n' && *next)
				return FALSE;
			at = *next ? next + 1 : next;
			line++;
		} else if (is_name_char(c, TRUE)) {
			const WCHAR *start = at;
			while (is_name_char(*at, FALSE))
				at++;
			push_token(list, (token){T_NAME, msi_copy_n(start, at - start), .line = line});
		} else if ((c >= L'0' && c <= L'9') || (c == L'.' && at[1] >= L'0' && at[1] <= L'9') || (c == L'&' && (at[1] == L'h' || at[1] == L'H'))) {
			const WCHAR *start = at;
			if (c == L'&') {
				at += 2;
				while ((*at >= L'0' && *at <= L'9') || (*at >= L'a' && *at <= L'f') || (*at >= L'A' && *at <= L'F'))
					at++;
				if (*at == L'&')
					at++;
			} else {
				while ((*at >= L'0' && *at <= L'9') || *at == L'.')
					at++;
				if ((*at == L'e' || *at == L'E') && (at[1] == L'-' || at[1] == L'+' || (at[1] >= L'0' && at[1] <= L'9'))) {
					at += 2;
					while (*at >= L'0' && *at <= L'9')
						at++;
				}
			}
			WCHAR *text = msi_copy_n(start, at - start);
			SIZE_T length = lstrlenW(text);
			if (length > 2 && text[0] == L'&' && text[length - 1] == L'&')
				text[length - 1] = 0;
			double number;
			BOOL fine = parse_number(text, &number);
			msi_free(text);
			if (!fine)
				return FALSE;
			push_token(list, (token){T_NUMBER, .number = number, .line = line});
		} else if (c == L'"') {
			SIZE_T room = 16, length = 0;
			WCHAR *text = msi_alloc(room * sizeof(WCHAR));
			for (at++;; at++) {
				if (!*at || *at == L'\n') {
					msi_free(text);
					return FALSE;
				}
				if (*at == L'"') {
					if (at[1] != L'"')
						break;
					at++;
				}
				if (length + 2 > room)
					text = msi_grow(text, (room *= 2) * sizeof(WCHAR));
				text[length++] = *at;
			}
			text[length] = 0;
			at++;
			push_token(list, (token){T_TEXT, text, .line = line});
		} else {
			token t = {T_SYMBOL, .line = line};
			static const WCHAR *pairs[] = {L"<>", L"<=", L">=", NULL};
			for (int i = 0; pairs[i]; i++)
				if (at[0] == pairs[i][0] && at[1] == pairs[i][1]) {
					t.symbol[0] = at[0];
					t.symbol[1] = at[1];
				}
			if (!t.symbol[0]) {
				if (!has_char(L"+-*/\\^&=<>(),.", c))
					return FALSE;
				t.symbol[0] = c;
			}
			at += lstrlenW(t.symbol);
			push_token(list, t);
		}
	}
	push_token(list, (token){T_LINE, .line = line});
	push_token(list, (token){T_END, .line = line});
	return TRUE;
}

/* The parser builds statements and expressions; everything it allocates is kept on one list and freed with the script. */

typedef enum { E_CONSTANT, E_NAME, E_MEMBER, E_CALL, E_BINARY, E_NOT, E_NEGATE } script_expr_kind;

typedef struct expr {
	script_expr_kind kind;
	UINT line;
	value constant;
	WCHAR *name;
	struct expr *target, *left, *right;
	struct expr **args;
	UINT arg_count;
	WCHAR op[4];
	BOOL parenthesized;
} expr;

typedef enum { S_CALL, S_ASSIGN, S_IF, S_FOR, S_FOR_EACH, S_DO, S_EXIT, S_DIM, S_ON_ERROR, S_SELECT } stmt_kind;

typedef struct block {
	struct stmt **items;
	UINT count;
} block;

typedef struct stmt {
	stmt_kind kind;
	UINT line;
	expr *target, *value, *to, *step;
	WCHAR *name;
	expr **conditions;	// If: one per branch; Do: the one condition; Select: the tested value in value
	block *blocks;
	UINT branch_count;
	block otherwise;
	BOOL has_otherwise, until, test_first, resume_next, preserve, set, redim;
	WCHAR exit_kind[12];
	expr ***case_values;
	UINT *case_counts;
	expr **dims;		// Dim: the size of each name, or NULL
	WCHAR **names;
	UINT name_count;
} stmt;

typedef struct {
	WCHAR *name;
	WCHAR **params;
	UINT param_count;
	block body;
	BOOL function;
} procedure;

typedef struct {
	token_list tokens;
	void **owned;
	UINT owned_count, owned_room;
	procedure *procedures;
	UINT procedure_count;
	block main;
	WCHAR error[160];
} parser;

static void *own(parser *p, SIZE_T size)
{
	if (p->owned_count == p->owned_room) {
		p->owned_room = p->owned_room ? p->owned_room * 2 : 256;
		p->owned = msi_grow(p->owned, p->owned_room * sizeof(void *));
	}
	return p->owned[p->owned_count++] = msi_alloc(size);
}

static token *peek(parser *p)
{
	return &p->tokens.tokens[p->tokens.at];
}

static token *advance(parser *p)
{
	token *t = peek(p);
	if (t->kind != T_END)
		p->tokens.at++;
	return t;
}

static BOOL is_word(parser *p, const WCHAR *word)
{
	return peek(p)->kind == T_NAME && same_word(peek(p)->text, word);
}

static BOOL is_word_at(parser *p, UINT offset, const WCHAR *word)
{
	if (p->tokens.at + offset >= p->tokens.count)
		return FALSE;
	token *t = &p->tokens.tokens[p->tokens.at + offset];
	return t->kind == T_NAME && same_word(t->text, word);
}

static BOOL accept_word(parser *p, const WCHAR *word)
{
	if (!is_word(p, word))
		return FALSE;
	advance(p);
	return TRUE;
}

static BOOL is_symbol(parser *p, const WCHAR *symbol)
{
	return peek(p)->kind == T_SYMBOL && !lstrcmpW(peek(p)->symbol, symbol);
}

static BOOL accept_symbol(parser *p, const WCHAR *symbol)
{
	if (!is_symbol(p, symbol))
		return FALSE;
	advance(p);
	return TRUE;
}

static BOOL fail(parser *p, const WCHAR *what)
{
	if (!p->error[0])
		wsprintfW(p->error, L"line %u: %s", peek(p)->line, what);
	return FALSE;
}

static BOOL is_colon(parser *p)
{
	return peek(p)->kind == T_LINE && peek(p)->symbol[0] == L':';
}

static BOOL at_line_end(parser *p)
{
	return peek(p)->kind == T_LINE || peek(p)->kind == T_END || is_word(p, L"else");
}

static expr *new_expr(parser *p, script_expr_kind kind)
{
	expr *e = own(p, sizeof(expr));
	e->kind = kind;
	e->line = peek(p)->line;
	return e;
}

static void add_arg(parser *p, expr *call, expr *arg)
{
	expr **args = own(p, (call->arg_count + 1) * sizeof(expr *));
	for (UINT i = 0; i < call->arg_count; i++)
		args[i] = call->args[i];
	args[call->arg_count++] = arg;
	call->args = args;
}

static expr *parse_expression(parser *p);

static BOOL parse_arguments(parser *p, expr *call)
{
	// The opening parenthesis is taken; an empty argument stands for one left out.
	if (accept_symbol(p, L")"))
		return TRUE;
	for (;;) {
		if (is_symbol(p, L",") || is_symbol(p, L")")) {
			add_arg(p, call, NULL);
		} else {
			expr *arg = parse_expression(p);
			if (!arg)
				return FALSE;
			add_arg(p, call, arg);
		}
		if (accept_symbol(p, L")"))
			return TRUE;
		if (!accept_symbol(p, L","))
			return fail(p, L"a comma or a closing parenthesis was expected");
	}
}

static expr *parse_postfix(parser *p, expr *e)
{
	for (;;) {
		if (accept_symbol(p, L".")) {
			if (peek(p)->kind != T_NAME)
				return fail(p, L"a member name was expected"), NULL;
			expr *member = new_expr(p, E_MEMBER);
			member->target = e;
			member->name = advance(p)->text;
			e = member;
		} else if (is_symbol(p, L"(")) {
			advance(p);
			expr *call = new_expr(p, E_CALL);
			call->target = e;
			if (!parse_arguments(p, call))
				return NULL;
			e = call;
		} else
			return e;
	}
}

static expr *parse_primary(parser *p)
{
	token *t = peek(p);
	expr *e;
	if (t->kind == T_NUMBER || t->kind == T_TEXT) {
		advance(p);
		e = new_expr(p, E_CONSTANT);
		e->constant = t->kind == T_NUMBER ? number_value(t->number) : (value){V_TEXT, 0, t->text};
		return e;
	}
	if (accept_symbol(p, L"(")) {
		e = parse_expression(p);
		if (!e || !accept_symbol(p, L")"))
			return fail(p, L"a closing parenthesis was expected"), NULL;
		if (e->kind == E_NAME || e->kind == E_CALL || e->kind == E_MEMBER) {
			// A parenthesized name is a value, not a place.
			expr *wrapped = new_expr(p, E_BINARY);
			lstrcpyW(wrapped->op, L"()");
			wrapped->left = e;
			e = wrapped;
		}
		return parse_postfix(p, e);
	}
	if (t->kind != T_NAME)
		return fail(p, L"an expression was expected"), NULL;
	static const struct { const WCHAR *word; value_kind kind; double number; } words[] = {
		{L"true", V_BOOL, -1}, {L"false", V_BOOL, 0}, {L"empty", V_EMPTY, 0}, {L"null", V_NULL, 0}, {L"nothing", V_NOTHING, 0},
	};
	for (UINT i = 0; i < ARRAYSIZE(words); i++)
		if (same_word(t->text, words[i].word)) {
			advance(p);
			e = new_expr(p, E_CONSTANT);
			e->constant = (value){words[i].kind, words[i].number};
			return e;
		}
	advance(p);
	e = new_expr(p, E_NAME);
	e->name = t->text;
	return parse_postfix(p, e);
}

static expr *binary(parser *p, const WCHAR *op, expr *left, expr *right)
{
	if (!left || !right)
		return NULL;
	expr *e = new_expr(p, E_BINARY);
	lstrcpynW(e->op, op, ARRAYSIZE(e->op));
	e->left = left;
	e->right = right;
	return e;
}

static expr *parse_power(parser *p)
{
	expr *e = parse_primary(p);
	while (e && accept_symbol(p, L"^"))
		e = binary(p, L"^", e, parse_primary(p));
	return e;
}

static expr *parse_unary(parser *p)
{
	if (accept_symbol(p, L"-")) {
		expr *e = new_expr(p, E_NEGATE);
		e->left = parse_unary(p);
		return e->left ? e : NULL;
	}
	if (accept_symbol(p, L"+"))
		return parse_unary(p);
	return parse_power(p);
}

static expr *parse_product(parser *p)
{
	expr *e = parse_unary(p);
	while (e && (is_symbol(p, L"*") || is_symbol(p, L"/"))) {
		WCHAR op[2] = {advance(p)->symbol[0]};
		e = binary(p, op, e, parse_unary(p));
	}
	return e;
}

static expr *parse_integer_division(parser *p)
{
	expr *e = parse_product(p);
	while (e && accept_symbol(p, L"\\"))
		e = binary(p, L"\\", e, parse_product(p));
	return e;
}

static expr *parse_modulo(parser *p)
{
	expr *e = parse_integer_division(p);
	while (e && accept_word(p, L"mod"))
		e = binary(p, L"mod", e, parse_integer_division(p));
	return e;
}

static expr *parse_sum(parser *p)
{
	expr *e = parse_modulo(p);
	while (e && (is_symbol(p, L"+") || is_symbol(p, L"-"))) {
		WCHAR op[2] = {advance(p)->symbol[0]};
		e = binary(p, op, e, parse_modulo(p));
	}
	return e;
}

static expr *parse_concatenation(parser *p)
{
	expr *e = parse_sum(p);
	while (e && accept_symbol(p, L"&"))
		e = binary(p, L"&", e, parse_sum(p));
	return e;
}

static expr *parse_comparison(parser *p)
{
	expr *e = parse_concatenation(p);
	static const WCHAR *ops[] = {L"=", L"<>", L"<", L">", L"<=", L">="};
	while (e) {
		const WCHAR *op = NULL;
		for (UINT i = 0; i < ARRAYSIZE(ops); i++)
			if (is_symbol(p, ops[i]))
				op = ops[i];
		if (op)
			advance(p);
		else if (accept_word(p, L"is"))
			op = L"is";
		else
			break;
		e = binary(p, op, e, parse_concatenation(p));
	}
	return e;
}

static expr *parse_not(parser *p)
{
	if (accept_word(p, L"not")) {
		expr *e = new_expr(p, E_NOT);
		e->left = parse_not(p);
		return e->left ? e : NULL;
	}
	return parse_comparison(p);
}

static expr *parse_logic(parser *p, UINT level)
{
	static const WCHAR *words[] = {L"imp", L"eqv", L"xor", L"or", L"and"};
	if (level == ARRAYSIZE(words))
		return parse_not(p);
	expr *e = parse_logic(p, level + 1);
	while (e && accept_word(p, words[level]))
		e = binary(p, words[level], e, parse_logic(p, level + 1));
	return e;
}

static expr *parse_expression(parser *p)
{
	return parse_logic(p, 0);
}

static stmt *new_stmt(parser *p, stmt_kind kind)
{
	stmt *s = own(p, sizeof(stmt));
	s->kind = kind;
	s->line = peek(p)->line;
	return s;
}

static void add_stmt(parser *p, block *b, stmt *s)
{
	stmt **items = own(p, (b->count + 1) * sizeof(stmt *));
	for (UINT i = 0; i < b->count; i++)
		items[i] = b->items[i];
	items[b->count++] = s;
	b->items = items;
}

static BOOL skip_lines(parser *p)
{
	while (peek(p)->kind == T_LINE)
		advance(p);
	return TRUE;
}

static BOOL end_of_statement(parser *p)
{
	if (peek(p)->kind == T_LINE || peek(p)->kind == T_END)
		return TRUE;
	return fail(p, L"the end of the statement was expected");
}

static BOOL parse_statement(parser *p, block *b, BOOL single_line);

// Statements up to one of the words that close the block, which is left for the caller.
static BOOL parse_block(parser *p, block *b, const WCHAR *const *ends)
{
	for (;;) {
		skip_lines(p);
		if (peek(p)->kind == T_END)
			return fail(p, L"the block is not closed");
		for (UINT i = 0; ends[i]; i++) {
			if (!lstrcmpiW(ends[i], L"end if") ? (is_word(p, L"end") && is_word_at(p, 1, L"if")) :
				!lstrcmpiW(ends[i], L"end select") ? (is_word(p, L"end") && is_word_at(p, 1, L"select")) :
				!lstrcmpiW(ends[i], L"end function") ? (is_word(p, L"end") && is_word_at(p, 1, L"function")) :
				!lstrcmpiW(ends[i], L"end sub") ? (is_word(p, L"end") && is_word_at(p, 1, L"sub")) : is_word(p, ends[i]))
				return TRUE;
		}
		if (!parse_statement(p, b, FALSE))
			return FALSE;
	}
}

static BOOL parse_if(parser *p, block *b)
{
	stmt *s = new_stmt(p, S_IF);
	expr *condition = parse_expression(p);
	if (!condition || !accept_word(p, L"then"))
		return fail(p, L"Then was expected");
	s->conditions = own(p, 64 * sizeof(expr *));
	s->blocks = own(p, 64 * sizeof(block));
	s->conditions[0] = condition;
	s->branch_count = 1;
	if (peek(p)->kind != T_LINE) {
		// The one-line form: statements up to the end of the line, then perhaps Else and more.
		while (!at_line_end(p) || is_colon(p)) {
			if (is_colon(p)) {
				advance(p);
				continue;
			}
			if (!parse_statement(p, &s->blocks[0], TRUE))
				return FALSE;
		}
		if (accept_word(p, L"else")) {
			s->has_otherwise = TRUE;
			while ((peek(p)->kind != T_LINE && peek(p)->kind != T_END) || is_colon(p)) {
				if (is_colon(p)) {
					advance(p);
					continue;
				}
				if (!parse_statement(p, &s->otherwise, TRUE))
					return FALSE;
			}
		}
		if (accept_word(p, L"end"))
			accept_word(p, L"if");
		add_stmt(p, b, s);
		return TRUE;
	}
	static const WCHAR *const ends[] = {L"elseif", L"else", L"end if", NULL};
	for (;;) {
		if (!parse_block(p, &s->blocks[s->branch_count - 1], ends))
			return FALSE;
		if (accept_word(p, L"elseif")) {
			if (s->branch_count == 64)
				return fail(p, L"too many ElseIf branches");
			s->conditions[s->branch_count] = parse_expression(p);
			if (!s->conditions[s->branch_count] || !accept_word(p, L"then"))
				return fail(p, L"Then was expected");
			s->branch_count++;
			continue;
		}
		if (accept_word(p, L"else")) {
			static const WCHAR *const last[] = {L"end if", NULL};
			s->has_otherwise = TRUE;
			if (!parse_block(p, &s->otherwise, last))
				return FALSE;
		}
		advance(p);
		advance(p);
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
}

static BOOL parse_names(parser *p, stmt *s)
{
	s->names = own(p, 64 * sizeof(WCHAR *));
	s->dims = own(p, 64 * sizeof(expr *));
	do {
		if (peek(p)->kind != T_NAME || s->name_count == 64)
			return fail(p, L"a name was expected");
		s->names[s->name_count] = advance(p)->text;
		if (accept_symbol(p, L"(")) {
			if (!accept_symbol(p, L")")) {
				s->dims[s->name_count] = parse_expression(p);
				if (!s->dims[s->name_count] || !accept_symbol(p, L")"))
					return fail(p, L"arrays of one dimension only");
			}
		}
		s->name_count++;
	} while (accept_symbol(p, L","));
	return TRUE;
}

static BOOL parse_procedure(parser *p, BOOL function)
{
	if (peek(p)->kind != T_NAME)
		return fail(p, L"a procedure name was expected");
	p->procedures = msi_grow(p->procedures, (p->procedure_count + 1) * sizeof(procedure));
	procedure *proc = &p->procedures[p->procedure_count];
	*proc = (procedure){advance(p)->text};
	proc->function = function;
	proc->params = own(p, 64 * sizeof(WCHAR *));
	if (accept_symbol(p, L"(") && !accept_symbol(p, L")")) {
		do {
			if (!accept_word(p, L"byval"))
				accept_word(p, L"byref");
			if (peek(p)->kind != T_NAME || proc->param_count == 64)
				return fail(p, L"a parameter name was expected");
			proc->params[proc->param_count++] = advance(p)->text;
			if (accept_symbol(p, L"(") && !accept_symbol(p, L")"))
				return fail(p, L"a closing parenthesis was expected");
		} while (accept_symbol(p, L","));
		if (!accept_symbol(p, L")"))
			return fail(p, L"a closing parenthesis was expected");
	}
	static const WCHAR *const function_end[] = {L"end function", NULL}, *const sub_end[] = {L"end sub", NULL};
	if (!parse_block(p, &proc->body, function ? function_end : sub_end))
		return FALSE;
	advance(p);
	advance(p);
	p->procedure_count++;
	return end_of_statement(p);
}

static BOOL parse_statement(parser *p, block *b, BOOL single_line)
{
	token *t = peek(p);
	if (t->kind != T_NAME)
		return fail(p, L"a statement was expected");
	if (accept_word(p, L"option")) {
		accept_word(p, L"explicit");
		return single_line || end_of_statement(p);
	}
	if (!single_line && (is_word(p, L"function") || is_word(p, L"sub") || ((is_word(p, L"public") || is_word(p, L"private")) &&
		(is_word_at(p, 1, L"function") || is_word_at(p, 1, L"sub"))))) {
		if (!accept_word(p, L"public"))
			accept_word(p, L"private");
		return parse_procedure(p, same_word(advance(p)->text, L"function"));
	}
	if (accept_word(p, L"dim") || accept_word(p, L"private") || accept_word(p, L"public") || is_word(p, L"redim")) {
		stmt *s = new_stmt(p, S_DIM);
		if ((s->redim = accept_word(p, L"redim")))
			s->preserve = accept_word(p, L"preserve");
		if (!parse_names(p, s))
			return FALSE;
		add_stmt(p, b, s);
		return single_line || end_of_statement(p);
	}
	if (accept_word(p, L"const")) {
		do {
			stmt *s = new_stmt(p, S_ASSIGN);
			if (peek(p)->kind != T_NAME)
				return fail(p, L"a constant name was expected");
			s->target = new_expr(p, E_NAME);
			s->target->name = advance(p)->text;
			if (!accept_symbol(p, L"=") || !(s->value = parse_expression(p)))
				return fail(p, L"a constant value was expected");
			add_stmt(p, b, s);
		} while (accept_symbol(p, L","));
		return single_line || end_of_statement(p);
	}
	if (accept_word(p, L"if"))
		return parse_if(p, b);
	if (accept_word(p, L"for")) {
		if (accept_word(p, L"each")) {
			stmt *s = new_stmt(p, S_FOR_EACH);
			if (peek(p)->kind != T_NAME)
				return fail(p, L"a loop variable was expected");
			s->name = advance(p)->text;
			if (!accept_word(p, L"in") || !(s->value = parse_expression(p)))
				return fail(p, L"In was expected");
			s->blocks = own(p, sizeof(block));
			static const WCHAR *const ends[] = {L"next", NULL};
			if (!parse_block(p, &s->blocks[0], ends))
				return FALSE;
			advance(p);
			if (peek(p)->kind == T_NAME)
				advance(p);
			add_stmt(p, b, s);
			return end_of_statement(p);
		}
		stmt *s = new_stmt(p, S_FOR);
		if (peek(p)->kind != T_NAME)
			return fail(p, L"a loop variable was expected");
		s->name = advance(p)->text;
		if (!accept_symbol(p, L"=") || !(s->value = parse_expression(p)) || !accept_word(p, L"to") || !(s->to = parse_expression(p)))
			return fail(p, L"a loop range was expected");
		if (accept_word(p, L"step") && !(s->step = parse_expression(p)))
			return FALSE;
		s->blocks = own(p, sizeof(block));
		static const WCHAR *const ends[] = {L"next", NULL};
		if (!parse_block(p, &s->blocks[0], ends))
			return FALSE;
		advance(p);
		if (peek(p)->kind == T_NAME)
			advance(p);
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	if (is_word(p, L"do") || is_word(p, L"while")) {
		stmt *s = new_stmt(p, S_DO);
		s->conditions = own(p, sizeof(expr *));
		BOOL wend = same_word(advance(p)->text, L"while");
		if (wend || is_word(p, L"while") || is_word(p, L"until")) {
			if (!wend)
				s->until = same_word(advance(p)->text, L"until");
			s->test_first = TRUE;
			if (!(s->conditions[0] = parse_expression(p)))
				return FALSE;
		}
		s->blocks = own(p, sizeof(block));
		static const WCHAR *const loop_end[] = {L"loop", NULL}, *const wend_end[] = {L"wend", NULL};
		if (!parse_block(p, &s->blocks[0], wend ? wend_end : loop_end))
			return FALSE;
		advance(p);
		if (!wend && !s->test_first && (is_word(p, L"while") || is_word(p, L"until"))) {
			s->until = same_word(advance(p)->text, L"until");
			if (!(s->conditions[0] = parse_expression(p)))
				return FALSE;
		}
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	if (accept_word(p, L"select")) {
		stmt *s = new_stmt(p, S_SELECT);
		if (!accept_word(p, L"case") || !(s->value = parse_expression(p)) || !end_of_statement(p))
			return fail(p, L"Select Case was expected");
		s->blocks = own(p, 128 * sizeof(block));
		s->case_values = own(p, 128 * sizeof(expr **));
		s->case_counts = own(p, 128 * sizeof(UINT));
		skip_lines(p);
		static const WCHAR *const ends[] = {L"case", L"end select", NULL};
		while (accept_word(p, L"case")) {
			if (accept_word(p, L"else")) {
				s->has_otherwise = TRUE;
				if (!parse_block(p, &s->otherwise, ends))
					return FALSE;
				continue;
			}
			if (s->branch_count == 128)
				return fail(p, L"too many cases");
			s->case_values[s->branch_count] = own(p, 64 * sizeof(expr *));
			do {
				expr *e = parse_expression(p);
				if (!e || s->case_counts[s->branch_count] == 64)
					return FALSE;
				s->case_values[s->branch_count][s->case_counts[s->branch_count]++] = e;
			} while (accept_symbol(p, L","));
			if (!parse_block(p, &s->blocks[s->branch_count++], ends))
				return FALSE;
		}
		if (!accept_word(p, L"end") || !accept_word(p, L"select"))
			return fail(p, L"End Select was expected");
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	if (accept_word(p, L"exit")) {
		stmt *s = new_stmt(p, S_EXIT);
		if (peek(p)->kind != T_NAME)
			return fail(p, L"Exit what was expected");
		lstrcpynW(s->exit_kind, advance(p)->text, ARRAYSIZE(s->exit_kind));
		add_stmt(p, b, s);
		return single_line || end_of_statement(p);
	}
	if (accept_word(p, L"on")) {
		stmt *s = new_stmt(p, S_ON_ERROR);
		if (!accept_word(p, L"error"))
			return fail(p, L"On Error was expected");
		if (accept_word(p, L"resume") && accept_word(p, L"next"))
			s->resume_next = TRUE;
		else if (!(accept_word(p, L"goto") && peek(p)->kind == T_NUMBER && !advance(p)->number))
			return fail(p, L"On Error Resume Next or GoTo 0 was expected");
		add_stmt(p, b, s);
		return single_line || end_of_statement(p);
	}
	BOOL set = accept_word(p, L"set");
	BOOL call = !set && accept_word(p, L"call");
	if (peek(p)->kind != T_NAME)
		return fail(p, L"a name was expected");
	expr *target = new_expr(p, E_NAME);
	target->name = advance(p)->text;
	if (!(target = parse_postfix(p, target)))
		return FALSE;
	if (!call && accept_symbol(p, L"=")) {
		stmt *s = new_stmt(p, S_ASSIGN);
		s->target = target;
		s->value = parse_expression(p);
		s->set = set;
		if (!s->value)
			return FALSE;
		add_stmt(p, b, s);
		return single_line || end_of_statement(p);
	}
	if (set)
		return fail(p, L"Set needs an assignment");
	stmt *s = new_stmt(p, S_CALL);
	if (!at_line_end(p)) {
		// A call without parentheses: its arguments follow, separated by commas.
		expr *callee = target;
		if (target->kind == E_CALL && target->arg_count == 1) {
			// "Name (a), b": the parentheses belonged to the first argument.
			callee = target->target;
			expr *first = target->args[0];
			target = new_expr(p, E_CALL);
			target->target = callee;
			add_arg(p, target, first);
			if (!accept_symbol(p, L","))
				return fail(p, L"a comma was expected");
		} else {
			target = new_expr(p, E_CALL);
			target->target = callee;
		}
		do {
			if (is_symbol(p, L",")) {
				add_arg(p, target, NULL);
				continue;
			}
			expr *arg = parse_expression(p);
			if (!arg)
				return FALSE;
			add_arg(p, target, arg);
		} while (accept_symbol(p, L","));
	}
	s->target = target;
	add_stmt(p, b, s);
	return single_line || end_of_statement(p);
}

static BOOL parse_script(parser *p)
{
	for (;;) {
		skip_lines(p);
		if (peek(p)->kind == T_END)
			return TRUE;
		if (!parse_statement(p, &p->main, FALSE))
			return FALSE;
	}
}

/* The interpreter. A runtime error stops the script unless the running procedure said On Error Resume Next, which records it in
 * Err and goes on with the next statement. */

typedef struct {
	WCHAR *name;
	value v;
} variable;

typedef struct {
	variable *vars;
	UINT count, room;
	procedure *proc;
	value result;
	BOOL resume_next;
} frame;

typedef enum { FLOW_NEXT, FLOW_EXIT_PROC, FLOW_EXIT_FOR, FLOW_EXIT_DO } flow;

typedef struct {
	msi_package *package;
	MSIHANDLE session;
	parser *program;
	frame global, *current;
	BOOL raised;
	LONG error_number;
	WCHAR error_text[200];
	UINT error_line, depth;
} interp;

static void raise(interp *in, LONG number, const WCHAR *text)
{
	if (in->raised)
		return;
	in->raised = TRUE;
	in->error_number = number;
	lstrcpynW(in->error_text, text, ARRAYSIZE(in->error_text));
}

static variable *find_variable(frame *f, const WCHAR *name)
{
	for (UINT i = 0; i < f->count; i++)
		if (same_word(f->vars[i].name, name))
			return &f->vars[i];
	return NULL;
}

static variable *declare(frame *f, const WCHAR *name)
{
	variable *found = find_variable(f, name);
	if (found)
		return found;
	if (f->count == f->room) {
		f->room = f->room ? f->room * 2 : 16;
		f->vars = msi_grow(f->vars, f->room * sizeof(variable));
	}
	f->vars[f->count] = (variable){msi_copy(name)};
	return &f->vars[f->count++];
}

static void frame_clear(frame *f)
{
	for (UINT i = 0; i < f->count; i++) {
		msi_free(f->vars[i].name);
		value_clear(&f->vars[i].v);
	}
	msi_free(f->vars);
	value_clear(&f->result);
}

static variable *lookup(interp *in, const WCHAR *name)
{
	variable *v = find_variable(in->current, name);
	return v ? v : in->current != &in->global ? find_variable(&in->global, name) : NULL;
}

static procedure *find_procedure(interp *in, const WCHAR *name)
{
	for (UINT i = 0; i < in->program->procedure_count; i++)
		if (same_word(in->program->procedures[i].name, name))
			return &in->program->procedures[i];
	return NULL;
}

static double to_number(interp *in, const value *v)
{
	double number = 0;
	switch (v->kind) {
	case V_EMPTY:
		return 0;
	case V_BOOL:
	case V_NUMBER:
		return v->number;
	case V_TEXT:
		if (!parse_number(v->text, &number))
			raise(in, 13, L"Type mismatch");
		return number;
	case V_OBJECT:
		if (v->object->kind == O_ERR)
			return in->error_number;
		/* fall through */
	default:
		raise(in, v->kind == V_NULL ? 94 : 13, v->kind == V_NULL ? L"Invalid use of Null" : L"Type mismatch");
		return 0;
	}
}

static WCHAR *to_text(interp *in, const value *v)
{
	switch (v->kind) {
	case V_EMPTY:
		return msi_copy(L"");
	case V_BOOL:
		return msi_copy(v->number ? L"True" : L"False");
	case V_NUMBER:
		return number_text(v->number);
	case V_TEXT:
		return msi_copy(v->text);
	case V_OBJECT:
		if (v->object->kind == O_ERR)
			return number_text(in->error_number);
		/* fall through */
	default:
		raise(in, v->kind == V_NULL ? 94 : 13, v->kind == V_NULL ? L"Invalid use of Null" : L"Type mismatch");
		return msi_copy(L"");
	}
}

static BOOL to_bool(interp *in, const value *v)
{
	if (v->kind == V_TEXT && (same_word(v->text, L"true") || same_word(v->text, L"false")))
		return same_word(v->text, L"true");
	return to_number(in, v) != 0;
}

// Rounds half to even, as conversions to whole numbers do.
static double round_even(double x)
{
	double low = whole(x), rest = x - low;
	if (rest > 0.5 || (rest == 0.5 && whole(low / 2) * 2 != low))
		return low + 1;
	return low;
}

static LONG to_long(interp *in, const value *v)
{
	double n = round_even(to_number(in, v));
	if (n > 2147483647.0 || n < -2147483648.0) {
		raise(in, 6, L"Overflow");
		return 0;
	}
	return (LONG)n;
}

static int compare_text(const WCHAR *a, const WCHAR *b, BOOL ignore_case)
{
	for (;; a++, b++) {
		WCHAR x = ignore_case ? lower_char(*a) : *a, y = ignore_case ? lower_char(*b) : *b;
		if (x != y)
			return x < y ? -1 : 1;
		if (!x)
			return 0;
	}
}

static value evaluate(interp *in, expr *e);
static value call_procedure(interp *in, procedure *proc, expr **args, UINT count);

/* The installer's objects. */

static UINT text_out(MSIHANDLE handle, UINT (WINAPI *get)(MSIHANDLE, LPCWSTR, LPWSTR, LPDWORD), const WCHAR *name, WCHAR **out)
{
	DWORD size = 0;
	WCHAR small[1];
	UINT status = get(handle, name, small, &size);
	if (status == ERROR_MORE_DATA) {
		*out = msi_alloc((++size) * sizeof(WCHAR));
		status = get(handle, name, *out, &size);
	} else
		*out = msi_copy(L"");
	return status;
}

static UINT WINAPI target_path(MSIHANDLE h, LPCWSTR n, LPWSTR o, LPDWORD s) { return MsiGetTargetPathW(h, n, o, s); }
static UINT WINAPI source_path(MSIHANDLE h, LPCWSTR n, LPWSTR o, LPDWORD s) { return MsiGetSourcePathW(h, n, o, s); }
static UINT WINAPI get_property(MSIHANDLE h, LPCWSTR n, LPWSTR o, LPDWORD s) { return MsiGetPropertyW(h, n, o, s); }

static WCHAR *arg_text(interp *in, value *args, UINT count, UINT index)
{
	return index < count ? to_text(in, &args[index]) : msi_copy(L"");
}

static value record_member(interp *in, script_object *record, const WCHAR *name, value *args, UINT count, const value *put)
{
	MSIHANDLE h = record->handle;
	UINT field = count ? (UINT)to_long(in, &args[0]) : 0;
	if (same_word(name, L"stringdata")) {
		if (put) {
			WCHAR *text = to_text(in, put);
			MsiRecordSetStringW(h, field, text);
			msi_free(text);
			return (value){V_EMPTY};
		}
		DWORD size = 0;
		WCHAR small[1];
		if (MsiRecordGetStringW(h, field, small, &size) == ERROR_MORE_DATA) {
			WCHAR *text = msi_alloc((++size) * sizeof(WCHAR));
			MsiRecordGetStringW(h, field, text, &size);
			return text_taken(text);
		}
		return text_value(L"");
	}
	if (same_word(name, L"integerdata")) {
		if (put) {
			MsiRecordSetInteger(h, field, to_long(in, put));
			return (value){V_EMPTY};
		}
		return number_value(MsiRecordGetInteger(h, field));
	}
	if (same_word(name, L"fieldcount"))
		return number_value(MsiRecordGetFieldCount(h));
	if (same_word(name, L"isnull"))
		return bool_value(MsiRecordIsNull(h, field));
	if (same_word(name, L"datasize"))
		return number_value(MsiRecordDataSize(h, field));
	if (same_word(name, L"cleardata")) {
		MsiRecordClearData(h);
		return (value){V_EMPTY};
	}
	raise(in, 438, L"Object doesn't support this property or method");
	return (value){V_EMPTY};
}

static value session_member(interp *in, const WCHAR *name, value *args, UINT count, const value *put)
{
	MSIHANDLE h = in->session;
	WCHAR *first = arg_text(in, args, count, 0), *text = NULL;
	value result = {V_EMPTY};
	if (same_word(name, L"property")) {
		if (put) {
			WCHAR *data = to_text(in, put);
			MsiSetPropertyW(h, first, data);
			msi_free(data);
		} else {
			text_out(h, get_property, first, &text);
			result = text_taken(text);
		}
	} else if (same_word(name, L"installer")) {
		result = object_value(O_INSTALLER, 0);
	} else if (same_word(name, L"message")) {
		if (count < 2 || args[1].kind != V_OBJECT || args[1].object->kind != O_RECORD)
			raise(in, 13, L"Type mismatch");
		else
			result = number_value(MsiProcessMessage(h, (INSTALLMESSAGE)to_long(in, &args[0]), args[1].object->handle));
	} else if (same_word(name, L"evaluatecondition")) {
		result = number_value(MsiEvaluateConditionW(h, first));
	} else if (same_word(name, L"mode")) {
		MSIRUNMODE mode = (MSIRUNMODE)(count ? to_long(in, &args[0]) : 0);
		if (put)
			MsiSetMode(h, mode, to_bool(in, put));
		else
			result = bool_value(MsiGetMode(h, mode));
	} else if (same_word(name, L"targetpath") || same_word(name, L"sourcepath")) {
		BOOL target = same_word(name, L"targetpath");
		if (put && target) {
			WCHAR *path = to_text(in, put);
			if (MsiSetTargetPathW(h, first, path))
				raise(in, 1000, L"The folder cannot be set");
			msi_free(path);
		} else if (text_out(h, target ? target_path : source_path, first, &text)) {
			msi_free(text);
			raise(in, 1000, L"The folder is not known");
		} else
			result = text_taken(text);
	} else if (same_word(name, L"doaction")) {
		UINT status = MsiDoActionW(h, first);
		result = number_value(status == ERROR_SUCCESS ? 1 : status == ERROR_FUNCTION_NOT_CALLED ? 0 : status == ERROR_INSTALL_USEREXIT ? 2 :
			status == ERROR_INSTALL_SUSPEND ? 4 : status == ERROR_NO_MORE_ITEMS ? 5 : 3);
	} else if (same_word(name, L"language")) {
		result = number_value(MsiGetLanguage(h));
	} else if (same_word(name, L"featurecurrentstate") || same_word(name, L"featurerequeststate") || same_word(name, L"componentcurrentstate") ||
		same_word(name, L"componentrequeststate")) {
		BOOL feature = starts_with_word(name, L"feature", 7), request = same_word(name, L"featurerequeststate") || same_word(name, L"componentrequeststate");
		if (put && request) {
			UINT status = feature ? MsiSetFeatureStateW(h, first, (INSTALLSTATE)to_long(in, put)) : MsiSetComponentStateW(h, first, (INSTALLSTATE)to_long(in, put));
			if (status)
				raise(in, 1000, L"The state cannot be set");
		} else {
			INSTALLSTATE installed = INSTALLSTATE_UNKNOWN, action = INSTALLSTATE_UNKNOWN;
			UINT status = feature ? MsiGetFeatureStateW(h, first, &installed, &action) : MsiGetComponentStateW(h, first, &installed, &action);
			if (status)
				raise(in, 1000, L"The feature or component is not known");
			result = number_value(request ? action : installed);
		}
	} else
		raise(in, 438, L"Object doesn't support this property or method");
	msi_free(first);
	return result;
}

static value object_member(interp *in, const value *object, const WCHAR *name, value *args, UINT count, const value *put)
{
	if (object->kind != V_OBJECT) {
		raise(in, 424, L"Object required");
		return (value){V_EMPTY};
	}
	switch (object->object->kind) {
	case O_SESSION:
		return session_member(in, name, args, count, put);
	case O_RECORD:
		return record_member(in, object->object, name, args, count, put);
	case O_INSTALLER:
		if (same_word(name, L"createrecord") && count == 1)
			return object_value(O_RECORD, MsiCreateRecord((UINT)to_long(in, &args[0])));
		break;
	case O_ERR:
		if (same_word(name, L"number")) {
			if (put)
				in->error_number = to_long(in, put);
			return number_value(in->error_number);
		}
		if (same_word(name, L"description"))
			return text_value(in->error_number ? in->error_text : L"");
		if (same_word(name, L"source"))
			return text_value(L"Microsoft VBScript runtime error");
		if (same_word(name, L"clear")) {
			in->error_number = 0;
			in->error_text[0] = 0;
			return (value){V_EMPTY};
		}
		if (same_word(name, L"raise") && count) {
			WCHAR *text = count > 2 ? to_text(in, &args[2]) : msi_copy(L"Unknown runtime error");
			raise(in, to_long(in, &args[0]), text);
			msi_free(text);
			return (value){V_EMPTY};
		}
		break;
	}
	raise(in, 438, L"Object doesn't support this property or method");
	return (value){V_EMPTY};
}

/* The functions of the language a custom action is likely to call. */

static WCHAR *substring(const WCHAR *text, LONG start, LONG length)
{
	LONG size = lstrlenW(text);
	if (start < 0)
		start = 0;
	if (start > size)
		start = size;
	if (length < 0 || start + length > size)
		length = size - start;
	return msi_copy_n(text + start, length);
}

static LONG find_text(const WCHAR *text, const WCHAR *what, LONG from, BOOL ignore_case)
{
	LONG size = lstrlenW(text), wanted = lstrlenW(what);
	for (LONG i = from; i + wanted <= size; i++) {
		LONG j = 0;
		while (j < wanted && (ignore_case ? lower_char(text[i + j]) == lower_char(what[j]) : text[i + j] == what[j]))
			j++;
		if (j == wanted)
			return i;
	}
	return -1;
}

static value make_array(UINT count)
{
	value v = {V_ARRAY};
	v.array = msi_alloc(sizeof(script_array));
	v.array->count = count;
	v.array->items = msi_alloc((count + 1) * sizeof(value));
	return v;
}

static BOOL builtin(interp *in, const WCHAR *name, value *args, UINT count, value *out)
{
	static const struct { const WCHAR *name; const WCHAR *text; double number; } constants[] = {
		{L"vbcrlf", L"\r\n"}, {L"vbnewline", L"\r\n"}, {L"vbcr", L"\r"}, {L"vblf", L"\n"}, {L"vbtab", L"\t"}, {L"vbnullstring", L""},
		{L"vbnullchar", L"\0"}, {L"vbbinarycompare", NULL, 0}, {L"vbtextcompare", NULL, 1}, {L"vbtrue", NULL, -1}, {L"vbfalse", NULL, 0},
		{L"vbokonly", NULL, 0}, {L"vbokcancel", NULL, 1}, {L"vbyesno", NULL, 4}, {L"vbok", NULL, 1}, {L"vbcancel", NULL, 2}, {L"vbyes", NULL, 6},
		{L"vbno", NULL, 7}, {L"vbinformation", NULL, 64}, {L"vbexclamation", NULL, 48}, {L"vbcritical", NULL, 16},
	};
	for (UINT i = 0; i < ARRAYSIZE(constants); i++)
		if (same_word(name, constants[i].name)) {
			*out = constants[i].text ? text_value(constants[i].text) : number_value(constants[i].number);
			return !count;
		}
	// The functions that look at what a value is take it as it comes; the others read their first argument as text.
	static const WCHAR *as_is[] = {L"typename", L"isempty", L"isnull", L"isobject", L"isarray", L"isnumeric", L"ubound", L"lbound", L"join",
		L"array", L"len"};
	BOOL text_first = count != 0;
	for (UINT i = 0; text_first && i < ARRAYSIZE(as_is); i++)
		if (same_word(name, as_is[i]))
			text_first = args[0].kind <= V_TEXT && args[0].kind != V_NULL;
	WCHAR *a = text_first ? to_text(in, &args[0]) : count ? msi_copy(L"") : NULL;
	BOOL found = TRUE;
	#define NAMED(word) same_word(name, word)
	if (NAMED(L"len"))
		*out = number_value(count && args[0].kind != V_NULL ? lstrlenW(a) : 0);
	else if (NAMED(L"left") && count == 2)
		*out = text_taken(substring(a, 0, to_long(in, &args[1])));
	else if (NAMED(L"right") && count == 2) {
		LONG length = to_long(in, &args[1]), size = lstrlenW(a);
		*out = text_taken(substring(a, length > size ? 0 : size - length, length));
	} else if (NAMED(L"mid") && count >= 2) {
		LONG start = to_long(in, &args[1]);
		if (start < 1)
			raise(in, 5, L"Invalid procedure call or argument");
		*out = text_taken(substring(a, start - 1, count > 2 ? to_long(in, &args[2]) : -1));
	} else if ((NAMED(L"instr") || NAMED(L"instrrev")) && count >= 2) {
		BOOL reverse = NAMED(L"instrrev");
		UINT first = !reverse && count >= 3 && args[0].kind != V_TEXT ? 1 : 0;
		LONG from = first ? to_long(in, &args[0]) - 1 : 0;
		WCHAR *text = to_text(in, &args[first]), *what = to_text(in, &args[first + 1]);
		BOOL ignore_case = count > first + 2 + (reverse ? 1 : 0) && to_long(in, &args[count - 1]) == 1;
		LONG at = -1;
		if (reverse) {
			for (LONG i = 0; (i = find_text(text, what, i, ignore_case)) >= 0; i++)
				at = i;
		} else
			at = find_text(text, what, from < 0 ? 0 : from, ignore_case);
		*out = number_value(at + 1);
		msi_free(text);
		msi_free(what);
	} else if (NAMED(L"ucase") || NAMED(L"lcase")) {
		WCHAR *text = msi_copy(a ? a : L"");
		if (NAMED(L"ucase"))
			CharUpperW(text);
		else
			CharLowerW(text);
		*out = text_taken(text);
	} else if (NAMED(L"trim") || NAMED(L"ltrim") || NAMED(L"rtrim")) {
		const WCHAR *start = a ? a : L"";
		LONG length = lstrlenW(start);
		if (!NAMED(L"rtrim"))
			while (*start == L' ')
				start++, length--;
		if (!NAMED(L"ltrim"))
			while (length > 0 && start[length - 1] == L' ')
				length--;
		*out = text_taken(msi_copy_n(start, length));
	} else if (NAMED(L"replace") && count >= 3) {
		WCHAR *what = to_text(in, &args[1]), *with = to_text(in, &args[2]);
		BOOL ignore_case = count >= 6 && to_long(in, &args[5]) == 1;
		LONG what_length = lstrlenW(what), with_length = lstrlenW(with), size = lstrlenW(a), room = size + 1;
		WCHAR *result = msi_alloc(room * sizeof(WCHAR));
		LONG length = 0, from = 0;
		if (what_length)
			for (LONG at; (at = find_text(a, what, from, ignore_case)) >= 0; from = at + what_length) {
				room += with_length + at - from;
				result = msi_grow(result, room * sizeof(WCHAR));
				CopyMemory(result + length, a + from, (at - from) * sizeof(WCHAR));
				length += at - from;
				CopyMemory(result + length, with, with_length * sizeof(WCHAR));
				length += with_length;
			}
		room += size - from + 1;
		result = msi_grow(result, room * sizeof(WCHAR));
		CopyMemory(result + length, a + from, (size - from) * sizeof(WCHAR));
		result[length + size - from] = 0;
		*out = text_taken(result);
		msi_free(what);
		msi_free(with);
	} else if ((NAMED(L"space") || NAMED(L"string")) && count >= 1) {
		LONG size = to_long(in, &args[0]);
		WCHAR fill = L' ';
		if (NAMED(L"string") && count == 2) {
			WCHAR *with = to_text(in, &args[1]);
			fill = args[1].kind == V_TEXT ? with[0] : (WCHAR)to_long(in, &args[1]);
			msi_free(with);
		}
		if (size < 0)
			size = 0;
		WCHAR *text = msi_alloc((size + 1) * sizeof(WCHAR));
		for (LONG i = 0; i < size; i++)
			text[i] = fill;
		*out = text_taken(text);
	} else if (NAMED(L"strcomp") && count >= 2) {
		WCHAR *b = to_text(in, &args[1]);
		*out = number_value(compare_text(a, b, count > 2 && to_long(in, &args[2]) == 1));
		msi_free(b);
	} else if (NAMED(L"strreverse")) {
		LONG size = lstrlenW(a);
		WCHAR *text = msi_alloc((size + 1) * sizeof(WCHAR));
		for (LONG i = 0; i < size; i++)
			text[i] = a[size - 1 - i];
		*out = text_taken(text);
	} else if (NAMED(L"cstr") && count == 1)
		*out = text_value(a);
	else if ((NAMED(L"cint") || NAMED(L"clng")) && count == 1)
		*out = number_value(to_long(in, &args[0]));
	else if ((NAMED(L"cdbl") || NAMED(L"csng")) && count == 1)
		*out = number_value(to_number(in, &args[0]));
	else if (NAMED(L"cbool") && count == 1)
		*out = bool_value(to_bool(in, &args[0]));
	else if ((NAMED(L"int") || NAMED(L"fix")) && count == 1) {
		double n = to_number(in, &args[0]), w = whole(n);
		*out = number_value(NAMED(L"fix") && n < 0 && w != n ? w + 1 : w);
	} else if (NAMED(L"abs") && count == 1) {
		double n = to_number(in, &args[0]);
		*out = number_value(n < 0 ? -n : n);
	} else if (NAMED(L"sgn") && count == 1) {
		double n = to_number(in, &args[0]);
		*out = number_value(n > 0 ? 1 : n < 0 ? -1 : 0);
	} else if (NAMED(L"round") && count >= 1) {
		double n = to_number(in, &args[0]), scale = 1;
		for (LONG places = count > 1 ? to_long(in, &args[1]) : 0; places > 0; places--)
			scale *= 10;
		*out = number_value(round_even(n * scale) / scale);
	} else if ((NAMED(L"chr") || NAMED(L"chrw")) && count == 1) {
		WCHAR text[2] = {(WCHAR)to_long(in, &args[0])};
		if (NAMED(L"chr") && text[0] > 127 && text[0] < 256) {
			char narrow = (char)text[0];
			MultiByteToWideChar(CP_ACP, 0, &narrow, 1, text, 1);
		}
		*out = text_value(text);
	} else if ((NAMED(L"asc") || NAMED(L"ascw")) && count == 1) {
		if (!a[0])
			raise(in, 5, L"Invalid procedure call or argument");
		*out = number_value(a[0]);
	} else if (NAMED(L"hex") && count == 1) {
		WCHAR text[16];
		wsprintfW(text, L"%X", (UINT)to_long(in, &args[0]));
		*out = text_value(text);
	} else if (NAMED(L"isempty") && count == 1)
		*out = bool_value(args[0].kind == V_EMPTY);
	else if (NAMED(L"isnull") && count == 1)
		*out = bool_value(args[0].kind == V_NULL);
	else if (NAMED(L"isobject") && count == 1)
		*out = bool_value(args[0].kind == V_OBJECT || args[0].kind == V_NOTHING);
	else if (NAMED(L"isarray") && count == 1)
		*out = bool_value(args[0].kind == V_ARRAY);
	else if (NAMED(L"isnumeric") && count == 1) {
		double n;
		*out = bool_value(args[0].kind == V_NUMBER || args[0].kind == V_BOOL || args[0].kind == V_EMPTY || (args[0].kind == V_TEXT && parse_number(a, &n)));
	} else if (NAMED(L"split") && count >= 1) {
		WCHAR *delimiter = count > 1 ? to_text(in, &args[1]) : msi_copy(L" ");
		LONG delimiter_length = lstrlenW(delimiter), pieces = 1;
		for (LONG at = 0; delimiter_length && (at = find_text(a, delimiter, at, FALSE)) >= 0; at += delimiter_length)
			pieces++;
		*out = make_array(a[0] ? pieces : 0);
		LONG from = 0;
		for (UINT i = 0; a[0] && i < (UINT)pieces; i++) {
			LONG at = delimiter_length ? find_text(a, delimiter, from, FALSE) : -1;
			if (at < 0)
				at = lstrlenW(a);
			out->array->items[i] = text_taken(msi_copy_n(a + from, at - from));
			from = at + delimiter_length;
		}
		msi_free(delimiter);
	} else if (NAMED(L"join") && count >= 1 && args[0].kind == V_ARRAY) {
		WCHAR *delimiter = count > 1 ? to_text(in, &args[1]) : msi_copy(L" ");
		SIZE_T room = 1;
		WCHAR **parts = msi_alloc((args[0].array->count + 1) * sizeof(WCHAR *));
		for (UINT i = 0; i < args[0].array->count; i++)
			room += lstrlenW(parts[i] = to_text(in, &args[0].array->items[i])) + lstrlenW(delimiter);
		WCHAR *text = msi_alloc(room * sizeof(WCHAR));
		for (UINT i = 0; i < args[0].array->count; i++) {
			if (i)
				lstrcatW(text, delimiter);
			lstrcatW(text, parts[i]);
			msi_free(parts[i]);
		}
		msi_free(parts);
		msi_free(delimiter);
		*out = text_taken(text);
	} else if ((NAMED(L"ubound") || NAMED(L"lbound")) && count >= 1) {
		if (args[0].kind != V_ARRAY)
			raise(in, 13, L"Type mismatch");
		else
			*out = number_value(NAMED(L"lbound") ? 0 : (double)args[0].array->count - 1);
	} else if (NAMED(L"array")) {
		*out = make_array(count);
		for (UINT i = 0; i < count; i++)
			out->array->items[i] = value_copy(&args[i]);
	} else if (NAMED(L"typename") && count == 1) {
		static const WCHAR *names[] = {L"Empty", L"Null", L"Boolean", L"Double", L"String", L"Object", L"Nothing", L"Variant()"};
		double n = args[0].number;
		BOOL counted = args[0].kind == V_NUMBER && n == whole(n);
		*out = text_value(counted && n < 32768 && n >= -32768 ? L"Integer" : counted && n < 2147483648.0 && n >= -2147483648.0 ? L"Long" :
			names[args[0].kind]);
	} else if (NAMED(L"msgbox")) {
		// No one is there to answer: the text goes to the log, and the answer is OK.
		WCHAR *line = msi_alloc((lstrlenW(a ? a : L"") + 16) * sizeof(WCHAR));
		wsprintfW(line, L"MsgBox: %s", a ? a : L"");
		package_log(L"%s", line);
		msi_free(line);
		*out = number_value(1);
	} else if (NAMED(L"createobject") || NAMED(L"getobject")) {
		raise(in, 429, L"ActiveX component can't create object");
	} else
		found = FALSE;
	#undef NAMED
	msi_free(a);
	return found;
}

/* Expressions. */

static value call_with(interp *in, procedure *proc, value *args, UINT count);

static value index_array(interp *in, const value *array, value *args, UINT count)
{
	if (count != 1) {
		raise(in, 9, L"Subscript out of range");
		return (value){V_EMPTY};
	}
	LONG index = to_long(in, &args[0]);
	if (in->raised)
		return (value){V_EMPTY};
	if (index < 0 || (UINT)index >= array->array->count) {
		raise(in, 9, L"Subscript out of range");
		return (value){V_EMPTY};
	}
	return value_copy(&array->array->items[index]);
}

// A name as a value or a call: a variable (an array takes an index), a procedure of the script, an object of the installer or a
// function of the language.
static value resolve(interp *in, const WCHAR *name, value *args, UINT count)
{
	if (in->current->proc && in->current->proc->function && same_word(name, in->current->proc->name) && !count)
		return value_copy(&in->current->result);
	variable *v = lookup(in, name);
	if (v) {
		if (v->v.kind == V_ARRAY && count)
			return index_array(in, &v->v, args, count);
		if (count)
			raise(in, 13, L"Type mismatch");
		return value_copy(&v->v);
	}
	procedure *proc = find_procedure(in, name);
	if (proc)
		return call_with(in, proc, args, count);
	if (same_word(name, L"session") && !count)
		return object_value(O_SESSION, in->session);
	if (same_word(name, L"err") && !count)
		return object_value(O_ERR, 0);
	value out = {V_EMPTY};
	if (builtin(in, name, args, count, &out))
		return out;
	if (count) {
		WCHAR text[160];
		wsprintfW(text, L"Type mismatch: '%.100s'", name);
		raise(in, 13, text);
	}
	return out;
}

static BOOL compare_values(interp *in, const value *a, const value *b, int *order)
{
	if (a->kind == V_NULL || b->kind == V_NULL)
		return FALSE;
	double x, y;
	BOOL a_text = a->kind == V_TEXT, b_text = b->kind == V_TEXT;
	if (a_text && b_text) {
		*order = compare_text(a->text, b->text, FALSE);
		return TRUE;
	}
	// A string beside a number compares as a number when it is one, and as text otherwise.
	if ((a_text && !parse_number(a->text, &x)) || (b_text && !parse_number(b->text, &y))) {
		WCHAR *left = to_text(in, a), *right = to_text(in, b);
		*order = compare_text(left, right, FALSE);
		msi_free(left);
		msi_free(right);
		return TRUE;
	}
	x = to_number(in, a);
	y = to_number(in, b);
	*order = x < y ? -1 : x > y ? 1 : 0;
	return TRUE;
}

static value binary_value(interp *in, const WCHAR *op, value *a, value *b)
{
	if (!lstrcmpW(op, L"&")) {
		if (a->kind == V_NULL && b->kind == V_NULL)
			return (value){V_NULL};
		WCHAR *left = a->kind == V_NULL ? msi_copy(L"") : to_text(in, a), *right = b->kind == V_NULL ? msi_copy(L"") : to_text(in, b);
		WCHAR *text = msi_alloc((lstrlenW(left) + lstrlenW(right) + 1) * sizeof(WCHAR));
		lstrcpyW(text, left);
		lstrcatW(text, right);
		msi_free(left);
		msi_free(right);
		return text_taken(text);
	}
	if (!lstrcmpW(op, L"is"))
		return bool_value((a->kind == V_NOTHING && b->kind == V_NOTHING) ||
			(a->kind == V_OBJECT && b->kind == V_OBJECT && a->object->kind == b->object->kind && a->object->handle == b->object->handle) ||
			(a->kind == V_OBJECT && b->kind == V_OBJECT && a->object == b->object));
	static const WCHAR *comparisons[] = {L"=", L"<>", L"<", L">", L"<=", L">="};
	for (UINT i = 0; i < ARRAYSIZE(comparisons); i++)
		if (!lstrcmpW(op, comparisons[i])) {
			int order;
			if (!compare_values(in, a, b, &order))
				return (value){V_NULL};
			BOOL yes = i == 0 ? !order : i == 1 ? order != 0 : i == 2 ? order < 0 : i == 3 ? order > 0 : i == 4 ? order <= 0 : order >= 0;
			return bool_value(yes);
		}
	if (a->kind == V_NULL || b->kind == V_NULL)
		return (value){V_NULL};
	if (!lstrcmpW(op, L"+") && a->kind == V_TEXT && b->kind == V_TEXT)
		return binary_value(in, L"&", a, b);
	static const WCHAR *logic[] = {L"and", L"or", L"xor", L"eqv", L"imp"};
	for (UINT i = 0; i < ARRAYSIZE(logic); i++)
		if (!lstrcmpW(op, logic[i])) {
			LONG x = a->kind == V_TEXT ? (to_bool(in, a) ? -1 : 0) : to_long(in, a), y = b->kind == V_TEXT ? (to_bool(in, b) ? -1 : 0) : to_long(in, b);
			LONG r = i == 0 ? x & y : i == 1 ? x | y : i == 2 ? x ^ y : i == 3 ? ~(x ^ y) : (~x | y);
			BOOL boolean = (a->kind == V_BOOL || a->kind == V_TEXT) && (b->kind == V_BOOL || b->kind == V_TEXT);
			return boolean ? bool_value(r != 0) : number_value(r);
		}
	double x = to_number(in, a), y = to_number(in, b);
	if (in->raised)
		return (value){V_EMPTY};
	switch (op[0]) {
	case L'+':
		return number_value(x + y);
	case L'-':
		return number_value(x - y);
	case L'*':
		return number_value(x * y);
	case L'/':
		if (!y) {
			raise(in, 11, L"Division by zero");
			return (value){V_EMPTY};
		}
		return number_value(x / y);
	case L'\\':
	case L'm': {
		LONG left = to_long(in, a), right = to_long(in, b);
		if (!right) {
			raise(in, 11, L"Division by zero");
			return (value){V_EMPTY};
		}
		return number_value(op[0] == L'm' ? left % right : left / right);
	}
	case L'^': {
		if (y != whole(y) || y < -1024 || y > 1024) {
			raise(in, 5, L"Invalid procedure call or argument");
			return (value){V_EMPTY};
		}
		double r = 1;
		for (LONG i = 0; i < (y < 0 ? -y : y); i++)
			r *= x;
		return number_value(y < 0 ? 1 / r : r);
	}
	}
	raise(in, 5, L"Invalid procedure call or argument");
	return (value){V_EMPTY};
}

static value *evaluate_args(interp *in, expr *call)
{
	value *args = msi_alloc((call->arg_count + 1) * sizeof(value));
	for (UINT i = 0; i < call->arg_count && !in->raised; i++)
		args[i] = call->args[i] ? evaluate(in, call->args[i]) : (value){V_EMPTY};
	return args;
}

static void free_args(value *args, UINT count)
{
	for (UINT i = 0; i < count; i++)
		value_clear(&args[i]);
	msi_free(args);
}

static value evaluate(interp *in, expr *e)
{
	value result = {V_EMPTY};
	if (in->raised)
		return result;
	switch (e->kind) {
	case E_CONSTANT:
		return value_copy(&e->constant);
	case E_NAME:
		return resolve(in, e->name, NULL, 0);
	case E_MEMBER: {
		value object = evaluate(in, e->target);
		if (!in->raised)
			result = object_member(in, &object, e->name, NULL, 0, NULL);
		value_clear(&object);
		return result;
	}
	case E_CALL: {
		value *args = evaluate_args(in, e);
		if (!in->raised) {
			if (e->target->kind == E_NAME)
				result = resolve(in, e->target->name, args, e->arg_count);
			else if (e->target->kind == E_MEMBER) {
				value object = evaluate(in, e->target->target);
				if (!in->raised)
					result = object_member(in, &object, e->target->name, args, e->arg_count, NULL);
				value_clear(&object);
			} else {
				value callee = evaluate(in, e->target);
				if (callee.kind == V_ARRAY)
					result = index_array(in, &callee, args, e->arg_count);
				else
					raise(in, 13, L"Type mismatch");
				value_clear(&callee);
			}
		}
		free_args(args, e->arg_count);
		return result;
	}
	case E_NOT: {
		value v = evaluate(in, e->left);
		if (v.kind == V_NULL)
			return v;
		result = v.kind == V_BOOL || v.kind == V_TEXT ? bool_value(!to_bool(in, &v)) : number_value(~to_long(in, &v));
		value_clear(&v);
		return result;
	}
	case E_NEGATE: {
		value v = evaluate(in, e->left);
		if (v.kind != V_NULL)
			result = number_value(-to_number(in, &v));
		else
			result = v, v = (value){V_EMPTY};
		value_clear(&v);
		return result;
	}
	case E_BINARY: {
		if (!lstrcmpW(e->op, L"()"))
			return evaluate(in, e->left);
		value a = evaluate(in, e->left), b = evaluate(in, e->right);
		if (!in->raised)
			result = binary_value(in, e->op, &a, &b);
		value_clear(&a);
		value_clear(&b);
		return result;
	}
	}
	return result;
}

/* Statements. */

static flow run_block(interp *in, block *b);

static void store(variable *v, value *taken)
{
	value_clear(&v->v);
	v->v = *taken;
	*taken = (value){V_EMPTY};
}

static void assign(interp *in, expr *target, value *v)
{
	if (target->kind == E_NAME) {
		if (in->current->proc && in->current->proc->function && same_word(target->name, in->current->proc->name)) {
			value_clear(&in->current->result);
			in->current->result = *v;
			*v = (value){V_EMPTY};
			return;
		}
		variable *found = lookup(in, target->name);
		store(found ? found : declare(in->current, target->name), v);
		return;
	}
	if (target->kind == E_MEMBER || (target->kind == E_CALL && target->target->kind == E_MEMBER)) {
		expr *member = target->kind == E_MEMBER ? target : target->target;
		value object = evaluate(in, member->target);
		value *args = target->kind == E_CALL ? evaluate_args(in, target) : NULL;
		UINT count = target->kind == E_CALL ? target->arg_count : 0;
		if (!in->raised) {
			value ignored = object_member(in, &object, member->name, args, count, v);
			value_clear(&ignored);
		}
		if (args)
			free_args(args, count);
		value_clear(&object);
		return;
	}
	if (target->kind == E_CALL && target->target->kind == E_NAME) {
		variable *found = lookup(in, target->target->name);
		if (!found || found->v.kind != V_ARRAY || target->arg_count != 1) {
			raise(in, 13, L"Type mismatch");
			return;
		}
		value *args = evaluate_args(in, target);
		LONG index = in->raised ? 0 : to_long(in, &args[0]);
		if (!in->raised && (index < 0 || (UINT)index >= found->v.array->count))
			raise(in, 9, L"Subscript out of range");
		if (!in->raised) {
			value_clear(&found->v.array->items[index]);
			found->v.array->items[index] = *v;
			*v = (value){V_EMPTY};
		}
		free_args(args, 1);
		return;
	}
	raise(in, 13, L"Type mismatch");
}

static BOOL loop_test(interp *in, stmt *s)
{
	value v = evaluate(in, s->conditions[0]);
	BOOL yes = !in->raised && to_bool(in, &v);
	value_clear(&v);
	return s->until ? !yes : yes;
}

static flow run_statement(interp *in, stmt *s)
{
	switch (s->kind) {
	case S_CALL: {
		value v = evaluate(in, s->target);
		value_clear(&v);
		return FLOW_NEXT;
	}
	case S_ASSIGN: {
		value v = evaluate(in, s->value);
		if (!in->raised)
			assign(in, s->target, &v);
		value_clear(&v);
		return FLOW_NEXT;
	}
	case S_IF:
		for (UINT i = 0; i < s->branch_count; i++) {
			value v = evaluate(in, s->conditions[i]);
			BOOL yes = !in->raised && v.kind != V_NULL && to_bool(in, &v);
			value_clear(&v);
			if (in->raised)
				return FLOW_NEXT;
			if (yes)
				return run_block(in, &s->blocks[i]);
		}
		return s->has_otherwise ? run_block(in, &s->otherwise) : FLOW_NEXT;
	case S_FOR: {
		value start = evaluate(in, s->value), end = evaluate(in, s->to), step = s->step ? evaluate(in, s->step) : number_value(1);
		double last = to_number(in, &end), by = to_number(in, &step);
		value counter = number_value(to_number(in, &start));
		value_clear(&start);
		value_clear(&end);
		value_clear(&step);
		expr name = {E_NAME, s->line};
		name.name = s->name;
		assign(in, &name, &counter);
		while (!in->raised) {
			variable *v = lookup(in, s->name);
			double at = v ? to_number(in, &v->v) : 0;
			if (in->raised || (by >= 0 ? at > last : at < last))
				break;
			flow f = run_block(in, &s->blocks[0]);
			if (f == FLOW_EXIT_FOR)
				break;
			if (f == FLOW_EXIT_PROC || in->raised)
				return f;
			v = lookup(in, s->name);
			counter = number_value((v ? to_number(in, &v->v) : 0) + by);
			assign(in, &name, &counter);
		}
		return FLOW_NEXT;
	}
	case S_FOR_EACH: {
		value collection = evaluate(in, s->value);
		if (!in->raised && collection.kind != V_ARRAY)
			raise(in, 451, L"Object not a collection");
		expr name = {E_NAME, s->line};
		name.name = s->name;
		for (UINT i = 0; !in->raised && i < collection.array->count; i++) {
			value item = value_copy(&collection.array->items[i]);
			assign(in, &name, &item);
			flow f = run_block(in, &s->blocks[0]);
			if (f == FLOW_EXIT_FOR)
				break;
			if (f == FLOW_EXIT_PROC) {
				value_clear(&collection);
				return f;
			}
		}
		value_clear(&collection);
		return FLOW_NEXT;
	}
	case S_DO:
		for (;;) {
			if (s->test_first && (!loop_test(in, s) || in->raised))
				break;
			flow f = run_block(in, &s->blocks[0]);
			if (f == FLOW_EXIT_DO)
				break;
			if (f == FLOW_EXIT_PROC || in->raised)
				return f;
			if (!s->test_first && s->conditions[0] && (!loop_test(in, s) || in->raised))
				break;
		}
		return FLOW_NEXT;
	case S_EXIT:
		if (same_word(s->exit_kind, L"for"))
			return FLOW_EXIT_FOR;
		if (same_word(s->exit_kind, L"do"))
			return FLOW_EXIT_DO;
		return FLOW_EXIT_PROC;
	case S_DIM:
		for (UINT i = 0; i < s->name_count && !in->raised; i++) {
			variable *v = s->redim ? lookup(in, s->names[i]) : NULL;
			if (!v)
				v = declare(in->current, s->names[i]);
			if (!s->dims[i])
				continue;
			value size = evaluate(in, s->dims[i]);
			LONG top = to_long(in, &size);
			value_clear(&size);
			if (in->raised || top < -1 || top > 1000000) {
				raise(in, 9, L"Subscript out of range");
				break;
			}
			value array = make_array((UINT)(top + 1));
			if (s->preserve && v->v.kind == V_ARRAY)
				for (UINT k = 0; k < array.array->count && k < v->v.array->count; k++)
					array.array->items[k] = value_copy(&v->v.array->items[k]);
			store(v, &array);
		}
		return FLOW_NEXT;
	case S_ON_ERROR:
		in->current->resume_next = s->resume_next;
		in->error_number = 0;
		in->error_text[0] = 0;
		return FLOW_NEXT;
	case S_SELECT: {
		value tested = evaluate(in, s->value);
		for (UINT i = 0; i < s->branch_count && !in->raised; i++)
			for (UINT k = 0; k < s->case_counts[i]; k++) {
				value candidate = evaluate(in, s->case_values[i][k]);
				int order = 1;
				BOOL match = !in->raised && compare_values(in, &tested, &candidate, &order) && !order;
				value_clear(&candidate);
				if (match) {
					value_clear(&tested);
					return run_block(in, &s->blocks[i]);
				}
			}
		value_clear(&tested);
		return s->has_otherwise && !in->raised ? run_block(in, &s->otherwise) : FLOW_NEXT;
	}
	}
	return FLOW_NEXT;
}

static flow run_block(interp *in, block *b)
{
	for (UINT i = 0; i < b->count; i++) {
		flow f = run_statement(in, b->items[i]);
		if (in->raised) {
			if (!in->error_line)
				in->error_line = b->items[i]->line;
			if (!in->current->resume_next)
				return FLOW_EXIT_PROC;
			// On Error Resume Next: Err keeps the error, and the next statement runs.
			in->raised = FALSE;
			in->error_line = 0;
			continue;
		}
		if (f != FLOW_NEXT)
			return f;
	}
	return FLOW_NEXT;
}

static value call_with(interp *in, procedure *proc, value *args, UINT count)
{
	if (count > proc->param_count) {
		raise(in, 450, L"Wrong number of arguments or invalid property assignment");
		return (value){V_EMPTY};
	}
	if (in->depth > 200) {
		raise(in, 28, L"Out of stack space");
		return (value){V_EMPTY};
	}
	frame f = {.proc = proc};
	for (UINT i = 0; i < proc->param_count; i++) {
		value v = i < count ? value_copy(&args[i]) : (value){V_EMPTY};
		store(declare(&f, proc->params[i]), &v);
	}
	frame *caller = in->current;
	in->current = &f;
	in->depth++;
	run_block(in, &proc->body);
	in->depth--;
	in->current = caller;
	value result = f.result;
	f.result = (value){V_EMPTY};
	frame_clear(&f);
	return result;
}

static WCHAR *script_text(const BYTE *data, SIZE_T size)
{
	if (size >= 2 && data[0] == 0xff && data[1] == 0xfe)
		return msi_copy_n((const WCHAR *)(data + 2), (size - 2) / 2);
	UINT codepage = CP_ACP;
	if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
		data += 3;
		size -= 3;
		codepage = CP_UTF8;
	}
	int length = MultiByteToWideChar(codepage, 0, (const char *)data, (int)size, NULL, 0);
	WCHAR *text = msi_alloc((length + 1) * sizeof(WCHAR));
	MultiByteToWideChar(codepage, 0, (const char *)data, (int)size, text, length);
	return text;
}

// Runs a script's own statements, then the function the action names, whose answer is the action's status.
UINT vbscript_run(msi_package *package, MSIHANDLE session, const BYTE *data, SIZE_T size, const WCHAR *entry, const WCHAR *action)
{
	WCHAR *code = script_text(data, size);
	parser p = {0};
	UINT status = ERROR_INSTALL_FAILURE;
	if (!tokenize(code, &p.tokens))
		package_log(L"Script of %s cannot be read", action);
	else if (!parse_script(&p))
		package_log(L"Script of %s does not parse: %s", action, p.error);
	else {
		interp in = {package, session, &p};
		in.current = &in.global;
		run_block(&in, &p.main);
		value result = {V_EMPTY};
		if (!in.raised && entry && *entry) {
			procedure *proc = find_procedure(&in, entry);
			if (proc)
				result = call_with(&in, proc, NULL, 0);
			else
				raise(&in, 13, L"The entry function is not in the script");
		}
		if (in.raised) {
			package_log(L"Script error in %s, line %u: %d %s", action, in.error_line, in.error_number, in.error_text);
		} else {
			// The documented answers: 1 succeeds, 2 the user cancelled, 3 failed, 4 suspends, 5 skips the remaining actions.
			LONG answer = result.kind == V_EMPTY ? 1 : (LONG)to_number(&in, &result);
			status = answer == 0 || answer == 1 ? ERROR_SUCCESS : answer == 2 ? ERROR_INSTALL_USEREXIT : answer == 4 ? ERROR_INSTALL_SUSPEND :
				answer == 5 ? ERROR_NO_MORE_ITEMS : ERROR_INSTALL_FAILURE;
		}
		value_clear(&result);
		frame_clear(&in.global);
	}
	for (UINT i = 0; i < p.tokens.count; i++)
		msi_free(p.tokens.tokens[i].text);
	msi_free(p.tokens.tokens);
	for (UINT i = 0; i < p.owned_count; i++)
		msi_free(p.owned[i]);
	msi_free(p.owned);
	msi_free(p.procedures);
	msi_free(code);
	return status;
}
