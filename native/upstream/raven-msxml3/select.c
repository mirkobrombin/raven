/* SPDX-License-Identifier: MIT */
#include "node.h"

/* --- XSL patterns are the older query language: most of it is XPath spelled a little differently, so
 * a query is rewritten token by token and libxml does the rest. --- */

typedef struct
{
	xmlChar *text;
	int used, room;
	const mx_tree *data;
} writer;

static void put_bytes(writer *out, const char *data, int size)
{
	if (out->used + size + 1 > out->room)
	{
		out->room = (out->used + size + 1) * 2;
		out->text = xmlRealloc(out->text, out->room);
	}
	memcpy(out->text + out->used, data, size);
	out->used += size;
	out->text[out->used] = 0;
}

static void put_text(writer *out, const char *text)
{
	put_bytes(out, text, (int)strlen(text));
}

static BOOL name_char(xmlChar c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':' || c >= 0x80;
}

static const char *const lower_case = "'abcdefghijklmnopqrstuvwxyz'";
static const char *const upper_case = "'ABCDEFGHIJKLMNOPQRSTUVWXYZ'";

/* The comparison words. The ones starting with an i ignore case. */
static const struct { const char *word, *xpath; BOOL fold; } operators[] = {
	{"$and$", " and ", FALSE}, {"$or$", " or ", FALSE}, {"$eq$", "=", FALSE}, {"$ne$", "!=", FALSE}, {"$lt$", "<", FALSE}, {"$le$", "<=", FALSE},
	{"$gt$", ">", FALSE}, {"$ge$", ">=", FALSE}, {"$union$", "|", FALSE}, {"$ieq$", "=", TRUE}, {"$ine$", "!=", TRUE}, {"$ilt$", "<", TRUE},
	{"$ile$", "<=", TRUE}, {"$igt$", ">", TRUE}, {"$ige$", ">=", TRUE},
};

/* Where the operand before an operator starts: back over one balanced term. */
static int operand_start(const xmlChar *text, int end)
{
	int at = end, depth = 0;
	while (at > 0 && text[at - 1] == ' ')
		at--;
	while (at > 0)
	{
		xmlChar c = text[at - 1];
		if (c == ')' || c == ']')
			depth++;
		else if (c == '(' || c == '[')
		{
			if (!depth)
				break;
			depth--;
		}
		else if (!depth && (c == ' ' || c == '=' || c == '<' || c == '>' || c == '|' || c == ','))
			break;
		at--;
	}
	return at;
}

static const xmlChar *operand_end(const xmlChar *at)
{
	int depth = 0;
	while (*at == ' ')
		at++;
	if (!xmlStrncasecmp(at, (const xmlChar *)"$any$", 5) || !xmlStrncasecmp(at, (const xmlChar *)"$all$", 5))
		return operand_end(at + 5);
	if (*at == '\'' || *at == '"')
	{
		const xmlChar *close = xmlStrchr(at + 1, *at);
		return close ? close + 1 : at + xmlStrlen(at);
	}
	while (*at)
	{
		if (*at == '\'' || *at == '"')
		{
			const xmlChar *close = xmlStrchr(at + 1, *at);
			if (!close)
				return at + xmlStrlen(at);
			at = close + 1;
			continue;
		}
		if (*at == '(' || *at == '[')
			depth++;
		else if (*at == ')' || *at == ']')
		{
			if (!depth)
				break;
			depth--;
		}
		else if (!depth && (*at == ' ' || *at == '$'))
			break;
		at++;
	}
	return at;
}

static void wrap_lowered(writer *out, int start)
{
	int size = out->used - start;
	xmlChar *term = xmlStrndup(out->text + start, size);
	out->used = start;
	put_text(out, "translate(");
	put_bytes(out, (const char *)term, size);
	put_text(out, ",");
	put_text(out, upper_case);
	put_text(out, ",");
	put_text(out, lower_case);
	put_text(out, ")");
	xmlFree(term);
}

