/* SPDX-License-Identifier: MIT */
#include "internal.h"

/* Regular expressions with the syntax ICU documents: a parser builds a tree, the tree becomes a small program, and a backtracking
 * machine runs it over code points. A step budget stops patterns that would run for ever, as ICU's time limit does. */

#define UREGEX_CASE_INSENSITIVE 2
#define UREGEX_COMMENTS 4
#define UREGEX_MULTILINE 8
#define UREGEX_LITERAL 16
#define UREGEX_DOTALL 32

#define U_REGEX_INVALID_CAPTURE_GROUP_NAME 0x10315
#define MAX_PROGRAM 200000
#define MAX_STEPS 40000000
#define MAX_DEPTH 200
#define REPEAT_FOREVER (-1)

typedef struct {
	INT line, offset;
	UChar pre_context[16], post_context[16];
} UParseError;

typedef enum {
	CLASS_RANGE, CLASS_DIGIT, CLASS_WORD, CLASS_SPACE, CLASS_LETTER, CLASS_UPPER, CLASS_LOWER, CLASS_NUMBER, CLASS_PUNCTUATION, CLASS_MARK, CLASS_ANY
} class_kind;

typedef struct {
	class_kind kind;
	BOOL negated;
	UChar32 low, high;
} class_item;

typedef struct {
	class_item *items;
	INT count;
	BOOL negated;
} char_set;

typedef enum {
	NODE_EMPTY, NODE_CHAR, NODE_ANY, NODE_SET, NODE_LINE_START, NODE_LINE_END, NODE_TEXT_START, NODE_TEXT_END, NODE_TEXT_END_LINE, NODE_WORD_EDGE, NODE_NOT_WORD_EDGE,
	NODE_CONCAT, NODE_ALTERNATE, NODE_CAPTURE, NODE_REPEAT, NODE_BACK_REF, NODE_LOOK
} node_kind;

typedef struct node {
	node_kind kind;
	UChar32 c;
	INT set, group, min, max;
	BOOL lazy, negated, caseless, dotall, multiline;
	struct node *left, *right;
} node;

typedef enum {
	OP_CHAR, OP_ANY, OP_SET, OP_SPLIT, OP_JUMP, OP_SAVE, OP_MARK, OP_CHECK, OP_LINE_START, OP_LINE_END, OP_TEXT_START, OP_TEXT_END, OP_TEXT_END_LINE,
	OP_WORD_EDGE, OP_NOT_WORD_EDGE, OP_BACK_REF, OP_LOOK, OP_LOOK_END, OP_MATCH
} op_kind;

typedef struct {
	op_kind op;
	INT x, y;
	BOOL caseless, flag;
} inst;

typedef struct URegularExpression {
	UChar *pattern;
	INT pattern_length;
	UINT flags;
	inst *program;
	INT program_count;
	char_set *sets;
	INT set_count;
	INT group_count, register_count;
	const UChar *text;
	UChar *owned_text;
	INT text_length;
	INT *slots;
	BOOL matched;
	INT next_start;
} URegularExpression;

typedef struct {
	const UChar *at, *end, *start;
	UINT flags;
	UErrorCode status;
	URegularExpression *regex;
	INT depth;
} parser;

static void fail(parser *p, UErrorCode code)
{
	if (!U_FAILED(p->status))
		p->status = code;
}

static node *new_node(parser *p, node_kind kind)
{
	node *item = icu_alloc(sizeof *item);
	if (!item) {
		fail(p, U_MEMORY_ALLOCATION_ERROR);
		return NULL;
	}
	item->kind = kind;
	item->caseless = (p->flags & UREGEX_CASE_INSENSITIVE) != 0;
	item->dotall = (p->flags & UREGEX_DOTALL) != 0;
	item->multiline = (p->flags & UREGEX_MULTILINE) != 0;
	return item;
}

static void free_tree(node *item)
{
	if (!item)
		return;
	free_tree(item->left);
	free_tree(item->right);
	icu_free(item);
}

static BOOL at_end(const parser *p)
{
	return p->at >= p->end;
}

static UChar32 take_char(parser *p)
{
	INT next;
	INT left = (INT)(p->end - p->at);
	UChar32 c = icu_at(p->at, left, 0, &next);
	p->at += next;
	return c;
}

static BOOL take_if(parser *p, UChar c)
{
	if (at_end(p) || *p->at != c)
		return FALSE;
	p->at++;
	return TRUE;
}

static INT add_set(parser *p)
{
	URegularExpression *regex = p->regex;
	char_set *grown = icu_alloc((regex->set_count + 1) * sizeof(char_set));
	if (!grown) {
		fail(p, U_MEMORY_ALLOCATION_ERROR);
		return 0;
	}
	if (regex->set_count)
		CopyMemory(grown, regex->sets, regex->set_count * sizeof(char_set));
	icu_free(regex->sets);
	regex->sets = grown;
	return regex->set_count++;
}

static void add_item(parser *p, INT set, class_kind kind, BOOL negated, UChar32 low, UChar32 high)
{
	char_set *target = &p->regex->sets[set];
	class_item *grown = icu_alloc((target->count + 1) * sizeof(class_item));
	if (!grown) {
		fail(p, U_MEMORY_ALLOCATION_ERROR);
		return;
	}
	if (target->count)
		CopyMemory(grown, target->items, target->count * sizeof(class_item));
	icu_free(target->items);
	target->items = grown;
	class_item *item = &grown[target->count++];
	item->kind = kind;
	item->negated = negated;
	item->low = low;
	item->high = high;
}

