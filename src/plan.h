#pragma once

#include "array.h"
#include "report.h"
#include "common.h"
#include "memory.h"

struct Type;

#define F_PLAN_WITHOUT_SOURCE          FLAG(0) // Plan has no source code
#define F_PLAN_SELECT_ALL              FLAG(1) // Only on Plan_Projection
#define F_PLAN_COLUMN_DEF_NOT_NULL     FLAG(2) // Only on Plan_Column_Def
#define F_PLAN_COLUMN_DEF_TYPE_INT     FLAG(3) // Only on Plan_Column_Def
#define F_PLAN_COLUMN_DEF_TYPE_BOOL    FLAG(4) // Only on Plan_Column_Def
#define F_PLAN_COLUMN_DEF_TYPE_TEXT    FLAG(5) // Only on Plan_Column_Def
#define F_PLAN_COLUMN_DEF_IS_PRIMARY   FLAG(6) // Only on Plan_Column_Def
#define F_PLAN_COLUMN_REF_OF_AGGREGATE FLAG(7) // Only on Plan_Column_Ref

#define PLAN_COLUMN_DEF_TYPE (F_PLAN_COLUMN_DEF_TYPE_INT | F_PLAN_COLUMN_DEF_TYPE_BOOL | F_PLAN_COLUMN_DEF_TYPE_TEXT)

// X(tag_val, tag, type)
//
// These refer to structs that do not represent real
// plan nodes but base structs that are common among
// them. They have their own tags which are flags so
// that one can check whether a Plan node has a base.
#define X_PLAN_BASE\
    X(0x1, PLAN, Plan)\
    X(0x4, PLAN_OP1, Plan_Op1)\
    X(0x8, PLAN_OP2, Plan_Op2)

// X(tag, type, str, flags, bases)
//
// These are the real plan nodes. You don't have to
// declare PLAN as a base since all nodes have this
// base in common.
#define X_PLAN\
    X(PLAN_TABLE_DEF, Plan_Table_Def, "table definition", 0, 0)\
    X(PLAN_COLUMN_DEF, Plan_Column_Def, "column definition", 0, 0)\
    X(PLAN_COLUMN_REF, Plan_Column_Ref, "field", 0, 0)\
    X(PLAN_INSERT, Plan_Insert, "insert", 0, 0)\
    X(PLAN_DELETE, Plan_Delete, "delete", 0, 0)\
    X(PLAN_UPDATE, Plan_Update, "update", 0, 0)\
    X(PLAN_DROP, Plan_Drop, "drop", 0, 0)\
    X(PLAN_SCAN, Plan_Scan, "scan", 0, 0)\
    X(PLAN_SCAN_DUMMY, Plan_Scan_Dummy, "scan dummy table", F_PLAN_WITHOUT_SOURCE, 0)\
    X(PLAN_AS, Plan_As, "as", 0, PLAN_OP1)\
    X(PLAN_PROJECTION, Plan_Projection, "projection", 0, PLAN_OP1)\
    X(PLAN_JOIN_CROSS, Plan_Join_Cross, "cross join", 0, PLAN_OP2)\
    X(PLAN_JOIN_INNER, Plan_Join_Inner, "inner join", 0, PLAN_OP2)\
    X(PLAN_FILTER, Plan_Filter, "filter", 0, PLAN_OP1)\
    X(PLAN_GROUP, Plan_Group, "group", 0, PLAN_OP1)\
    X(PLAN_ORDER, Plan_Order, "order", 0, PLAN_OP1)\
    X(PLAN_LIMIT, Plan_Limit, "limit", 0, 0)\
    X(PLAN_EXPLAIN, Plan_Explain, "explain", 0, 0)\
    X(PLAN_EXPLAIN_RUN, Plan_Explain_Run, "explain and run", 0, 0)\
    X(PLAN_NEG, Plan_Neg, "-", 0, PLAN_OP1)\
    X(PLAN_NOT, Plan_Not, "not", 0, PLAN_OP1)\
    X(PLAN_IS_NULL, Plan_Is_Null, "is null", 0, PLAN_OP1)\
    X(PLAN_OR, Plan_Or, "or", 0, PLAN_OP2)\
    X(PLAN_AND, Plan_And, "and", 0, PLAN_OP2)\
    X(PLAN_ADD, Plan_Add, "+", 0, PLAN_OP2)\
    X(PLAN_SUB, Plan_Sub, "-", 0, PLAN_OP2)\
    X(PLAN_MUL, Plan_Mul, "*", 0, PLAN_OP2)\
    X(PLAN_DIV, Plan_Div, "/", 0, PLAN_OP2)\
    X(PLAN_EQUAL, Plan_Equal, "=", 0, PLAN_OP2)\
    X(PLAN_NOT_EQUAL, Plan_Not_Equal, "!=", 0, PLAN_OP2)\
    X(PLAN_LESS, Plan_Less, "<", 0, PLAN_OP2)\
    X(PLAN_GREATER, Plan_Greater, ">", 0, PLAN_OP2)\
    X(PLAN_LESS_EQUAL, Plan_Less_Equal, "<=", 0, PLAN_OP2)\
    X(PLAN_GREATER_EQUAL, Plan_Greater_Equal, ">=", 0, PLAN_OP2)\
    X(PLAN_LITERAL_INT, Plan_Literal_Int, "literal int", 0, 0)\
    X(PLAN_LITERAL_NULL, Plan_Literal_Null, "literal null", 0, 0)\
    X(PLAN_LITERAL_BOOL, Plan_Literal_Bool, "literal bool", 0, 0)\
    X(PLAN_LITERAL_STRING, Plan_Literal_String, "literal string", 0, 0)

