#include <setjmp.h>

#include "runner.h"
#include "report.h"

struct Runner {
    Mem *mem;
    Mem_Arena *mem_tmp;
    jmp_buf error;
    Plan *plan;
    String query;
    Typer *typer;
    DString *report;
    BEngine *engine;
    Array(BCursor*) cursors;
};

static Db_Row *next (Runner *, Plan *);

static Noreturn error (Runner *run, Source src, char *fmt, ...) {
    report_fmt_va(run->report, REPORT_ERROR, fmt);
    report_source(run->report, run->query, src);
    longjmp(run->error, 1);
}

static bool value_match (Type *type, Db_Value v1, Db_Value v2) {
    switch (type->tag) {
    case TYPE_INT:  return (v1.integer == v2.integer);
    case TYPE_BOOL: return (v1.boolean == v2.boolean);
    case TYPE_TEXT: return str_match(*v1.string, *v2.string);
    default:        unreachable;
    }
}

static Db_Value eval_expr (Runner *run, Plan *expr, Db_Row *row) {
    #define EVAL1(OUTPUT, INPUT, OP) {\
        Db_Value v = eval_expr(run, ((Plan_Op1*)expr)->op, row);\
        if (v.is_null) return v;\
        v.OUTPUT = (OP v.INPUT);\
        return v;\
    }

    #define EVAL2(OUTPUT, INPUT, OP) {\
        Db_Value v1 = eval_expr(run, ((Plan_Op2*)expr)->op1, row);\
        Db_Value v2 = eval_expr(run, ((Plan_Op2*)expr)->op2, row);\
        if (v1.is_null) return v1;\
        if (v2.is_null) return v2;\
        v1.OUTPUT = (v1.INPUT OP v2.INPUT);\
        return v1;\
    }

    switch (expr->tag) {
    case PLAN_AS:             return eval_expr(run, ((Plan_Op1*)expr)->op, row);
    case PLAN_COLUMN_REF:     return array_get(&row->values, ((Plan_Column_Ref*)expr)->idx);

    case PLAN_LITERAL_INT:    return (Db_Value){ .integer = ((Plan_Literal_Int*)expr)->val };
    case PLAN_LITERAL_BOOL:   return (Db_Value){ .boolean = ((Plan_Literal_Bool*)expr)->val };
    case PLAN_LITERAL_NULL:   return (Db_Value){ .is_null = true };
    case PLAN_LITERAL_STRING: return (Db_Value){ .string  = &((Plan_Literal_String*)expr)->val };

    case PLAN_NOT:            EVAL1(boolean, boolean, !);
    case PLAN_NEG:            EVAL1(integer, integer, -);
    case PLAN_ADD:            EVAL2(integer, integer, +);
    case PLAN_SUB:            EVAL2(integer, integer, -);
    case PLAN_MUL:            EVAL2(integer, integer, *);
    case PLAN_DIV:            EVAL2(integer, integer, /);
    case PLAN_LESS:           EVAL2(boolean, integer, <);
    case PLAN_GREATER:        EVAL2(boolean, integer, >);
    case PLAN_LESS_EQUAL:     EVAL2(boolean, integer, <=);
    case PLAN_GREATER_EQUAL:  EVAL2(boolean, integer, >=);

    case PLAN_IS_NULL: {
        Db_Value v = eval_expr(run, ((Plan_Op1*)expr)->op, row);
        v.boolean = v.is_null;
        v.is_null = false;
        return v;
    }

    case PLAN_OR: {
        Db_Value v1 = eval_expr(run, ((Plan_Op2*)expr)->op1, row);
        Db_Value v2 = eval_expr(run, ((Plan_Op2*)expr)->op2, row);

        if (v1.is_null) return (!v2.is_null && v2.boolean) ? v2 : v1;
        if (v2.is_null) return v1.boolean ? v1 : v2;

        v1.boolean = v1.boolean || v2.boolean;
        return v1;
    }

    case PLAN_AND: {
        Db_Value v1 = eval_expr(run, ((Plan_Op2*)expr)->op1, row);
        Db_Value v2 = eval_expr(run, ((Plan_Op2*)expr)->op2, row);

        if (v1.is_null) return (!v2.is_null && !v2.boolean) ? v2 : v1;
        if (v2.is_null) return !v1.boolean ? v1 : v2;

        v1.boolean = v1.boolean && v2.boolean;
        return v1;
    }

    case PLAN_EQUAL:
    case PLAN_NOT_EQUAL: {
        Db_Value v1 = eval_expr(run, ((Plan_Op2*)expr)->op1, row);
        Db_Value v2 = eval_expr(run, ((Plan_Op2*)expr)->op2, row);

        if (v1.is_null) return v1;
        if (v2.is_null) return v2;

        v1.boolean = value_match(((Plan_Op2*)expr)->op1->type, v1, v2);
        if (expr->tag == PLAN_NOT_EQUAL) v1.boolean = !v1.boolean;
        return v1;
    }

    default: unreachable;
    }

    #undef EVAL1
    #undef EVAL2
}