static INT hex_value(UChar c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static UChar32 take_hex(parser *p, INT digits)
{
	UChar32 value = 0;
	for (INT i = 0; i < digits; i++) {
		INT digit = at_end(p) ? -1 : hex_value(*p->at);
		if (digit < 0) {
			fail(p, U_REGEX_BAD_ESCAPE_SEQUENCE);
			return 0;
		}
		value = value * 16 + digit;
		p->at++;
	}
	return value;
}

static BOOL class_escape(UChar c, class_kind *kind, BOOL *negated)
{
	*negated = c >= 'A' && c <= 'Z';
	switch (c) {
	case 'd': case 'D': *kind = CLASS_DIGIT; return TRUE;
	case 'w': case 'W': *kind = CLASS_WORD; return TRUE;
	case 's': case 'S': *kind = CLASS_SPACE; return TRUE;
	default: return FALSE;
	}
}

static BOOL same_ascii(const UChar *text, INT length, const char *name)
{
	INT i = 0;
	for (; i < length && name[i]; i++)
		if (icu_lower(text[i]) != (UChar32)(unsigned char)name[i])
			return FALSE;
	return i == length && !name[i];
}

// \p{...}: the general categories the machine can tell apart without Unicode's tables.
static class_kind take_property(parser *p)
{
	const UChar *start = p->at;
	if (!take_if(p, '{')) {
		fail(p, U_REGEX_BAD_ESCAPE_SEQUENCE);
		return CLASS_ANY;
	}
	start = p->at;
	while (!at_end(p) && *p->at != '}')
		p->at++;
	if (at_end(p)) {
		fail(p, U_REGEX_BAD_ESCAPE_SEQUENCE);
		return CLASS_ANY;
	}
	INT length = (INT)(p->at - start);
	p->at++;
	if (same_ascii(start, length, "l") || same_ascii(start, length, "letter") || same_ascii(start, length, "alphabetic"))
		return CLASS_LETTER;
	if (same_ascii(start, length, "lu") || same_ascii(start, length, "uppercase"))
		return CLASS_UPPER;
	if (same_ascii(start, length, "ll") || same_ascii(start, length, "lowercase"))
		return CLASS_LOWER;
	if (same_ascii(start, length, "n") || same_ascii(start, length, "nd") || same_ascii(start, length, "number"))
		return CLASS_NUMBER;
	if (same_ascii(start, length, "p") || same_ascii(start, length, "punctuation"))
		return CLASS_PUNCTUATION;
	if (same_ascii(start, length, "m") || same_ascii(start, length, "mn") || same_ascii(start, length, "mark"))
		return CLASS_MARK;
	if (same_ascii(start, length, "z") || same_ascii(start, length, "zs") || same_ascii(start, length, "whitespace") || same_ascii(start, length, "white_space"))
		return CLASS_SPACE;
	fail(p, U_REGEX_UNIMPLEMENTED);
	return CLASS_ANY;
}

static UChar32 literal_escape(parser *p, UChar c)
{
	switch (c) {
	case 'a': return 7;
	case 'e': return 27;
	case 'f': return 12;
	case 'n': return 10;
	case 'r': return 13;
	case 't': return 9;
	case 'x':
		if (take_if(p, '{')) {
			UChar32 value = 0;
			while (!at_end(p) && *p->at != '}') {
				INT digit = hex_value(*p->at++);
				if (digit < 0) {
					fail(p, U_REGEX_BAD_ESCAPE_SEQUENCE);
					return 0;
				}
				value = value * 16 + digit;
			}
			if (!take_if(p, '}'))
				fail(p, U_REGEX_BAD_ESCAPE_SEQUENCE);
			return value;
		}
		return take_hex(p, 2);
	case 'u': return take_hex(p, 4);
	case 'U': return take_hex(p, 8);
	case 'c':
		if (at_end(p)) {
			fail(p, U_REGEX_BAD_ESCAPE_SEQUENCE);
			return 0;
		}
		return *p->at++ ^ 64;
	default:
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			fail(p, U_REGEX_BAD_ESCAPE_SEQUENCE);
		return c;
	}
}

// The members up to the closing bracket. A nested set of plain members is their union; set arithmetic and negated nesting are refused.
static void parse_set_into(parser *p, INT set)
{
	BOOL first = TRUE;
	while (!U_FAILED(p->status)) {
		if (at_end(p)) {
			fail(p, U_REGEX_MISSING_CLOSE_BRACKET);
			return;
		}
		if (*p->at == ']' && !first) {
			p->at++;
			return;
		}
		first = FALSE;
		if ((p->end - p->at >= 2 && p->at[0] == '&' && p->at[1] == '&') || (p->end - p->at >= 2 && p->at[0] == '[' && (p->at[1] == ':' || p->at[1] == '^'))) {
			fail(p, U_REGEX_UNIMPLEMENTED);
			return;
		}
		if (take_if(p, '[')) {
			parse_set_into(p, set);
			continue;
		}
		UChar32 low = take_char(p);
		if (low == '\\') {
			if (at_end(p)) {
				fail(p, U_REGEX_BAD_ESCAPE_SEQUENCE);
				return;
			}
			UChar c = *p->at++;
			class_kind kind;
			BOOL negated;
			if (class_escape(c, &kind, &negated)) {
				add_item(p, set, kind, negated, 0, 0);
				continue;
			}
			if (c == 'p' || c == 'P') {
				kind = take_property(p);
				add_item(p, set, kind, c == 'P', 0, 0);
				continue;
			}
			low = literal_escape(p, c);
		}
		UChar32 high = low;
		if (p->end - p->at >= 2 && p->at[0] == '-' && p->at[1] != ']') {
			p->at++;
			high = take_char(p);
			if (high == '\\') {
				if (at_end(p)) {
					fail(p, U_REGEX_BAD_ESCAPE_SEQUENCE);
					return;
				}
				high = literal_escape(p, *p->at++);
			}
			if (high < low) {
				fail(p, U_REGEX_INVALID_RANGE);
				return;
			}
		}
		add_item(p, set, CLASS_RANGE, FALSE, low, high);
	}
}

static node *parse_alternation(parser *p);

static node *class_node(parser *p, class_kind kind, BOOL negated)
{
	node *item = new_node(p, NODE_SET);
	if (!item)
		return NULL;
	item->set = add_set(p);
	if (!U_FAILED(p->status))
		add_item(p, item->set, kind, negated, 0, 0);
	return item;
}

static node *parse_escape(parser *p)
{
	if (at_end(p)) {
		fail(p, U_REGEX_BAD_ESCAPE_SEQUENCE);
		return NULL;
	}
	UChar c = *p->at++;
	class_kind kind;
	BOOL negated;
	if (class_escape(c, &kind, &negated))
		return class_node(p, kind, negated);
	if (c == 'p' || c == 'P') {
		kind = take_property(p);
		return class_node(p, kind, c == 'P');
	}
	node_kind simple = NODE_EMPTY;
	switch (c) {
	case 'b': simple = NODE_WORD_EDGE; break;
	case 'B': simple = NODE_NOT_WORD_EDGE; break;
	case 'A': simple = NODE_TEXT_START; break;
	case 'z': simple = NODE_TEXT_END; break;
	case 'Z': simple = NODE_TEXT_END_LINE; break;
	default: break;
	}
	if (simple != NODE_EMPTY)
		return new_node(p, simple);
	if (c == 'Q') {
		node *all = NULL;
		while (!at_end(p) && !(p->end - p->at >= 2 && p->at[0] == '\\' && p->at[1] == 'E')) {
			node *one = new_node(p, NODE_CHAR);
			if (!one)
				break;
			one->c = take_char(p);
			if (all) {
				node *joined = new_node(p, NODE_CONCAT);
				if (!joined)
					break;
				joined->left = all;
				joined->right = one;
				all = joined;
			} else
				all = one;
		}
		if (!at_end(p))
			p->at += 2;
		return all ? all : new_node(p, NODE_EMPTY);
	}
	if (c >= '1' && c <= '9') {
		INT group = c - '0';
		while (!at_end(p) && *p->at >= '0' && *p->at <= '9' && group * 10 + (*p->at - '0') <= p->regex->group_count)
			group = group * 10 + (*p->at++ - '0');
		if (group > p->regex->group_count) {
			fail(p, U_REGEX_INVALID_BACK_REF);
			return NULL;
		}
		node *ref = new_node(p, NODE_BACK_REF);
		if (ref)
			ref->group = group;
		return ref;
	}
	node *item = new_node(p, NODE_CHAR);
	if (item)
		item->c = literal_escape(p, c);
	return item;
}

static BOOL inline_flags(parser *p, BOOL *scoped)
{
	BOOL on = TRUE;
	UINT flags = p->flags;
	for (;;) {
		if (at_end(p)) {
			fail(p, U_REGEX_MISMATCHED_PAREN);
			return FALSE;
		}
		UChar c = *p->at++;
		UINT bit = 0;
		switch (c) {
		case '-': on = FALSE; continue;
		case 'i': bit = UREGEX_CASE_INSENSITIVE; break;
		case 'm': bit = UREGEX_MULTILINE; break;
		case 's': bit = UREGEX_DOTALL; break;
		case 'x': bit = UREGEX_COMMENTS; break;
		case 'w': continue;
		case ')': *scoped = FALSE; p->flags = flags; return TRUE;
		case ':': *scoped = TRUE; p->flags = flags; return TRUE;
		default: fail(p, U_REGEX_INVALID_FLAG); return FALSE;
		}
		flags = on ? flags | bit : flags & ~bit;
	}
}

static node *parse_group(parser *p)
{
	UINT saved = p->flags;
	node *inner, *group = NULL;
	if (!take_if(p, '?')) {
		group = new_node(p, NODE_CAPTURE);
		if (!group)
			return NULL;
		group->group = ++p->regex->group_count;
	} else if (take_if(p, ':') || take_if(p, '>')) {
	} else if (take_if(p, '=') || take_if(p, '!')) {
		group = new_node(p, NODE_LOOK);
		if (!group)
			return NULL;
		group->negated = p->at[-1] == '!';
	} else if (!at_end(p) && *p->at == '<' && p->end - p->at >= 2 && (p->at[1] == '=' || p->at[1] == '!')) {
		fail(p, U_REGEX_UNIMPLEMENTED);
		return NULL;
	} else if (take_if(p, '<')) {
		while (!at_end(p) && *p->at != '>')
			p->at++;
		if (!take_if(p, '>')) {
			fail(p, U_REGEX_RULE_SYNTAX);
			return NULL;
		}
		group = new_node(p, NODE_CAPTURE);
		if (!group)
			return NULL;
		group->group = ++p->regex->group_count;
	} else {
		BOOL scoped = FALSE;
		if (!inline_flags(p, &scoped))
			return NULL;
		// (?i) changes the flags for the rest of the enclosing group; the caller restores them there.
		if (!scoped)
			return new_node(p, NODE_EMPTY);
	}
	inner = parse_alternation(p);
	p->flags = saved;
	if (!take_if(p, ')')) {
		fail(p, U_REGEX_MISMATCHED_PAREN);
		free_tree(inner);
		free_tree(group);
		return NULL;
	}
	if (!group)
		return inner;
	group->left = inner;
	return group;
}

static void skip_comments(parser *p)
{
	if (!(p->flags & UREGEX_COMMENTS))
		return;
	while (!at_end(p)) {
		if (icu_is_space(*p->at))
			p->at++;
		else if (*p->at == '#') {
			while (!at_end(p) && *p->at != '\n')
				p->at++;
		} else
			break;
	}
}

static node *parse_atom(parser *p)
{
	UChar32 c = take_char(p);
	switch (c) {
	case '.': return new_node(p, NODE_ANY);
	case '^': return new_node(p, NODE_LINE_START);
	case '$': return new_node(p, NODE_LINE_END);
	case '\\': return parse_escape(p);
	case '(':
		if (++p->depth > MAX_DEPTH) {
			fail(p, U_REGEX_STACK_OVERFLOW);
			return NULL;
		}
		{
			node *group = parse_group(p);
			p->depth--;
			return group;
		}
	case '[': {
		node *item = new_node(p, NODE_SET);
		if (!item)
			return NULL;
		item->set = add_set(p);
		if (!U_FAILED(p->status)) {
			p->regex->sets[item->set].negated = take_if(p, '^');
			parse_set_into(p, item->set);
		}
		return item;
	}
	case '*': case '+': case '?': case ')':
		fail(p, c == ')' ? U_REGEX_MISMATCHED_PAREN : U_REGEX_RULE_SYNTAX);
		return NULL;
	default: {
		node *item = new_node(p, NODE_CHAR);
		if (item)
			item->c = c;
		return item;
	}
	}
}

static BOOL parse_interval(parser *p, INT *min, INT *max)
{
	INT low = 0, high = 0;
	BOOL any = FALSE;
	while (!at_end(p) && *p->at >= '0' && *p->at <= '9') {
		low = low * 10 + (*p->at++ - '0');
		any = TRUE;
		if (low > 100000) {
			fail(p, U_REGEX_BAD_INTERVAL);
			return FALSE;
		}
	}
	if (!any) {
		fail(p, U_REGEX_BAD_INTERVAL);
		return FALSE;
	}
	high = low;
	if (take_if(p, ',')) {
		if (!at_end(p) && *p->at == '}')
			high = REPEAT_FOREVER;
		else {
			high = 0;
			any = FALSE;
			while (!at_end(p) && *p->at >= '0' && *p->at <= '9') {
				high = high * 10 + (*p->at++ - '0');
				any = TRUE;
				if (high > 100000) {
					fail(p, U_REGEX_BAD_INTERVAL);
					return FALSE;
				}
			}
			if (!any) {
				fail(p, U_REGEX_BAD_INTERVAL);
				return FALSE;
			}
			if (high < low) {
				fail(p, U_REGEX_MAX_LT_MIN);
				return FALSE;
			}
		}
	}
	if (!take_if(p, '}')) {
		fail(p, U_REGEX_BAD_INTERVAL);
		return FALSE;
	}
	*min = low;
	*max = high;
	return TRUE;
}

static node *parse_repeat(parser *p)
{
	node *atom = parse_atom(p);
	while (atom && !U_FAILED(p->status)) {
		skip_comments(p);
		INT min, max;
		if (take_if(p, '*')) {
			min = 0;
			max = REPEAT_FOREVER;
		} else if (take_if(p, '+')) {
			min = 1;
			max = REPEAT_FOREVER;
		} else if (take_if(p, '?')) {
			min = 0;
			max = 1;
		} else if (take_if(p, '{')) {
			if (!parse_interval(p, &min, &max))
				break;
		} else
			break;
		node *loop = new_node(p, NODE_REPEAT);
		if (!loop)
			break;
		loop->left = atom;
		loop->min = min;
		loop->max = max;
		loop->lazy = take_if(p, '?');
		// Possessive loops give the same matches as greedy ones wherever they succeed, only later.
		if (!loop->lazy)
			take_if(p, '+');
		atom = loop;
	}
	return atom;
}

static node *parse_concat(parser *p)
{
	node *all = NULL;
	for (;;) {
		skip_comments(p);
		if (at_end(p) || *p->at == '|' || *p->at == ')' || U_FAILED(p->status))
			break;
		node *next = parse_repeat(p);
		if (!next)
			break;
		if (!all)
			all = next;
		else {
			node *joined = new_node(p, NODE_CONCAT);
			if (!joined) {
				free_tree(next);
				break;
			}
			joined->left = all;
			joined->right = next;
			all = joined;
		}
	}
	return all ? all : new_node(p, NODE_EMPTY);
}

static node *parse_alternation(parser *p)
{
	node *all = parse_concat(p);
	while (all && !U_FAILED(p->status) && take_if(p, '|')) {
		node *next = parse_concat(p);
		node *choice = new_node(p, NODE_ALTERNATE);
		if (!choice) {
			free_tree(next);
			break;
		}
		choice->left = all;
		choice->right = next;
		all = choice;
	}
	return all;
}

/* The program. */

static INT emit(parser *p, op_kind op, INT x, INT y)
{
	URegularExpression *regex = p->regex;
	if (regex->program_count >= MAX_PROGRAM) {
		fail(p, U_REGEX_STACK_OVERFLOW);
		return 0;
	}
	if (!(regex->program_count & (regex->program_count - 1))) {
		INT room = regex->program_count ? regex->program_count * 2 : 16;
		inst *grown = icu_alloc(room * sizeof(inst));
		if (!grown) {
			fail(p, U_MEMORY_ALLOCATION_ERROR);
			return 0;
		}
		if (regex->program_count)
			CopyMemory(grown, regex->program, regex->program_count * sizeof(inst));
		icu_free(regex->program);
		regex->program = grown;
	}
	inst *item = &regex->program[regex->program_count];
	item->op = op;
	item->x = x;
	item->y = y;
	return regex->program_count++;
}

static void generate(parser *p, const node *item);

// An empty pass through a loop body must not loop again: the register holds where the pass began.
static void generate_star(parser *p, const node *item)
{
	INT reg = p->regex->register_count++;
	INT split = emit(p, OP_SPLIT, 0, 0);
	if (U_FAILED(p->status))
		return;
	emit(p, OP_MARK, reg, 0);
	INT body = split + 1;
	generate(p, item->left);
	emit(p, OP_CHECK, reg, 0);
	emit(p, OP_JUMP, split, 0);
	if (U_FAILED(p->status))
		return;
	INT after = p->regex->program_count;
	p->regex->program[split].x = item->lazy ? after : body;
	p->regex->program[split].y = item->lazy ? body : after;
}

static void generate_optional(parser *p, const node *item, INT remaining)
{
	if (!remaining || U_FAILED(p->status))
		return;
	INT split = emit(p, OP_SPLIT, 0, 0);
	if (U_FAILED(p->status))
		return;
	generate(p, item->left);
	generate_optional(p, item, remaining - 1);
	if (U_FAILED(p->status))
		return;
	INT after = p->regex->program_count;
	p->regex->program[split].x = item->lazy ? after : split + 1;
	p->regex->program[split].y = item->lazy ? split + 1 : after;
}

static void generate(parser *p, const node *item)
{
	if (!item || U_FAILED(p->status))
		return;
	INT at;
	switch (item->kind) {
	case NODE_EMPTY:
		break;
	case NODE_CHAR:
		at = emit(p, OP_CHAR, item->c, 0);
		if (!U_FAILED(p->status))
			p->regex->program[at].caseless = item->caseless;
		break;
	case NODE_ANY:
		at = emit(p, OP_ANY, 0, 0);
		if (!U_FAILED(p->status))
			p->regex->program[at].flag = item->dotall;
		break;
	case NODE_SET:
		at = emit(p, OP_SET, item->set, 0);
		if (!U_FAILED(p->status))
			p->regex->program[at].caseless = item->caseless;
		break;
	case NODE_LINE_START:
		at = emit(p, OP_LINE_START, 0, 0);
		if (!U_FAILED(p->status))
			p->regex->program[at].flag = item->multiline;
		break;
	case NODE_LINE_END:
		at = emit(p, OP_LINE_END, 0, 0);
		if (!U_FAILED(p->status))
			p->regex->program[at].flag = item->multiline;
		break;
	case NODE_TEXT_START: emit(p, OP_TEXT_START, 0, 0); break;
	case NODE_TEXT_END: emit(p, OP_TEXT_END, 0, 0); break;
	case NODE_TEXT_END_LINE: emit(p, OP_TEXT_END_LINE, 0, 0); break;
	case NODE_WORD_EDGE: emit(p, OP_WORD_EDGE, 0, 0); break;
	case NODE_NOT_WORD_EDGE: emit(p, OP_NOT_WORD_EDGE, 0, 0); break;
	case NODE_BACK_REF:
		at = emit(p, OP_BACK_REF, item->group, 0);
		if (!U_FAILED(p->status))
			p->regex->program[at].caseless = item->caseless;
		break;
	case NODE_CONCAT:
		generate(p, item->left);
		generate(p, item->right);
		break;
	case NODE_ALTERNATE: {
		INT split = emit(p, OP_SPLIT, 0, 0);
		generate(p, item->left);
		INT jump = emit(p, OP_JUMP, 0, 0);
		INT second = p->regex->program_count;
		generate(p, item->right);
		if (U_FAILED(p->status))
			return;
		p->regex->program[split].x = split + 1;
		p->regex->program[split].y = second;
		p->regex->program[jump].x = p->regex->program_count;
		break;
	}
	case NODE_CAPTURE:
		emit(p, OP_SAVE, item->group * 2, 0);
		generate(p, item->left);
		emit(p, OP_SAVE, item->group * 2 + 1, 0);
		break;
	case NODE_LOOK: {
		INT look = emit(p, OP_LOOK, 0, item->negated);
		generate(p, item->left);
		emit(p, OP_LOOK_END, 0, 0);
		if (!U_FAILED(p->status))
			p->regex->program[look].x = p->regex->program_count;
		break;
	}
	case NODE_REPEAT:
		for (INT i = 0; i < item->min && !U_FAILED(p->status); i++)
			generate(p, item->left);
		if (item->max == REPEAT_FOREVER)
			generate_star(p, item);
		else
			generate_optional(p, item, item->max - item->min);
		break;
	}
}

/* The machine. */

static BOOL class_has(const class_item *item, UChar32 c)
{
	BOOL in;
	WORD kind;
	switch (item->kind) {
	case CLASS_RANGE: in = c >= item->low && c <= item->high; break;
	case CLASS_DIGIT: in = icu_is_digit(c); break;
	case CLASS_WORD: in = icu_is_word(c); break;
	case CLASS_SPACE: in = icu_is_space(c) || c == 0x85; break;
	case CLASS_LETTER: in = icu_is_letter(c); break;
	case CLASS_UPPER: kind = icu_kind(c); in = (kind & C1_UPPER) != 0; break;
	case CLASS_LOWER: kind = icu_kind(c); in = (kind & C1_LOWER) != 0; break;
	case CLASS_NUMBER: in = icu_is_digit(c); break;
	case CLASS_PUNCTUATION: kind = icu_kind(c); in = (kind & C1_PUNCT) != 0; break;
	case CLASS_MARK: in = icu_is_mark(c); break;
	default: in = TRUE; break;
	}
	return item->negated ? !in : in;
}

static BOOL set_holds(const char_set *set, UChar32 c)
{
	for (INT i = 0; i < set->count; i++)
		if (class_has(&set->items[i], c))
			return TRUE;
	return FALSE;
}

static BOOL set_matches(const char_set *set, UChar32 c, BOOL caseless)
{
	BOOL in = set_holds(set, c);
	if (!in && caseless)
		in = set_holds(set, icu_lower(c)) || set_holds(set, icu_upper(c));
	return set->negated ? !in : in;
}

static BOOL is_line_end(UChar32 c)
{
	return c == '\n' || c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029 || c == 0xc;
}

static BOOL word_before(const URegularExpression *regex, INT at)
{
	if (at <= 0)
		return FALSE;
	INT start = at - 1;
	if (start > 0 && regex->text[start] >= 0xdc00 && regex->text[start] < 0xe000 && regex->text[start - 1] >= 0xd800 && regex->text[start - 1] < 0xdc00)
		start--;
	return icu_is_word(icu_at(regex->text, regex->text_length, start, NULL));
}

static BOOL word_after(const URegularExpression *regex, INT at)
{
	return at < regex->text_length && icu_is_word(icu_at(regex->text, regex->text_length, at, NULL));
}

typedef struct {
	INT pc_or_slot;
	INT value;
	BOOL restore;
} back_entry;

typedef struct {
	back_entry *items;
	INT count, room;
} back_stack;

static BOOL back_push(back_stack *stack, INT a, INT b, BOOL restore)
{
	if (stack->count == stack->room) {
		INT room = stack->room ? stack->room * 2 : 64;
		if (room > 16 * 1024 * 1024)
			return FALSE;
		back_entry *grown = icu_alloc(room * sizeof(back_entry));
		if (!grown)
			return FALSE;
		if (stack->count)
			CopyMemory(grown, stack->items, stack->count * sizeof(back_entry));
		icu_free(stack->items);
		stack->items = grown;
		stack->room = room;
	}
	back_entry *item = &stack->items[stack->count++];
	item->pc_or_slot = a;
	item->value = b;
	item->restore = restore;
	return TRUE;
}

typedef enum { RUN_FAILED, RUN_MATCHED, RUN_ERROR } run_result;

// Runs from pc until OP_MATCH or OP_LOOK_END. whole asks that a match reach the end of the text.
static run_result run(URegularExpression *regex, INT pc, INT at, BOOL whole, INT *end, INT *steps, INT depth, UErrorCode *status)
{
	back_stack stack = {0};
	run_result result = RUN_FAILED;
	const UChar *text = regex->text;
	INT length = regex->text_length;
	INT *slots = regex->slots;
	if (depth > MAX_DEPTH) {
		*status = U_REGEX_STACK_OVERFLOW;
		return RUN_ERROR;
	}
	for (;;) {
		const inst *item = &regex->program[pc];
		BOOL ok = TRUE;
		if (++*steps > MAX_STEPS) {
			*status = U_REGEX_TIME_OUT;
			result = RUN_ERROR;
			break;
		}
		switch (item->op) {
		case OP_CHAR: {
			INT next;
			if (at >= length) {
				ok = FALSE;
				break;
			}
			UChar32 c = icu_at(text, length, at, &next);
			ok = c == item->x || (item->caseless && (icu_lower(c) == icu_lower(item->x) || icu_upper(c) == icu_upper(item->x)));
			if (ok) {
				at = next;
				pc++;
			}
			break;
		}
		case OP_ANY: {
			INT next;
			if (at >= length) {
				ok = FALSE;
				break;
			}
			UChar32 c = icu_at(text, length, at, &next);
			ok = item->flag || !is_line_end(c);
			if (ok) {
				// A CR LF pair is one line ending, and DOTALL takes it as one character.
				if (c == '\r' && next < length && text[next] == '\n')
					next++;
				at = next;
				pc++;
			}
			break;
		}
		case OP_SET: {
			INT next;
			if (at >= length) {
				ok = FALSE;
				break;
			}
			UChar32 c = icu_at(text, length, at, &next);
			ok = set_matches(&regex->sets[item->x], c, item->caseless);
			if (ok) {
				at = next;
				pc++;
			}
			break;
		}
		case OP_SPLIT:
			if (!back_push(&stack, item->y, at, FALSE)) {
				*status = U_REGEX_STACK_OVERFLOW;
				icu_free(stack.items);
				return RUN_ERROR;
			}
			pc = item->x;
			break;
		case OP_JUMP:
			pc = item->x;
			break;
		case OP_SAVE:
		case OP_MARK: {
			INT slot = item->op == OP_SAVE ? item->x : (regex->group_count + 1) * 2 + item->x;
			if (!back_push(&stack, slot, slots[slot], TRUE)) {
				*status = U_REGEX_STACK_OVERFLOW;
				icu_free(stack.items);
				return RUN_ERROR;
			}
			slots[slot] = at;
			pc++;
			break;
		}
		case OP_CHECK:
			ok = slots[(regex->group_count + 1) * 2 + item->x] != at;
			if (ok)
				pc++;
			break;
		case OP_LINE_START:
			ok = at == 0 || (item->flag && is_line_end(text[at - 1]) && !(text[at - 1] == '\r' && at < length && text[at] == '\n') && at < length);
			if (ok)
				pc++;
			break;
		case OP_LINE_END:
			ok = at == length || (item->flag && is_line_end(text[at]) && !(at > 0 && text[at] == '\n' && text[at - 1] == '\r'));
			if (!ok && !item->flag) {
				INT rest = length - at;
				ok = (rest == 1 && is_line_end(text[at])) || (rest == 2 && text[at] == '\r' && text[at + 1] == '\n');
			}
			if (ok)
				pc++;
			break;
		case OP_TEXT_START:
			ok = at == 0;
			if (ok)
				pc++;
			break;
		case OP_TEXT_END:
			ok = at == length;
			if (ok)
				pc++;
			break;
		case OP_TEXT_END_LINE: {
			INT rest = length - at;
			ok = rest == 0 || (rest == 1 && is_line_end(text[at])) || (rest == 2 && text[at] == '\r' && text[at + 1] == '\n');
			if (ok)
				pc++;
			break;
		}
		case OP_WORD_EDGE:
		case OP_NOT_WORD_EDGE: {
			BOOL edge = word_before(regex, at) != word_after(regex, at);
			ok = item->op == OP_WORD_EDGE ? edge : !edge;
			if (ok)
				pc++;
			break;
		}
		case OP_BACK_REF: {
			INT from = slots[item->x * 2], to = slots[item->x * 2 + 1];
			if (from < 0 || to < 0) {
				ok = FALSE;
				break;
			}
			for (INT i = from; i < to && ok; i++, at++) {
				if (at >= length)
					ok = FALSE;
				else if (item->caseless)
					ok = icu_lower(text[i]) == icu_lower(text[at]);
				else
					ok = text[i] == text[at];
			}
			if (ok)
				pc++;
			break;
		}
		case OP_LOOK: {
			INT ignored;
			run_result inner = run(regex, pc + 1, at, FALSE, &ignored, steps, depth + 1, status);
			if (inner == RUN_ERROR) {
				icu_free(stack.items);
				return RUN_ERROR;
			}
			ok = (inner == RUN_MATCHED) != item->y;
			if (ok)
				pc = item->x;
			break;
		}
		case OP_LOOK_END:
			*end = at;
			icu_free(stack.items);
			return RUN_MATCHED;
		case OP_MATCH:
			ok = !whole || at == length;
			if (ok) {
				*end = at;
				icu_free(stack.items);
				return RUN_MATCHED;
			}
			break;
		}
		if (ok)
			continue;
		// Back to the last choice, undoing the captures made since.
		BOOL resumed = FALSE;
		while (stack.count) {
			back_entry *top = &stack.items[--stack.count];
			if (top->restore) {
				slots[top->pc_or_slot] = top->value;
				continue;
			}
			pc = top->pc_or_slot;
			at = top->value;
			resumed = TRUE;
			break;
		}
		if (!resumed)
			break;
	}
	icu_free(stack.items);
	return result;
}

static void clear_match(URegularExpression *regex)
{
	INT count = (regex->group_count + 1) * 2 + regex->register_count;
	for (INT i = 0; i < count; i++)
		regex->slots[i] = -1;
	regex->matched = FALSE;
}

static BOOL try_at(URegularExpression *regex, INT start, BOOL whole, INT *steps, UErrorCode *status)
{
	clear_match(regex);
	INT end;
	if (run(regex, 0, start, whole, &end, steps, 0, status) != RUN_MATCHED)
		return FALSE;
	regex->slots[0] = start;
	regex->slots[1] = end;
	regex->matched = TRUE;
	// An empty match must not be found again at the same place.
	regex->next_start = end;
	if (end == start) {
		if (end < regex->text_length)
			icu_at(regex->text, regex->text_length, end, &regex->next_start);
		else
			regex->next_start = end + 1;
	}
	return TRUE;
}

static UBool search_from(URegularExpression *regex, INT start, UErrorCode *status)
{
	INT steps = 0;
	for (INT at = start; at <= regex->text_length && !U_FAILED(*status);) {
		if (try_at(regex, at, FALSE, &steps, status))
			return TRUE;
		if (at == regex->text_length)
			break;
		icu_at(regex->text, regex->text_length, at, &at);
	}
	clear_match(regex);
	regex->next_start = regex->text_length + 1;
	return FALSE;
}

static BOOL has_text(URegularExpression *regex, UErrorCode *status)
{
	if (U_FAILED(*status))
		return FALSE;
	if (!regex->text) {
		*status = U_REGEX_INVALID_STATE;
		return FALSE;
	}
	return TRUE;
}

/* The interface. */

static void free_program(URegularExpression *regex)
{
	for (INT i = 0; i < regex->set_count; i++)
		icu_free(regex->sets[i].items);
	icu_free(regex->sets);
	icu_free(regex->program);
	icu_free(regex->slots);
}

void __cdecl uregex_close(URegularExpression *regex)
{
	if (!regex)
		return;
	free_program(regex);
	icu_free(regex->pattern);
	icu_free(regex->owned_text);
	icu_free(regex);
}

static URegularExpression *compile(const UChar *pattern, INT length, UINT flags, UParseError *error, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	if (!pattern || length < -1 || !length) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return NULL;
	}
	length = icu_length(pattern, length);
	URegularExpression *regex = icu_alloc(sizeof *regex);
	if (!regex || !(regex->pattern = icu_alloc((length + 1) * sizeof(UChar)))) {
		icu_free(regex);
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	CopyMemory(regex->pattern, pattern, length * sizeof(UChar));
	regex->pattern_length = length;
	regex->flags = flags;
	parser p = {0};
	p.at = p.start = regex->pattern;
	p.end = regex->pattern + length;
	p.flags = flags;
	p.regex = regex;
	node *tree = NULL;
	if (flags & UREGEX_LITERAL) {
		while (!at_end(&p)) {
			node *one = new_node(&p, NODE_CHAR);
			if (!one)
				break;
			one->c = take_char(&p);
			if (tree) {
				node *joined = new_node(&p, NODE_CONCAT);
				if (!joined)
					break;
				joined->left = tree;
				joined->right = one;
				tree = joined;
			} else
				tree = one;
		}
	} else {
		tree = parse_alternation(&p);
		if (!U_FAILED(p.status) && !at_end(&p))
			fail(&p, U_REGEX_MISMATCHED_PAREN);
	}
	generate(&p, tree);
	emit(&p, OP_MATCH, 0, 0);
	free_tree(tree);
	if (!U_FAILED(p.status)) {
		regex->slots = icu_alloc(((regex->group_count + 1) * 2 + regex->register_count) * sizeof(INT));
		if (!regex->slots)
			fail(&p, U_MEMORY_ALLOCATION_ERROR);
	}
	if (U_FAILED(p.status)) {
		if (error) {
			error->line = 1;
			error->offset = (INT)(p.at - p.start);
		}
		*status = p.status;
		uregex_close(regex);
		return NULL;
	}
	clear_match(regex);
	return regex;
}

URegularExpression *__cdecl uregex_open(const UChar *pattern, INT length, UINT flags, UParseError *error, UErrorCode *status)
{
	return compile(pattern, length, flags, error, status);
}

URegularExpression *__cdecl uregex_openUText(UText *pattern, UINT flags, UParseError *error, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	if (!pattern) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return NULL;
	}
	return compile(pattern->units, pattern->length, flags, error, status);
}

