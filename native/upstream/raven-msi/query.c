/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

// A view holds its result as tuples of row pointers, one per table, worked out in one go when it executes.
typedef struct {
	UINT table, column;
} column_ref;

typedef struct {
	msi_object base;
	msi_db *db;
	sql_statement *statement;
	db_table *tables[32];
	column_ref *columns;
	UINT column_count;
	db_row **tuples;
	UINT tuple_count, tuple_room;
	UINT position;
	BOOL executed;
	BOOL fetched_one;
	msi_record *params;
	WCHAR *error_column;
	MSIDBERROR error;
	db_table *unheld;
} msi_view;

static BOOL is_virtual_name(const WCHAR *name)
{
	return name[0] == L'_';
}

static BOOL resolve_column(msi_view *view, const WCHAR *table, const WCHAR *name, UINT *table_index, UINT *column_index)
{
	sql_statement *s = view->statement;
	for (UINT t = 0; t < s->table_count; t++) {
		if (table && lstrcmpW(table, view->tables[t]->name))
			continue;
		for (UINT c = 0; c < view->tables[t]->column_count; c++)
			if (!lstrcmpW(view->tables[t]->columns[c].name, name)) {
				*table_index = t;
				*column_index = c;
				return TRUE;
			}
	}
	return FALSE;
}

static BOOL resolve_expr(msi_view *view, sql_expr *expr)
{
	if (!expr)
		return TRUE;
	if (expr->kind == EXPR_COLUMN)
		return resolve_column(view, expr->table, expr->name, &expr->table_index, &expr->column_index);
	if (!resolve_expr(view, expr->left) || !resolve_expr(view, expr->right))
		return FALSE;
	// Strings compare for equality and nothing else.
	if (expr->kind >= OP_LT && expr->kind <= OP_GE) {
		const sql_expr *sides[2] = {expr->left, expr->right};
		for (UINT i = 0; i < 2; i++) {
			if (sides[i]->kind == EXPR_TEXT)
				return FALSE;
			if (sides[i]->kind == EXPR_COLUMN && (view->tables[sides[i]->table_index]->columns[sides[i]->column_index].type & TYPE_STRING))
				return FALSE;
		}
	}
	return TRUE;
}

typedef struct {
	BOOL null, is_text;
	INT number;
	const WCHAR *text;
} expr_value;

static expr_value eval_operand(msi_view *view, const sql_expr *expr, db_row **tuple)
{
	expr_value value = {0};
	switch (expr->kind) {
	case EXPR_COLUMN: {
		const db_column *column = &view->tables[expr->table_index]->columns[expr->column_index];
		UINT cell = tuple[expr->table_index]->cells[expr->column_index];
		if (!cell) {
			value.null = TRUE;
			value.is_text = (column->type & TYPE_STRING) != 0;
		} else if (column->type & TYPE_STRING) {
			value.is_text = TRUE;
			value.text = pool_text(view->db, cell);
		} else {
			value.number = cell_to_int(column->type, cell);
		}
		break;
	}
	case EXPR_NUMBER:
		value.number = expr->number;
		break;
	case EXPR_TEXT:
		value.is_text = TRUE;
		value.text = expr->name;
		if (!*value.text)
			value.null = TRUE;
		break;
	case EXPR_PARAM: {
		msi_record *params = view->params;
		if (!params || record_is_null(params, expr->param)) {
			value.null = TRUE;
		} else if (params->fields[expr->param].type == FIELD_INT) {
			value.number = params->fields[expr->param].number;
		} else {
			value.is_text = TRUE;
			value.text = params->fields[expr->param].text;
		}
		break;
	}
	default:
		value.null = TRUE;
	}
	return value;
}

static BOOL eval_condition(msi_view *view, const sql_expr *expr, db_row **tuple)
{
	if (!expr)
		return TRUE;
	switch (expr->kind) {
	case OP_AND:
		return eval_condition(view, expr->left, tuple) && eval_condition(view, expr->right, tuple);
	case OP_OR:
		return eval_condition(view, expr->left, tuple) || eval_condition(view, expr->right, tuple);
	case OP_IS_NULL:
		return eval_operand(view, expr->left, tuple).null;
	case OP_NOT_NULL:
		return !eval_operand(view, expr->left, tuple).null;
	default:
		break;
	}
	expr_value left = eval_operand(view, expr->left, tuple), right = eval_operand(view, expr->right, tuple);
	if (left.null || right.null) {
		BOOL both = left.null && right.null;
		return expr->kind == OP_EQ ? both : expr->kind == OP_NE ? !both : FALSE;
	}
	int order;
	if (left.is_text && right.is_text)
		order = wcscmp(left.text, right.text);
	else if (left.is_text || right.is_text)
		return expr->kind == OP_NE;
	else
		order = left.number < right.number ? -1 : left.number > right.number;
	switch (expr->kind) {
	case OP_EQ: return order == 0;
	case OP_NE: return order != 0;
	case OP_LT: return order < 0;
	case OP_GT: return order > 0;
	case OP_LE: return order <= 0;
	case OP_GE: return order >= 0;
	default: return FALSE;
	}
}

// The first table a condition reads: tables bind from the last one named down, and each condition is tested as
// soon as every table it reads is bound.
static UINT expr_floor(const sql_expr *expr)
{
	if (!expr)
		return 32;
	if (expr->kind == EXPR_COLUMN)
		return expr->table_index;
	UINT left = expr_floor(expr->left), right = expr_floor(expr->right);
	return left < right ? left : right;
}

static BOOL eval_partial(msi_view *view, const sql_expr *expr, db_row **tuple, UINT bound)
{
	if (!expr)
		return TRUE;
	if (expr->kind == OP_AND)
		return eval_partial(view, expr->left, tuple, bound) && eval_partial(view, expr->right, tuple, bound);
	return expr_floor(expr) < bound || eval_condition(view, expr, tuple);
}

static void push_tuple(msi_view *view, db_row **tuple)
{
	UINT width = view->statement->table_count;
	if (view->tuple_count == view->tuple_room) {
		view->tuple_room = view->tuple_room ? view->tuple_room * 2 : 32;
		view->tuples = msi_grow(view->tuples, view->tuple_room * width * sizeof(db_row *));
	}
	memcpy(view->tuples + view->tuple_count * width, tuple, width * sizeof(db_row *));
	view->tuple_count++;
}

