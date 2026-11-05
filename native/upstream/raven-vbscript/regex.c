/* SPDX-License-Identifier: MIT */
#include "regex.h"

/* A pattern compiles to a graph of nodes, each pointing at the one that follows it. Branch points (alternation, quantifiers,
 * lookahead) try their choices in order and back out of a choice that fails; plain steps advance in a loop. A quantifier over one
 * character test counts the characters it can take and backs off one at a time, so long runs do not deepen the recursion. */

typedef enum {
	N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_WORD_BOUNDARY, N_NOT_WORD_BOUNDARY, N_GROUP_START, N_GROUP_END, N_BACKREF,
	N_ALT, N_REPEAT, N_REPEAT_END, N_LOOK, N_LOOK_END, N_DONE,
} node_kind;

typedef struct {
	WCHAR low, high;
} char_range;

typedef struct node node;
struct node {
	node_kind kind;
	node *next;
	WCHAR c;
	BOOL negate;
	char_range *ranges;
	UINT range_count;
	UINT index;
	node **branches;
	UINT branch_count;
	node *body;
	UINT min, max;
	BOOL greedy, simple;
	node *owner;
};

struct rx {
	node **nodes;
	UINT node_count;
	node *start;
	UINT groups, repeats;
	DWORD flags;
};

#define UNBOUNDED 0xffffffffu
#define MAX_DEPTH 3000

static void *rx_alloc(SIZE_T size) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1); }
static void rx_release(void *block) { if (block) HeapFree(GetProcessHeap(), 0, block); }

static void *rx_grow(void *block, SIZE_T size)
{
	return block ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, block, size) : rx_alloc(size);
}

/* Parsing. */

typedef struct {
	rx *re;
	const WCHAR *at, *end;
	UINT total_groups;
	LONG error;
} parser;

typedef struct {
	node *head, *tail;
} fragment;

static node *new_node(parser *p, node_kind kind)
{
	rx *re = p->re;
	if (!(re->node_count & 63)) {
		node **grown = rx_grow(re->nodes, (re->node_count + 64) * sizeof(node *));
		if (!grown)
			return NULL;
		re->nodes = grown;
	}
	node *n = rx_alloc(sizeof(node));
	if (n) {
		n->kind = kind;
		re->nodes[re->node_count++] = n;
	}
	return n;
}

static fragment single(node *n) { return (fragment){ n, n }; }

static BOOL add_range(node *n, WCHAR low, WCHAR high)
{
	char_range *grown = rx_grow(n->ranges, (n->range_count + 1) * sizeof(char_range));
	if (!grown)
		return FALSE;
	n->ranges = grown;
	n->ranges[n->range_count++] = (char_range){ low, high };
	return TRUE;
}

static const char_range digit_ranges[] = { { L'0', L'9' } };
static const char_range word_ranges[] = { { L'0', L'9' }, { L'A', L'Z' }, { L'_', L'_' }, { L'a', L'z' } };
static const char_range space_ranges[] = {
	{ 0x09, 0x0d }, { 0x20, 0x20 }, { 0xa0, 0xa0 }, { 0x1680, 0x1680 }, { 0x2000, 0x200a }, { 0x2028, 0x2029 }, { 0x202f, 0x202f },
	{ 0x205f, 0x205f }, { 0x3000, 0x3000 }, { 0xfeff, 0xfeff },
};

/* Adds a set, or everything outside it (the ranges are sorted and apart). */
static BOOL add_set(node *n, const char_range *set, UINT count, BOOL outside)
{
	if (!outside) {
		for (UINT i = 0; i < count; i++)
			if (!add_range(n, set[i].low, set[i].high))
				return FALSE;
		return TRUE;
	}
	UINT from = 0;
	for (UINT i = 0; i < count; i++) {
		if (set[i].low > from && !add_range(n, (WCHAR)from, set[i].low - 1))
			return FALSE;
		from = set[i].high + 1u;
	}
	return from > 0xffff || add_range(n, (WCHAR)from, 0xffff);
}

