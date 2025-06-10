#include <stdio.h>
#include <setjmp.h>

#include "plan.h"
#include "lexer.h"
#include "report.h"
#include "parser.h"
#include "string.h"

typedef struct {
    Plan_Projection *proj;
    Plan *from;
    Plan *where;
    Plan_Group *group;
    Plan *having;
    Plan *order;
    Plan *limit;

    Array_Plan bottom_proj_cols;
    Plan_Projection *bottom_proj;
    Array_Aggregate aggregates;

    bool order_clause_contains_alias_ref;
    Array_Plan_Column_Ref refs_within_order_but_not_select;
} Select;

typedef struct {
    Mem *mem;
    Lexer *lex;
    String query;
    Array_Plan queries;

    DString *report;
    jmp_buf *error;

    Select *select;
    bool aggregates_allowed;
    bool parsing_order_clause;

    #define L (P->lex)
} Parser;

static Plan *parse_expr (Parser *, u32);
static Plan *parse_statement (Parser *);
static Plan *parse_join_expr (Parser *);

static Noreturn error (Parser *P, char *fmt, ...) {
    report_fmt_va(P->report, REPORT_ERROR, fmt);
    report_source(P->report, P->query, lex_peek_token(L)->src);
    longjmp(*P->error, 1);
}

static Noreturn error_src (Parser *P, Source src, char *fmt, ...) {
    report_fmt_va(P->report, REPORT_ERROR, fmt);
    report_source(P->report, P->query, src);
    longjmp(*P->error, 1);
}

static Noreturn error_src2 (Parser *P, Source src1, Source src2, char *fmt, ...) {
    report_fmt_va(P->report, REPORT_ERROR, fmt);
    report_sources(P->report, P->query, src1, src2);
    longjmp(*P->error, 1);
}

static Noreturn error_plan (Parser *P, Plan *plan, char *fmt, ...) {
    report_fmt_va(P->report, REPORT_ERROR, fmt);
    report_source(P->report, P->query, plan->src);
    longjmp(*P->error, 1);
}

static void *start_node (Parser *P, Plan_Tag tag) {
    Plan *node = plan_alloc(tag, 0, P->mem);
    Source src = lex_peek_token(L)->src;
    node->src.offset = src.offset;
    node->src.first_line = src.first_line;
    return node;
}

static void *start_node_lhs (Parser *P, Plan_Tag tag, Plan *lhs) {
    Plan *node = plan_alloc(tag, 0, P->mem);
    node->src.offset = lhs->src.offset;
    node->src.first_line = lhs->src.first_line;
    return node;
}

static Plan *finish_node (Parser *P, void *node) {
    Plan *N = node;
    N->src.last_line = lex_get_prev_end_line(L);
    N->src.length = lex_get_prev_end_offset(L) - N->src.offset;
    return N;
}

static void eat_semicolons (Parser *P, bool at_leplan_one) {
    if (at_leplan_one) lex_eat_the_token(L, ';');
    while (lex_try_eat_token(L, ';'));
}

#define is_left_associative(token_tag) true

#define prefix(token_tag) (token_tag + TOKEN_TAG_MAX_)

// If it's a prefix operator, do precedence_of(prefix(token_tag)).
static u32 precedence_of (u32 token_tag) {
    switch (token_tag) {
    case prefix('-'):
    case prefix(TOKEN_NOT):
        return 8;

    case '!':
    case TOKEN_IS:
        return 7;

    case '*':
    case '/':
        return 6;

    case '+':
    case '-':
        return 5;

    case '<':
    case '>':
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER_EQUAL:
        return 4;

    case TOKEN_EQUAL:
    case TOKEN_NOT_EQUAL:
        return 3;

    case TOKEN_AND:
        return 2;

    case TOKEN_OR:
        return 1;
    }

    return 0;
}

static Plan *parse_prefix_op (Parser *P, Plan_Tag tag) {
    Plan_Op1 *node = start_node(P, tag);
    Token_Tag op = lex_eat_token(L)->tag;
    node->op = parse_expr(P, precedence_of(prefix(op)));
    return finish_node(P, node);
}

static Plan *parse_postfix_op (Parser *P, Plan_Tag tag, Plan *lhs) {
    Plan_Op1 *node = start_node(P, tag);
    node->op = lhs;
    lex_eat_token(L);
    return finish_node(P, node);
}

