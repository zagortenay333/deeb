#pragma once

#include "db.h"
#include "engine.h"

Db_Result db_run_query  (Database *, String, Mem *, DString *, bool user_is_admin);
BEngine  *db_get_engine (Database *);
