/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

// The installer's SQL subset, as its public documentation lists it, read by plain recursive descent.
typedef enum {
	TOKEN_END, TOKEN_NAME, TOKEN_QUOTED_NAME, TOKEN_TEXT, TOKEN_NUMBER, TOKEN_PARAM, TOKEN_STAR, TOKEN_COMMA, TOKEN_DOT,
	TOKEN_OPEN, TOKEN_CLOSE, TOKEN_EQ, TOKEN_NE, TOKEN_LT, TOKEN_GT, TOKEN_LE, TOKEN_GE, TOKEN_BAD,
} token_kind;

typedef struct {
	const WCHAR *at;
	token_kind kind;
	const WCHAR *start;
	UINT length;
	sql_statement *statement;
	BOOL failed;
} parser;

static BOOL is_name_start(WCHAR c)
{
	return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || c >= 0x80 || (c < 0x20 && c && c != L' ' && c != L'\t' && c != L'\r' && c != L'\n');
}

static BOOL is_digit(WCHAR c)
{
	return c >= L'0' && c <= L'9';
}

static void next_token(parser *p)
{
	while (*p->at == L' ' || *p->at == L'\t' || *p->at == L'\r' || *p->at == L'\n')
		p->at++;
	WCHAR c = *p->at;
	p->start = p->at;
	p->length = 1;
	if (!c) {
		p->kind = TOKEN_END;
		p->length = 0;
	} else if (c == L'`' || c == L'\'') {
		const WCHAR *end = p->at + 1;
		while (*end && *end != c)
			end++;
		if (!*end) {
			p->kind = TOKEN_BAD;
			return;
		}
		p->start = p->at + 1;
		p->length = (UINT)(end - p->start);
		p->at = end + 1;
		p->kind = c == L'`' ? TOKEN_QUOTED_NAME : TOKEN_TEXT;
	} else if (is_name_start(c)) {
		const WCHAR *end = p->at;
		while (is_name_start(*end) || is_digit(*end) || *end == L'-')
			end++;
		p->length = (UINT)(end - p->at);
		// A backtick left at the end of a plain name closes nothing and is not an error.
		p->at = *end == L'`' ? end + 1 : end;
		p->kind = TOKEN_NAME;
	} else if (is_digit(c) || (c == L'-' && is_digit(p->at[1]))) {
		const WCHAR *end = p->at + 1;
		while (is_digit(*end))
			end++;
		p->length = (UINT)(end - p->at);
		p->at = end;
		p->kind = TOKEN_NUMBER;
	} else {
		p->at++;
		switch (c) {
		case L'?': p->kind = TOKEN_PARAM; break;
		case L'*': p->kind = TOKEN_STAR; break;
		case L',': p->kind = TOKEN_COMMA; break;
		case L'.': p->kind = TOKEN_DOT; break;
		case L'(': p->kind = TOKEN_OPEN; break;
		case L')': p->kind = TOKEN_CLOSE; break;
		case L'=': p->kind = TOKEN_EQ; break;
		case L'<':
			p->kind = TOKEN_LT;
			if (*p->at == L'>') {
				p->kind = TOKEN_NE;
				p->at++;
			} else if (*p->at == L'=') {
				p->kind = TOKEN_LE;
				p->at++;
			}
			break;
		case L'>':
			p->kind = TOKEN_GT;
			if (*p->at == L'=') {
				p->kind = TOKEN_GE;
				p->at++;
			}
			break;
		default:
			p->kind = TOKEN_BAD;
		}
	}
}

static BOOL at_word(parser *p, const WCHAR *word)
{
	if (p->kind != TOKEN_NAME || (UINT)lstrlenW(word) != p->length)
		return FALSE;
	return !_wcsnicmp(p->start, word, p->length);
}

static BOOL take_word(parser *p, const WCHAR *word)
{
	if (!at_word(p, word))
		return FALSE;
	next_token(p);
	return TRUE;
}

