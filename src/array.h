#pragma once

#include "error.h"
#include "common.h"
#include "memory.h"

// =============================================================================
// A polymorphic dynamic array with bounds checking.
//
// Example usage:
//
//    Array(int) arr;
//    array_init(&arr, mem);
//
//    array_add(&arr, 42);
//    array_add(&arr, 420);
//    array_add(&arr, 4200);
//
//    array_iter (it, arr) printf("1 --- %i\n", it);
//
//    array_iter (n, arr) {
//        if (ARRAY_IDX == 1) {
//            printf("2 === %i %i\n", ARRAY_IDX, n);
//        }
//    }
//
//    {
//        Array_View(int) arr_view = {
//            .data  = arr.data,
//            .count = arr.count - 1
//        };
//
//        printf("3 === %i\n", array_get(&arr_view, 0));
//        printf("4 === %i\n", array_get_last(&arr_view));
//
//        array_iter (n, arr_view) printf("5 === %i\n", n);
//    }
//
//    array_free(&arr);
//
// This array reallocs it's buffer when out of space, so avoid
// holding pointers to it's elements for too long. By the same
// logic, if you make an Array_View and then trigger a realloc
// (by calling array_add() for example), the view will dangle.
//
// Members of Array and Array_View can be accessed, but mostly
// stick to read only. In the example above, the members of an
// Array_View are accessed during initialization.
// =============================================================================
#define Array_View_Core struct {\
    char *data;\
    u32 count;\
}

#define Array_Core struct {\
    Array_View_Core;\
    u32 capacity;\
    Mem *mem;\
}

typedef Array_View_Core Array_View;

#define Array(T) union {\
    Array_Core;         /* Anon member for convenient access. */\
    Array_View view;    /* Named member for passing to procedures. */\
    T *array_elem_type; /* Dummy member for holding type information. */\
}

#define Array_View(T) union {\
    Array_View_Core;\
    Array_View view;\
    T *array_elem_type;\
}

// =============================================================================
// Sometimes it will be necessary to use typedef since
// the above type constructors produce tagless unions.
//
// The following will not work:
//
//     Array(int) x;
//     Array(int) y = x;
//
// This is also problematic with procedure signatures.
// The following procedure cannot be called without a
// typecheck fail:
//
//     void foo (Array(bool) a) {}
// =============================================================================
typedef Array(char)  Array_Char;
typedef Array(bool)  Array_Bool;
typedef Array(void*) Array_Ptr;
typedef Array(s8)    Array_s8;
typedef Array(s16)   Array_s16;
typedef Array(s32)   Array_s32;
typedef Array(s64)   Array_s64;
typedef Array(u8)    Array_u8;
typedef Array(u16)   Array_u16;
typedef Array(u32)   Array_u32;
typedef Array(u64)   Array_u64;

typedef Array_View(char)  Array_View_Char;
typedef Array_View(bool)  Array_View_Bool;
typedef Array_View(void*) Array_View_Ptr;
typedef Array_View(s8)    Array_View_s8;
typedef Array_View(s16)   Array_View_s16;
typedef Array_View(s32)   Array_View_s32;
typedef Array_View(s64)   Array_View_s64;
typedef Array_View(u8)    Array_View_u8;
typedef Array_View(u16)   Array_View_u16;
typedef Array_View(u32)   Array_View_u32;
typedef Array_View(u64)   Array_View_u64;

