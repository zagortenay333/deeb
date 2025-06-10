#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "array.h"
#include "report.h"
#include "memory.h"

static Noreturn error (Mem *mem) {
    panic_fmt("OOM");
}

// =============================================================================
// Arena allocator.
// =============================================================================
#define ARENA_BLOCK_PREFIX_SIZE\
    (sizeof(Mem_Arena_Block) + PADDING_TO_ALIGN(sizeof(Mem_Arena_Block), MAX_ALIGNMENT))

typedef struct Mem_Arena_Block Mem_Arena_Block;

struct Mem_Arena_Block {
    size_t capacity;
    Mem_Arena_Block *prev;
};

struct Mem_Arena {
    Mem mem;
    Mem *mem_root;
    size_t min_block_capacity;
    size_t current_block_count;
    Mem_Arena_Block *current_block;
};

static size_t arena_remaining_space (Mem_Arena *arena) {
    return arena->current_block->capacity - arena->current_block_count;
}

static void arena_alloc_block (Mem_Arena *arena, size_t size) {
    if (size < arena->min_block_capacity) size = arena->min_block_capacity;

    size_t capacity = ARENA_BLOCK_PREFIX_SIZE + size; // TODO: Overflow check.

    Mem_Arena_Block *new_block = MEM_ALLOC(arena->mem_root, capacity);
    new_block->capacity        = capacity;
    new_block->prev            = arena->current_block;

    arena->current_block       = new_block;
    arena->current_block_count = ARENA_BLOCK_PREFIX_SIZE;
}

void *mem_arena_alloc (Mem *mem, size_t size) {
    if (size == 0) error(mem);

    Mem_Arena *arena = (Mem_Arena*)mem;
    size_t padding   = PADDING_TO_ALIGN(arena->current_block_count, MAX_ALIGNMENT);

    if (arena_remaining_space(arena) < (size + padding)) {
        arena_alloc_block(arena, size + padding);
        padding = 0;
    }

    void *result = (char*)arena->current_block + arena->current_block_count + padding;
    arena->current_block_count += (size + padding);

    return result;
}

void *mem_arena_alloc_z (Mem *mem, size_t size) {
    void *result = mem_arena_alloc(mem, size);
    memset(result, 0, size);
    return result;
}

void *mem_arena_grow (Mem *mem, void *ptr, size_t old_size, size_t new_size) {
    if (! ptr) return mem_arena_alloc(mem, new_size);
    if (new_size < old_size) error(mem);

    void *result = mem_arena_alloc(mem, new_size);
    memcpy(result, ptr, old_size);
    return result;
}

void *mem_arena_grow_z (Mem *mem, void *ptr, size_t old_size, size_t new_size) {
    char *result = mem_arena_grow(mem, ptr, new_size, old_size);
    memset(result + old_size, 0, new_size - old_size);
    return result;
}

void *mem_arena_shrink (Mem *mem, void *ptr, size_t old_size, size_t new_size) {
    if (new_size > old_size) error(mem);
    (void)mem;
    return ptr;
}

void mem_arena_free (Mem *mem, void *ptr, size_t size) {
    (void)mem;
    (void)ptr;
    (void)size;
}

void mem_arena_clear (Mem_Arena *arena) {
    Mem_Arena_Block *block = arena->current_block->prev;

    while (block) {
        Mem_Arena_Block *prev = block->prev;
        MEM_FREE(arena->mem_root, block, block->capacity);
        block = prev;
    }

    arena->current_block->prev = NULL;
    arena->current_block_count = ARENA_BLOCK_PREFIX_SIZE;
}

void mem_arena_destroy (Mem_Arena *arena) {
    Mem *mem_root = arena->mem_root;
    mem_arena_clear(arena);
    MEM_FREE(mem_root, arena->current_block, arena->current_block->capacity);
    MEM_FREE(mem_root, arena, sizeof(Mem_Arena));
}

Mem_Arena *mem_arena_new (Mem *mem, size_t min_block_capacity) {
    Mem_Arena *result = MEM_ALLOC_Z(mem, sizeof(Mem_Arena));

    result->mem = (Mem){
        .free    = mem_arena_free,
        .alloc   = mem_arena_alloc,
        .alloc_z = mem_arena_alloc_z,
        .grow    = mem_arena_grow,
        .grow_z  = mem_arena_grow_z,
        .shrink  = mem_arena_shrink,
    };

    result->mem_root = mem;
    result->min_block_capacity = min_block_capacity;

    arena_alloc_block(result, min_block_capacity);

    return result;
}

// =============================================================================
// System allocator.
// =============================================================================
struct Mem_Clib {
    Mem mem;
};

void mem_clib_free (Mem *mem, void *ptr, size_t size) {
    (void)mem;
    (void)size;
    free(ptr);
}

void *mem_clib_alloc (Mem *mem, size_t size) {
    if (size == 0) error(mem);
    void *result = malloc(size);
    if (! result) error(mem);
    return result;
}

void *mem_clib_alloc_z (Mem *mem, size_t size) {
    if (size == 0) error(mem);
    void *result = calloc(1, size);
    if (! result) error(mem);
    return result;
}