static BOOL take(parser *p, token_kind kind)
{
	if (p->kind != kind)
		return FALSE;
	next_token(p);
	return TRUE;
}

static BOOL fail(parser *p)
{
	p->failed = TRUE;
	return FALSE;
}

static const WCHAR *reserved[] = {
	L"ADD", L"ALTER", L"AND", L"BY", L"CHAR", L"CHARACTER", L"CREATE", L"DELETE", L"DISTINCT", L"DROP", L"FREE", L"FROM", L"HOLD", L"INSERT", L"INT",
	L"INTEGER", L"INTO", L"IS", L"KEY", L"LOCALIZABLE", L"LONG", L"LONGCHAR", L"NOT", L"NULL", L"OBJECT", L"OR", L"ORDER", L"PRIMARY", L"SELECT",
	L"SET", L"SHORT", L"TABLE", L"TEMPORARY", L"UPDATE", L"VALUES", L"WHERE",
};

static WCHAR *take_name(parser *p)
{
	if (p->kind == TOKEN_NAME) {
		for (UINT i = 0; i < sizeof(reserved) / sizeof(*reserved); i++)
			if (at_word(p, reserved[i])) {
				fail(p);
				return NULL;
			}
	} else if (p->kind != TOKEN_QUOTED_NAME) {
		fail(p);
		return NULL;
	}
	WCHAR *name = msi_copy_n(p->start, p->length);
	next_token(p);
	return name;
}

static void expr_free(sql_expr *expr)
{
	if (!expr)
		return;
	expr_free(expr->left);
	expr_free(expr->right);
	msi_free(expr->table);
	msi_free(expr->name);
	msi_free(expr);
}

static sql_expr *expr_new(expr_kind kind)
{
	sql_expr *expr = msi_alloc(sizeof(*expr));
	expr->kind = kind;
	return expr;
}

static INT text_number(const WCHAR *text, UINT length)
{
	BOOL negative = *text == L'-';
	UINT value = 0;
	for (UINT i = negative; i < length; i++)
		value = value * 10 + (text[i] - L'0');
	return negative ? -(INT)value : (INT)value;
}

static sql_expr *parse_value(parser *p)
{
	sql_expr *expr = NULL;
	switch (p->kind) {
	case TOKEN_NUMBER:
		expr = expr_new(EXPR_NUMBER);
		expr->number = text_number(p->start, p->length);
		next_token(p);
		return expr;
	case TOKEN_TEXT:
		expr = expr_new(EXPR_TEXT);
		expr->name = msi_copy_n(p->start, p->length);
		next_token(p);
		return expr;
	case TOKEN_PARAM:
		expr = expr_new(EXPR_PARAM);
		expr->param = ++p->statement->param_count;
		next_token(p);
		return expr;
	default:
		fail(p);
		return NULL;
	}
}

static sql_expr *parse_column_expr(parser *p)
{
	WCHAR *first = take_name(p);
	if (!first)
		return NULL;
	sql_expr *expr = expr_new(EXPR_COLUMN);
	expr->name = first;
	if (take(p, TOKEN_DOT)) {
		expr->table = first;
		expr->name = take_name(p);
		if (!expr->name) {
			expr_free(expr);
			return NULL;
		}
	}
	return expr;
}

static sql_expr *parse_operand(parser *p)
{
	return p->kind == TOKEN_NAME || p->kind == TOKEN_QUOTED_NAME ? parse_column_expr(p) : parse_value(p);
}

static sql_expr *parse_or(parser *p);

