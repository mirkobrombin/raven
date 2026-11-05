/* SPDX-License-Identifier: MIT */
#include "vbs.h"

/* The parser builds statements and expressions; everything it allocates is kept on the code's list and freed with it. */

typedef struct {
	token_list tokens;
	UINT at;
	script_code *code;
	parse_error *error;
	/* Inside With blocks a name may begin with a dot; inside a class Me and the class's members are known. */
	UINT with_depth;
	BOOL in_class;
} parser;

static void *own(parser *p, SIZE_T size)
{
	script_code *code = p->code;
	if (code->owned_count == code->owned_room) {
		code->owned_room = code->owned_room ? code->owned_room * 2 : 256;
		code->owned = vbs_grow(code->owned, code->owned_room * sizeof(void *));
	}
	return code->owned[code->owned_count++] = vbs_alloc(size);
}

/* A copy of a token's text, which the code owns; the token keeps its own, since a call statement may be read twice. */
static WCHAR *take_text(parser *p, token *t)
{
	if (!t->text)
		return NULL;
	WCHAR *text = own(p, (lstrlenW(t->text) + 1) * sizeof(WCHAR));
	lstrcpyW(text, t->text);
	return text;
}

static token *peek(parser *p) { return &p->tokens.items[p->at]; }

static token *peek_at(parser *p, UINT offset)
{
	return p->at + offset < p->tokens.count ? &p->tokens.items[p->at + offset] : &p->tokens.items[p->tokens.count - 1];
}

static token *advance(parser *p)
{
	token *t = peek(p);
	if (t->kind != T_END)
		p->at++;
	return t;
}

static BOOL is_word(parser *p, const WCHAR *word) { return peek(p)->kind == T_NAME && vbs_same(peek(p)->text, word); }

static BOOL is_word_at(parser *p, UINT offset, const WCHAR *word)
{
	token *t = peek_at(p, offset);
	return t->kind == T_NAME && vbs_same(t->text, word);
}

static BOOL accept_word(parser *p, const WCHAR *word)
{
	if (!is_word(p, word))
		return FALSE;
	advance(p);
	return TRUE;
}

static BOOL is_symbol(parser *p, const WCHAR *symbol) { return peek(p)->kind == T_SYMBOL && !lstrcmpW(peek(p)->symbol, symbol); }

static BOOL accept_symbol(parser *p, const WCHAR *symbol)
{
	if (!is_symbol(p, symbol))
		return FALSE;
	advance(p);
	return TRUE;
}

static BOOL fail(parser *p, const WCHAR *what)
{
	if (!p->error->text[0]) {
		lstrcpynW(p->error->text, what, ARRAYSIZE(p->error->text));
		p->error->line = peek(p)->line;
		p->error->column = peek(p)->column;
		p->error->number = VBS_E_SYNTAX;
	}
	return FALSE;
}

static BOOL at_statement_end(parser *p)
{
	return peek(p)->kind == T_LINE || peek(p)->kind == T_END || is_word(p, L"else");
}

/* Words that cannot name a variable or a procedure. */
static BOOL reserved(const WCHAR *name)
{
	static const WCHAR *const words[] = { L"and", L"call", L"case", L"class", L"const", L"dim", L"do", L"each", L"else", L"elseif", L"empty",
		L"end", L"eqv", L"erase", L"error", L"exit", L"explicit", L"false", L"for", L"function", L"get", L"goto", L"if", L"imp", L"in", L"is",
		L"let", L"loop", L"me", L"mod", L"new", L"next", L"not", L"nothing", L"null", L"on", L"option", L"or", L"preserve", L"private",
		L"public", L"redim", L"resume", L"select", L"set", L"step", L"sub", L"then", L"to", L"true", L"until", L"wend", L"while", L"with",
		L"xor", NULL };
	for (UINT i = 0; words[i]; i++)
		if (vbs_same(name, words[i]))
			return TRUE;
	return FALSE;
}

static BOOL is_name(parser *p) { return peek(p)->kind == T_NAME && !reserved(peek(p)->text); }

/* A member name after a dot may be any word, keywords included (Err.Raise, obj.Property, d.Item). */
static WCHAR *member_name(parser *p)
{
	if (peek(p)->kind != T_NAME)
		return fail(p, L"Expected identifier"), NULL;
	return take_text(p, advance(p));
}

static WCHAR *name(parser *p)
{
	if (!is_name(p))
		return fail(p, L"Expected identifier"), NULL;
	return take_text(p, advance(p));
}

static expr *new_expr(parser *p, expr_kind kind)
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

static expr *expression(parser *p);

