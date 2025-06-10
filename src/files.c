#include <stdio.h>

#include "files.h"
#include "array.h"
#include "report.h"
#include "string.h"

typedef struct {
    String path;
    FILE *handle;
} File_Info;

struct Files {
    Mem *mem;
    Array(File_Info) files;
};

static Noreturn error (Files *fs) {
    panic_fmt("File system error.");
}

static String save_path (Files *fs, String str) {
    DString ds = ds_new_cap(fs->mem, str.count + 1);
    ds_add_fmt(&ds, "%.*s", str.count, str.data);
    ds_add_byte(&ds, 0);
    return ds_to_str(&ds);
}

static void seek (Files *fs, File file, u64 offset) {
    // TODO: We cannot offset with a u64...
    if (fseek((FILE*)file, (long)offset, SEEK_SET)) error(fs);
}

static void seek_end (Files *fs, File file) {
    if (fseek((FILE*)file, 0, SEEK_END)) error(fs);
}

u64 fs_get_file_size (Files *fs, File file) {
    seek_end(fs, file);
    long result = ftell((FILE*)file);
    if (result < 0) error(fs);
    return (u64)result;
}

Files *fs_new (Mem *mem) {
    Files *fs = MEM_ALLOC_Z(mem, sizeof(Files));
    fs->mem = mem;
    array_init(&fs->files, mem);
    return fs;
}

void fs_destroy (Files *fs) {
    array_iter (it, fs->files) {
        fclose(it.handle);
        MEM_FREE(fs->mem, it.path.data, it.path.count + 1);
    }

    array_free(&fs->files);
    MEM_FREE(fs->mem, fs, sizeof(Files));
}

File fs_open_file (Files *fs, String path) {
    path = save_path(fs, path);

    FILE *file = fopen(path.data, "r+b");

    if (! file) {
        // If the file doesn't exist try to make one.
        file = fopen(path.data, "w+b");
        if (! file) error(fs);

        // Switch again to r+b mode...
        fclose(file);
        file = fopen(path.data, "r+b");
        if (! file) error(fs);
    }

    setbuf(file, NULL); // Disable buffering.

    array_add(&fs->files, ((File_Info){ .path = path, .handle = file }));

    return (File)file;
}

void fs_create_file (Files *fs, String path) {
    fs_close_file(fs, fs_open_file(fs, path));
}

String fs_get_file_path (Files *fs, File file) {
    array_iter (it, fs->files) if (it.handle == (FILE*)file) return it.path;
    unreachable;
}

void fs_close_file (Files *fs, File file) {
    fclose((FILE*)file);

    array_iter (it, fs->files) {
        if (it.handle == (FILE*)file) {
            MEM_FREE(fs->mem, it.path.data, it.path.count + 1);
            array_remove_fast(&fs->files, ARRAY_IDX);
            break;
        }
    }
}

void fs_append_to_file (Files *fs, File file, String payload) {
    seek_end(fs, file);
    size_t n_written = fwrite(payload.data, 1, payload.count, (FILE*)file);
    if (n_written != payload.count) error(fs);
}

void fs_write_to_file (Files *fs, File file, String payload, u64 offset) {
    seek(fs, file, offset);
    size_t n_written = fwrite(payload.data, 1, payload.count, (FILE*)file);
    if (n_written != payload.count) error(fs);
}

void fs_overwrite_file (Files *fs, File file, String payload) {
    { // Truncate the file to zero length:
        String path = fs_get_file_path(fs, file);
        FILE *F = fopen(path.data, "w+");
        if (! F) error(fs);
        fclose(F);
    }

    fs_append_to_file(fs, file, payload);
}

void fs_read_from_file (Files *fs, File file, u64 offset, u32 amount, u8 *out) {
    seek(fs, file, offset);
    size_t n_read = fread(out, 1, amount, (FILE*)file);
    if (n_read != amount) error(fs);
}

String fs_read_from_file_mem (Files *fs, File file, u64 offset, u32 amount, Mem *mem) {
    char *buf = MEM_ALLOC(mem, amount);
    seek(fs, file, offset);
    size_t n_read = fread(buf, 1, amount, (FILE*)file);
    if (n_read != amount) error(fs);
    return (String){ buf, amount };
}

String fs_read_entire_file (Files *fs, File file, Mem *mem) {
    u64 size = fs_get_file_size(fs, file);
    if (size > UINT32_MAX) error(fs); // TODO: Not clear why we do this.
    return fs_read_from_file_mem(fs, file, 0, (u32)size, mem);
}

String fs_read_entire_file_p (Files *fs, String path, Mem *mem) {
    File file = fs_open_file(fs, path);
    String result = fs_read_entire_file(fs, file, mem);
    fs_close_file(fs, file);
    return result;
}
