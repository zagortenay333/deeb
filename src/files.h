#pragma once

#include "common.h"
#include "string.h"
#include "memory.h"

typedef intptr_t File;
typedef struct Files Files;

Files *fs_new                  (Mem *);
void   fs_destroy              (Files *);
File   fs_open_file            (Files *, String path);
void   fs_create_file          (Files *, String path);
void   fs_close_file           (Files *, File);
String fs_get_file_path        (Files *, File);
u64    fs_get_file_size        (Files *, File);
void   fs_append_to_file       (Files *, File, String);
void   fs_write_to_file        (Files *, File, String, u64 offset);
void   fs_overwrite_file       (Files *, File, String);
void   fs_read_from_file       (Files *, File, u64 offset, u32 amount, u8 *out);
String fs_read_from_file_mem   (Files *, File, u64 offset, u32 amount, Mem *);
String fs_read_entire_file     (Files *, File, Mem *);
String fs_read_entire_file_p   (Files *, String path, Mem *);
