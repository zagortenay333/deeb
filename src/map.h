#pragma once

#include "common.h"
#include "memory.h"

// =============================================================================
// A polymorphic hash table using open addressing and linear probing.
//
// To generate a new map type:
//   1. Use the MAP_DECL macro in this file to make the type decls.
//   2. Use the MAP_IMPL macro in "map.c" to make the functions defs.
//   3. Potentially extend the MAP_DISPATCH macro below.
//
// S   = suffix
// Key = key type
// Val = value type
// =============================================================================
#define MAP_DECL(S, Key, Val)                                                  \
                                                                               \
typedef struct {                                                               \
    u32 hash;                                                                  \
    Key key;                                                                   \
    Val val;                                                                   \
} Map_Slot##S;                                                                 \
                                                                               \
typedef struct {                                                               \
    Mem *mem;                                                                  \
    u32 count;                                                                 \
    u32 capacity;                                                              \
    u32 tomb_count;                                                            \
    Map_Slot##S *slots;                                                        \
} Map##S;                                                                      \
                                                                               \
void map_init_cap##S (Map##S *, Mem *, u32);                                   \
void map_init##S     (Map##S *, Mem *);                                        \
void map_free##S     (Map##S *);                                               \
void map_del##S      (Map##S *, Key);                                          \
void map_add##S      (Map##S *, Key, Val);                                     \
bool map_get##S      (Map##S *, Key, Val *);

// =============================================================================
// Generate common map types:
// =============================================================================
MAP_DECL(_u32_Ptr, u32, void*)

// =============================================================================
// The following are map methods that do not have to use the
// type suffix. Unfortunately, they only work for types that
// can be included in this file which will be just the types
// in the "common.h" header.
// =============================================================================
#define MAP_DISPATCH(OP, map, ...) _Generic((map),                             \
    Map_u32_Ptr *: map_##OP##_u32_Ptr                                          \
)(map, __VA_ARGS__)

#define map_init_cap(map, mem, cap) MAP_DISPATCH(init_cap, map, mem, cap)
#define map_init(map, mem)          MAP_DISPATCH(init, map, mem)
#define map_del(map, key)           MAP_DISPATCH(del, map, key)
#define map_add(map, key, val)      MAP_DISPATCH(add, map, key, val)
#define map_get(map, key, out)      MAP_DISPATCH(get, map, key, out)

#define map_free(MAP) do{\
    DEF(map, MAP);\
    MEM_FREE(map->mem, map->slots, map->capacity * sizeof(*map->slots));\
}while(0)

// =============================================================================
// The following iterators can be used like regular loops:
//
//     map_iter_val  (val, map) fn(val);
//     map_iter_slot (slot, map) { fn(slot->key); }
//
// - Braces can be ommitted.
// - The arguments are evaled only once.
// - Inside the loop the variable MAP is defined.
// - Both "break" and "continue" work as expected.
// - You cannot modify the hash map while looping over it.
// - The iterators skip any unoccupied slots automatically.
// =============================================================================
#define map_iter_val(IT, M)\
    for (bool _(ONCE)=1; _(ONCE);)\
    for (DEF(MAP, M); _(ONCE);)\
    for (u32 _(IDX) = 0; _(ONCE);)\
    for (; _(ONCE); _(ONCE)=0)\
    for (Typeof(MAP.slots->val) IT; (_(IDX) < MAP.capacity) && (IT = MAP.slots[_(IDX)].val, (void)IT, true); _(IDX)++)\
        if (MAP.slots[_(IDX)].hash > 1)

#define map_iter_slot(IT, M)\
    for (bool _(ONCE)=1; _(ONCE);)\
    for (DEF(MAP, M); _(ONCE);)\
    for (u32 _(IDX) = 0; _(ONCE);)\
    for (; _(ONCE); _(ONCE)=0)\
    for (Typeof(*MAP.slots) *IT; (_(IDX) < MAP.capacity) && (IT = &MAP.slots[_(IDX)], (void)IT, true); _(IDX)++)\
        if (IT->hash > 1)