// =============================================================================
// Public methods.
//
// These macros eval their arguments only once.
// Some of these macros also work on array views (marked with a "v").
//
//   array_init             (Array(T)*, Mem *)
//   array_init_cap         (Array(T)*, Mem *, u32 capacity)
//   array_free             (Array(T)*)
// v array_ref              (Array(T)*, u32)
// v array_ref_last         (Array(T)*)
// v array_ref_first        (Array(T)*)
// v array_ref_raw          (Array(T)*, u32)
// v array_get              (Array(T)*, u32)
// v array_get_last         (Array(T)*)
// v array_get_first        (Array(T)*)
// v array_get_raw          (Array(T)*, u32)
// v array_set              (Array(T)*, u32, T)
// v array_set_last         (Array(T)*, T)
// v array_set_first        (Array(T)*, T)
// v array_set_raw          (Array(T)*, u32, T)
// v array_clear            (Array(T)*)
//   array_trim             (Array(T)*)
//   array_reserve          (Array(T)*, u32)
//   array_reserve_only     (Array(T)*, u32)
//   array_add              (Array(T)*, T element)
//   array_add_unique       (Array(T)*, T element)
//   array_add_n_times      (Array(T)*, T element, u32 n)
//   array_remove           (Array(T)*, u32 idx)
//   array_remove_fast      (Array(T)*, u32 idx)
// v array_fake_remove      (Array(T)*, u32 idx)
// v array_swap_elements    (Array(T)*, u32 idx1, u32 idx2)
// v array_has              (Array(T)*, T element, OUT bool found)
// v array_find             (Array(T)*, T element, OUT u32 idx, Out bool found)
//   array_find_remove      (Array(T)*, T element)
//   array_find_remove_fast (Array(T)*, T element)
// v array_find_replace     (Array(T)*, T element, T replacement)
// v array_find_replace_all (Array(T)*, T element, T replacement)
//   array_add_many         (Array(T)*, Array(T) *elements_to_append)
//   array_prepend_many     (Array(T)*, Array(T) *elements_to_prepend)
// v array_elem_size        (Array(T)*)
// v Array_Elem_Type        (Array(T)*)
// =============================================================================
#define Array_Elem_Type(A) Typeof(*(A)->array_elem_type)
#define array_elem_size(A) sizeof(*(A)->array_elem_type)

// The access methods (ref/get/set) are expressions.
// The "_raw" methods don't perform bounds checking.
// The "_ref" methods return a pointer to an element.
#define array_ref(A, IDX)          ((Array_Elem_Type(A)*) ARRAY_REF_FN((Array_View*)(A), array_elem_size(A), IDX))
#define array_ref_last(A)          ((Array_Elem_Type(A)*) ARRAY_REF_LAST_FN((Array_View*)(A), array_elem_size(A)))
#define array_ref_first(A)         array_ref(A, 0)
#define array_ref_raw(A, IDX)      (&ARRAY_DATA_PTR(A)[IDX])

#define array_get(A, IDX)          (* (Array_Elem_Type(A)*) ARRAY_REF_FN((Array_View*)(A), array_elem_size(A), IDX))
#define array_get_last(A)          (* (Array_Elem_Type(A)*) ARRAY_REF_LAST_FN((Array_View*)(A), array_elem_size(A)))
#define array_get_first(A)         array_get(A, 0)
#define array_get_raw(A, IDX)      (ARRAY_DATA_PTR(A)[IDX])

#define array_set(A, IDX, VAL)     (*array_ref(A, IDX) = (VAL))
#define array_set_last(A, VAL)     (*array_ref_last(A) = (VAL))
#define array_set_first(A, VAL)    (*array_ref_first(A) = (VAL))
#define array_set_raw(A, IDX, VAL) (*array_ref_raw(A, IDX) = (VAL))

#define array_init(A, MEM) do{\
    DEF(array, A);\
    DEF(Mem *, mem, MEM);\
    ARRAY_INIT(array, mem);\
}while(0)

#define array_init_cap(A, MEM, CAPACITY) do{\
    DEF(array, A);\
    DEF(Mem *, mem, MEM);\
    DEF(u32, capacity, CAPACITY);\
    ARRAY_INIT(array, mem);\
    ARRAY_GROW_BY(array, capacity);\
}while(0)

#define array_free(A) do{\
    DEF(array, A);\
    MEM_FREE(array->mem, array->data, array->capacity * array_elem_size(array));\
    array->data = NULL;\
    array->count = 0;\
}while(0)

#define array_clear(A) do{\
    DEF(array, A);\
    array->count = 0;\
}while(0)

// Make the capacity match the count.
#define array_trim(A) do{\
    DEF(array, A);\
    if (array->count < array->capacity) {\
        u32 esize = array_elem_size(array);\
        array->data = MEM_SHRINK(array->mem, array->data, array->capacity * esize, array->count * esize);\
        array->capacity = array->count;\
    }\
}while(0)

