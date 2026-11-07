/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_JSCRIPT_JS_H
#define RAVEN_JSCRIPT_JS_H

#define COBJMACROS
#include <windows.h>
#include <oleauto.h>
#include <activscp.h>
#include <dispex.h>
#include <objsafe.h>

/* activscp.h names the parse interface by pointer size; the source context cookie follows it. */
#ifdef _WIN64
typedef IActiveScriptParse64Vtbl IActiveScriptParseVtbl;
typedef DWORDLONG script_context;
#else
typedef IActiveScriptParse32Vtbl IActiveScriptParseVtbl;
typedef DWORD script_context;
#endif

/* Memory, from the process heap; js_alloc zeroes what it hands out. */
void *js_alloc(SIZE_T size);
void *js_grow(void *block, SIZE_T size);
void js_free(void *block);
WCHAR *js_wcsdup(const WCHAR *text);

typedef struct engine engine;
typedef struct js_object js_object;

/* Strings are counted and shared; text is always followed by a NUL. */
typedef struct js_string {
	LONG refs;
	UINT length;
	WCHAR text[1];
} js_string;

js_string *js_string_new(const WCHAR *text, UINT length);
js_string *js_string_of(const WCHAR *text);
js_string *js_string_concat(const js_string *a, const js_string *b);
static inline js_string *js_string_retain(js_string *s) { if (s) InterlockedIncrement(&s->refs); return s; }
void js_string_release(js_string *s);
BOOL js_string_equal(const js_string *a, const js_string *b);
BOOL js_string_is(const js_string *s, const WCHAR *text);
int js_string_compare(const js_string *a, const js_string *b);

/* A growing buffer of text. */
typedef struct {
	WCHAR *text;
	UINT length, room;
} js_buffer;

void js_buffer_add(js_buffer *b, const WCHAR *text, UINT length);
void js_buffer_text(js_buffer *b, const WCHAR *text);
void js_buffer_char(js_buffer *b, WCHAR c);
js_string *js_buffer_string(js_buffer *b);

/* Values. A date is the variant date COM hands over, kept as milliseconds since 1970 in UTC as a Date object keeps it. */
typedef enum { JV_UNDEFINED, JV_NULL, JV_BOOL, JV_NUMBER, JV_STRING, JV_OBJECT, JV_DATE } js_type;

typedef struct {
	js_type type;
	union {
		BOOL boolean;
		double number;
		js_string *string;
		js_object *object;
	};
} js_value;

extern const js_value js_undefined;
extern const js_value js_null;
static inline js_value js_number(double n) { js_value v = { JV_NUMBER }; v.number = n; return v; }
static inline js_value js_bool(BOOL b) { js_value v = { JV_BOOL }; v.boolean = b ? TRUE : FALSE; return v; }
/* Takes the reference passed in. */
static inline js_value js_str(js_string *s) { js_value v = { JV_STRING }; v.string = s; return v; }
static inline js_value js_obj(js_object *o) { js_value v = { JV_OBJECT }; v.object = o; return v; }
js_value js_text(const WCHAR *text);
js_value js_retain(js_value v);
void js_release(js_value v);

/* Numbers as text and back, as ECMAScript writes and reads them. */
js_string *js_number_string(double n);
void js_number_text(double n, WCHAR *out);
double js_string_number(const js_string *s);
double js_nan(void);
double js_infinity(void);
BOOL js_is_nan(double n);
BOOL js_is_finite(double n);
double js_integer(double n);
/* The C runtime's math functions, looked up in msvcrt.dll: MinGW's static library would bring its own versions, which need the
 * startup code a DLL without the C runtime's entry does not run. */
double js_crt_math(const char *name, double x);
double js_crt_pow(double x, double y);
/* Decimal text of digits, point and exponent, as the nearest double. */
double js_strtod(const char *text);
/* The shortest digits that read back as v, and the exact digits of v rounded half up to count of them; v is positive and finite, and
 * v = 0.digits * 10^point. */
int js_shortest_digits(double v, char *digits, int *point);
int js_exact_digits(double v, int count, char *digits, int *point);
/* The exact digits of v rounded half up at fraction places after the decimal point. */
int js_fixed_digits(double v, int fraction, char *digits, int *point);
INT32 js_int32(double n);
UINT32 js_uint32(double n);

/* Objects. */

