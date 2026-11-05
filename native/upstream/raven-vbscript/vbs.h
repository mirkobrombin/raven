#ifndef RAVEN_VBSCRIPT_VBS_H
#define RAVEN_VBSCRIPT_VBS_H

#define COBJMACROS
#include <windows.h>
#include <oleauto.h>
#include <activscp.h>
#include <objsafe.h>

/* activscp.h names the parse interface by pointer size; the source context cookie follows it. */
#ifdef _WIN64
typedef IActiveScriptParse64Vtbl IActiveScriptParseVtbl;
typedef DWORDLONG script_context;
#else
typedef IActiveScriptParse32Vtbl IActiveScriptParseVtbl;
typedef DWORD script_context;
#endif

/* Memory of the engine, from the process heap; vbs_alloc zeroes what it hands out. */
void *vbs_alloc(SIZE_T size);
void *vbs_grow(void *block, SIZE_T size);
void vbs_free(void *block);
WCHAR *vbs_copy(const WCHAR *text);
WCHAR *vbs_copy_n(const WCHAR *text, SIZE_T length);
BOOL vbs_same(const WCHAR *a, const WCHAR *b);
WCHAR vbs_lower(WCHAR c);

/* The lexer's tokens. A colon ends a statement as a line end does and is told apart for the one-line If. */
typedef enum { T_END, T_LINE, T_NAME, T_NUMBER, T_TEXT, T_DATE, T_SYMBOL } token_kind;

typedef struct {
	token_kind kind;
	WCHAR *text;
	VARIANT number;
	WCHAR symbol[3];
	BOOL colon;
	/* Blank space came before the token: "a .b" in a With block is a call of a with .b, not a member of a. */
	BOOL spaced;
	UINT line, column;
} token;

typedef struct {
	token *items;
	UINT count, room;
} token_list;

/* Fails with the line and column of the character it could not read. */
BOOL vbs_tokenize(const WCHAR *code, token_list *list, UINT *line, UINT *column);
void vbs_tokens_free(token_list *list);

/* Expressions and statements; a parsed script owns all of them and frees them at once. */
typedef enum { E_CONSTANT, E_NAME, E_MEMBER, E_CALL, E_UNARY, E_BINARY, E_NOTHING, E_ME, E_NEW, E_WITH } expr_kind;

typedef struct expr {
	expr_kind kind;
	UINT line;
	VARIANT constant;
	WCHAR *name;
	struct expr *target, *left, *right;
	struct expr **args;
	UINT arg_count;
	WCHAR op[5];
	BOOL parenthesized;
} expr;

typedef enum {
	S_CALL, S_ASSIGN, S_SET, S_IF, S_FOR, S_FOR_EACH, S_DO, S_WHILE, S_EXIT, S_DIM, S_REDIM, S_ERASE, S_CONST, S_ON_ERROR, S_SELECT,
	S_WITH, S_RANDOMIZE, S_EXECUTE
} stmt_kind;

typedef struct block {
	struct stmt **items;
	UINT count;
} block;

typedef struct stmt {
	stmt_kind kind;
	UINT line;
	expr *target, *value, *to, *step;
	WCHAR *name;
	/* If: one condition and one block per branch; Do: the one condition; Select: per Case, its values. */
	expr **conditions;
	block *blocks;
	UINT branch_count;
	block otherwise;
	BOOL has_otherwise, until, test_first, resume_next, preserve, global;
	WCHAR exit_kind[12];
	expr ***case_values;
	UINT *case_counts;
	/* Dim, ReDim, Const: the names, with the sizes of each one's dimensions or its value. */
	WCHAR **names;
	expr ***bounds;
	UINT *bound_counts;
	BOOL *arrays;
	UINT name_count;
} stmt;

typedef enum { P_SUB, P_FUNCTION, P_GET, P_LET, P_SET } procedure_kind;

typedef struct procedure {
	procedure_kind kind;
	WCHAR *name;
	WCHAR **params;
	BOOL *by_value;
	UINT param_count;
	block body;
	BOOL is_default, is_private;
} procedure;

/* A class: its fields and its procedures, Class_Initialize and Class_Terminate among them. */
typedef struct class_def {
	WCHAR *name;
	WCHAR **fields;
	expr ***field_bounds;
	UINT *field_bound_counts;
	BOOL *field_arrays, *field_private;
	UINT field_count;
	procedure **procedures;
	UINT procedure_count;
} class_def;

typedef struct script_code {
	void **owned;
	UINT owned_count, owned_room;
	procedure **procedures;
	UINT procedure_count;
	class_def **classes;
	UINT class_count;
	block main;
	BOOL explicit_names;
	/* For an expression parsed alone, the expression. */
	expr *expression;
	WCHAR *source;
} script_code;

typedef struct {
	WCHAR text[160];
	UINT line, column;
	LONG number;
} parse_error;

/* Parses statements, or one expression when expression is set. NULL with error filled on a syntax error. */
script_code *vbs_parse(const WCHAR *code, BOOL expression, parse_error *error);
void vbs_code_free(script_code *code);

/* The runtime. */
typedef struct engine engine;
typedef struct frame frame;

typedef struct variable {
	WCHAR *name;
	VARIANT value;
	BOOL constant, is_private;
	struct variable *next;
} variable;