static Db_Row *row_new (Runner *run, Type_Row *type) {
    Db_Row *row = MEM_ALLOC(run->mem_tmp, sizeof(Db_Row));
    row->type = type;
    array_init(&row->values, (Mem*)run->mem_tmp);
    return row;
}

// TODO: Document the format of the on-disk row.
//
// We could also improve this format a lot:
//
// - For example, we could in certain cases not put the length
//   of the row at the beginning if the Typer knows the length
//   of the row statically.
//
// - We could also use a bitmask to represent nulls instead of
//   placing a byte in front of every field.
static u8 *serialize_row (Runner *run, Db_Row *row) {
    u32 length = 4;

    array_iter (value, row->values) {
        length++; // For the is_null byte.
        if (value.is_null) continue;

        Type_Column *col_type = typer_get_col_type(row->type, ARRAY_IDX);

        switch (col_type->field->tag) {
        case TYPE_INT:  length += 8; break;
        case TYPE_BOOL: length += 1; break;
        case TYPE_TEXT: length += 4 + value.string->count; break;
        default:        unreachable;
        }
    }

    u8 *result = MEM_ALLOC(run->mem_tmp, length);
    u8 *cursor = result;

    write_u32_le(cursor, length);
    cursor += 4;

    array_iter (value, row->values) {
        if (value.is_null) { *cursor++ = 1; continue; }
        *cursor++ = 0;

        Type_Column *col_type = typer_get_col_type(row->type, ARRAY_IDX);

        switch (col_type->field->tag) {
        case TYPE_BOOL: *cursor++ = value.boolean; break;
        case TYPE_INT:  write_s64_le(cursor, value.integer); cursor += 8; break;

        case TYPE_TEXT: {
            u32 count = value.string->count;
            write_u32_le(cursor, count);
            cursor += 4;
            memcpy(cursor, value.string->data, count);
            cursor += count;
        } break;

        default: unreachable;
        }
    }

    return result;
}

static Db_Row *deserialize_row (Runner *run, Type_Table *table, u8 *buf) {
    Db_Row *row = row_new(run, table->row);
    row->type = table->row;

    Row_Scope *scope = array_get_first(&table->row->scopes);

    buf += 4; // Skip the buf length.

    array_iter (col_type, scope->cols) {
        bool is_null = *buf++;

        if (is_null) {
            array_add(&row->values, (Db_Value){ .is_null = true });
            continue;
        }

        switch (col_type->field->tag) {
        case TYPE_INT: {
            s64 val = read_s64_le(buf);
            buf += 8;
            array_add(&row->values, (Db_Value){ .integer = val });
        } break;

        case TYPE_BOOL: {
            u8 val = *buf++;
            array_add(&row->values, (Db_Value){ .boolean = val });
        } break;

        case TYPE_TEXT: {
            u32 count = read_u32_le(buf);
            buf += 4;
            char *str = mem_copy((Mem*)run->mem_tmp, buf, count);
            buf += count;

            String *val = MEM_ALLOC(run->mem_tmp, sizeof(String));
            val->count  = count;
            val->data   = str;

            array_add(&row->values, (Db_Value){ .string = val });
        } break;

        default: unreachable;
        }
    }

    return row;
}

typedef struct {
    Db_Row *row;
    Array(Db_Value) keys;
} Sort_Item;

typedef struct {
    u32 idx;
    Array_Plan *keys;
    Array(Sort_Item*) items;
    Array_Bool *directions;
} Sorter;