// Keep doubling the array until there is enough room to fit N elements.
#define array_reserve(A, N) do{\
    DEF(array, A);\
    DEF(u32, n, N);\
    ASSERT(n);\
    if (array->capacity) {\
        u32 new_cap = array->capacity;\
        while ((new_cap - array->count) < n) new_cap *= 2; /* TODO: Overflow. */\
        u32 dt = new_cap - array->capacity;\
        if (dt) ARRAY_GROW_BY(array, dt);\
    } else {\
        ARRAY_GROW_BY(array, n);\
    }\
}while(0)

// Ensure the unused space in the array can contain N elements.
// Unlike array_reserve(), this method grows the array only by
// the minimum amount necessary.
#define array_reserve_only(A, N) do{\
    DEF(array, A);\
    DEF(u32, n, N);\
    ASSERT(n);\
    u32 unused = array->capacity - array->count;\
    if (unused < n) ARRAY_GROW_BY(array, (n - unused));\
}while(0)

// Appends an element to the array.
#define array_add(A, ELEM) do{\
    DEF(array, A);\
    DEF(Array_Elem_Type(array), elem, ELEM);\
    ARRAY_ADD(array, elem);\
}while(0)

// Appends an element to the array N times.
#define array_add_n_times(A, ELEM, N) do{\
    DEF(array, A);\
    DEF(Array_Elem_Type(array), elem, ELEM);\
    DEF(u32, n, N);\
    array_reserve(array, n);\
    while (N--) ARRAY_ADD(array, elem);\
}while(0)

// Appends an element only if the element doesn't
// already appear in the array.
#define array_add_unique(A, ELEM) do{\
    DEF(array, A);\
    DEF(Array_Elem_Type(array), elem, ELEM);\
    bool found = false;\
    array_iter (it, *array) if (it == elem) { found = true; break; }\
    if (! found) ARRAY_ADD(array, elem);\
}while(0)

// Removes an element while preserving order. That
// means that all the elements to the right of IDX
// have to be shifted left.
#define array_remove(A, IDX) do{\
    DEF(array, A);\
    DEF(u32, idx, IDX);\
    ARRAY_REMOVE(array, idx);\
}while(0)

// Removes an element from location IDX by placing
// the last element of the array in location IDX.
#define array_remove_fast(A, IDX) do{\
    DEF(array, A);\
    DEF(u32, idx, IDX);\
    ARRAY_REMOVE_FAST(array, idx);\
}while(0)

// This method swaps the element at IDX with the last
// element of the array and decrements array.count
// without shrinking the array buffer. By resetting
// the array.count all the "removed" elements are back.
#define array_fake_remove(A, IDX) do{\
    DEF(array, A);\
    DEF(u32, idx, IDX);\
    ARRAY_FAKE_REMOVE(array, idx);\
}while(0)

#define array_swap_elements(A, I, J) do{\
    DEF(array, A);\
    DEF(u32, i, I);\
    DEF(u32, j, J);\
    DEF(tmp, array_get(array, i));\
    array_set_raw(array, i, array_get(array, j));\
    array_set_raw(array, j, tmp);\
}while(0)

#define array_has(A, ELEM, OUT_FOUND) do{\
    DEF(array, A);\
    DEF(Array_Elem_Type(array), element, ELEM);\
    OUT_FOUND = false;\
    array_iter (it, *array) if (it == elem) { OUT_FOUND = true; break; }\
}while(0)

#define array_find(A, ELEM, OUT_RESULT_IDX, OUT_FOUND) do{\
    DEF(array, A);\
    DEF(Array_Elem_Type(array), elem, ELEM);\
    OUT_FOUND = false;\
    array_iter (it, *array) if (it == elem) { OUT_FOUND = true; OUT_RESULT_IDX = ARRAY_IDX; break; }\
}while(0)

#define array_find_remove(A, ELEM) do{\
    DEF(array, A);\
    DEF(Array_Elem_Type(array), elem, ELEM);\
    array_iter (it, *array) if (it == elem) { ARRAY_REMOVE(array, ARRAY_IDX); break; }\
}while(0)

#define array_find_remove_fast(A, ELEM) do{\
    DEF(array, A);\
    DEF(Array_Elem_Type(array), elem, ELEM);\
    array_iter (it, *array) if (it == elem) { ARRAY_REMOVE_FAST(array, ARRAY_IDX); break; }\
}while(0)

