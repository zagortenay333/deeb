#pragma once

#include <stdio.h>
#include "array.h"

// =============================================================================
// String.
// =============================================================================
typedef Array_View_Char String;

Inline String str       (char *str)            { return (String){ .data = str, .count = (u32)strlen(str) }; }
Inline bool   str_match (String s1, String s2) { return (s1.count == s2.count) && (! strncmp(s1.data, s2.data, s1.count)); }
Inline String str_copy  (Mem *mem, String str) { return (String){ .count = str.count, .data = mem_copy(mem, str.data, str.count) }; }
Inline u32    str_hash  (String str)           { /*djb hash*/ u32 hash = 5381; array_iter (ch, str) hash = ((hash << 5) + hash) + ch; return hash; }

// =============================================================================
// Dynamic String.
// =============================================================================
typedef Array_Char DString;

Inline DString ds_new          (Mem *mem)                  { DString ds; array_init(&ds, mem); return ds; }
Inline DString ds_new_cap      (Mem *mem, u32 capacity)    { DString ds; array_init_cap(&ds, mem, capacity); return ds; }
Inline void    ds_free         (DString *ds)               { array_free(ds); }
Inline void    ds_clear        (DString *ds)               { array_clear(ds); }
Inline void    ds_print        (DString *ds)               { if (ds->count) printf("%.*s", ds->count, ds->data); }
Inline void    ds_println      (DString *ds)               { if (ds->count) printf("%.*s\n", ds->count, ds->data); }
Inline void    ds_add_byte     (DString *ds, u8 b)         { array_add(ds, b); }
Inline void    ds_add_2byte    (DString *ds, u8 b1, u8 b2) { array_add(ds, b1); array_add(ds, b2); }
Inline void    ds_add_bytes    (DString *ds, u8 b, u32 n)  { array_add_n_times(ds, b, n); }
Inline char   *ds_to_cstr      (DString *ds)               { ds_add_byte(ds, 0); return ds->data; }
Inline String  ds_to_str       (DString *ds)               { return (String){ .count = ds->count, .data = ds->data }; }
Inline String  ds_to_str_trim  (DString *ds)               { array_trim(ds); return ds_to_str(ds); }
Inline void    ds_add_str      (DString *ds, String str)   { array_add_many(ds, &str); }
Inline void    ds_add_cstr     (DString *ds, char *str)    { ds_add_str(ds, (String){ .data = str, .count = (u32)(strlen(str)) }); }
Inline void    ds_add_cstr_nul (DString *ds, char *str)    { ds_add_str(ds, (String){ .data = str, .count = (u32)(strlen(str) + 1) }); }

Inline void ds_add_fmt_va (DString *ds, char *fmt, va_list va) {
    va_list va2;
    va_copy(va2, va);

    u32 count = (u32)vsnprintf(NULL, 0, fmt, va);
    array_reserve(ds, count + 1);
    vsnprintf(ds->data + ds->count, count + 1, fmt, va2);
    ds->count += count;

    va_end(va2);
}

Inline void ds_add_fmt (DString *ds, char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    ds_add_fmt_va(ds, fmt, va);
    va_end(va);
}