static void join_level(msi_view *view, db_row **tuple, UINT level)
{
	sql_statement *s = view->statement;
	if (!level) {
		if (eval_condition(view, s->where, tuple))
			push_tuple(view, tuple);
		return;
	}
	db_table *table = view->tables[level - 1];
	for (UINT r = 0; r < table->row_count; r++) {
		tuple[level - 1] = table->rows[r];
		if (eval_partial(view, s->where, tuple, level - 1))
			join_level(view, tuple, level - 1);
	}
}

static int compare_order(msi_view *view, db_row **a, db_row **b)
{
	sql_statement *s = view->statement;
	for (UINT i = 0; i < s->order_count; i++) {
		UINT t = 0, c = 0;
		if (!resolve_column(view, s->order[i].table, s->order[i].name, &t, &c))
			continue;
		UINT left = a[t]->cells[c], right = b[t]->cells[c];
		if (left == right)
			continue;
		// Strings sort by their place in the pool, not by their text.
		return left < right ? -1 : 1;
	}
	return 0;
}

static void sort_tuples(msi_view *view)
{
	UINT width = view->statement->table_count, count = view->tuple_count;
	if (count < 2)
		return;
	db_row **from = view->tuples, **to = msi_alloc(count * width * sizeof(db_row *));
	for (UINT run = 1; run < count; run *= 2) {
		for (UINT start = 0; start < count; start += run * 2) {
			UINT mid = start + run < count ? start + run : count, end = start + run * 2 < count ? start + run * 2 : count;
			UINT i = start, j = mid, out = start;
			while (i < mid || j < end) {
				BOOL take_left = j >= end || (i < mid && compare_order(view, from + i * width, from + j * width) <= 0);
				UINT source = take_left ? i++ : j++;
				memcpy(to + out++ * width, from + source * width, width * sizeof(db_row *));
			}
		}
		db_row **swap = from;
		from = to;
		to = swap;
	}
	if (from != view->tuples)
		memcpy(view->tuples, from, count * width * sizeof(db_row *));
	msi_free(from == view->tuples ? to : from);
}

static BOOL same_columns(msi_view *view, db_row **a, db_row **b)
{
	for (UINT i = 0; i < view->column_count; i++) {
		column_ref ref = view->columns[i];
		if (a[ref.table]->cells[ref.column] != b[ref.table]->cells[ref.column])
			return FALSE;
	}
	return TRUE;
}

static void make_distinct(msi_view *view)
{
	UINT width = view->statement->table_count, kept = 0;
	for (UINT i = 0; i < view->tuple_count; i++) {
		BOOL seen = FALSE;
		for (UINT k = 0; k < kept && !seen; k++)
			seen = same_columns(view, view->tuples + k * width, view->tuples + i * width);
		if (!seen) {
			if (kept != i)
				memcpy(view->tuples + kept * width, view->tuples + i * width, width * sizeof(db_row *));
			kept++;
		}
	}
	view->tuple_count = kept;
}

static void reset_result(msi_view *view)
{
	msi_free(view->tuples);
	view->tuples = NULL;
	view->tuple_count = view->tuple_room = 0;
	view->position = 0;
}

static UINT run_select(msi_view *view)
{
	sql_statement *s = view->statement;
	db_row *tuple[32];
	reset_result(view);
	for (UINT t = 0; t < s->table_count; t++)
		db_load_table(view->db, view->tables[t]);
	join_level(view, tuple, s->table_count);
	if (s->order_count)
		sort_tuples(view);
	if (s->distinct)
		make_distinct(view);
	return ERROR_SUCCESS;
}

static BOOL text_is_number(const WCHAR *text, INT *value)
{
	BOOL negative = *text == L'-';
	UINT number = 0;
	const WCHAR *at = text + negative;
	if (!*at)
		return FALSE;
	for (; *at; at++) {
		if (*at < L'0' || *at > L'9')
			return FALSE;
		number = number * 10 + (*at - L'0');
	}
	*value = negative ? -(INT)number : (INT)number;
	return TRUE;
}

// What a column stores for a value of the query or a record field.
static BOOL expr_to_cell(msi_view *view, const sql_expr *expr, UINT type, UINT *cell)
{
	expr_value value = eval_operand(view, expr, NULL);
	if (value.null) {
		*cell = 0;
		return TRUE;
	}
	if (type & TYPE_STRING) {
		WCHAR digits[16];
		const WCHAR *text = value.text;
		if (!value.is_text) {
			wsprintfW(digits, L"%d", value.number);
			text = digits;
		}
		*cell = pool_add(view->db, text, lstrlenW(text));
		return TRUE;
	}
	INT number = value.number;
	if (value.is_text && !text_is_number(value.text, &number))
		return FALSE;
	*cell = cell_from_int(type, number);
	return TRUE;
}

static UINT run_insert(msi_view *view)
{
	sql_statement *s = view->statement;
	db_table *table = view->tables[0];
	db_load_table(view->db, table);
	UINT *cells = msi_alloc((table->column_count + 1) * sizeof(UINT));
	UINT status = ERROR_SUCCESS;
	for (UINT i = 0; i < s->column_count && !status; i++) {
		UINT t, c;
		if (!resolve_column(view, NULL, s->columns[i].name, &t, &c) || !expr_to_cell(view, s->columns[i].value, table->columns[c].type, &cells[c]))
			status = ERROR_FUNCTION_FAILED;
		else if (s->columns[i].value->kind == EXPR_PARAM && TYPE_IS_BINARY(table->columns[c].type))
			status = ERROR_SUCCESS;
	}
	for (UINT i = 0; i < s->column_count && !status; i++) {
		UINT t, c;
		resolve_column(view, NULL, s->columns[i].name, &t, &c);
		if (!cells[c] && !(table->columns[c].type & TYPE_NULLABLE) && !TYPE_IS_BINARY(table->columns[c].type))
			status = ERROR_FUNCTION_FAILED;
	}
	if (!status && table_find_key(view->db, table, cells))
		status = ERROR_FUNCTION_FAILED;
	if (!status) {
		for (UINT i = 0; i < s->column_count; i++) {
			UINT t, c;
			resolve_column(view, NULL, s->columns[i].name, &t, &c);
			if (TYPE_IS_BINARY(table->columns[c].type) && s->columns[i].value->kind == EXPR_PARAM) {
				msi_record *params = view->params;
				UINT field = s->columns[i].value->param;
				if (params && field <= params->count && params->fields[field].type == FIELD_BLOB) {
					cells[c] = 1;
					WCHAR *name = table->virtual_table ? msi_copy(pool_text(view->db, cells[0])) : row_stream_name(view->db, table, cells, c);
					stream_put(view->db, name, params->fields[field].blob);
					if (table->virtual_table && !lstrcmpW(table->name, L"_Storages"))
						stream_find(view->db, name)->storage = TRUE;
					msi_free(name);
				}
			}
		}
		table_insert(view->db, table, cells, s->temporary);
	}
	msi_free(cells);
	return status;
}