static sql_expr *parse_comparison(parser *p)
{
	if (take(p, TOKEN_OPEN)) {
		sql_expr *inner = parse_or(p);
		if (!inner || !take(p, TOKEN_CLOSE)) {
			fail(p);
			expr_free(inner);
			return NULL;
		}
		return inner;
	}
	sql_expr *left = parse_operand(p);
	if (!left)
		return NULL;
	expr_kind kind;
	if (take_word(p, L"IS")) {
		kind = take_word(p, L"NOT") ? OP_NOT_NULL : OP_IS_NULL;
		if (!take_word(p, L"NULL")) {
			fail(p);
			expr_free(left);
			return NULL;
		}
		sql_expr *test = expr_new(kind);
		test->left = left;
		return test;
	}
	switch (p->kind) {
	case TOKEN_EQ: kind = OP_EQ; break;
	case TOKEN_NE: kind = OP_NE; break;
	case TOKEN_LT: kind = OP_LT; break;
	case TOKEN_GT: kind = OP_GT; break;
	case TOKEN_LE: kind = OP_LE; break;
	case TOKEN_GE: kind = OP_GE; break;
	default:
		fail(p);
		expr_free(left);
		return NULL;
	}
	next_token(p);
	sql_expr *right = parse_operand(p);
	if (!right) {
		expr_free(left);
		return NULL;
	}
	sql_expr *compare = expr_new(kind);
	compare->left = left;
	compare->right = right;
	return compare;
}

static sql_expr *parse_and(parser *p)
{
	sql_expr *left = parse_comparison(p);
	while (left && take_word(p, L"AND")) {
		sql_expr *join = expr_new(OP_AND);
		join->left = left;
		join->right = parse_comparison(p);
		left = join;
		if (!join->right) {
			expr_free(join);
			return NULL;
		}
	}
	return left;
}

static sql_expr *parse_or(parser *p)
{
	sql_expr *left = parse_and(p);
	while (left && take_word(p, L"OR")) {
		sql_expr *join = expr_new(OP_OR);
		join->left = left;
		join->right = parse_and(p);
		left = join;
		if (!join->right) {
			expr_free(join);
			return NULL;
		}
	}
	return left;
}

static sql_column *add_column(sql_column **list, UINT *count)
{
	*list = msi_grow(*list, (*count + 1) * sizeof(sql_column));
	sql_column *column = &(*list)[*count];
	memset(column, 0, sizeof(*column));
	(*count)++;
	return column;
}

static BOOL parse_column_ref(parser *p, sql_column *column)
{
	column->name = take_name(p);
	if (!column->name)
		return FALSE;
	if (take(p, TOKEN_DOT)) {
		column->table = column->name;
		column->name = take_name(p);
		if (!column->name)
			return FALSE;
	}
	return TRUE;
}

static BOOL parse_select(parser *p)
{
	sql_statement *s = p->statement;
	s->kind = SQL_SELECT;
	s->distinct = take_word(p, L"DISTINCT");
	if (take(p, TOKEN_STAR)) {
		s->star = TRUE;
	} else {
		do {
			sql_column *column = add_column(&s->columns, &s->column_count);
			if (!parse_column_ref(p, column))
				return fail(p);
		} while (take(p, TOKEN_COMMA));
	}
	if (!take_word(p, L"FROM"))
		return fail(p);
	do {
		if (s->table_count >= 32)
			return fail(p);
		s->tables[s->table_count] = take_name(p);
		if (!s->tables[s->table_count])
			return fail(p);
		s->table_count++;
	} while (take(p, TOKEN_COMMA));
	if (take_word(p, L"WHERE")) {
		s->where = parse_or(p);
		if (!s->where)
			return fail(p);
	}
	if (take_word(p, L"ORDER")) {
		if (!take_word(p, L"BY"))
			return fail(p);
		do {
			sql_column *column = add_column(&s->order, &s->order_count);
			if (!parse_column_ref(p, column))
				return fail(p);
		} while (take(p, TOKEN_COMMA));
	}
	return TRUE;
}