/* Square brackets holding nothing but a number count from nought here. */
static BOOL plain_index(const xmlChar *at, int *value, const xmlChar **after)
{
	const xmlChar *from = at;
	int number = 0;
	while (*from == ' ')
		from++;
	if (*from < '0' || *from > '9')
		return FALSE;
	while (*from >= '0' && *from <= '9')
		number = number * 10 + (*from++ - '0');
	if (*from == '.')
	{
		from++;
		while (*from == '0')
			from++;
	}
	while (*from == ' ')
		from++;
	if (*from != ']')
		return FALSE;
	*value = number;
	*after = from;
	return TRUE;
}

static void rewrite(const xmlChar *at, writer *out);

/* A word is an operator only where an operand has just ended. */
static BOOL after_operand(const writer *out)
{
	int at = out->used;
	while (at > 0 && out->text[at - 1] == ' ')
		at--;
	if (!at || at == out->used)
		return FALSE;
	return name_char(out->text[at - 1]) || out->text[at - 1] == ')' || out->text[at - 1] == ']' || out->text[at - 1] == '\'' || out->text[at - 1] == '"' || out->text[at - 1] == '*';
}

static BOOL prefix_registered(const writer *out, const xmlChar *name, int size)
{
	const mx_select_ns *space;
	if (!out->data)
		return FALSE;
	for (space = out->data->props->namespaces; space; space = space->next)
		if (space->prefix && !xmlStrncmp(space->prefix, name, size) && !space->prefix[size])
			return TRUE;
	return FALSE;
}

/* A pattern's names match as written in the document: a bare one takes no namespace into account, and a prefix nobody has
 * registered is compared as text. */
static void put_name_test(writer *out, const xmlChar *name, int size)
{
	const xmlChar *colon = xmlStrchr(name, ':');
	int prefix = colon && colon < name + size ? (int)(colon - name) : -1;
	if (prefix >= 0 && prefix_registered(out, name, prefix))
	{
		put_bytes(out, (const char *)name, size);
		return;
	}
	if (prefix >= 0 && prefix == size - 1)
	{
		put_text(out, "*[starts-with(name(),'");
		put_bytes(out, (const char *)name, size);
		put_text(out, "')]");
		return;
	}
	put_text(out, "*[name()='");
	put_bytes(out, (const char *)name, size);
	put_text(out, "']");
}

/* The named node tests and functions. Anything else passes through as it stands. */
static void put_name_test(writer *out, const xmlChar *name, int size);
static const xmlChar *rewrite_function(const xmlChar *at, const xmlChar *end, writer *out)
{
	int length = (int)(end - at);
	const xmlChar *close = end + 1;
	BOOL empty;
	while (*close == ' ')
		close++;
	empty = *close == ')';
#define IS(word) (length == (int)strlen(word) && !xmlStrncmp(at, (const xmlChar *)word, length))
	if (empty)
	{
		const char *replacement = NULL;
		if (IS("end")) replacement = "(position()=last())";
		else if (IS("index")) replacement = "(position()-1)";
		else if (IS("nodeName")) replacement = "name()";
		else if (IS("nodeType")) replacement = "mx-node-type()";
		else if (IS("baseName")) replacement = "local-name()";
		else if (IS("namespaceURI")) replacement = "namespace-uri()";
		else if (IS("prefix")) replacement = "substring-before(name(),':')";
		else if (IS("value")) replacement = "string(.)";
		else if (IS("text")) replacement = "text()";
		else if (IS("textnode")) replacement = "text()";
		else if (IS("element")) replacement = "*";
		else if (IS("attribute")) replacement = "@*";
		else if (IS("pi")) replacement = "processing-instruction()";
		else if (IS("cdata")) replacement = "text()[mx-node-type()=4]";
		else if (IS("comment")) replacement = "comment()";
		else if (IS("node")) replacement = "node()";
		else if (IS("context")) replacement = ".";
		if (replacement)
		{
			put_text(out, replacement);
			return close + 1;
		}
	}
	else if (IS("element") || IS("attribute") || IS("pi"))
	{
		const xmlChar *argument = close, *finish;
		if (*argument == '\'' || *argument == '"')
		{
			finish = xmlStrchr(argument + 1, *argument);
			if (finish)
			{
				const xmlChar *past = finish + 1;
				while (*past == ' ')
					past++;
				if (*past == ')')
				{
					BOOL any = finish == argument + 2 && argument[1] == '*';
					if (IS("attribute"))
						put_text(out, "@");
					if (IS("pi"))
					{
						put_text(out, "processing-instruction(");
						if (!any)
							put_bytes(out, (const char *)argument, (int)(finish - argument + 1));
						put_text(out, ")");
					}
					else if (any)
						put_text(out, "*");
					else
						put_name_test(out, argument + 1, (int)(finish - argument - 1));
					return past + 1;
				}
			}
		}
	}
#undef IS
	put_bytes(out, (const char *)at, length);
	put_text(out, "(");
	return end + 1;
}

