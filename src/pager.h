#pragma once

#include "files.h"

typedef u32 Page_Id;
typedef struct Pager Pager;

typedef struct {
    Page_Id id;
    u8 *buf;
    void *user_buf;
} Page_Ref;

Pager    *pager_new               (Files *, Mem *, String db_file_path);
Page_Ref *pager_alloc_page        (Pager *);
void      pager_unref_page        (Pager *, Page_Ref *);
bool      pager_delete_page       (Pager *, Page_Ref *);
Page_Ref *pager_get_page          (Pager *, Page_Id);
Page_Ref *pager_get_page_mutable  (Pager *, Page_Id);
bool      pager_is_page_mutable   (Pager *, Page_Ref *);
bool      pager_make_page_mutable (Pager *, Page_Ref *);
void      pager_init_user_buffers (Pager *, u32 buf_size);
u16       pager_get_page_size     (Pager *);
bool      pager_file_is_empty     (Pager *);
u32       pager_get_ref_count     (Page_Ref *);
