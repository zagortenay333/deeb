#include <setjmp.h>

#include "typer.h"
#include "string.h"
#include "engine.h"
#include "parser.h"
#include "db_private.h"

static size_t type_size [TYPE_TAG_MAX_] = {
    #define X(_, type, ...) sizeof(type),
    X_TYPE
    #undef X
};

static char *type_str [TYPE_TAG_MAX_] = {
    #define X(_, __, str) str,
    X_TYPE
    #undef X
};

struct Typer {
    Database *db;

    Mem *mem;
    Array(Type_Table*) tables;

    Type *type_int;
    Type *type_bool;
    Type *type_text;
    Type *type_void;
    Type *type_void_row;

    struct {
        Mem *mem;
        String query;
        jmp_buf error;
        DString *report;
        bool user_is_admin;
        Type_Row *input_row;
    } check;
};

static void check (Typer *, Plan *);
static void match_type_tag (Typer *, Plan *, Type_Tag);

static void *type_new (Type_Tag tag, Mem *mem) {
    Type *type = MEM_ALLOC_Z(mem, type_size[tag]);
    type->tag  = tag;
    if (tag == TYPE_ROW) array_init(&((Type_Row*)type)->scopes, mem);
    return type;
}

static Row_Scope *scope_new (Mem *mem, String name) {
    Row_Scope *scope = MEM_ALLOC(mem, sizeof(Row_Scope));
    scope->name = name;
    array_init(&scope->cols, mem);
    return scope;
}

Typer *typer_new (Database *db, Mem *mem) {
    Typer *typer = MEM_ALLOC_Z(mem, sizeof(Typer));

    typer->db = db;
    typer->mem = mem;
    array_init(&typer->tables, mem);

    typer->type_int      = type_new(TYPE_INT, mem);
    typer->type_bool     = type_new(TYPE_BOOL, mem);
    typer->type_text     = type_new(TYPE_TEXT, mem);
    typer->type_void     = type_new(TYPE_VOID, mem);
    typer->type_void_row = type_new(TYPE_ROW, mem);

    return typer;
}

bool typer_check (Typer *typer, Plan *plan, String query, Mem *mem, DString *report, bool user_is_admin) {
    if (setjmp(typer->check.error)) return false;

    typer->check.mem = mem;
    typer->check.query = query;
    typer->check.report = report;
    typer->check.user_is_admin = user_is_admin;

    check(typer, plan);

    return true;
}

static Type_Table *create_table_from_plan (Typer *typer, Plan_Table_Def *plan) {
    u32 arena_block_size = sizeof(Type_Table) + sizeof(Type_Row) + sizeof(Row_Scope) + 10*sizeof(Type_Column);
    Mem_Arena *arena = mem_arena_new(typer->mem, arena_block_size);

    Type_Table *table   = type_new(TYPE_TABLE, (Mem*)arena);
    table->mem          = arena;
    table->prim_key_col = plan->prim_key_col;
    table->row          = type_new(TYPE_ROW, (Mem*)arena);

    String table_name = str_copy((Mem*)arena, plan->name);
    Row_Scope *scope = scope_new((Mem*)arena, table_name);
    array_add(&table->row->scopes, scope);

    array_iter (col, plan->cols) {
        Plan *C = (Plan*)col;

        Type_Column *col_type = type_new(TYPE_COLUMN, (Mem*)arena);
        col_type->name = str_copy((Mem*)arena, col->name);

        switch (C->flags & PLAN_COLUMN_DEF_TYPE) {
        case F_PLAN_COLUMN_DEF_TYPE_INT:  col_type->field = typer->type_int; break;
        case F_PLAN_COLUMN_DEF_TYPE_BOOL: col_type->field = typer->type_bool; break;
        case F_PLAN_COLUMN_DEF_TYPE_TEXT: col_type->field = typer->type_text; break;
        default: unreachable;
        }

        if (C->flags & F_PLAN_COLUMN_DEF_NOT_NULL) col_type->not_null = true;

        array_add(&scope->cols, col_type);
    }

    ((Plan*)plan)->type = (Type*)table;
    array_add(&typer->tables, table);

    return table;
}