/* The class escapes: \d \D \w \W \s \S; FALSE when c is not one. */
static BOOL class_escape(node *n, WCHAR c, BOOL *failed)
{
	const char_range *set;
	UINT count;
	switch (c | 0x20) {
	case L'd': set = digit_ranges, count = ARRAYSIZE(digit_ranges); break;
	case L'w': set = word_ranges, count = ARRAYSIZE(word_ranges); break;
	case L's': set = space_ranges, count = ARRAYSIZE(space_ranges); break;
	default: return FALSE;
	}
	*failed = !add_set(n, set, count, c >= L'A' && c <= L'Z');
	return TRUE;
}

static int hex_digit(WCHAR c)
{
	if (c >= L'0' && c <= L'9')
		return c - L'0';
	if ((c | 0x20) >= L'a' && (c | 0x20) <= L'f')
		return (c | 0x20) - L'a' + 10;
	return -1;
}

/* A character escape after the backslash: \n \r \t \f \v \0 \xhh \uhhhh \cX, or the character itself. */
static WCHAR char_escape(parser *p, BOOL in_class)
{
	WCHAR c = *p->at++;
	switch (c) {
	case L'n': return L'\n';
	case L'r': return L'\r';
	case L't': return L'\t';
	case L'f': return L'\f';
	case L'v': return L'\v';
	case L'b': return in_class ? L'\b' : c;
	case L'0': return 0;
	case L'c':
		if (p->at < p->end && ((*p->at | 0x20) >= L'a' && (*p->at | 0x20) <= L'z'))
			return *p->at++ & 0x1f;
		p->at--;
		return L'\\';
	case L'x':
	case L'u': {
		int digits = c == L'x' ? 2 : 4, value = 0;
		const WCHAR *from = p->at;
		for (int i = 0; i < digits; i++) {
			int d = p->at < p->end ? hex_digit(*p->at) : -1;
			if (d < 0) {
				p->at = from;
				return c;
			}
			value = value * 16 + d;
			p->at++;
		}
		return (WCHAR)value;
	}
	}
	return c;
}

static fragment parse_alternation(parser *p);

static fragment fail(parser *p, LONG error)
{
	if (!p->error)
		p->error = error;
	return (fragment){ NULL, NULL };
}

static fragment parse_class(parser *p)
{
	node *n = new_node(p, N_CLASS);
	if (!n)
		return fail(p, RX_E_SYNTAX);
	if (p->at < p->end && *p->at == L'^') {
		n->negate = TRUE;
		p->at++;
	}
	while (p->at < p->end && *p->at != L']') {
		WCHAR low;
		if (*p->at == L'\\' && p->at + 1 < p->end) {
			p->at++;
			BOOL failed = FALSE;
			if (class_escape(n, *p->at, &failed)) {
				p->at++;
				if (failed)
					return fail(p, RX_E_SYNTAX);
				continue;
			}
			low = char_escape(p, TRUE);
		} else
			low = *p->at++;
		WCHAR high = low;
		if (p->at + 1 < p->end && *p->at == L'-' && p->at[1] != L']') {
			p->at++;
			if (*p->at == L'\\' && p->at + 1 < p->end) {
				p->at++;
				high = char_escape(p, TRUE);
			} else
				high = *p->at++;
			if (high < low)
				return fail(p, RX_E_SYNTAX);
		}
		if (!add_range(n, low, high))
			return fail(p, RX_E_SYNTAX);
	}
	if (p->at >= p->end)
		return fail(p, RX_E_BRACKET);
	p->at++;
	return single(n);
}

static fragment parse_group(parser *p)
{
	node_kind kind = N_GROUP_START;
	BOOL negative = FALSE;
	if (p->at + 1 < p->end && *p->at == L'?') {
		WCHAR how = p->at[1];
		if (how == L':')
			kind = N_EMPTY;
		else if (how == L'=' || how == L'!') {
			kind = N_LOOK;
			negative = how == L'!';
		} else
			return fail(p, RX_E_SYNTAX);
		p->at += 2;
	}
	UINT index = kind == N_GROUP_START ? ++p->re->groups : 0;
	fragment inner = parse_alternation(p);
	if (!inner.head)
		return inner;
	if (p->at >= p->end || *p->at != L')')
		return fail(p, RX_E_PARENTHESIS);
	p->at++;
	if (kind == N_EMPTY)
		return inner;
	if (kind == N_LOOK) {
		node *look = new_node(p, N_LOOK), *end = new_node(p, N_LOOK_END);
		if (!look || !end)
			return fail(p, RX_E_SYNTAX);
		look->negate = negative;
		look->body = inner.head;
		inner.tail->next = end;
		return single(look);
	}
	node *open = new_node(p, N_GROUP_START), *close = new_node(p, N_GROUP_END);
	if (!open || !close)
		return fail(p, RX_E_SYNTAX);
	open->index = close->index = index;
	open->next = inner.head;
	inner.tail->next = close;
	return (fragment){ open, close };
}