static bool a_before_b (Sorter *sorter, Sort_Item *A, Sort_Item *B) {
    array_iter (key1, A->keys) {
        Db_Value key2 = array_get(&B->keys, ARRAY_IDX);
        Type *key_type = array_get(sorter->keys, ARRAY_IDX)->type;
        bool ascending = sorter->directions ? array_get(sorter->directions, ARRAY_IDX) : true;

        if (! ascending) swap(key1, key2);

        if (key1.is_null && key2.is_null) continue;
        if (key1.is_null) return true;
        if (key2.is_null) return false;

        switch (key_type->tag) {
        case TYPE_INT: {
            if (key1.integer == key2.integer) continue;
            return key1.integer < key2.integer;
        }

        case TYPE_BOOL: {
            if (key1.boolean == key2.boolean) continue;
            return !key1.boolean && key2.boolean;
        }

        case TYPE_TEXT: {
            int result = strncmp(key1.string->data, key2.string->data, MIN(key1.string->count, key2.string->count));
            if (result == 0) continue;
            return result == -1;
        }

        default: unreachable;
        }
    }

    return false;
}

static Sort_Item *sort_item_new (Runner *run, Array_Plan *keys, Db_Row *row) {
    Sort_Item *item = MEM_ALLOC((Mem*)run->mem_tmp, sizeof(Sort_Item));
    item->row = row;
    array_init(&item->keys, (Mem*)run->mem_tmp);
    array_iter (key, *keys) array_add(&item->keys, eval_expr(run, key, row));
    return item;
}

// Setting @directions to NULL means all keys are in ascending order.
static Sorter *sorter_new (Runner *run, Plan *input, Array_Bool *directions, Array_Plan *keys) {
    Sorter *sorter = MEM_ALLOC((Mem*)run->mem_tmp, sizeof(Sorter));

    { // Init:
        sorter->idx = 0;
        sorter->keys = keys;
        sorter->directions = directions;
        array_init(&sorter->items, (Mem*)run->mem_tmp);
    }

    // Collect:
    while (1) {
        Db_Row *row = next(run, input);
        if (! row) break;
        array_add(&sorter->items, sort_item_new(run, keys, row));
    }

    { // Sort:

        // TODO: For now we only support in-memory sorting. Would
        // be nice to support something like external merge sort.
        //
        // We also use a simple insertion sort algorithm because
        // the C lib qsort function can't pass a context struct
        // to the comparator function...
        Array_View(Sort_Item*) sorted = { .count = 1, .data = sorter->items.data };

        while (sorted.count < sorter->items.count) {
            Sort_Item *A = array_get(&sorter->items, sorted.count);

            array_iter_ptr (B, sorted) {
                if (a_before_b(sorter, A, *B)) {
                    memmove(B+1, B, (sorted.count - ARRAY_IDX) * array_elem_size(&sorted));
                    *B = A;
                    break;
                }
            }

            sorted.count++;
        }
    }

    return sorter;
}

static Sort_Item *sorter_peek (Sorter *sorter) {
    return (sorter->idx < sorter->items.count) ?
           array_get(&sorter->items, sorter->idx) :
           NULL;
}

static Sort_Item *sorter_next (Sorter *sorter) {
    return (sorter->idx < sorter->items.count) ?
           array_get(&sorter->items, sorter->idx++) :
           NULL;
}

static void sorter_reset (Sorter *sorter) {
    sorter->idx = 0;
}

static void sorter_close (Sorter *sorter) {
    // TODO: We should be able to release the
    // memory without also releasing the memory
    // of the operators above this one.
}

static bool passes_filter (Runner *run, Plan *filter, Db_Row *row) {
    Db_Value result = eval_expr(run, filter, row);
    return !result.is_null && result.boolean;
}

static void explain (Runner *run, Plan *plan) {
    if (run->report) {
        ds_add_fmt(run->report, "Plan for query at line [%i]:\n", plan->src.first_line);
        plan_print_indent(run->report, plan, 1);
        ds_add_2byte(run->report, '\n', '\n');
    }
}

static void close (Runner *run, Plan *plan) {
    switch (plan->tag) {
    case PLAN_SCAN: {
        Plan_Scan *P = (Plan_Scan*)plan;
        P->done = false;
        BCursor *cursor = array_get(&run->cursors, P->cur - 1);
        bcursor_close(cursor);
    } break;

    case PLAN_SCAN_DUMMY: {
        ((Plan_Scan_Dummy*)plan)->done = false;
    } break;

    case PLAN_ORDER: {
        Sorter *sorter = ((Plan_Order*)plan)->sorter;
        sorter_close(sorter);
    } break;

    case PLAN_GROUP: {
        Sorter *sorter = ((Plan_Group*)plan)->sorter;
        if (sorter) sorter_close(sorter);
        else        close(run, ((Plan_Op1*)plan)->op);
    } break;

    default: {
        if (plan_has_bases(plan, PLAN_OP1)) {
            close(run, ((Plan_Op1*)plan)->op);
        } else if (plan_has_bases(plan, PLAN_OP2)) {
            close(run, ((Plan_Op2*)plan)->op1);
            close(run, ((Plan_Op2*)plan)->op2);
        }
    } break;
    }
}