static UINT run_update(msi_view *view)
{
	sql_statement *s = view->statement;
	db_table *table = view->tables[0];
	db_load_table(view->db, table);
	for (UINT i = 0; i < s->column_count; i++) {
		UINT t, c;
		if (!resolve_column(view, NULL, s->columns[i].name, &t, &c))
			return ERROR_FUNCTION_FAILED;
		if (table->columns[c].type & TYPE_KEY)
			return ERROR_FUNCTION_FAILED;
	}
	for (UINT r = 0; r < table->row_count; r++) {
		db_row *row = table->rows[r];
		if (!eval_condition(view, s->where, &row))
			continue;
		for (UINT i = 0; i < s->column_count; i++) {
			UINT t, c, cell;
			resolve_column(view, NULL, s->columns[i].name, &t, &c);
			if (TYPE_IS_BINARY(table->columns[c].type)) {
				msi_record *params = view->params;
				const sql_expr *value = s->columns[i].value;
				if (value->kind == EXPR_PARAM && params && value->param <= params->count && params->fields[value->param].type == FIELD_BLOB) {
					row->cells[c] = 1;
					WCHAR *name = table->virtual_table ? msi_copy(pool_text(view->db, row->cells[0])) : row_stream_name(view->db, table, row->cells, c);
					stream_put(view->db, name, params->fields[value->param].blob);
					msi_free(name);
				}
				continue;
			}
			if (!expr_to_cell(view, s->columns[i].value, table->columns[c].type, &cell))
				return ERROR_FUNCTION_FAILED;
			row->cells[c] = cell;
		}
		view->db->dirty = TRUE;
	}
	return ERROR_SUCCESS;
}

static UINT run_delete(msi_view *view)
{
	sql_statement *s = view->statement;
	db_table *table = view->tables[0];
	db_load_table(view->db, table);
	for (UINT r = table->row_count; r; r--) {
		db_row *row = table->rows[r - 1];
		if (eval_condition(view, s->where, &row))
			table_delete(view->db, table, row);
	}
	return ERROR_SUCCESS;
}

static UINT run_create(msi_view *view)
{
	sql_statement *s = view->statement;
	db_column *columns = msi_alloc(s->column_count * sizeof(db_column));
	BOOL temporary = TRUE;
	UINT placed = 0;
	for (UINT i = 1; i < s->column_count; i++)
		if ((s->columns[i].type & TYPE_TEMPORARY) && !(s->columns[i - 1].type & TYPE_TEMPORARY)) {
			msi_free(columns);
			return ERROR_FUNCTION_FAILED;
		}
	// Keys come first in a table, in the order the key list gives them.
	for (UINT k = 0; k < s->key_count; k++)
		for (UINT i = 0; i < s->column_count; i++)
			if (!lstrcmpW(s->keys[k], s->columns[i].name)) {
				columns[placed].name = s->columns[i].name;
				columns[placed++].type = s->columns[i].type | TYPE_KEY;
			}
	for (UINT i = 0; i < s->column_count; i++) {
		BOOL key = FALSE;
		for (UINT k = 0; k < s->key_count; k++)
			if (!lstrcmpW(s->keys[k], s->columns[i].name))
				key = TRUE;
		if (!key) {
			columns[placed].name = s->columns[i].name;
			columns[placed++].type = s->columns[i].type;
		}
	}
	for (UINT i = 0; i < s->column_count; i++)
		if (!(columns[i].type & TYPE_TEMPORARY))
			temporary = FALSE;
	if (DB_READONLY(view->db) && !temporary) {
		msi_free(columns);
		return ERROR_FUNCTION_FAILED;
	}
	db_table *table = NULL;
	UINT status = db_create_table(view->db, s->tables[0], columns, s->column_count, temporary, &table);
	if (!status && s->hold)
		table->holds = 1;
	// Unheld, a temporary table lasts only as long as the query that made it.
	if (!status && temporary && !s->hold)
		view->unheld = table;
	msi_free(columns);
	return status;
}

static UINT run_alter(msi_view *view)
{
	sql_statement *s = view->statement;
	db_table *table = view->tables[0];
	if (s->add_column) {
		for (UINT c = 0; c < table->column_count; c++)
			if (!lstrcmpW(table->columns[c].name, s->columns[0].name))
				return ERROR_BAD_QUERY_SYNTAX;
		db_add_column(view->db, table, s->columns[0].name, s->columns[0].type, table->column_count + 1);
	}
	if (s->hold)
		table->holds++;
	if (s->free_table && (!table->holds || !--table->holds)) {
		if (table->temporary)
			db_drop_table(view->db, table);
		else
			db_drop_temporary_columns(table);
	}
	return ERROR_SUCCESS;
}

static void drop_unheld(msi_view *view)
{
	if (view->unheld && !view->unheld->holds && db_find_table(view->db, view->unheld->name) == view->unheld)
		db_drop_table(view->db, view->unheld);
	view->unheld = NULL;
}

static void view_destroy(msi_object *object)
{
	msi_view *view = (msi_view *)object;
	drop_unheld(view);
	reset_result(view);
	sql_free(view->statement);
	msi_free(view->columns);
	msi_free(view->error_column);
	if (view->params)
		object_unref(&view->params->base);
	object_unref(&view->db->base);
}