static void rewrite(const xmlChar *at, writer *out)
{
	while (*at)
	{
		xmlChar c = *at;
		if (c == '\'' || c == '"')
		{
			const xmlChar *close = xmlStrchr(at + 1, c);
			int size = close ? (int)(close - at + 1) : xmlStrlen(at);
			put_bytes(out, (const char *)at, size);
			at += size;
			continue;
		}
		/* The information method of a step: Name!text() is the text below it. */
		if (c == '!' && at[1] != '=')
		{
			put_text(out, "/");
			at++;
			continue;
		}
		if (c == '[')
		{
			int value;
			const xmlChar *after;
			put_text(out, "[");
			at++;
			if (plain_index(at, &value, &after))
			{
				char number[32];
				wsprintfA(number, "%d", value + 1);
				put_text(out, number);
				at = after;
			}
			continue;
		}
		if (c == '$')
		{
			unsigned index;
			BOOL matched = FALSE;
			for (index = 0; index < ARRAYSIZE(operators); index++)
			{
				int length = (int)strlen(operators[index].word);
				if (!xmlStrncasecmp(at, (const xmlChar *)operators[index].word, length))
				{
					at += length;
					if (operators[index].fold)
					{
						const xmlChar *end = operand_end(at);
						xmlChar *right = xmlStrndup(at, (int)(end - at));
						writer term = {.data = out->data};
						int start = operand_start(out->text, out->used);
						/* XPath orders only numbers, so text goes through a comparison of its own. */
						BOOL ordered = operators[index].xpath[0] == '<' || operators[index].xpath[0] == '>';
						wrap_lowered(out, start);
						if (ordered)
						{
							xmlChar *left = xmlStrdup(out->text + start);
							out->used = start;
							out->text[start] = 0;
							put_text(out, "mx-compare(");
							put_text(out, (const char *)left);
							xmlFree(left);
							put_text(out, ",");
						}
						else
							put_text(out, operators[index].xpath);
						rewrite(right, &term);
						put_text(out, "translate(");
						if (term.text)
							put_bytes(out, (const char *)term.text, term.used);
						put_text(out, ",");
						put_text(out, upper_case);
						put_text(out, ",");
						put_text(out, lower_case);
						put_text(out, ")");
						if (ordered)
						{
							put_text(out, ")");
							put_text(out, operators[index].xpath);
							put_text(out, "0");
						}
						xmlFree(term.text);
						xmlFree(right);
						at = end;
					}
					else
						put_text(out, operators[index].xpath);
					matched = TRUE;
					break;
				}
			}
			if (!matched && !xmlStrncasecmp(at, (const xmlChar *)"$not$", 5))
			{
				const xmlChar *end = operand_end(at + 5);
				xmlChar *term = xmlStrndup(at + 5, (int)(end - at - 5));
				put_text(out, "not(");
				rewrite(term, out);
				put_text(out, ")");
				xmlFree(term);
				at = end;
				matched = TRUE;
			}
			if (!matched && !xmlStrncasecmp(at, (const xmlChar *)"$all$", 5))
			{
				/* Every member has to pass: that is no member failing, and comparing a set finds any member that does. */
				const xmlChar *end = at + 5, *compare = NULL;
				int depth = 0;
				for (; *end && (depth || *end != ']'); end++)
				{
					if (*end == '\'' || *end == '"')
					{
						const xmlChar *close = xmlStrchr(end + 1, *end);
						if (!close)
							break;
						end = close;
					}
					else if (*end == '(' || *end == '[')
						depth++;
					else if (*end == ')' || *end == ']')
						depth--;
					else if (!depth && !compare && (*end == '=' || (*end == '!' && end[1] == '=')))
						compare = end;
				}
				if (compare)
				{
					BOOL different = *compare == '!';
					xmlChar *left = xmlStrndup(at + 5, (int)(compare - at - 5));
					xmlChar *right = xmlStrndup(compare + (different ? 2 : 1), (int)(end - compare - (different ? 2 : 1)));
					put_text(out, "not(");
					rewrite(left, out);
					put_text(out, different ? "=" : "!=");
					rewrite(right, out);
					put_text(out, ")");
					xmlFree(left);
					xmlFree(right);
					at = end;
					continue;
				}
			}
			if (!matched && (!xmlStrncasecmp(at, (const xmlChar *)"$all$", 5) || !xmlStrncasecmp(at, (const xmlChar *)"$any$", 5)))
			{
				int value;
				const xmlChar *after;
				at += 5;
				if (plain_index(at, &value, &after))
				{
					char number[32];
					wsprintfA(number, "%d", value + 1);
					put_text(out, number);
					at = after;
				}
				matched = TRUE;
			}
			if (matched)
				continue;
			put_bytes(out, (const char *)at, 1);
			at++;
			continue;
		}
		if (name_char(c) && !(c >= '0' && c <= '9') && c != '.' && c != '-')
		{
			const xmlChar *end = at;
			while (name_char(*end))
				end++;
			if (*end == '(')
				at = rewrite_function(at, end, out);
			else if (end - at > 2 && xmlStrstr(at, (const xmlChar *)"::") && xmlStrstr(at, (const xmlChar *)"::") < end)
			{
				/* An axis stays as it is; the name after it does not. */
				const xmlChar *axis = xmlStrstr(at, (const xmlChar *)"::") + 2;
				put_bytes(out, (const char *)at, (int)(axis - at));
				if (end > axis && *end != '(')
				{
					put_name_test(out, axis, (int)(end - axis));
					at = end;
				}
				else
					at = axis;
			}
			else if (after_operand(out))
			{
				put_bytes(out, (const char *)at, (int)(end - at));
				at = end;
			}
			else
			{
				put_name_test(out, at, (int)(end - at));
				at = end;
				/* The star of a whole prefix went with it. */
				if (end[-1] == ':' && *end == '*')
					at++;
			}
			continue;
		}
		if ((c == '|' && at[1] == '|') || (c == '&' && at[1] == '&'))
		{
			put_text(out, c == '|' ? " or " : " and ");
			at += 2;
			continue;
		}
		put_bytes(out, (const char *)at, 1);
		at++;
	}
}