static void reset (Runner *run, Plan *plan) {
    switch (plan->tag) {
    case PLAN_SCAN: {
        Plan_Scan *P = (Plan_Scan*)plan;
        P->done = false;
        BCursor *cursor = array_get(&run->cursors, P->cur - 1);
        bcursor_goto_first(cursor);
    } break;

    case PLAN_SCAN_DUMMY: {
        ((Plan_Scan_Dummy*)plan)->done = false;
    } break;

    case PLAN_ORDER: {
        Sorter *sorter = ((Plan_Order*)plan)->sorter;
        sorter_reset(sorter);
    } break;

    case PLAN_GROUP: {
        Sorter *sorter = ((Plan_Group*)plan)->sorter;
        if (sorter) sorter_reset(sorter);
        else        reset(run, ((Plan_Op1*)plan)->op);
    } break;

    default: {
        if (plan_has_bases(plan, PLAN_OP1)) {
            reset(run, ((Plan_Op1*)plan)->op);
        } else if (plan_has_bases(plan, PLAN_OP2)) {
            reset(run, ((Plan_Op2*)plan)->op1);
            reset(run, ((Plan_Op2*)plan)->op2);
        }
    } break;
    }
}

static Db_Row *next (Runner *run, Plan *plan) {
    switch (plan->tag) {
    case PLAN_EXPLAIN: {
        explain(run, ((Plan_Op1*)plan)->op);
        return NULL;
    }

    case PLAN_EXPLAIN_RUN: {
        Plan *op = ((Plan_Op1*)plan)->op;
        Db_Row *row = next(run, op);
        if (row) return row;
        explain(run, op);
        return NULL;
    }

    case PLAN_TABLE_DEF: {
        typer_add_table(run->typer, (Plan_Table_Def*)plan);
        return NULL;
    }

    case PLAN_DROP: {
        typer_del_table(run->typer, ((Plan_Drop*)plan)->table);
        return NULL;
    }

    case PLAN_DELETE: {
        Plan_Delete *P    = (Plan_Delete*)plan;
        Type_Table *table = typer_get_table(run->typer, P->table);
        BTree *tree       = table->engine_specific_info;
        BCursor *cursor   = bcursor_new(tree);

        if (! bcursor_goto_first(cursor)) return NULL;

        while (1) {
            Db_Row *row = deserialize_row(run, table, bcursor_read(cursor).ptr);
            if (passes_filter(run, P->filter, row)) bcursor_remove(cursor);
            if (! bcursor_goto_next(cursor)) break;
        }

        bcursor_close(cursor);
        return NULL;
    }

    case PLAN_UPDATE: {
        Plan_Update *P          = (Plan_Update*)plan;
        Type_Table *table       = typer_get_table(run->typer, P->table);
        Array_Type_Column *cols = &array_get_first(&table->row->scopes)->cols;
        BTree *tree             = table->engine_specific_info;
        BCursor *cursor         = bcursor_new(tree);

        if (! bcursor_goto_first(cursor)) return NULL;

        Array(Db_Value) tmp_row;
        array_init_cap(&tmp_row, run->mem, cols->count);
        tmp_row.count = cols->count;

        while (1) {
            Db_Row *row = deserialize_row(run, table, bcursor_read(cursor).ptr);

            if (passes_filter(run, P->filter, row)) {
                array_iter (col, P->cols) {
                    Type_Column *col_type = array_get(cols, col->idx);
                    Plan *expr         = array_get(&P->vals, ARRAY_IDX);
                    Db_Value value    = eval_expr(run, expr, row);

                    if (col_type->not_null && value.is_null) error(run, ((Plan*)col)->src, "Attempting to set null on a column with a 'NOT NULL' constraint.");

                    array_set(&tmp_row, col->idx, value);
                }

                array_iter (col, P->cols) {
                    Db_Value new_val = array_get(&tmp_row, col->idx);
                    array_set(&row->values, col->idx, new_val);
                }

                bcursor_update(cursor, (Val){ serialize_row(run, row) });
            }

            if (! bcursor_goto_next(cursor)) break;
            mem_arena_clear(run->mem_tmp);
        }

        array_free(&tmp_row);
        return NULL;
    }

    case PLAN_INSERT: {
        Plan_Insert *P = (Plan_Insert*)plan;
        Type_Table *table = typer_get_table(run->typer, P->table);
        Array_Type_Column *cols = &array_get_first(&table->row->scopes)->cols;

        UKey ukey;
        Db_Row row = { .type = table->row };
        array_init(&row.values, (Mem*)run->mem_tmp);

        array_iter (expr, P->values) {
            Db_Value value = eval_expr(run, expr, NULL);
            Type_Column *col_type = array_get(cols, ARRAY_IDX);

            if (col_type->not_null && value.is_null) error(run, expr->src, "Attempting to set null on a column with a 'NOT NULL' constraint.");
            array_add(&row.values, value);

            if (ARRAY_IDX == table->prim_key_col) {
                switch (col_type->field->tag) {
                case TYPE_INT:  ukey.ptr = &array_ref_last(&row.values)->integer; break;
                case TYPE_BOOL: ukey.ptr = &array_ref_last(&row.values)->boolean; break;
                case TYPE_TEXT: ukey.ptr = array_ref_last(&row.values)->string; break;
                default: unreachable;
                }
            }
        }

        BTree *tree = table->engine_specific_info;
        BCursor *cursor = bcursor_new(tree);

        bcursor_goto_ukey(cursor, ukey);
        bcursor_insert(cursor, ukey, (Val){ serialize_row(run, &row) });
        bcursor_close(cursor);

        return NULL;
    }

    case PLAN_LIMIT: {
        Plan_Limit *P = (Plan_Limit*)plan;
        Plan *input = ((Plan_Op1*)plan)->op;

        switch (P->state) {
        case 0: {
            for (s64 i = 0; i < P->offset; ++i) if (! next(run, input)) { P->state = 2; return NULL; }
            P->state = 1;
        } // fallthrough

        case 1: {
            if (P->emitted == P->limit) { P->state = 2; return NULL; }
            P->emitted++;
            Db_Row *row = next(run, input);
            if (! row) P->state = 2;
            return row;
        }

        default: return NULL;
        }
    }

    case PLAN_ORDER: {
        Plan_Order *P = (Plan_Order*)plan;
        if (! P->sorter) P->sorter = sorter_new(run, ((Plan_Op1*)plan)->op, &P->directions, &P->keys);
        Sort_Item *it = sorter_next(P->sorter);
        return it ? it->row : NULL;
    }

    case PLAN_GROUP: {
        Plan_Group *P = (Plan_Group*)plan;
        Plan *input   = ((Plan_Op1*)plan)->op;
        s64 count     = 1;

        #define AGGREGATE(input_row) do{\
            array_iter_ptr (agg, P->aggregates) {\
                Db_Value *acc = array_ref(&output_row->values, ARRAY_IDX);\
                Db_Value val  = array_get(&(input_row)->values, agg->ref->idx);\
                if (! val.is_null) {\
                    switch (agg->tag) {\
                    case AGGREGATE_AVG:   acc->integer += val.integer; break;\
                    case AGGREGATE_SUM:   acc->integer += val.integer; break;\
                    case AGGREGATE_MAX:   if (val.integer < acc->integer) { acc->integer = val.integer; } break;\
                    case AGGREGATE_MIN:   if (val.integer < acc->integer) { acc->integer = val.integer; } break;\
                    case AGGREGATE_COUNT: acc->integer++; break;\
                    default:              break;\
                    }\
                }\
            }\
        }while(0)

        Db_Row *output_row = NULL;

        if (P->keys.count == 0) {
            Db_Row *input_row = next(run, input);
            if (! input_row) return NULL;

            output_row = row_new(run, (Type_Row*)plan->type);
            array_iter_ptr (agg, P->aggregates) array_add(&output_row->values, (Db_Value){0});

            while (1) {
                AGGREGATE(input_row);
                input_row = next(run, input);
                if (! input_row) break;
                count++;
            }
        } else {
            if (! P->sorter) P->sorter = sorter_new(run, input, NULL, &P->keys);

            Sort_Item *input_row = sorter_next(P->sorter);
            if (! input_row) return NULL;

            output_row = row_new(run, (Type_Row*)plan->type);
            array_iter_ptr (agg, P->aggregates) array_add(&output_row->values, (Db_Value){0});

            while (1) {
                AGGREGATE(input_row->row);

                Sort_Item *next_input_row = sorter_peek(P->sorter);
                if (! next_input_row) break;

                array_iter (key1, input_row->keys) { // See if the next input row is in a different group:
                    Db_Value key2 = array_get(&next_input_row->keys, ARRAY_IDX);
                    Type *type = array_get(&P->keys, ARRAY_IDX)->type;
                    if (! value_match(type, key1, key2)) goto done;
                }

                input_row = sorter_next(P->sorter);
                count++;
            } done:

            array_iter (key, P->keys) {
                Db_Value value = array_get(&input_row->row->values, ((Plan_Column_Ref*)key)->idx);
                array_add(&output_row->values, value);
            }
        }

        array_iter_ptr (agg, P->aggregates) {
            Db_Value *acc = array_ref(&output_row->values, ARRAY_IDX);

            switch (agg->tag) {
            case AGGREGATE_AVG:       acc->integer /= count; break;
            case AGGREGATE_COUNT_ALL: acc->integer  = count; break;
            default: break;
            }
        }

        #undef AGGREGATE
        return output_row;
    }

    case PLAN_PROJECTION: {
        Plan_Projection *P = (Plan_Projection*)plan;
        Type_Row *row_type = (Type_Row*)plan->type;

        Db_Row *row = next(run, ((Plan_Op1*)plan)->op);
        if (! row) return NULL;

        Typeof(row->values) values;
        array_init(&values, (Mem*)run->mem_tmp);

        u32 idx = 0;
        array_iter (scope, row_type->scopes) {
            array_iter (col_type, scope->cols) {
                Plan *expr = array_get(&P->cols, idx++);
                array_add(&values, eval_expr(run, expr, row));
            }
        }

        row->type = row_type;
        row->values = values;

        return row;
    }

    case PLAN_FILTER: {
        while (1) {
            Db_Row *row = next(run, ((Plan_Op1*)plan)->op);
            if (! row) return NULL;
            if (passes_filter(run, ((Plan_Filter*)plan)->expr, row)) return row;
        }
    }

    case PLAN_JOIN_CROSS: {
        Plan_Join_Cross *P = (Plan_Join_Cross*)plan;

        Plan *op1 = ((Plan_Op2*)plan)->op1;
        Plan *op2 = ((Plan_Op2*)plan)->op2;

        if (! P->cur) P->cur = next(run, op1);
        if (! P->cur) return NULL;

        Db_Row *row1 = P->cur;
        Db_Row *row2 = next(run, op2);

        if (! row2) {
            row1 = P->cur = next(run, op1);
            if (! row1) return NULL;
            reset(run, op2);
            row2 = next(run, op2);
        }

        row2->type = (Type_Row*)plan->type;
        array_prepend_many(&row2->values, &row1->values);

        return row2;
    }

    case PLAN_JOIN_INNER: {
        Plan_Join_Inner *P = (Plan_Join_Inner*)plan;

        Plan *op1 = ((Plan_Op2*)plan)->op1;
        Plan *op2 = ((Plan_Op2*)plan)->op2;

        if (! P->cur) P->cur = next(run, op1);
        if (! P->cur) return NULL;

        Db_Row *row1 = P->cur;

        while (1) {
            Db_Row *row2 = next(run, op2);

            if (! row2) {
                row1 = P->cur = next(run, op1);
                if (! row1) return NULL;
                reset(run, op2);
                row2 = next(run, op2);
            }

            row2->type = (Type_Row*)plan->type;
            array_prepend_many(&row2->values, &row1->values);

            if (passes_filter(run, P->on, row2)) return row2;
        }
    }

    case PLAN_SCAN_DUMMY: {
        Plan_Scan_Dummy *P = (Plan_Scan_Dummy*)plan;
        if (P->done) return NULL;

        Db_Row *row = MEM_ALLOC((Mem*)run->mem_tmp, sizeof(Db_Row));
        row->type = (Type_Row*)plan->type;
        array_init(&row->values, (Mem*)run->mem_tmp);
        P->done = true;

        return row;
    }

    case PLAN_SCAN: {
        Plan_Scan *P = (Plan_Scan*)plan;

        if (P->done) return NULL;

        Type_Table *table = typer_get_table(run->typer, P->table);

        if (P->cur == 0) {
            BTree *tree = table->engine_specific_info;
            BCursor *cursor = bcursor_new(tree);
            array_add(&run->cursors, cursor);
            P->cur = run->cursors.count;
            if (! bcursor_goto_first(cursor)) { P->done = true; return NULL; }
        }

        BCursor *cursor = array_get(&run->cursors, P->cur - 1);
        Db_Row *row = deserialize_row(run, table, bcursor_read(cursor).ptr);

        if (! bcursor_goto_next(cursor)) P->done = true;

        return row;
    }

    default: return NULL;
    }
}