/* The opening parenthesis is taken; an empty argument stands for one left out. */
static BOOL arguments(parser *p, expr *call)
{
	if (accept_symbol(p, L")"))
		return TRUE;
	for (;;) {
		if (is_symbol(p, L",") || is_symbol(p, L")")) {
			add_arg(p, call, NULL);
		} else {
			expr *arg = expression(p);
			if (!arg)
				return FALSE;
			add_arg(p, call, arg);
		}
		if (accept_symbol(p, L")"))
			return TRUE;
		if (!accept_symbol(p, L","))
			return fail(p, L"Expected ')'");
	}
}

/* Members and calls after a value. last_call is where the last parenthesis of the chain began, for the statement form of a call. */
static expr *postfix(parser *p, expr *e, UINT *last_call)
{
	for (;;) {
		if (is_symbol(p, L".") && peek(p)->spaced && p->with_depth && e->kind != E_WITH)
			return e;
		if (accept_symbol(p, L".")) {
			expr *member = new_expr(p, E_MEMBER);
			member->target = e;
			if (!(member->name = member_name(p)))
				return NULL;
			e = member;
		} else if (is_symbol(p, L"(")) {
			if (last_call)
				*last_call = p->at;
			advance(p);
			expr *call = new_expr(p, E_CALL);
			call->target = e;
			if (!arguments(p, call))
				return NULL;
			e = call;
		} else {
			return e;
		}
	}
}

static expr *constant(parser *p, VARIANT value)
{
	expr *e = new_expr(p, E_CONSTANT);
	e->constant = value;
	return e;
}

static expr *primary(parser *p)
{
	token *t = peek(p);
	VARIANT v;
	VariantInit(&v);
	if (t->kind == T_NUMBER || t->kind == T_DATE) {
		advance(p);
		return constant(p, t->number);
	}
	if (t->kind == T_TEXT) {
		advance(p);
		V_VT(&v) = VT_BSTR;
		/* The constant's string is freed with the code, as a BSTR: kept apart from the owned list. */
		V_BSTR(&v) = SysAllocString(t->text);
		expr *e = constant(p, v);
		return e;
	}
	if (accept_symbol(p, L"(")) {
		expr *e = expression(p);
		if (!e || !accept_symbol(p, L")"))
			return fail(p, L"Expected ')'"), NULL;
		e->parenthesized = TRUE;
		return postfix(p, e, NULL);
	}
	if (is_symbol(p, L".") && p->with_depth) {
		/* .Member inside With names a member of the With object. */
		expr *with = new_expr(p, E_WITH);
		return postfix(p, with, NULL);
	}
	if (t->kind != T_NAME)
		return fail(p, L"Expected expression"), NULL;
	if (accept_word(p, L"true") || accept_word(p, L"false")) {
		V_VT(&v) = VT_BOOL;
		V_BOOL(&v) = vbs_same(t->text, L"true") ? VARIANT_TRUE : VARIANT_FALSE;
		return constant(p, v);
	}
	if (accept_word(p, L"empty"))
		return constant(p, v);
	if (accept_word(p, L"null")) {
		V_VT(&v) = VT_NULL;
		return constant(p, v);
	}
	if (accept_word(p, L"nothing"))
		return new_expr(p, E_NOTHING);
	if (accept_word(p, L"me"))
		return postfix(p, new_expr(p, E_ME), NULL);
	if (accept_word(p, L"new")) {
		expr *e = new_expr(p, E_NEW);
		if (!(e->name = name(p)))
			return NULL;
		return e;
	}
	if (reserved(t->text))
		return fail(p, L"Expected expression"), NULL;
	expr *e = new_expr(p, E_NAME);
	e->name = take_text(p, advance(p));
	return postfix(p, e, NULL);
}

static expr *binary(parser *p, const WCHAR *op, expr *left, expr *right)
{
	if (!right)
		return NULL;
	expr *e = new_expr(p, E_BINARY);
	lstrcpyW(e->op, op);
	e->left = left;
	e->right = right;
	return e;
}

static expr *power(parser *p)
{
	expr *e = primary(p);
	while (e && is_symbol(p, L"^")) {
		advance(p);
		/* The exponent may carry its own sign: 2 ^ -1. */
		expr *right;
		if (accept_symbol(p, L"-")) {
			right = new_expr(p, E_UNARY);
			lstrcpyW(right->op, L"-");
			if (!(right->left = primary(p)))
				return NULL;
		} else {
			right = primary(p);
		}
		e = binary(p, L"^", e, right);
	}
	return e;
}

static expr *unary(parser *p)
{
	if (accept_symbol(p, L"-")) {
		expr *e = new_expr(p, E_UNARY);
		lstrcpyW(e->op, L"-");
		return (e->left = unary(p)) ? e : NULL;
	}
	if (accept_symbol(p, L"+"))
		return unary(p);
	return power(p);
}

static expr *product(parser *p)
{
	expr *e = unary(p);
	while (e && (is_symbol(p, L"*") || is_symbol(p, L"/"))) {
		WCHAR op[2] = { advance(p)->symbol[0], 0 };
		e = binary(p, op, e, unary(p));
	}
	return e;
}