static void create_table_from_sql (Typer *typer, Mem *mem, String sql, s64 engine_tag) {
    Plan *plan = parse_the_statement(sql, mem, TOKEN_CREATE, NULL);
    Type_Table *table_type = create_table_from_plan(typer, (Plan_Table_Def*)plan);
    BEngine *engine = db_get_engine(typer->db);
    table_type->engine_specific_info = btree_load(engine, table_type, engine_tag);
}

bool typer_add_table (Typer *typer, Plan_Table_Def *plan) {
    if (typer_get_table(typer, plan->name)) return false;

    Type_Table *table_type = create_table_from_plan(typer, plan);

    { // Create on-disk table:
        table_type->engine_specific_info = btree_new(db_get_engine(typer->db), table_type);
    }

    { // Create on-disk schema:
        ASSERT(! (((Plan*)plan)->flags & F_PLAN_WITHOUT_SOURCE));
        Source src = ((Plan*)plan)->src;
        String str = { .data = plan->text_base + src.offset, .count = src.length };

        Mem_Arena *arena = mem_arena_new(typer->mem, 1*KB);

        DString query = ds_new((Mem*)arena);
        ds_add_cstr(&query, "insert into CATALOG (");
        ds_add_fmt(&query, "\"%.*s\", \"%.*s\", %li)", plan->name.count, plan->name.data, str.count, str.data, bengine_get_tag(table_type));

        db_run_query(typer->db, ds_to_str(&query), (Mem*)arena, NULL, true);

        mem_arena_destroy(arena);
    }

    return true;
}

void typer_del_table (Typer *typer, String table_name) {
    Type_Table *table = typer_get_table(typer, table_name);
    if (! table) return;

    { // Delete on-disk table:
        BTree *tree = table->engine_specific_info;
        btree_delete(tree);
    }

    { // Delete on-disk schema:
        String name = array_get_first(&table->row->scopes)->name;
        DString query = ds_new((Mem*)table->mem);
        ds_add_fmt(&query, "delete from CATALOG where name = \"%.*s\"", name.count, name.data);
        db_run_query(typer->db, ds_to_str(&query), (Mem*)table->mem, NULL, true);
    }

    { // Delete in-memory schema:
        array_find_remove_fast(&typer->tables, table);
        mem_arena_destroy(table->mem);
    }
}

void typer_init_catalog (Typer *typer, bool db_is_empty) {
    Mem_Arena *arena = mem_arena_new(typer->mem, 1*KB);

    String text = str("create table CATALOG (\n"
                      "    name       text primary key,\n"
                      "    sql        text,\n"
                      "    engine_tag int\n"
                      ")");

    if (db_is_empty) {
        db_run_query(typer->db, text, (Mem*)arena, NULL, true);
    } else {
        create_table_from_sql(typer, (Mem*)arena, text, 1);

        Db_Query *query;
        db_query_init(&query, typer->db, str("select * from CATALOG"));

        while (1) {
            Db_Row *row = db_query_next(query);
            if (! row) break;

            String sql = *array_ref(&row->values, 1)->string;
            s64 engine_tag = array_ref(&row->values, 2)->integer;
            create_table_from_sql(typer, (Mem*)arena, sql, engine_tag);
        }

        db_query_close(query);
    }

    mem_arena_destroy(arena);
}

Type_Table *typer_get_table (Typer *typer, String name) {
    array_iter (table, typer->tables) {
        String table_name = array_get_first(&table->row->scopes)->name;
        if (str_match(name, table_name)) return table;
    }

    return NULL;
}

Type_Column *typer_get_col_type (Type_Row *row, u32 idx) {
    u32 cursor = 0;

    array_iter (scope, row->scopes) {
        array_iter (col_type, scope->cols) {
            if (cursor++ == idx) return col_type;
        }
    }

    return NULL;
}

static Noreturn error (Typer *typer, Plan *plan, char *fmt, ...) {
    report_fmt_va(typer->check.report, REPORT_ERROR, fmt);
    if (! (plan->flags & F_PLAN_WITHOUT_SOURCE)) report_source(typer->check.report, typer->check.query, plan->src);
    longjmp(typer->check.error, 1);
}