// Documented: a clone has the pattern and its flags, and not the input or the match.
URegularExpression *__cdecl uregex_clone(const URegularExpression *regex, UErrorCode *status)
{
	return compile(regex->pattern, regex->pattern_length, regex->flags, NULL, status);
}

void __cdecl uregex_setText(URegularExpression *regex, const UChar *text, INT length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return;
	if (!text || length < -1) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return;
	}
	icu_free(regex->owned_text);
	regex->owned_text = NULL;
	regex->text = text;
	regex->text_length = icu_length(text, length);
	regex->next_start = 0;
	clear_match(regex);
}

void __cdecl uregex_setUText(URegularExpression *regex, UText *text, UErrorCode *status)
{
	if (U_FAILED(*status))
		return;
	if (!text) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return;
	}
	UChar *copy = icu_alloc((text->length + 1) * sizeof(UChar));
	if (!copy) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return;
	}
	if (text->length)
		CopyMemory(copy, text->units, text->length * sizeof(UChar));
	icu_free(regex->owned_text);
	regex->owned_text = copy;
	regex->text = copy;
	regex->text_length = text->length;
	regex->next_start = 0;
	clear_match(regex);
}

UBool __cdecl uregex_matches(URegularExpression *regex, INT start, UErrorCode *status)
{
	if (!has_text(regex, status))
		return FALSE;
	if (start > regex->text_length) {
		*status = U_INDEX_OUTOFBOUNDS_ERROR;
		return FALSE;
	}
	INT steps = 0;
	return try_at(regex, start < 0 ? 0 : start, TRUE, &steps, status);
}