static expr *integer_division(parser *p)
{
	expr *e = product(p);
	while (e && accept_symbol(p, L"\\"))
		e = binary(p, L"\\", e, product(p));
	return e;
}

static expr *modulo(parser *p)
{
	expr *e = integer_division(p);
	while (e && accept_word(p, L"mod"))
		e = binary(p, L"mod", e, integer_division(p));
	return e;
}

static expr *sum(parser *p)
{
	expr *e = modulo(p);
	while (e && (is_symbol(p, L"+") || is_symbol(p, L"-"))) {
		WCHAR op[2] = { advance(p)->symbol[0], 0 };
		e = binary(p, op, e, modulo(p));
	}
	return e;
}

static expr *concatenation(parser *p)
{
	expr *e = sum(p);
	while (e && accept_symbol(p, L"&"))
		e = binary(p, L"&", e, sum(p));
	return e;
}

static expr *comparison(parser *p)
{
	expr *e = concatenation(p);
	for (;;) {
		if (!e)
			return NULL;
		static const WCHAR *const ops[] = { L"=", L"<>", L"<=", L">=", L"<", L">", NULL };
		BOOL matched = FALSE;
		for (UINT i = 0; ops[i]; i++)
			if (is_symbol(p, ops[i])) {
				advance(p);
				e = binary(p, ops[i], e, concatenation(p));
				matched = TRUE;
				break;
			}
		if (!matched && accept_word(p, L"is")) {
			e = binary(p, L"is", e, concatenation(p));
			matched = TRUE;
		}
		if (!matched)
			return e;
	}
}

static expr *negation(parser *p)
{
	if (accept_word(p, L"not")) {
		expr *e = new_expr(p, E_UNARY);
		lstrcpyW(e->op, L"not");
		return (e->left = negation(p)) ? e : NULL;
	}
	return comparison(p);
}

static expr *logic(parser *p, UINT level)
{
	static const WCHAR *const ops[] = { L"imp", L"eqv", L"xor", L"or", L"and" };
	if (level == ARRAYSIZE(ops))
		return negation(p);
	expr *e = logic(p, level + 1);
	while (e && accept_word(p, ops[level]))
		e = binary(p, ops[level], e, logic(p, level + 1));
	return e;
}

static expr *expression(parser *p) { return logic(p, 0); }

/* Statements. */

static stmt *new_stmt(parser *p, stmt_kind kind)
{
	stmt *s = own(p, sizeof(stmt));
	s->kind = kind;
	s->line = peek(p)->line;
	return s;
}

static void add_stmt(parser *p, block *b, stmt *s)
{
	struct stmt **items = own(p, (b->count + 1) * sizeof(stmt *));
	for (UINT i = 0; i < b->count; i++)
		items[i] = b->items[i];
	items[b->count++] = s;
	b->items = items;
}

static void skip_lines(parser *p)
{
	while (peek(p)->kind == T_LINE)
		advance(p);
}

static BOOL end_of_statement(parser *p)
{
	if (peek(p)->kind == T_LINE || peek(p)->kind == T_END || is_word(p, L"else"))
		return TRUE;
	return fail(p, L"Expected end of statement");
}

static BOOL statement(parser *p, block *b, BOOL single_line);

/* Statements up to one of the words that end the block, which stays unread. */
static BOOL statements(parser *p, block *b, const WCHAR *const *ends)
{
	for (;;) {
		skip_lines(p);
		if (peek(p)->kind == T_END)
			return fail(p, L"Expected 'End'");
		for (UINT i = 0; ends[i]; i++) {
			const WCHAR *end = ends[i];
			/* "end if" and friends are two words; others are one. */
			if (!wcsncmp(end, L"end ", 4)) {
				if (is_word(p, L"end") && is_word_at(p, 1, end + 4))
					return TRUE;
			} else if (is_word(p, end)) {
				return TRUE;
			}
		}
		if (!statement(p, b, FALSE))
			return FALSE;
	}
}

static BOOL expect_end(parser *p, const WCHAR *what)
{
	if (!accept_word(p, L"end") || !accept_word(p, what))
		return fail(p, L"Expected 'End'");
	return end_of_statement(p);
}

static void add_branch(parser *p, stmt *s, expr *condition)
{
	expr **conditions = own(p, (s->branch_count + 1) * sizeof(expr *));
	block *blocks = own(p, (s->branch_count + 1) * sizeof(block));
	for (UINT i = 0; i < s->branch_count; i++) {
		conditions[i] = s->conditions[i];
		blocks[i] = s->blocks[i];
	}
	conditions[s->branch_count++] = condition;
	s->conditions = conditions;
	s->blocks = blocks;
}