Db_Row *run_next (Runner *run) {
    if (setjmp(run->error)) return NULL;
    return next(run, run->plan);
}

void run_close (Runner *run) {
    close(run, run->plan);
}

Runner *run_new (Plan *plan, String query, Typer *typer, BEngine *engine, Mem *mem, DString *report) {
    Runner *run = MEM_ALLOC(mem, sizeof(Runner));

    run->mem     = mem;
    run->mem_tmp = mem_arena_new(mem, 4*KB);
    run->plan    = plan;
    run->query   = query;
    run->typer   = typer;
    run->report  = report;
    run->engine  = engine;

    array_init(&run->cursors, mem);

    return run;
}

typedef enum {
    TCELL_INT,
    TCELL_NULL,
    TCELL_BOOL,
    TCELL_TEXT,
    TCELL_NAME,
    TCELL_BLANK,
} Table_Cell_Tag;

typedef struct {
    Table_Cell_Tag tag;

    union {
        s64 integer;
        bool boolean;
        struct { u32 offset; String str; } text;
        struct { String qualifier, name; } name;
    };
} Table_Cell;

typedef struct {
    Mem *mem;
    Runner *run;
    DString *ds;
    u32 row_width;
    Array_u32 col_widths;
    Array(Db_Row*) rows;
    Array(Table_Cell) row_to_print;
} Table_Printer;