UBool __cdecl uregex_lookingAt(URegularExpression *regex, INT start, UErrorCode *status)
{
	if (!has_text(regex, status))
		return FALSE;
	if (start > regex->text_length) {
		*status = U_INDEX_OUTOFBOUNDS_ERROR;
		return FALSE;
	}
	INT steps = 0;
	return try_at(regex, start < 0 ? 0 : start, FALSE, &steps, status);
}

UBool __cdecl uregex_findNext(URegularExpression *regex, UErrorCode *status)
{
	if (!has_text(regex, status))
		return FALSE;
	if (regex->next_start > regex->text_length) {
		clear_match(regex);
		return FALSE;
	}
	return search_from(regex, regex->next_start, status);
}

UBool __cdecl uregex_find(URegularExpression *regex, INT start, UErrorCode *status)
{
	if (start < 0)
		return uregex_findNext(regex, status);
	if (!has_text(regex, status))
		return FALSE;
	if (start > regex->text_length) {
		*status = U_INDEX_OUTOFBOUNDS_ERROR;
		return FALSE;
	}
	return search_from(regex, start, status);
}

INT __cdecl uregex_groupCount(URegularExpression *regex, UErrorCode *status)
{
	(void)status;
	return regex->group_count;
}

static INT group_edge(const URegularExpression *regex, INT group, INT edge, UErrorCode *status)
{
	if (U_FAILED(*status))
		return -1;
	if (!regex->matched) {
		*status = U_REGEX_INVALID_STATE;
		return -1;
	}
	if (group < 0 || group > regex->group_count) {
		*status = U_INDEX_OUTOFBOUNDS_ERROR;
		return -1;
	}
	// A group that took no part in the match has no text, both of its ends are -1.
	INT from = regex->slots[group * 2], to = regex->slots[group * 2 + 1];
	return from < 0 || to < 0 ? -1 : edge ? to : from;
}