static fragment parse_atom(parser *p)
{
	WCHAR c = *p->at++;
	node *n;
	switch (c) {
	case L'(':
		return parse_group(p);
	case L'[':
		return parse_class(p);
	case L'.':
		n = new_node(p, N_ANY);
		break;
	case L'^':
		n = new_node(p, N_BOL);
		break;
	case L'$':
		n = new_node(p, N_EOL);
		break;
	case L'*':
	case L'+':
	case L'?':
		return fail(p, RX_E_SYNTAX);
	case L'\\': {
		if (p->at >= p->end) {
			n = new_node(p, N_CHAR);
			if (n)
				n->c = L'\\';
			break;
		}
		WCHAR e = *p->at;
		BOOL failed = FALSE;
		if (e == L'b' || e == L'B') {
			p->at++;
			n = new_node(p, e == L'b' ? N_WORD_BOUNDARY : N_NOT_WORD_BOUNDARY);
			break;
		}
		if (e >= L'1' && e <= L'9') {
			UINT number = 0;
			const WCHAR *from = p->at;
			while (p->at < p->end && *p->at >= L'0' && *p->at <= L'9' && number * 10 + (*p->at - L'0') <= p->total_groups)
				number = number * 10 + (*p->at++ - L'0');
			if (number) {
				n = new_node(p, N_BACKREF);
				if (n)
					n->index = number;
				break;
			}
			p->at = from;
		}
		n = new_node(p, N_CLASS);
		if (n && class_escape(n, e, &failed)) {
			p->at++;
			if (failed)
				return fail(p, RX_E_SYNTAX);
			break;
		}
		if (n)
			n->kind = N_CHAR, n->c = char_escape(p, FALSE);
		break;
	}
	default:
		n = new_node(p, N_CHAR);
		if (n)
			n->c = c;
		break;
	}
	return n ? single(n) : fail(p, RX_E_SYNTAX);
}

static BOOL read_count(parser *p, UINT *out)
{
	const WCHAR *from = p->at;
	UINT value = 0;
	while (p->at < p->end && *p->at >= L'0' && *p->at <= L'9') {
		if (value < 100000)
			value = value * 10 + (*p->at - L'0');
		p->at++;
	}
	*out = value;
	return p->at > from;
}

/* A quantifier after an atom; a brace that does not make one is a literal character. */
static BOOL parse_quantifier(parser *p, UINT *min, UINT *max, BOOL *greedy)
{
	if (p->at >= p->end)
		return FALSE;
	WCHAR c = *p->at;
	if (c == L'*' || c == L'+' || c == L'?') {
		p->at++;
		*min = c == L'+' ? 1 : 0;
		*max = c == L'?' ? 1 : UNBOUNDED;
	} else if (c == L'{') {
		const WCHAR *from = p->at++;
		if (!read_count(p, min)) {
			p->at = from;
			return FALSE;
		}
		*max = *min;
		if (p->at < p->end && *p->at == L',') {
			p->at++;
			if (!read_count(p, max))
				*max = UNBOUNDED;
		}
		if (p->at >= p->end || *p->at != L'}') {
			p->at = from;
			return FALSE;
		}
		p->at++;
	} else
		return FALSE;
	*greedy = TRUE;
	if (p->at < p->end && *p->at == L'?') {
		*greedy = FALSE;
		p->at++;
	}
	return TRUE;
}

