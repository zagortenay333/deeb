#pragma once

#include "files.h"
#include "typer.h"
#include "string.h"
#include "common.h"
#include "memory.h"

typedef struct BEngine BEngine;
typedef struct BTree   BTree;
typedef struct BType   BType;
typedef struct BCursor BCursor;

typedef struct { void *ptr; } Key;
typedef struct { void *ptr; } Val;
typedef struct { void *ptr; } UKey;

struct BType {
    int  (*key_cmp)       (UKey, Key);
    void (*key_print)     (DString *, Key);
    u32  (*sizeof_key)    (Key);
    u32  (*sizeof_ukey)   (UKey);
    void (*serialize_key) (Key, UKey);
    u32  (*sizeof_val)    (Val);
    int  (*key_cmp2)      (Key, Key);
};

BEngine *bengine_new         (Files *, Mem *, String db_file_path);
void     bengine_close       (BEngine *);
bool     bengine_db_is_empty (BEngine *);
s64      bengine_get_tag     (Type_Table *);

BTree   *btree_new           (BEngine *, Type_Table *);
BTree   *btree_load          (BEngine *, Type_Table *, s64);
void     btree_delete        (BTree *);
void     btree_print         (BTree *);

BCursor *bcursor_new         (BTree *);
void     bcursor_close       (BCursor *);
void     bcursor_reset       (BCursor *);
Val      bcursor_read        (BCursor *);
void     bcursor_insert      (BCursor *, UKey, Val);
void     bcursor_update      (BCursor *, Val);
void     bcursor_remove      (BCursor *);
bool     bcursor_goto_ukey   (BCursor *, UKey);
bool     bcursor_goto_key    (BCursor *, Key);
bool     bcursor_goto_next   (BCursor *);
bool     bcursor_goto_prev   (BCursor *);
bool     bcursor_goto_first  (BCursor *);
