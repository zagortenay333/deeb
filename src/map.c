#include <stdlib.h>

#include "map.h"
#include "report.h"
#include "string.h"

#define LOAD_MAX 75
#define LOAD_MIN 25
#define MIN_CAPACITY 8

static_assert(LOAD_MAX < 100, ""); // To ensure 1 empty slot.
static_assert(LOAD_MIN < LOAD_MAX, "");
static_assert(MIN_CAPACITY > 0, "");
static_assert(MIN_CAPACITY % 2 == 0, "");

#define is_tomb(slot)     ((slot)->hash == 1)
#define is_empty(slot)    ((slot)->hash == 0)
#define is_occupied(slot) ((slot)->hash > 1)
#define mark_tomb(slot)   ((slot)->hash = 1)
#define sizeof_slot(map)  (sizeof(*(map)->slots))

// =============================================================================
// S    = suffix
// Key  = key type
// Val  = value type
// CMP  = code to embed into cmp##S.
// HASH = code to embed into hash##S.
// =============================================================================
#define MAP_IMPL(S, Key, Val, CMP, HASH)                                       \
                                                                               \
static bool cmp##S   (Key K1, Key K2) { CMP; }                                 \
static u32  _hash##S (Key K) { HASH; }                                         \
Inline u32  hash##S  (Key K) { u32 H = _hash##S(K); return H > 1 ? H : 2; }    \
                                                                               \
/* If not found, the returned slot will be empty or a tomb. */                 \
static Map_Slot##S *find##S (Map##S *map, Key key, u32 hash) {                 \
    Map_Slot##S *tomb = NULL;                                                  \
    u32 idx = hash % map->capacity;                                            \
                                                                               \
    while (1) {                                                                \
        Map_Slot##S *slot = &map->slots[idx];                                  \
        if (is_empty(slot)) return tomb ? tomb : slot;                         \
        if (hash == slot->hash && cmp##S(key, slot->key)) return slot;         \
        if (is_tomb(slot) && !tomb) tomb = slot;                               \
        idx = (idx + 1) % map->capacity;                                       \
    }                                                                          \
}                                                                              \
                                                                               \
/* Optimized version that only looks for an empty slot. */                     \
static Map_Slot##S *find_empty##S (Map##S *map, u32 hash) {                    \
    u32 idx = hash % map->capacity;                                            \
                                                                               \
    while (1) {                                                                \
        Map_Slot##S *slot = &map->slots[idx];                                  \
        if (is_empty(slot)) return slot;                                       \
        idx = (idx + 1) % map->capacity;                                       \
    }                                                                          \
}                                                                              \
                                                                               \
static void rehash##S (Map##S *map, u32 new_capacity) {                        \
    u32 old_capacity = map->capacity;                                          \
    Map_Slot##S *old_slots = map->slots;                                       \
                                                                               \
    map->slots = MEM_ALLOC_Z(map->mem, new_capacity * sizeof_slot(map));       \
    map->capacity = new_capacity;                                              \
    map->tomb_count = 0;                                                       \
                                                                               \
    for (u32 i = 0; i < old_capacity; ++i) {                                   \
        Map_Slot##S *slot = &old_slots[i];                                     \
        if (is_occupied(slot)) *find_empty##S(map, slot->hash) = *slot;        \
    }                                                                          \
                                                                               \
    MEM_FREE(map->mem, old_slots, old_capacity * sizeof_slot(map));            \
}                                                                              \
                                                                               \
static void rehash_if_too_full##S (Map##S *map) {                              \
    u32 max_load = (u32)((u64)map->capacity * LOAD_MAX / 100);                 \
                                                                               \
    if (map->count + map->tomb_count > max_load) {                             \
        u32 cap = (map->count > max_load) ? 2*map->capacity : map->capacity;   \
        rehash##S(map, cap);                                                   \
    }                                                                          \
}                                                                              \
                                                                               \
static void rehash_if_too_empty##S (Map##S *map) {                             \
    if (map->capacity == MIN_CAPACITY) return;                                 \
    u32 min_load = (u32)((u64)map->capacity * LOAD_MIN / 100);                 \
    if (map->count < min_load) rehash##S(map, map->capacity / 2);              \
}                                                                              \
                                                                               \
bool map_get##S (Map##S *map, Key key, Val *out) {                             \
    Map_Slot##S *slot = find##S(map, key, hash##S(key));                       \
    if (! is_occupied(slot)) return false;                                     \
    *out = slot->val;                                                          \
    return true;                                                               \
}                                                                              \
                                                                               \
void map_add##S (Map##S *map, Key key, Val val) {                              \
    u32 hash = hash##S(key);                                                   \
    Map_Slot##S *slot = find##S(map, key, hash);                               \
    slot->val = val;                                                           \
                                                                               \
    if (is_tomb(slot)) {                                                       \
        slot->key = key;                                                       \
        slot->hash = hash;                                                     \
        map->count++;                                                          \
        map->tomb_count--;                                                     \
    } else if (is_empty(slot)) {                                               \
        slot->key = key;                                                       \
        slot->hash = hash;                                                     \
        map->count++;                                                          \
        rehash_if_too_full##S(map);                                            \
    }                                                                          \
}                                                                              \
                                                                               \
void map_del##S (Map##S *map, Key key) {                                       \
    Map_Slot##S *slot = find##S(map, key, hash##S(key));                       \
    if (! is_occupied(slot)) return;                                           \
    mark_tomb(slot);                                                           \
    map->count--;                                                              \
    map->tomb_count++;                                                         \
    rehash_if_too_empty##S(map);                                               \
}                                                                              \
                                                                               \
void map_init_cap##S (Map##S *map, Mem *mem, u32 cap) {                        \
    map->mem        = mem;                                                     \
    map->count      = 0;                                                       \
    map->capacity   = cap;                                                     \
    map->tomb_count = 0;                                                       \
    map->slots      = MEM_ALLOC_Z(mem, cap * sizeof_slot(map));                \
}                                                                              \
                                                                               \
void map_init##S (Map##S *map, Mem *mem) {                                     \
    map_init_cap##S(map, mem, MIN_CAPACITY);                                   \
}

// =============================================================================
// Generate common implementations:
// =============================================================================
MAP_IMPL(_u32_Ptr, u32, void*, return(K1 == K2), return(K))