static UINT view_prepare(msi_db *db, const WCHAR *query, msi_view **result)
{
	sql_statement *s = NULL;
	UINT status = sql_parse(query, &s);
	if (status)
		return status;
	msi_view *view = msi_alloc(sizeof(*view));
	object_init(&view->base, KIND_VIEW, view_destroy);
	object_ref(&db->base);
	view->db = db;
	view->statement = s;
	if (s->kind == SQL_CREATE) {
		if (db_find_table(db, s->tables[0]) || !s->column_count || !s->key_count)
			status = ERROR_BAD_QUERY_SYNTAX;
		for (UINT i = 0; i < s->column_count && !status; i++)
			for (UINT k = 0; k < i; k++)
				if (!lstrcmpW(s->columns[i].name, s->columns[k].name))
					status = ERROR_BAD_QUERY_SYNTAX;
		for (UINT i = 0; i < s->key_count && !status; i++) {
			status = ERROR_BAD_QUERY_SYNTAX;
			for (UINT c = 0; c < s->column_count; c++)
				if (!lstrcmpW(s->keys[i], s->columns[c].name))
					status = ERROR_SUCCESS;
		}
	} else {
		for (UINT t = 0; t < s->table_count && !status; t++) {
			view->tables[t] = db_find_table(db, s->tables[t]);
			if (!view->tables[t])
				status = ERROR_BAD_QUERY_SYNTAX;
			else if (is_virtual_name(s->tables[t]))
				db_refresh_virtual(db);
		}
	}
	if (!status && s->kind == SQL_SELECT) {
		if (s->star) {
			UINT total = 0;
			for (UINT t = 0; t < s->table_count; t++)
				total += view->tables[t]->column_count;
			view->columns = msi_alloc(total * sizeof(column_ref));
			for (UINT t = 0; t < s->table_count; t++)
				for (UINT c = 0; c < view->tables[t]->column_count; c++) {
					view->columns[view->column_count].table = t;
					view->columns[view->column_count++].column = c;
				}
		} else {
			view->columns = msi_alloc(s->column_count * sizeof(column_ref));
			view->column_count = s->column_count;
			for (UINT i = 0; i < s->column_count && !status; i++)
				if (!resolve_column(view, s->columns[i].table, s->columns[i].name, &view->columns[i].table, &view->columns[i].column))
					status = ERROR_BAD_QUERY_SYNTAX;
		}
		for (UINT i = 0; i < s->order_count && !status; i++) {
			UINT t, c;
			if (!resolve_column(view, s->order[i].table, s->order[i].name, &t, &c))
				status = ERROR_BAD_QUERY_SYNTAX;
		}
	}
	if (!status && (s->kind == SQL_INSERT || s->kind == SQL_UPDATE)) {
		for (UINT i = 0; i < s->column_count && !status; i++) {
			UINT t, c;
			if (!resolve_column(view, s->columns[i].table, s->columns[i].name, &t, &c))
				status = ERROR_BAD_QUERY_SYNTAX;
		}
	}
	if (!status && !resolve_expr(view, s->where))
		status = ERROR_BAD_QUERY_SYNTAX;
	if (status) {
		object_unref(&view->base);
		return status;
	}
	*result = view;
	return ERROR_SUCCESS;
}

static UINT view_execute(msi_view *view, msi_record *params)
{
	sql_statement *s = view->statement;
	if (view->params) {
		object_unref(&view->params->base);
		view->params = NULL;
	}
	if (params) {
		object_ref(&params->base);
		view->params = params;
	}
	if (s->param_count && (!params || params->count < s->param_count))
		return ERROR_FUNCTION_FAILED;
	for (UINT t = 0; t < s->table_count; t++)
		if (is_virtual_name(s->tables[t]) && s->kind != SQL_CREATE)
			db_refresh_virtual(view->db);
	UINT status;
	switch (s->kind) {
	case SQL_SELECT:
		status = run_select(view);
		break;
	case SQL_INSERT:
		status = run_insert(view);
		break;
	case SQL_UPDATE:
		status = run_update(view);
		break;
	case SQL_DELETE:
		status = run_delete(view);
		break;
	case SQL_CREATE:
		status = run_create(view);
		break;
	case SQL_ALTER:
		status = run_alter(view);
		break;
	case SQL_DROP:
		db_drop_table(view->db, view->tables[0]);
		status = ERROR_SUCCESS;
		break;
	default:
		status = ERROR_FUNCTION_FAILED;
	}
	view->executed = TRUE;
	return status;
}

static msi_record *view_row_record(msi_view *view, UINT index)
{
	msi_record *record = record_new(view->column_count);
	if (!record)
		return NULL;
	db_row **tuple = view->tuples + index * view->statement->table_count;
	// Field 0 of a fetched row says it came from a view: any value but null will do.
	record_set_int(record, 0, (INT)index + 1);
	for (UINT i = 0; i < view->column_count; i++) {
		column_ref ref = view->columns[i];
		row_to_record(view->db, view->tables[ref.table], tuple[ref.table]->cells, &ref.column, 1, record, i + 1);
	}
	return record;
}

UINT WINAPI MsiDatabaseOpenViewW(MSIHANDLE database, LPCWSTR query, MSIHANDLE *handle)
{
	if (!query || !handle) {
		msi_object *known = handle_lookup(database, KIND_DATABASE);
		if (known)
			object_unref(known);
		return known ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE;
	}
	msi_db *db = (msi_db *)handle_lookup(database, KIND_DATABASE);
	if (!db)
		return ERROR_INVALID_HANDLE;
	UINT status = view_open(db, query, handle);
	object_unref(&db->base);
	return status;
}

UINT WINAPI MsiDatabaseOpenViewA(MSIHANDLE database, LPCSTR query, MSIHANDLE *handle)
{
	WCHAR *wide = msi_from_ansi(query);
	UINT status = MsiDatabaseOpenViewW(database, wide, handle);
	msi_free(wide);
	return status;
}

UINT view_open(msi_db *db, const WCHAR *query, MSIHANDLE *handle)
{
	msi_view *view;
	UINT status = view_prepare(db, query, &view);
	if (status)
		return status;
	*handle = handle_open(&view->base);
	object_unref(&view->base);
	return *handle ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
}

UINT db_query_each(msi_db *db, const WCHAR *query, msi_record *params, UINT (*callback)(msi_record *row, void *context), void *context)
{
	msi_view *view = NULL;
	UINT status = view_prepare(db, query, &view);
	if (!status)
		status = view_execute(view, params);
	if (status) {
		if (!view)
			return status;
		object_unref(&view->base);
		return status;
	}
	for (UINT i = 0; i < view->tuple_count && !status; i++) {
		msi_record *row = view_row_record(view, i);
		status = callback(row, context);
		object_unref(&row->base);
	}
	object_unref(&view->base);
	return status;
}