static Noreturn error2 (Typer *typer, Plan *p1, Plan *p2, char *fmt, ...) {
    report_fmt_va(typer->check.report, REPORT_ERROR, fmt);

    if (!(p1->flags & F_PLAN_WITHOUT_SOURCE) && !(p2->flags & F_PLAN_WITHOUT_SOURCE)) {
        report_sources(typer->check.report, typer->check.query, p1->src, p2->src);
    }

    longjmp(typer->check.error, 1);
}

static void match_type_tag (Typer *typer, Plan *plan, Type_Tag tag) {
    if (plan->tag == PLAN_LITERAL_NULL) return;
    if (plan->type->tag != tag) error(typer, plan, "Type mismatch: expected [%s] got [%s]", type_str[tag], type_str[plan->type->tag]);
}

static void match_type_tags (Typer *typer, Plan *plan1, Plan *plan2) {
    if (plan1->tag == PLAN_LITERAL_NULL) return;
    if (plan2->tag == PLAN_LITERAL_NULL) return;
    if (plan1->type->tag == plan2->type->tag) return;
    error2(typer, plan1, plan1, "Type mismatch. [%s] vs [%s]", type_str[plan1->type->tag], type_str[plan1->type->tag]);
}

static void set_input_row (Typer *typer, Type_Row *row) {
    typer->check.input_row = row;
}

static Type_Row *get_input_row (Typer *typer) {
    ASSERT(typer->check.input_row);
    return typer->check.input_row;
}

static Type_Row *get_row_type (Typer *typer, Plan *plan, String name) {
    Type_Table *result = typer_get_table(typer, name);
    if (! result) error(typer, plan, "Table does not exist.");
    return result->row;
}

