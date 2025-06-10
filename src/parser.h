#pragma once

#include "plan.h"
#include "lexer.h"
#include "common.h"
#include "string.h"

bool  parse_statements    (String, Mem *, Array_Plan *output, DString *);
Plan *parse_the_statement (String, Mem *, Token_Tag, DString *);