UINT db_run_query(msi_db *db, const WCHAR *query)
{
	msi_view *view = NULL;
	UINT status = view_prepare(db, query, &view);
	if (status)
		return status;
	status = view_execute(view, NULL);
	object_unref(&view->base);
	return status;
}

UINT WINAPI MsiViewExecute(MSIHANDLE handle, MSIHANDLE params)
{
	msi_view *view = (msi_view *)handle_lookup(handle, KIND_VIEW);
	msi_record *record = NULL;
	if (!view)
		return ERROR_INVALID_HANDLE;
	if (params) {
		record = (msi_record *)handle_lookup(params, KIND_RECORD);
		if (!record) {
			object_unref(&view->base);
			return ERROR_INVALID_HANDLE;
		}
	}
	UINT status = view_execute(view, record);
	if (record)
		object_unref(&record->base);
	object_unref(&view->base);
	return status;
}

UINT WINAPI MsiViewFetch(MSIHANDLE handle, MSIHANDLE *out)
{
	if (!out)
		return ERROR_INVALID_PARAMETER;
	*out = 0;
	msi_view *view = (msi_view *)handle_lookup(handle, KIND_VIEW);
	if (!view)
		return ERROR_INVALID_HANDLE;
	UINT status = ERROR_FUNCTION_FAILED;
	if (view->executed && view->statement->kind == SQL_SELECT) {
		if (view->position >= view->tuple_count) {
			status = ERROR_NO_MORE_ITEMS;
			view->position = 0;
		} else {
			msi_record *record = view_row_record(view, view->position++);
			*out = handle_open(&record->base);
			object_unref(&record->base);
			status = ERROR_SUCCESS;
		}
	}
	object_unref(&view->base);
	return status;
}

UINT WINAPI MsiViewClose(MSIHANDLE handle)
{
	msi_view *view = (msi_view *)handle_lookup(handle, KIND_VIEW);
	if (!view)
		return ERROR_INVALID_HANDLE;
	reset_result(view);
	view->executed = FALSE;
	drop_unheld(view);
	object_unref(&view->base);
	return ERROR_SUCCESS;
}

static void column_type_text(UINT type, WCHAR *out)
{
	WCHAR letter;
	UINT size = type & TYPE_SIZE;
	BOOL nullable = (type & TYPE_NULLABLE) != 0;
	if (type & TYPE_TEMPORARY)
		letter = (type & TYPE_STRING) ? (nullable ? L'G' : L'g') : (nullable ? L'J' : L'j');
	else if (TYPE_IS_BINARY(type))
		letter = nullable ? L'V' : L'v';
	else if (type & TYPE_LOCALIZABLE)
		letter = nullable ? L'L' : L'l';
	else if (type & TYPE_STRING)
		letter = nullable ? L'S' : L's';
	else
		letter = nullable ? L'I' : L'i';
	wsprintfW(out, L"%c%u", letter, size);
}

UINT WINAPI MsiViewGetColumnInfo(MSIHANDLE handle, MSICOLINFO info, MSIHANDLE *out)
{
	if (info != MSICOLINFO_NAMES && info != MSICOLINFO_TYPES)
		return ERROR_INVALID_PARAMETER;
	if (!out)
		return ERROR_INVALID_PARAMETER;
	msi_view *view = (msi_view *)handle_lookup(handle, KIND_VIEW);
	if (!view)
		return ERROR_INVALID_HANDLE;
	msi_record *record = record_new(view->column_count);
	for (UINT i = 0; i < view->column_count; i++) {
		const db_column *column = &view->tables[view->columns[i].table]->columns[view->columns[i].column];
		if (info == MSICOLINFO_NAMES) {
			record_set_text(record, i + 1, column->name);
		} else {
			WCHAR text[16];
			column_type_text(column->type, text);
			record_set_text(record, i + 1, text);
		}
	}
	*out = handle_open(&record->base);
	object_unref(&record->base);
	object_unref(&view->base);
	return ERROR_SUCCESS;
}

// The cells of a record laid over one table's row: the view's columns come from the record, the others stay.
static UINT record_to_table_cells(msi_view *view, UINT source, msi_record *record, UINT *cells, BOOL *changed)
{
	db_table *table = view->tables[source];
	for (UINT i = 0; i < view->column_count && i < record->count; i++) {
		if (view->columns[i].table != source)
			continue;
		UINT c = view->columns[i].column, type = table->columns[c].type;
		record_field *field = &record->fields[i + 1];
		UINT cell = 0;
		if (TYPE_IS_BINARY(type)) {
			cell = field->type == FIELD_BLOB ? 1 : cells[c];
			if (field->type == FIELD_NULL)
				cell = 0;
		} else if (type & TYPE_STRING) {
			WCHAR scratch[16];
			const WCHAR *text = record_text(record, i + 1, scratch);
			cell = text ? pool_add(view->db, text, lstrlenW(text)) : 0;
		} else if (field->type != FIELD_NULL) {
			INT number = record_get_int(record, i + 1);
			if (number == MSI_NULL_INTEGER)
				return ERROR_FUNCTION_FAILED;
			cell = cell_from_int(type, number);
		}
		if (cells[c] != cell && changed)
			*changed = TRUE;
		cells[c] = cell;
	}
	return ERROR_SUCCESS;
}

static UINT record_to_cells(msi_view *view, msi_record *record, UINT *cells, BOOL *changed)
{
	return record_to_table_cells(view, 0, record, cells, changed);
}

static void store_record_streams(msi_view *view, msi_record *record, const UINT *cells)
{
	db_table *table = view->tables[0];
	for (UINT i = 0; i < view->column_count && i < record->count; i++) {
		UINT c = view->columns[i].column;
		if (TYPE_IS_BINARY(table->columns[c].type) && record->fields[i + 1].type == FIELD_BLOB) {
			WCHAR *name = table->virtual_table ? msi_copy(pool_text(view->db, cells[0])) : row_stream_name(view->db, table, cells, c);
			stream_put(view->db, name, record->fields[i + 1].blob);
			msi_free(name);
		}
	}
}

static BOOL view_is_single_table(msi_view *view)
{
	return view->statement->table_count == 1;
}