// Return the length of the longest line.
// Multibyte UTF8 characters count as 1.
static u32 tp_get_text_width (String str) {
    u32 max = 0;
    u32 cur = 0;

    array_iter (ch, str) {
        if (ch == '\n') {
            if (cur > max) max = cur;
            cur = 0;
        } else if (((u8)ch & 0xc0) != 0x80) {
            cur++;
        }
    }

    return MAX(cur, max);
}

static u32 tp_get_cell_width (Db_Value value, Type *type) {
    if (value.is_null) return 4;

    switch (type->tag) {
    case TYPE_TEXT: return tp_get_text_width(*value.string);
    case TYPE_INT:  return digit_count(value.integer);
    case TYPE_BOOL: return 5;
    default:        unreachable;
    }
}

static void tp_print_top_row_separator (Table_Printer *tp) {
    ds_add_cstr(tp->ds, "┌");

    array_iter (W, tp->col_widths) {
        for (u32 i = 0; i < (W+2); ++i) ds_add_cstr(tp->ds, "─");
        if (! ARRAY_ITER_ON_LAST_ELEMENT) ds_add_cstr(tp->ds, "┬");
    }

    ds_add_cstr(tp->ds, "┐\n");
}

static void tp_print_mid_row_separator (Table_Printer *tp) {
    ds_add_cstr(tp->ds, "├");

    array_iter (W, tp->col_widths) {
        for (u32 i = 0; i < (W+2); ++i) ds_add_cstr(tp->ds, "─");
        if (! ARRAY_ITER_ON_LAST_ELEMENT) ds_add_cstr(tp->ds, "┼");
    }

    ds_add_cstr(tp->ds, "┤\n");
}