INT __cdecl uregex_start(URegularExpression *regex, INT group, UErrorCode *status)
{
	return group_edge(regex, group, 0, status);
}

INT __cdecl uregex_end(URegularExpression *regex, INT group, UErrorCode *status)
{
	return group_edge(regex, group, 1, status);
}

INT __cdecl uregex_group(URegularExpression *regex, INT group, UChar *dest, INT capacity, UErrorCode *status)
{
	INT from = group_edge(regex, group, 0, status), to = group_edge(regex, group, 1, status);
	if (U_FAILED(*status))
		return 0;
	INT count = from < 0 ? 0 : to - from;
	if (dest && capacity && count)
		CopyMemory(dest, regex->text + from, (count < capacity ? count : capacity) * sizeof(UChar));
	if (dest && count < capacity)
		dest[count] = 0;
	return icu_finish(count, capacity, status);
}

void __cdecl uregex_reset(URegularExpression *regex, INT index, UErrorCode *status)
{
	if (!has_text(regex, status))
		return;
	if (index < 0 || index > regex->text_length) {
		*status = U_INDEX_OUTOFBOUNDS_ERROR;
		return;
	}
	regex->next_start = index;
	clear_match(regex);
}

/* Replacement. The output grows as needed and is measured by the caller's capacity only at the end. */

