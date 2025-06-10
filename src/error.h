#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>

#include "common.h"

#if defined(ASAN_BUILD)
    #include <sanitizer/common_interface_defs.h>
    #define print_stack_trace() __sanitizer_print_stack_trace()
    #define ASSERT(cond) do { if (! (cond)) { printf("ASSERT:\n"); panic(); } }while(0)
#else
    #define print_stack_trace()
    #define ASSERT(cond) assert(cond)
#endif

#define unreachable panic()

// TODO: We need to switch awway from these panic
// functions and build a simple exception system
// using setjmp().
Inline Noreturn panic (void) {
    print_stack_trace();
    exit(EXIT_FAILURE);
}

Inline Noreturn panic_va (char *msg, va_list va) {
    printf(ANSI_RED("ERROR: "));
    vprintf(msg, va);
    printf("\n\n");
    panic();
}

Inline Noreturn panic_fmt (char *msg, ...) {
    va_list va;
    va_start(va, msg);
    panic_va(msg, va);
    vprintf(msg, va);
    va_end(va);
}
