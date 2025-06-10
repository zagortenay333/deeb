#pragma once

#include <stdint.h>
#include <stdbool.h>

#define Typeof(x) __typeof__(x)
#define Inline    static inline
#define Noreturn  _Noreturn void

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define ANSI_END                "\x1b[0m"
#define ANSI_START_BLACK        "\x1b[30m"
#define ANSI_START_RED          "\x1b[31m"
#define ANSI_START_GREEN        "\x1b[32m"
#define ANSI_START_YELLOW       "\x1b[33m"
#define ANSI_START_BLUE         "\x1b[34m"
#define ANSI_START_MAGENTA      "\x1b[35m"
#define ANSI_START_CYAN         "\x1b[36m"
#define ANSI_START_WHITE        "\x1b[37m"
#define ANSI_START_BOLD         "\x1b[1m"
#define ANSI_START_BOLD_BLACK   "\x1b[1;30m"
#define ANSI_START_BOLD_RED     "\x1b[1;31m"
#define ANSI_START_BOLD_GREEN   "\x1b[1;32m"
#define ANSI_START_BOLD_YELLOW  "\x1b[1;33m"
#define ANSI_START_BOLD_BLUE    "\x1b[1;34m"
#define ANSI_START_BOLD_MAGENTA "\x1b[1;35m"
#define ANSI_START_BOLD_CYAN    "\x1b[1;36m"
#define ANSI_START_BOLD_WHITE   "\x1b[1;37m"

#define ANSI_BLACK(txt)        ANSI_START_BLACK        txt ANSI_END
#define ANSI_RED(txt)          ANSI_START_RED          txt ANSI_END
#define ANSI_GREEN(txt)        ANSI_START_GREEN        txt ANSI_END
#define ANSI_YELLOW(txt)       ANSI_START_YELLOW       txt ANSI_END
#define ANSI_BLUE(txt)         ANSI_START_BLUE         txt ANSI_END
#define ANSI_MAGENTA(txt)      ANSI_START_MAGENTA      txt ANSI_END
#define ANSI_CYAN(txt)         ANSI_START_CYAN         txt ANSI_END
#define ANSI_WHITE(txt)        ANSI_START_WHITE        txt ANSI_END
#define ANSI_BOLD(txt)         ANSI_START_BOLD         txt ANSI_END
#define ANSI_BOLD_BLACK(txt)   ANSI_START_BOLD_BLACK   txt ANSI_END
#define ANSI_BOLD_RED(txt)     ANSI_START_BOLD_RED     txt ANSI_END
#define ANSI_BOLD_GREEN(txt)   ANSI_START_BOLD_GREEN   txt ANSI_END
#define ANSI_BOLD_YELLOW(txt)  ANSI_START_BOLD_YELLOW  txt ANSI_END
#define ANSI_BOLD_BLUE(txt)    ANSI_START_BOLD_BLUE    txt ANSI_END
#define ANSI_BOLD_MAGENTA(txt) ANSI_START_BOLD_MAGENTA txt ANSI_END
#define ANSI_BOLD_CYAN(txt)    ANSI_START_BOLD_CYAN    txt ANSI_END
#define ANSI_BOLD_WHITE(txt)   ANSI_START_BOLD_WHITE   txt ANSI_END

// Use this inside macros to define a variable that
// doesn't shadow any variables outside the macro.
#define _(var) var##_ba5edd06b3035ce8b0eba9a6fcc56389641d1ce1

// Use this macro to define variables. It can be invoked
// with 2 or 3 arguments:
//
//     DEF(T, x, y); // Means Typeof(T) x = y;
//     DEF(x, y);    // Means Typeof(y) x = y;
//
// This macro assigns the init expression to a temporary
// variable that is name mangled before assigning it to
// the final variable. This makes it possible to use this
// macro within another macro to make a local copy of an
// argument without expanding into: type foo = foo.
//
// Note that we must use a different unique variable suffix
// here from the one in the _() macro, or else the following
// would produce a variable redefinition:
//
//     #define bar(x) DEF(int, foo, x);
//     int _(foo) = 0;
//     bar(_(foo))
#define DEF3(T, var, init)\
    Typeof(T) var##_ea6503451764d7482a3a9bf80c423cef4298e457 = (init),\
              var = var##_ea6503451764d7482a3a9bf80c423cef4298e457

#define DEF2(var, init)\
    DEF3(Typeof(init), var, init)

#define DEF_DISPATCH(_1, _2, _3, D, ...) D
#define DEF(...) DEF_DISPATCH(__VA_ARGS__, DEF3, DEF2, 0)(__VA_ARGS__)

