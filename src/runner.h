#pragma once

#include "db.h"
#include "plan.h"
#include "typer.h"
#include "memory.h"
#include "common.h"
#include "engine.h"

typedef struct Runner Runner;

Runner *run_new         (Plan *, String, Typer *, BEngine *, Mem *, DString *);
void    run_close       (Runner *);
Db_Row *run_next        (Runner *);
void    run_print_table (Runner *, DString *);