static void tp_print_bot_row_separator (Table_Printer *tp) {
    ds_add_cstr(tp->ds, "└");

    array_iter (W, tp->col_widths) {
        for (u32 i = 0; i < (W+2); ++i) ds_add_cstr(tp->ds, "─");
        if (! ARRAY_ITER_ON_LAST_ELEMENT) ds_add_cstr(tp->ds, "┴");
    }

    ds_add_cstr(tp->ds, "┘\n");
}

static void tp_print_row (Table_Printer *tp, bool is_last_row) {
    bool looping = true;
    while (looping) {
        looping = false;

        ds_add_cstr(tp->ds, "│");

        array_iter_ptr (cell, tp->row_to_print) {
            u32 W = array_get(&tp->col_widths, ARRAY_IDX);

            ds_add_byte(tp->ds, ' ');

            switch (cell->tag) {
            case TCELL_BLANK: {
                ds_add_fmt(tp->ds, "%-*s", W, "");
            } break;

            case TCELL_NULL: {
                ds_add_fmt(tp->ds, "%-*s", W, "NULL");
                cell->tag = TCELL_BLANK;
            } break;

            case TCELL_INT: {
                ds_add_fmt(tp->ds, "%-*li", W, cell->integer);
                cell->tag = TCELL_BLANK;
            } break;

            case TCELL_BOOL: {
                ds_add_fmt(tp->ds, "%-*s", W, cell->boolean ? "true" : "false");
                cell->tag = TCELL_BLANK;
            } break;

            case TCELL_NAME: {
                String a = cell->name.qualifier;
                String b = cell->name.name;
                u32 len  = 1 + tp_get_text_width(a) + tp_get_text_width(b);
                ds_add_fmt(tp->ds, "%.*s.%.*s%-*s", a.count, a.data, b.count, b.data, W - len, "");
                cell->tag = TCELL_BLANK;
            } break;

            case TCELL_TEXT: {
                String line = { .data = "" };

                array_iter_from (ch, cell->text.str, cell->text.offset) {
                    if (ch == '\n' || ARRAY_ITER_ON_LAST_ELEMENT) {
                        line.count = ARRAY_IDX - cell->text.offset + (ch != '\n');
                        line.data  = cell->text.str.data + cell->text.offset;
                        cell->text.offset = ARRAY_IDX + 1;
                        break;
                    }
                }

                u32 len = tp_get_text_width(line);
                ds_add_fmt(tp->ds, "%.*s%-*s", line.count, line.data, W - len, "");

                if (cell->text.offset == cell->text.str.count) {
                    cell->tag = TCELL_BLANK;
                } else {
                    looping = true;
                }
            } break;
            }

            ds_add_cstr(tp->ds, " │");
        }

        ds_add_byte(tp->ds, '\n');
    }

    if (is_last_row) tp_print_bot_row_separator(tp);
    else             tp_print_mid_row_separator(tp);
}