static fragment parse_sequence(parser *p)
{
	fragment whole = { NULL, NULL };
	while (p->at < p->end && *p->at != L'|' && *p->at != L')') {
		fragment atom;
		if (*p->at == L'{') {
			/* A brace that opens no quantifier stands for itself. */
			UINT min, max;
			BOOL greedy;
			const WCHAR *save = p->at;
			if (parse_quantifier(p, &min, &max, &greedy))
				return fail(p, RX_E_SYNTAX);
			p->at = save + 1;
			node *n = new_node(p, N_CHAR);
			if (!n)
				return fail(p, RX_E_SYNTAX);
			n->c = L'{';
			atom = single(n);
		} else
			atom = parse_atom(p);
		if (!atom.head)
			return atom;
		UINT min, max;
		BOOL greedy;
		if (parse_quantifier(p, &min, &max, &greedy)) {
			if (max < min)
				return fail(p, RX_E_SYNTAX);
			node *repeat = new_node(p, N_REPEAT);
			if (!repeat)
				return fail(p, RX_E_SYNTAX);
			repeat->min = min;
			repeat->max = max;
			repeat->greedy = greedy;
			repeat->body = atom.head;
			repeat->simple = atom.head == atom.tail && (atom.head->kind == N_CHAR || atom.head->kind == N_ANY || atom.head->kind == N_CLASS);
			if (!repeat->simple) {
				node *end = new_node(p, N_REPEAT_END);
				if (!end)
					return fail(p, RX_E_SYNTAX);
				end->owner = repeat;
				atom.tail->next = end;
				repeat->index = p->re->repeats++;
			}
			atom = single(repeat);
		}
		if (whole.head)
			whole.tail->next = atom.head;
		else
			whole.head = atom.head;
		whole.tail = atom.tail;
	}
	if (!whole.head) {
		node *n = new_node(p, N_EMPTY);
		return n ? single(n) : fail(p, RX_E_SYNTAX);
	}
	return whole;
}

static fragment parse_alternation(parser *p)
{
	fragment first = parse_sequence(p);
	if (!first.head || p->at >= p->end || *p->at != L'|')
		return first;
	node *alt = new_node(p, N_ALT), *join = new_node(p, N_EMPTY);
	if (!alt || !join)
		return fail(p, RX_E_SYNTAX);
	fragment branch = first;
	for (;;) {
		node **grown = rx_grow(alt->branches, (alt->branch_count + 1) * sizeof(node *));
		if (!grown)
			return fail(p, RX_E_SYNTAX);
		alt->branches = grown;
		alt->branches[alt->branch_count++] = branch.head;
		branch.tail->next = join;
		if (p->at >= p->end || *p->at != L'|')
			break;
		p->at++;
		branch = parse_sequence(p);
		if (!branch.head)
			return branch;
	}
	alt->next = join;
	return (fragment){ alt, join };
}

/* Counts the capturing groups first, so that \10 can be told apart from \1 followed by 0. */
static UINT count_groups(const WCHAR *at, const WCHAR *end)
{
	UINT count = 0;
	BOOL in_class = FALSE;
	for (; at < end; at++) {
		if (*at == L'\\') {
			at++;
			continue;
		}
		if (in_class)
			in_class = *at != L']';
		else if (*at == L'[')
			in_class = TRUE;
		else if (*at == L'(' && (at + 1 >= end || at[1] != L'?'))
			count++;
	}
	return count;
}

void rx_free(rx *re)
{
	if (!re)
		return;
	for (UINT i = 0; i < re->node_count; i++) {
		rx_release(re->nodes[i]->ranges);
		rx_release(re->nodes[i]->branches);
		rx_release(re->nodes[i]);
	}
	rx_release(re->nodes);
	rx_release(re);
}

rx *rx_compile(const WCHAR *pattern, UINT length, DWORD flags, LONG *error)
{
	rx *re = rx_alloc(sizeof(rx));
	*error = 0;
	if (!re) {
		*error = RX_E_SYNTAX;
		return NULL;
	}
	re->flags = flags;
	parser p = { re, pattern, pattern + length, count_groups(pattern, pattern + length), 0 };
	fragment whole = parse_alternation(&p);
	if (whole.head && p.at < p.end)
		whole = fail(&p, RX_E_SYNTAX);
	node *done = whole.head ? new_node(&p, N_DONE) : NULL;
	if (!done) {
		*error = p.error ? p.error : RX_E_SYNTAX;
		rx_free(re);
		return NULL;
	}
	whole.tail->next = done;
	re->start = whole.head;
	return re;
}