static BOOL parse_insert(parser *p)
{
	sql_statement *s = p->statement;
	s->kind = SQL_INSERT;
	if (!take_word(p, L"INTO"))
		return fail(p);
	s->tables[0] = take_name(p);
	if (!s->tables[0] || !take(p, TOKEN_OPEN))
		return fail(p);
	s->table_count = 1;
	do {
		sql_column *column = add_column(&s->columns, &s->column_count);
		if (!parse_column_ref(p, column))
			return fail(p);
	} while (take(p, TOKEN_COMMA));
	if (!take(p, TOKEN_CLOSE) || !take_word(p, L"VALUES") || !take(p, TOKEN_OPEN))
		return fail(p);
	UINT index = 0;
	do {
		if (index >= s->column_count)
			return fail(p);
		s->columns[index].value = parse_value(p);
		if (!s->columns[index].value)
			return fail(p);
		index++;
	} while (take(p, TOKEN_COMMA));
	if (index != s->column_count || !take(p, TOKEN_CLOSE))
		return fail(p);
	s->temporary = take_word(p, L"TEMPORARY");
	return TRUE;
}

static BOOL parse_update(parser *p)
{
	sql_statement *s = p->statement;
	s->kind = SQL_UPDATE;
	s->tables[0] = take_name(p);
	if (!s->tables[0] || !take_word(p, L"SET"))
		return fail(p);
	s->table_count = 1;
	do {
		sql_column *column = add_column(&s->columns, &s->column_count);
		if (!parse_column_ref(p, column) || !take(p, TOKEN_EQ))
			return fail(p);
		column->value = parse_value(p);
		if (!column->value)
			return fail(p);
	} while (take(p, TOKEN_COMMA));
	if (take_word(p, L"WHERE")) {
		s->where = parse_or(p);
		if (!s->where)
			return fail(p);
	}
	return TRUE;
}

static BOOL parse_delete(parser *p)
{
	sql_statement *s = p->statement;
	s->kind = SQL_DELETE;
	if (!take_word(p, L"FROM"))
		return fail(p);
	s->tables[0] = take_name(p);
	if (!s->tables[0])
		return fail(p);
	s->table_count = 1;
	if (take_word(p, L"WHERE")) {
		s->where = parse_or(p);
		if (!s->where)
			return fail(p);
	}
	return TRUE;
}

// CHAR(n), LONGCHAR, SHORT, INT, LONG and OBJECT, then what may follow them.
static BOOL parse_type(parser *p, UINT *type)
{
	UINT bits;
	if (take_word(p, L"CHAR") || take_word(p, L"CHARACTER")) {
		bits = TYPE_STRING | TYPE_NOT_BINARY;
		if (take(p, TOKEN_OPEN)) {
			if (p->kind != TOKEN_NUMBER)
				return fail(p);
			INT size = text_number(p->start, p->length);
			if (size < 0 || size > 255)
				return fail(p);
			bits |= size;
			next_token(p);
			if (!take(p, TOKEN_CLOSE))
				return fail(p);
		}
	} else if (take_word(p, L"LONGCHAR")) {
		bits = TYPE_STRING | TYPE_NOT_BINARY;
	} else if (take_word(p, L"SHORT") || take_word(p, L"INT") || take_word(p, L"INTEGER")) {
		bits = TYPE_NOT_BINARY | 2;
	} else if (take_word(p, L"LONG")) {
		bits = 4;
	} else if (take_word(p, L"OBJECT")) {
		bits = TYPE_STRING;
	} else {
		return fail(p);
	}
	bits |= TYPE_VALID | TYPE_NULLABLE;
	for (;;) {
		if (take_word(p, L"NOT")) {
			if (!take_word(p, L"NULL"))
				return fail(p);
			bits &= ~TYPE_NULLABLE;
		} else if (take_word(p, L"TEMPORARY")) {
			bits |= TYPE_TEMPORARY;
		} else if (take_word(p, L"LOCALIZABLE")) {
			bits |= TYPE_LOCALIZABLE;
		} else {
			break;
		}
	}
	*type = bits;
	return TRUE;
}

