#pragma once

#include "typer.h"
#include "common.h"
#include "string.h"
#include "memory.h"

typedef enum {
    DB_OK,
    DB_FAIL,
} Db_Result;

typedef struct {
    bool is_null;

    union {
        s64 integer;
        bool boolean;
        String *string;
    };
} Db_Value;

typedef struct {
    Type_Row *type;
    Array(Db_Value) values;
} Db_Row;

typedef struct Database Database;
typedef struct Db_Query Db_Query;

Db_Result db_init        (Database **, String db_file_path, Mem *);
void      db_close       (Database *);
Db_Result db_run         (Database *, String query, DString *report);
Db_Result db_query_init  (Db_Query **, Database *, String select_statement);
void      db_query_close (Db_Query *);
Db_Row   *db_query_next  (Db_Query *);