#define array_find_replace(A, ELEM, REPLACEMENT) do{\
    DEF(array, A);\
    DEF(Array_Elem_Type(array), elem, ELEM);\
    DEF(Array_Elem_Type(array), replacement, REPLACEMENT);\
    array_iter (it, *array) if (it == elem) { array_set_raw(array, ARRAY_IDX, replacement); break; }\
}while(0)

#define array_find_replace_all(A, ELEM, REPLACEMENT) do{\
    DEF(array, A);\
    DEF(Array_Elem_Type(array), elem, ELEM);\
    DEF(Array_Elem_Type(array), replacement, REPLACEMENT);\
    array_iter (it, *array) if (it == elem) array_set_raw(array, ARRAY_IDX, replacement);\
}while(0)

#define array_add_many(A, ELEMS) do{\
    DEF(array, A);\
    DEF(elems, ELEMS);\
    if (elems->count) {\
        u32 esize = array_elem_size(array);\
        u32 new_count = array->count + elems->count; /* TODO: Overflow. */ \
        array_reserve(array, elems->count);\
        void *dst = array_ref_raw(array, array->count);\
        void *src = array_ref_raw(elems, 0);\
        memcpy(dst, src, elems->count * esize);\
        array->count = new_count;\
    }\
}while(0)

#define array_prepend_many(A, ELEMS) do{\
    DEF(array, A);\
    DEF(elems, ELEMS);\
    if (elems->count) {\
        u32 esize = array_elem_size(array);\
        u32 new_count = array->count + elems->count; /* TODO: Overflow. */ \
        array_reserve(array, new_count);\
        void *p1 = array_ref_raw(array, elems->count);\
        void *p2 = array_ref_raw(array, 0);\
        void *p3 = array_ref_raw(elems, 0);\
        memmove(p1, p2, array->count*esize);\
        memcpy(p2, p3, elems->count*esize);\
        array->count = new_count;\
    }\
}while(0)

// =============================================================================
// Iterators.
//
// - Arguments are evaled only once.
// - Both "break" and "continue" work.
// - The "_from" iterators are inclusive.
// - Inside iterators the ARRAY_IDX and ARRAY variables are defined.
// - Elements cannot be removed/added while iterating, but any manipulation
//   is possible within the loop scope as long as you break immediately after.
//
// Example usage:
//
//     array_iter (foo, arr) baz(foo);
//     array_iter (bar, arr) { baz(bar); }
// =============================================================================
#define ARRAY_ITER(IT, A, INIT_IDX, CHECK, ADVANCE)\
    for (bool _(ONCE)=1; _(ONCE);)\
    for (DEF(ARRAY, A); _(ONCE);)\
    for (DEF(u32, ARRAY_IDX, INIT_IDX); _(ONCE);)\
    for (; _(ONCE); _(ONCE)=0)\
    for (Array_Elem_Type(&ARRAY) IT; (CHECK) && (IT = array_get_raw(&ARRAY, ARRAY_IDX), (void)IT, true); ADVANCE)

#define ARRAY_ITER_PTR(IT, A, INIT_IDX, CHECK, ADVANCE)\
    for (bool _(ONCE)=1; _(ONCE);)\
    for (DEF(ARRAY, A); _(ONCE);)\
    for (DEF(u32, ARRAY_IDX, INIT_IDX); _(ONCE);)\
    for (; _(ONCE); _(ONCE)=0)\
    for (Array_Elem_Type(&ARRAY) *IT; (CHECK) && (IT = array_ref_raw(&ARRAY, ARRAY_IDX), (void)IT, true); ADVANCE)

#define array_iter(it, A)                        ARRAY_ITER(it, A, 0, ARRAY_IDX < ARRAY.count, ++ARRAY_IDX)
#define array_iter_from(it, A, from)             ARRAY_ITER(it, A, from, ARRAY_IDX < ARRAY.count, ++ARRAY_IDX)
#define array_iter_reverse(it, A)                ARRAY_ITER(it, A, ARRAY.count ? ARRAY.count : 0, ARRAY_IDX-- > 0, )
#define array_iter_reverse_from(it, A, from)     ARRAY_ITER(it, A, ARRAY.count ? (from)+1 : 0, ARRAY_IDX-- > 0, )