typedef enum {
	JC_OBJECT, JC_FUNCTION, JC_ARRAY, JC_STRING, JC_NUMBER, JC_BOOLEAN, JC_DATE, JC_REGEXP, JC_ERROR, JC_ARGUMENTS, JC_MATH, JC_GLOBAL,
	JC_ACTIVATION, JC_DISPATCH, JC_ENUMERATOR, JC_VBARRAY,
} js_class;

#define JP_DONTENUM 1
#define JP_READONLY 2
#define JP_DONTDELETE 4
#define JP_HIDDEN (JP_DONTENUM | JP_DONTDELETE)

typedef struct {
	js_string *name;
	js_value value;
	DWORD flags;
	UINT hash;
} js_property;

/* A native function: this, the arguments and where its result goes. FALSE means it threw, with the exception in the engine. */
typedef BOOL (*js_native)(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee);

typedef struct function_info function_info;
typedef struct js_scope js_scope;

struct js_object {
	LONG refs;
	js_class cls;
	engine *en;
	js_object *proto;
	js_property *props;
	UINT count, room;
	UINT *index;
	UINT index_size;
	/* Arrays: the elements by index, holes as JV_UNDEFINED with present cleared. */
	js_value *elements;
	BYTE *present;
	UINT length, elements_room;
	/* Functions: native ones, or script ones with the scope they close over. */
	js_native native;
	UINT native_length;
	const void *native_data;
	function_info *code;
	js_scope *scope;
	js_object *bound;
	/* Number, String, Boolean, Date: the primitive inside. */
	js_value primitive;
	/* RegExp. */
	struct rx *regex;
	js_string *source;
	DWORD rx_flags;
	/* Objects of the host, enumerators and safe arrays. */
	IDispatch *dispatch;
	IEnumVARIANT *enumerator;
	VARIANT item;
	BOOL at_end;
	SAFEARRAY *array;
	/* The IDispatchEx the host sees, made when first asked for. */
	struct outward *outward;
	/* Every object of an engine, so closing it can break the cycles between them. */
	js_object *next_object, *previous_object;
};

js_object *js_object_new(engine *en, js_class cls, js_object *proto);
static inline js_object *js_object_retain(js_object *o) { if (o) InterlockedIncrement(&o->refs); return o; }
void js_object_release(js_object *o);

/* Own properties. */
js_property *js_own(js_object *o, const js_string *name);
void js_define(js_object *o, js_string *name, js_value value, DWORD flags);
void js_define_text(js_object *o, const WCHAR *name, js_value value, DWORD flags);
js_object *js_define_native(js_object *o, const WCHAR *name, js_native fn, UINT length);
BOOL js_remove_own(js_object *o, const js_string *name);

/* Array indexes: TRUE with the index for a canonical array index name. */
BOOL js_array_index(const js_string *name, UINT32 *index);
void js_array_set_length(js_object *a, UINT32 length);
void js_array_push(js_object *a, js_value value);
js_object *js_array_new(engine *en, UINT length);

/* The property operations of ECMAScript, through the prototype chain and, for the host's objects, through IDispatch. */
BOOL js_get(engine *en, js_object *o, js_string *name, js_value *result);
BOOL js_get_text(engine *en, js_object *o, const WCHAR *name, js_value *result);
BOOL js_get_index(engine *en, js_object *o, UINT32 index, js_value *result);
BOOL js_put(engine *en, js_object *o, js_string *name, js_value value);
BOOL js_put_text(engine *en, js_object *o, const WCHAR *name, js_value value);
BOOL js_put_index(engine *en, js_object *o, UINT32 index, js_value value);
BOOL js_has(engine *en, js_object *o, js_string *name);
BOOL js_has_own(engine *en, js_object *o, js_string *name);
/* Empties every object of a closing engine, breaking the cycles between them. */
void js_free_objects(engine *en);
BOOL js_delete(engine *en, js_object *o, js_string *name, BOOL *deleted);
/* The names for-in visits: enumerable own and inherited properties. */
js_string **js_enumerate(engine *en, js_object *o, UINT *count);

/* Functions. */
js_object *js_native_new(engine *en, js_native fn, UINT length, const void *data);
BOOL js_is_callable(const js_value v);
BOOL js_call(engine *en, js_value fn, js_value self, UINT argc, js_value *argv, js_value *result);
BOOL js_construct(engine *en, js_value fn, UINT argc, js_value *argv, js_value *result);

