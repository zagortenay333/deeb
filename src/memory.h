#pragma once

#include <stddef.h>
#include <string.h>

#define KB 1024
#define MB (1024*KB)
#define GB (1024*MB)

// =============================================================================
// Common allocator interface.
//
// - Attempting to allocate 0 bytes is an error.
// - The "_z" suffix means zero clear the memory.
// - Calling grow on a null pointer behaves like calling alloc.
// =============================================================================
typedef struct Mem Mem;

struct Mem {
    void  (*free)    (Mem *, void *, size_t);
    void *(*alloc)   (Mem *, size_t);
    void *(*alloc_z) (Mem *, size_t);
    void *(*grow)    (Mem *, void *, size_t old_size, size_t new_size);
    void *(*grow_z)  (Mem *, void *, size_t old_size, size_t new_size);
    void *(*shrink)  (Mem *, void *, size_t old_size, size_t new_size);

    // The methods in this struct should only be invoked with the following macros.
    // These are more convenient to use: foo->alloc(foo) becomes MEM_ALLOC(foo).
    // They'll try to generate a direct call rather than go through a pointer.
    #define MEM_DISPATCH(op, mem, ...) _Generic((mem), \
        Mem *:       ((Mem*)(mem))->op,                \
        Mem_Clib *:  mem_clib_##op,                    \
        Mem_Arena *: mem_arena_##op,                   \
        Mem_Track *: mem_track_##op                    \
    )((Mem*)mem, __VA_ARGS__)

    #define MEM_FREE(mem, ptr, size)         MEM_DISPATCH(free, mem, ptr, size)
    #define MEM_ALLOC(mem, size)             MEM_DISPATCH(alloc, mem, size)
    #define MEM_ALLOC_Z(mem, size)           MEM_DISPATCH(alloc_z, mem, size)
    #define MEM_GROW(mem, ptr, olds, news)   MEM_DISPATCH(grow, mem, ptr, olds, news)
    #define MEM_GROW_Z(mem, ptr, olds, news) MEM_DISPATCH(grow_z, mem, ptr, olds, news)
    #define MEM_SHRINK(mem, ptr, olds, news) MEM_DISPATCH(shrink, mem, ptr, olds, news)
};

// =============================================================================
// Arena allocator.
//
// This is a linked list of fixed sized blocks. Allocation is done
// by simply pushing new memory on top of a block. When a block
// runs out of memory a new one is appended to the linked list.
//
// All allocations always fit within a single block.
// =============================================================================
typedef struct Mem_Arena Mem_Arena;

Mem_Arena *mem_arena_new     (Mem *, size_t min_block_capacity);
void       mem_arena_clear   (Mem_Arena *);
void       mem_arena_destroy (Mem_Arena *);
void       mem_arena_free    (Mem *, void *, size_t);
void      *mem_arena_alloc   (Mem *, size_t);
void      *mem_arena_alloc_z (Mem *, size_t);
void      *mem_arena_grow    (Mem *, void *, size_t old_size, size_t new_size);
void      *mem_arena_grow_z  (Mem *, void *, size_t old_size, size_t new_size);
void      *mem_arena_shrink  (Mem *, void *, size_t old_size, size_t new_size);

// =============================================================================
// Wrapper around the C lib malloc/calloc/realloc.
// =============================================================================
typedef struct Mem_Clib Mem_Clib;

Mem_Clib *mem_clib_new     (void);
void      mem_clib_destroy (Mem_Clib *);
void      mem_clib_free    (Mem *, void *, size_t);
void     *mem_clib_alloc   (Mem *, size_t);
void     *mem_clib_alloc_z (Mem *, size_t);
void     *mem_clib_grow    (Mem *, void *, size_t old_size, size_t new_size);
void     *mem_clib_grow_z  (Mem *, void *, size_t old_size, size_t new_size);
void     *mem_clib_shrink  (Mem *, void *, size_t old_size, size_t new_size);

// =============================================================================
// This is a wrapper around other allocators that keeps track
// of all allocations made by the underlying allocator and makes
// it possible to free them all.
// =============================================================================
typedef struct Mem_Track Mem_Track;

Mem_Track *mem_track_new     (Mem *);
void       mem_track_destroy (Mem_Track *);
void       mem_track_free    (Mem *, void *, size_t);
void      *mem_track_alloc   (Mem *, size_t);
void      *mem_track_alloc_z (Mem *, size_t);
void      *mem_track_grow    (Mem *, void *, size_t old_size, size_t new_size);
void      *mem_track_grow_z  (Mem *, void *, size_t old_size, size_t new_size);
void      *mem_track_shrink  (Mem *, void *, size_t old_size, size_t new_size);

// =============================================================================
// Misc.
// =============================================================================
Inline void *mem_copy (Mem *mem, void *ptr, size_t size) {
    if (size == 0) return NULL;
    void *result = MEM_ALLOC(mem, size);
    memcpy(result, ptr, size);
    return result;
}