static Plan *parse_binary_op (Parser *P, Plan_Tag tag, Plan *lhs) {
    Plan_Op2 *node = start_node_lhs(P, tag, lhs);
    node->op1 = lhs;
    Token_Tag op = lex_eat_token(L)->tag;
    node->op2 = parse_expr(P, precedence_of(op));
    return finish_node(P, node);
}

static Plan *parse_negate (Parser *P) {
    Plan *node = NULL;

    Source start = lex_eat_the_token(L, '-')->src;
    Plan *op = parse_expr(P, precedence_of(prefix('-')));

    if (op->tag == PLAN_LITERAL_INT) {
        node = op;
        ((Plan_Literal_Int*)op)->val = -((Plan_Literal_Int*)op)->val;
    } else {
        node = start_node(P, PLAN_NEG);
        ((Plan_Op1*)node)->op = op;
    }

    node->src = start;
    return finish_node(P, node);
}

static Plan *parse_parens (Parser *P) {
    Source src1 = lex_eat_the_token(L, '(')->src;
    Plan *node  = parse_expr(P, 0);
    Source src2 = lex_eat_the_token(L, ')')->src;

    node->src = (Source){
        .offset     = src1.offset,
        .length     = src2.offset - src1.offset + 1,
        .first_line = src1.first_line,
        .last_line  = src2.last_line,
    };

    return node;
}

static Plan *copy_ref (Parser *P, Plan_Column_Ref *ref) {
    Plan *result = plan_alloc(PLAN_COLUMN_REF, 0, P->mem);
    result->src = ((Plan*)ref)->src;
    ((Plan_Column_Ref*)result)->qualifier = ref->qualifier;
    ((Plan_Column_Ref*)result)->name = ref->name;
    return result;
}

static Plan *ref_from_as (Parser *P, Plan_As *as) {
    Plan *result = plan_alloc(PLAN_COLUMN_REF, 0, P->mem);
    result->src = ((Plan*)as)->src;
    ((Plan_Column_Ref*)result)->name = as->name;
    return result;
}

static Plan *add_bottom_proj_column (Parser *P, Plan *expr) {
    Select *S = P->select;

    if (expr->tag == PLAN_COLUMN_REF) {
        Plan_Column_Ref *ref = (Plan_Column_Ref*)expr;

        // See if we already added this ref.
        array_iter (col, S->bottom_proj_cols) {
            if (col->tag != PLAN_COLUMN_REF) continue;
            if (! str_match(ref->name, ((Plan_Column_Ref*)col)->name)) continue;
            if (ref->qualifier.data && !str_match(ref->qualifier, ((Plan_Column_Ref*)col)->qualifier)) continue;
            return expr;
        }

        array_add(&S->bottom_proj_cols, copy_ref(P, ref));
        return expr;
    } else {
        DString ds = ds_new_cap(P->mem, 6);
        ds_add_fmt(&ds, "#%i", S->bottom_proj_cols.count);

        Plan_As *as = plan_alloc(PLAN_AS, 0, P->mem);
        ((Plan*)as)->src = expr->src;
        ((Plan_Op1*)as)->op = expr;
        as->name = ds_to_str(&ds);
        array_add(&S->bottom_proj_cols, (Plan*)as);

        return ref_from_as(P, as);
    }
}