static BOOL parse_if(parser *p, block *b)
{
	stmt *s = new_stmt(p, S_IF);
	expr *condition = expression(p);
	if (!condition)
		return FALSE;
	if (!accept_word(p, L"then"))
		return fail(p, L"Expected 'Then'");
	add_branch(p, s, condition);
	if (peek(p)->kind != T_LINE || peek(p)->colon) {
		/* The one-line form: statements separated by colons, then perhaps Else and more, to the end of the line. */
		for (;;) {
			if (!statement(p, &s->blocks[0], TRUE))
				return FALSE;
			if (peek(p)->kind == T_LINE && peek(p)->colon) {
				advance(p);
				if (peek(p)->kind == T_LINE || peek(p)->kind == T_END)
					break;
				continue;
			}
			break;
		}
		if (accept_word(p, L"else")) {
			s->has_otherwise = TRUE;
			for (;;) {
				if (!statement(p, &s->otherwise, TRUE))
					return FALSE;
				if (peek(p)->kind == T_LINE && peek(p)->colon) {
					advance(p);
					if (peek(p)->kind == T_LINE || peek(p)->kind == T_END)
						break;
					continue;
				}
				break;
			}
		}
		/* An End If may close even the one-line form. */
		if (is_word(p, L"end") && is_word_at(p, 1, L"if")) {
			advance(p);
			advance(p);
		}
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	static const WCHAR *const ends[] = { L"elseif", L"else", L"end if", NULL };
	if (!statements(p, &s->blocks[0], ends))
		return FALSE;
	for (;;) {
		if (accept_word(p, L"elseif")) {
			expr *next = expression(p);
			if (!next)
				return FALSE;
			if (!accept_word(p, L"then"))
				return fail(p, L"Expected 'Then'");
			add_branch(p, s, next);
			if (!statements(p, &s->blocks[s->branch_count - 1], ends))
				return FALSE;
		} else if (accept_word(p, L"else")) {
			static const WCHAR *const last[] = { L"end if", NULL };
			s->has_otherwise = TRUE;
			if (!statements(p, &s->otherwise, last))
				return FALSE;
		} else {
			break;
		}
	}
	add_stmt(p, b, s);
	return expect_end(p, L"if");
}

/* Names with optional bounds, for Dim, ReDim, Public and Private. */
static BOOL declarations(parser *p, stmt *s, BOOL with_bounds_required)
{
	for (;;) {
		WCHAR *n = name(p);
		if (!n)
			return FALSE;
		UINT index = s->name_count++;
		WCHAR **names = own(p, s->name_count * sizeof(WCHAR *));
		expr ***bounds = own(p, s->name_count * sizeof(expr **));
		UINT *counts = own(p, s->name_count * sizeof(UINT));
		BOOL *arrays = own(p, s->name_count * sizeof(BOOL));
		for (UINT i = 0; i < index; i++) {
			names[i] = s->names[i];
			bounds[i] = s->bounds[i];
			counts[i] = s->bound_counts[i];
			arrays[i] = s->arrays[i];
		}
		names[index] = n;
		s->names = names;
		s->bounds = bounds;
		s->bound_counts = counts;
		s->arrays = arrays;
		if (accept_symbol(p, L"(")) {
			arrays[index] = TRUE;
			expr call = { 0 };
			if (!arguments(p, &call))
				return FALSE;
			for (UINT i = 0; i < call.arg_count; i++)
				if (!call.args[i])
					return fail(p, L"Expected expression");
			bounds[index] = call.args;
			counts[index] = call.arg_count;
		} else if (with_bounds_required) {
			return fail(p, L"Expected '('");
		}
		if (!accept_symbol(p, L","))
			return TRUE;
	}
}

static BOOL parse_const(parser *p, block *b)
{
	stmt *s = new_stmt(p, S_CONST);
	for (;;) {
		WCHAR *n = name(p);
		if (!n || !accept_symbol(p, L"="))
			return n ? fail(p, L"Expected '='") : FALSE;
		expr *v = expression(p);
		if (!v)
			return FALSE;
		UINT index = s->name_count++;
		WCHAR **names = own(p, s->name_count * sizeof(WCHAR *));
		expr **values = own(p, s->name_count * sizeof(expr *));
		for (UINT i = 0; i < index; i++) {
			names[i] = s->names[i];
			values[i] = s->conditions[i];
		}
		names[index] = n;
		values[index] = v;
		s->names = names;
		s->conditions = values;
		if (!accept_symbol(p, L","))
			break;
	}
	add_stmt(p, b, s);
	return end_of_statement(p);
}

static BOOL parse_procedure(parser *p, procedure_kind kind, BOOL is_private, BOOL is_default, procedure **out)
{
	procedure *proc = own(p, sizeof(procedure));
	proc->kind = kind;
	proc->is_private = is_private;
	proc->is_default = is_default;
	if (!(proc->name = name(p)))
		return FALSE;
	if (accept_symbol(p, L"(") && !accept_symbol(p, L")")) {
		for (;;) {
			BOOL by_value = FALSE;
			if (accept_word(p, L"byval"))
				by_value = TRUE;
			else
				accept_word(p, L"byref");
			WCHAR *param = name(p);
			if (!param)
				return FALSE;
			if (accept_symbol(p, L"(") && !accept_symbol(p, L")"))
				return fail(p, L"Expected ')'");
			UINT index = proc->param_count++;
			WCHAR **params = own(p, proc->param_count * sizeof(WCHAR *));
			BOOL *values = own(p, proc->param_count * sizeof(BOOL));
			for (UINT i = 0; i < index; i++) {
				params[i] = proc->params[i];
				values[i] = proc->by_value[i];
			}
			params[index] = param;
			values[index] = by_value;
			proc->params = params;
			proc->by_value = values;
			if (accept_symbol(p, L")"))
				break;
			if (!accept_symbol(p, L","))
				return fail(p, L"Expected ')'");
		}
	}
	if (!end_of_statement(p))
		return FALSE;
	const WCHAR *end = kind == P_SUB ? L"end sub" : kind == P_FUNCTION ? L"end function" : L"end property";
	const WCHAR *const ends[] = { end, NULL };
	if (!statements(p, &proc->body, ends))
		return FALSE;
	if (!expect_end(p, end + 4))
		return FALSE;
	*out = proc;
	return TRUE;
}

static void add_procedure(parser *p, procedure ***list, UINT *count, procedure *proc)
{
	procedure **items = own(p, (*count + 1) * sizeof(procedure *));
	for (UINT i = 0; i < *count; i++)
		items[i] = (*list)[i];
	items[(*count)++] = proc;
	*list = items;
}

/* Sub, Function or Property after its modifiers; FALSE with no error when the words are not one. */
static BOOL procedure_start(parser *p, procedure_kind *kind)
{
	if (accept_word(p, L"sub"))
		*kind = P_SUB;
	else if (accept_word(p, L"function"))
		*kind = P_FUNCTION;
	else if (is_word(p, L"property")) {
		advance(p);
		if (accept_word(p, L"get"))
			*kind = P_GET;
		else if (accept_word(p, L"let"))
			*kind = P_LET;
		else if (accept_word(p, L"set"))
			*kind = P_SET;
		else
			return fail(p, L"Expected 'Get', 'Let' or 'Set'");
	} else
		return FALSE;
	return TRUE;
}

static BOOL parse_class(parser *p)
{
	class_def *c = own(p, sizeof(class_def));
	if (!(c->name = name(p)) || !end_of_statement(p))
		return FALSE;
	p->in_class = TRUE;
	for (;;) {
		skip_lines(p);
		if (is_word(p, L"end") && is_word_at(p, 1, L"class"))
			break;
		BOOL is_private = FALSE, is_public = FALSE, is_default = FALSE, field = FALSE;
		if (accept_word(p, L"private"))
			is_private = TRUE;
		else if (accept_word(p, L"public"))
			is_public = TRUE, is_default = accept_word(p, L"default");
		else if (accept_word(p, L"dim"))
			field = TRUE;
		procedure_kind kind;
		if (!field && procedure_start(p, &kind)) {
			procedure *proc;
			if (!parse_procedure(p, kind, is_private, is_default, &proc))
				return FALSE;
			add_procedure(p, &c->procedures, &c->procedure_count, proc);
			continue;
		}
		if (p->error->text[0])
			return FALSE;
		if (!field && !is_private && !is_public)
			return fail(p, L"Expected 'End'");
		stmt fields = { 0 };
		if (!declarations(p, &fields, FALSE))
			return FALSE;
		UINT start = c->field_count;
		c->field_count += fields.name_count;
		WCHAR **names = own(p, c->field_count * sizeof(WCHAR *));
		expr ***bounds = own(p, c->field_count * sizeof(expr **));
		UINT *counts = own(p, c->field_count * sizeof(UINT));
		BOOL *arrays = own(p, c->field_count * sizeof(BOOL)), *privates = own(p, c->field_count * sizeof(BOOL));
		for (UINT i = 0; i < start; i++) {
			names[i] = c->fields[i];
			bounds[i] = c->field_bounds[i];
			counts[i] = c->field_bound_counts[i];
			arrays[i] = c->field_arrays[i];
			privates[i] = c->field_private[i];
		}
		for (UINT i = 0; i < fields.name_count; i++) {
			names[start + i] = fields.names[i];
			bounds[start + i] = fields.bounds[i];
			counts[start + i] = fields.bound_counts[i];
			arrays[start + i] = fields.arrays[i];
			privates[start + i] = is_private;
		}
		c->fields = names;
		c->field_bounds = bounds;
		c->field_bound_counts = counts;
		c->field_arrays = arrays;
		c->field_private = privates;
		if (!end_of_statement(p))
			return FALSE;
	}
	p->in_class = FALSE;
	if (!expect_end(p, L"class"))
		return FALSE;
	class_def **classes = own(p, (p->code->class_count + 1) * sizeof(class_def *));
	for (UINT i = 0; i < p->code->class_count; i++)
		classes[i] = p->code->classes[i];
	classes[p->code->class_count++] = c;
	p->code->classes = classes;
	return TRUE;
}

/* The While or Until of a Do loop, at its start or its end, into the loop's one branch. */
static BOOL loop_condition(parser *p, stmt *s)
{
	if (accept_word(p, L"while") || (is_word(p, L"until") && (s->until = TRUE, advance(p), TRUE)))
		return (s->conditions[0] = expression(p)) != NULL;
	return TRUE;
}

/* A call written as a statement: a name or member, with its arguments in parentheses or after it without them. */
static BOOL call_statement(parser *p, block *b, expr *target, UINT last_call, BOOL explicit_call)
{
	stmt *s = new_stmt(p, S_CALL);
	expr *call;
	if (at_statement_end(p)) {
		if (target->kind == E_CALL) {
			call = target;
		} else {
			call = new_expr(p, E_CALL);
			call->target = target;
		}
	} else {
		if (explicit_call)
			return fail(p, L"Expected end of statement");
		/* f (a), b and f (a) + 1: the parenthesis began the first argument, not an argument list. */
		if (target->kind == E_CALL && last_call != (UINT)-1) {
			p->at = last_call;
			target = target->target;
		}
		call = new_expr(p, E_CALL);
		call->target = target;
		for (;;) {
			if (is_symbol(p, L",")) {
				add_arg(p, call, NULL);
			} else {
				expr *arg = expression(p);
				if (!arg)
					return FALSE;
				add_arg(p, call, arg);
			}
			if (!accept_symbol(p, L","))
				break;
			if (at_statement_end(p))
				add_arg(p, call, NULL);
		}
	}
	s->target = call;
	add_stmt(p, b, s);
	return end_of_statement(p);
}

static BOOL statement(parser *p, block *b, BOOL single_line)
{
	if (accept_word(p, L"if"))
		return parse_if(p, b);
	if (accept_word(p, L"dim") || ((is_word(p, L"private") || is_word(p, L"public")) && !is_word_at(p, 1, L"sub") &&
		!is_word_at(p, 1, L"function") && !is_word_at(p, 1, L"property") && !is_word_at(p, 1, L"default") && !is_word_at(p, 1, L"const") &&
		(advance(p), TRUE))) {
		stmt *s = new_stmt(p, S_DIM);
		if (!declarations(p, s, FALSE))
			return FALSE;
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	if (accept_word(p, L"redim")) {
		stmt *s = new_stmt(p, S_REDIM);
		s->preserve = accept_word(p, L"preserve");
		if (!declarations(p, s, TRUE))
			return FALSE;
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	if (accept_word(p, L"erase")) {
		stmt *s = new_stmt(p, S_ERASE);
		if (!declarations(p, s, FALSE))
			return FALSE;
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	if ((is_word(p, L"private") || is_word(p, L"public")) && is_word_at(p, 1, L"const"))
		advance(p);
	if (accept_word(p, L"const"))
		return parse_const(p, b);
	if (!single_line && b == &p->code->main) {
		BOOL is_private = FALSE, is_default = FALSE;
		UINT start = p->at;
		if (accept_word(p, L"private"))
			is_private = TRUE;
		else if (accept_word(p, L"public"))
			is_default = accept_word(p, L"default");
		procedure_kind kind;
		if (procedure_start(p, &kind)) {
			procedure *proc;
			if (!parse_procedure(p, kind, is_private, is_default, &proc))
				return FALSE;
			add_procedure(p, &p->code->procedures, &p->code->procedure_count, proc);
			return TRUE;
		}
		if (p->error->text[0])
			return FALSE;
		p->at = start;
		if (accept_word(p, L"class"))
			return parse_class(p);
		if (accept_word(p, L"option")) {
			if (!accept_word(p, L"explicit"))
				return fail(p, L"Expected 'Explicit'");
			p->code->explicit_names = TRUE;
			return end_of_statement(p);
		}
	}
	if (accept_word(p, L"for")) {
		if (accept_word(p, L"each")) {
			stmt *s = new_stmt(p, S_FOR_EACH);
			if (!(s->name = name(p)))
				return FALSE;
			if (!accept_word(p, L"in"))
				return fail(p, L"Expected 'In'");
			if (!(s->value = expression(p)))
				return FALSE;
			add_branch(p, s, NULL);
			static const WCHAR *const ends[] = { L"next", NULL };
			if (!statements(p, &s->blocks[0], ends))
				return FALSE;
			advance(p);
			if (is_name(p))
				advance(p);
			add_stmt(p, b, s);
			return end_of_statement(p);
		}
		stmt *s = new_stmt(p, S_FOR);
		if (!(s->name = name(p)))
			return FALSE;
		if (!accept_symbol(p, L"="))
			return fail(p, L"Expected '='");
		if (!(s->value = expression(p)))
			return FALSE;
		if (!accept_word(p, L"to"))
			return fail(p, L"Expected 'To'");
		if (!(s->to = expression(p)))
			return FALSE;
		if (accept_word(p, L"step") && !(s->step = expression(p)))
			return FALSE;
		add_branch(p, s, NULL);
		static const WCHAR *const ends[] = { L"next", NULL };
		if (!statements(p, &s->blocks[0], ends))
			return FALSE;
		advance(p);
		if (is_name(p))
			advance(p);
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	if (accept_word(p, L"do")) {
		stmt *s = new_stmt(p, S_DO);
		s->test_first = is_word(p, L"while") || is_word(p, L"until");
		add_branch(p, s, NULL);
		if (!loop_condition(p, s))
			return FALSE;
		static const WCHAR *const ends[] = { L"loop", NULL };
		if (!statements(p, &s->blocks[0], ends))
			return FALSE;
		advance(p);
		if (!s->test_first && !loop_condition(p, s))
			return FALSE;
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	if (accept_word(p, L"while")) {
		stmt *s = new_stmt(p, S_WHILE);
		expr *condition = expression(p);
		if (!condition)
			return FALSE;
		add_branch(p, s, condition);
		static const WCHAR *const ends[] = { L"wend", NULL };
		if (!statements(p, &s->blocks[0], ends))
			return FALSE;
		advance(p);
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	if (accept_word(p, L"select")) {
		if (!accept_word(p, L"case"))
			return fail(p, L"Expected 'Case'");
		stmt *s = new_stmt(p, S_SELECT);
		if (!(s->value = expression(p)) || !end_of_statement(p))
			return FALSE;
		skip_lines(p);
		while (accept_word(p, L"case")) {
			static const WCHAR *const ends[] = { L"case", L"end select", NULL };
			if (accept_word(p, L"else")) {
				s->has_otherwise = TRUE;
				if (!statements(p, &s->otherwise, ends))
					return FALSE;
				continue;
			}
			add_branch(p, s, NULL);
			UINT index = s->branch_count - 1;
			expr ***values = own(p, s->branch_count * sizeof(expr **));
			UINT *counts = own(p, s->branch_count * sizeof(UINT));
			for (UINT i = 0; i < index; i++) {
				values[i] = s->case_values[i];
				counts[i] = s->case_counts[i];
			}
			expr list = { 0 };
			for (;;) {
				expr *v = expression(p);
				if (!v)
					return FALSE;
				add_arg(p, &list, v);
				if (!accept_symbol(p, L","))
					break;
			}
			values[index] = list.args;
			counts[index] = list.arg_count;
			s->case_values = values;
			s->case_counts = counts;
			if (!statements(p, &s->blocks[index], ends))
				return FALSE;
		}
		add_stmt(p, b, s);
		return expect_end(p, L"select");
	}
	if (accept_word(p, L"with")) {
		stmt *s = new_stmt(p, S_WITH);
		if (!(s->value = expression(p)))
			return FALSE;
		add_branch(p, s, NULL);
		p->with_depth++;
		static const WCHAR *const ends[] = { L"end with", NULL };
		BOOL fine = statements(p, &s->blocks[0], ends);
		p->with_depth--;
		if (!fine)
			return FALSE;
		add_stmt(p, b, s);
		return expect_end(p, L"with");
	}
	if (accept_word(p, L"exit")) {
		stmt *s = new_stmt(p, S_EXIT);
		if (peek(p)->kind != T_NAME)
			return fail(p, L"Expected 'Do', 'For', 'Sub', 'Function' or 'Property'");
		static const WCHAR *const kinds[] = { L"do", L"for", L"sub", L"function", L"property", NULL };
		for (UINT i = 0; kinds[i]; i++)
			if (accept_word(p, kinds[i]))
				lstrcpyW(s->exit_kind, kinds[i]);
		if (!s->exit_kind[0])
			return fail(p, L"Expected 'Do', 'For', 'Sub', 'Function' or 'Property'");
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	if (accept_word(p, L"on")) {
		stmt *s = new_stmt(p, S_ON_ERROR);
		if (!accept_word(p, L"error"))
			return fail(p, L"Expected 'Error'");
		if (accept_word(p, L"resume")) {
			if (!accept_word(p, L"next"))
				return fail(p, L"Expected 'Next'");
			s->resume_next = TRUE;
		} else if (accept_word(p, L"goto")) {
			if (peek(p)->kind != T_NUMBER)
				return fail(p, L"Expected '0'");
			advance(p);
		} else {
			return fail(p, L"Expected 'Resume' or 'GoTo'");
		}
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	if (accept_word(p, L"randomize")) {
		stmt *s = new_stmt(p, S_RANDOMIZE);
		if (!at_statement_end(p) && !(s->value = expression(p)))
			return FALSE;
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	if (accept_word(p, L"stop"))
		return end_of_statement(p);
	BOOL is_set = accept_word(p, L"set");
	if (!is_set)
		accept_word(p, L"let");
	BOOL explicit_call = !is_set && accept_word(p, L"call");
	expr *target;
	UINT last_call = (UINT)-1;
	if (is_symbol(p, L".") && p->with_depth) {
		target = postfix(p, new_expr(p, E_WITH), &last_call);
	} else if (accept_word(p, L"me")) {
		target = postfix(p, new_expr(p, E_ME), &last_call);
	} else {
		if (!is_name(p))
			return fail(p, L"Expected statement");
		target = new_expr(p, E_NAME);
		target->name = take_text(p, advance(p));
		target = postfix(p, target, &last_call);
	}
	if (!target)
		return FALSE;
	if (is_set || (!explicit_call && is_symbol(p, L"="))) {
		if (!accept_symbol(p, L"="))
			return fail(p, L"Expected '='");
		stmt *s = new_stmt(p, is_set ? S_SET : S_ASSIGN);
		s->target = target;
		if (is_set && accept_word(p, L"nothing"))
			s->value = new_expr(p, E_NOTHING);
		else if (!(s->value = expression(p)))
			return FALSE;
		add_stmt(p, b, s);
		return end_of_statement(p);
	}
	return call_statement(p, b, target, last_call, explicit_call);
}

script_code *vbs_parse(const WCHAR *source, BOOL is_expression, parse_error *error)
{
	*error = (parse_error){ 0 };
	parser p = { .error = error };
	p.code = vbs_alloc(sizeof(script_code));
	p.code->source = vbs_copy(source);
	if (!vbs_tokenize(source, &p.tokens, &error->line, &error->column)) {
		lstrcpyW(error->text, L"Invalid character");
		error->number = 1032;
		vbs_tokens_free(&p.tokens);
		vbs_code_free(p.code);
		return NULL;
	}
	BOOL fine = TRUE;
	if (is_expression) {
		skip_lines(&p);
		fine = (p.code->expression = expression(&p)) != NULL;
		skip_lines(&p);
		if (fine && peek(&p)->kind != T_END)
			fine = fail(&p, L"Expected end of statement");
	} else {
		for (;;) {
			skip_lines(&p);
			if (peek(&p)->kind == T_END)
				break;
			if (!(fine = statement(&p, &p.code->main, FALSE)))
				break;
		}
	}
	vbs_tokens_free(&p.tokens);
	if (!fine) {
		vbs_code_free(p.code);
		return NULL;
	}
	return p.code;
}

static void free_constants(block *b);

static void free_expr(expr *e)
{
	if (!e)
		return;
	if (e->kind == E_CONSTANT)
		VariantClear(&e->constant);
	free_expr(e->target);
	free_expr(e->left);
	free_expr(e->right);
	for (UINT i = 0; i < e->arg_count; i++)
		free_expr(e->args[i]);
}

static void free_stmt(stmt *s)
{
	free_expr(s->target);
	free_expr(s->value);
	free_expr(s->to);
	free_expr(s->step);
	for (UINT i = 0; i < s->branch_count; i++) {
		if (s->conditions)
			free_expr(s->conditions[i]);
		if (s->blocks)
			free_constants(&s->blocks[i]);
		if (s->case_values)
			for (UINT j = 0; j < s->case_counts[i]; j++)
				free_expr(s->case_values[i][j]);
	}
	if (s->kind == S_CONST)
		for (UINT i = 0; i < s->name_count; i++)
			free_expr(s->conditions[i]);
	if (s->bounds)
		for (UINT i = 0; i < s->name_count; i++)
			for (UINT j = 0; j < s->bound_counts[i]; j++)
				free_expr(s->bounds[i][j]);
	free_constants(&s->otherwise);
}

/* The string constants are BSTRs, which only VariantClear may free; the nodes themselves go with the owned list. */
static void free_constants(block *b)
{
	for (UINT i = 0; i < b->count; i++)
		free_stmt(b->items[i]);
}

void vbs_code_free(script_code *code)
{
	if (!code)
		return;
	free_constants(&code->main);
	free_expr(code->expression);
	for (UINT i = 0; i < code->procedure_count; i++)
		free_constants(&code->procedures[i]->body);
	for (UINT i = 0; i < code->class_count; i++) {
		class_def *c = code->classes[i];
		for (UINT j = 0; j < c->procedure_count; j++)
			free_constants(&c->procedures[j]->body);
		for (UINT j = 0; j < c->field_count; j++)
			for (UINT k = 0; k < c->field_bound_counts[j]; k++)
				free_expr(c->field_bounds[j][k]);
	}
	for (UINT i = 0; i < code->owned_count; i++)
		vbs_free(code->owned[i]);
	vbs_free(code->owned);
	vbs_free(code->source);
	vbs_free(code);
}