// Amount that must be added to @addr to make it a
// multiple of @align. If @address is already aligned,
// then this evals to 0. @align must be a power of two.
#define PADDING_TO_ALIGN(addr, align) (((align) - ((addr) & ((align) - 1u))) & ((align) - 1u))

#define MAX_ALIGNMENT _Alignof(max_align_t)

#define FLAG(n) (1u << n)

#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define swap(a, b) do {\
    Typeof(a) tmp = (a);\
    (a)           = (b);\
    (b)           = tmp;\
}while(0)

Inline u8 digit_count (u64 n) {
    u8 result = 1;
    while (n >= 10) { result++; n /= 10; }
    return result;
}

// Helpers for (de)serialization. (be=big endian; le=little endian)
Inline u16 read_u16_le (u8 *b) { return ((u16)b[1] << 8) | b[0]; }
Inline u32 read_u32_le (u8 *b) { return ((u32)b[3] << 24) | ((u32)b[2] << 16) | ((u32)b[1] << 8) | b[0]; }
Inline u64 read_u64_le (u8 *b) { return ((u64)b[7] << 56) | ((u64)b[6] << 48) | ((u64)b[5] << 40) | ((u64)b[4] << 32) | ((u64)b[3] << 24) | ((u64)b[2] << 16) | ((u64)b[1] << 8) | (u64)b[0]; }
Inline s16 read_s16_le (u8 *b) { return (s16)read_u16_le(b); }
Inline s32 read_s32_le (u8 *b) { return (s32)read_u32_le(b); }
Inline s64 read_s64_le (u8 *b) { return (s64)read_u64_le(b); }

Inline u16 read_u16_be (u8 *b) { return ((u16)b[0] << 8) | b[1]; }
Inline u32 read_u32_be (u8 *b) { return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3]; }
Inline u64 read_u64_be (u8 *b) { return ((u64)b[0] << 56) | ((u64)b[1] << 48) | ((u64)b[2] << 40) | ((u64)b[3] << 32) | ((u64)b[4] << 24) | ((u64)b[5] << 16) | ((u64)b[6] << 8) | ((u64)b[7] << 0); }
Inline s16 read_s16_be (u8 *b) { return (s16)read_u16_be(b); }
Inline s32 read_s32_be (u8 *b) { return (s32)read_u32_be(b); }
Inline s64 read_s64_be (u8 *b) { return (s64)read_u64_be(b); }

Inline void write_u16_le (u8 *b, u16 n) { b[1] = (u8)(n >> 8);  b[0] = (u8)(n >> 0); }
Inline void write_u32_le (u8 *b, u32 n) { b[3] = (u8)(n >> 24); b[2] = (u8)(n >> 16); b[1] = (u8)(n >> 8);  b[0] = (u8)(n >> 0); }
Inline void write_u64_le (u8 *b, u64 n) { b[7] = (u8)(n >> 56); b[6] = (u8)(n >> 48); b[5] = (u8)(n >> 40); b[4] = (u8)(n >> 32); b[3] = (u8)(n >> 24); b[2] = (u8)(n >> 16); b[1] = (u8)(n >> 8); b[0] = (u8)(n >> 0); }
Inline void write_s16_le (u8 *b, s16 n) { write_u16_le(b, (u16)n); }
Inline void write_s32_le (u8 *b, s32 n) { write_u32_le(b, (u32)n); }
Inline void write_s64_le (u8 *b, s64 n) { write_u64_le(b, (u64)n); }

Inline void write_u16_be (u8 *b, u16 n) { b[0] = (u8)(n >> 8);  b[1] = (u8)(n >> 0); }
Inline void write_u32_be (u8 *b, u32 n) { b[0] = (u8)(n >> 24); b[1] = (u8)(n >> 16); b[2] = (u8)(n >> 8);  b[3] = (u8)(n >> 0); }
Inline void write_u64_be (u8 *b, u64 n) { b[0] = (u8)(n >> 56); b[1] = (u8)(n >> 48); b[2] = (u8)(n >> 40); b[3] = (u8)(n >> 32); b[4] = (u8)(n >> 24); b[5] = (u8)(n >> 16); b[6] = (u8)(n >> 8); b[7] = (u8)(n >> 0); }
Inline void write_s16_be (u8 *b, s16 n) { write_u16_be(b, (u16)n); }
Inline void write_s32_be (u8 *b, s32 n) { write_u32_be(b, (u32)n); }
Inline void write_s64_be (u8 *b, s64 n) { write_u64_be(b, (u64)n); }
