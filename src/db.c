#include <stdio.h>

#include "files.h"
#include "typer.h"
#include "runner.h"
#include "parser.h"
#include "report.h"
#include "engine.h"
#include "db_private.h"

struct Database {
    Files *fs;
    String file_path;

    Mem_Track *mem;
    Mem_Clib  *mem_clib;
    Mem_Arena *mem_query;

    Typer *typer;
    BEngine *engine;
};

struct Db_Query {
    Database *db;
    Mem_Arena *mem;
    Runner *runner;
    DString report;
    String text;
    Plan *plan;
};

BEngine *db_get_engine  (Database *db) {
    return db->engine;
}

Db_Result db_run_query (Database *db, String text, Mem *mem, DString *report, bool user_is_admin) {
    Array_Plan statements;
    if (! parse_statements(text, mem, &statements, report)) return DB_FAIL;

    array_iter (stmt, statements) {
        if (! typer_check(db->typer, stmt, text, mem, report, user_is_admin)) return DB_FAIL;

        Runner *run = run_new(stmt, text, db->typer, db->engine, mem, report);
        if (stmt->type->tag == TYPE_ROW) run_print_table(run, report);
        else while (run_next(run));
        run_close(run);
    }

    return DB_OK;
}

Db_Result db_query_init  (Db_Query **out_query, Database *db, String text) {
    Mem_Arena *arena = mem_arena_new((Mem*)db->mem, 1*KB);
    Db_Query *query = MEM_ALLOC(arena, sizeof(Db_Query));

    query->db     = db;
    query->mem    = arena;
    query->report = ds_new((Mem*)arena);
    query->text   = text;
    query->plan   = parse_the_statement(text, (Mem*)arena, TOKEN_SELECT, &query->report);

    if (! query->plan) return DB_FAIL;
    if (! typer_check(db->typer, query->plan, text, (Mem*)arena, &query->report, false)) return DB_FAIL;

    query->runner = run_new(query->plan, text, db->typer, db->engine, (Mem*)arena, &query->report);

    *out_query = query;
    return DB_OK;
}

void db_query_close (Db_Query *query) {
    run_close(query->runner);
    mem_arena_destroy(query->mem);
}

Db_Row *db_query_next (Db_Query *query) {
    return run_next(query->runner);
}

Db_Result db_run (Database *db, String text, DString *report) {
    mem_arena_clear(db->mem_query);
    return db_run_query(db, text, (Mem*)db->mem_query, report, false);
}

void db_close (Database *db) {
    Mem_Clib *mem_clib = db->mem_clib;

    fs_destroy(db->fs);
    mem_track_destroy(db->mem);
    mem_clib_destroy(mem_clib);
}

Db_Result db_init (Database **out_db, String db_file_path, Mem *mem) {
    Mem_Clib *mem_clib   = NULL;
    Mem_Track *mem_track = NULL;

    if (mem) {
        mem_track = mem_track_new(mem);
    } else {
        mem_clib  = mem_clib_new();
        mem_track = mem_track_new((Mem*)mem_clib);
    }

    Database *db = MEM_ALLOC(mem_track, sizeof(Database));

    db->mem       = mem_track;
    db->mem_clib  = mem_clib;
    db->mem_query = mem_arena_new((Mem*)db->mem, 1*MB);
    db->fs        = fs_new((Mem*)db->mem);
    db->typer     = typer_new(db, (Mem*)db->mem);
    db->engine    = bengine_new(db->fs, (Mem*)db->mem, db_file_path);

    typer_init_catalog(db->typer, bengine_db_is_empty(db->engine));

    *out_db = db;
    return DB_OK;
}