/* Conversions; the ones that may call script code report a throw with FALSE. */
BOOL js_to_primitive(engine *en, js_value v, int hint, js_value *result);
#define HINT_NONE 0
#define HINT_NUMBER 1
#define HINT_STRING 2
BOOL js_to_number(engine *en, js_value v, double *result);
BOOL js_to_string(engine *en, js_value v, js_string **result);
BOOL js_to_object(engine *en, js_value v, js_object **result);
BOOL js_to_boolean(js_value v);
BOOL js_to_int32(engine *en, js_value v, INT32 *result);
BOOL js_to_uint32(engine *en, js_value v, UINT32 *result);
BOOL js_to_integer(engine *en, js_value v, double *result);
BOOL js_strict_equal(js_value a, js_value b);
BOOL js_loose_equal(engine *en, js_value a, js_value b, BOOL *result);
const WCHAR *js_typeof(js_value v);

/* Exceptions: the value thrown waits in the engine while the C code unwinds. */
BOOL js_throw(engine *en, js_value v);
/* Throws a new error object of a kind (L"TypeError" and the others) with a JScript error number and its text. */
BOOL js_throw_error(engine *en, const WCHAR *kind, LONG number, const WCHAR *argument);
BOOL js_throw_hresult(engine *en, HRESULT hr, const EXCEPINFO *info);
js_object *js_error_new(engine *en, const WCHAR *kind, LONG number, const WCHAR *text);

/* JScript's error numbers, without the facility (0x800A0000 is added). */
#define JS_E_INVALID_CALL 5
#define JS_E_OUT_OF_MEMORY 7
#define JS_E_SUBSCRIPT 9
#define JS_E_TYPE_MISMATCH 13
#define JS_E_OUT_OF_STACK 28
#define JS_E_CANNOT_CREATE 429
#define JS_E_NO_PROPERTY 438
#define JS_E_NUMBER_EXPECTED 5001
#define JS_E_FUNCTION_EXPECTED 5002
#define JS_E_STRING_EXPECTED 5005
#define JS_E_ASSIGN_RESULT 5003
#define JS_E_DATE_EXPECTED 5006
#define JS_E_OBJECT_EXPECTED 5007
#define JS_E_ILLEGAL_ASSIGN 5008
#define JS_E_UNDEFINED 5009
#define JS_E_BOOLEAN_EXPECTED 5010
#define JS_E_JSCRIPT_OBJECT 5014
#define JS_E_ENUMERATOR_EXPECTED 5015
#define JS_E_REGEXP_EXPECTED 5016
#define JS_E_REGEXP_SYNTAX 5017
#define JS_E_NOT_CAUGHT 5022
#define JS_E_ARRAY_LENGTH 5029
#define JS_E_ARRAY_EXPECTED 5028
#define JS_E_VBARRAY_EXPECTED 5013
#define JS_E_INVALID_URI 5021
#define JS_E_PRECISION 5025
#define JS_E_FRACTION 5026
#define JS_E_RADIX 5027
#define JS_E_SYNTAX 1002
#define JS_E_EXPECTED_SEMICOLON 1004
#define JS_E_EXPECTED_LPAREN 1005
#define JS_E_EXPECTED_RPAREN 1006
#define JS_E_EXPECTED_RBRACKET 1007
#define JS_E_EXPECTED_LBRACE 1008
#define JS_E_EXPECTED_RBRACE 1009
#define JS_E_EXPECTED_IDENTIFIER 1010
#define JS_E_EXPECTED_ASSIGN 1011
#define JS_E_INVALID_CHAR 1014
#define JS_E_UNTERMINATED_STRING 1015
#define JS_E_UNTERMINATED_COMMENT 1016
#define JS_E_RETURN_OUTSIDE 1018
#define JS_E_BREAK_OUTSIDE 1019
#define JS_E_CONTINUE_OUTSIDE 1020
#define JS_E_EXPECTED_WHILE 1024
#define JS_E_LABEL_REDEFINED 1025
#define JS_E_LABEL_NOT_FOUND 1026
#define JS_E_EXPECTED_COLON 1003
#define JS_E_EXPECTED_MEMBER 1028
#define JS_E_UNTERMINATED_REGEXP 1015
#define JS_E_EXPECTED_CATCH 1031
#define JS_E_EXPECTED_ESCAPE 1032
const WCHAR *js_error_text(LONG number);
#define JS_HRESULT(number) ((HRESULT)(0x800A0000 | (number)))

/* Syntax. */