void run_print_table (Runner *run, DString *ds) {
    ASSERT(run->plan->type->tag == TYPE_ROW);

    Type_Row *row = (Type_Row*)run->plan->type;
    Table_Printer *tp = MEM_ALLOC(run->mem_tmp, sizeof(Table_Printer));

    { // Init:
        tp->ds  = ds;
        tp->run = run;
        tp->mem = (Mem*)run->mem_tmp;

        array_init(&tp->rows, tp->mem);
        array_init(&tp->col_widths, tp->mem);
        array_init(&tp->row_to_print, tp->mem);
    }

    // Collect the column widths of the header:
    array_iter (scope, row->scopes) {
        array_iter (col, scope->cols) {
            if (scope->name.data) {
                array_add(&tp->col_widths, scope->name.count + col->name.count + 1);
            } else {
                array_add(&tp->col_widths, tp_get_text_width(col->name));
            }
        }
    }

    { // Collect all rows and compute the widths of all columns:
        while (1) {
            Db_Row *row = run_next(run);
            if (! row) break;

            array_add(&tp->rows, row);

            array_iter (value, row->values) {
                u32 *prev   = array_ref(&tp->col_widths, ARRAY_IDX);
                u32 current = tp_get_cell_width(value, typer_get_col_type(row->type, ARRAY_IDX)->field);
                if (current > *prev) *prev = current;
            }
        }

        array_iter (w, tp->col_widths) tp->row_width += w;
    }

    { // Print the header:
        tp_print_top_row_separator(tp);

        array_clear(&tp->row_to_print);

        array_iter (scope, row->scopes) {
            array_iter (col, scope->cols) {
                if (scope->name.data) {
                    array_add(&tp->row_to_print, ((Table_Cell){
                        .tag = TCELL_NAME,
                        .name.name = col->name,
                        .name.qualifier = scope->name,
                    }));
                } else {
                    array_add(&tp->row_to_print, ((Table_Cell){
                        .tag = TCELL_TEXT,
                        .text.str = col->name,
                    }));
                }
            }
        }

        tp_print_row(tp, tp->rows.count == 0);
    }

    // Print the rows:
    array_iter (row, tp->rows) {
        array_clear(&tp->row_to_print);

        array_iter (value, row->values) {
            Table_Cell cell;

            if (value.is_null) {
                cell = (Table_Cell){ .tag = TCELL_NULL };
            } else {
                Type_Column *col_type = typer_get_col_type(row->type, ARRAY_IDX);

                switch (col_type->field->tag) {
                case TYPE_INT:  cell = (Table_Cell){ .tag = TCELL_INT, .integer = value.integer }; break;
                case TYPE_BOOL: cell = (Table_Cell){ .tag = TCELL_BOOL, .boolean = value.boolean }; break;
                case TYPE_TEXT: cell = (Table_Cell){ .tag = TCELL_TEXT, .text.str = *value.string }; break;
                default:        unreachable;
                }
            }

            array_add(&tp->row_to_print, cell);
        }

        tp_print_row(tp, ARRAY_ITER_ON_LAST_ELEMENT);
    }
}