static BOOL key_columns_present(msi_view *view)
{
	db_table *table = view->tables[0];
	for (UINT c = 0; c < table->column_count; c++) {
		if (!(table->columns[c].type & TYPE_KEY))
			continue;
		BOOL found = FALSE;
		for (UINT i = 0; i < view->column_count; i++)
			if (view->columns[i].column == c)
				found = TRUE;
		if (!found)
			return FALSE;
	}
	return TRUE;
}

// Field 0 of a fetched record names its row: a record built by hand has no row to update.
static db_row *record_row(msi_view *view, msi_record *record)
{
	if (record->fields[0].type != FIELD_INT)
		return NULL;
	UINT index = (UINT)record->fields[0].number;
	if (!index || index > view->tuple_count)
		return NULL;
	return view->tuples[(index - 1) * view->statement->table_count];
}

static void add_tuple_row(msi_view *view, db_row *row)
{
	if (view->statement->table_count == 1)
		push_tuple(view, &row);
}

static UINT modify_insert(msi_view *view, msi_record *record, BOOL temporary)
{
	db_table *table = view->tables[0];
	UINT *cells = msi_alloc((table->column_count + 1) * sizeof(UINT));
	UINT status = record_to_cells(view, record, cells, NULL);
	if (!status && table_find_key(view->db, table, cells))
		status = ERROR_FUNCTION_FAILED;
	if (!status) {
		store_record_streams(view, record, cells);
		add_tuple_row(view, table_insert(view->db, table, cells, temporary));
	}
	msi_free(cells);
	return status;
}

static WCHAR *append(WCHAR *at, const WCHAR *text)
{
	while (*text)
		*at++ = *text++;
	*at = 0;
	return at;
}

// A custom action works on a copy of the session's database, so a temporary row it adds is added to the installer's session as
// well: a query naming the view's columns, and their values as I<number>; S<length>;<text> or N; one after the other.
static UINT forward_insert(msi_view *view, msi_record *record)
{
	db_table *table = view->tables[0];
	SIZE_T query_room = lstrlenW(table->name) + 32, values_room = 1;
	for (UINT i = 0; i < view->column_count; i++) {
		const record_field *field = &record->fields[i + 1];
		query_room += lstrlenW(table->columns[view->columns[i].column].name) + 4;
		values_room += 16 + (field->type == FIELD_TEXT && field->text ? lstrlenW(field->text) : 0);
	}
	WCHAR *query = msi_alloc(query_room * sizeof(WCHAR)), *values = msi_alloc(values_room * sizeof(WCHAR));
	WCHAR *at = append(query, L"SELECT "), *value = values;
	*value = 0;
	for (UINT i = 0; i < view->column_count; i++) {
		const record_field *field = &record->fields[i + 1];
		at = append(append(append(at, i ? L", `" : L"`"), table->columns[view->columns[i].column].name), L"`");
		WCHAR head[24];
		BOOL text = field->type == FIELD_TEXT && field->text;
		if (field->type == FIELD_INT)
			wsprintfW(head, L"I%d;", field->number);
		else if (text)
			wsprintfW(head, L"S%d;", lstrlenW(field->text));
		else
			lstrcpyW(head, L"N;");
		value = append(value, head);
		if (text)
			value = append(value, field->text);
	}
	append(append(append(at, L" FROM `"), table->name), L"`");
	UINT status = remote_call(view->db->remote, REMOTE_INSERT_ROW, query, values, 0, NULL, NULL);
	msi_free(query);
	msi_free(values);
	return status;
}

static UINT modify_set(msi_view *view, msi_record *record, db_row *row, BOOL keys_fixed)
{
	db_table *table = view->tables[0];
	UINT *cells = msi_alloc((table->column_count + 1) * sizeof(UINT));
	memcpy(cells, row->cells, table->column_count * sizeof(UINT));
	BOOL changed = FALSE;
	UINT status = record_to_cells(view, record, cells, &changed);
	if (!status && keys_fixed)
		for (UINT c = 0; c < table->column_count; c++)
			if ((table->columns[c].type & TYPE_KEY) && cells[c] != row->cells[c])
				status = ERROR_FUNCTION_FAILED;
	if (!status) {
		memcpy(row->cells, cells, table->column_count * sizeof(UINT));
		store_record_streams(view, record, cells);
		view->db->dirty = TRUE;
	}
	msi_free(cells);
	return status;
}

// A joined row is several rows: each table takes the columns the view shows of it, its keys staying as they are.
static UINT modify_update(msi_view *view, msi_record *record)
{
	if (record->fields[0].type != FIELD_INT)
		return ERROR_FUNCTION_FAILED;
	UINT index = (UINT)record->fields[0].number;
	if (!index || index > view->tuple_count)
		return ERROR_FUNCTION_FAILED;
	db_row **tuple = view->tuples + (index - 1) * view->statement->table_count;
	UINT *fresh[32] = {0}, status = ERROR_SUCCESS;
	for (UINT t = 0; t < view->statement->table_count && !status; t++) {
		db_table *table = view->tables[t];
		if (tuple[t]->dead) {
			status = ERROR_FUNCTION_FAILED;
			break;
		}
		fresh[t] = msi_alloc((table->column_count + 1) * sizeof(UINT));
		memcpy(fresh[t], tuple[t]->cells, table->column_count * sizeof(UINT));
		status = record_to_table_cells(view, t, record, fresh[t], NULL);
		for (UINT c = 0; c < table->column_count && !status; c++)
			if ((table->columns[c].type & TYPE_KEY) && fresh[t][c] != tuple[t]->cells[c])
				status = ERROR_FUNCTION_FAILED;
	}
	for (UINT t = 0; t < view->statement->table_count; t++) {
		if (!status) {
			memcpy(tuple[t]->cells, fresh[t], view->tables[t]->column_count * sizeof(UINT));
			if (t == 0 && view->statement->table_count == 1)
				store_record_streams(view, record, fresh[t]);
			view->db->dirty = TRUE;
		}
		msi_free(fresh[t]);
	}
	return status;
}