static BOOL parse_create(parser *p)
{
	sql_statement *s = p->statement;
	s->kind = SQL_CREATE;
	if (!take_word(p, L"TABLE"))
		return fail(p);
	s->tables[0] = take_name(p);
	if (!s->tables[0] || !take(p, TOKEN_OPEN))
		return fail(p);
	s->table_count = 1;
	do {
		sql_column *column = add_column(&s->columns, &s->column_count);
		column->name = take_name(p);
		if (!column->name || !parse_type(p, &column->type))
			return fail(p);
	} while (take(p, TOKEN_COMMA));
	if (!take_word(p, L"PRIMARY") || !take_word(p, L"KEY"))
		return fail(p);
	do {
		s->keys = msi_grow(s->keys, (s->key_count + 1) * sizeof(*s->keys));
		s->keys[s->key_count] = take_name(p);
		if (!s->keys[s->key_count])
			return fail(p);
		s->key_count++;
	} while (take(p, TOKEN_COMMA));
	if (!take(p, TOKEN_CLOSE))
		return fail(p);
	if (take_word(p, L"HOLD"))
		s->hold = TRUE;
	else if (take_word(p, L"FREE"))
		s->free_table = TRUE;
	return TRUE;
}

static BOOL parse_alter(parser *p)
{
	sql_statement *s = p->statement;
	s->kind = SQL_ALTER;
	if (!take_word(p, L"TABLE"))
		return fail(p);
	s->tables[0] = take_name(p);
	if (!s->tables[0])
		return fail(p);
	s->table_count = 1;
	if (take_word(p, L"ADD")) {
		s->add_column = TRUE;
		sql_column *column = add_column(&s->columns, &s->column_count);
		column->name = take_name(p);
		if (!column->name || !parse_type(p, &column->type))
			return fail(p);
	}
	if (take_word(p, L"HOLD"))
		s->hold = TRUE;
	else if (take_word(p, L"FREE"))
		s->free_table = TRUE;
	else if (!s->add_column)
		return fail(p);
	return TRUE;
}

static BOOL parse_drop(parser *p)
{
	sql_statement *s = p->statement;
	s->kind = SQL_DROP;
	if (!take_word(p, L"TABLE"))
		return fail(p);
	s->tables[0] = take_name(p);
	if (!s->tables[0])
		return fail(p);
	s->table_count = 1;
	return TRUE;
}

void sql_free(sql_statement *s)
{
	if (!s)
		return;
	for (UINT i = 0; i < s->table_count; i++)
		msi_free(s->tables[i]);
	for (UINT i = 0; i < s->column_count; i++) {
		msi_free(s->columns[i].table);
		msi_free(s->columns[i].name);
		expr_free(s->columns[i].value);
	}
	msi_free(s->columns);
	for (UINT i = 0; i < s->order_count; i++) {
		msi_free(s->order[i].table);
		msi_free(s->order[i].name);
	}
	msi_free(s->order);
	for (UINT i = 0; i < s->key_count; i++)
		msi_free(s->keys[i]);
	msi_free(s->keys);
	expr_free(s->where);
	msi_free(s);
}

UINT sql_parse(const WCHAR *text, sql_statement **statement)
{
	parser p = {text};
	sql_statement *s = msi_alloc(sizeof(*s));
	p.statement = s;
	next_token(&p);
	BOOL ok;
	if (take_word(&p, L"SELECT"))
		ok = parse_select(&p);
	else if (take_word(&p, L"INSERT"))
		ok = parse_insert(&p);
	else if (take_word(&p, L"UPDATE"))
		ok = parse_update(&p);
	else if (take_word(&p, L"DELETE"))
		ok = parse_delete(&p);
	else if (take_word(&p, L"CREATE"))
		ok = parse_create(&p);
	else if (take_word(&p, L"ALTER"))
		ok = parse_alter(&p);
	else if (take_word(&p, L"DROP"))
		ok = parse_drop(&p);
	else
		ok = FALSE;
	if (!ok || p.failed || p.kind != TOKEN_END) {
		sql_free(s);
		return ERROR_BAD_QUERY_SYNTAX;
	}
	*statement = s;
	return ERROR_SUCCESS;
}
