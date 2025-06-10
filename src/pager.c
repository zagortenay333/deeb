#include <stdio.h>

#include "pager.h"
#include "array.h"

#define PAGE_SIZE             8*KB
#define MIN_PAGE_SIZE         512
#define CACHE_SIZE            1024
#define FILE_HEADER_SIZE      64
#define FILE_HEADER_TITLE     "My custom database."
#define PSIZE                 (pager->header.page_size)
#define NEXT_FREE_PAGE_OFFSET (PSIZE - 4)

typedef struct Page Page;
typedef Array_View(Page) Array_View_Page;

struct Page {
    Page_Ref ref; // Keep this the first field.

    #define F_PAGE_HAS_MUTABLE_REF FLAG(0)

    u32 flags;
    u32 ref_count;
    Page *map_next;
    Page *lru_next;
    Page *lru_prev;
};

struct Pager {
    Mem *mem;
    Files *fs;
    File db_file;
    u32 db_file_page_count;

    struct {
        u16 page_size;

        // This is a linked list of free pages. In each page
        // the Page_Id of the next page in the list is stored
        // at offset NEXT_FREE_PAGE_OFFSET. The value 0 is used
        // to indicate that there is no free page available.
        Page_Id free_page;
    } header;

    struct {
        u32 count;
        u32 capacity;

        Page **map; // Chained hash table mapping Page_Id to Page*. (length: cache.capacity)
        Page *pages; // Array of Page. (length: cache.capacity)
        u8 *raw_pages; // Array of page buffers read from disk. (length: cache.capacity)
        u8 *user_buffers; // Array of user buffers (1 per page). (length: cache.capacity)
        u32 user_buffer_size;

        // This is a sentinel node in the LRU circular doubly linked
        // list. This node's lru_next is the most recently used (MRU)
        // page. It's lru_prev is the least recently used (LRU) page.
        // In case the LRU linked list is empty this node will point
        // to itself: (lru.lru_next == lru) and (lru.lru_prev == lru).
        Page lru;
    } cache;
};

static void header_default_init (Pager *pager) {
    pager->header.page_size = PAGE_SIZE;
}

static void header_write_to_disk (Pager *pager) {
    u8 buf[FILE_HEADER_SIZE];

    memcpy(buf, FILE_HEADER_TITLE, 19);
    write_u16_le(buf + 19, PSIZE);
    write_u32_le(buf + 21, pager->header.free_page);

    String str = { .data = (char*)buf, .count = FILE_HEADER_SIZE };
    fs_write_to_file(pager->fs, pager->db_file, str, 0);
}

static void header_read_from_disk (Pager *pager) {
    u8 buf[FILE_HEADER_SIZE];
    fs_read_from_file(pager->fs, pager->db_file, 0, FILE_HEADER_SIZE, buf);

    pager->header.page_size = read_u16_le(buf + 19);
    pager->header.free_page = read_u32_le(buf + 21);
}

static void init_page_cache (Pager *pager) {
    pager->cache.capacity     = CACHE_SIZE;
    pager->cache.raw_pages    = MEM_ALLOC(pager->mem, pager->cache.capacity * PSIZE);
    pager->cache.pages        = MEM_ALLOC(pager->mem, pager->cache.capacity * sizeof(Page));
    pager->cache.map          = MEM_ALLOC_Z(pager->mem, pager->cache.capacity * sizeof(void*));
    pager->cache.lru.lru_next = &pager->cache.lru;
    pager->cache.lru.lru_prev = &pager->cache.lru;
}

Pager *pager_new (Files *fs, Mem *mem, String db_file_path) {
    Pager *pager   = MEM_ALLOC_Z(mem, sizeof(Pager));
    pager->mem     = mem;
    pager->fs      = fs;
    pager->db_file = fs_open_file(fs, db_file_path);

    u64 file_size = fs_get_file_size(fs, pager->db_file);

    if (file_size < MIN_PAGE_SIZE) { // The db file is uninitialized.
        header_default_init(pager);
        init_page_cache(pager);
        header_write_to_disk(pager);
        pager->db_file_page_count = 1;
    } else {
        header_read_from_disk(pager);
        init_page_cache(pager);
        ASSERT(file_size < UINT32_MAX); // TODO: This should be an exception.
        ASSERT((file_size % PSIZE) == 0); // TODO: This should be an exception.
        pager->db_file_page_count = (u32)file_size / PSIZE;
    }

    return pager;
}

static void lru_add (Pager *pager, Page *page) {
    page->lru_next            = pager->cache.lru.lru_next;
    pager->cache.lru.lru_next = page;
    page->lru_prev            = &pager->cache.lru;
    page->lru_next->lru_prev  = page;
}

static void lru_remove (Page *page) {
    page->lru_prev->lru_next = page->lru_next;
    page->lru_next->lru_prev = page->lru_prev;
}

static void decrement_ref_count (Pager *pager, Page *page) {
    ASSERT(page->ref_count > 0);
    page->ref_count--;
    if (page->ref_count == 0) lru_add(pager, page);
}

static Page **map_get_slot (Pager *pager, Page_Id id) {
    return &pager->cache.map[id % pager->cache.capacity];
}

static Page *map_get (Pager *pager, Page_Id id) {
    Page *slot = *map_get_slot(pager, id);
    while (slot && slot->ref.id != id) slot = slot->map_next;
    return slot;
}

static void map_add (Pager *pager, Page *page, Page_Id id) {
    Page **slot = map_get_slot(pager, id);
    page->map_next = *slot;
    *slot = page;
}

static void map_remove (Pager *pager, Page *page) {
    Page **slot = map_get_slot(pager, page->ref.id);
    while (*slot != page) slot = &(*slot)->map_next;
    *slot = page->map_next;
}