static UINT modify_assign(msi_view *view, msi_record *record, db_row *row)
{
	db_table *table = view->tables[0];
	UINT *cells = msi_alloc((table->column_count + 1) * sizeof(UINT));
	UINT status = record_to_cells(view, record, cells, NULL);
	if (!status) {
		memcpy(row->cells, cells, table->column_count * sizeof(UINT));
		store_record_streams(view, record, cells);
		view->db->dirty = TRUE;
	}
	msi_free(cells);
	return status;
}

static db_row *row_for_record(msi_view *view, msi_record *record)
{
	db_table *table = view->tables[0];
	UINT *cells = msi_alloc((table->column_count + 1) * sizeof(UINT));
	db_row *row = NULL;
	if (!record_to_cells(view, record, cells, NULL))
		row = table_find_key(view->db, table, cells);
	msi_free(cells);
	return row;
}

static UINT validate_record(msi_view *view, msi_record *record, UINT mode)
{
	db_table *table = view->tables[0];
	msi_free(view->error_column);
	view->error_column = NULL;
	view->error = MSIDBERROR_NOERROR;
	for (UINT i = 0; i < view->column_count && i < record->count; i++) {
		UINT c = view->columns[i].column, type = table->columns[c].type;
		record_field *field = &record->fields[i + 1];
		MSIDBERROR problem = MSIDBERROR_NOERROR;
		if (field->type == FIELD_NULL) {
			if (!(type & TYPE_NULLABLE))
				problem = MSIDBERROR_REQUIRED;
		} else if (!(type & TYPE_STRING)) {
			if (record_get_int(record, i + 1) == MSI_NULL_INTEGER)
				problem = MSIDBERROR_OVERFLOW;
		}
		if (problem) {
			view->error = problem;
			view->error_column = msi_copy(table->columns[c].name);
			return ERROR_INVALID_DATA;
		}
	}
	if (mode == MSIMODIFY_VALIDATE_NEW && row_for_record(view, record)) {
		for (UINT c = 0; c < table->column_count; c++)
			if (table->columns[c].type & TYPE_KEY) {
				view->error = MSIDBERROR_DUPLICATEKEY;
				view->error_column = msi_copy(table->columns[c].name);
				break;
			}
		return ERROR_INVALID_DATA;
	}
	return ERROR_SUCCESS;
}

UINT WINAPI MsiViewModify(MSIHANDLE handle, MSIMODIFY mode, MSIHANDLE record_handle)
{
	msi_view *view = (msi_view *)handle_lookup(handle, KIND_VIEW);
	if (!view)
		return ERROR_INVALID_HANDLE;
	msi_record *record = (msi_record *)handle_lookup(record_handle, KIND_RECORD);
	UINT status = ERROR_SUCCESS;
	if (!record) {
		object_unref(&view->base);
		return ERROR_INVALID_HANDLE;
	}
	// A row is added through a view that has not run yet as well; the other changes work on the rows it fetched.
	if (view->statement->kind != SQL_SELECT || (!view->executed && mode != MSIMODIFY_INSERT && mode != MSIMODIFY_INSERT_TEMPORARY)) {
		status = ERROR_FUNCTION_FAILED;
		goto done;
	}
	if (record->count < view->column_count && mode != MSIMODIFY_DELETE && mode != MSIMODIFY_REFRESH) {
		status = ERROR_INVALID_DATA;
		goto done;
	}
	if (!view_is_single_table(view) && mode != MSIMODIFY_REFRESH && mode != MSIMODIFY_SEEK && mode != MSIMODIFY_UPDATE) {
		status = ERROR_FUNCTION_FAILED;
		goto done;
	}
	if (DB_READONLY(view->db) && mode != MSIMODIFY_REFRESH && mode != MSIMODIFY_SEEK && mode != MSIMODIFY_INSERT_TEMPORARY
		&& mode != MSIMODIFY_VALIDATE && mode != MSIMODIFY_VALIDATE_NEW && mode != MSIMODIFY_VALIDATE_FIELD && mode != MSIMODIFY_VALIDATE_DELETE) {
		status = ERROR_ACCESS_DENIED;
		goto done;
	}
	db_row *row;
	switch (mode) {
	case MSIMODIFY_SEEK:
		status = ERROR_FUNCTION_FAILED;
		break;
	case MSIMODIFY_REFRESH: {
		db_row *current = record_row(view, record);
		if (!current || current->dead) {
			status = ERROR_FUNCTION_FAILED;
			break;
		}
		msi_record *fresh = view_row_record(view, (UINT)record->fields[0].number - 1);
		for (UINT i = 1; i <= fresh->count && i <= record->count; i++) {
			WCHAR scratch[16];
			if (fresh->fields[i].type == FIELD_INT)
				record_set_int(record, i, fresh->fields[i].number);
			else if (fresh->fields[i].type == FIELD_BLOB)
				record_set_blob(record, i, fresh->fields[i].blob);
			else
				record_set_text(record, i, record_text(fresh, i, scratch));
		}
		object_unref(&fresh->base);
		break;
	}
	case MSIMODIFY_INSERT:
		status = modify_insert(view, record, FALSE);
		break;
	case MSIMODIFY_INSERT_TEMPORARY:
		status = modify_insert(view, record, TRUE);
		if (!status && view->db->remote)
			status = forward_insert(view, record);
		break;
	case MSIMODIFY_UPDATE:
		status = modify_update(view, record);
		break;
	case MSIMODIFY_ASSIGN:
		// Assign writes the whole row: columns outside the view end up empty.
		row = row_for_record(view, record);
		status = row ? modify_assign(view, record, row) : modify_insert(view, record, FALSE);
		break;
	case MSIMODIFY_REPLACE:
		if (!key_columns_present(view)) {
			status = ERROR_FUNCTION_FAILED;
			break;
		}
		row = row_for_record(view, record);
		status = row ? modify_set(view, record, row, FALSE) : modify_insert(view, record, FALSE);
		break;
	case MSIMODIFY_MERGE:
		row = row_for_record(view, record);
		if (!row) {
			status = modify_insert(view, record, FALSE);
		} else {
			UINT *cells = msi_alloc((view->tables[0]->column_count + 1) * sizeof(UINT));
			memcpy(cells, row->cells, view->tables[0]->column_count * sizeof(UINT));
			BOOL changed = FALSE;
			status = record_to_cells(view, record, cells, &changed);
			if (!status && changed)
				status = ERROR_FUNCTION_FAILED;
			msi_free(cells);
		}
		break;
	case MSIMODIFY_DELETE:
		// A fetched record names its row, whatever its fields say by now.
		row = record_row(view, record);
		if (!row)
			row = row_for_record(view, record);
		if (row)
			table_delete(view->db, view->tables[0], row);
		else
			status = ERROR_FUNCTION_FAILED;
		break;
	case MSIMODIFY_VALIDATE:
	case MSIMODIFY_VALIDATE_NEW:
	case MSIMODIFY_VALIDATE_FIELD:
	case MSIMODIFY_VALIDATE_DELETE:
		status = validate_record(view, record, mode);
		break;
	default:
		status = ERROR_INVALID_PARAMETER;
	}
done:
	object_unref(&record->base);
	object_unref(&view->base);
	return status;
}