UINT rx_groups(const rx *re) { return re->groups; }

/* Matching. */

typedef struct {
	const rx *re;
	const WCHAR *text;
	UINT length;
	int *spans, *pending;
	UINT *counts;
	int *iteration_start;
	UINT depth;
	UINT final;
} matcher;

static WCHAR fold(WCHAR c)
{
	if (c < 0x80)
		return c >= L'A' && c <= L'Z' ? c + 0x20 : c;
	return (WCHAR)(ULONG_PTR)CharLowerW((LPWSTR)(ULONG_PTR)c);
}

static WCHAR unfold(WCHAR c)
{
	if (c < 0x80)
		return c >= L'a' && c <= L'z' ? c - 0x20 : c;
	return (WCHAR)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)c);
}

static BOOL is_line_end(WCHAR c) { return c == L'\n' || c == L'\r' || c == 0x2028 || c == 0x2029; }

static BOOL is_word(WCHAR c)
{
	return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'_';
}

static BOOL in_ranges(const node *n, WCHAR c)
{
	for (UINT i = 0; i < n->range_count; i++)
		if (c >= n->ranges[i].low && c <= n->ranges[i].high)
			return TRUE;
	return FALSE;
}

/* One character test: a literal, any character but a line end, or a class. */
static BOOL takes(const matcher *m, const node *n, WCHAR c)
{
	BOOL ignore = (m->re->flags & RX_IGNORE_CASE) != 0;
	switch (n->kind) {
	case N_CHAR:
		return c == n->c || (ignore && fold(c) == fold(n->c));
	case N_ANY:
		return !is_line_end(c);
	case N_CLASS: {
		BOOL inside = in_ranges(n, c) || (ignore && (in_ranges(n, fold(c)) || in_ranges(n, unfold(c))));
		return inside != n->negate;
	}
	default:
		return FALSE;
	}
}

static BOOL at_boundary(const matcher *m, UINT pos)
{
	BOOL before = pos > 0 && is_word(m->text[pos - 1]), after = pos < m->length && is_word(m->text[pos]);
	return before != after;
}

static BOOL match(matcher *m, const node *n, UINT pos);

static BOOL repeat_step(matcher *m, const node *r, UINT pos)
{
	UINT id = r->index, count = m->counts[id];
	int started = m->iteration_start[id];
	BOOL more = count < r->max;
	if (count >= r->min && !r->greedy && match(m, r->next, pos))
		return TRUE;
	if (count < r->min || more) {
		m->counts[id] = count + 1;
		m->iteration_start[id] = (int)pos;
		if (match(m, r->body, pos))
			return TRUE;
		m->counts[id] = count;
		m->iteration_start[id] = started;
	}
	if (count >= r->min && r->greedy)
		return match(m, r->next, pos);
	return FALSE;
}