typedef enum {
	/* Expressions. */
	N_NUMBER, N_STRING, N_REGEXP, N_NAME, N_THIS, N_NULL, N_TRUE, N_FALSE, N_ARRAY, N_OBJECT, N_FUNCTION, N_MEMBER, N_INDEX, N_CALL,
	N_NEW, N_POSTFIX, N_PREFIX, N_UNARY, N_BINARY, N_LOGICAL, N_CONDITIONAL, N_ASSIGN, N_COMMA,
	/* Statements. */
	N_BLOCK, N_VAR, N_EMPTY, N_EXPRESSION, N_IF, N_DO, N_WHILE, N_FOR, N_FOR_IN, N_CONTINUE, N_BREAK, N_RETURN, N_WITH, N_SWITCH,
	N_LABEL, N_THROW, N_TRY, N_FUNCTION_DECLARATION,
} node_kind;

/* Operators, as the tokens that write them. */
typedef enum {
	OP_NONE, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_SHL, OP_SAR, OP_SHR, OP_LT, OP_GT, OP_LE, OP_GE, OP_INSTANCEOF, OP_IN, OP_EQ,
	OP_NE, OP_SEQ, OP_SNE, OP_BITAND, OP_BITXOR, OP_BITOR, OP_AND, OP_OR, OP_NOT, OP_BITNOT, OP_NEG, OP_PLUS, OP_TYPEOF, OP_VOID,
	OP_DELETE, OP_INC, OP_DEC,
} js_op;

typedef struct node node;
struct node {
	node_kind kind;
	js_op op;
	UINT line, column;
	double number;
	js_string *name;
	node *a, *b, *c, *d;
	/* Arguments, elements, statements, cases, declarations; for an object literal the names in names. */
	node **list;
	js_string **names;
	UINT count;
	function_info *function;
	/* Regular expression literals: the flags; var declarations: each one's initializer is in list, its name in names. */
	DWORD flags;
};

struct function_info {
	js_string *name;
	js_string **params;
	UINT param_count;
	node **body;
	UINT body_count;
	/* The names its var statements and function declarations make, found before it runs. */
	js_string **vars;
	UINT var_count;
	node **functions;
	UINT function_count;
	/* Its text, for toString. */
	js_string *source;
	BOOL uses_arguments, uses_eval, is_expression;
};

typedef struct {
	LONG number;
	UINT line, column;
} parse_error;

/* A parsed piece of code: the program as a function of no parameters, and the memory of all its nodes. */
typedef struct script_code {
	WCHAR *source;
	function_info *program;
	void *arena;
	struct script_code *next;
} script_code;

script_code *js_parse(const WCHAR *source, BOOL expression, parse_error *error);
void js_code_free(script_code *code);

/* Running. */

struct js_scope {
	LONG refs;
	js_object *object;
	js_scope *outer;
};

js_scope *js_scope_new(js_object *object, js_scope *outer);
void js_scope_release(js_scope *scope);

/* What a statement ended with. */
typedef enum { C_NORMAL, C_BREAK, C_CONTINUE, C_RETURN, C_THROW } completion;

/* The context code runs in: its scope, its this, the object its var declarations land in. */
typedef struct context {
	js_scope *scope;
	js_value self;
	js_object *variables;
	struct context *outer;
} context;

/* Runs a program in a context; the value of its last expression statement goes to result. */
BOOL js_run_program(engine *en, function_info *program, context *ctx, js_value *result);
/* Turns the function declarations and var names of code into properties of the context's variable object. */
void js_hoist(engine *en, function_info *code, context *ctx);
js_object *js_function_new(engine *en, function_info *code, js_scope *scope);

/* The host: named items, objects made through COM, variants. */

typedef struct {
	WCHAR *name;
	js_string *key;
	DWORD flags;
	IDispatch *object;
	BOOL fetched;
} named_item;

typedef struct {
	LONG number;
	BSTR description, source;
	UINT line, column;
} script_failure;

void js_failure_clear(script_failure *failure);

struct engine {
	IActiveScript IActiveScript_iface;
	IActiveScriptParse IActiveScriptParse_iface;
	IObjectSafety IObjectSafety_iface;
	LONG refs;
	SCRIPTSTATE state;
	IActiveScriptSite *site;
	DWORD safety, thread;
	named_item *items;
	UINT item_count;
	/* Code waiting for the start, and all the code parsed, which lives as long as the engine. */
	script_code **pending;
	UINT pending_count;
	script_code *kept;
	/* The context of the code that runs, for the errors the site is told of. */
	script_code *running;
	script_context context;
	ULONG start_line;
	UINT line, column;
	UINT depth;
	/* The thrown value, while C code unwinds; the line it was thrown at. */
	BOOL throwing;
	js_value exception;
	UINT exception_line, exception_column;
	/* The global object, the prototypes and constructors. */
	js_object *global;
	js_object *object_prototype, *function_prototype, *array_prototype, *string_prototype, *number_prototype, *boolean_prototype,
		*date_prototype, *regexp_prototype, *error_prototype, *enumerator_prototype, *vbarray_prototype;
	js_object *regexp_constructor;
	js_object *error_prototypes[7];
	js_object *objects;
	context *current;
	BOOL closing;
	/* Names given dispatch identifiers by the objects the host sees. */
	js_string **dispatch_names;
	UINT dispatch_name_count;
	/* The last regular expression match, for RegExp.$1 and the rest. */
	js_string *last_input;
	int *last_spans;
	UINT last_groups;
	double random_state;
	IDispatch *global_dispatch;
};

