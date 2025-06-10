#include "plan.h"
#include "string.h"

char *plan_str [PLAN_TAG_MAX_] = {
    #define X(_, __, str, ...) str,
    X_PLAN
    #undef X
};

size_t plan_type_size [PLAN_TAG_MAX_] = {
    #define X(_, type, ...) sizeof(type),
    X_PLAN
    #undef X
};

u64 plan_bases [PLAN_TAG_MAX_] = {
    #define X(a, b, c, d, base_flags) (PLAN | base_flags),
    X_PLAN
    #undef X
};

u32 plan_default_flags [PLAN_TAG_MAX_] = {
    #define X(a, b, c, flags, ...) flags,
    X_PLAN
    #undef X
};

char *aggregate_tag_to_str [AGGREGATE_TAG_MAX_] = {
    #define X(_, __, name) #name,
    X_AGGREGATE
    #undef X

    [AGGREGATE_COUNT_ALL] = "count_all"
};

static void print_expr (DString *, Plan *, bool);

// TODO: How can we make this not return void*
// yet keep it is convenient as it is right now?
void *plan_alloc (Plan_Tag tag, u32 flags, Mem *mem) {
    Plan *plan   = MEM_ALLOC_Z(mem, plan_type_size[tag]);
    plan->tag    = tag;
    plan->flags |= plan_default_flags[tag] | flags;

    switch (tag) {
    case PLAN_TABLE_DEF:
        array_init(&((Plan_Table_Def*)plan)->cols, mem);
        break;
    case PLAN_INSERT:
        array_init(&((Plan_Insert*)plan)->values, mem);
        break;
    case PLAN_UPDATE:
        array_init(&((Plan_Update*)plan)->cols, mem);
        array_init(&((Plan_Update*)plan)->vals, mem);
        break;
    case PLAN_PROJECTION:
        array_init(&((Plan_Projection*)plan)->cols, mem);
        break;
    case PLAN_GROUP:
        array_init(&((Plan_Group*)plan)->keys, mem);
        array_init(&((Plan_Group*)plan)->aggregates, mem);
        break;
    case PLAN_ORDER:
        array_init(&((Plan_Order*)plan)->keys, mem);
        array_init(&((Plan_Order*)plan)->directions, mem);
        break;
    default:
        break;
    }

    return plan;
}

Inline void print_tag (DString *ds, Plan *plan) {
    ds_add_cstr(ds, plan_str[plan->tag]);
    ds_add_byte(ds, ' ');
}

static void print_aggregate (Aggregate *agg, DString *ds) {
    ds_add_cstr(ds, aggregate_tag_to_str[agg->tag]);

    ds_add_byte(ds, '(');
    print_expr(ds, (Plan*)agg->ref, false);
    ds_add_byte(ds, ')');

    ds_add_cstr(ds, " as ");
    ds_add_str(ds, agg->name);
}

static void print_expr (DString *ds, Plan *plan, bool in_parens) {
    switch (plan->tag) {
    case PLAN_LITERAL_INT:    ds_add_fmt(ds, "%li", ((Plan_Literal_Int*)plan)->val); break;
    case PLAN_LITERAL_BOOL:   ds_add_fmt(ds, "%s", ((Plan_Literal_Bool*)plan)->val ? "true" : "false"); break;
    case PLAN_LITERAL_NULL:   ds_add_cstr(ds, "null"); break;

    case PLAN_LITERAL_STRING: {
        ds_add_byte(ds, '"');
        ds_add_str(ds, ((Plan_Literal_String*)plan)->val);
        ds_add_byte(ds, '"');
    } break;

    case PLAN_AS: {
        print_expr(ds, ((Plan_Op1*)plan)->op, true);
        ds_add_cstr(ds, " as ");
        ds_add_str(ds, ((Plan_As*)plan)->name);
    } break;

    case PLAN_IS_NULL: {
        print_expr(ds, ((Plan_Op1*)plan)->op, true);
        ds_add_cstr(ds, " is null");
    } break;

    case PLAN_COLUMN_REF: {
        Plan_Column_Ref *P = (Plan_Column_Ref*)plan;

        if (P->agg_expr.data) {
            ds_add_str(ds, P->agg_expr);
        } else if (P->qualifier.data) {
            ds_add_str(ds, P->qualifier);
            ds_add_byte(ds, '.');
            ds_add_str(ds, P->name);
        } else {
            ds_add_str(ds, P->name);
        }
    } break;

    default:
        if (plan_has_bases(plan, PLAN_OP1)) {
            if (in_parens) ds_add_byte(ds, '(');
            print_tag(ds, plan);
            print_expr(ds, ((Plan_Op1*)plan)->op, true);
            if (in_parens) ds_add_byte(ds, ')');
        } else if (plan_has_bases(plan, PLAN_OP2)) {
            if (in_parens) ds_add_byte(ds, '(');
            print_expr(ds, ((Plan_Op2*)plan)->op1, true);
            ds_add_byte(ds, ' ');
            print_tag(ds, plan);
            print_expr(ds, ((Plan_Op2*)plan)->op2, true);
            if (in_parens) ds_add_byte(ds, ')');
        } else {
            unreachable;
        }
    }
}