xmlChar *mx_pattern_to_xpath(const xmlChar *pattern, const mx_tree *data)
{
	writer out = {.data = data};
	put_text(&out, "");
	rewrite(pattern, &out);
	return out.text;
}

/* --- Evaluation. --- */

static void xpath_node_type(xmlXPathParserContextPtr parser, int count)
{
	xmlNodePtr node = parser->context->node;
	int type = 0;
	if (count)
	{
		xmlXPathSetArityError(parser);
		return;
	}
	switch (node ? node->type : XML_DOCUMENT_NODE)
	{
	case XML_ELEMENT_NODE: type = NODE_ELEMENT; break;
	case XML_ATTRIBUTE_NODE: type = NODE_ATTRIBUTE; break;
	case XML_TEXT_NODE: type = NODE_TEXT; break;
	case XML_CDATA_SECTION_NODE: type = NODE_CDATA_SECTION; break;
	case XML_ENTITY_REF_NODE: type = NODE_ENTITY_REFERENCE; break;
	case XML_PI_NODE: type = NODE_PROCESSING_INSTRUCTION; break;
	case XML_COMMENT_NODE: type = NODE_COMMENT; break;
	case XML_DOCUMENT_NODE: type = NODE_DOCUMENT; break;
	case XML_DOCUMENT_FRAG_NODE: type = NODE_DOCUMENT_FRAGMENT; break;
	default: break;
	}
	xmlXPathReturnNumber(parser, type);
}