void js_init_runtime(engine *en);
void js_free_runtime(engine *en);
IDispatch *js_item_object(engine *en, UINT index);
HRESULT js_create_object(const WCHAR *name, IDispatch **out);
HRESULT js_report(engine *en, const script_failure *failure);
/* Takes the engine's exception and describes it for the site. */
void js_failure_of_exception(engine *en, script_failure *failure);
/* Compiles and runs code given as text, as eval and the Function constructor do. */
BOOL js_eval_text(engine *en, const js_string *source, context *ctx, js_value *result);

/* Variants. */
BOOL js_from_variant(engine *en, const VARIANT *v, js_value *result);
HRESULT js_to_variant(engine *en, js_value v, VARIANT *result);
js_object *js_dispatch_object(engine *en, IDispatch *dispatch);
/* The IDispatchEx of a script object, with a reference for the caller. */
IDispatch *js_outward(js_object *o);
void js_outward_detach(js_object *o);
BOOL js_dispatch_get(engine *en, js_object *o, js_string *name, js_value *result);
BOOL js_dispatch_put(engine *en, js_object *o, js_string *name, js_value value);
/* obj.name(args) = value, or obj(args) = value without a name: a put with indexes, which JScript allows on the host's objects. */
BOOL js_dispatch_put_at(engine *en, js_object *o, js_string *name, UINT argc, js_value *argv, js_value value);
BOOL js_dispatch_has(engine *en, js_object *o, js_string *name);
BOOL js_dispatch_call(engine *en, IDispatch *dispatch, DISPID id, WORD flags, UINT argc, js_value *argv, js_value *result);
BOOL js_dispatch_invoke_name(engine *en, js_object *o, js_string *name, UINT argc, js_value *argv, js_value *result);
DISPID js_dispatch_id(engine *en, const js_string *name);
js_string *js_dispatch_name(engine *en, DISPID id);
IDispatch *js_global_dispatch_new(engine *en);

/* The built-in objects; each installs its constructor and prototype into the global object. */
void js_init_object(engine *en);
void js_init_function(engine *en);
void js_init_array(engine *en);
void js_init_string(engine *en);
void js_init_number(engine *en);
void js_init_boolean(engine *en);
void js_init_math(engine *en);
void js_init_date(engine *en);
void js_init_regexp(engine *en);
void js_init_error(engine *en);
void js_init_global(engine *en);
void js_init_host(engine *en);

/* The pieces of the built-ins other built-ins use. */
js_object *js_regexp_new(engine *en, const js_string *pattern, DWORD flags, BOOL *failed);
BOOL js_regexp_exec(engine *en, js_object *re, js_string *text, BOOL update_last_index, js_value *result);
BOOL js_regexp_flags(engine *en, const js_string *flags, DWORD *out);
BOOL js_string_match(engine *en, js_object *re, js_string *text, js_value *result);
BOOL js_string_replace(engine *en, js_value self, UINT argc, js_value *argv, js_value *result);
BOOL js_string_split(engine *en, js_string *text, js_value separator, UINT32 limit, js_value *result);
BOOL js_string_search(engine *en, js_object *re, js_string *text, js_value *result);
js_string *js_date_string(double time, int which);
double js_date_now(void);
double js_date_parse(const js_string *text);
double js_from_var_date(double date);
double js_to_var_date(double time);
js_string *js_error_message(engine *en, js_object *error);
js_string *js_object_class_name(js_object *o);
void js_record_match(engine *en, js_string *input, const int *spans, UINT groups);
#define RXF_GLOBAL 0x100

/* The argument at an index, undefined past the end. */
static inline js_value js_arg(UINT argc, js_value *argv, UINT index) { return index < argc ? argv[index] : js_undefined; }

#endif