static void print_expr_list (Array_Plan *plans, DString *ds) {
    array_iter (plan, *plans) {
        print_expr(ds, plan, false);
        if (! ARRAY_ITER_ON_LAST_ELEMENT) ds_add_2byte(ds, ',', ' ');
    }
}

static void print (DString *ds, Plan *plan, u32 depth) {
    switch (plan->tag) {
    case PLAN_TABLE_DEF: {
        print_tag(ds, plan);
        ds_add_str(ds, ((Plan_Table_Def*)plan)->name);
        array_iter (col, ((Plan_Table_Def*)plan)->cols) plan_print_indent(ds, (Plan*)col, depth+1);
    } break;

    case PLAN_COLUMN_DEF: {
        ds_add_str(ds, ((Plan_Column_Def*)plan)->name);
    } break;

    case PLAN_INSERT: {
        print_tag(ds, plan);
        ds_add_str(ds, ((Plan_Insert*)plan)->table);
        ds_add_2byte(ds, ' ', '(');
        print_expr_list(&((Plan_Insert*)plan)->values, ds);
        ds_add_byte(ds, ')');
    } break;

    case PLAN_DELETE: {
        print_tag(ds, plan);
        plan_print_indent(ds, ((Plan_Op1*)plan)->op, depth+1);
    } break;

    case PLAN_PROJECTION: {
        print_tag(ds, plan);
        print_expr_list(&((Plan_Projection*)plan)->cols, ds);
        plan_print_indent(ds, ((Plan_Op1*)plan)->op, depth+1);
    } break;

    case PLAN_LIMIT: {
        print_tag(ds, plan);
        Plan_Limit *P = (Plan_Limit*)plan;
        ds_add_fmt(ds, "%li", P->limit);
        if (P->offset) ds_add_fmt(ds, " %li", P->offset);
        plan_print_indent(ds, ((Plan_Op1*)plan)->op, depth+1);
    } break;

    case PLAN_JOIN_CROSS: {
        print_tag(ds, plan);
        plan_print_indent(ds, ((Plan_Op2*)plan)->op1, depth+1);
        plan_print_indent(ds, ((Plan_Op2*)plan)->op2, depth+1);
    } break;

    case PLAN_JOIN_INNER: {
        print_tag(ds, plan);
        ds_add_cstr(ds, "on ");
        print_expr(ds, ((Plan_Join_Inner*)plan)->on, false);
        plan_print_indent(ds, ((Plan_Op2*)plan)->op1, depth+1);
        plan_print_indent(ds, ((Plan_Op2*)plan)->op2, depth+1);
    } break;

    case PLAN_EXPLAIN: {
        print_tag(ds, plan);
        plan_print_indent(ds, ((Plan_Op1*)plan)->op, depth+1);
    } break;

    case PLAN_SCAN: {
        print_tag(ds, plan);
        ds_add_str(ds, ((Plan_Scan*)plan)->table);
    } break;

    case PLAN_SCAN_DUMMY: {
        print_tag(ds, plan);
    } break;

    case PLAN_ORDER: {
        print_tag(ds, plan);

        Plan_Order *P = (Plan_Order*)plan;

        array_iter (plan, P->keys) {
            print_expr(ds, plan, false);
            bool direction = array_get(&P->directions, ARRAY_IDX);
            if (! direction) ds_add_cstr(ds, " DESC");
            if (! ARRAY_ITER_ON_LAST_ELEMENT) ds_add_2byte(ds, ',', ' ');
        }

        plan_print_indent(ds, ((Plan_Op1*)plan)->op, depth+1);
    } break;

    case PLAN_GROUP: {
        print_tag(ds, plan);

        ds_add_cstr(ds, "[");
        print_expr_list(&((Plan_Group*)plan)->keys, ds);
        ds_add_byte(ds, ']');

        ds_add_cstr(ds, " [");

        array_iter_ptr (agg, ((Plan_Group*)plan)->aggregates) {
            print_aggregate(agg, ds);
            if (! ARRAY_ITER_ON_LAST_ELEMENT) ds_add_2byte(ds, ',', ' ');
        }

        ds_add_byte(ds, ']');

        plan_print_indent(ds, ((Plan_Op1*)plan)->op, depth+1);
    } break;

    case PLAN_FILTER: {
        print_tag(ds, plan);
        print_expr(ds, ((Plan_Filter*)plan)->expr, false);
        plan_print_indent(ds, ((Plan_Op1*)plan)->op, depth+1);
    } break;

    case PLAN_DROP: {
        print_tag(ds, plan);
        ds_add_str(ds, ((Plan_Drop*)plan)->table);
    } break;

    default: {
        print_expr(ds, plan, false);
    } break;
    }
}

void plan_print_indent (DString *ds, Plan *plan, u32 depth) {
    ds_add_byte(ds, '\n');
    ds_add_bytes(ds, ' ', 4*depth);
    print(ds, plan, depth);
}

void plan_print (DString *ds, Plan *plan) {
    print(ds, plan, 0);
}