static Plan *parse_aggregate (Parser *P) {
    if (! P->aggregates_allowed) error(P, "No aggregates allowed here.");

    Select *S     = P->select;
    Source src    = lex_peek_token(L)->src;
    Aggregate agg = {0};

    { // Figure out which aggregate it is:
        String name = lex_eat_the_token(L, TOKEN_IDENT)->txt;

        if (false);
        #define X(upp, mid, low)\
            else if (str_match(name, str(#upp))) agg.tag = AGGREGATE_##upp;\
            else if (str_match(name, str(#mid))) agg.tag = AGGREGATE_##upp;\
            else if (str_match(name, str(#low))) agg.tag = AGGREGATE_##upp;
        X_AGGREGATE
        #undef X
        else error(P, "Unknown aggregate function.");
    }

    Plan *aggregate_operand = NULL;

    { // Parse aggregate operand:
        lex_eat_the_token(L, '(');

        if (agg.tag == AGGREGATE_COUNT && lex_try_eat_token(L, '*')) {
            Plan *dummy = plan_alloc(PLAN_LITERAL_INT, F_PLAN_WITHOUT_SOURCE, P->mem);
            agg.ref = (Plan_Column_Ref*)add_bottom_proj_column(P, dummy);
        } else {
            P->aggregates_allowed = false;
            aggregate_operand = parse_expr(P, 0);
            P->aggregates_allowed = true;
            agg.ref = (Plan_Column_Ref*)add_bottom_proj_column(P, aggregate_operand);
        }

        lex_eat_the_token(L, ')');
    }

    // Replace aggregate expression with an implicit column ref of the form $n.
    Plan_Column_Ref *ref = plan_alloc(PLAN_COLUMN_REF, 0, P->mem);
    ((Plan*)ref)->src = src;

    DString implicit_name = ds_new_cap(P->mem, 6);
    ds_add_fmt(&implicit_name, "$%i", S->aggregates.count);

    agg.name  = ds_to_str(&implicit_name);
    ref->name = ds_to_str(&implicit_name);

    { // Store the original form of the aggregate expression for pretty printing:
        DString ds = ds_new(P->mem);
        ds_add_cstr(&ds, aggregate_tag_to_str[agg.tag]);

        if (aggregate_operand) {
            ds_add_byte(&ds, '(');
            plan_print(&ds, aggregate_operand);
            ds_add_byte(&ds, ')');
        } else {
            ds_add_cstr(&ds, "(*)");
        }

        ref->agg_expr = ds_to_str(&ds);
    }

    array_add(&S->aggregates, agg); // TODO: We should try to deduplicate aggregates.

    return finish_node(P, ref);
}

static Plan *parse_column_ref (Parser *P) {
    Plan_Column_Ref *node = start_node(P, PLAN_COLUMN_REF);

    String name = lex_eat_the_token(L, TOKEN_IDENT)->txt;

    if (lex_try_eat_token(L, '.')) {
        node->qualifier = name;
        node->name = lex_eat_the_token(L, TOKEN_IDENT)->txt;
    } else {
        node->name = name;
    }

    finish_node(P, node);

    if (P->parsing_order_clause) {
        Select *S = P->select;
        bool ref_appears_in_select_clause = false;

        array_iter (col, S->proj->cols) {
            if (col->tag == PLAN_COLUMN_REF) {
                if (str_match(name, ((Plan_Column_Ref*)col)->name)) {
                    ref_appears_in_select_clause = true;
                    break;
                }
            } else if (col->tag == PLAN_AS) {
                if (str_match(name, ((Plan_As*)col)->name)) {
                    if (! P->aggregates_allowed) error_plan(P, (Plan*)node, "Alias reference cannot appear inside aggregate.");
                    S->order_clause_contains_alias_ref = true;
                    ref_appears_in_select_clause = true;
                    break;
                }
            }
        }

        if (! ref_appears_in_select_clause) array_add(&S->refs_within_order_but_not_select, node);
    }

    return (Plan*)node;
}

static Plan *parse_as (Parser *P, Plan *lhs) {
    Plan_As *node = start_node(P, PLAN_AS);
    lex_eat_the_token(L, TOKEN_AS);
    ((Plan_Op1*)node)->op = lhs;
    node->name = lex_eat_the_token(L, TOKEN_IDENT)->txt;
    return finish_node(P, node);
}

static Plan *parse_is_null (Parser *P, Plan *lhs) {
    Plan *node = start_node(P, PLAN_IS_NULL);
    ((Plan_Op1*)node)->op = lhs;

    lex_eat_the_token(L, TOKEN_IS);
    bool negate = lex_try_eat_token(L, TOKEN_NOT);
    lex_eat_the_token(L, TOKEN_NULL);

    finish_node(P, node);

    if (negate) {
        Plan *not = plan_alloc(PLAN_NOT, 0, P->mem);
        not->src = node->src;
        ((Plan_Op1*)not)->op = node;
        return not;
    }

    return node;
}

static Plan *parse_literal_string (Parser *P) {
    Plan_Literal_String *node = start_node(P, PLAN_LITERAL_STRING);
    node->val = lex_eat_the_token(L, TOKEN_LITERAL_STRING)->str;
    return finish_node(P, node);
}

static Plan *parse_literal_bool (Parser *P, Token_Tag tag) {
    Plan_Literal_Bool *node = start_node(P, PLAN_LITERAL_BOOL);
    lex_eat_the_token(L, tag);
    node->val = (tag == TOKEN_TRUE);
    return finish_node(P, node);
}

static Plan *parse_literal_null (Parser *P) {
    Plan_Literal_Null *node = start_node(P, PLAN_LITERAL_NULL);
    lex_eat_the_token(L, TOKEN_NULL);
    return finish_node(P, node);
}

static Plan *parse_literal_int (Parser *P) {
    Plan_Literal_Int *node = start_node(P, PLAN_LITERAL_INT);
    node->val = lex_eat_token(L)->val;
    return finish_node(P, node);
}

static Plan *parse_expr_with_lhs (Parser *P, Plan *lhs) {
    switch (lex_peek_token(L)->tag) {
    case '+':                 return parse_binary_op(P, PLAN_ADD, lhs);
    case '-':                 return parse_binary_op(P, PLAN_SUB, lhs);
    case '*':                 return parse_binary_op(P, PLAN_MUL, lhs);
    case '/':                 return parse_binary_op(P, PLAN_DIV, lhs);
    case '<':                 return parse_binary_op(P, PLAN_LESS, lhs);
    case '>':                 return parse_binary_op(P, PLAN_GREATER, lhs);
    case TOKEN_IS:            return parse_is_null(P, lhs);
    case TOKEN_OR:            return parse_binary_op(P, PLAN_OR, lhs);
    case TOKEN_AND:           return parse_binary_op(P, PLAN_AND, lhs);
    case TOKEN_EQUAL:         return parse_binary_op(P, PLAN_EQUAL, lhs);
    case TOKEN_NOT_EQUAL:     return parse_binary_op(P, PLAN_NOT_EQUAL, lhs);
    case TOKEN_LESS_EQUAL:    return parse_binary_op(P, PLAN_LESS_EQUAL, lhs);
    case TOKEN_GREATER_EQUAL: return parse_binary_op(P, PLAN_GREATER_EQUAL, lhs);
    default:                  unreachable;
    }
}

static Plan *parse_expr_without_lhs (Parser *P) {
    switch (lex_peek_token(L)->tag) {
    case '(':                  return parse_parens(P);
    case '-':                  return parse_negate(P);
    case TOKEN_NOT:            return parse_prefix_op(P, PLAN_NOT);
    case TOKEN_NULL:           return parse_literal_null(P);
    case TOKEN_TRUE:           return parse_literal_bool(P, TOKEN_TRUE);
    case TOKEN_FALSE:          return parse_literal_bool(P, TOKEN_FALSE);
    case TOKEN_LITERAL_INT:    return parse_literal_int(P);
    case TOKEN_LITERAL_STRING: return parse_literal_string(P);
    case TOKEN_IDENT:          return (lex_peek_nth_token(L, 2)->tag == '(') ? parse_aggregate(P) : parse_column_ref(P);
    default:                   return NULL;
    }
}

// The argument "precedence_of_lhs" is the precedence of the
// operator to the left of the expression. For the top level
// call (without an operator to the left) call this function
// with precedence_of_lhs = 0.
static Plan *parse_expr (Parser *P, u32 precedence_of_lhs) {
    Plan *result = parse_expr_without_lhs(P);
    if (! result) error(P, "Expected expression.");

    while (1) {
        Token_Tag op = lex_peek_token(L)->tag;
        u32 p = precedence_of(op);
        if (p < precedence_of_lhs || (p == precedence_of_lhs && is_left_associative(op))) break;
        result = parse_expr_with_lhs(P, result);
    }

    return result;
}

static Plan *parse_def_column (Parser *P) {
    Plan *node = start_node(P, PLAN_COLUMN_DEF);

    ((Plan_Column_Def*)node)->name = lex_eat_the_token(L, TOKEN_IDENT)->txt;

    switch (lex_peek_token(L)->tag) {
    case TOKEN_INT:  node->flags |= F_PLAN_COLUMN_DEF_TYPE_INT; break;
    case TOKEN_BOOL: node->flags |= F_PLAN_COLUMN_DEF_TYPE_BOOL; break;
    case TOKEN_TEXT: node->flags |= F_PLAN_COLUMN_DEF_TYPE_TEXT; break;
    default:         error(P, "Invalid type declaration.");
    }

    lex_eat_token(L);

    Source null_constraint_src;
    Source not_null_constraint_src;
    bool found_null_constraint = false;

    while (1) {
        Token *token = lex_peek_token(L);

        switch (token->tag) {
        case TOKEN_PRIMARY: {
            lex_eat_token(L);
            lex_eat_the_token(L, TOKEN_KEY);
            node->flags |= F_PLAN_COLUMN_DEF_IS_PRIMARY;
            node->flags |= F_PLAN_COLUMN_DEF_NOT_NULL;
        } break;

        case TOKEN_NOT: {
            not_null_constraint_src = lex_eat_token(L)->src;
            lex_eat_the_token(L, TOKEN_NULL);
            node->flags |= F_PLAN_COLUMN_DEF_NOT_NULL;
        } break;

        case TOKEN_NULL: {
            null_constraint_src = lex_eat_token(L)->src;
            found_null_constraint = true;
        } break;

        default: goto brk;
        }
    } brk:

    if (found_null_constraint) {
        if (node->flags & F_PLAN_COLUMN_DEF_IS_PRIMARY) error_src(P, null_constraint_src, "The primary key column cannot have a 'NULL' constraint.");
        if (node->flags & F_PLAN_COLUMN_DEF_NOT_NULL) error_src2(P, null_constraint_src, not_null_constraint_src, "Column cannot have both a 'NULL' and 'NOT NULL' constraint.");
    }

    return finish_node(P, node);
}

static Plan *parse_def_table (Parser *P) {
    Plan_Table_Def *node = start_node(P, PLAN_TABLE_DEF);

    lex_eat_the_token(L, TOKEN_CREATE);
    lex_eat_the_token(L, TOKEN_TABLE);

    Source err_src;

    {
        Token *tok = lex_eat_the_token(L, TOKEN_IDENT);
        node->name = tok->txt;
        err_src    = tok->src;
    }

    s64 prim_key_col = -1;

    lex_eat_the_token(L, '(');

    while (1) {
        Plan_Column_Def *col = (Plan_Column_Def*)parse_def_column(P);

        if (((Plan*)col)->flags & F_PLAN_COLUMN_DEF_IS_PRIMARY) {
            if (prim_key_col != -1) {
                Plan *prev_col = (Plan*)array_get(&node->cols, prim_key_col);
                error_src2(P, prev_col->src, ((Plan*)col)->src, "Table cannot have two primary keys.");
            }

            prim_key_col = node->cols.count;
        }

        array_add(&node->cols, col);
        if (! lex_try_eat_token(L, ',')) break;
    }

    lex_eat_the_token(L, ')');

    if (prim_key_col == -1) error_src(P, err_src, "Table does not have primary key.");
    node->prim_key_col = (u32)prim_key_col;

    return finish_node(P, node);
}

static Plan *parse_insert (Parser *P) {
    Plan_Insert *node = start_node(P, PLAN_INSERT);

    lex_eat_the_token(L, TOKEN_INSERT);
    lex_eat_the_token(L, TOKEN_INTO);

    node->table = lex_eat_the_token(L, TOKEN_IDENT)->txt;

    lex_eat_the_token(L, '(');

    while (1) {
        array_add(&node->values, parse_expr(P, 0));
        if (! lex_try_eat_token(L, ',')) break;
    }

    lex_eat_the_token(L, ')');

    return finish_node(P, node);
}

static Plan *parse_delete (Parser *P) {
    Plan_Delete *node = start_node(P, PLAN_DELETE);

    lex_eat_the_token(L, TOKEN_DELETE);
    lex_eat_the_token(L, TOKEN_FROM);

    node->table = lex_eat_the_token(L, TOKEN_IDENT)->txt;
    if (lex_try_eat_token(L, TOKEN_WHERE)) node->filter = parse_expr(P, 0);

    return finish_node(P, node);
}

static Plan *parse_update (Parser *P) {
    Plan_Update *node = start_node(P, PLAN_UPDATE);

    lex_eat_the_token(L, TOKEN_UPDATE);

    node->table = lex_eat_the_token(L, TOKEN_IDENT)->txt;

    lex_eat_the_token(L, TOKEN_SET);

    while (1) {
        array_add(&node->cols, (Plan_Column_Ref*)parse_column_ref(P));
        lex_eat_the_token(L, '=');
        array_add(&node->vals, parse_expr(P, 0));
        if (! lex_try_eat_token(L, ',')) break;
    }

    lex_eat_the_token(L, TOKEN_WHERE);

    node->filter = parse_expr(P, 0);

    return finish_node(P, node);
}

static void parse_limit_clause (Parser *P) {
    Plan_Limit *node = start_node(P, PLAN_LIMIT);

    lex_eat_the_token(L, TOKEN_LIMIT);

    node->limit = lex_eat_the_token(L, TOKEN_LITERAL_INT)->val;
    if (lex_try_eat_token(L, TOKEN_OFFSET)) node->offset = lex_eat_the_token(L, TOKEN_LITERAL_INT)->val;

    P->select->limit = finish_node(P, node);
}

static void parse_order_clause (Parser *P) {
    Plan_Order *node = start_node(P, PLAN_ORDER);
    lex_eat_the_token(L, TOKEN_ORDER);
    lex_eat_the_token(L, TOKEN_BY);

    P->aggregates_allowed   = true;
    P->parsing_order_clause = true;

    while (1) {
        array_add(&node->keys, parse_expr(P, 0));
        bool ascending = !lex_try_eat_token(L, TOKEN_DESC);
        if (ascending) lex_try_eat_token(L, TOKEN_ASC);
        array_add(&node->directions, ascending);
        if (! lex_try_eat_token(L, ',')) break;
    }

    P->aggregates_allowed   = false;
    P->parsing_order_clause = false;

    P->select->order = finish_node(P, node);
}

static void parse_having_clause (Parser *P) {
    Plan_Filter *node = start_node(P, PLAN_FILTER);
    lex_eat_the_token(L, TOKEN_HAVING);

    P->aggregates_allowed = true;
    node->expr = parse_expr(P, 0);
    P->aggregates_allowed = false;

    P->select->having = finish_node(P, node);
}

static void parse_group_clause (Parser *P) {
    Select *S = P->select;

    Plan_Group *node = start_node(P, PLAN_GROUP);

    lex_eat_the_token(L, TOKEN_GROUP);
    lex_eat_the_token(L, TOKEN_BY);

    while (1) {
        Plan *key = add_bottom_proj_column(P, parse_expr(P, 0));
        array_add(&node->keys, key);
        if (! lex_try_eat_token(L, ',')) break;
    }

    if (lex_try_peek_token(L, TOKEN_HAVING)) parse_having_clause(P);

    S->group = (Plan_Group*)finish_node(P, node);
}

static void parse_where_clause (Parser *P) {
    Plan_Filter *node = start_node(P, PLAN_FILTER);
    lex_eat_the_token(L, TOKEN_WHERE);
    node->expr = parse_expr(P, 0);
    P->select->where = finish_node(P, node);
}

static Plan *parse_join_table_ref (Parser *P) {
    switch (lex_peek_token(L)->tag) {
    case '(': {
        Source src1 = lex_eat_the_token(L, '(')->src;
        Plan *node  = parse_join_expr(P);
        Source src2 = lex_eat_the_token(L, ')')->src;

        node->src = (Source){
            .offset     = src1.offset,
            .length     = src2.offset - src1.offset + 1,
            .first_line = src1.first_line,
            .last_line  = src2.last_line,
        };

        return node;
    }

    case TOKEN_IDENT: {
        Plan *node = start_node(P, PLAN_SCAN);
        ((Plan_Scan*)node)->table = lex_eat_the_token(L, TOKEN_IDENT)->txt;
        if (lex_try_eat_token(L, TOKEN_AS)) ((Plan_Scan*)node)->alias = lex_eat_the_token(L, TOKEN_IDENT)->txt;
        return finish_node(P, node);
    };

    default: error(P, "Expected table reference.");
    }
}

static Plan *parse_join_expr (Parser *P) {
    Plan *result = parse_join_table_ref(P);

    switch (lex_peek_token(L)->tag) {
    case TOKEN_CROSS: {
        Plan_Op2 *node = start_node_lhs(P, PLAN_JOIN_CROSS, result);
        lex_eat_token(L);
        lex_eat_the_token(L, TOKEN_JOIN);
        node->op1 = result;
        node->op2 = parse_join_expr(P);
        return finish_node(P, node);
    }

    case TOKEN_JOIN:
    case TOKEN_INNER: {
        Plan_Op2 *node = start_node_lhs(P, PLAN_JOIN_INNER, result);
        if (lex_eat_token(L)->tag == TOKEN_INNER) lex_eat_the_token(L, TOKEN_JOIN);
        node->op1 = result;
        node->op2 = parse_join_expr(P);
        lex_eat_the_token(L, TOKEN_ON);
        ((Plan_Join_Inner*)node)->on = parse_expr(P, 0);
        return finish_node(P, node);
    }

    default: return result;
    }
}

static Plan *parse_join_expr_list (Parser *P) {
    Plan *result = parse_join_expr(P);

    if (lex_try_eat_token(L, ',')) {
        Plan_Op2 *node = start_node_lhs(P, PLAN_JOIN_CROSS, result);
        node->op1      = result;
        node->op2      = parse_join_expr_list(P);
        return finish_node(P, node);
    }

    return result;
}

static void parse_from_clause (Parser *P) {
    lex_eat_the_token(L, TOKEN_FROM);
    P->select->from = parse_join_expr_list(P);
}

static void parse_select_clause (Parser *P) {
    Plan_Projection *node = start_node(P, PLAN_PROJECTION);

    lex_eat_the_token(L, TOKEN_SELECT);

    if (lex_try_eat_token(L, '*')) {
        ((Plan*)node)->flags |= F_PLAN_SELECT_ALL;
    } else {
        P->aggregates_allowed = true;

        while (1) {
            Plan *expr = parse_expr(P, 0);
            if (lex_try_peek_token(L, TOKEN_AS)) expr = parse_as(P, expr);
            array_add(&node->cols, expr);
            if (! lex_try_eat_token(L, ',')) break;
        }

        P->aggregates_allowed = false;
    }

    P->select->proj = (Plan_Projection*)finish_node(P, node);
}

static void push_select_context (Parser *P, Select *ctx) {
    P->select = ctx;
    array_init(&ctx->aggregates, P->mem);
    array_init(&ctx->bottom_proj_cols, P->mem);
    array_init(&ctx->refs_within_order_but_not_select, P->mem);
}

static void pop_select_context (Parser *P) {
    Select *S = P->select;

    if (! S->group) {
        array_free(&S->aggregates);
        array_free(&S->bottom_proj_cols);
    }

    array_free(&S->refs_within_order_but_not_select);

    P->select = NULL;
}

static Plan *assemble_select (Parser *P) {
    Select *S = P->select;

    if (! S->from) S->from = plan_alloc(PLAN_SCAN_DUMMY, 0, P->mem);
    if (!S->group && S->bottom_proj_cols.count) S->group = plan_alloc(PLAN_GROUP, F_PLAN_WITHOUT_SOURCE, P->mem);

    if (S->group) {
        S->bottom_proj       = plan_alloc(PLAN_PROJECTION, F_PLAN_WITHOUT_SOURCE, P->mem);
        S->bottom_proj->cols = S->bottom_proj_cols;
        S->group->aggregates = S->aggregates;
    }

    if (((Plan*)S->proj)->flags & F_PLAN_SELECT_ALL) {
        // The query is a "SELECT *", but we don't know
        // the column names at this stage. We save the
        // FROM clause on this node so that the Typer
        // can do the replacement later.
        array_add(&S->proj->cols, S->from);
    }

    Plan *root = S->from;

    #define R(N) if (N) { ((Plan_Op1*)N)->op = root; root = (Plan*)N; }

    R(S->where);
    R(S->bottom_proj);
    R(S->group);
    R(S->having);

    if (S->order_clause_contains_alias_ref && S->refs_within_order_but_not_select.count) {
        Plan_Projection *extra_proj = plan_alloc(PLAN_PROJECTION, F_PLAN_WITHOUT_SOURCE, P->mem);

        array_iter (col, S->proj->cols) {
            if (col->tag == PLAN_AS) {
                array_add(&extra_proj->cols, col);
                array_set(&S->proj->cols, ARRAY_IDX, ref_from_as(P, (Plan_As*)col));
            }
        }

        array_iter (order_ref, S->refs_within_order_but_not_select) {
            array_add(&extra_proj->cols, copy_ref(P, order_ref));
        }

        R(extra_proj);
        R(S->order);
        R(S->proj);
    } else if (S->order_clause_contains_alias_ref) {
        R(S->proj);
        R(S->order);
    } else {
        R(S->order);
        R(S->proj);
    }

    R(S->limit);

    #undef R
    pop_select_context(P);
    return root;
}

// The plan graph for the SELECT statement is generated in two
// phases. First, the parser will parse the string producing the
// relevant subtrees and collecting them into a context struct.
// Ater that, the final tree will be assembled.
//
// The GROUP clause and aggregates are handled by first emitting
// a "bottom projection" whose columns are the operands of all
// aggregates and the keys listed in the GROUP clause. The output
// of this bottom projection is then fed into the group operator.
// Here is an example compilation:
//
//     SELECT sum(age), avg(id + 1) FROM foo GROUP BY name;
//
//     projection $0, $1
//         group [name] [sum(num) as $0, avg(#1) as $1]
//             projection num, (id + 1) as #1, name
//                 scan foo
//
// For ordering the ORDER and SELECT clauses there are 3 cases:
//
//     1. The order clause doesn't reference any alias in the
//        select clause. We feed the output of order into select.
//
//        SELECT name FROM Foo ORDER BY age;
//
//        projection name
//            order age
//                scan Foo
//
//     2. The order clause references an alias in select, but
//        all columns it references appear in the select clause.
//        We feed the output of select into order.
//
//        SELECT age, name as B FROM Foo ORDER BY age, B;
//
//        order age, B
//            projection age, name as B
//                scan Foo
//
//     3. The order clause both references an alias and a column
//        that doesn't appear in the select clause. We allocate
//        an extra projection and sandwich the order clause
//        between it and the select clause.
//
//        SELECT name as B FROM Foo ORDER BY age, B;
//
//        projection B
//            order age, B
//                projection name as B, age
//                    scan Foo
static Plan *parse_select (Parser *P) {
    Select select = {0};
    push_select_context(P, &select);

    parse_select_clause(P);
    if (lex_try_peek_token(L, TOKEN_FROM))  parse_from_clause(P);
    if (lex_try_peek_token(L, TOKEN_WHERE)) parse_where_clause(P);
    if (lex_try_peek_token(L, TOKEN_GROUP)) parse_group_clause(P);
    if (lex_try_peek_token(L, TOKEN_ORDER)) parse_order_clause(P);
    if (lex_try_peek_token(L, TOKEN_LIMIT)) parse_limit_clause(P);

    return assemble_select(P);
}

static Plan *parse_drop (Parser *P) {
    Plan_Drop *node = start_node(P, PLAN_DROP);
    lex_eat_the_token(L, TOKEN_DROP);
    lex_eat_the_token(L, TOKEN_TABLE);
    node->table = lex_eat_the_token(L, TOKEN_IDENT)->txt;
    return finish_node(P, node);
}

static Plan *parse_explain (Parser *P) {
    bool run = lex_try_peek_nth_token(L, 2, TOKEN_RUN);

    Plan_Op1 *node = start_node(P, run ? PLAN_EXPLAIN_RUN : PLAN_EXPLAIN);

    lex_eat_the_token(L, TOKEN_EXPLAIN);
    if (run) lex_eat_token(L);
    if (lex_try_peek_token(L, TOKEN_EXPLAIN)) error(P, "Cannot nest explain statements.");

    node->op = parse_statement(P);
    finish_node(P, node);

    if (! node->op) error_plan(P, (Plan*)node, "Expected a statement.");
    return (Plan*)node;
}

static Plan *parse_statement_by_tag (Parser *P, Token_Tag tag) {
    eat_semicolons(P, false);

    switch (tag) {
    case TOKEN_DROP:    return parse_drop(P);
    case TOKEN_INSERT:  return parse_insert(P);
    case TOKEN_DELETE:  return parse_delete(P);
    case TOKEN_UPDATE:  return parse_update(P);
    case TOKEN_SELECT:  return parse_select(P);
    case TOKEN_CREATE:  return parse_def_table(P);
    case TOKEN_EXPLAIN: return parse_explain(P);
    case TOKEN_EOF:     return NULL;
    default:            error(P, "Invalid statement.");
    }
}

static Plan *parse_statement (Parser *P) {
    return parse_statement_by_tag(P, lex_peek_token(L)->tag);
}

static Parser parser_new (String query, Mem *mem, jmp_buf *error, DString *report) {
    return (Parser){
        .query  = query,
        .mem    = mem,
        .error  = error,
        .report = report,
        .lex    = lex_new(mem, query, report, error),
    };
}

bool parse_statements (String query, Mem *mem, Array_Plan *out_ast, DString *report) {
    jmp_buf error;
    if (setjmp(error)) return false;

    Parser P = parser_new(query, mem, &error, report);

    array_init(&P.queries, mem);

    while (1) {
        Plan *node = parse_statement(&P);
        if (! node) break;
        array_add(&P.queries, node);
    }

    *out_ast = P.queries;
    return true;
}

Plan *parse_the_statement (String query, Mem *mem, Token_Tag tag, DString *report) {
    jmp_buf error;
    if (setjmp(error)) return NULL;
    Parser P = parser_new(query, mem, &error, report);
    return parse_statement_by_tag(&P, tag);
}