#define array_iter_ptr(it, A)                    ARRAY_ITER_PTR(it, A, 0, ARRAY_IDX < ARRAY.count, ++ARRAY_IDX)
#define array_iter_ptr_from(it, A, from)         ARRAY_ITER_PTR(it, A, from, ARRAY_IDX < ARRAY.count, ++ARRAY_IDX)
#define array_iter_ptr_reverse(it, A)            ARRAY_ITER_PTR(it, A, ARRAY.count ? ARRAY.count : 0, ARRAY_IDX-- > 0, )
#define array_iter_ptr_reverse_from(it, A, from) ARRAY_ITER_PTR(it, A, ARRAY.count ? (from)+1 : 0, ARRAY_IDX-- > 0, )

#define ARRAY_ITER_ON_LAST_ELEMENT (ARRAY_IDX == ARRAY.count - 1)

// =============================================================================
// Private methods.
//
// These macros can eval their arguments multiple times,
// and they don't wrap their arguments in parens.
// =============================================================================
#define ARRAY_DATA_PTR(A)\
    ((Array_Elem_Type(A)*)(A)->data)

// TODO: It should be possible to disable this at compile time.
#define ARRAY_BOUNDS_CHECK(A, IDX) do{\
    if (IDX >= A->count) panic_fmt("Array out of bounds access. (idx = %i; count = %i)", IDX, A->count);\
}while(0)

#define ARRAY_INIT(A, MEM) do{\
    A->mem      = mem;\
    A->data     = NULL;\
    A->count    = 0;\
    A->capacity = 0;\
}while(0)

#define ARRAY_GROW_BY(A, N) do{\
    u32 esize   = array_elem_size(A);\
    u32 new_cap = A->capacity + N; /* TODO: Overflow. */ \
    A->data     = MEM_GROW(A->mem, A->data, A->capacity * esize, new_cap * esize);\
    A->capacity = new_cap;\
}while(0)

#define ARRAY_MAYBE_SHRINK(A) do{\
    if ((A->capacity > 2) && (A->count < ((u64)A->capacity * 25 / 100))) {\
        u32 esize   = array_elem_size(A);\
        u32 new_cap = A->capacity / 2;\
        A->data     = MEM_SHRINK(A->mem, A->data, A->capacity * esize, new_cap * esize);\
        A->capacity = new_cap;\
    }\
}while(0)

#define ARRAY_ADD(A, ELEM) do{\
    if (A->count == A->capacity) {\
        if (A->capacity) ARRAY_GROW_BY(A, A->capacity);\
        else             ARRAY_GROW_BY(A, 2);\
    }\
    array_set_raw(A, A->count++, ELEM);\
}while(0)

#define ARRAY_REMOVE_FAST(A, IDX) do{\
    array_set(A, IDX, array_get_raw(A, A->count-1));\
    A->count--;\
    ARRAY_MAYBE_SHRINK(A);\
}while(0)

#define ARRAY_REMOVE(A, IDX) do{\
    if (IDX == A->count - 1) {\
        A->count--;\
    } else {\
        void *src = array_addr(A, IDX + 1);\
        void *dst = array_ref_raw(A, IDX);\
        memmove(dst, src, (--A->count - IDX) * array_elem_size(A));\
    }\
    ARRAY_MAYBE_SHRINK(A);\
}while(0)

#define ARRAY_FAKE_REMOVE(A, IDX) do{\
    DEF(tmp, array_get(A, IDX));\
    array_set_raw(A, IDX, array_get_raw(A, A->count-1));\
    array_set_raw(A, A->count-1, tmp);\
    A->count--;\
}while(0)

Inline char *ARRAY_REF_FN (Array_View *array, u32 esize, u32 idx) {
    ARRAY_BOUNDS_CHECK(array, idx);
    return &array->data[esize * idx];
}

Inline char *ARRAY_REF_LAST_FN (Array_View *array, u32 esize) {
    ARRAY_BOUNDS_CHECK(array, array->count - 1);
    return &array->data[esize * (array->count - 1)];
}