typedef struct {
	UChar *units;
	INT count, room;
	BOOL failed;
} text_out;

static void out_units(text_out *out, const UChar *units, INT count)
{
	if (out->failed || count <= 0)
		return;
	if (out->count + count > out->room) {
		INT room = out->room ? out->room * 2 : 256;
		while (room < out->count + count)
			room *= 2;
		UChar *grown = icu_alloc(room * sizeof(UChar));
		if (!grown) {
			out->failed = TRUE;
			return;
		}
		if (out->count)
			CopyMemory(grown, out->units, out->count * sizeof(UChar));
		icu_free(out->units);
		out->units = grown;
		out->room = room;
	}
	CopyMemory(out->units + out->count, units, count * sizeof(UChar));
	out->count += count;
}

// In a replacement $n is group n and a backslash takes the next character as it is.
static void append_replacement(const URegularExpression *regex, text_out *out, const UChar *replacement, INT length, UErrorCode *status)
{
	for (INT i = 0; i < length && !U_FAILED(*status);) {
		UChar c = replacement[i];
		if (c == '\\') {
			if (i + 1 >= length) {
				*status = U_REGEX_INVALID_STATE;
				return;
			}
			out_units(out, &replacement[i + 1], 1);
			i += 2;
		} else if (c == '$') {
			i++;
			if (i >= length || replacement[i] < '0' || replacement[i] > '9') {
				*status = U_REGEX_INVALID_CAPTURE_GROUP_NAME;
				return;
			}
			INT group = replacement[i++] - '0';
			while (i < length && replacement[i] >= '0' && replacement[i] <= '9' && group * 10 + (replacement[i] - '0') <= regex->group_count)
				group = group * 10 + (replacement[i++] - '0');
			if (group > regex->group_count) {
				*status = U_INDEX_OUTOFBOUNDS_ERROR;
				return;
			}
			INT from = regex->slots[group * 2], to = regex->slots[group * 2 + 1];
			if (from >= 0 && to >= 0)
				out_units(out, regex->text + from, to - from);
		} else {
			out_units(out, &replacement[i], 1);
			i++;
		}
	}
}