MSIDBERROR WINAPI MsiViewGetErrorW(MSIHANDLE handle, LPWSTR out, LPDWORD size)
{
	if (!size)
		return MSIDBERROR_INVALIDARG;
	msi_view *view = (msi_view *)handle_lookup(handle, KIND_VIEW);
	if (!view)
		return MSIDBERROR_INVALIDARG;
	const WCHAR *column = view->error_column ? view->error_column : L"";
	MSIDBERROR error = view->error;
	if (out_text_w(column, lstrlenW(column), out, size))
		error = MSIDBERROR_MOREDATA;
	else if (out) {
		view->error = MSIDBERROR_NOERROR;
		msi_free(view->error_column);
		view->error_column = NULL;
	}
	object_unref(&view->base);
	return error;
}

MSIDBERROR WINAPI MsiViewGetErrorA(MSIHANDLE handle, LPSTR out, LPDWORD size)
{
	if (!size)
		return MSIDBERROR_INVALIDARG;
	msi_view *view = (msi_view *)handle_lookup(handle, KIND_VIEW);
	if (!view)
		return MSIDBERROR_INVALIDARG;
	const WCHAR *column = view->error_column ? view->error_column : L"";
	MSIDBERROR error = view->error;
	if (out_text_a(column, lstrlenW(column), out, size))
		error = MSIDBERROR_MOREDATA;
	else if (out) {
		view->error = MSIDBERROR_NOERROR;
		msi_free(view->error_column);
		view->error_column = NULL;
	}
	object_unref(&view->base);
	return error;
}

UINT WINAPI MsiOpenDatabaseW(LPCWSTR path, LPCWSTR persist, MSIHANDLE *handle)
{
	if (!path || !handle)
		return ERROR_INVALID_PARAMETER;
	msi_db *db = NULL;
	UINT status = db_open(path, persist, &db);
	if (status)
		return status;
	*handle = handle_open(&db->base);
	object_unref(&db->base);
	return *handle ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
}

UINT WINAPI MsiOpenDatabaseA(LPCSTR path, LPCSTR persist, MSIHANDLE *handle)
{
	WCHAR *wide_path = msi_from_ansi(path);
	WCHAR *wide_persist = (ULONG_PTR)persist > 0xffff ? msi_from_ansi(persist) : (WCHAR *)persist;
	UINT status = MsiOpenDatabaseW(wide_path, wide_persist, handle);
	msi_free(wide_path);
	if ((ULONG_PTR)persist > 0xffff)
		msi_free(wide_persist);
	return status;
}

UINT WINAPI MsiDatabaseCommit(MSIHANDLE handle)
{
	msi_db *db = (msi_db *)handle_lookup(handle, KIND_DATABASE);
	if (!db)
		return ERROR_INVALID_HANDLE;
	UINT status = db_commit(db);
	object_unref(&db->base);
	return status;
}

MSIDBSTATE WINAPI MsiGetDatabaseState(MSIHANDLE handle)
{
	msi_db *db = (msi_db *)handle_lookup(handle, KIND_DATABASE);
	if (!db)
		return MSIDBSTATE_ERROR;
	MSIDBSTATE state = DB_READONLY(db) ? MSIDBSTATE_READ : MSIDBSTATE_WRITE;
	object_unref(&db->base);
	return state;
}

UINT WINAPI MsiDatabaseGetPrimaryKeysW(MSIHANDLE handle, LPCWSTR name, MSIHANDLE *out)
{
	if (!name || !out)
		return ERROR_INVALID_PARAMETER;
	msi_db *db = (msi_db *)handle_lookup(handle, KIND_DATABASE);
	if (!db)
		return ERROR_INVALID_HANDLE;
	db_table *table = db_find_table(db, name);
	UINT status = ERROR_INVALID_TABLE;
	if (table) {
		UINT keys = 0;
		for (UINT c = 0; c < table->column_count; c++)
			if (table->columns[c].type & TYPE_KEY)
				keys++;
		msi_record *record = record_new(keys);
		UINT at = 1;
		for (UINT c = 0; c < table->column_count; c++)
			if (table->columns[c].type & TYPE_KEY)
				record_set_text(record, at++, table->columns[c].name);
		record_set_text(record, 0, table->name);
		*out = handle_open(&record->base);
		object_unref(&record->base);
		status = ERROR_SUCCESS;
	}
	object_unref(&db->base);
	return status;
}

UINT WINAPI MsiDatabaseGetPrimaryKeysA(MSIHANDLE handle, LPCSTR name, MSIHANDLE *out)
{
	WCHAR *wide = msi_from_ansi(name);
	UINT status = MsiDatabaseGetPrimaryKeysW(handle, wide, out);
	msi_free(wide);
	return status;
}

MSICONDITION WINAPI MsiDatabaseIsTablePersistentW(MSIHANDLE handle, LPCWSTR name)
{
	if (!name)
		return MSICONDITION_ERROR;
	msi_db *db = (msi_db *)handle_lookup(handle, KIND_DATABASE);
	if (!db)
		return MSICONDITION_ERROR;
	db_table *table = db_find_table(db, name);
	MSICONDITION result = !table || table->virtual_table ? MSICONDITION_NONE : table->temporary ? MSICONDITION_FALSE : MSICONDITION_TRUE;
	object_unref(&db->base);
	return result;
}

MSICONDITION WINAPI MsiDatabaseIsTablePersistentA(MSIHANDLE handle, LPCSTR name)
{
	WCHAR *wide = msi_from_ansi(name);
	MSICONDITION result = MsiDatabaseIsTablePersistentW(handle, wide);
	msi_free(wide);
	return result;
}