static u64 page_id_to_file_offset (Pager *pager, Page_Id id) {
    return id * PSIZE;
}

static void page_write_to_disk (Pager *pager, Page *page) {
    u64 file_offset = page_id_to_file_offset(pager, page->ref.id);
    String payload = { .data = (char*)page->ref.buf, .count = PSIZE };
    fs_write_to_file(pager->fs, pager->db_file, payload, file_offset);
}

static void page_read_from_disk (Pager *pager, Page *page) {
    u64 file_offset = page_id_to_file_offset(pager, page->ref.id);
    fs_read_from_file(pager->fs, pager->db_file, file_offset, PSIZE, page->ref.buf);
}

static void clear_user_buffer (Pager *pager, u8 *buf) {
    memset(buf, 0, pager->cache.user_buffer_size);
}

static Page *get_empty_cache_slot (Pager *pager, Page_Id id) {
    Page *page = NULL;

    if (pager->cache.count < pager->cache.capacity) {
        u32 idx            = pager->cache.count++;
        page               = &pager->cache.pages[idx];
        page->ref.buf      = &pager->cache.raw_pages[idx * PSIZE];
        page->ref.user_buf = pager->cache.user_buffers ? &pager->cache.user_buffers[idx * pager->cache.user_buffer_size] : NULL;
    } else {
        page = pager->cache.lru.lru_prev;
        ASSERT(page);
        ASSERT(page->ref_count == 0);
        ASSERT(page != &pager->cache.lru); // TODO: LRU is empty. This should either be an exception or return NULL.
        lru_remove(page);
        map_remove(pager, page);
        clear_user_buffer(pager, page->ref.user_buf);
    }

    *page = (Page){
        .ref_count    = 1,
        .ref.id       = id,
        .ref.buf      = page->ref.buf,
        .ref.user_buf = page->ref.user_buf,
    };

    map_add(pager, page, id);

    return page;
}

// It's the job of the user code to ensure that the page this
// function returns is not inside the free list of the pager.
//
// A page that is in the free list can only be used when it
// is returned by a call to pager_alloc_page();
//
// This function will return NULL in case of error.
Page_Ref *pager_get_page (Pager *pager, Page_Id id) {
    ASSERT(id != 0); // TODO: These two asserts should be exceptions.
    ASSERT(id < pager->db_file_page_count);

    Page *page = map_get(pager, id);

    if (page) {
        if (page->flags & F_PAGE_HAS_MUTABLE_REF) return NULL;
        if (page->ref_count == 0) lru_remove(page);
        page->ref_count++;
    } else {
        page = get_empty_cache_slot(pager, id);
        page_read_from_disk(pager, page);
    }

    return (Page_Ref*)page;
}

Page_Ref *pager_get_page_mutable (Pager *pager, Page_Id id) {
    Page_Ref *page = pager_get_page(pager, id);
    if (! page) return NULL;
    if (pager_make_page_mutable(pager, page)) return page;
    pager_unref_page(pager, page);
    return NULL;
}

bool pager_make_page_mutable (Pager *pager, Page_Ref *ref) {
    Page *page = (Page*)ref;
    ASSERT(page->ref_count > 0);
    if (page->ref_count != 1) return false;
    page->flags |= F_PAGE_HAS_MUTABLE_REF;
    return true;
}

bool pager_is_page_mutable (Pager *pager, Page_Ref *ref) {
    Page *page = (Page*)ref;
    return page->flags | F_PAGE_HAS_MUTABLE_REF;
}

Page_Ref *pager_alloc_page (Pager *pager) {
    Page_Ref *page = NULL;

    if (pager->header.free_page) {
        page = (Page_Ref*)get_empty_cache_slot(pager, pager->header.free_page);
        page_read_from_disk(pager, (Page*)page);
        pager->header.free_page = read_u32_le(page->buf + NEXT_FREE_PAGE_OFFSET);
    } else {
        page = (Page_Ref*)get_empty_cache_slot(pager, pager->db_file_page_count++);
        fs_append_to_file(pager->fs, pager->db_file, (String){ .data = (char*)page->buf, .count = PSIZE });
    }

    pager_make_page_mutable(pager, page);

    return page;
}

bool pager_delete_page (Pager *pager, Page_Ref *ref) {
    Page *page = (Page*)ref;

    if (page->ref_count != 1) return false;

    // Add page to free list:
    write_u32_le(ref->buf + NEXT_FREE_PAGE_OFFSET, pager->header.free_page);
    pager->header.free_page = ref->id;
    page_write_to_disk(pager, page);
    header_write_to_disk(pager);

    decrement_ref_count(pager, page);

    return true;
}

void pager_unref_page (Pager *pager, Page_Ref *ref) {
    Page *page = (Page*)ref;

    decrement_ref_count(pager, page);

    if (page->flags & F_PAGE_HAS_MUTABLE_REF) {
        page->flags &= ~F_PAGE_HAS_MUTABLE_REF;
        page_write_to_disk(pager, page);
    }
}

void pager_init_user_buffers (Pager *pager, u32 buf_size) {
    if (buf_size < 8) buf_size = 8;
    pager->cache.user_buffer_size = buf_size;
    pager->cache.user_buffers = MEM_ALLOC_Z(pager->mem, pager->cache.capacity * buf_size);
}

u16 pager_get_page_size (Pager *pager) {
    return PSIZE;
}

u32 pager_get_ref_count (Page_Ref *ref) {
    return ((Page*)ref)->ref_count;
}

bool pager_file_is_empty (Pager *pager) {
    return pager->db_file_page_count == 1;
}