static BOOL replace_matches(URegularExpression *regex, const UChar *replacement, INT length, BOOL all, text_out *out, UErrorCode *status)
{
	if (!has_text(regex, status))
		return FALSE;
	INT copied = 0;
	regex->next_start = 0;
	while (uregex_findNext(regex, status) && !U_FAILED(*status)) {
		out_units(out, regex->text + copied, regex->slots[0] - copied);
		append_replacement(regex, out, replacement, length, status);
		copied = regex->slots[1];
		if (!all)
			break;
	}
	if (U_FAILED(*status))
		return FALSE;
	out_units(out, regex->text + copied, regex->text_length - copied);
	if (out->failed)
		*status = U_MEMORY_ALLOCATION_ERROR;
	return !U_FAILED(*status);
}

static INT replace_to_buffer(URegularExpression *regex, const UChar *replacement, INT length, BOOL all, UChar *dest, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	if (!replacement || capacity < 0 || (capacity && !dest)) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	text_out out = {0};
	INT count = 0;
	if (replace_matches(regex, replacement, icu_length(replacement, length), all, &out, status)) {
		count = out.count;
		if (capacity && count)
			CopyMemory(dest, out.units, (count < capacity ? count : capacity) * sizeof(UChar));
		if (count < capacity)
			dest[count] = 0;
		count = icu_finish(count, capacity, status);
	}
	icu_free(out.units);
	return count;
}

INT __cdecl uregex_replaceAll(URegularExpression *regex, const UChar *replacement, INT length, UChar *dest, INT capacity, UErrorCode *status)
{
	return replace_to_buffer(regex, replacement, length, TRUE, dest, capacity, status);
}

INT __cdecl uregex_replaceFirst(URegularExpression *regex, const UChar *replacement, INT length, UChar *dest, INT capacity, UErrorCode *status)
{
	return replace_to_buffer(regex, replacement, length, FALSE, dest, capacity, status);
}

// A destination to reuse is not supported: winget and the documented default pass none and get a new text.
UText *__cdecl uregex_replaceAllUText(URegularExpression *regex, UText *replacement, UText *dest, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	if (!replacement || dest) {
		*status = !replacement ? U_ILLEGAL_ARGUMENT_ERROR : U_UNSUPPORTED_ERROR;
		return NULL;
	}
	text_out out = {0};
	UText *result = NULL;
	if (replace_matches(regex, replacement->units, replacement->length, TRUE, &out, status))
		result = utext_openUChars(NULL, out.units ? out.units : L"", out.count, status);
	icu_free(out.units);
	return result;
}