static void *clib_realloc (Mem *mem, void *ptr, size_t size) {
    if (size == 0) error(mem);
    void *result = realloc(ptr, size);
    if (! result) error(mem);
    return result;
}

void *mem_clib_grow (Mem *mem, void *ptr, size_t old_size, size_t new_size) {
    if (! ptr) return mem_clib_alloc(mem, new_size);
    if (old_size > new_size) error(mem);
    return clib_realloc(mem, ptr, new_size);
}

void *mem_clib_grow_z (Mem *mem, void *ptr, size_t old_size, size_t new_size) {
    char *result = mem_clib_grow(mem, ptr, old_size, new_size);
    memset(result + old_size, 0, new_size - old_size);
    return result;
}

void *mem_clib_shrink (Mem *mem, void *ptr, size_t old_size, size_t new_size) {
    if (old_size < new_size) error(mem);
    return clib_realloc(mem, ptr, new_size);
}

void mem_clib_destroy (Mem_Clib *mem) {
    free(mem);
}

Mem_Clib *mem_clib_new (void) {
    Mem_Clib *result = calloc(1, sizeof(Mem_Clib));

    result->mem = (Mem){
        .free    = mem_clib_free,
        .alloc   = mem_clib_alloc,
        .alloc_z = mem_clib_alloc_z,
        .grow    = mem_clib_grow,
        .grow_z  = mem_clib_grow_z,
        .shrink  = mem_clib_shrink,
    };

    return result;
}

// =============================================================================
// Track allocator.
// =============================================================================
typedef struct {
    void *ptr;
    size_t size;
} Mem_Track_Info;

struct Mem_Track {
    Mem mem;
    Mem *mem_root;
    Array(Mem_Track_Info) infos;
};

static void mem_track_update_info (Mem_Track *mem, void *old_ptr, void *new_ptr, size_t new_size) {
    array_iter_ptr (info, mem->infos) {
        if (info->ptr == old_ptr) {
            info->ptr  = new_ptr;
            info->size = new_size;
            return;
        }
    }

    unreachable; // Asserts that the loop above found the info.
}

void mem_track_free (Mem *mem, void *ptr, size_t size) {
    if (! ptr) return;
    Mem_Track *M = (Mem_Track*)mem;
    MEM_FREE(M->mem_root, ptr, size);
    array_iter_ptr (info, M->infos) if (info->ptr == ptr) { array_remove_fast(&M->infos, ARRAY_IDX); break; }
}

void *mem_track_alloc (Mem *mem, size_t size) {
    Mem_Track *M = (Mem_Track*)mem;
    void *result = MEM_ALLOC(M->mem_root, size);
    array_add(&M->infos, ((Mem_Track_Info){ result, size }));
    return result;
}

void *mem_track_alloc_z (Mem *mem, size_t size) {
    Mem_Track *M = (Mem_Track*)mem;
    void *result = MEM_ALLOC_Z(M->mem_root, size);
    array_add(&M->infos, ((Mem_Track_Info){ result, size }));
    return result;
}

void *mem_track_grow (Mem *mem, void *ptr, size_t old_size, size_t new_size) {
    if (! ptr) return mem_track_alloc(mem, new_size);

    Mem_Track *M = (Mem_Track*)mem;
    void *result = MEM_GROW(M->mem_root, ptr, old_size, new_size);
    mem_track_update_info(M, ptr, result, new_size);
    return result;
}

void *mem_track_grow_z (Mem *mem, void *ptr, size_t old_size, size_t new_size) {
    if (! ptr) return mem_track_alloc_z(mem, new_size);

    Mem_Track *M = (Mem_Track*)mem;
    void *result = MEM_GROW_Z(M->mem_root, ptr, old_size, new_size);
    mem_track_update_info(M, ptr, result, new_size);
    return result;
}

void *mem_track_shrink (Mem *mem, void *ptr, size_t old_size, size_t new_size) {
    Mem_Track *M = (Mem_Track*)mem;
    void *result = MEM_SHRINK(M->mem_root, ptr, old_size, new_size);
    mem_track_update_info(M, ptr, result, new_size);
    return result;
}

void mem_track_destroy (Mem_Track *mem) {
    Mem *mem_root = mem->mem_root;
    array_iter_ptr (info, mem->infos) MEM_FREE(mem_root, info->ptr, info->size);
    array_free(&mem->infos);
    MEM_FREE(mem_root, mem, sizeof(Mem_Track));
}

Mem_Track *mem_track_new (Mem *mem) {
    Mem_Track *result = MEM_ALLOC_Z(mem, sizeof(Mem_Track));

    result->mem_root = mem;

    result->mem = (Mem){
        .free    = mem_track_free,
        .alloc   = mem_track_alloc,
        .alloc_z = mem_track_alloc_z,
        .grow    = mem_track_grow,
        .grow_z  = mem_track_grow_z,
        .shrink  = mem_track_shrink,
    };

    array_init(&result->infos, mem);

    return result;
}