#define X(_, __, T)\
    typedef struct T T;\
    typedef Array(T*) Array_##T;\
    typedef Array_View(T*) Array_View_##T;
X_PLAN_BASE
#undef X

#define X(_, T, ...)\
    typedef struct T T;\
    typedef Array(T*) Array_##T;\
    typedef Array_View(T*) Array_View_##T;
X_PLAN
#undef X

typedef enum {
    #define X(flag, name, ...) name = flag,
    X_PLAN_BASE
    #undef X
} Plan_Base_Tag;

typedef enum {
    #define X(tag, ...) tag,
    X_PLAN
    #undef X

    PLAN_TAG_MAX_
} Plan_Tag;

// X(uppercase, camelcase, lowercase)
#define X_AGGREGATE\
    X(AVG, Avg, avg)\
    X(MAX, Max, max)\
    X(MIN, Min, min)\
    X(SUM, Sum, sum)\
    X(COUNT, Count, count)

typedef enum {
    #define X(upp, ...) AGGREGATE_##upp,
    X_AGGREGATE
    #undef X

    AGGREGATE_COUNT_ALL,
    AGGREGATE_TAG_MAX_
} Aggregate_Tag;

typedef struct {
    String name;
    Aggregate_Tag tag;
    Plan_Column_Ref *ref;
} Aggregate;

typedef Array(Aggregate) Array_Aggregate;

struct Plan                { Plan_Tag tag; u32 flags; Source src; struct Type *type; };
struct Plan_Op1            { Plan base; Plan *op; };
struct Plan_Op2            { Plan base; Plan *op1, *op2; };
struct Plan_Table_Def      { Plan base; String name; Array_Plan_Column_Def cols; u32 prim_key_col; char *text_base; };
struct Plan_Column_Def     { Plan base; String name; };
struct Plan_Column_Ref     { Plan base; String qualifier, name; u32 idx; String agg_expr; };
struct Plan_Insert         { Plan base; String table; Array_Plan values; };
struct Plan_Drop           { Plan base; String table; };
struct Plan_Scan           { Plan base; String table, alias; u32 cur; bool done; };
struct Plan_Scan_Dummy     { Plan base; bool done; };
struct Plan_Delete         { Plan base; String table; Plan *filter; };
struct Plan_Update         { Plan base; String table; Plan *filter; Array_Plan_Column_Ref cols; Array_Plan vals; };
struct Plan_As             { Plan_Op1 base; String name; };
struct Plan_Projection     { Plan_Op1 base; Array_Plan cols; };
struct Plan_Filter         { Plan_Op1 base; Plan *expr; };
struct Plan_Group          { Plan_Op1 base; Array_Plan keys; Array_Aggregate aggregates; void *sorter; };
struct Plan_Order          { Plan_Op1 base; Array_Bool /*1=ASC*/ directions; Array_Plan keys; void *sorter; };
struct Plan_Limit          { Plan_Op1 base; s64 limit, offset, emitted; u8 state; };
struct Plan_Explain        { Plan_Op1 base; };
struct Plan_Explain_Run    { Plan_Op1 base; };
struct Plan_Neg            { Plan_Op1 base; };
struct Plan_Not            { Plan_Op1 base; };
struct Plan_Is_Null        { Plan_Op1 base; };
struct Plan_Join_Inner     { Plan_Op2 base; Plan *on; void *cur; };
struct Plan_Join_Cross     { Plan_Op2 base; void *cur; };
struct Plan_Add            { Plan_Op2 base; };
struct Plan_Sub            { Plan_Op2 base; };
struct Plan_Mul            { Plan_Op2 base; };
struct Plan_Div            { Plan_Op2 base; };
struct Plan_Or             { Plan_Op2 base; };
struct Plan_And            { Plan_Op2 base; };
struct Plan_Equal          { Plan_Op2 base; };
struct Plan_Not_Equal      { Plan_Op2 base; };
struct Plan_Less           { Plan_Op2 base; };
struct Plan_Less_Equal     { Plan_Op2 base; };
struct Plan_Greater        { Plan_Op2 base; };
struct Plan_Greater_Equal  { Plan_Op2 base; };
struct Plan_Literal_Int    { Plan base; s64 val; };
struct Plan_Literal_Null   { Plan base; };
struct Plan_Literal_Bool   { Plan base; bool val; };
struct Plan_Literal_String { Plan base; String val; };

extern char  *plan_str             [PLAN_TAG_MAX_];
extern u64    plan_bases           [PLAN_TAG_MAX_];
extern size_t plan_type_size       [PLAN_TAG_MAX_];
extern u32    plan_default_flags   [PLAN_TAG_MAX_];
extern char  *aggregate_tag_to_str [AGGREGATE_TAG_MAX_];

Inline bool plan_has_bases (Plan *P, u64 B) { return plan_bases[P->tag] & B; }

void *plan_alloc        (Plan_Tag, u32 flags, Mem *);
void  plan_print        (DString *, Plan *);
void  plan_print_indent (DString *, Plan *, u32 indentation);