static void xpath_compare(xmlXPathParserContextPtr parser, int count)
{
	xmlChar *right, *left;
	if (count != 2)
	{
		xmlXPathSetArityError(parser);
		return;
	}
	right = xmlXPathPopString(parser);
	left = xmlXPathPopString(parser);
	xmlXPathReturnNumber(parser, xmlStrcmp(left, right));
	xmlFree(left);
	xmlFree(right);
}

static void quiet_xpath_errors(void *data, const xmlError *error)
{
}

/* Prefixes come from the document's SelectionNamespaces; with none set, the old language still sees
 * the ones in scope at the context. */
static void register_namespaces(xmlXPathContextPtr context, xmlNodePtr node, const mx_tree *data)
{
	const mx_select_ns *space;
	for (space = data ? data->props->namespaces : NULL; space; space = space->next)
		if (space->prefix)
			xmlXPathRegisterNs(context, space->prefix, space->uri);
	if (!data || !data->props->xpath)
	{
		xmlNodePtr scope = node->type == XML_DOCUMENT_NODE ? xmlDocGetRootElement((xmlDocPtr)node) : node;
		xmlNsPtr *all = scope && scope->type == XML_ELEMENT_NODE ? xmlGetNsList(scope->doc, scope) : NULL;
		int index;
		for (index = 0; all && all[index]; index++)
			if (all[index]->prefix && !xmlXPathNsLookup(context, all[index]->prefix))
				xmlXPathRegisterNs(context, all[index]->prefix, all[index]->href);
		xmlFree(all);
	}
}

static xmlXPathObjectPtr evaluate(xmlNodePtr node, const WCHAR *query)
{
	xmlDocPtr tree = node->doc;
	mx_tree *data = tree ? mx_tree_of(tree) : NULL;
	xmlXPathContextPtr context = xmlXPathNewContext(tree);
	xmlXPathObjectPtr result = NULL;
	xmlChar *text = mx_to_utf8(query), *rewritten = NULL;
	if (!context || !text)
		goto done;
	context->node = node;
	context->error = quiet_xpath_errors;
	register_namespaces(context, node, data);
	xmlXPathRegisterFunc(context, (const xmlChar *)"mx-node-type", xpath_node_type);
	xmlXPathRegisterFunc(context, (const xmlChar *)"mx-compare", xpath_compare);
	if (!data || !data->props->xpath)
		rewritten = mx_pattern_to_xpath(text, data);
	result = xmlXPathEvalExpression(rewritten ? rewritten : text, context);
done:
	xmlFree(rewritten);
	xmlFree(text);
	if (context)
		xmlXPathFreeContext(context);
	return result;
}

/* A found set can name a namespace node, which has no object to stand for it. */
static xmlNodePtr top_of(xmlNodePtr node)
{
	while (node->parent)
		node = node->parent;
	return node;
}

/* Outside the tree, the document stops being reachable: "/" is the top of the subtree and it has no parent. */
static void keep_inside(xmlNodePtr context, const WCHAR *query, xmlNodeSetPtr set)
{
	xmlNodePtr top;
	int index;
	if (!set || context->type == XML_DOCUMENT_NODE || context->type == XML_NAMESPACE_DECL)
		return;
	top = top_of(context);
	if (top->type == XML_DOCUMENT_NODE)
		return;
	while (*query == ' ' || *query == '\t')
		query++;
	for (index = 0; index < set->nodeNr; index++)
		if (set->nodeTab[index]->type == XML_DOCUMENT_NODE)
		{
			if (*query == '/')
				set->nodeTab[index] = top;
			else
			{
				set->nodeTab[index--] = set->nodeTab[--set->nodeNr];
			}
		}
}

