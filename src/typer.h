#pragma once

#include "plan.h"
#include "string.h"
#include "common.h"
#include "memory.h"

struct Database;

#define X_TYPE\
    X(TYPE_INT , Type_Int, "int")\
    X(TYPE_BOOL , Type_Bool, "bool")\
    X(TYPE_TEXT, Type_Text, "text")\
    X(TYPE_VOID, Type_Void, "void")\
    X(TYPE_ROW, Type_Row, "row")\
    X(TYPE_COLUMN, Type_Column, "column")\
    X(TYPE_TABLE, Type_Table, "table")

#define X(_, T, ...)\
    typedef struct T T;\
    typedef Array(T*) Array_##T;\
    typedef Array_View(T*) Array_View_##T;
X_TYPE
#undef X

typedef enum {
    #define X(tag, ...) tag,
    X_TYPE
    #undef X

    TYPE_TAG_MAX_
} Type_Tag;

typedef struct Type {
    Type_Tag tag;
} Type;

typedef struct {
    String name; // For example, Foo in Foo.name.
    Array_Type_Column cols;
} Row_Scope;

struct Type_Int    { Type base; };
struct Type_Bool   { Type base; };
struct Type_Text   { Type base; };
struct Type_Void   { Type base; };
struct Type_Row    { Type base; Array(Row_Scope*) scopes; };
struct Type_Column { Type base; bool not_null; String name; Type *field; };

struct Type_Table {
    Type base;
    Type_Row *row;
    u32 prim_key_col;
    void *engine_specific_info;

    // This arena is used to allocate this Type_Table
    // struct as well as all it's children such as
    // Type_Row and Type_Column as well as the
    // engine_specific_info.
    Mem_Arena *mem;
};

typedef struct Typer Typer;

Typer       *typer_new          (struct Database *, Mem *);
void         typer_init_catalog (Typer *, bool db_is_empty);
bool         typer_check        (Typer *, Plan *, String, Mem *, DString *, bool user_is_admin);
bool         typer_add_table    (Typer *, Plan_Table_Def *);
void         typer_del_table    (Typer *, String);
Type_Table  *typer_get_table    (Typer *, String);
Type_Column *typer_get_col_type (Type_Row *, u32 column_idx);