static void check (Typer *typer, Plan *plan) {
    switch (plan->tag) {
    case PLAN_DROP: {
        Plan_Drop *P = (Plan_Drop*)plan;
        if (str_match(P->table, str("CATALOG")) && !typer->check.user_is_admin) error(typer, plan, "Cannot modify the 'CATALOG' table.");
        get_row_type(typer, plan, P->table);
        plan->type = typer->type_void;
    } break;

    case PLAN_TABLE_DEF: {
        Plan_Table_Def *P = (Plan_Table_Def*)plan;

        // TODO: The only reason we save this is because the Source
        // struct cannot by itself be used to reconstruct the String
        // since we don't have the damn base pointer... We really
        // should just make the Source struct be standalone.
        P->text_base = typer->check.query.data;

        if (typer_get_table(typer, P->name)) error(typer, plan, "Table already exits.");
        plan->type = typer->type_void;
    } break;

    case PLAN_INSERT: {
        Plan_Insert *P = (Plan_Insert*)plan;

        Type_Row *row = get_row_type(typer, plan, P->table);
        Array_Type_Column *col_types = &array_get_first(&row->scopes)->cols;
        Array_Plan values = ((Plan_Insert*)plan)->values;

        if (col_types->count != values.count) error(typer, plan, "Number of values to insert does not match number of columns [%i].", col_types->count);

        if (str_match(P->table, str("CATALOG")) && !typer->check.user_is_admin) error(typer, plan, "Cannot modify the 'CATALOG' table.");

        array_iter (value, values) {
            check(typer, value);
            Type_Column *col_type = array_get(col_types, ARRAY_IDX);
            match_type_tag(typer, value, col_type->field->tag);
        }

        plan->type = typer->type_void;
    } break;

    case PLAN_DELETE: {
        Plan_Delete *P = (Plan_Delete*)plan;
        if (str_match(P->table, str("CATALOG")) && !typer->check.user_is_admin) error(typer, plan, "Cannot modify the 'CATALOG' table.");
        set_input_row(typer, get_row_type(typer, plan, P->table));
        check(typer, P->filter);
        plan->type = typer->type_void;
    } break;

    case PLAN_UPDATE: {
        Plan_Update *P = (Plan_Update*)plan;
        Type_Row *row_type = get_row_type(typer, plan, P->table);
        Array_Type_Column *col_types = &array_get_first(&row_type->scopes)->cols;

        if (str_match(P->table, str("CATALOG")) && !typer->check.user_is_admin) error(typer, plan, "Cannot modify the 'CATALOG' table.");

        set_input_row(typer, row_type);

        check(typer, P->filter);

        array_iter (col, P->cols) {
            check(typer, (Plan*)col);

            Plan *val = array_get(&P->vals, ARRAY_IDX);
            Type_Column *col_type = array_get(col_types, col->idx);

            check(typer, val);
            match_type_tag(typer, val, col_type->field->tag);
        }

        plan->type = typer->type_void;
    } break;

    case PLAN_SCAN: {
        Plan_Scan *P = (Plan_Scan*)plan;
        Type_Row *row = get_row_type(typer, plan, P->table);

        if (P->alias.data) {
            Type_Row *new_row = type_new(TYPE_ROW, typer->check.mem);
            Row_Scope *new_scope = scope_new(typer->check.mem, P->alias);
            new_scope->cols = array_get_first(&row->scopes)->cols;
            array_add(&new_row->scopes, new_scope);
            row = new_row;
        }

        plan->type = (Type*)row;
    } break;

    case PLAN_SCAN_DUMMY: {
        plan->type = typer->type_void_row;
    } break;

    case PLAN_COLUMN_REF: {
        Plan_Column_Ref *P = (Plan_Column_Ref*)plan;
        Type_Row *row = get_input_row(typer);

        { // Find column type and index:
            u32 idx = 0;
            Type_Column *found_col_type = NULL;

            if (P->qualifier.data) {
                array_iter (scope, row->scopes) {
                    if (str_match(scope->name, P->qualifier)) {
                        array_iter (col, scope->cols) if (str_match(col->name, P->name)) { idx += ARRAY_IDX; found_col_type = col; break; }
                        break;
                    }

                    idx += scope->cols.count;
                }

                if (! found_col_type) error(typer, plan, "Column [%.*s.%.*s] does not exist.", P->qualifier.count, P->qualifier.data, P->name.count, P->name.data);
            } else {
                u32 i = 0;

                array_iter (scope, row->scopes) {
                    array_iter (col, scope->cols) {
                        if (str_match(col->name, P->name)) {
                            if (found_col_type) error(typer, plan, "Column reference [%.*s] is ambiguous.", P->name.count, P->name.data);
                            found_col_type = col;
                            idx = i;
                        }

                        i++;
                    }
                }

                if (! found_col_type) error(typer, plan, "Column [%.*s] does not exist.", P->name.count, P->name.data);
            }

            P->idx = idx;
            plan->type = found_col_type->field;
        }
    } break;

    case PLAN_AS: {
        check(typer, ((Plan_Op1*)plan)->op);
        plan->type = ((Plan_Op1*)plan)->op->type;
    } break;

    case PLAN_ORDER: {
        check(typer, ((Plan_Op1*)plan)->op);
        plan->type = ((Plan_Op1*)plan)->op->type;
        set_input_row(typer, (Type_Row*)plan->type);
        array_iter (key, ((Plan_Order*)plan)->keys) check(typer, key);
    } break;

    case PLAN_GROUP: {
        Plan_Group *P = (Plan_Group*)plan;
        Plan *op = ((Plan_Op1*)plan)->op;

        check(typer, op);
        set_input_row(typer, (Type_Row*)op->type);

        array_iter (key, P->keys) check(typer, key);

        array_iter_ptr (agg, P->aggregates) {
            check(typer, (Plan*)agg->ref);
            if (agg->tag != AGGREGATE_COUNT) match_type_tag(typer, (Plan*)agg->ref, TYPE_INT);
        }

        { // Build the new row type:
            Type_Row *row = type_new(TYPE_ROW, typer->check.mem);
            plan->type = (Type*)row;

            Row_Scope *noname_scope = scope_new(typer->check.mem, (String){0});
            array_add(&row->scopes, noname_scope);

            array_iter_ptr (agg, P->aggregates) {
                Type_Column *col_type = type_new(TYPE_COLUMN, typer->check.mem);
                col_type->field    = typer->type_int;
                col_type->name     = agg->name;
                array_add(&noname_scope->cols, col_type);
            }

            array_iter (key, P->keys) {
                if (key->tag != PLAN_COLUMN_REF) continue;

                Plan_Column_Ref *K = (Plan_Column_Ref*)key;

                if (array_get_first(&K->name) == '#') continue;

                Type_Column *col_type = type_new(TYPE_COLUMN, typer->check.mem);
                col_type->field    = key->type;
                col_type->name     = K->name;

                Row_Scope *scope = noname_scope;

                if (K->qualifier.data) {
                    array_iter (s, row->scopes) if (str_match(s->name, K->qualifier)) { scope = s; break; }

                    if (! scope) {
                        scope = scope_new(typer->check.mem, K->qualifier);
                        array_add(&row->scopes, scope);
                    }
                }

                array_add(&scope->cols, col_type);
            }
        }
    } break;

    case PLAN_FILTER: {
        check(typer, ((Plan_Op1*)plan)->op);
        plan->type = ((Plan_Op1*)plan)->op->type;
        set_input_row(typer, (Type_Row*)plan->type);
        check(typer, ((Plan_Filter*)plan)->expr);
        match_type_tag(typer, ((Plan_Filter*)plan)->expr, TYPE_BOOL);
    } break;

    case PLAN_LIMIT: {
        check(typer, ((Plan_Op1*)plan)->op);
        plan->type = ((Plan_Op1*)plan)->op->type;
    } break;

    case PLAN_PROJECTION: {
        Plan_Projection *P = (Plan_Projection*)plan;

        check(typer, ((Plan_Op1*)plan)->op);

        if (plan->flags & F_PLAN_SELECT_ALL) {
            Type_Row *row = (Type_Row*)array_get_first(&P->cols)->type;
            array_clear(&P->cols);

            array_iter (scope, row->scopes) {
                array_iter (col, scope->cols) {
                    Plan_Column_Ref *ref = plan_alloc(PLAN_COLUMN_REF, 0, typer->check.mem);
                    ((Plan*)ref)->src = plan->src;
                    if (row->scopes.count > 1) ref->qualifier = scope->name;
                    ref->name = col->name;
                    array_add(&P->cols, (Plan*)ref);
                }
            }
        }

        set_input_row(typer, (Type_Row*)((Plan_Op1*)plan)->op->type);
        array_iter (col, P->cols) check(typer, col);

        { // Build the new row type:
            Type_Row *row = type_new(TYPE_ROW, typer->check.mem);
            plan->type = (Type*)row;

            Row_Scope *noname_scope = scope_new(typer->check.mem, (String){0});
            array_add(&row->scopes, noname_scope);

            array_iter (col, P->cols) {
                Type_Column *col_type = type_new(TYPE_COLUMN, typer->check.mem);
                col_type->field = col->type;

                switch (col->tag) {
                case PLAN_COLUMN_REF: {
                    Plan_Column_Ref *P = (Plan_Column_Ref*)col;
                    col_type->name = P->name;

                    if (P->agg_expr.data) {
                        col_type->name = P->agg_expr;
                        array_add(&noname_scope->cols, col_type);
                    } else if (P->qualifier.data) {
                        Row_Scope *found_scope = NULL;
                        array_iter (scope, row->scopes) if (str_match(scope->name, P->qualifier)) { found_scope = scope; break; }

                        if (! found_scope) {
                            found_scope = scope_new(typer->check.mem, P->qualifier);
                            array_add(&row->scopes, found_scope);
                        }

                        array_add(&found_scope->cols, col_type);
                    } else {
                        array_add(&noname_scope->cols, col_type);
                    }
                } break;

                case PLAN_AS: {
                    col_type->name = ((Plan_As*)col)->name;
                    array_add(&noname_scope->cols, col_type);
                } break;

                default: {
                    DString ds = ds_new(typer->check.mem);
                    plan_print(&ds, col);
                    col_type->name = ds_to_str(&ds);
                    array_add(&noname_scope->cols, col_type);
                } break;
                }

            }
        }
    } break;

    case PLAN_JOIN_CROSS:
    case PLAN_JOIN_INNER: {
        Plan *op1 = ((Plan_Op2*)plan)->op1;
        Plan *op2 = ((Plan_Op2*)plan)->op2;

        check(typer, op1);
        check(typer, op2);

        { // Construct the new joined row type:
            Type_Row *row1 = (Type_Row*)op1->type;
            Type_Row *row2 = (Type_Row*)op2->type;

            array_iter (scope1, row1->scopes) {
                array_iter (scope2, row2->scopes) {
                    if (str_match(scope1->name, scope2->name)) {
                        error2(typer, op1, op2, "The rows to be joined contain the identical table name [%.*s].", scope1->name.count, scope1->name.data);
                    }
                }
            }

            Type_Row *row = type_new(TYPE_ROW, typer->check.mem);
            array_add_many(&row->scopes, &row1->scopes);
            array_add_many(&row->scopes, &row2->scopes);

            plan->type = (Type*)row;
        }

        if (plan->tag == PLAN_JOIN_INNER) {
            set_input_row(typer, (Type_Row*)plan->type);
            check(typer, ((Plan_Join_Inner*)plan)->on);
            match_type_tag(typer, ((Plan_Join_Inner*)plan)->on, TYPE_BOOL);
        }
    } break;

    case PLAN_EXPLAIN: {
        check(typer, ((Plan_Op1*)plan)->op);
        plan->type = typer->type_void;
    } break;

    case PLAN_EXPLAIN_RUN: {
        check(typer, ((Plan_Op1*)plan)->op);
        plan->type = ((Plan_Op1*)plan)->op->type;
    } break;

    case PLAN_LITERAL_INT: {
        plan->type = typer->type_int;
    } break;

    case PLAN_LITERAL_NULL: {
        plan->type = typer->type_void;
    } break;

    case PLAN_LITERAL_BOOL: {
        plan->type = typer->type_bool;
    } break;

    case PLAN_LITERAL_STRING: {
        plan->type = typer->type_text;
    } break;

    case PLAN_NEG: {
        Plan *op = ((Plan_Op1*)plan)->op;
        check(typer, op);
        match_type_tag(typer, op, TYPE_INT);
        plan->type = op->type;
    } break;

    case PLAN_ADD:
    case PLAN_SUB:
    case PLAN_MUL:
    case PLAN_DIV: {
        Plan *op1 = ((Plan_Op2*)plan)->op1;
        Plan *op2 = ((Plan_Op2*)plan)->op2;

        check(typer, op1);
        check(typer, op2);

        match_type_tag(typer, op1, TYPE_INT);
        match_type_tag(typer, op2, TYPE_INT);

        plan->type = op1->type;
    } break;

    case PLAN_NOT: {
        Plan *op = ((Plan_Op1*)plan)->op;
        check(typer, op);
        match_type_tag(typer, op, TYPE_BOOL);
        plan->type = typer->type_bool;
    } break;

    case PLAN_IS_NULL: {
        check(typer, ((Plan_Op1*)plan)->op);
        plan->type = typer->type_bool;
    } break;

    case PLAN_OR:
    case PLAN_AND: {
        Plan *op1 = ((Plan_Op2*)plan)->op1;
        Plan *op2 = ((Plan_Op2*)plan)->op2;

        check(typer, op1);
        check(typer, op2);

        match_type_tag(typer, op1, TYPE_BOOL);
        match_type_tag(typer, op2, TYPE_BOOL);

        plan->type = typer->type_bool;
    } break;

    case PLAN_EQUAL:
    case PLAN_NOT_EQUAL: {
        Plan *op1 = ((Plan_Op2*)plan)->op1;
        Plan *op2 = ((Plan_Op2*)plan)->op2;

        check(typer, op1);
        check(typer, op2);

        match_type_tags(typer, op1, op2);

        plan->type = typer->type_bool;
    } break;

    case PLAN_LESS:
    case PLAN_GREATER:
    case PLAN_LESS_EQUAL:
    case PLAN_GREATER_EQUAL: {
        Plan *op1 = ((Plan_Op2*)plan)->op1;
        Plan *op2 = ((Plan_Op2*)plan)->op2;

        check(typer, op1);
        check(typer, op2);

        match_type_tag(typer, op1, TYPE_INT);
        match_type_tag(typer, op2, TYPE_INT);

        plan->type = typer->type_bool;
    } break;

    default: unreachable;
    }
}
