#pragma once

#include "common.h"
#include "string.h"

typedef struct {
    u32 offset;
    u32 length;
    u32 first_line;
    u32 last_line;
} Source;

void report_source  (DString *, String, Source);
void report_sources (DString *, String, Source, Source);

#define REPORT_NOTE    ANSI_CYAN("NOTE: ")
#define REPORT_ERROR   ANSI_RED("ERROR: ")
#define REPORT_WARNING ANSI_YELLOW("WARNING: ")

// This func is meant to be used within a vararg function.
// The @fmt arg must be the name of the last function arg.
#define report_fmt_va(ds, header, fmt) do{\
    if (! ds) break;\
    ds_add_cstr(ds, header);\
    va_list va;\
    va_start(va, fmt);\
    ds_add_fmt_va(ds, fmt, va);\
    va_end(va);\
    ds_add_byte(ds, '\n');\
}while(0)