static BOOL match(matcher *m, const node *n, UINT pos)
{
	if (++m->depth > MAX_DEPTH) {
		m->depth--;
		return FALSE;
	}
	BOOL matched = FALSE;
	for (;;) {
		switch (n->kind) {
		case N_EMPTY:
			n = n->next;
			continue;
		case N_CHAR:
		case N_ANY:
		case N_CLASS:
			if (pos >= m->length || !takes(m, n, m->text[pos]))
				goto out;
			pos++;
			n = n->next;
			continue;
		case N_BOL:
			if (pos && !((m->re->flags & RX_MULTILINE) && is_line_end(m->text[pos - 1])))
				goto out;
			n = n->next;
			continue;
		case N_EOL:
			if (pos < m->length && !((m->re->flags & RX_MULTILINE) && is_line_end(m->text[pos])))
				goto out;
			n = n->next;
			continue;
		case N_WORD_BOUNDARY:
		case N_NOT_WORD_BOUNDARY:
			if (at_boundary(m, pos) != (n->kind == N_WORD_BOUNDARY))
				goto out;
			n = n->next;
			continue;
		case N_BACKREF: {
			int from = m->spans[2 * n->index], to = m->spans[2 * n->index + 1];
			if (from >= 0 && to >= from) {
				UINT size = (UINT)(to - from);
				if (pos + size > m->length)
					goto out;
				for (UINT i = 0; i < size; i++) {
					WCHAR a = m->text[from + i], b = m->text[pos + i];
					if (a != b && !((m->re->flags & RX_IGNORE_CASE) && fold(a) == fold(b)))
						goto out;
				}
				pos += size;
			}
			n = n->next;
			continue;
		}
		case N_GROUP_START: {
			int saved = m->pending[n->index];
			m->pending[n->index] = (int)pos;
			matched = match(m, n->next, pos);
			if (!matched)
				m->pending[n->index] = saved;
			goto out;
		}
		case N_GROUP_END: {
			int from = m->spans[2 * n->index], to = m->spans[2 * n->index + 1];
			m->spans[2 * n->index] = m->pending[n->index];
			m->spans[2 * n->index + 1] = (int)pos;
			matched = match(m, n->next, pos);
			if (!matched) {
				m->spans[2 * n->index] = from;
				m->spans[2 * n->index + 1] = to;
			}
			goto out;
		}
		case N_ALT:
			for (UINT i = 0; i < n->branch_count && !matched; i++)
				matched = match(m, n->branches[i], pos);
			goto out;
		case N_REPEAT:
			if (n->simple) {
				UINT most = 0;
				while (most < n->max && pos + most < m->length && takes(m, n->body, m->text[pos + most]))
					most++;
				if (most < n->min)
					goto out;
				if (n->greedy) {
					for (UINT take = most + 1; take-- > n->min && !matched;)
						matched = match(m, n->next, pos + take);
				} else {
					for (UINT take = n->min; take <= most && !matched; take++)
						matched = match(m, n->next, pos + take);
				}
				goto out;
			} else {
				UINT saved_count = m->counts[n->index];
				int saved_start = m->iteration_start[n->index];
				m->counts[n->index] = 0;
				m->iteration_start[n->index] = -1;
				matched = repeat_step(m, n, pos);
				m->counts[n->index] = saved_count;
				m->iteration_start[n->index] = saved_start;
				goto out;
			}
		case N_REPEAT_END: {
			const node *r = n->owner;
			/* An iteration that took nothing, once the least count is reached, would go round for ever. */
			if (m->counts[r->index] > r->min && m->iteration_start[r->index] == (int)pos)
				goto out;
			matched = repeat_step(m, r, pos);
			goto out;
		}
		case N_LOOK: {
			/* What the lookahead captured stays only when it matched and is not negative; there is no backing into it. */
			SIZE_T size = 2 * (m->re->groups + 1) * sizeof(int);
			int *saved = rx_alloc(size);
			if (!saved)
				goto out;
			memcpy(saved, m->spans, size);
			BOOL found = match(m, n->body, pos);
			if (found == n->negate || n->negate)
				memcpy(m->spans, saved, size);
			if (found != n->negate) {
				matched = match(m, n->next, pos);
				if (!matched)
					memcpy(m->spans, saved, size);
			}
			rx_release(saved);
			goto out;
		}
		case N_LOOK_END:
			matched = TRUE;
			goto out;
		case N_DONE:
			m->final = pos;
			matched = TRUE;
			goto out;
		}
	}
out:
	m->depth--;
	return matched;
}

BOOL rx_search(const rx *re, const WCHAR *text, UINT length, UINT start, int *spans)
{
	UINT groups = re->groups + 1;
	matcher m = { re, text, length, spans };
	m.pending = rx_alloc(groups * sizeof(int));
	m.counts = rx_alloc((re->repeats + 1) * sizeof(UINT));
	m.iteration_start = rx_alloc((re->repeats + 1) * sizeof(int));
	BOOL found = FALSE;
	if (m.pending && m.counts && m.iteration_start) {
		BOOL anchored = re->start->kind == N_BOL && !(re->flags & RX_MULTILINE);
		for (UINT at = start; at <= length && !found; at++) {
			for (UINT i = 0; i < 2 * groups; i++)
				spans[i] = -1;
			m.depth = 0;
			if (match(&m, re->start, at)) {
				spans[0] = (int)at;
				spans[1] = (int)m.final;
				found = TRUE;
			}
			if (anchored)
				break;
		}
	}
	rx_release(m.pending);
	rx_release(m.counts);
	rx_release(m.iteration_start);
	return found;
}