/* A script's error as Err holds it, and as the host is told of one nothing caught. */
typedef struct {
	LONG number;
	BSTR description, source;
	UINT line, column;
} script_failure;

/* Runs parsed code at global level; the failure is filled when it returns FALSE. */
BOOL vbs_run(engine *en, script_code *code, script_failure *failure);
/* Evaluates the expression of code parsed with expression set. */
BOOL vbs_evaluate(engine *en, script_code *code, VARIANT *result, script_failure *failure);
/* Calls a global procedure by name with arguments in the order of the call (not of DISPPARAMS). */
BOOL vbs_call_global(engine *en, const WCHAR *name, VARIANT *args, UINT count, VARIANT *result, script_failure *failure);
variable *vbs_global_variable(engine *en, const WCHAR *name);
procedure *vbs_global_procedure(engine *en, const WCHAR *name);
void vbs_runtime_free(engine *en);

/* What the runtime asks of the engine it runs in. */
typedef struct {
	IDispatch *object;
	DWORD flags;
	BOOL global_members, fetched;
	WCHAR *name;
} named_item;

struct engine {
	LONG refs;
	IActiveScript IActiveScript_iface;
	IActiveScriptParse IActiveScriptParse_iface;
	IObjectSafety IObjectSafety_iface;
	IDispatch *global_dispatch;
	IActiveScriptSite *site;
	SCRIPTSTATE state;
	DWORD safety;
	named_item *items;
	UINT item_count;
	script_code **pending;
	UINT pending_count;
	script_code **kept;
	UINT kept_count;
	variable *globals;
	procedure **procedures;
	UINT procedure_count;
	class_def **classes;
	UINT class_count;
	BOOL explicit_names;
	/* Where the code running now came from, for the lines an error reports. */
	script_context context;
	ULONG start_line;
	script_code *running;
	/* The Err object's state and the current depth of calls. */
	script_failure err;
	UINT depth;
	DWORD thread;
	ULONG random_seed;
};

void vbs_failure_clear(script_failure *failure);
/* Tells the site of an error nothing caught; returns the HRESULT the caller hands back. */
HRESULT vbs_report(engine *en, const script_failure *failure);
void vbs_add_code(engine *en, script_code *code);
/* A named item's object, asked of the site the first time it is used; NULL when the site has none. */
IDispatch *vbs_item_object(engine *en, UINT index);
/* CreateObject: an object of a ProgID or {CLSID} through COM. */
HRESULT vbs_create_object(engine *en, const WCHAR *name, IDispatch **out);
/* GetRef: an object whose default method calls the script's procedure of this name; NULL when there is none. */
IDispatch *vbs_function_ref(engine *en, const WCHAR *name);
/* A new RegExp object. */
IDispatch *vbs_regexp_new(void);
/* Eval and Execute: parses text and evaluates or runs it at global level. */
BOOL vbs_execute_text(engine *en, const WCHAR *source, BOOL expression, VARIANT *result, script_failure *failure);

/* Built-in functions; FALSE when the name is not one. The arguments are values already. */
typedef struct builtin_call {
	engine *en;
	VARIANT *args;
	UINT count;
	VARIANT *result;
	script_failure *failure;
} builtin_call;

BOOL vbs_builtin(const WCHAR *name, builtin_call *call, BOOL *found);
/* The Err object, a new reference to an object reading and changing the engine's error state. */
IDispatch *vbs_err_object(engine *en);
/* The name of a class the script made, for TypeName; NULL for objects of other kinds. */
BSTR vbs_class_name(IDispatch *object);

/* Raises a runtime error into failure; returns FALSE so callers can return it. */
BOOL vbs_raise(script_failure *failure, LONG number, const WCHAR *description);
/* The text VBScript gives a runtime error number. */
const WCHAR *vbs_error_text(LONG number);
/* The value of a variant: the default property of an object, what a reference points at. */
HRESULT vbs_value_of(const VARIANT *in, VARIANT *out);
HRESULT vbs_to_text(const VARIANT *in, BSTR *out);
HRESULT vbs_to_number(const VARIANT *in, double *out);
HRESULT vbs_to_long(const VARIANT *in, LONG *out);
HRESULT vbs_to_bool(const VARIANT *in, BOOL *out);
/* Calls a member of an object through IDispatch. */
HRESULT vbs_invoke(IDispatch *object, const WCHAR *name, WORD flags, VARIANT *args, UINT count, VARIANT *result, script_failure *failure);

/* Script errors as Windows numbers them. */
#define VBS_E_TYPE_MISMATCH 13
#define VBS_E_SUBSCRIPT 9
#define VBS_E_DIVISION 11
#define VBS_E_OVERFLOW 6
#define VBS_E_ARGUMENT 5
#define VBS_E_OBJECT_REQUIRED 424
#define VBS_E_UNDEFINED 500
#define VBS_E_NOT_SUPPORTED 438
#define VBS_E_CANNOT_CREATE 429
#define VBS_E_WRONG_ARGUMENTS 450
#define VBS_E_OUT_OF_MEMORY 7
#define VBS_E_NAME_REDEFINED 1041
#define VBS_E_SYNTAX 1002

#endif