static LONG usable_nodes(xmlNodeSetPtr set, xmlNodePtr **out)
{
	xmlNodePtr *items;
	LONG count = 0;
	int index;
	*out = NULL;
	if (!set || !set->nodeNr)
		return 0;
	items = mx_alloc(set->nodeNr * sizeof(*items));
	if (!items)
		return 0;
	for (index = 0; index < set->nodeNr; index++)
		if (set->nodeTab[index]->type != XML_NAMESPACE_DECL)
			items[count++] = set->nodeTab[index];
		else
		{
			/* A selected declaration comes out as the attribute it reads as, and libxml keeps its element in the link. */
			xmlNsPtr space = (xmlNsPtr)set->nodeTab[index];
			xmlNodePtr attribute = space->next ? mx_declaration_attribute((xmlNodePtr)space->next, space) : NULL;
			if (attribute)
				items[count++] = attribute;
		}
	*out = items;
	return count;
}

HRESULT mx_select_nodes(xmlNodePtr context, const WCHAR *query, IXMLDOMNodeList **out)
{
	xmlXPathObjectPtr result = evaluate(context, query);
	xmlNodePtr *items;
	LONG count;
	HRESULT status;
	if (!result)
		return E_FAIL;
	if (result->type != XPATH_NODESET)
	{
		xmlXPathFreeObject(result);
		return E_FAIL;
	}
	keep_inside(context, query, result->nodesetval);
	count = usable_nodes(result->nodesetval, &items);
	status = mx_found_list(context, items, count, out);
	mx_free(items);
	xmlXPathFreeObject(result);
	return status;
}

HRESULT mx_select_single(xmlNodePtr context, const WCHAR *query, IXMLDOMNode **out)
{
	xmlXPathObjectPtr result = evaluate(context, query);
	xmlNodePtr *items;
	LONG count;
	HRESULT status;
	if (!result)
		return E_FAIL;
	if (result->type != XPATH_NODESET)
	{
		xmlXPathFreeObject(result);
		return E_FAIL;
	}
	keep_inside(context, query, result->nodesetval);
	count = usable_nodes(result->nodesetval, &items);
	status = mx_node_return(count ? items[0] : NULL, out);
	mx_free(items);
	xmlXPathFreeObject(result);
	return status;
}

/* --- getElementsByTagName walks the elements and compares written names. --- */

typedef struct
{
	xmlNodePtr *items;
	LONG count, room;
	const xmlChar *name;
	BOOL any;
} name_walk;

static BOOL written_name_is(xmlNodePtr node, const xmlChar *name)
{
	if (node->ns && node->ns->prefix)
	{
		int size = xmlStrlen(node->ns->prefix);
		return !xmlStrncmp(name, node->ns->prefix, size) && name[size] == ':' && !xmlStrcmp(name + size + 1, node->name);
	}
	return !xmlStrcmp(node->name, name);
}

static void walk_elements(xmlNodePtr parent, name_walk *walk)
{
	xmlNodePtr at;
	for (at = parent->children; at; at = at->next)
	{
		/* An empty name asks for every node, whatever its kind. */
		if (at->type == XML_DTD_NODE || (at->type != XML_ELEMENT_NODE && *walk->name))
			continue;
		if (walk->any || !*walk->name || written_name_is(at, walk->name))
		{
			if (walk->count == walk->room)
			{
				walk->room = walk->room ? walk->room * 2 : 16;
				walk->items = mx_realloc(walk->items, walk->room * sizeof(*walk->items));
			}
			walk->items[walk->count++] = at;
		}
		if (at->type == XML_ELEMENT_NODE)
			walk_elements(at, walk);
	}
}

HRESULT mx_select_by_name(xmlNodePtr context, const WCHAR *name, IXMLDOMNodeList **out)
{
	xmlChar *utf8 = mx_to_utf8(name);
	name_walk walk = {0};
	HRESULT status;
	walk.name = utf8;
	walk.any = !xmlStrcmp(utf8, (const xmlChar *)"*");
	walk_elements(context, &walk);
	status = mx_found_list(context, walk.items, walk.count, out);
	mx_free(walk.items);
	xmlFree(utf8);
	return status;
}
